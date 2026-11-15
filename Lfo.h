// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>

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

    void prepare (double newSampleRate) noexcept   { sampleRate = newSampleRate; reset(); }

    /** Phase 0, Random reseeded: a render from here repeats exactly. */
    void reset() noexcept   { phase = 0.0; }

    /** Audio thread, once per block before render(). */
    void setSettings (const Settings& s) noexcept   { settings = s; }

    /** Fills out[0..numSamples) with values from -1 to +1. Audio thread. */
    void render (float* out, int numSamples, const Transport&) noexcept
    {
        // Stub (shared contract): agent A replaces this with the real LFO.
        std::fill (out, out + numSamples, 0.0f);
    }

    /** Current phase 0..1, for the LFO view. Audio thread writes it; read it via Modulation's atomic. */
    double getPhase() const noexcept   { return phase; }

private:
    double   sampleRate = 48000.0;
    Settings settings;
    double   phase = 0.0;
};
