// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include "dsp/Blep4.h"
#include <juce_audio_basics/juce_audio_basics.h>

/** CHOP's settings, shared by every voice; read once per block. */
struct ChopSettings
{
    juce::uint16 pattern = 0xAAAA;   ///< bit k set = step k (0..15) chopped; default every second step
    int   comb     = 1;              ///< the pattern repeats this many times per cycle (1..32)
    float overtone = 0.0f;           ///< 0..2: a chopped step is scaled by 1 - overtone (0 = no chop)
};

/**
    CHOP, after the Roland S-1's OSC CHOP: cuts each cycle of a wave into 16 x Comb
    equal steps and scales the steps the pattern marks by 1 - Overtone (unchanged at
    0%, silent at 100%, phase-inverted at 200%).

    Where the gain changes at a step edge, the wave jumps by its value there times
    the change in gain; the same 4-point residual as the oscillator's own edges
    smooths that jump. A step shorter than four samples can't be smoothed properly,
    so CHOP fades out as steps shrink from four samples to two (at high notes and
    high Comb), instead of the S-1's hard cut-off above 1 kHz.

    The pattern and Comb change only at a cycle boundary, so an edge never changes
    while it is being corrected; Overtone glides over 10 ms. With Overtone at 0 and
    not gliding, apply() returns its input untouched (bit-identical to no CHOP).

    Audio thread only; no allocation.
*/
class Chop
{
public:
    static constexpr int stepsPerPattern = 16;

    void prepare (double sampleRate) noexcept
    {
        gain.reset (sampleRate, 0.01);
        gain.setCurrentAndTargetValue (1.0f - pendingOvertone);
    }

    void set (const ChopSettings& s) noexcept
    {
        pendingPattern  = s.pattern;
        pendingComb     = juce::jlimit (1, 32, s.comb);
        pendingOvertone = juce::jlimit (0.0f, 2.0f, s.overtone);
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
        const float depth = juce::jlimit (0.0f, 1.0f, (1.0f / juce::jmax (segDt, 1.0e-6f) - 2.0f) * 0.5f);
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
    juce::uint16 pattern = 0xAAAA, pendingPattern = 0xAAAA;
    int   comb = 1, pendingComb = 1;
    float pendingOvertone = 0.0f;
};
