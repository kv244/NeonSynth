// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include "dsp/Blep4.h"
#include <juce_audio_basics/juce_audio_basics.h>

/**
    CHOP's settings, shared by every voice; read once per block.

    Depths (v1.13.0): each of the 16 steps has a depth from 0 (open) to 15 (fully chopped),
    four bits each in one 64-bit word, so the whole pattern still travels to the audio thread
    in one lock-free atomic. A step plays at 1 - Overtone x depth / 15. Depths 0 and 15 are
    v1.12's off and on steps, computed exactly as before.
*/
struct ChopSettings
{
    using Depths = juce::uint64;

    static constexpr int maxDepth = 15;   ///< fully chopped; a step's depth is 0..maxDepth

    /** Every second step chopped (steps 1, 3 .. 15): the pattern a new session starts with (v1.6.0 plan). */
    static constexpr juce::uint16 defaultPattern = 0xAAAA;
    /** Comb's range ends here (as the parameter's does): 16 x 32 segments per cycle at most. */
    static constexpr int maxComb = 32;
    /** Overtone's top, 200%: a chopped step phase-inverted at full level. */
    static constexpr float maxOvertone = 2.0f;

    /** Step k's depth, 0..15. */
    static constexpr int depthOf (Depths d, int k) noexcept   { return (int) ((d >> (4 * (k & 15))) & 15u); }

    /** The same depths with step k set to `depth` (clamped to 0..15). */
    static constexpr Depths withDepth (Depths d, int k, int depth) noexcept
    {
        const int shift = 4 * (k & 15);
        const auto clamped = (Depths) (depth < 0 ? 0 : (depth > maxDepth ? maxDepth : depth));
        return (d & ~((Depths) 15u << shift)) | (clamped << shift);
    }

    /** An on/off pattern (v1.12 and earlier, bit k = step k chopped) as depths 15 and 0. */
    static constexpr Depths fromPattern (juce::uint16 pattern) noexcept
    {
        Depths d = 0;
        for (int k = 0; k < 16; ++k)
            if (((pattern >> k) & 1u) != 0)
                d |= (Depths) maxDepth << (4 * k);
        return d;
    }

    /** The nearest on/off pattern: a step is on at depth 8 or more (what v1.12 can play). */
    static constexpr juce::uint16 toPattern (Depths d) noexcept
    {
        juce::uint16 pattern = 0;
        for (int k = 0; k < 16; ++k)
            if (depthOf (d, k) >= 8)
                pattern = (juce::uint16) (pattern | (1u << k));
        return pattern;
    }

    /** True if every step is 0 or 15, so the on/off pattern says it all. */
    static constexpr bool isOnOff (Depths d) noexcept
    {
        for (int k = 0; k < 16; ++k)
            if (const int depth = depthOf (d, k); depth != 0 && depth != maxDepth)
                return false;
        return true;
    }

    /** defaultPattern as depths: steps 2, 4 .. 16 (bits 1, 3 .. 15) at 15 (checked below). */
    static constexpr Depths defaultDepths = 0xF0F0F0F0F0F0F0F0ull;

    Depths depths  = defaultDepths;  ///< step k's depth in bits 4k..4k+3; default every second step chopped
    int   comb     = 1;              ///< the pattern repeats this many times per cycle (1..32)
    float overtone = 0.0f;           ///< 0..2: a fully chopped step is scaled by 1 - overtone (0 = no chop)
};

static_assert (ChopSettings::defaultDepths == ChopSettings::fromPattern (ChopSettings::defaultPattern));
static_assert (ChopSettings::toPattern (ChopSettings::defaultDepths) == ChopSettings::defaultPattern);

/**
    CHOP, after the Roland S-1's OSC CHOP: cuts each cycle of a wave into 16 x Comb
    equal steps and scales the steps the pattern marks by 1 - Overtone (unchanged at
    0%, silent at 100%, phase-inverted at 200%); from v1.13.0 a step can be marked
    partly, by its depth (see ChopSettings).

    Where the gain changes at a step edge, the wave jumps by its value there times
    the change in gain; the same 4-point residual as the oscillator's own edges
    smooths that jump. A step shorter than four samples can't be smoothed properly,
    so CHOP fades out as steps shrink from four samples to two (at high notes and
    high Comb), instead of the S-1's hard cut-off above 1 kHz.

    The pattern and Comb change only at a cycle boundary, so an edge never changes
    while it is being corrected; Overtone glides over 10 ms. With Overtone at 0 and
    not gliding, apply() returns its input untouched (bit-identical to no CHOP).

    NAIVE (v1.12.0): with hard edges on (setHardEdges(), which the oscillator calls at
    its cycle boundary) the gain changes from one sample to the next with no residual
    and no fade-out as steps shrink, so CHOP's edges alias like the textbook waves.
    With them off, apply() is the band-limited CHOP above, operation for operation.

    Audio thread only; no allocation.
*/
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
        pendingDepths   = s.depths;
        pendingComb     = juce::jlimit (1, ChopSettings::maxComb, s.comb);
        pendingOvertone = juce::jlimit (0.0f, ChopSettings::maxOvertone, s.overtone);
        gain.setTargetValue (1.0f - pendingOvertone);
    }

    /** Call when a new cycle starts: the pattern and Comb take effect. */
    void startCycle() noexcept
    {
        depths  = pendingDepths;
        comb    = pendingComb;
    }

    /** NAIVE (v1.12.0): hard edges, no residuals. The caller switches it at a cycle boundary. */
    void setHardEdges (bool hard) noexcept   { hardEdges = hard; }

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

        if (hardEdges)   // NAIVE: the textbook cut, no residuals, at any step length
        {
            const int stepDepth = ChopSettings::depthOf (depths, segment % stepsPerPattern);
            if (stepDepth == ChopSettings::maxDepth)
                return chopGain * x;
            if (stepDepth == 0)
                return x;
            return (1.0f - (1.0f - chopGain) * depthScale (stepDepth)) * x;   // v1.13.0: a partial depth
        }

        // Full CHOP while a segment lasts four samples or more, none below two.
        const float depth = juce::jlimit (0.0f, 1.0f, (1.0f / juce::jmax (segDt, minSegmentStep) - 2.0f) * 0.5f);
        if (depth <= 0.0f)
            return x;
        const float chopped = 1.0f - depth * (1.0f - chopGain);

        // Depth 15 and 0 are v1.12's on and off steps, computed as before; between them
        // (v1.13.0) the step's share of the cut scales the same arithmetic.
        const auto gainOf = [&] (int k) noexcept
        {
            const int stepDepth = ChopSettings::depthOf (depths, (k + segments) % stepsPerPattern);
            if (stepDepth == ChopSettings::maxDepth)
                return chopped;
            if (stepDepth == 0)
                return 1.0f;
            return 1.0f - depth * ((1.0f - chopGain) * depthScale (stepDepth));
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

    /** A depth of 0..15 as a share of the full cut, 0..1. */
    static constexpr float depthScale (int depth) noexcept   { return (float) depth / (float) ChopSettings::maxDepth; }

private:
    juce::SmoothedValue<float> gain { 1.0f };   // 1 - Overtone
    ChopSettings::Depths depths = ChopSettings::defaultDepths, pendingDepths = ChopSettings::defaultDepths;
    int   comb = 1, pendingComb = 1;
    float pendingOvertone = 0.0f;
    bool  hardEdges = false;   // NAIVE (v1.12.0)
};
