// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "plugin/MidiRouter.h"
#include <cstring>

void MidiRouter::prepare()
{
    playMidi.ensureSize (8192);   // bytes; far more than a block of keyboard + sequencer events needs
    restartInternalClock();
    numHeldKeys    = 0;
}

void MidiRouter::trackHeldKey (int note, bool isDown) noexcept
{
    // Remove the key if present, then (for a key-down) append it as the most recent.
    for (int i = 0; i < numHeldKeys; ++i)
    {
        if (heldKeys[(size_t) i] == note)
        {
            for (int j = i + 1; j < numHeldKeys; ++j)
                heldKeys[(size_t) (j - 1)] = heldKeys[(size_t) j];
            --numHeldKeys;
            break;
        }
    }

    if (isDown && numHeldKeys < (int) heldKeys.size())
        heldKeys[(size_t) numHeldKeys++] = note;
}

void MidiRouter::releaseHeldNotes (SequencerMode mode) noexcept
{
    // v1.11.0 (region B): HOLD switched off. Each kept-back note-off goes to the synth (which
    // ignores one for a note no longer sounding) and, in Transpose mode, lets go of the key.
    for (int note = 0; note < (int) notesHeldByHold.size(); ++note)
    {
        if (! notesHeldByHold[(size_t) note])
            continue;
        notesHeldByHold[(size_t) note] = false;
        synth.forgetKey (note);   // its key is up: the note-off below must not step a LEGATO voice back to it
        playMidi.addEvent (juce::MidiMessage::noteOff (keyboardChannel, note), 0);
        if (mode == SequencerMode::transpose)
            trackHeldKey (note, false);
    }
}

void MidiRouter::pushEnteredNote (int note, int velocity) noexcept
{
    const auto scope = enteredNotesFifo.write (1);
    if (scope.blockSize1 > 0)
        enteredNotes[(size_t) scope.startIndex1] = (juce::uint16) ((velocity << 8) | note);
}

bool MidiRouter::popEnteredNote (int& note, int& velocity) noexcept
{
    const auto scope = enteredNotesFifo.read (1);
    if (scope.blockSize1 == 0)
        return false;
    const auto packed = enteredNotes[(size_t) scope.startIndex1];
    note = packed & 0x7f;
    velocity = (packed >> 8) & 0x7f;
    return true;
}

const juce::MidiBuffer& MidiRouter::buildPlayMidi (const juce::MidiBuffer& input, int numSamples,
                                                   const Controls& controls, juce::AudioPlayHead* playHead) noexcept
{
    playMidi.clear();

    const bool sequencerOn = controls.sequencerOn;
    const auto mode = sequencerOn ? controls.sequencerMode : SequencerMode::layer;

    if (mode != SequencerMode::transpose)
        numHeldKeys = 0;   // forget keys held under another mode

    // v1.11.0 (region B), HOLD: like a sustain pedal for the keyboard. While it is on, keyboard
    // note-offs are kept back (below); switching it off sends them all, at the block's first
    // sample. The sequencer's notes are made after this and never pass through it. Off (the
    // default), nothing here runs and the keyboard path is exactly v1.10.1's.
    const bool holding = controls.hold;
    if (wasHolding && ! holding)
        releaseHeldNotes (mode);
    wasHolding = holding;

    // 1. Keyboard input. Layer passes everything through; Replace and Transpose
    //    keep controllers, pitch bend and so on, but take note-ons out of the
    //    synth's path (Transpose uses them to steer the pattern instead).
    //
    //    Note-offs reach the synth in every mode: a key pressed in Layer and
    //    released after switching to Replace or Transpose (or after turning the
    //    sequencer on) must still stop its voice. juce::Synthesiser ignores a
    //    note-off for a note that isn't sounding, so this is harmless.
    //    SysEx is dropped: the synth ignores it, and a large dump could
    //    outgrow playMidi's preallocated capacity on the audio thread.
    //
    //    Keyboard messages are moved to MIDI channel 1, whatever channel they
    //    arrived on. The sequencer owns channel 16; juce::Synthesiser matches
    //    note-offs by channel and note, so a keyboard on channel 16 would
    //    otherwise have its notes cut off by the sequencer playing the same
    //    pitch. The synth has no per-channel behaviour of its own, so nothing
    //    else changes (a sustain pedal still holds the keyboard's notes).
    for (const auto event : input)
    {
        if (event.numBytes <= 0 || event.data[0] == 0xf0)
            continue;   // empty or SysEx

        juce::uint8 bytes[3] = {};
        const int numBytes = juce::jmin (event.numBytes, 3);
        std::memcpy (bytes, event.data, (size_t) numBytes);

        if (event.numBytes <= 3 && bytes[0] >= 0x80 && bytes[0] < 0xf0)
            bytes[0] = (juce::uint8) ((bytes[0] & 0xf0) | (keyboardChannel - 1));

        const int  status  = numBytes >= 3 ? (bytes[0] & 0xf0) : 0;
        const bool noteOn  = status == 0x90 && bytes[2] > 0;
        const bool noteOff = status == 0x80 || (status == 0x90 && bytes[2] == 0);

        // v1.11.0 (region B), HOLD: the key stays down for the synth, and for Transpose mode too
        // (the pattern goes on playing, latched), until HOLD is switched off.
        if (noteOff && holding)
        {
            notesHeldByHold[(size_t) (bytes[1] & 0x7f)] = true;
            continue;
        }

        if ((noteOn || noteOff) && mode == SequencerMode::transpose)
            trackHeldKey (bytes[1], noteOn);

        if (noteOn && noteEntryArmed.load (std::memory_order_relaxed))
            pushEnteredNote (bytes[1], bytes[2]);

        if (! noteOn || mode == SequencerMode::layer)
        {
            if (event.numBytes <= 3)
                playMidi.addEvent (bytes, numBytes, event.samplePosition);
            else
                playMidi.addEvent (event.data, event.numBytes, event.samplePosition);   // longer system messages, unchanged
        }
    }

    // 2. Musical time: the host transport when Sync is on and the host reports
    //    one (a DAW), otherwise the internal clock at the Tempo parameter
    //    (the Standalone, or Sync off). The internal clock restarts at beat 0
    //    each time the sequencer is switched on.
    if (sequencerOn && ! wasSequencerOn)
        restartInternalClock();
    wasSequencerOn = sequencerOn;

    StepSequencer::Transport transport;
    bool followsHost = false;

    if (controls.sync)
    {
        if (auto* hostPlayHead = playHead)
        {
            if (const auto position = hostPlayHead->getPosition(); position.hasValue())
            {
                const auto ppq = position->getPpqPosition();
                const auto bpm = position->getBpm();

                if (ppq.hasValue() && bpm.hasValue())
                {
                    transport   = { position->getIsPlaying(), *ppq, *bpm };
                    followsHost = true;
                }
            }
        }
    }

    if (! followsHost)
    {
        // The beat is worked out from the samples counted since the last tempo change, not
        // summed block by block: a running sum gathers rounding error with every block, which
        // at one-sample blocks and 192 kHz put a step one sample late (SweepTests). v1.11.0.
        const double tempo = controls.tempo;
        if (! juce::exactlyEqual (tempo, internalTempo))
        {
            internalBeatOrigin = internalBeatAt (internalSamples, controls.sampleRate);
            internalSamples    = 0;
            internalTempo      = tempo;
        }
        transport = { true, internalBeatAt (internalSamples, controls.sampleRate), tempo };
        internalSamples += numSamples;
    }

    transport.playing = transport.playing && sequencerOn;   // switching off releases sounding notes
    sequencerTempo.store ((float) transport.bpm, std::memory_order_relaxed);
    sequencerFollowsHost.store (followsHost, std::memory_order_relaxed);

    // 3. The sequencer's own events.
    StepSequencer::Settings settings;
    settings.rate  = controls.rate;
    settings.swing = controls.swing;
    settings.gate  = controls.gate;
    settings.length = controls.length;   // v1.11.0 (region A); 16, the default, plays as before

    if (mode == SequencerMode::transpose)
    {
        // Steered by the most recently pressed key that is still held; silent with no key down.
        settings.triggersEnabled = numHeldKeys > 0;
        settings.transpose = numHeldKeys > 0 ? heldKeys[(size_t) (numHeldKeys - 1)] - transposeReferenceNote : 0;
    }

    sequencer.process (playMidi, numSamples, transport, settings);
    return playMidi;
}
