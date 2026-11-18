// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Unit tests for VoiceEnvelope, the synth voice's ADSR.

    The first test pins it to juce::ADSR for ordinary use (parameters set once),
    so the sound of existing patches doesn't change; the others cover the two
    things it does differently: a slewed sustain level, and a release that
    survives parameter updates. The last is a property test over the extremes of
    every setting (and seeded random ones): the level stays in [0, 1] and every
    note-off reaches silence in time.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target VoiceEnvelopeTests
    Run the resulting VoiceEnvelopeTests executable; it exits non-zero on failure.
*/
#include <juce_audio_basics/juce_audio_basics.h>
#include "../VoiceEnvelope.h"
#include <cstdio>
#include <cmath>
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

    constexpr double testSampleRate = 48000.0;

    /** Samples until the envelope goes idle (or limit). */
    int samplesUntilIdle (VoiceEnvelope& env, int limit, int paramEvery = 0, VoiceEnvelope::Parameters p = {})
    {
        for (int i = 0; i < limit; ++i)
        {
            if (paramEvery > 0 && i % paramEvery == 0)
                env.setParameters (p);
            env.getNextSample();
            if (! env.isActive())
                return i + 1;
        }
        return limit;
    }
}

int main()
{
    std::printf ("Matches juce::ADSR with fixed parameters\n");
    {
        const VoiceEnvelope::Parameters p { 0.010f, 0.050f, 0.6f, 0.100f };
        juce::ADSR reference;
        reference.setSampleRate (testSampleRate);
        reference.setParameters (p);
        VoiceEnvelope env;
        env.setSampleRate (testSampleRate);
        env.setParameters (p);

        float worst = 0.0f;
        reference.noteOn();
        env.noteOn();
        for (int i = 0; i < 9600; ++i)   // 200 ms held: attack, decay, sustain
            worst = std::max (worst, std::abs (reference.getNextSample() - env.getNextSample()));
        reference.noteOff();
        env.noteOff();
        for (int i = 0; i < 6000; ++i)   // release
            worst = std::max (worst, std::abs (reference.getNextSample() - env.getNextSample()));
        check (worst < 1.0e-5f && ! env.isActive() && ! reference.isActive(),
               "attack, decay, sustain and release match juce::ADSR sample for sample");
    }

    std::printf ("Release survives per-block parameter updates\n");
    {
        // Sustain 0, released halfway through the decay, parameters re-sent every
        // 64 samples as the voice does every block. juce::ADSR recomputes its
        // release rate from the sustain level (0) and stops the note dead.
        const VoiceEnvelope::Parameters p { 0.001f, 1.0f, 0.0f, 0.200f };
        VoiceEnvelope env;
        env.setSampleRate (testSampleRate);
        env.setParameters (p);
        env.noteOn();
        for (int i = 0; i < 24000; ++i) env.getNextSample();   // 0.5 s into a 1 s decay
        env.noteOff();
        const int releaseSamples = samplesUntilIdle (env, 48000, 64, p);
        check (std::abs (releaseSamples - 9600) <= 2, "release from mid-decay with Sustain 0 lasts the Release time (200 ms)");

        juce::ADSR reference;
        reference.setSampleRate (testSampleRate);
        reference.setParameters (p);
        reference.noteOn();
        for (int i = 0; i < 24000; ++i) reference.getNextSample();
        reference.noteOff();
        int referenceSamples = 0;
        for (; referenceSamples < 48000 && reference.isActive(); ++referenceSamples)
        {
            if (referenceSamples % 64 == 0) reference.setParameters (p);
            reference.getNextSample();
        }
        std::printf ("        (juce::ADSR in the same situation: %d samples)\n", referenceSamples);
    }

    std::printf ("Release from the attack keeps its speed\n");
    {
        const VoiceEnvelope::Parameters p { 1.0f, 0.1f, 0.5f, 0.100f };
        VoiceEnvelope env;
        env.setSampleRate (testSampleRate);
        env.setParameters (p);
        env.noteOn();
        for (int i = 0; i < 38400; ++i) env.getNextSample();   // 80% into the attack
        env.noteOff();
        const int releaseSamples = samplesUntilIdle (env, 48000, 64, p);
        check (std::abs (releaseSamples - 4800) <= 2, "release from level 0.8 takes the Release time (100 ms), not 1.6x");
    }

    std::printf ("Sustain slews\n");
    {
        VoiceEnvelope::Parameters p { 0.001f, 0.001f, 1.0f, 0.1f };
        VoiceEnvelope env;
        env.setSampleRate (testSampleRate);
        env.setParameters (p);
        env.noteOn();
        for (int i = 0; i < 4800; ++i) env.getNextSample();   // sustaining at 1.0
        p.sustain = 0.0f;
        env.setParameters (p);
        float previous = 1.0f, biggestStep = 0.0f;
        int samplesToSettle = 0;
        for (int i = 0; i < 4800; ++i)
        {
            const float v = env.getNextSample();
            biggestStep = std::max (biggestStep, std::abs (v - previous));
            previous = v;
            if (v > 0.0f) samplesToSettle = i + 1;
        }
        check (biggestStep <= (float) (1.0 / (VoiceEnvelope::sustainSlewSeconds * testSampleRate)) + 1.0e-6f
                   && std::abs (samplesToSettle - 480) <= 1,
               "sustain 1 -> 0 while held ramps over 10 ms (480 samples) instead of jumping");
    }

    std::printf ("Retrigger\n");
    {
        const VoiceEnvelope::Parameters p { 0.010f, 0.050f, 0.6f, 0.200f };
        VoiceEnvelope env;
        env.setSampleRate (testSampleRate);
        env.setParameters (p);
        env.noteOn();
        for (int i = 0; i < 9600; ++i) env.getNextSample();
        env.noteOff();
        float level = 0.0f;
        for (int i = 0; i < 2400; ++i) level = env.getNextSample();   // halfway through the release
        env.noteOn();
        const float next = env.getNextSample();
        check (next > level && next - level < 0.01f, "retriggering during the release attacks from the current level");
    }

    std::printf ("Release time change during the release\n");
    {
        VoiceEnvelope::Parameters p { 0.001f, 0.001f, 1.0f, 1.0f };
        VoiceEnvelope env;
        env.setSampleRate (testSampleRate);
        env.setParameters (p);
        env.noteOn();
        for (int i = 0; i < 480; ++i) env.getNextSample();
        env.noteOff();
        for (int i = 0; i < 24000; ++i) env.getNextSample();   // halfway down a 1 s release
        p.release = 0.1f;
        env.setParameters (p);
        const int remaining = samplesUntilIdle (env, 96000);
        check (std::abs (remaining - 4800) <= 2, "shortening Release mid-fade finishes in the new time, from the current level");
    }

    std::printf ("Edge values (property test)\n");
    {
        // Every combination of the extremes the knobs reach (attack and decay 0..5 s, sustain 0..1,
        // release 0..10 s; 0 stands for the shortest setting and for a host sending 0), at three
        // sample rates, plus seeded random settings. The note is held for a while, with parameters
        // re-sent every block as the voice does and the Sustain knob sometimes moved, and then
        // released. Throughout, the level must stay finite and within [0, 1], and after the note-off
        // the envelope must go silent within the Release time (plus a margin for the float rounding
        // of a long linear ramp). Some notes are retriggered during the release and released again.
        const float times[]    = { 0.0f, 0.001f, 5.0f };
        const float sustains[] = { 0.0f, 0.5f, 1.0f };
        struct Case { double rate; VoiceEnvelope::Parameters p; int holdSamples; bool retrigger; };
        std::vector<Case> cases;
        for (double rate : { 22050.0, 48000.0, 192000.0 })
            for (float a : times) for (float d : times) for (float s : sustains)
                for (float r : { 0.0f, 0.001f, rate == 48000.0 ? 10.0f : 0.25f })   // the longest release at one rate only (run time)
                    for (bool retrigger : { false, true })
                        if (r < 10.0f || retrigger)   // the retrigger case releases twice, so it covers the plain one
                            cases.push_back ({ rate, { a, d, s, r }, (int) (rate * 0.05), retrigger });

        juce::Random random (20261118);
        const auto randomTime = [&random] (float maxSeconds)   // log-uniform, with a few exact zeros
        {
            return random.nextInt (10) == 0 ? 0.0f : maxSeconds * std::pow (10.0f, -4.0f * random.nextFloat());
        };
        for (int i = 0; i < 400; ++i)
            cases.push_back ({ 48000.0,
                               { randomTime (0.5f), randomTime (0.5f), random.nextInt (4) == 0 ? (float) random.nextInt (2) : random.nextFloat(), randomTime (1.0f) },
                               1 + random.nextInt (48000), random.nextBool() });

        bool inRange = true, silentInTime = true;
        double worstOvershoot = 0.0;   // samples past the Release time, as a fraction of it
        int worstExtraSamples = 0;

        const auto observe = [&inRange] (float v) { inRange = inRange && std::isfinite (v) && v >= 0.0f && v <= 1.0f; };

        // Releases the note and returns how many samples it took to go silent (or the limit).
        const auto release = [&] (VoiceEnvelope& env, const VoiceEnvelope::Parameters& p, double rate)
        {
            env.noteOff();
            const auto allowed = (juce::int64) std::ceil ((double) p.release * rate);
            const auto limit   = allowed + allowed / 100 + 16;   // 1% + 16 samples
            juce::int64 n = 0;
            for (; env.isActive() && n <= limit; ++n)
            {
                if (n % 64 == 0)
                    env.setParameters (p);
                observe (env.getNextSample());
            }
            silentInTime = silentInTime && ! env.isActive();
            worstExtraSamples = juce::jmax (worstExtraSamples, (int) (n - allowed));
            if (allowed > 0)
                worstOvershoot = juce::jmax (worstOvershoot, (double) (n - allowed) / (double) allowed);
        };

        for (const auto& c : cases)
        {
            VoiceEnvelope env;
            env.setSampleRate (c.rate);
            auto p = c.p;
            env.setParameters (p);
            env.noteOn();
            for (int i = 0; i < c.holdSamples; ++i)
            {
                if (i % 64 == 0)
                {
                    if (random.nextInt (8) == 0)   // the Sustain knob moves while the note is held
                        p.sustain = random.nextInt (3) == 0 ? (float) random.nextInt (2) : random.nextFloat();
                    env.setParameters (p);
                }
                observe (env.getNextSample());
            }
            release (env, p, c.rate);

            if (c.retrigger)
            {
                env.noteOn();
                for (int i = 0; i < 3; ++i) observe (env.getNextSample());
                env.noteOff();   // released again a moment later, then retriggered during that release
                for (int i = 0; i < 100 && env.isActive(); ++i) observe (env.getNextSample());
                env.noteOn();
                for (int i = 0; i < 200; ++i) observe (env.getNextSample());
                release (env, p, c.rate);
            }
        }

        std::printf ("        (%d cases; latest silence: %d samples, %.4f%% after the Release time)\n",
                     (int) cases.size(), worstExtraSamples, 100.0 * worstOvershoot);
        check (inRange, "the level is always finite and within [0, 1]");
        check (silentInTime, "a note-off always reaches silence within the Release time (+1% + 16 samples)");
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
