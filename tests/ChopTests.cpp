// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Unit tests for CHOP (Chop.h): Overtone 0 changes nothing, chopped steps
    are silenced at 100% and inverted at 200%, Comb repeats the pattern, the
    edges are band-limited, and CHOP fades out where steps get too short.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target ChopTests
    Run the resulting ChopTests executable; it exits non-zero on failure.
*/
#include <juce_audio_basics/juce_audio_basics.h>
#include "dsp/Oscillator.h"
#include <cstdio>
#include <cmath>
#include <array>
#include <algorithm>
#include <vector>

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

    /** An oscillator on `waveform` at `hz` with CHOP settled (Overtone glide finished, pattern and Comb in effect). */
    PolyBlepOscillator make (int waveform, float hz, ChopSettings chop)
    {
        PolyBlepOscillator osc;
        osc.prepare (rate);
        osc.setWaveform (waveform);
        osc.setFrequency (hz, rate);
        osc.setChop (chop);
        for (int i = 0; i < 4800 || osc.getWaveform() != waveform; ++i)   // 100 ms: the glide has ended
            osc.nextSample();
        // Run to the end of a cycle so the next sample starts one (phase 0).
        float last = osc.nextSample(), now = osc.nextSample();
        (void) last; (void) now;
        return osc;
    }

    std::vector<float> render (PolyBlepOscillator& osc, int n)
    {
        std::vector<float> out ((size_t) n);
        for (auto& x : out)
            x = osc.nextSample();
        return out;
    }
}

int main()
{
    char what[200];

    std::printf ("Overtone 0%% changes nothing\n");
    for (const int w : { 0, 1, 2, 3 })
    {
        PolyBlepOscillator plain, chopped;
        plain.prepare (rate);
        chopped.prepare (rate);
        for (auto* o : { &plain, &chopped })
        {
            o->setWaveform (w);
            o->setFrequency (440.0f, rate);
        }
        chopped.setChop ({ ChopSettings::fromPattern (0xAAAA), 4, 0.0f });
        bool same = true;
        for (int i = 0; i < 48000; ++i)
            same = same && plain.nextSample() == chopped.nextSample();
        std::snprintf (what, sizeof (what), "waveform %d with Overtone 0%% is bit-identical to no CHOP", w);
        check (same, what);
    }

    // 375 Hz at 48 kHz: exactly 128 samples a cycle, 8 per step, so steps can be measured by sample.
    constexpr int cycle = 128, stepLength = 8;
    const auto stepRms = [&] (const std::vector<float>& x, int step, int cycles)
    {
        // RMS over the middle of the step (away from the smoothed edges), across several cycles
        double sum = 0.0;
        int n = 0;
        for (int c = 0; c < cycles; ++c)
            for (int i = 2; i < stepLength - 2; ++i)
            {
                const int index = c * cycle + step * stepLength + i;
                const float v = x[(size_t) index];
                sum += (double) v * v;
                ++n;
            }
        return std::sqrt (sum / n);
    };

    std::printf ("Overtone 100%% and 200%%\n");
    {
        auto open   = make (PolyBlepOscillator::sawtooth, 375.0f, { ChopSettings::fromPattern (0xAAAA), 1, 0.0f });
        auto silent = make (PolyBlepOscillator::sawtooth, 375.0f, { ChopSettings::fromPattern (0xAAAA), 1, 1.0f });
        auto invert = make (PolyBlepOscillator::sawtooth, 375.0f, { ChopSettings::fromPattern (0xAAAA), 1, 2.0f });
        const auto a = render (open, cycle * 20), b = render (silent, cycle * 20), c = render (invert, cycle * 20);

        double worstSilent = 0.0, worstKept = 0.0, worstInverted = 0.0;
        for (int step = 0; step < 16; ++step)
        {
            const bool chopped = (0xAAAA >> step) & 1;
            const double reference = stepRms (a, step, 20);
            if (reference < 1e-3)
                continue;
            if (chopped)
            {
                worstSilent = std::max (worstSilent, stepRms (b, step, 20) / reference);
                worstInverted = std::max (worstInverted, std::abs (stepRms (c, step, 20) / reference - 1.0));
            }
            else
                worstKept = std::max (worstKept, std::abs (stepRms (b, step, 20) / reference - 1.0));
        }
        std::snprintf (what, sizeof (what), "100%%: chopped steps fall to %.1f%% of their level (under 2%%), open steps keep it within %.1f%%",
                       100.0 * worstSilent, 100.0 * worstKept);
        check (worstSilent < 0.02 && worstKept < 0.03, what);

        // At 200% the chopped steps are the original upside down: their sum with it cancels.
        double residual = 0.0, reference = 0.0;
        for (int cyc = 0; cyc < 20; ++cyc)
            for (int step = 1; step < 16; step += 2)
                for (int i = 2; i < stepLength - 2; ++i)
                {
                    const int index = cyc * cycle + step * stepLength + i;
                    const size_t k = (size_t) index;
                    residual += std::pow ((double) a[k] + c[k], 2.0);
                    reference += (double) a[k] * a[k];
                }
        std::snprintf (what, sizeof (what), "200%%: chopped steps are inverted (same level within %.1f%%, residual after adding the original %.2f%%)",
                       100.0 * worstInverted, 100.0 * std::sqrt (residual / reference));
        check (worstInverted < 0.03 && std::sqrt (residual / reference) < 0.03, what);
    }

    std::printf ("Depths (v1.13.0)\n");
    {
        // Step k at depth k (0..15), Overtone 100%: each step's level falls to 1 - k/15 of the open wave's.
        ChopSettings::Depths ramp = 0;
        for (int k = 0; k < 16; ++k)
            ramp = ChopSettings::withDepth (ramp, k, k);
        auto open   = make (PolyBlepOscillator::sawtooth, 375.0f, { ChopSettings::fromPattern (0x0000), 1, 1.0f });
        auto ramped = make (PolyBlepOscillator::sawtooth, 375.0f, { ramp, 1, 1.0f });
        const auto a = render (open, cycle * 20), b = render (ramped, cycle * 20);
        double worst = 0.0;
        for (int step = 0; step < 16; ++step)
        {
            const double reference = stepRms (a, step, 20);
            if (reference < 1e-3)
                continue;
            // make() leaves the oscillator half a cycle in, so measured step s is pattern step s + 8
            // (the 0xAAAA tests above look the same either way).
            const int patternStep = (step + 8) % 16;
            worst = std::max (worst, std::abs (stepRms (b, step, 20) / reference - (1.0 - patternStep / 15.0)));
        }
        std::snprintf (what, sizeof (what), "a ramp of depths 0..15 gives levels 1 - k/15 step by step (worst error %.2f%%, under 3%%)", 100.0 * worst);
        check (worst < 0.03, what);

        // NAIVE's hard edges: the same ramp, sample for sample inside each step.
        PolyBlepOscillator naiveOpen, naiveRamp;
        for (auto* o : { &naiveOpen, &naiveRamp })
        {
            o->prepare (rate);
            o->setWaveform (PolyBlepOscillator::sawtooth);
            o->setNaive (true);
            o->setFrequency (375.0f, rate);
        }
        naiveOpen.setChop ({ ChopSettings::fromPattern (0x0000), 1, 1.0f });
        naiveRamp.setChop ({ ramp, 1, 1.0f });
        for (int i = 0; i < 4800; ++i)
        {
            naiveOpen.nextSample();
            naiveRamp.nextSample();
        }
        // Every sample is the open one times one of the 16 levels, and all 16 occur.
        double hardWorst = 0.0;
        std::array<bool, 16> seen {};
        for (int i = 0; i < cycle * 4; ++i)
        {
            const float plain = naiveOpen.nextSample(), cut = naiveRamp.nextSample();
            if (std::abs (plain) < 0.05f)
                continue;
            const double ratio = (double) cut / plain;
            const int k = juce::jlimit (0, 15, (int) std::lround ((1.0 - ratio) * 15.0));
            seen[(size_t) k] = true;
            hardWorst = std::max (hardWorst, std::abs (ratio - (1.0 - k / 15.0)));
        }
        const bool allSeen = std::all_of (seen.begin(), seen.end(), [] (bool present) { return present; });
        std::snprintf (what, sizeof (what), "NAIVE: every sample scaled by exactly one of the 16 levels 1 - k/15 (worst %.1e), all 16 present", hardWorst);
        check (hardWorst < 1.0e-5 && allSeen, what);

        // Depths 0 and 15 are v1.12's off and on: bit for bit the on/off pattern.
        auto onOff = make (PolyBlepOscillator::sawtooth, 375.0f, { ChopSettings::fromPattern (0x5A5A), 1, 0.7f });
        auto asDepths = make (PolyBlepOscillator::sawtooth, 375.0f,
                              { ChopSettings::withDepth (ChopSettings::fromPattern (0x5A5A), 0, 0), 1, 0.7f });
        const auto x = render (onOff, cycle * 10), y = render (asDepths, cycle * 10);
        check (x == y, "depths 0 and 15 render bit for bit as the on/off pattern they stand for");
        check (ChopSettings::isOnOff (ChopSettings::fromPattern (0x5A5A)) && ! ChopSettings::isOnOff (ramp)
                   && ChopSettings::toPattern (ramp) == 0xFF00,
               "an on/off pattern is recognised as one; a ramp's nearest on/off pattern is steps 9-16");
    }

    std::printf ("Comb\n");
    for (const int comb : { 1, 2, 8 })
    {
        // One chopped step (step 0) at 100%: silent stretches appear `comb` times per cycle.
        auto osc = make (PolyBlepOscillator::sawtooth, 46.875f, { ChopSettings::fromPattern (0x0001), comb, 1.0f });   // 1024 samples a cycle
        const auto x = render (osc, 1024 * 4);
        // Count silent stretches of at least a quarter of a step (the smoothed edges take two
        // samples at each end; the saw's own zero crossing is silent for only a sample).
        const int minRun = std::max (2, 1024 / (16 * comb) / 4);
        int silentRuns = 0, run = 0;
        for (size_t i = 1024; i < 2048; ++i)   // one whole cycle
        {
            run = std::abs (x[i]) < 1e-3f ? run + 1 : 0;
            if (run == minRun)
                ++silentRuns;
        }
        std::snprintf (what, sizeof (what), "Comb %d gives %d silent step%s per cycle", comb, silentRuns, silentRuns == 1 ? "" : "s");
        check (silentRuns == comb, what);
    }

    std::printf ("Band-limited edges\n");
    {
        // CHOP's smoothed edges against the same chop with hard gain steps. The reference is
        // built here from the same band-limited saw (Blep4) and passed through the same treble
        // compensation, so the only difference is how the chop edges are made.
        const float hz = 440.25f, dt = hz / (float) rate;
        PolyBlepOscillator osc;
        osc.prepare (rate);
        osc.setWaveform (PolyBlepOscillator::sawtooth);
        osc.setFrequency (hz, rate);
        osc.setChop ({ ChopSettings::fromPattern (0xAAAA), 1, 1.0f });
        osc.nextSample();   // the sine's first cycle ends at once (phase 0): from here both run the saw
        Blep4::TrebleCompensation treble;
        float phase = dt;
        for (int i = 0; i < 48000; ++i)   // let the Overtone glide finish, keeping the reference in step
        {
            osc.nextSample();
            phase += dt;
            if (phase >= 1.0f) phase -= 1.0f;
        }
        constexpr int n = 8192;
        std::vector<float> smooth (n), hard (n);
        for (int i = 0; i < n; ++i)
        {
            const int step = std::min ((int) (phase * 16.0f), 15);
            const float saw = 2.0f * phase - 1.0f - 2.0f * Blep4::step (phase, dt);
            smooth[(size_t) i] = osc.nextSample();
            hard[(size_t) i] = treble.process (((0xAAAA >> step) & 1) ? 0.0f : saw);
            phase += dt;
            if (phase >= 1.0f) phase -= 1.0f;
        }
        const auto bandEnergy = [&] (const std::vector<float>& x, double fromHz, double toHz)
        {
            double total = 0.0;
            for (int k = (int) (fromHz * n / rate); k < (int) (toHz * n / rate); ++k)
            {
                double re = 0.0, im = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    const double w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (n - 1));
                    const double angle = juce::MathConstants<double>::twoPi * (double) k * i / n;
                    re += w * x[(size_t) i] * std::cos (angle);
                    im -= w * x[(size_t) i] * std::sin (angle);
                }
                total += re * re + im * im;
            }
            return total;
        };
        const double gain = 10.0 * std::log10 (bandEnergy (hard, 18000.0, 24000.0) / bandEnergy (smooth, 18000.0, 24000.0));
        std::snprintf (what, sizeof (what), "smoothed CHOP edges leave %.1f dB less energy at 18-24 kHz than hard ones (at least 6 dB)", gain);
        check (gain > 6.0, what);
    }

    std::printf ("Short steps\n");
    {
        // A high note with Comb 32: steps far shorter than two samples. CHOP must fade out, so the
        // output is the plain wave, never louder than it.
        auto plain = make (PolyBlepOscillator::sawtooth, 2000.0f, { ChopSettings::fromPattern (0xAAAA), 32, 0.0f });
        auto osc   = make (PolyBlepOscillator::sawtooth, 2000.0f, { ChopSettings::fromPattern (0xAAAA), 32, 2.0f });
        const auto a = render (plain, 48000), b = render (osc, 48000);
        float peakA = 0.0f, peakB = 0.0f;
        bool same = true;
        for (size_t i = 0; i < a.size(); ++i)
        {
            peakA = std::max (peakA, std::abs (a[i]));
            peakB = std::max (peakB, std::abs (b[i]));
            same = same && a[i] == b[i];
        }
        std::snprintf (what, sizeof (what), "2 kHz, Comb 32 (1.5-sample steps): CHOP has faded out entirely (peak %.3f, plain %.3f)", peakB, peakA);
        check (same, what);

        // Between four and two samples per step it fades: the chopped wave stays within the plain one's peak.
        auto mid = make (PolyBlepOscillator::sawtooth, 1000.0f, { ChopSettings::fromPattern (0xAAAA), 1, 2.0f });   // 3-sample steps
        const auto c = render (mid, 48000);
        float peakC = 0.0f;
        for (const float v : c)
            peakC = std::max (peakC, std::abs (v));
        std::snprintf (what, sizeof (what), "1 kHz (3-sample steps, half faded): peak %.3f stays under 1.25", peakC);
        check (peakC < 1.25f, what);
    }

    std::printf ("Random source\n");
    {
        RandomStepOscillator plain, chopped;
        plain.prepare (rate);
        chopped.prepare (rate);
        plain.reseed (7);
        chopped.reseed (7);
        plain.setFrequency (220.0f, rate);
        chopped.setFrequency (220.0f, rate);
        chopped.setChop ({ ChopSettings::fromPattern (0xAAAA), 1, 0.0f });
        bool same = true;
        for (int i = 0; i < 48000; ++i)
            same = same && plain.nextSample() == chopped.nextSample();
        check (same, "Random with Overtone 0% is bit-identical to no CHOP");

        chopped.reseed (7);
        chopped.setChop ({ ChopSettings::fromPattern (0xFFFF), 1, 1.0f });   // every step chopped, 100%: silence once the glide ends
        float tail = 0.0f;
        for (int i = 0; i < 48000; ++i)
        {
            const float v = chopped.nextSample();
            if (i > 4800)
                tail = std::max (tail, std::abs (v));
        }
        std::snprintf (what, sizeof (what), "Random with every step chopped at 100%% falls silent (peak %.5f)", tail);
        check (tail < 1e-4f, what);
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
