// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include "dsp/Lfo.h"

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

    How it behaves:
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
    static constexpr int maxEvents = 256;          // controller messages kept per block; more are merged into the last
    static constexpr double vibratoRateHz = 5.5;   // the mod wheel's vibrato
    static constexpr double bendGlideSeconds = 0.005;    // 5 ms: a bend message glides instead of stepping the pitch
    static constexpr double depthGlideSeconds = 0.02;    // 20 ms: the wheel and the depths don't zipper
    static constexpr float  maxVibratoCents = 100.0f;    // the vibrato depth's range ends at a semitone
    static constexpr int    maxBendRangeSemitones = 24;  // the bend range's top: two octaves

    // MIDI controller numbers handleMidi() reads.
    static constexpr int modWheelController = 1;               // CC 1, coarse (MSB)
    static constexpr int modWheelFineController = 33;          // CC 33, fine (LSB): CC 1 + 32
    static constexpr int resetAllControllers = 121;            // CC 121

    void prepare (double newSampleRate, int samplesPerBlock)
    {
        sampleRate = newSampleRate;
        lfo.prepare (newSampleRate);
        const auto size = (size_t) juce::jmax (samplesPerBlock, minBufferSize);
        lfoBuffer.assign (size, 0.0f);
        pitchBuffer.assign (size, 1.0f);
        cutoffBuffer.assign (size, 0.0f);
        ampBuffer.assign (size, 1.0f);

        bend.reset (newSampleRate, bendGlideSeconds);
        for (auto* s : { &wheel, &vibratoDepth, &pitchDepth, &cutoffDepth, &ampDepth })
            s->reset (newSampleRate, depthGlideSeconds);
        reset();
    }

    /** Phase 0, bend and wheel centred: a render from here repeats exactly. */
    void reset() noexcept
    {
        lfo.reset();
        vibratoPhase = 0.0;
        numEvents = 0;
        midiBend = 0.0f;
        midiWheel = 0.0f;
        wheelMsb = 0;
        bend.setCurrentAndTargetValue (0.0f);
        wheel.setCurrentAndTargetValue (0.0f);
        snapDepths();                  // to the settings already given...
        snapNextSettings = true;       // ...or to the next ones, if they come first
        ampSamples = 0;
        lastLfoValue = 0.0f;
        bendPosition.store (0.0f, std::memory_order_relaxed);
        wheelPosition.store (0.0f, std::memory_order_relaxed);
    }

    /** Audio thread, once per block. */
    void setSettings (const Settings& s) noexcept
    {
        settings = s;
        lfo.setSettings (s.lfo);
        vibratoDepth.setTargetValue (juce::jlimit (0.0f, maxVibratoCents, s.vibratoCents) * 0.01f);   // semitones at the wheel's top
        pitchDepth  .setTargetValue (s.lfoPitchSemitones);
        cutoffDepth .setTargetValue (s.lfoCutoffOctaves);
        ampDepth    .setTargetValue (juce::jlimit (0.0f, 1.0f, s.lfoAmp));

        // After prepare() or reset() the depths start at their settings: a session that loads
        // with an LFO depth set doesn't fade it in, and a render from reset() repeats exactly.
        if (snapNextSettings)
        {
            snapNextSettings = false;
            snapDepths();
        }
    }

    /** Audio thread: pitch bend, mod wheel and reset messages from this block's incoming MIDI.
        Any channel: the keyboard and the sequencer play on different channels, and the bend
        must reach both. Each message is kept with its sample position; render() applies it there. */
    void handleMidi (const juce::MidiBuffer& midi, int numSamples) noexcept
    {
        numEvents = 0;
        for (const auto event : midi)
        {
            if (event.numBytes != 3)
                continue;   // bend and controllers are three bytes; SysEx and the rest don't concern us

            const int status = event.data[0] & 0xf0;
            const int data1  = event.data[1] & 0x7f, data2 = event.data[2] & 0x7f;

            if (status == 0xe0)   // pitch bend: 14 bits, 8192 at the centre
            {
                const int offset = (data1 | (data2 << 7)) - 8192;
                midiBend = offset >= 0 ? (float) offset / 8191.0f : (float) offset / 8192.0f;
            }
            else if (status == 0xb0 && data1 == modWheelController)   // mod wheel, coarse: on its own it is a 7-bit value
            {
                wheelMsb = data2;
                midiWheel = (float) data2 / 127.0f;
            }
            else if (status == 0xb0 && data1 == modWheelFineController)  // mod wheel, fine: with the last coarse value, 14 bits
            {
                midiWheel = (float) ((wheelMsb << 7) | data2) / 16383.0f;
            }
            else if (status == 0xb0 && data1 == resetAllControllers) // reset all controllers
            {
                midiBend = 0.0f;
                midiWheel = 0.0f;
                wheelMsb = 0;
            }
            else
            {
                continue;
            }

            // Each event holds the whole controller state after its message. When the list is
            // full the last entry takes the newer state, so the block still ends on the latest values.
            const Event e { juce::jlimit (0, juce::jmax (0, numSamples - 1), event.samplePosition), midiBend, midiWheel };
            if (numEvents < maxEvents)
                events[(size_t) numEvents++] = e;
            else
                events[(size_t) maxEvents - 1] = { events[(size_t) maxEvents - 1].position, e.bend, e.wheel };
        }
    }

    /** Audio thread: builds this block's buffers (see the contract above). */
    ModulationBlock render (int numSamples, const Lfo::Transport& transport) noexcept
    {
        ModulationBlock block;
        const int capacity = (int) lfoBuffer.size();
        const int n = juce::jlimit (0, capacity, numSamples);
        block.numSamples = n;
        ampSamples = n;

        if (testLfo != nullptr)
            std::copy (testLfo, testLfo + n, lfoBuffer.data());
        else
            lfo.render (lfoBuffer.data(), n, transport);

        // Nothing contributes to a buffer when its sources are at rest at their neutral value
        // for the whole block (smoothers only move on their own or at an event, never otherwise).
        const bool vibratoIdle = (wheel.getCurrentValue() == 0.0f && ! wheel.isSmoothing())
                              || (vibratoDepth.getCurrentValue() == 0.0f && ! vibratoDepth.isSmoothing());
        const bool pitchIdle = numEvents == 0
                            && bend.getCurrentValue() == 0.0f && ! bend.isSmoothing()
                            && vibratoIdle
                            && pitchDepth.getCurrentValue() == 0.0f && ! pitchDepth.isSmoothing();
        const bool cutoffIdle = cutoffDepth.getCurrentValue() == 0.0f && ! cutoffDepth.isSmoothing();
        ampActive = ampDepth.getCurrentValue() != 0.0f || ampDepth.isSmoothing();

        const double vibratoIncrement = vibratoRateHz / sampleRate;
        bool pitchNeutral = true, cutoffNeutral = true;
        const float bendRange = (float) juce::jlimit (0, maxBendRangeSemitones, settings.bendRangeSemitones);
        int nextEvent = 0;

        for (int i = 0; i < n; ++i)
        {
            const float lfoNow = lfoBuffer[(size_t) i];

            if (! pitchIdle)
            {
                // Bend and wheel change from their message's sample on.
                for (; nextEvent < numEvents && events[(size_t) nextEvent].position <= i; ++nextEvent)
                {
                    bend.setTargetValue (events[(size_t) nextEvent].bend);
                    wheel.setTargetValue (events[(size_t) nextEvent].wheel);
                }

                const float vibrato = wheel.getNextValue() * vibratoDepth.getNextValue()
                                    * (float) std::sin (juce::MathConstants<double>::twoPi * vibratoPhase);
                const float semitones = bend.getNextValue() * bendRange + vibrato + lfoNow * pitchDepth.getNextValue();
                const float ratio = std::exp2 (semitones / 12.0f);
                pitchBuffer[(size_t) i] = ratio;
                pitchNeutral = pitchNeutral && ratio == 1.0f;
            }

            vibratoPhase += vibratoIncrement;   // free-running, whether the wheel is up or not
            if (vibratoPhase >= 1.0)
                vibratoPhase -= 1.0;

            if (! cutoffIdle)
            {
                const float octaves = lfoNow * cutoffDepth.getNextValue();
                cutoffBuffer[(size_t) i] = octaves;
                cutoffNeutral = cutoffNeutral && octaves == 0.0f;
            }

            // The tremolo only dips: at the LFO's top the gain is 1, at its bottom 1 - AMP.
            if (ampActive)
                ampBuffer[(size_t) i] = 1.0f - ampDepth.getNextValue() * (1.0f - lfoNow) * 0.5f;
        }

        if (pitchIdle)
        {
            // Idle, but a wheel or vibrato depth can still be gliding to where it makes no sound.
            wheel.skip (n);
            vibratoDepth.skip (n);
        }

        lastLfoValue = n > 0 ? lfoBuffer[(size_t) n - 1] : lastLfoValue;
        if (numSamples > n)
            skipBeyondCapacity (numSamples - n, nextEvent, transport);

        block.pitchRatio    = pitchIdle  || pitchNeutral  ? nullptr : pitchBuffer.data();
        block.cutoffOctaves = cutoffIdle || cutoffNeutral ? nullptr : cutoffBuffer.data();
        numEvents = 0;

        lfoPhase.store ((float) lfo.getPhase(), std::memory_order_relaxed);
        lfoValue.store (lastLfoValue, std::memory_order_relaxed);
        bendPosition.store (bend.getCurrentValue(), std::memory_order_relaxed);
        wheelPosition.store (wheel.getCurrentValue(), std::memory_order_relaxed);
        return block;
    }

    /** Audio thread: applies the tremolo to the summed voices. */
    void applyAmplitude (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
    {
        if (! ampActive || ampSamples <= 0)
            return;

        const int n = juce::jmin (numSamples, buffer.getNumSamples());
        const float last = ampBuffer[(size_t) ampSamples - 1];   // held beyond the buffers' capacity
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* samples = buffer.getWritePointer (ch);
            const int covered = juce::jmin (n, ampSamples);
            juce::FloatVectorOperations::multiply (samples, ampBuffer.data(), covered);
            if (n > covered)
                juce::FloatVectorOperations::multiply (samples + covered, last, n - covered);
        }
    }

    /** For tests only: render() takes the LFO's values from here (at least a block's worth)
        instead of from the LFO. nullptr (the default) goes back to the LFO. */
    void setLfoValuesForTesting (const float* values) noexcept   { testLfo = values; }

    // For the MODULATION panel (message thread reads, audio thread writes).
    float getLfoPhase() const noexcept       { return lfoPhase.load (std::memory_order_relaxed); }   ///< 0..1
    float getLfoValue() const noexcept       { return lfoValue.load (std::memory_order_relaxed); }   ///< -1..1, last sample of the last block
    float getBendPosition() const noexcept   { return bendPosition.load (std::memory_order_relaxed); }  ///< -1..1
    float getWheelPosition() const noexcept  { return wheelPosition.load (std::memory_order_relaxed); } ///< 0..1

private:
    /** A bend, wheel or reset message: the controller state from this sample on. */
    struct Event
    {
        int   position = 0;
        float bend = 0.0f, wheel = 0.0f;
    };

    void snapDepths() noexcept
    {
        for (auto* s : { &vibratoDepth, &pitchDepth, &cutoffDepth, &ampDepth })
            s->setCurrentAndTargetValue (s->getTargetValue());
    }

    /** A block longer than the buffers: the samples beyond them hold the last values (see the
        contract), but time still passes, so the LFO, the vibrato and the glides move on. */
    void skipBeyondCapacity (int extra, int nextEvent, const Lfo::Transport& transport) noexcept
    {
        for (; nextEvent < numEvents; ++nextEvent)
        {
            bend.setTargetValue (events[(size_t) nextEvent].bend);
            wheel.setTargetValue (events[(size_t) nextEvent].wheel);
        }

        for (auto* s : { &bend, &wheel, &vibratoDepth, &pitchDepth, &cutoffDepth, &ampDepth })
            s->skip (extra);

        vibratoPhase += vibratoRateHz / sampleRate * extra;
        vibratoPhase -= std::floor (vibratoPhase);

        // The LFO renders the rest into its own buffer (its first part has been used already).
        if (testLfo != nullptr)
            return;   // the test's values stop at the capacity: the last one holds
        const int capacity = (int) lfoBuffer.size();
        for (int left = extra; left > 0; left -= capacity)
        {
            const int chunk = juce::jmin (left, capacity);
            lfo.render (lfoBuffer.data(), chunk, transport);
            lastLfoValue = lfoBuffer[(size_t) chunk - 1];
        }
    }

    double   sampleRate = 48000.0;
    Settings settings;
    Lfo      lfo;
    std::vector<float> lfoBuffer, pitchBuffer, cutoffBuffer, ampBuffer;
    const float* testLfo = nullptr;
    float lastLfoValue = 0.0f;

    // MIDI controllers: the state as parsed so far (it persists across blocks), and this block's events.
    float midiBend = 0.0f, midiWheel = 0.0f;
    int   wheelMsb = 0;
    std::array<Event, (size_t) maxEvents> events {};
    int   numEvents = 0;

    juce::SmoothedValue<float> bend { 0.0f }, wheel { 0.0f };   // -1..1, 0..1
    juce::SmoothedValue<float> vibratoDepth { 0.3f };           // semitones at the wheel's top
    juce::SmoothedValue<float> pitchDepth { 0.0f }, cutoffDepth { 0.0f }, ampDepth { 0.0f };
    double vibratoPhase = 0.0;                                   // 0..1, free-running at vibratoRateHz
    bool   snapNextSettings = true, ampActive = false;
    int    ampSamples = 0;                                       // valid length of ampBuffer this block

    std::atomic<float> lfoPhase { 0.0f }, lfoValue { 0.0f }, bendPosition { 0.0f }, wheelPosition { 0.0f };
};
