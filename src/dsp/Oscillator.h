// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include "dsp/Blep4.h"
#include "dsp/Chop.h"   // CHOP lived here until v1.11.1; included so every existing include still finds it
#include "dsp/Wavetable.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <utility>

/**
    @file Oscillator.h

    Band-limited oscillators for one synth voice: PolyBlepOscillator (sine,
    square, triangle and sawtooth) and RandomStepOscillator (the Random
    source), sharing the residuals in Blep4.

    Jumps (the square and saw edges, Random's steps) get a 4-point BLEP
    residual, and the triangle's corners a 4-point BLAMP residual. Both come
    from the cubic B-spline: it spreads each jump or corner over two samples
    either side, which removes more of the aliasing near Nyquist than the
    2-point PolyBLEP used before v1.6.0.

    A waveform change waits for the next cycle boundary, so switching shapes
    while a note sounds never clicks. The phase is kept across notes, so a
    retriggered or stolen voice continues smoothly.

    v1.12.0 "Fourier":
    - NAIVE plays the textbook waves (2t - 1, +-1, the triangle) with no
      residuals and no treble compensation, Random's steps without their tails
      and CHOP with hard edges, so the aliasing can be heard. Same level as the
      band-limited waves; the sine is unchanged.
    - CUSTOM (PolyBlepOscillator only) plays CUSTOM's wavetables instead of the
      waveform: table min (HARMONICS, the highest harmonic below Nyquist at the
      current pitch), or table HARMONICS with NAIVE on.
    Both switch at the next cycle boundary, like a waveform change. With both
    off, every sample is computed exactly as in v1.11.1.

    Audio thread only; no allocation.
*/

/**
    The oscillator for the four waveforms.

    The name is historical: up to v1.5.0 it used 2-point PolyBLEP. Since v1.6.0 it uses the
    4-point BLEP and BLAMP residuals in Blep4 (dsp/Blep4.h), so "BandLimitedOscillator" would describe
    it better. Not renamed yet, because the name is used across the voice, the views and the
    tests (code review, 12 Nov 2026); do it when those files are next open for another reason.
*/
class PolyBlepOscillator
{
public:
    enum Waveform { sine = 0, square = 1, triangle = 2, sawtooth = 3 };

    /**
        A change of the table played (HARMONICS moved, or the pitch crossed a harmonic's
        Nyquist limit) crossfades between the two tables over this long (v1.12.0). 5 ms is
        240 samples at 48 kHz: a harmonic fading in or out over that long moves the wave by
        under 1/240 of its height per sample, far below a click, and short enough that a
        glide or a bend leaves a harmonic above Nyquist for 10 ms at most (a fade under
        way finishes before the next starts), where it folds back close to Nyquist.
    */
    static constexpr double tableFadeSeconds = 0.005;

    /**
        A new spectrum (a new set of tables) mid-note (v1.12.0): the step between the old
        wave and the new one at the same phase is added as an offset that shrinks to 0 over
        this long, so the new wave starts where the old one was. The old set is never read
        again (the bank may be rebuilding it); 5 ms, as tableFadeSeconds.
    */
    static constexpr double spectrumFadeSeconds = 0.005;

    /** Takes effect at the next cycle boundary. */
    void setWaveform (int newWaveform) noexcept   { pendingWaveform = newWaveform; }

    /** NAIVE (v1.12.0): no band-limiting. Takes effect at the next cycle boundary. */
    void setNaive (bool shouldBeNaive) noexcept   { pendingNaive = shouldBeNaive; }

    /**
        CUSTOM (v1.12.0), once per block: on or off (from the next cycle boundary), how many
        harmonics play (1..32) and this block's tables. A null `tables` plays the waveform,
        as with CUSTOM off. A set other than last block's is a new spectrum (see
        spectrumFadeSeconds); the old one is not read again.
    */
    void setCustom (bool on, int harmonics, const WavetableSet* tables) noexcept
    {
        pendingCustom = on;
        const int count = juce::jlimit (1, WavetableSet::numTables, harmonics);
        if (count != harmonicsSetting)
        {
            harmonicsSetting = count;
            customSteady = false;
        }
        if (tables != wavetables)
        {
            if (wavetables == nullptr)
                restartCustom();   // nothing played from a set before: nothing to fade from
            else
                tablesChanged = true;
            wavetables = tables;
            customSteady = false;
        }
    }

    void prepare (double sampleRate) noexcept
    {
        chop.prepare (sampleRate);
        tableFadeStep = 1.0f / (float) juce::jmax (1.0, tableFadeSeconds * sampleRate);
        spectrumFadeSamples = juce::jmax (1, (int) (spectrumFadeSeconds * sampleRate));
    }
    void setChop (const ChopSettings& settings) noexcept     { chop.set (settings); }

    /** Sets the pitch immediately (no glide). Clamped below half the sample rate. */
    void setFrequency (float hz, double sampleRate) noexcept
    {
        baseInc  = (float) (hz / sampleRate);
        phaseInc = juce::jmin (baseInc, Blep4::maxPhaseStep);
    }

    int getWaveform() const noexcept   { return waveform; }

    /** One sample at the note's own pitch. */
    float nextSample() noexcept   { return nextSampleWithStep (phaseInc); }

    /**
        One sample with the pitch multiplied by `pitchRatio` (pitch bend, vibrato,
        LFO; v1.9.0). The step is clamped like setFrequency()'s, so a bend far up
        stays band-limited. Nothing is kept: the next nextSample() is back at the
        note's own pitch.
    */
    float nextSample (float pitchRatio) noexcept
    {
        return nextSampleWithStep (juce::jmin (baseInc * pitchRatio, Blep4::maxPhaseStep));
    }

private:
    float nextSampleWithStep (float dt) noexcept
    {
        const float t  = phase;
        if (custom && wavetables != nullptr)   // v1.12.0; with both off, what follows is v1.11.1's
            return nextCustomSample (t, dt);
        if (naive)
            return nextNaiveSample (t, dt);

        float value;
        switch (waveform)
        {
            case square:     // +2 at phase 0, -2 at phase 0.5
            {
                const float half = t < 0.5f ? t + 0.5f : t - 0.5f;   // the phase seen from the second edge
                value = (t < 0.5f ? 1.0f : -1.0f)
                      + 2.0f * (Blep4::step (t, dt) - Blep4::step (half, dt));
                break;
            }
            case triangle:   // no jumps, but corners: the slope changes by +8 / -8 per cycle at phase 0 / 0.5
            {
                const float half = t < 0.5f ? t + 0.5f : t - 0.5f;
                value = (t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t)
                      + 8.0f * dt * (Blep4::corner (t, dt) - Blep4::corner (half, dt));
                break;
            }
            case sawtooth:   // -2 at phase 0
                value = 2.0f * t - 1.0f - 2.0f * Blep4::step (t, dt);
                break;
            default:         // sine
                value = std::sin (juce::MathConstants<float>::twoPi * t);
                break;
        }

        value = chop.apply (value, t, dt, [this] (int segment) noexcept { return valueAtSegment (segment); });
        const float out = waveform == sine ? treble.bypass (value) : treble.process (value);

        advancePhase (dt);
        return out;
    }

    /** One step on; at the cycle boundary the pending shape, NAIVE and CUSTOM take effect. */
    void advancePhase (float dt) noexcept
    {
        phase += dt;
        if (phase >= 1.0f)
        {
            phase -= 1.0f;
            waveform = pendingWaveform;   // switch shapes only at the cycle boundary
            naive = pendingNaive;         // (v1.12.0) NAIVE and CUSTOM too
            if (pendingCustom && ! custom)
                restartCustom();          // CUSTOM starts on its table, with no fade from an old one
            custom = pendingCustom;
            customSteady = false;         // (NAIVE may have changed the table wanted)
            chop.setHardEdges (naive);
            chop.startCycle();
        }
    }

    /** NAIVE (v1.12.0): the textbook wave, hard CHOP edges, no treble compensation. */
    float nextNaiveSample (float t, float dt) noexcept
    {
        const float value = chop.apply (textbookValue (t), t, dt, [this] (int segment) noexcept { return valueAtSegment (segment); });
        const float out = treble.bypass (value);   // keeps its history, for a switch back
        advancePhase (dt);
        return out;
    }

    /** The waves with no band-limiting: 2t - 1, +-1, the triangle (-1 at phase 0, +1 at 0.5), the sine. */
    float textbookValue (float t) const noexcept
    {
        switch (waveform)
        {
            case square:   return t < 0.5f ? 1.0f : -1.0f;
            case triangle: return t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t;
            case sawtooth: return 2.0f * t - 1.0f;
            default:       return std::sin (juce::MathConstants<float>::twoPi * t);
        }
    }

    /**
        CUSTOM (v1.12.0): the table for this step, crossfading when it changes, plus the
        offset left by a new spectrum. CHOP and the rest follow as for any wave; no treble
        compensation (the tables are exact).
    */
    float nextCustomSample (float t, float dt) noexcept
    {
        // Almost always the same table as the sample before, with nothing fading: one read.
        float value = customSteady && dt == limitStep ? WavetableSet::readTable (playingTable, t)
                                                      : nextMovingCustomValue (t, dt);
        lastCustomValue = value;
        lastPhase = t;

        value = chop.apply (value, t, dt, [this] (int segment) noexcept
        {
            return tableValue ((float) segment / (float) chop.getSegments());   // the table is continuous: its own value
        });
        const float out = treble.bypass (value);   // keeps its history, for a switch back to a waveform
        advancePhase (dt);
        return out;
    }

    /** nextCustomSample()'s value when anything moves: the pitch, HARMONICS, a fade, a new set. */
    float nextMovingCustomValue (float t, float dt) noexcept
    {
        updateTable (dt);

        if (tablesChanged)
        {
            // A new spectrum: start from where the old wave was. Its value at the last
            // sample's phase was lastCustomValue; the new tables give tableValue (lastPhase)
            // there, so the difference is the step a plain switch would make.
            tablesChanged = false;
            if (hasLastCustomValue)
            {
                spectrumOffset = lastCustomValue - tableValue (lastPhase);
                spectrumOffsetStart = spectrumOffset;
                spectrumSamplesLeft = spectrumFadeSamples;
            }
        }

        float value = tableValue (t);
        if (spectrumSamplesLeft > 0)
        {
            value += spectrumOffset;
            --spectrumSamplesLeft;
            spectrumOffset = spectrumOffsetStart * (float) spectrumSamplesLeft / (float) spectrumFadeSamples;
        }
        if (tableFade < 1.0f)
            tableFade = juce::jmin (1.0f, tableFade + tableFadeStep);

        hasLastCustomValue = true;
        playingTable = wavetables->getTable (tableTo);
        customSteady = tableFade >= 1.0f && spectrumSamplesLeft == 0;
        return value;
    }

    /** CUSTOM starts afresh: on the table it wants, with no fade and no offset. */
    void restartCustom() noexcept
    {
        customSteady = false;
        tableTo = 0;
        tablesChanged = false;
        hasLastCustomValue = false;
        spectrumSamplesLeft = 0;
    }

    /**
        The table to play at phase step dt: HARMONICS, limited to the harmonics below Nyquist
        unless NAIVE. A change starts a crossfade from the table playing; a change back to
        the table it fades from reverses it; any other change waits for the fade to end.
    */
    void updateTable (float dt) noexcept
    {
        if (dt != limitStep)   // the limit's division and ceil only when the pitch moves
        {
            limitStep = dt;
            nyquistLimit = WavetableSet::harmonicsBelowNyquist (dt);
        }
        const int wanted = naive ? harmonicsSetting : juce::jmin (harmonicsSetting, nyquistLimit);

        if (tableTo == 0)   // CUSTOM just started: no fade
        {
            tableTo = tableFrom = wanted;
            tableFade = 1.0f;
        }
        else if (tableFade >= 1.0f)
        {
            if (wanted != tableTo)
            {
                tableFrom = tableTo;
                tableTo = wanted;
                tableFade = 0.0f;
            }
        }
        else if (wanted == tableFrom)
        {
            std::swap (tableFrom, tableTo);
            tableFade = 1.0f - tableFade;
        }
    }

    /** The current tables at phase p: table tableTo, or the crossfade from tableFrom to it. */
    float tableValue (float p) const noexcept
    {
        const float to = wavetables->read (tableTo, p);
        if (tableFade >= 1.0f)
            return to;
        const float from = wavetables->read (tableFrom, p);
        return from + tableFade * (to - from);
    }

    /** The ideal wave at the start of CHOP segment k (the midpoint where the wave itself jumps). */
    float valueAtSegment (int segment) const noexcept
    {
        const int segments = chop.getSegments();
        const float p = (float) segment / (float) segments;
        switch (waveform)
        {
            case square:   return (segment % segments == 0 || 2 * segment == segments) ? 0.0f : (p < 0.5f ? 1.0f : -1.0f);
            case triangle: return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
            case sawtooth: return segment % segments == 0 ? 0.0f : 2.0f * p - 1.0f;
            default:       return std::sin (juce::MathConstants<float>::twoPi * p);
        }
    }

    Blep4::TrebleCompensation treble;
    Chop chop;
    float phase = 0.0f, phaseInc = 0.0f;
    float baseInc = 0.0f;   // hz / sampleRate before the clamp: what a pitch ratio multiplies
    int   waveform = sine, pendingWaveform = sine;

    // NAIVE and CUSTOM (v1.12.0), switched at the cycle boundary like the waveform.
    bool  naive = false, pendingNaive = false;
    bool  custom = false, pendingCustom = false;

    // CUSTOM's tables (see nextCustomSample()).
    const WavetableSet* wavetables = nullptr;           // this block's set; never read once replaced
    int   harmonicsSetting = WavetableSet::numTables;   // HARMONICS, 1..32
    const float* playingTable = nullptr;                // table tableTo, while customSteady
    bool  customSteady = false;                         // one table, nothing fading, no offset: see nextCustomSample()
    int   tableFrom = 0, tableTo = 0;                   // the tables playing (0 = CUSTOM not started)
    float tableFade = 1.0f, tableFadeStep = 1.0f;       // 0..1 from tableFrom to tableTo; per sample
    float limitStep = -1.0f;                            // the phase step nyquistLimit was worked out for
    int   nyquistLimit = WavetableSet::numTables;
    bool  tablesChanged = false;                        // a new set arrived: work out the offset
    bool  hasLastCustomValue = false;
    float lastCustomValue = 0.0f, lastPhase = 0.0f;     // the last custom sample (before CHOP) and its phase
    float spectrumOffset = 0.0f, spectrumOffsetStart = 0.0f;
    int   spectrumSamplesLeft = 0, spectrumFadeSamples = 1;
};

/**
    The Random source: a pitched wave of 16 random steps per cycle that keeps
    evolving.

    Each cycle is 16 equal steps, each holding a level drawn uniformly from
    -1..1. About once per cycle (every 17 steps) one level is replaced by a
    new random value, so the wave mostly repeats from cycle to cycle (that
    repetition is what gives it a pitch) while its timbre drifts: the whole
    shape is renewed every 17 cycles. Re-rolling every level every cycle
    would repeat nothing, which sounds like sample-and-hold noise with no
    pitch at the note's frequency.

    The replaced level is always the one 8 steps away from the playhead, so
    neither of the two step edges next to the playhead ever changes while
    they are being band-limited.

    Each step edge gets the 4-point residual (Blep4) scaled by its height, the
    same correction the square and saw edges get. It is exact while a step
    lasts at least four samples (up to 1/64 of the sample rate: 750 Hz at
    48 kHz); shorter steps get overlapping corrections, then a capped one, so
    the output stays within -1..1 but aliases more.

    NAIVE (v1.12.0) drops the residuals: the steps and CHOP's edges are hard,
    switched at the next cycle boundary.

    Seeded: the same seed and the same notes render the same audio. The phase
    is kept across notes like the oscillator's, so a retrigger never clicks.

    Audio thread only; no allocation.
*/
class RandomStepOscillator
{
public:
    static constexpr int numSteps = 16;

    /** The seed used for a seed of 0 (xorshift would stay at 0 for ever): 2^32 / golden ratio. */
    static constexpr juce::uint32 goldenRatioSeed = 0x9e3779b9u;

    /** Generator steps thrown away after seeding, so nearby seeds give unrelated streams. */
    static constexpr int warmUpSteps = 8;

    /** Restarts the random stream and the phase (prepare, reset). */
    void reseed (juce::uint32 seed) noexcept
    {
        state = seed != 0 ? seed : goldenRatioSeed;
        for (int i = 0; i < warmUpSteps; ++i)   // warm up: nearby seeds give unrelated streams
            nextRandom();
        for (auto& l : levels)
            l = nextRandom();
        phase = 0.0f;
        lastStep = 0;
        stepsSinceReroll = 0;
    }

    void prepare (double sampleRate) noexcept                { chop.prepare (sampleRate); }
    void setChop (const ChopSettings& settings) noexcept     { chop.set (settings); }

    /** NAIVE (v1.12.0): hard steps. Takes effect at the next cycle boundary. */
    void setNaive (bool shouldBeNaive) noexcept   { pendingNaive = shouldBeNaive; }

    /** Sets the pitch immediately (no glide). */
    void setFrequency (float hz, double sampleRate) noexcept
    {
        baseInc  = (float) (hz / sampleRate);
        phaseInc = juce::jmin (baseInc, Blep4::maxPhaseStep);
    }

    /** One sample at the note's own pitch. */
    float nextSample() noexcept   { return nextSampleWithStep (phaseInc); }

    /** One sample with the pitch multiplied by `pitchRatio` (v1.9.0), clamped as setFrequency() clamps. */
    float nextSample (float pitchRatio) noexcept
    {
        return nextSampleWithStep (juce::jmin (baseInc * pitchRatio, Blep4::maxPhaseStep));
    }

    /** The 16 levels of the current cycle (tests and visualisers). */
    const std::array<float, numSteps>& getLevels() const noexcept   { return levels; }

private:
    float nextSampleWithStep (float phaseStep) noexcept
    {
        const float s     = phase * (float) numSteps;
        const int   step  = juce::jmin ((int) s, numSteps - 1);
        const float local = s - (float) step;                                 // 0..1 within the step
        const float dt    = juce::jmin (phaseStep * (float) numSteps, 0.5f);  // phase step within a step; capped

        if (step != lastStep)
            advanceTo (step);

        const float current = levels[(size_t) step];
        float value = current;

        // Within two samples after the edge into this step, or before the edge out of it
        // (both, when a step is shorter than four samples). NAIVE: neither (v1.12.0).
        if (! naive)
        {
            if (local < 2.0f * dt)
                value -= (current - levels[(size_t) ((step + numSteps - 1) % numSteps)]) * Blep4::tail (local / dt);
            if (local > 1.0f - 2.0f * dt)
                value += (levels[(size_t) ((step + 1) % numSteps)] - current) * Blep4::tail ((1.0f - local) / dt);
        }

        // CHOP: with Comb = 1 its edges are Random's own step edges, where the wave's value
        // is the midpoint of the two levels; with a higher Comb the extra edges fall inside a step.
        value = chop.apply (value, phase, phaseStep, [this] (int segment) noexcept
        {
            const int comb = chop.getSegments() / numSteps;
            const int k = segment / comb;
            const float level = levels[(size_t) (k % numSteps)];
            return segment % comb == 0 ? 0.5f * (level + levels[(size_t) ((k + numSteps - 1) % numSteps)]) : level;
        });

        phase += phaseStep;
        if (phase >= 1.0f)
        {
            phase -= 1.0f;
            naive = pendingNaive;   // v1.12.0, at the cycle boundary
            chop.setHardEdges (naive);
            chop.startCycle();
        }
        return value;   // no treble compensation: at high notes Random's steps run far above Nyquist, and the boost would amplify them
    }

    /** Counts the steps entered since the last re-roll; every 17th re-rolls the level 8 steps ahead. */
    void advanceTo (int step) noexcept
    {
        stepsSinceReroll += (step - lastStep + numSteps) % numSteps;
        lastStep = step;
        if (stepsSinceReroll >= numSteps + 1)
        {
            stepsSinceReroll = 0;
            levels[(size_t) ((step + numSteps / 2) % numSteps)] = nextRandom();
        }
    }

    /** xorshift32, mapped to -1..1. */
    float nextRandom() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float) (juce::int32) state * (1.0f / 2147483648.0f);
    }

    std::array<float, numSteps> levels {};
    Chop chop;
    juce::uint32 state = goldenRatioSeed;
    float phase = 0.0f, phaseInc = 0.0f;
    float baseInc = 0.0f;   // hz / sampleRate before the clamp: what a pitch ratio multiplies
    int   lastStep = 0, stepsSinceReroll = 0;
    bool  naive = false, pendingNaive = false;   // v1.12.0
};
