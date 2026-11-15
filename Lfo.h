// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <juce_core/juce_core.h>
#include <cmath>
#include <cstdint>
#include <algorithm>

/**
    The LFO (v1.9.0): one for the whole synth, free-running, rendered once per
    block into a buffer of values from -1 to +1. Modulation.h scales it by the
    PITCH, CUTOFF and AMP depths and hands the results to the voices.

    CONTRACT (v1.9.0 shared contract; agent A implements the body):
      - Shapes: Sine, Triangle, Square, Saw (rising) and Random (sample and hold,
        a new value each cycle). Square and Random are slewed over 2 ms so their
        jumps don't click; a shape change crossfades over 10 ms.
      - Rate 0.02 to 20 Hz, or with sync a note length (Division). Synced to a
        playing host, the phase comes from the song position: frac (ppq / beats),
        so it lands on the beat after a jump or a loop. Otherwise (host stopped,
        no host, the Standalone) it free-runs at the given tempo.
      - reset() puts the phase back to 0 and reseeds Random, so a render repeats.
      - render() is real-time safe: no allocation, no locks.

    How the clicks are kept out:
      - Slew: every shape goes through a rate limiter that lets the output move
        at most 2 per 2 ms (a full -1 to +1 jump takes exactly 2 ms; a smaller
        jump proportionally less). A limiter rather than a one-pole because it
        leaves anything slower than that untouched, bit for bit: the fastest
        Sine (20 Hz) moves at most 2 pi x 20 = 126 per second, the Triangle 80
        and the Saw's ramp 40, all far below the limit of 1000 per second. So in
        practice it only acts on the Square's edges, Random's steps and the
        Saw's wrap, as agreed; and on the jump when a synced host loops or
        relocates, which would otherwise click on every shape.
      - Shape change: the old shape keeps running and is crossfaded (linearly)
        into the new one over 10 ms. Another change during a fade starts a new
        fade from wherever the output is, so turning the knob through several
        shapes quickly still doesn't step.
*/
class Lfo
{
public:
    enum class Shape { sine = 0, triangle = 1, square = 2, saw = 3, random = 4 };   // the LFO Shape parameter's order

    /** The LFO Division parameter's order. */
    enum class Division { bars4 = 0, bars2, bar1, half, quarter, eighth, sixteenth, eighthTriplet, sixteenthTriplet };

    /** Length of a division in beats (quarter notes). */
    static constexpr double divisionBeats (Division d) noexcept
    {
        constexpr double beats[] = { 16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25, 1.0 / 3.0, 1.0 / 6.0 };
        return beats[(int) d];
    }

    struct Settings
    {
        Shape    shape    = Shape::sine;
        float    rateHz   = 1.0f;              // 0.02 .. 20, used when sync is off
        bool     sync     = false;
        Division division = Division::quarter;
    };

    /** Musical time for this block (the same transport the sequencer follows). */
    struct Transport
    {
        bool   hostPlaying = false;   // a host transport is running: take the phase from ppq
        double ppq = 0.0;             // song position at the block's first sample, in beats
        double bpm = 120.0;           // tempo (host or internal)
    };

    static constexpr float  minRateHz     = 0.02f;
    static constexpr float  maxRateHz     = 20.0f;
    static constexpr double slewSeconds   = 0.002;   // a jump of 2 (-1 to +1) takes this long
    static constexpr double crossfadeSeconds = 0.010;

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate;
        maxStep = (float) (2.0 / (slewSeconds * sampleRate));
        crossfadeLength = juce::jmax (1, (int) std::lround (crossfadeSeconds * sampleRate));
        reset();
    }

    /** Phase 0, Random reseeded, slew and crossfade cleared: a render from here repeats exactly
        (the next setSettings() shape is taken as is, with no crossfade). */
    void reset() noexcept
    {
        phase = 0.0;
        cycle = 0;
        random.setSeed (randomSeed);
        held = nextRandom();
        current  = { settings.shape, 0.0f, false };
        previous = current;
        previousOffset = 0.0f;
        fadePosition = crossfadeLength;   // no fade running
        lastValue = 0.0f;
    }

    /** Audio thread, once per block before render(). A new shape starts a 10 ms crossfade. */
    void setSettings (const Settings& s) noexcept
    {
        if (s.shape != current.shape && ! current.primed)
        {
            // Nothing rendered since reset(): there is no old shape to fade from, so start on
            // the new one directly. This keeps a render from reset() independent of the shape
            // that was playing before it.
            current.shape = s.shape;
        }
        else if (s.shape != current.shape)
        {
            // The outgoing signal is whatever is playing now, which mid-fade is a mix of two
            // shapes. Keep the shape that was fading in running as the old one, and carry the
            // difference between it and the actual output as an offset that fades out with
            // it, so the output carries on from exactly where it was.
            previousOffset = lastValue - current.value;
            previous = current;
            current  = { s.shape, 0.0f, false };
            fadePosition = 0;
        }

        settings = s;
    }

    /** Fills out[0..numSamples) with values from -1 to +1. Audio thread. */
    void render (float* out, int numSamples, const Transport& transport) noexcept
    {
        const double beats = divisionBeats (settings.division);
        const double rate = settings.sync ? transport.bpm / 60.0 / beats
                                          : (double) juce::jlimit (minRateHz, maxRateHz, settings.rateHz);
        const double increment = rate / sampleRate;

        if (settings.sync && transport.hostPlaying)
            followHost (transport.ppq / beats);

        for (int i = 0; i < numSamples; ++i)
        {
            float value = slewed (current, phase);

            if (fadePosition < crossfadeLength)
            {
                const float t = (float) fadePosition / (float) crossfadeLength;
                const float old = slewed (previous, phase) + previousOffset;
                value = old + t * (value - old);
                ++fadePosition;
            }

            out[i] = value;
            lastValue = value;

            phase += increment;

            if (phase >= 1.0)
            {
                // Rates top out well below the sample rate, so this wraps at most once.
                phase -= std::floor (phase);
                ++cycle;
                held = nextRandom();
            }
        }
    }

    /** Current phase 0..1, for the LFO view. Audio thread writes it; read it via Modulation's atomic. */
    double getPhase() const noexcept   { return phase; }

    /** The last value render() wrote (0 before the first render). */
    float getLastValue() const noexcept   { return lastValue; }

    /** The raw shape at phase p (0..1) with the given held Random value, before slew and crossfade. */
    static float shapeValue (Shape shape, double p, float heldRandom) noexcept
    {
        switch (shape)
        {
            case Shape::sine:      return (float) std::sin (juce::MathConstants<double>::twoPi * p);
            case Shape::triangle:  return (float) (p < 0.25 ? 4.0 * p
                                                 : p < 0.75 ? 2.0 - 4.0 * p
                                                            : 4.0 * p - 4.0);
            case Shape::square:    return p < 0.5 ? 1.0f : -1.0f;
            case Shape::saw:       return (float) (2.0 * p - 1.0);
            case Shape::random:    return heldRandom;
        }

        return 0.0f;
    }

private:
    /** One shape's output through its own slew limiter (one for the shape playing, one for the
        shape fading out, so a crossfade mixes two click-free signals). */
    struct Follower
    {
        Shape shape  = Shape::sine;
        float value  = 0.0f;
        bool  primed = false;   // false: the first sample starts at the shape's value, no slew in
    };

    float slewed (Follower& v, double p) const noexcept
    {
        const float target = shapeValue (v.shape, p, held);

        if (! v.primed)
        {
            v.value = target;
            v.primed = true;
            return target;
        }

        // Assign the target outright when it is in reach (rather than adding the clamped
        // difference) so a shape slower than the limit comes out exactly as calculated.
        const float difference = target - v.value;

        if (std::abs (difference) <= maxStep)
            v.value = target;
        else
            v.value += difference > 0.0f ? maxStep : -maxStep;

        return v.value;
    }

    /** Synced to a playing host: take the phase from the song position (in cycles). */
    void followHost (double position) noexcept
    {
        const double hostCycleFloor = std::floor (position);
        const auto hostCycle = (std::int64_t) hostCycleFloor;
        const double expected = (double) cycle + phase;

        // Random takes a new value when the song position has moved into another cycle. When the
        // host is just carrying on where we were, rounding can put its position a hair either side
        // of a wrap we did (or haven't yet done) ourselves: count only a forward crossing then, so
        // a wrap never draws twice. A real jump (a loop, a relocate) draws if the cycle changed.
        const bool continuing = std::abs (position - expected) < 0.01;

        if (continuing ? hostCycle > cycle : hostCycle != cycle)
            held = nextRandom();

        cycle = hostCycle;
        phase = juce::jlimit (0.0, std::nextafter (1.0, 0.0), position - hostCycleFloor);
    }

    float nextRandom() noexcept   { return 2.0f * random.nextFloat() - 1.0f; }

    static constexpr juce::int64 randomSeed = 0x4e656f6e4c666fLL;   // fixed, so Random repeats after reset()

    double   sampleRate = 48000.0;
    float    maxStep = (float) (2.0 / (slewSeconds * 48000.0));
    int      crossfadeLength = 480;
    Settings settings;
    double   phase = 0.0;
    std::int64_t cycle = 0;      // whole cycles so far (song position in cycles when following a host)
    juce::Random random { randomSeed };
    float    held = 0.0f;        // Random's value for this cycle
    Follower current, previous;
    float    previousOffset = 0.0f;
    int      fadePosition = 480;
    float    lastValue = 0.0f;
};
