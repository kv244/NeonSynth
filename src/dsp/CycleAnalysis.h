// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include "dsp/Wavetable.h"
#include <algorithm>
#include <array>
#include <cmath>

/**
    @file CycleAnalysis.h

    DRAW's arithmetic (v1.12.0): a cycle drawn in the SHAPE strip becomes CUSTOM's 32 harmonics,
    and a spectrum becomes the cycle that plays.

    - analyse() takes one period drawn as 256 points (values -1..1, point i at phase i / 256)
      and finds, by a discrete Fourier transform, the amplitude A_k and phase phi_k of each
      harmonic k = 1..32 such that

          x (t) ~ sum over k of  A_k * sin (2 pi k t + phi_k)

      The constant part (DC) is dropped: an offset is not a harmonic, and CUSTOM's wave is
      always centred. Harmonics above 32 are dropped too, which is the lesson: a sharp corner
      needs every harmonic, so 32 of them round it off and ripple beside it (Gibbs).
    - Levels are A_k / HarmonicSpectrum::seriesGain, so a drawn full-height square gives the
      SQUARE preset. If a level comes out above 1 (the HARMONICS view's ceiling) every level is
      scaled down by the same factor, so the shape survives and only the loudness changes.
    - resynthesise() is the reverse: the sum of a spectrum's first n harmonics, scaled exactly
      as WavetableSet::build() scales the tables the voices play, so a view drawing it shows
      what is heard (before CHOP, Fold and the filter).

    Plain arithmetic, no JUCE GUI: the naive DFT (32 harmonics x 256 points) runs
    once when a drag ends, on the message thread.
*/
namespace CycleAnalysis
{
    /** Points in one drawn cycle; FourierState::drawingPoints must match. */
    constexpr int numPoints = 256;
    using Cycle = std::array<float, numPoints>;

    /** Amplitudes below this are silence: their phase means nothing, so it is stored as 0. */
    constexpr double silentAmplitude = 1.0e-6;

    namespace detail
    {
        constexpr double twoPi = juce::MathConstants<double>::twoPi;

        /** An angle wrapped into 0 <= p < 2 pi, also after rounding to float. */
        inline float wrapPhase (double p) noexcept
        {
            p = std::fmod (p, twoPi);
            if (p < 0.0)
                p += twoPi;
            const auto f = (float) p;
            return f >= juce::MathConstants<float>::twoPi ? 0.0f : f;
        }
    }

    /** The 32 harmonics of one drawn cycle (see the file's comment). */
    inline HarmonicSpectrum analyse (const Cycle& cycle) noexcept
    {
        HarmonicSpectrum s;
        float loudest = 0.0f;
        for (int k = 1; k <= HarmonicSpectrum::numHarmonics; ++k)
        {
            // X_k = sum x_i e^(-i 2 pi k i / N) = a - i b. For x = A sin (2 pi k t + phi):
            // a = (N A / 2) sin phi and b = (N A / 2) cos phi.
            double a = 0.0, b = 0.0;
            for (int i = 0; i < numPoints; ++i)
            {
                const double angle = detail::twoPi * k * i / numPoints;
                const double x = std::isfinite (cycle[(size_t) i]) ? (double) cycle[(size_t) i] : 0.0;
                a += x * std::cos (angle);
                b += x * std::sin (angle);
            }
            const double amplitude = 2.0 * std::sqrt (a * a + b * b) / numPoints;
            const auto index = (size_t) (k - 1);
            s.levels[index] = (float) (amplitude / HarmonicSpectrum::seriesGain);
            s.phases[index] = amplitude < silentAmplitude ? 0.0f : detail::wrapPhase (std::atan2 (a, b));
            loudest = std::max (loudest, s.levels[index]);
        }

        if (loudest > 1.0f)   // keep the shape, lower the loudness
            for (auto& l : s.levels)
                l /= loudest;
        return s;
    }

    /**
        The scale WavetableSet::build() gives a spectrum's tables: the series gain 4/pi, reduced
        only if the full 32-harmonic wave would peak above WavetableSet::maxPeak. Measured over
        the same tableSize points the tables have, so the two agree exactly.
    */
    inline float playingScale (const HarmonicSpectrum& spectrum) noexcept
    {
        double peak = 0.0;
        for (int i = 0; i < WavetableSet::tableSize; ++i)
        {
            double sum = 0.0;
            for (int k = 1; k <= HarmonicSpectrum::numHarmonics; ++k)
                if (const double level = spectrum.levels[(size_t) (k - 1)]; level != 0.0)
                    sum += level * std::sin (detail::twoPi * k * i / WavetableSet::tableSize + spectrum.phases[(size_t) (k - 1)]);
            peak = std::max (peak, std::abs (sum));
        }
        return (float) std::min ((double) HarmonicSpectrum::seriesGain, peak > 0.0 ? WavetableSet::maxPeak / peak : 1.0);
    }

    /**
        The wave of harmonics 1..harmonics (clamped to 1..32) at `count` evenly spaced phases
        (point i at phase i / count), scaled as the voices' tables are. Pass the scale from
        playingScale() when it is already known, to skip measuring it again.
    */
    inline void resynthesise (const HarmonicSpectrum& spectrum, int harmonics, float* out, int count, float scale) noexcept
    {
        const int n = juce::jlimit (1, HarmonicSpectrum::numHarmonics, harmonics);
        for (int i = 0; i < count; ++i)
        {
            double sum = 0.0;
            for (int k = 1; k <= n; ++k)
                if (const double level = spectrum.levels[(size_t) (k - 1)]; level != 0.0)
                    sum += level * std::sin (detail::twoPi * k * i / count + spectrum.phases[(size_t) (k - 1)]);
            out[i] = (float) (scale * sum);
        }
    }

    /** All 32 harmonics of a spectrum as one 256-point cycle: what a drawing plays as. */
    inline Cycle resynthesise (const HarmonicSpectrum& spectrum) noexcept
    {
        Cycle c {};
        resynthesise (spectrum, HarmonicSpectrum::numHarmonics, c.data(), numPoints, playingScale (spectrum));
        return c;
    }
}
