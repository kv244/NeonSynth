// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "Lfo.h"

/**
    What the voices read from the modulation each block (v1.9.0). Both buffers
    are indexed by the sample's position in the processor's block (the same
    index as the startSample the synth passes to renderNextBlock()).

    A null pointer means "nothing modulates this block": the voice must then do
    exactly what v1.8.0 did, with no extra arithmetic, so default sessions stay
    bit-identical.
*/
struct ModulationBlock
{
    const float* pitchRatio    = nullptr;   // frequency multiplier per sample (bend, vibrato, LFO pitch); null = 1
    const float* cutoffOctaves = nullptr;   // octaves added to the cutoff per sample (LFO CUTOFF); null = 0
    int numSamples = 0;                     // valid length of both buffers
};

/**
    All the synth's modulation (v1.9.0), shared by every voice: the LFO, MIDI
    pitch bend, and the mod wheel's vibrato. The processor owns one.

    CONTRACT (v1.9.0 shared contract; agent C implements the body, agent A the Lfo):
      - handleMidi() reads pitch bend (any channel), CC 1 + CC 33 (mod wheel, 7 or
        14 bit) and CC 121 (reset all controllers) with their sample positions,
        from the block's incoming MIDI, before render().
      - render() builds, per sample: pitchRatio = 2^((bend + vibrato + lfo pitch) / 12)
        and cutoffOctaves = LFO x CUTOFF depth, and the tremolo gain
        1 - AMP x (1 - lfo) / 2 for applyAmplitude(). Bend glides over 5 ms, the
        wheel and the three depths over 20 ms. A buffer whose value is the
        neutral one for the whole block is returned as nullptr.
      - applyAmplitude() multiplies the summed voices by the tremolo gain; it does
        nothing when AMP is 0 and no tremolo is fading out.
      - Buffers are allocated in prepare() for max (samplesPerBlock, 8192) samples;
        a longer block holds the last value for the samples beyond that.
      - Everything on the audio thread is real-time safe.
*/
class Modulation
{
public:
    struct Settings
    {
        Lfo::Settings lfo;
        float lfoPitchSemitones = 0.0f;   // -12 .. +12
        float lfoCutoffOctaves  = 0.0f;   // -5 .. +5
        float lfoAmp            = 0.0f;   // 0 .. 1
        int   bendRangeSemitones = 2;     // 0 .. 24
        float vibratoCents      = 30.0f;  // 0 .. 100, at the mod wheel's top
    };

    static constexpr int minBufferSize = 8192;

    void prepare (double newSampleRate, int samplesPerBlock)
    {
        sampleRate = newSampleRate;
        lfo.prepare (newSampleRate);
        const auto size = (size_t) juce::jmax (samplesPerBlock, minBufferSize);
        lfoBuffer.assign (size, 0.0f);
        pitchBuffer.assign (size, 1.0f);
        cutoffBuffer.assign (size, 0.0f);
        ampBuffer.assign (size, 1.0f);
    }

    /** Phase 0, bend and wheel centred: a render from here repeats exactly. */
    void reset() noexcept   { lfo.reset(); }

    /** Audio thread, once per block. */
    void setSettings (const Settings& s) noexcept   { settings = s; lfo.setSettings (s.lfo); }

    /** Audio thread: pitch bend, mod wheel and reset messages from this block's incoming MIDI. */
    void handleMidi (const juce::MidiBuffer&, int /*numSamples*/) noexcept {}

    /** Audio thread: builds this block's buffers. Stub: nothing modulates yet. */
    ModulationBlock render (int numSamples, const Lfo::Transport&) noexcept
    {
        ModulationBlock block;
        block.numSamples = numSamples;
        return block;
    }

    /** Audio thread: applies the tremolo to the summed voices. Stub: does nothing. */
    void applyAmplitude (juce::AudioBuffer<float>&, int /*numSamples*/) noexcept {}

    // For the MODULATION panel (message thread reads, audio thread writes).
    float getLfoPhase() const noexcept       { return lfoPhase.load (std::memory_order_relaxed); }   ///< 0..1
    float getLfoValue() const noexcept       { return lfoValue.load (std::memory_order_relaxed); }   ///< -1..1, last sample of the last block
    float getBendPosition() const noexcept   { return bendPosition.load (std::memory_order_relaxed); }  ///< -1..1
    float getWheelPosition() const noexcept  { return wheelPosition.load (std::memory_order_relaxed); } ///< 0..1

private:
    double   sampleRate = 48000.0;
    Settings settings;
    Lfo      lfo;
    std::vector<float> lfoBuffer, pitchBuffer, cutoffBuffer, ampBuffer;

    std::atomic<float> lfoPhase { 0.0f }, lfoValue { 0.0f }, bendPosition { 0.0f }, wheelPosition { 0.0f };
};
