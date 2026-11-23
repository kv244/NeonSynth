// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>

/**
    @file Wavetable.h

    CUSTOM's wave (v1.12.0): a Fourier series of up to 32 harmonics, played from tables.

        wave (t) = gain * sum over k = 1..n of  level[k] * sin (2 pi k t + phase[k])

    - HarmonicSpectrum is the 32 levels and phases the session saves and the HARMONICS view edits.
    - WavetableSet holds 32 tables built from one spectrum: table n sums harmonics 1..n. A voice
      plays table min (HARMONICS, the highest harmonic below Nyquist at its pitch), so one set
      serves both the HARMONICS count and band-limiting, at any sample rate, with no rebuild.
      NAIVE plays table HARMONICS whatever the pitch, so harmonics above Nyquist alias.
    - WavetableBank hands finished sets from the builder (message thread, or wherever the host
      loads a session) to the audio thread: a lock-free triple buffer, so the audio thread never
      waits, allocates or sees a set being written.
*/

/** The 32 levels and phases of CUSTOM's wave. */
struct HarmonicSpectrum
{
    static constexpr int numHarmonics = 32;

    /**
        A level of 1 is the fundamental of a full-height square wave: the series is scaled by
        4 / pi, so the SQUARE preset (level 1/k on odd k) peaks at 1, like the oscillator's square.
    */
    static constexpr float seriesGain = 4.0f / juce::MathConstants<float>::pi;

    std::array<float, numHarmonics> levels {};   ///< 0..1, harmonic k + 1 at index k
    std::array<float, numHarmonics> phases {};   ///< radians, 0..2 pi

    bool operator== (const HarmonicSpectrum& other) const noexcept
    {
        return levels == other.levels && phases == other.phases;
    }
    bool operator!= (const HarmonicSpectrum& other) const noexcept   { return ! (*this == other); }

    /** The textbook series. Each peaks at about 1 with all 32 harmonics (Gibbs adds 9% at a jump). */
    static HarmonicSpectrum sine() noexcept
    {
        HarmonicSpectrum s;
        s.levels[0] = 1.0f / seriesGain;
        return s;
    }

    /** Rising saw, 2t - 1 over a cycle: -(2/pi) sum sin (2 pi k t) / k, so level 1/(2k), phase pi. */
    static HarmonicSpectrum saw() noexcept
    {
        HarmonicSpectrum s;
        for (int k = 1; k <= numHarmonics; ++k)
        {
            s.levels[(size_t) (k - 1)] = 0.5f / (float) k;
            s.phases[(size_t) (k - 1)] = juce::MathConstants<float>::pi;
        }
        return s;
    }

    /** +1 for the first half cycle, -1 for the second: (4/pi) sum over odd k of sin (2 pi k t) / k. */
    static HarmonicSpectrum square() noexcept
    {
        HarmonicSpectrum s;
        for (int k = 1; k <= numHarmonics; k += 2)
            s.levels[(size_t) (k - 1)] = 1.0f / (float) k;
        return s;
    }

    /** The oscillator's triangle (-1 at phase 0, +1 at phase 0.5): odd k, level 2 / (pi k^2), phase -pi/2. */
    static HarmonicSpectrum triangle() noexcept
    {
        HarmonicSpectrum s;
        for (int k = 1; k <= numHarmonics; k += 2)
        {
            s.levels[(size_t) (k - 1)] = 2.0f / (juce::MathConstants<float>::pi * (float) (k * k));
            s.phases[(size_t) (k - 1)] = 1.5f * juce::MathConstants<float>::pi;
        }
        return s;
    }

    /** What a session without a <Harmonics> element starts with. */
    static HarmonicSpectrum defaultSpectrum() noexcept   { return saw(); }

    /** Levels clamped to 0..1 and phases wrapped to 0..2 pi; anything not finite becomes 0. */
    void sanitise() noexcept
    {
        constexpr float twoPi = juce::MathConstants<float>::twoPi;
        for (auto& l : levels)
            l = std::isfinite (l) ? juce::jlimit (0.0f, 1.0f, l) : 0.0f;
        for (auto& p : phases)
        {
            p = std::isfinite (p) ? std::fmod (p, twoPi) : 0.0f;
            if (p < 0.0f)
                p += twoPi;
        }
    }
};

/**
    32 tables built from one spectrum: table n (1..32) is harmonics 1..n. Built off the audio
    thread; read-only once handed over.
*/
class WavetableSet
{
public:
    static constexpr int tableSize = 2048;                          ///< points per cycle
    static constexpr int guardPoints = 3;                           ///< copies of points 0..2 after the end, for cubic reads
    static constexpr int numTables = HarmonicSpectrum::numHarmonics;
    static constexpr double maxPeak = 2.0;                          ///< the loudest a custom wave may peak (a full-height wave peaks at 1)

    /**
        Builds the 32 tables. One scale for all of them: gain 4/pi, reduced only if the full
        32-harmonic wave would peak above maxPeak (twice a full-height wave, so every textbook
        shape, Gibbs overshoot included, plays at its true level). Adding harmonics (BUILD)
        therefore never changes the level of those already there, and changing phases changes
        the sound only in that extreme case.
        Allocates on first use; call off the audio thread.
    */
    void build (const HarmonicSpectrum& spectrum)
    {
        if (data == nullptr)
            data = std::make_unique<float[]> ((size_t) numTables * stride);

        std::array<double, tableSize> sum {};
        double peak = 0.0;
        for (int n = 1; n <= numTables; ++n)
        {
            const double level = spectrum.levels[(size_t) (n - 1)];
            const double phase = spectrum.phases[(size_t) (n - 1)];
            if (level != 0.0)
                for (int i = 0; i < tableSize; ++i)
                    sum[(size_t) i] += level * std::sin (juce::MathConstants<double>::twoPi * n * i / tableSize + phase);
            float* t = table (n);
            for (int i = 0; i < tableSize; ++i)
                t[i] = (float) sum[(size_t) i];
        }
        for (auto v : sum)
            peak = std::max (peak, std::abs (v));

        scale = (float) std::min ((double) HarmonicSpectrum::seriesGain, peak > 0.0 ? maxPeak / peak : 1.0);
        for (int n = 1; n <= numTables; ++n)
        {
            float* t = table (n);
            for (int i = 0; i < tableSize; ++i)
                t[i] *= scale;
            for (int g = 0; g < guardPoints; ++g)
                t[tableSize + g] = t[g];
        }
        source = spectrum;
    }

    bool isBuilt() const noexcept   { return data != nullptr; }

    /** Table n, 1..32 (clamped), tableSize + guardPoints values. */
    const float* getTable (int n) const noexcept   { return data.get() + (size_t) (juce::jlimit (1, numTables, n) - 1) * stride; }

    /**
        The value of table n at phase t (0..1), by cubic (Catmull-Rom) interpolation between
        table points. Audio thread: no allocation.
    */
    float read (int n, float t) const noexcept
    {
        const float* tab = getTable (n);
        const float x = t * (float) tableSize;
        int i = (int) x;
        const float f = x - (float) i;
        i = (i + tableSize - 1) % tableSize;   // the point before; guard points cover i + 3
        const float y0 = tab[i], y1 = tab[i + 1], y2 = tab[i + 2], y3 = tab[i + 3];
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }

    /** The highest harmonic below Nyquist for a phase step dt (cycles per sample), 1..32. */
    static int harmonicsBelowNyquist (float dt) noexcept
    {
        if (dt <= 0.0f)
            return numTables;
        return juce::jlimit (1, numTables, (int) std::ceil (0.5f / dt) - 1);
    }

    /** The scale build() applied (4/pi unless the wave would have peaked above maxPeak). */
    float getScale() const noexcept   { return scale; }

    /** The spectrum these tables were built from. */
    const HarmonicSpectrum& getSpectrum() const noexcept   { return source; }

private:
    static constexpr size_t stride = (size_t) (tableSize + guardPoints);

    float* table (int n) noexcept   { return data.get() + (size_t) (n - 1) * stride; }

    std::unique_ptr<float[]> data;
    float scale = HarmonicSpectrum::seriesGain;
    HarmonicSpectrum source;
};

/**
    Hands WavetableSets from one builder to the audio thread: a lock-free triple buffer.

    The builder owns a back set, the audio thread a front set, and the third waits in the
    middle. publish() builds into the back set and swaps it into the middle; acquire() swaps
    the middle into the front when a newer set is there. Swaps are single atomic exchanges, so
    the audio thread never waits, and no set is written while the audio thread can read it.
    Every set is built once in the constructor, so nothing allocates after that.

    publish() may run on any thread but one at a time (the caller locks); acquire() runs on the
    audio thread only.
*/
class WavetableBank
{
public:
    explicit WavetableBank (const HarmonicSpectrum& initial = HarmonicSpectrum::defaultSpectrum())
    {
        for (auto& s : sets)
            s.build (initial);
    }

    /** Builds `spectrum` into the back set and makes it the newest. Not the audio thread. */
    void publish (const HarmonicSpectrum& spectrum)
    {
        sets[(size_t) back].build (spectrum);
        back = middle.exchange (back | freshBit, std::memory_order_acq_rel) & indexMask;
    }

    /** The newest finished set. Audio thread; once per block. */
    const WavetableSet& acquire() noexcept
    {
        if ((middle.load (std::memory_order_acquire) & freshBit) != 0)
            front = middle.exchange (front, std::memory_order_acq_rel) & indexMask;
        return sets[(size_t) front];
    }

private:
    static constexpr int freshBit = 4, indexMask = 3;

    std::array<WavetableSet, 3> sets;
    int back = 0, front = 2;          // owned by the builder and the audio thread
    std::atomic<int> middle { 1 };    // the set between them; freshBit = not yet taken by acquire()
};
