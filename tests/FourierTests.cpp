// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    v1.12.0 "Fourier", the shared base: the wavetables, their hand-off to the audio thread and
    the session state of CUSTOM's harmonics and the drawn cycle.

    - The four presets, played from their 32-harmonic tables, match the oscillator's textbook
      waves away from the jumps (the Fourier series converges there), and overshoot by Gibbs' 9%
      at a jump.
    - Table n holds exactly harmonics 1..n (a DFT of each table), and harmonicsBelowNyquist()
      picks the highest harmonic strictly below half the sample rate.
    - WavetableBank: acquire() returns the newest published set, and a set the audio thread
      holds is never rebuilt while it holds it.
    - FourierState: <Harmonics> and <Drawing> are written only when they differ from a new
      session's, save and load round trip exactly, damaged values fall back to the default,
      and a session saved by v1.11.1 loads with CUSTOM off and the SAW set.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target FourierTests
*/
#include "plugin/NeonSynthProcessor.h"
#include "dsp/Wavetable.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <thread>

namespace
{
    int failures = 0;

    void check (bool condition, const char* what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what);
        if (! condition)
            ++failures;
    }

    constexpr double twoPi = juce::MathConstants<double>::twoPi;

    /** The textbook waves the presets approximate, at phase t (0..1). */
    double sawAt (double t)      { return 2.0 * t - 1.0; }
    double squareAt (double t)   { return t < 0.5 ? 1.0 : -1.0; }
    double triangleAt (double t) { return t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t; }

    /** Largest difference from `ideal`, ignoring a band of +-margin (in cycles) around each jump. */
    template <typename Ideal>
    double maxErrorAwayFromJumps (const WavetableSet& set, Ideal ideal, double margin)
    {
        double worst = 0.0;
        for (int i = 0; i < 4000; ++i)
        {
            const double t = (i + 0.5) / 4000.0;
            if (std::abs (t) < margin || std::abs (t - 0.5) < margin || std::abs (t - 1.0) < margin)
                continue;
            worst = std::max (worst, std::abs ((double) set.read (32, (float) t) - ideal (t)));
        }
        return worst;
    }

    /** Magnitude of harmonic k in table n (a DFT over the table's points). */
    double harmonicMagnitude (const WavetableSet& set, int n, int k)
    {
        const float* t = set.getTable (n);
        std::complex<double> sum;
        for (int i = 0; i < WavetableSet::tableSize; ++i)
            sum += (double) t[i] * std::exp (std::complex<double> (0.0, -twoPi * k * i / WavetableSet::tableSize));
        return 2.0 * std::abs (sum) / WavetableSet::tableSize;
    }

    void testPresets()
    {
        std::puts ("Presets against the textbook waves:");
        WavetableSet set;
        set.build (HarmonicSpectrum::saw());
        check (maxErrorAwayFromJumps (set, sawAt, 0.08) < 0.04, "SAW: within 0.04 of 2t - 1 away from the jump");
        set.build (HarmonicSpectrum::square());
        check (maxErrorAwayFromJumps (set, squareAt, 0.08) < 0.04, "SQUARE: within 0.04 of +-1 away from the jumps");

        float peak = 0.0f;
        for (int i = 0; i < 20000; ++i)
            peak = std::max (peak, set.read (32, (float) i / 20000.0f));
        std::printf ("    square's peak with 32 harmonics: %.4f\n", (double) peak);
        check (peak > 1.17f && peak < 1.19f, "SQUARE: Gibbs overshoots by about 9% of the jump (0.18 of 2)");

        set.build (HarmonicSpectrum::triangle());
        check (maxErrorAwayFromJumps (set, triangleAt, 0.0) < 0.02, "TRIANGLE: within 0.02 everywhere (no jumps; the corners round off)");
        set.build (HarmonicSpectrum::sine());
        double sineErr = 0.0;
        for (int i = 0; i < 1000; ++i)
            sineErr = std::max (sineErr, std::abs ((double) set.read (1, (float) i / 1000.0f) - std::sin (twoPi * i / 1000.0)));
        check (sineErr < 1.0e-5, "SINE: sin (2 pi t), as the oscillator's");
        check (std::abs (set.getScale() - HarmonicSpectrum::seriesGain) < 1.0e-6f, "a textbook wave is not rescaled");
    }

    void testTables()
    {
        std::puts ("Tables and band-limiting:");
        WavetableSet set;
        set.build (HarmonicSpectrum::saw());
        bool exact = true;
        for (int n : { 1, 5, 17, 32 })
            for (int k = 1; k <= 40; ++k)
            {
                const double expected = k <= n && k <= 32 ? 0.5 / k * HarmonicSpectrum::seriesGain : 0.0;
                if (std::abs (harmonicMagnitude (set, n, k) - expected) > 1.0e-4)
                    exact = false;
            }
        check (exact, "table n holds exactly harmonics 1..n at their levels");

        check (WavetableSet::harmonicsBelowNyquist (0.1f) == 4, "dt 0.1: harmonic 5 is at Nyquist, so 4 play");
        check (WavetableSet::harmonicsBelowNyquist (0.01f) == 32, "dt 0.01: all 32 are below Nyquist");
        check (WavetableSet::harmonicsBelowNyquist (0.3f) == 1, "dt 0.3: only the fundamental");
        check (WavetableSet::harmonicsBelowNyquist (0.0f) == 32, "dt 0: all 32");

        HarmonicSpectrum loud;
        loud.levels.fill (1.0f);
        set.build (loud);
        float peak = 0.0f;
        for (int i = 0; i < 20000; ++i)
            peak = std::max (peak, std::abs (set.read (32, (float) i / 20000.0f)));
        check (peak <= (float) WavetableSet::maxPeak + 0.01f, "32 harmonics at full level in phase are scaled to peak at maxPeak");
    }

    void testBank()
    {
        std::puts ("WavetableBank:");
        WavetableBank bank (HarmonicSpectrum::saw());
        check (bank.acquire().getSpectrum() == HarmonicSpectrum::saw(), "starts with the spectrum it was built with");
        bank.publish (HarmonicSpectrum::square());
        const auto& a = bank.acquire();
        check (a.getSpectrum() == HarmonicSpectrum::square(), "acquire() returns the newest published set");
        bank.publish (HarmonicSpectrum::triangle());
        bank.publish (HarmonicSpectrum::sine());   // two publishes while the audio thread holds `a`
        check (a.getSpectrum() == HarmonicSpectrum::square(), "the set the audio thread holds is not rebuilt under it");
        check (bank.acquire().getSpectrum() == HarmonicSpectrum::sine(), "the next acquire() gets the newest, skipping the one in between");
        check (bank.acquire().getSpectrum() == HarmonicSpectrum::sine(), "with nothing new, acquire() keeps its set");

        // Concurrent: a writer publishes 300 spectra while an "audio thread" acquires and reads.
        // Each spectrum has every level equal, so a set that is complete has table 32's first
        // point equal to its spectrum's prediction; a torn read (a set rebuilt under the reader)
        // would not. Under ThreadSanitizer (the TSan job) any race here fails the run.
        WavetableBank shared (HarmonicSpectrum::sine());
        std::atomic<bool> done { false };
        std::atomic<int> torn { 0 }, reads { 0 };
        std::thread audio ([&]
        {
            while (! done.load())
            {
                const auto& set = shared.acquire();
                const float level = set.getSpectrum().levels[0];
                double expected = 0.0;   // every harmonic at `level`, phase 0.25 rad: sum of level * sin (0.25)
                for (int k = 0; k < HarmonicSpectrum::numHarmonics; ++k)
                    expected += (double) set.getSpectrum().levels[(size_t) k] * std::sin ((double) set.getSpectrum().phases[(size_t) k]);
                if (level > 0.0f && std::abs ((double) set.getTable (32)[0] - expected * set.getScale()) > 1.0e-4)
                    torn.fetch_add (1);
                reads.fetch_add (1);
            }
        });
        for (int i = 1; i <= 300; ++i)
        {
            HarmonicSpectrum s;
            s.levels.fill ((float) (i % 7 + 1) / 32.0f);
            s.phases.fill (0.25f);
            shared.publish (s);
        }
        done.store (true);
        audio.join();
        std::printf ("    %d reads while publishing\n", reads.load());
        check (torn.load() == 0, "concurrent publish and acquire: every set read is complete");
    }

    juce::MemoryBlock saveState (NeonSynthProcessor& p)
    {
        juce::MemoryBlock block;
        p.getStateInformation (block);
        return block;
    }

    juce::String stateXml (NeonSynthProcessor& p)
    {
        const auto block = saveState (p);
        return juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize())->toString();
    }

    void loadXml (NeonSynthProcessor& p, const juce::String& xml)
    {
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (*juce::parseXML (xml), block);
        p.setStateInformation (block.getData(), (int) block.getSize());
    }

    void testState()
    {
        std::puts ("Session state:");
        NeonSynthProcessor p;
        const auto fresh = stateXml (p);
        check (! fresh.contains ("<Harmonics") && ! fresh.contains ("<Drawing"), "a new session writes neither element");

        HarmonicSpectrum s = HarmonicSpectrum::square();
        s.phases[2] = 1.25f;
        p.setHarmonicSpectrum (s);
        FourierState::Drawing drawing {};
        for (int i = 0; i < FourierState::drawingPoints; ++i)
            drawing[(size_t) i] = (float) std::sin (twoPi * i / FourierState::drawingPoints) * 0.5f;
        p.setCycleDrawing (drawing, s);
        const auto saved = saveState (p);

        NeonSynthProcessor q;
        q.setStateInformation (saved.getData(), (int) saved.getSize());
        check (q.getHarmonicSpectrum() == p.getHarmonicSpectrum(), "the spectrum round trips exactly");
        check (q.getCycleDrawing().has_value() && *q.getCycleDrawing() == drawing, "the drawing round trips exactly");

        // Loading a session without them (as v1.11.1 saved) restores a new session's values.
        loadXml (q, fresh);
        check (q.getHarmonicSpectrum() == HarmonicSpectrum::defaultSpectrum(), "a session without <Harmonics> loads the SAW set");
        check (! q.getCycleDrawing().has_value(), "a session without <Drawing> loads none");
        check (! (bool) q.getAPVTS().getParameter ("custom")->getValue(), "CUSTOM starts off");

        // Damaged elements fall back to the default; nothing reaches the tables unchecked.
        const char* damaged[] = {
            R"(<Harmonics levels="1 2 3" phases="0"/>)",
            R"(<Harmonics levels="nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan nan" phases="0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0"/>)",
            R"(<Drawing points="a b c"/>)",
        };
        bool safe = true;
        for (const auto* element : damaged)
        {
            loadXml (q, fresh.replace ("</Parameters>", juce::String (element) + "</Parameters>"));
            const auto got = q.getHarmonicSpectrum();
            for (int k = 0; k < HarmonicSpectrum::numHarmonics; ++k)
                if (! std::isfinite (got.levels[(size_t) k]) || got.levels[(size_t) k] < 0.0f || got.levels[(size_t) k] > 1.0f)
                    safe = false;
            if (q.getCycleDrawing().has_value())
                safe = false;
        }
        check (safe, "damaged or hostile values are ignored or clamped");

        // Out-of-range levels and phases are clamped and wrapped.
        HarmonicSpectrum wild;
        wild.levels[0] = 7.0f;
        wild.levels[1] = -1.0f;
        wild.phases[0] = 100.0f;
        p.setHarmonicSpectrum (wild);
        const auto got = p.getHarmonicSpectrum();
        check (got.levels[0] == 1.0f && got.levels[1] == 0.0f && got.phases[0] >= 0.0f
               && got.phases[0] < juce::MathConstants<float>::twoPi, "levels clamp to 0..1, phases wrap to 0..2 pi");
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testPresets();
    testTables();
    testBank();
    testState();
    std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
