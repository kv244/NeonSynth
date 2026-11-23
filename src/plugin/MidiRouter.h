// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include "plugin/PlayingSynthesiser.h"
#include "sequencer/StepSequencer.h"

/**
    Builds the MIDI the synth plays each block (v1.11.1: moved out of NeonSynthProcessor):
    the keyboard's input, moved to its own channel and filtered by the sequencer mode, HOLD's
    kept-back note-offs, and the step sequencer's events, clocked by the host's transport or by
    the internal clock. It also keeps ENTER's queue of keyboard notes for the editor.

    The processor reads its parameters into Controls once per block and hands them over, so
    the router can be driven on its own (RouterTests) with no processor, host or APVTS.
    Everything here runs on the audio thread except setNoteEntryArmed(), popEnteredNote() and
    the two getters for display, which are lock-free.
*/
class MidiRouter
{
public:
    /** How the keyboard and the sequencer share the synth (the Sequencer Mode parameter). */
    enum class SequencerMode
    {
        layer,      ///< sequencer and keyboard both play
        replace,    ///< sequencer plays, keyboard notes are ignored
        transpose   ///< the pattern plays only while a key is held, shifted by (key - C4)
    };

    /** Keyboard input is played on this channel; the sequencer has StepSequencer::midiChannel (16). */
    static constexpr int keyboardChannel = 1;
    static_assert (keyboardChannel != StepSequencer::midiChannel);

    /** Notes are transposed relative to this key in Transpose mode (C4). */
    static constexpr int transposeReferenceNote = 60;

    /** This block's settings, read from the parameters by the processor. */
    struct Controls
    {
        bool sequencerOn = false;                            ///< Sequencer On (RUN)
        SequencerMode sequencerMode = SequencerMode::layer;  ///< Sequencer Mode (only used while the sequencer is on)
        bool hold = false;                                   ///< HOLD
        bool sync = true;                                    ///< Sequencer Sync: follow the host's transport when it has one
        float tempo = 120.0f;                                ///< Sequencer Tempo, BPM, for the internal clock
        StepSequencer::Rate rate = StepSequencer::Rate::sixteenth;
        float swing = 0.0f;
        float gate = 0.5f;
        int length = StepSequencer::numSteps;
        double sampleRate = 44100.0;                         ///< the processor's, for the internal clock
    };

    /** The router plays `sequencer` and tells `synth` about keys HOLD lets go of; both must outlive it. */
    MidiRouter (StepSequencer& sequencerToPlay, PlayingSynthesiser& synthToPlay) noexcept
        : sequencer (sequencerToPlay), synth (synthToPlay) {}

    /** Call from prepareToPlay(), after the sequencer's own prepare(). */
    void prepare();

    /**
        Builds the MIDI the synth will play this block: keyboard input filtered
        by the sequencer mode, plus the sequencer's own events. Audio thread.
        `playHead` may be null (no host transport: the internal clock plays).
    */
    const juce::MidiBuffer& buildPlayMidi (const juce::MidiBuffer& input, int numSamples,
                                           const Controls& controls, juce::AudioPlayHead* playHead) noexcept;

    /**
        v1.11.0, ENTER: while armed, every keyboard note-on is also queued for the editor, which
        writes it into the step being entered. Message thread arms and reads; the audio thread
        only pushes (lock-free, dropped when the queue is full).
    */
    void setNoteEntryArmed (bool armed) noexcept     { noteEntryArmed.store (armed, std::memory_order_relaxed); }
    bool popEnteredNote (int& note, int& velocity) noexcept;

    /** Tempo the sequencer followed in the last block (host or internal), for display. */
    float getSequencerTempo() const noexcept    { return sequencerTempo.load (std::memory_order_relaxed); }

    /** True when the last block was clocked by the host transport rather than the internal tempo. */
    bool isSequencerFollowingHost() const noexcept   { return sequencerFollowsHost.load (std::memory_order_relaxed); }

private:
    StepSequencer& sequencer;
    PlayingSynthesiser& synth;

    /** Keeps the list of held keys up to date (Transpose mode). */
    void trackHeldKey (int note, bool isDown) noexcept;

    // v1.11.0 (region B), HOLD: keyboard note-offs kept back while it is on, sent when it goes off.
    std::array<bool, 128> notesHeldByHold {};   // keyboard notes whose note-off HOLD is keeping back
    bool wasHolding = false;
    void releaseHeldNotes (SequencerMode mode) noexcept;

    juce::MidiBuffer playMidi;                 // keyboard + sequencer events for the synth; capacity reserved in prepare()
    // The internal clock (v1.11.0): the beat at the last tempo change or restart, the samples
    // since then and the tempo in force, so the position is one multiplication, not a running sum.
    double       internalBeatOrigin = 0.0;
    juce::int64  internalSamples    = 0;
    double       internalTempo      = 0.0;
    double internalBeatAt (juce::int64 samples, double sampleRate) const noexcept
    {
        return internalBeatOrigin + (double) samples * internalTempo / (60.0 * sampleRate);
    }
    void restartInternalClock() noexcept   { internalBeatOrigin = 0.0; internalSamples = 0; internalTempo = 0.0; }
    bool   wasSequencerOn = false;
    std::array<int, 16> heldKeys {};           // Transpose mode: keys currently down, most recent last
    int    numHeldKeys = 0;

    // ENTER's queue of keyboard notes (note in the low byte, velocity in the next).
    std::atomic<bool> noteEntryArmed { false };
    juce::AbstractFifo enteredNotesFifo { 64 };
    std::array<juce::uint16, 64> enteredNotes {};
    void pushEnteredNote (int note, int velocity) noexcept;
    std::atomic<float> sequencerTempo { 120.0f };
    std::atomic<bool>  sequencerFollowsHost { false };

    JUCE_DECLARE_NON_COPYABLE (MidiRouter)
};
