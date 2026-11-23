// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    v1.12.0 "Fourier", DRAW: a drawn cycle becomes CUSTOM's 32 harmonics (CycleAnalysis.h).

    - A cycle made of harmonics 1..32 with known levels and phases gives them back (to 1e-5).
    - A constant offset (DC) is dropped, and leaves the harmonics as they were.
    - A drawn square (+1 for the first half, -1 for the second) gives the SQUARE preset: odd
      levels 1/k (within 1% of full scale; the 256 sampled points put the highest harmonics a
      little above 1/k, which is the DFT of a sampled square, not an error), even levels ~0.
    - A drawing whose fundamental would need a level above 1 is scaled so the largest is 1,
      keeping the ratios between harmonics.
    - Phases always lie in 0..2 pi, and a silent harmonic's phase is 0.
    - resynthesise() gives back a band-limited cycle exactly, uses the tables' scale, and its
      first n harmonics are what table n of a WavetableSet plays.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target CycleAnalysisTests
*/
#include "dsp/CycleAnalysis.h"
#include <cmath>
#include <cstdio>

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
    constexpr int numHarmonics = HarmonicSpectrum::numHarmonics;

    /** The series sum (gain 4/pi) of a spectrum at the 256 drawing points, in double precision. */
    CycleAnalysis::Cycle cycleOf (const HarmonicSpectrum& s, double offset = 0.0)
    {
        CycleAnalysis::Cycle c {};
        for (int i = 0; i < CycleAnalysis::numPoints; ++i)
        {
            double sum = offset;
            for (int k = 1; k <= numHarmonics; ++k)
                sum += HarmonicSpectrum::seriesGain * s.levels[(size_t) (k - 1)]
                       * std::sin (twoPi * k * i / CycleAnalysis::numPoints + s.phases[(size_t) (k - 1)]);
            c[(size_t) i] = (float) sum;
        }
        return c;
    }

    /** Smallest difference between two angles, allowing for wrap-around. */
    double angleDifference (double a, double b)
    {
        const double d = std::fmod (std::abs (a - b), twoPi);
        return std::min (d, twoPi - d);
    }

    /** A spectrum with every harmonic present at a different level and phase, peaking within -1..1. */
    HarmonicSpectrum knownSpectrum()
    {
        HarmonicSpectrum s;
        for (int k = 1; k <= numHarmonics; ++k)
        {
            s.levels[(size_t) (k - 1)] = 0.02f + 0.3f / (float) k;
            s.phases[(size_t) (k - 1)] = (float) std::fmod (0.7 * k + 0.3, twoPi);
        }
        return s;
    }

    bool matches (const HarmonicSpectrum& got, const HarmonicSpectrum& want, double tolerance)
    {
        for (size_t k = 0; k < (size_t) numHarmonics; ++k)
            if (std::abs (got.levels[k] - want.levels[k]) > tolerance
                || angleDifference (got.phases[k], want.phases[k]) > tolerance)
                return false;
        return true;
    }

    bool phasesInRange (const HarmonicSpectrum& s)
    {
        for (auto p : s.phases)
            if (! (p >= 0.0f && p < juce::MathConstants<float>::twoPi))
                return false;
        return true;
    }
}

int main()
{
    std::printf ("Analysis of a cycle of known harmonics\n");
    {
        const auto want = knownSpectrum();
        const auto got = CycleAnalysis::analyse (cycleOf (want));
        check (matches (got, want, 1.0e-5), "32 levels and phases come back within 1e-5");

        const auto offset = CycleAnalysis::analyse (cycleOf (want, 0.4));
        check (matches (offset, want, 1.0e-5), "a constant offset (DC) is dropped: the same harmonics");

        CycleAnalysis::Cycle flat {};
        flat.fill (0.5f);
        const auto dc = CycleAnalysis::analyse (flat);
        bool silent = true;
        for (size_t k = 0; k < (size_t) numHarmonics; ++k)
            silent = silent && dc.levels[k] < 1.0e-6f && dc.phases[k] == 0.0f;
        check (silent, "a flat line (DC only) has no harmonics, and their phases are 0");

        HarmonicSpectrum one;
        one.levels[4] = 0.5f;
        one.phases[4] = 2.0f;
        const auto fifth = CycleAnalysis::analyse (cycleOf (one));
        bool onlyFifth = std::abs (fifth.levels[4] - 0.5f) < 1.0e-5f && std::abs (fifth.phases[4] - 2.0f) < 1.0e-5f;
        for (size_t k = 0; k < (size_t) numHarmonics; ++k)
            onlyFifth = onlyFifth && (k == 4 || (fifth.levels[k] < 1.0e-6f && fifth.phases[k] == 0.0f));
        check (onlyFifth, "one harmonic alone: only it, the others silent with phase 0");
    }

    std::printf ("\nA drawn square gives the SQUARE preset\n");
    {
        CycleAnalysis::Cycle square {};
        for (int i = 0; i < CycleAnalysis::numPoints; ++i)
            square[(size_t) i] = i < CycleAnalysis::numPoints / 2 ? 1.0f : -1.0f;
        const auto got = CycleAnalysis::analyse (square);
        const auto want = HarmonicSpectrum::square();

        double worstOdd = 0.0, worstEven = 0.0;
        for (int k = 1; k <= numHarmonics; ++k)
        {
            const double error = std::abs (got.levels[(size_t) (k - 1)] - want.levels[(size_t) (k - 1)]);
            (k % 2 == 1 ? worstOdd : worstEven) = std::max (k % 2 == 1 ? worstOdd : worstEven, error);
        }
        std::printf ("        largest level error: odd k %.5f, even k %.2g\n", worstOdd, worstEven);
        check (worstOdd < 0.01, "odd levels within 1% of 1/k");
        check (worstEven < 1.0e-5, "even levels ~0");
        check (std::abs (got.levels[0] - 1.0f) < 1.0e-3f && std::abs (got.levels[2] - 1.0f / 3.0f) / (1.0f / 3.0f) < 0.01f,
               "the fundamental at 1 and the third at 1/3 (within 1% of their own size)");
        check (got.levels[0] <= 1.0f, "no level above 1");
        check (phasesInRange (got), "phases in 0..2 pi");
    }

    std::printf ("\nScaling when a level would exceed 1\n");
    {
        HarmonicSpectrum loud;
        loud.levels[0] = 1.0f;
        loud.levels[1] = 0.5f;
        loud.levels[6] = 0.25f;
        CycleAnalysis::Cycle c = cycleOf (loud);
        for (auto& v : c)
            v *= 1.5f;   // the fundamental now needs a level of 1.5
        const auto got = CycleAnalysis::analyse (c);
        check (std::abs (got.levels[0] - 1.0f) < 1.0e-5f, "the largest level becomes exactly 1");
        check (std::abs (got.levels[1] - 0.5f) < 1.0e-5f && std::abs (got.levels[6] - 0.25f) < 1.0e-5f,
               "the others keep their ratios to it (0.5 and 0.25)");
        float largest = 0.0f;
        for (auto l : got.levels)
            largest = std::max (largest, l);
        check (largest <= 1.0f, "no level above 1");
    }

    std::printf ("\nPhases\n");
    {
        bool allInRange = true;
        for (int seed = 1; seed <= 20; ++seed)
        {
            juce::Random random (seed);
            CycleAnalysis::Cycle c {};
            for (auto& v : c)
                v = random.nextFloat() * 2.0f - 1.0f;
            allInRange = allInRange && phasesInRange (CycleAnalysis::analyse (c));
        }
        check (allInRange, "20 random drawings: every phase in 0..2 pi");

        HarmonicSpectrum nearTwoPi;
        nearTwoPi.levels[0] = 0.5f;
        nearTwoPi.phases[0] = juce::MathConstants<float>::twoPi - 1.0e-7f;
        check (phasesInRange (CycleAnalysis::analyse (cycleOf (nearTwoPi))), "a phase just below 2 pi stays below it");
    }

    std::printf ("\nResynthesis\n");
    {
        const auto spectrum = knownSpectrum();
        const auto original = cycleOf (spectrum);
        const auto back = CycleAnalysis::resynthesise (CycleAnalysis::analyse (original));
        double worst = 0.0;
        for (size_t i = 0; i < original.size(); ++i)
            worst = std::max (worst, (double) std::abs (back[i] - original[i]));
        check (worst < 1.0e-4, "analyse then resynthesise gives a band-limited cycle back (within 1e-4)");

        WavetableSet set;
        set.build (spectrum);
        check (std::abs (CycleAnalysis::playingScale (spectrum) - set.getScale()) < 1.0e-6f, "the same scale the voices' tables use (4/pi here)");

        HarmonicSpectrum huge;
        huge.levels.fill (1.0f);   // peaks far above WavetableSet::maxPeak
        WavetableSet hugeSet;
        hugeSet.build (huge);
        check (CycleAnalysis::playingScale (huge) < HarmonicSpectrum::seriesGain
                   && std::abs (CycleAnalysis::playingScale (huge) - hugeSet.getScale()) < 1.0e-6f,
               "and the same reduced scale when the wave would peak above maxPeak");

        bool tablesMatch = true;
        std::array<float, 64> wave {};
        for (int n : { 1, 5, 32 })
        {
            CycleAnalysis::resynthesise (spectrum, n, wave.data(), (int) wave.size(), CycleAnalysis::playingScale (spectrum));
            for (size_t i = 0; i < wave.size(); ++i)
                tablesMatch = tablesMatch && std::abs (wave[i] - set.read (n, (float) i / (float) wave.size())) < 1.0e-4f;
        }
        check (tablesMatch, "the first n harmonics match table n of a WavetableSet (n = 1, 5, 32)");
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
