// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    v1.12.0 "Fourier", the DSP: NAIVE and CUSTOM in the oscillators, CHOP and the voice.

    - NAIVE: the saw, square and triangle are 2t - 1, +-1 and the triangle sample for sample
      (phase-aligned), CHOP's edges are hard (also where band-limited CHOP fades out), Random's
      steps have no tails, and NAIVE switches at the cycle boundary.
    - NAIVE and CUSTOM off: the oscillators and CHOP render bit for bit as v1.11.1's (a frozen
      copy of those classes below is the reference), for every waveform and for Random, also
      after NAIVE or CUSTOM was set and cleared again within a cycle.
    - CUSTOM at C8 (48 kHz): nothing folds back above -90 dB of the fundamental; with NAIVE the
      partials above Nyquist fold back exactly where |k f0 - m fs| predicts, at their levels.
    - HARMONICS 1 of the SAW set is a pure sine.
    - No clicks: HARMONICS stepping and jumping, a glide across the harmonics' Nyquist limits,
      and a new spectrum mid-note (in the oscillator, and through the whole processor with
      the real WavetableBank) never step more than 1.1 x a steady note's largest step (the
      threshold PlayingTests uses), where a hard switch would.
    - A null table pointer plays the waveform, as CUSTOM off does.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target FourierDspTests
    Run the resulting FourierDspTests executable; it exits non-zero on failure.
*/
#include "plugin/NeonSynthProcessor.h"
#include "dsp/Oscillator.h"
#include "dsp/Wavetable.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

/**
    v1.11.1's Chop, PolyBlepOscillator and RandomStepOscillator, copied unchanged (only the
    namespace is new): the reference the oscillators must still match, bit for bit, with NAIVE
    and CUSTOM off. Never edit these.
*/
namespace v111
{
class Chop
{
public:
    static constexpr int stepsPerPattern = 16;

    /** Overtone glides over this long (10 ms), so turning it never steps the level. */
    static constexpr double overtoneGlideSeconds = 0.01;

    /** Floor for a segment's phase step in the fade-out test: keeps 1 / step finite. */
    static constexpr float minSegmentStep = 1.0e-6f;

    void prepare (double sampleRate) noexcept
    {
        gain.reset (sampleRate, overtoneGlideSeconds);
        gain.setCurrentAndTargetValue (1.0f - pendingOvertone);
    }

    void set (const ChopSettings& s) noexcept
    {
        pendingPattern  = s.pattern;
        pendingComb     = juce::jlimit (1, ChopSettings::maxComb, s.comb);
        pendingOvertone = juce::jlimit (0.0f, ChopSettings::maxOvertone, s.overtone);
        gain.setTargetValue (1.0f - pendingOvertone);
    }

    /** Call when a new cycle starts: the pattern and Comb take effect. */
    void startCycle() noexcept
    {
        pattern = pendingPattern;
        comb    = pendingComb;
    }

    /**
        Applies CHOP to one band-limited sample x of a wave at phase t (0..1) with
        phase step dt. edgeValue (segment) returns the wave's value at the start of
        segment k (0 .. 16 x Comb), taking the midpoint where the wave itself jumps.
    */
    template <typename EdgeValue>
    float apply (float x, float t, float dt, EdgeValue&& edgeValue) noexcept
    {
        if (! gain.isSmoothing() && gain.getTargetValue() == 1.0f)
            return x;   // Overtone 0: no CHOP, bit for bit

        const float chopGain = gain.getNextValue();
        const int   segments = stepsPerPattern * comb;
        const float position = t * (float) segments;
        const int   segment  = juce::jmin ((int) position, segments - 1);
        const float local    = position - (float) segment;
        const float segDt    = dt * (float) segments;   // phase step within a segment

        // Full CHOP while a segment lasts four samples or more, none below two.
        const float depth = juce::jlimit (0.0f, 1.0f, (1.0f / juce::jmax (segDt, minSegmentStep) - 2.0f) * 0.5f);
        if (depth <= 0.0f)
            return x;
        const float chopped = 1.0f - depth * (1.0f - chopGain);

        const auto gainOf = [&] (int k) noexcept
        {
            return ((pattern >> ((k + segments) % stepsPerPattern)) & 1u) != 0 ? chopped : 1.0f;
        };

        const float g = gainOf (segment);
        float y = g * x;

        if (local < 2.0f * segDt)   // just after this segment's first edge
        {
            const float before = gainOf (segment - 1);
            if (before != g)
                y -= (g - before) * edgeValue (segment) * Blep4::tail (local / segDt);
        }
        if (local > 1.0f - 2.0f * segDt)   // just before the next edge
        {
            const float after = gainOf (segment + 1);
            if (after != g)
                y += (after - g) * edgeValue (segment + 1) * Blep4::tail ((1.0f - local) / segDt);
        }
        return y;
    }

    int getSegments() const noexcept   { return stepsPerPattern * comb; }

private:
    juce::SmoothedValue<float> gain { 1.0f };   // 1 - Overtone
    juce::uint16 pattern = ChopSettings::defaultPattern, pendingPattern = ChopSettings::defaultPattern;
    int   comb = 1, pendingComb = 1;
    float pendingOvertone = 0.0f;
};

class PolyBlepOscillator
{
public:
    enum Waveform { sine = 0, square = 1, triangle = 2, sawtooth = 3 };

    /** Takes effect at the next cycle boundary. */
    void setWaveform (int newWaveform) noexcept   { pendingWaveform = newWaveform; }

    void prepare (double sampleRate) noexcept                { chop.prepare (sampleRate); }
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

        phase += dt;
        if (phase >= 1.0f)
        {
            phase -= 1.0f;
            waveform = pendingWaveform;   // switch shapes only at the cycle boundary
            chop.startCycle();
        }
        return out;
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
};
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
        // (both, when a step is shorter than four samples).
        if (local < 2.0f * dt)
            value -= (current - levels[(size_t) ((step + numSteps - 1) % numSteps)]) * Blep4::tail (local / dt);
        if (local > 1.0f - 2.0f * dt)
            value += (levels[(size_t) ((step + 1) % numSteps)] - current) * Blep4::tail ((1.0f - local) / dt);

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
};
} // namespace v111

namespace
{
    int failures = 0;

    void check (bool condition, const char* what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what);
        if (! condition)
            ++failures;
    }

    constexpr double rate = 48000.0;
    constexpr float clickMargin = 1.1f;   // PlayingTests' threshold: 1.1 x a steady note's largest step

    float largestStep (const std::vector<float>& x, size_t from = 0, size_t to = (size_t) -1)
    {
        float most = 0.0f;
        for (size_t i = from + 1; i < std::min (to, x.size()); ++i)
            most = std::max (most, std::abs (x[i] - x[i - 1]));
        return most;
    }

    WavetableSet makeSet (const HarmonicSpectrum& s)
    {
        WavetableSet set;
        set.build (s);
        return set;
    }

    /** Renders n samples; ratio (i) is the pitch ratio of sample i (nullptr: none). */
    template <typename Osc, typename Ratio>
    std::vector<float> render (Osc& osc, int n, Ratio ratio)
    {
        std::vector<float> out ((size_t) n);
        for (int i = 0; i < n; ++i)
            out[(size_t) i] = osc.nextSample (ratio (i));
        return out;
    }

    template <typename Osc>
    std::vector<float> render (Osc& osc, int n)
    {
        std::vector<float> out ((size_t) n);
        for (auto& x : out)
            x = osc.nextSample();
        return out;
    }

    bool bitIdentical (const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
    }

    // dt = 1/256 exactly (187.5 Hz at 48 kHz): every phase is a multiple of 2^-8, so the
    // textbook formulas can be compared exactly.
    constexpr int cycle = 256;
    constexpr float exactHz = (float) (rate / cycle);

    //==============================================================================
    void testNaiveWaves()
    {
        std::puts ("NAIVE: the textbook waves");
        const char* names[] = { "sine", "square", "triangle", "sawtooth" };
        for (int w = 0; w < 4; ++w)
        {
            PolyBlepOscillator osc;
            osc.prepare (rate);
            osc.setWaveform (w);
            osc.setFrequency (exactHz, rate);
            osc.setNaive (true);
            const auto out = render (osc, 4 * cycle);
            bool exact = true, firstCycleBandLimited = false;
            for (int i = cycle; i < 4 * cycle; ++i)
            {
                const float t = (float) (i % cycle) / (float) cycle;
                const float ideal = w == 1 ? (t < 0.5f ? 1.0f : -1.0f)
                                  : w == 2 ? (t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t)
                                  : w == 3 ? 2.0f * t - 1.0f
                                           : std::sin (juce::MathConstants<float>::twoPi * t);
                exact = exact && out[(size_t) i] == ideal;
            }
            // Before the first cycle boundary the wave was still band-limited (the switch waits).
            for (int i = 0; i < cycle; ++i)
            {
                const float t = (float) i / (float) cycle;
                const float naiveValue = w == 1 ? (t < 0.5f ? 1.0f : -1.0f) : w == 3 ? 2.0f * t - 1.0f : out[(size_t) i];
                firstCycleBandLimited = firstCycleBandLimited || out[(size_t) i] != naiveValue;
            }
            char what[160];
            std::snprintf (what, sizeof (what), "%s: exactly the textbook wave, sample for sample, from the first cycle boundary", names[w]);
            check (exact, what);
            if (w == 1 || w == 3)
            {
                std::snprintf (what, sizeof (what), "%s: the cycle before the boundary is still band-limited (NAIVE waits for it)", names[w]);
                check (firstCycleBandLimited, what);
            }
        }

        // Same level as the band-limited waves: the band-limited square and saw settle on +-1
        // and 2t - 1 away from their edges.
        PolyBlepOscillator bl;
        bl.prepare (rate);
        bl.setWaveform (PolyBlepOscillator::square);
        bl.setFrequency (exactHz, rate);
        const auto sq = render (bl, 4 * cycle);
        check (std::abs (sq[(size_t) (2 * cycle + cycle / 4)] - 1.0f) < 1.0e-3f, "the band-limited square sits at the same level (1) between its edges");

        std::puts ("NAIVE: CHOP's edges are hard");
        for (const float hz : { exactHz, (float) (rate / 16.0) })   // 187.5 Hz, and 3 kHz: one sample per segment
        {
            ChopSettings chop;
            chop.overtone = 1.0f;   // chopped steps silent; the default pattern chops every second step
            PolyBlepOscillator naive, bandLimited, unchopped;
            for (auto* o : { &naive, &bandLimited, &unchopped })
            {
                if (o != &unchopped)
                    o->setChop (chop);
                o->prepare (rate);   // after setChop: Overtone starts at its value, no glide
                o->setWaveform (PolyBlepOscillator::sawtooth);
                o->setFrequency (hz, rate);
            }
            naive.setNaive (true);
            const int perCycle = (int) std::lround (rate / hz);
            const auto a = render (naive, 4 * perCycle);
            const auto b = render (bandLimited, 4 * perCycle);
            const bool bandLimitedChops = ! bitIdentical (b, render (unchopped, 4 * perCycle));
            bool hard = true;
            for (int i = perCycle; i < 4 * perCycle; ++i)   // from the first cycle boundary
            {
                const float t = (float) (i % perCycle) / (float) perCycle;
                const int segment = (int) (t * (float) Chop::stepsPerPattern);
                const float expected = (segment & 1) != 0 ? 0.0f : 2.0f * t - 1.0f;
                hard = hard && a[(size_t) i] == expected;
            }
            char what[160];
            std::snprintf (what, sizeof (what), "%.1f Hz: chopped steps are exactly silent and the others exactly the saw, edges unsmoothed", (double) hz);
            check (hard, what);
            if (hz > 1000.0f)
                check (! bandLimitedChops, "  (where band-limited CHOP has faded out: segments of one sample)");
        }

        std::puts ("NAIVE: Random's steps are hard");
        {
            RandomStepOscillator osc;
            osc.reseed (1234u);
            osc.prepare (rate);
            osc.setFrequency (exactHz, rate);
            osc.setNaive (true);
            render (osc, cycle);   // to the cycle boundary
            bool hard = true;
            for (int i = 0; i < 8 * cycle; ++i)
            {
                const float x = osc.nextSample();
                const int step = (i % cycle) * RandomStepOscillator::numSteps / cycle;
                hard = hard && x == osc.getLevels()[(size_t) step];
            }
            check (hard, "every sample is exactly its step's level (no tails at the edges)");
        }
    }

    //==============================================================================
    /** v1.11.1's oscillators against today's, with the same settings, CHOP and a moving pitch. */
    void testBitIdentity()
    {
        std::puts ("NAIVE and CUSTOM off: bit for bit v1.11.1");
        const auto set = makeSet (HarmonicSpectrum::square());
        ChopSettings chop;
        chop.overtone = 0.7f;
        chop.comb = 3;
        const int n = 3 * (int) rate;
        const auto vibrato = [] (int i) { return (float) std::exp2 (0.02 * std::sin (i * 0.0007) + (i > 90000 ? 1.5 : 0.0)); };   // and a jump to near Nyquist

        // Three ways of leaving the new features off: never touched; set and cleared within the
        // first cycle (before they could take effect); CUSTOM on with no tables.
        for (int variant = 0; variant < 3; ++variant)
        {
            bool allSame = true;
            for (int w = 0; w < 4; ++w)
            {
                v111::PolyBlepOscillator ref;
                PolyBlepOscillator now;
                ref.setChop (chop);   now.setChop (chop);
                ref.prepare (rate);   now.prepare (rate);
                ref.setWaveform (w);  now.setWaveform (w);
                ref.setFrequency (1234.5f, rate);   now.setFrequency (1234.5f, rate);
                if (variant == 1)
                {
                    now.setNaive (true);
                    now.setCustom (true, 7, &set);
                    now.nextSample();
                    ref.nextSample();
                    now.setNaive (false);
                    now.setCustom (false, 7, &set);
                }
                if (variant == 2)
                    now.setCustom (true, 32, nullptr);
                const auto a = render (ref, n, vibrato);
                const auto b = render (now, n, vibrato);
                // A waveform change mid-note too.
                ref.setWaveform ((w + 1) % 4);
                now.setWaveform ((w + 1) % 4);
                allSame = allSame && bitIdentical (a, b) && bitIdentical (render (ref, 2000), render (now, 2000));
            }
            static const char* what[] = { "all four waveforms, with CHOP and a moving pitch",
                                          "the same after NAIVE and CUSTOM were set and cleared within a cycle",
                                          "CUSTOM on with no tables (a null pointer) plays the waveform, bit for bit" };
            check (allSame, what[variant]);
        }

        bool randomSame = true;
        for (int variant = 0; variant < 2; ++variant)
        {
            v111::RandomStepOscillator ref;
            RandomStepOscillator now;
            ref.reseed (99u);       now.reseed (99u);
            ref.setChop (chop);     now.setChop (chop);
            ref.prepare (rate);     now.prepare (rate);
            ref.setFrequency (220.0f, rate);   now.setFrequency (220.0f, rate);
            if (variant == 1)
            {
                now.setNaive (true);
                now.setNaive (false);
            }
            randomSame = randomSame && bitIdentical (render (ref, n, vibrato), render (now, n, vibrato));
        }
        check (randomSame, "Random, with CHOP and a moving pitch");
    }

    //==============================================================================
    /** In-place radix-2 FFT (as OscillatorTests'). */
    void fft (std::vector<double>& re, std::vector<double>& im)
    {
        const size_t n = re.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const double a = -juce::MathConstants<double>::twoPi / (double) len;
            for (size_t i = 0; i < n; i += len)
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const double c = std::cos (a * (double) k), s = std::sin (a * (double) k);
                    const double xr = re[i + k + len / 2] * c - im[i + k + len / 2] * s;
                    const double xi = re[i + k + len / 2] * s + im[i + k + len / 2] * c;
                    re[i + k + len / 2] = re[i + k] - xr;  im[i + k + len / 2] = im[i + k] - xi;
                    re[i + k] += xr;                       im[i + k] += xi;
                }
        }
    }

    /** Magnitudes of bins 0..n/2 of x (rectangular window: the tests play whole cycles). */
    std::vector<double> spectrum (const std::vector<float>& x)
    {
        std::vector<double> re (x.begin(), x.end()), im (x.size(), 0.0);
        fft (re, im);
        std::vector<double> mag (x.size() / 2 + 1);
        for (size_t k = 0; k < mag.size(); ++k)
            mag[k] = std::hypot (re[k], im[k]);
        return mag;
    }

    void testAliasing()
    {
        std::puts ("CUSTOM at C8 (48 kHz), SAW set");
        // f0 = 5715 bins of a 65536-point FFT (4185.8 Hz, C8 within a cent): the phase step
        // 5715 / 65536 is exact in a float, so the wave repeats exactly every 65536 samples
        // and every partial, folded or not, lands on a bin.
        constexpr int fftSize = 65536, f0Bin = 5715;
        const float hz = (float) ((double) f0Bin * rate / fftSize);
        const auto set = makeSet (HarmonicSpectrum::saw());
        char what[200];

        for (const bool naive : { false, true })
        {
            PolyBlepOscillator osc;
            osc.prepare (rate);
            osc.setFrequency (hz, rate);
            osc.setNaive (naive);
            osc.setCustom (true, HarmonicSpectrum::numHarmonics, &set);
            render (osc, fftSize);   // past the first cycle boundary and the table fade
            const auto mag = spectrum (render (osc, fftSize));
            const double fundamental = mag[(size_t) f0Bin];

            // Where each harmonic lands: k f0 folded about multiples of the sample rate.
            std::map<int, std::vector<int>> partials;   // bin -> the harmonics there
            const int played = naive ? HarmonicSpectrum::numHarmonics : WavetableSet::harmonicsBelowNyquist ((float) f0Bin / fftSize);
            for (int k = 1; k <= played; ++k)
            {
                int b = (k * f0Bin) % fftSize;
                partials[b <= fftSize / 2 ? b : fftSize - b].push_back (k);
            }
            double worstOther = 0.0;
            int worstBin = 0;
            for (size_t b = 1; b < mag.size(); ++b)
                if (partials.count ((int) b) == 0 && mag[b] > worstOther)
                {
                    worstOther = mag[b];
                    worstBin = (int) b;
                }
            const double otherDb = 20.0 * std::log10 (std::max (worstOther, 1.0e-30) / fundamental);

            if (! naive)
            {
                std::snprintf (what, sizeof (what), "band-limited: %d harmonics play, and nothing else reaches -90 dB of the fundamental (loudest %.1f dB, bin %d)",
                               played, otherDb, worstBin);
                check (played == 5 && otherDb < -90.0, what);
                continue;
            }

            // NAIVE: each folded harmonic sits where predicted, at its level (1/k of the fundamental
            // for the saw), where it does not share a bin with another.
            int folded = 0, atLevel = 0;
            for (const auto& [bin, ks] : partials)
            {
                if (ks.size() != 1 || ks[0] * f0Bin <= fftSize / 2)
                    continue;
                ++folded;
                const double db = 20.0 * std::log10 (mag[(size_t) bin] / fundamental), expected = -20.0 * std::log10 ((double) ks[0]);
                if (std::abs (db - expected) < 0.1)
                    ++atLevel;
            }
            std::snprintf (what, sizeof (what), "NAIVE: all %d folded partials are at |k f0 - m fs| at their own level (%d of them within 0.1 dB)", folded, atLevel);
            check (folded >= 20 && atLevel == folded, what);
            std::snprintf (what, sizeof (what), "NAIVE: nothing else above -90 dB (loudest %.1f dB)", otherDb);
            check (otherDb < -90.0, what);
        }

        std::puts ("HARMONICS 1 of the SAW set");
        {
            PolyBlepOscillator osc;
            osc.prepare (rate);
            osc.setFrequency (exactHz, rate);
            osc.setCustom (true, 1, &set);
            render (osc, cycle);
            double worst = 0.0;
            const double amplitude = 0.5 * HarmonicSpectrum::seriesGain;   // level 1/2, phase pi
            for (int i = 0; i < 4 * cycle; ++i)
            {
                const double t = (double) (i % cycle) / cycle;
                worst = std::max (worst, std::abs ((double) osc.nextSample() + amplitude * std::sin (juce::MathConstants<double>::twoPi * t)));
            }
            std::snprintf (what, sizeof (what), "a pure sine, -(2/pi) sin (2 pi t): largest error %.2g", worst);
            check (worst < 1.0e-5, what);
        }
    }

    /** The phase as PolyBlepOscillator steps it (in floats, the same operations), from 0. */
    struct PhaseTracker
    {
        explicit PhaseTracker (float hz) : dt ((float) (hz / rate)) {}
        void advance (int samples)
        {
            for (int i = 0; i < samples; ++i)
            {
                t += dt;
                if (t >= 1.0f)
                    t -= 1.0f;
            }
        }
        float dt, t = 0.0f;
    };

    //==============================================================================
    void testNoClicks()
    {
        char what[220];
        const auto square = makeSet (HarmonicSpectrum::square());
        const auto saw = makeSet (HarmonicSpectrum::saw());
        const float lowHz = 110.0f;

        /** A steady custom note's largest sample-to-sample step. */
        const auto steadyStep = [&] (const WavetableSet& set, int harmonics, float hz)
        {
            PolyBlepOscillator osc;
            osc.prepare (rate);
            osc.setFrequency (hz, rate);
            osc.setCustom (true, harmonics, &set);
            render (osc, (int) rate / 10);
            return largestStep (render (osc, (int) rate / 10));
        };

        std::puts ("HARMONICS stepping (BUILD) and jumping, SQUARE set at 110 Hz");
        {
            PolyBlepOscillator osc;
            osc.prepare (rate);
            osc.setFrequency (lowHz, rate);
            osc.setCustom (true, 1, &square);
            render (osc, 1000);

            // BUILD: 1, 2 .. 32, one every 20 ms; then jumps 32 -> 1 -> 32 -> 3 at odd times.
            std::vector<float> out;
            PhaseTracker phase (lowHz);
            phase.advance (1000);   // the warm-up
            int harmonics = 1;
            const auto play = [&] (int samples)
            {
                for (int i = 0; i < samples; ++i)
                    out.push_back (osc.nextSample());
                phase.advance (samples);
            };
            float hardStep = 0.0f;   // what a switch with no fade would have stepped, at the phases the switches came
            const auto setHarmonics = [&] (int h)
            {
                hardStep = std::max (hardStep, std::abs (square.read (h, phase.t) - square.read (harmonics, phase.t)));
                harmonics = h;
                osc.setCustom (true, h, &square);
            };
            for (int h = 2; h <= 32; ++h)
            {
                play (960);
                setHarmonics (h);
            }
            for (const auto& [wait, h] : { std::pair { 1337, 1 }, { 2011, 32 }, { 517, 3 }, { 1789, 32 }, { 263, 1 } })
            {
                play (wait);
                setHarmonics (h);
            }
            play (4800);
            const float limit = clickMargin * steadyStep (square, 32, lowHz);
            std::snprintf (what, sizeof (what), "largest step %.4f, under %.4f (1.1 x a steady 32-harmonic note's); a hard switch would step %.4f",
                           largestStep (out), limit, hardStep);
            check (largestStep (out) <= limit && hardStep > limit, what);
        }

        std::puts ("A glide across the harmonics' Nyquist limits, SAW set");
        for (const bool up : { true, false })
        {
            // C5 to C8 and back in 200 ms: the band limit falls from 32 harmonics to 5.
            PolyBlepOscillator osc;
            osc.prepare (rate);
            const float c5 = 523.25f;
            osc.setFrequency (c5, rate);
            osc.setCustom (true, 32, &saw);
            const int glide = (int) (0.2 * rate);
            const auto ratio = [=] (int i)
            {
                const double x = juce::jlimit (0.0, 1.0, (double) (i - 2400) / glide);
                return (float) std::exp2 (3.0 * (up ? x : 1.0 - x));
            };
            const auto out = render (osc, glide + 9600, ratio);
            // The reference: steady notes all along the path (a note just below a band edge, with
            // its top harmonic close to Nyquist, steps the most).
            float steadyMost = 0.0f;
            for (int k = 0; k <= 96; ++k)
                steadyMost = std::max (steadyMost, steadyStep (saw, 32, c5 * (float) std::exp2 (3.0 * k / 96.0)));
            const float limit = clickMargin * steadyMost;
            std::snprintf (what, sizeof (what), "%s: largest step %.4f, under %.4f (1.1 x the steady notes' along the path)", up ? "up" : "down", largestStep (out), limit);
            check (largestStep (out) <= limit, what);
        }

        std::puts ("A new spectrum mid-note (SAW set to SQUARE set at 110 Hz)");
        {
            PolyBlepOscillator osc;
            osc.prepare (rate);
            osc.setFrequency (lowHz, rate);
            osc.setCustom (true, 32, &saw);
            const auto cycleLength = rate / lowHz;
            // Three cycles and a quarter: the saw is at -0.5 and the square at +1 there.
            const int before = (int) (3.25 * cycleLength);
            auto out = render (osc, before);
            PhaseTracker phase (lowHz);
            phase.advance (before);
            const float hardStep = std::abs (square.read (32, phase.t) - saw.read (32, phase.t));
            osc.setCustom (true, 32, &square);
            const auto after = render (osc, (int) rate / 10);
            out.insert (out.end(), after.begin(), after.end());
            const float limit = clickMargin * std::max (steadyStep (saw, 32, lowHz), steadyStep (square, 32, lowHz));
            std::snprintf (what, sizeof (what), "largest step %.4f, under %.4f; a hard switch would step %.4f", largestStep (out), limit, hardStep);
            check (largestStep (out) <= limit && hardStep > limit, what);

            // After the 5 ms the offset is gone: exactly the square's table.
            const int settled = (int) (PolyBlepOscillator::spectrumFadeSeconds * rate) + 2;
            double worst = 0.0;
            phase.advance (settled);
            for (size_t i = (size_t) (before + settled); i < out.size(); ++i, phase.advance (1))
                worst = std::max (worst, (double) std::abs (out[i] - square.read (32, phase.t)));
            std::snprintf (what, sizeof (what), "after %.0f ms it is exactly the new table (largest difference %.2g)", PolyBlepOscillator::spectrumFadeSeconds * 1000.0, worst);
            check (worst == 0.0, what);
        }
    }

    //==============================================================================
    void setParameter (juce::AudioProcessor& p, const juce::String& id, float value)
    {
        for (auto* param : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param); ranged != nullptr && ranged->getParameterID() == id)
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
    }

    /** The whole processor: a held note with CUSTOM on and the filter off; the spectrum changes
        (through the real WavetableBank) after `changeBlock` blocks, if it is >= 0. */
    std::vector<float> renderProcessor (int changeBlock, bool naive)
    {
        constexpr int blockSize = 256;
        NeonSynthProcessor p;
        setParameter (p, "custom", 1.0f);
        setParameter (p, "naive", naive ? 1.0f : 0.0f);
        setParameter (p, "filterEnabled", 0.0f);
        setParameter (p, "sustain", 1.0f);
        p.setRateAndBufferSizeDetails (rate, blockSize);
        p.prepareToPlay (rate, blockSize);
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;
        std::vector<float> out;
        for (int b = 0; b < 160; ++b)
        {
            midi.clear();
            if (b == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 45, (juce::uint8) 100), 0);   // A2, 110 Hz
            if (b == changeBlock)
                p.setHarmonicSpectrum (HarmonicSpectrum::square());
            p.processBlock (buffer, midi);
            if (b >= 40)   // after the attack
                out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
        }
        p.releaseResources();
        return out;
    }

    void testProcessor()
    {
        std::puts ("Through the processor: a new spectrum mid-note");
        char what[200];
        for (const bool naive : { false, true })
        {
            const auto steadySaw = renderProcessor (-1, naive);
            const auto changed = renderProcessor (75, naive);   // 35 blocks after the recording starts
            const float limit = clickMargin * largestStep (steadySaw);
            std::snprintf (what, sizeof (what), "%s: largest step %.4f, under %.4f (1.1 x the steady saw's)", naive ? "NAIVE" : "band-limited", largestStep (changed), limit);
            check (largestStep (changed) <= limit, what);
            check (! bitIdentical (steadySaw, changed), "  and the spectrum did change the sound");
        }
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;   // the processor's JUCE classes expect it

    testNaiveWaves();
    testBitIdentity();
    testAliasing();
    testNoClicks();
    testProcessor();

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
