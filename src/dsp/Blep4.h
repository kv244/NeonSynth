// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once

/**
    4-point band-limiting residuals from the cubic B-spline.

    A jump of height h at the start of a cycle is corrected by adding
    h * step (t, dt) to the naive wave; a corner where the slope (per sample)
    changes by k is corrected by adding k * corner (t, dt). t is the phase
    (0..1) and dt the phase step per sample.

    Each residual reaches two samples either side of the edge. That is exact
    while dt <= 0.25 (an edge's reach is then shorter than a cycle, so only the
    edges at the start of this cycle and the next can be in range); above it,
    up to the 0.49 cap, some aliasing returns but the output stays bounded.
*/
struct Blep4
{
    /** Integral of the B-spline from d to 2 (d = samples from the edge, 0..2): 0.5 at the edge, 0 at 2. */
    static float tail (float d) noexcept
    {
        if (d >= 1.0f)
        {
            const float u = 2.0f - d, u2 = u * u;
            return u2 * u2 * (1.0f / 24.0f);
        }
        const float d2 = d * d;
        return 0.5f - (4.0f * d - 2.0f * d2 * d + 0.75f * d2 * d2) * (1.0f / 6.0f);
    }

    /** Integral of tail() from d to 2: the corner residual's shape, 7/30 at the corner. */
    static float tail2 (float d) noexcept
    {
        if (d >= 1.0f)
        {
            const float u = 2.0f - d, u2 = u * u;
            return u2 * u2 * u * (1.0f / 120.0f);
        }
        const float d2 = d * d, d4 = d2 * d2;
        return 1.0f / 120.0f + 0.5f * (1.0f - d) - (1.65f - 2.0f * d2 + 0.5f * d4 - 0.15f * d4 * d) * (1.0f / 6.0f);
    }

    /** Residual for a unit upward jump at phase 0 (repeating every cycle). */
    static float step (float t, float dt) noexcept
    {
        float r = 0.0f;
        if (t < 2.0f * dt)          r -= tail (t / dt);            // just after the jump
        if (t > 1.0f - 2.0f * dt)   r += tail ((1.0f - t) / dt);   // just before the next one
        return r;
    }

    /** Residual for a corner at phase 0 whose slope (per sample) rises by 1. */
    static float corner (float t, float dt) noexcept
    {
        float r = 0.0f;
        if (t < 2.0f * dt)          r += tail2 (t / dt);
        if (t > 1.0f - 2.0f * dt)   r += tail2 ((1.0f - t) / dt);
        return r;
    }

    /** Phase steps at or above this are clamped: band-limiting needs at least two samples a cycle. */
    static constexpr float maxPhaseStep = 0.49f;

    /**
        Treble compensation for the B-spline residuals.

        Spreading every edge with the cubic B-spline acts like a fixed low-pass on
        the whole wave: its harmonics are scaled by sinc^4 (f / sampleRate), about
        -2.8 dB at 10 kHz and -6.3 dB at 16 kHz at 48 kHz (twice the old 2-point
        residual's loss). This 3-tap filter, y[n] = (1 + 2a) x[n-1] - a (x[n] + x[n-2]),
        undoes most of it: harmonics stay within 0.5 dB of the ideal wave up to a
        quarter of the sample rate. It boosts the leftover aliases too, but they
        stay 4-5 dB below the 2-point residual's. The cost is three multiplies and
        one sample of latency (none for the sine, which bypasses it).
    */
    struct TrebleCompensation
    {
        static constexpr float a = 0.27f;   // matches 1 / sinc^4 at a quarter of the sample rate

        float process (float x) noexcept
        {
            const float y = (1.0f + 2.0f * a) * x1 - a * (x + x2);
            x2 = x1;
            x1 = x;
            return y;
        }

        /**
            For the sine, which has no edges to compensate: passes x straight through
            (so sine renders are unchanged from v1.5.0) and only keeps the history,
            so a switch to another shape at the cycle boundary starts from real samples.
        */
        float bypass (float x) noexcept
        {
            x2 = x1;
            x1 = x;
            return x;
        }

        float x1 = 0.0f, x2 = 0.0f;
    };
};
