// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include <array>
#include "plugin/NeonSynthVoice.h"
#include "sequencer/StepSequencer.h"

/**
    v1.11.0: juce::Synthesiser with NeonSynth's two ways of moving a sounding note to a new
    pitch instead of starting a new one. All its voices must be NeonSynthVoices.

    LEGATO (keyboard, with the voice limit at 1, MONO). The synth keeps a stack of the
    keyboard keys that are down, most recent last. A key pressed while another is held moves
    the sounding voice to the new note: it glides there at GLIDE time (or jumps, at GLIDE 0)
    and its envelopes, source and velocity carry on. Letting go of the key that sounds, while
    others are still down, moves the voice back to the most recent of them the same way; the
    last key up releases the note as usual. With more than one voice LEGATO does nothing: a
    poly legato would have to guess which voice a key belongs to.

    SLIDE (sequencer, on channel 16, whatever the keyboard's mode). At the step a slide goes
    into, the sequencer sends, on channel 16 and at the same sample, in this order: CC 68
    (Legato Footswitch) = 127, the new note-on, the sliding note's note-off, CC 68 = 0. A
    channel-16 note-on that arrives while CC 68 is down, and finds a voice still holding a
    channel-16 note, is taken by that voice as a legato step, exactly as LEGATO does (GLIDE
    time; at GLIDE 0 a jump in pitch with no envelope restart). The old note-off that follows
    finds no voice on its note any more and does nothing; for a slide to the same pitch it is
    swallowed instead, so the note goes on. The mark is needed because channel 16 notes can
    overlap without a slide (a gate of 100% puts the old note-off on the new note-on's sample,
    after it, and swing with a long gate overlaps by up to half a step): those must keep
    retriggering as they did before v1.11.0, and with no CC 68 they do. A slide that finds no
    held note (it was released, or stolen) starts normally.

    Both work through juce::Synthesiser::startVoice() on the sounding voice, after
    NeonSynthVoice::prepareLegatoStep(), so the voice's note number, channel and age are the
    synth's own bookkeeping: later note-offs, the sustain pedal and voice stealing see the
    new note as if it had been played on that voice. With LEGATO off (or the voice limit
    above 1) and no marks, every event goes to juce::Synthesiser unchanged.

    Audio thread: no allocation; the stack and the mark are plain members, guarded by the
    synth's own lock like everything else it does.
*/
class PlayingSynthesiser : public juce::Synthesiser
{
public:

    /** Audio thread, once per block: LEGATO is on and the voice limit is 1. */
    void setMonoLegato (bool shouldBeOn) noexcept   { monoLegato = shouldBeOn; }

    void noteOn (int midiChannel, int midiNoteNumber, float velocity) override
    {
        const juce::ScopedLock sl (lock);

        if (midiChannel == StepSequencer::midiChannel)
        {
            if (slideDown)
            {
                if (auto* voice = findHeldVoice (midiChannel, -1))
                {
                    // A slide to the pitch already held: the old note-off that follows must not end it.
                    // (The sequencer turns a same-pitch slide into a tie and sends no note-on, so
                    // this is for other senders of the slide MIDI; kept as a safeguard.)
                    if (voice->getCurrentlyPlayingNote() == midiNoteNumber)
                        swallowNoteOff = midiNoteNumber;
                    legatoStep (*voice, midiChannel, midiNoteNumber, velocity);
                    return;
                }
            }
        }
        else
        {
            const bool otherKeysDown = numKeysDown > 0;
            pushKey (midiNoteNumber);

            // GLIDE: whichever voice takes this note glides from the last keyboard note played.
            for (auto* voice : voices)
                static_cast<NeonSynthVoice*> (voice)->setKeyboardGlideFrom (lastKeyboardNote);
            lastKeyboardNote = midiNoteNumber;

            if (monoLegato && otherKeysDown)
            {
                if (auto* voice = findHeldVoice (midiChannel, -1))
                {
                    legatoStep (*voice, midiChannel, midiNoteNumber, velocity);
                    return;
                }
            }
        }

        juce::Synthesiser::noteOn (midiChannel, midiNoteNumber, velocity);
    }

    void noteOff (int midiChannel, int midiNoteNumber, float velocity, bool allowTailOff) override
    {
        const juce::ScopedLock sl (lock);

        if (midiChannel == StepSequencer::midiChannel && midiNoteNumber == swallowNoteOff)
        {
            swallowNoteOff = -1;   // the same-pitch slide's old note-off (see noteOn())
            return;
        }

        if (midiChannel != StepSequencer::midiChannel)
        {
            removeKey (midiNoteNumber);

            // The key that sounds goes up while others are down: back to the most recent of them.
            if (monoLegato && numKeysDown > 0)
            {
                if (auto* voice = findHeldVoice (midiChannel, midiNoteNumber);
                    voice != nullptr && ! voice->isSustainPedalDown() && ! voice->isSostenutoPedalDown())
                {
                    legatoStep (*voice, midiChannel, keysDown[(size_t) (numKeysDown - 1)], velocity);
                    return;
                }
            }
        }

        juce::Synthesiser::noteOff (midiChannel, midiNoteNumber, velocity, allowTailOff);
    }

    void allNotesOff (int midiChannel, bool allowTailOff) override
    {
        const juce::ScopedLock sl (lock);
        if (midiChannel <= 0 || midiChannel != StepSequencer::midiChannel)
            numKeysDown = 0;
        if (midiChannel <= 0 || midiChannel == StepSequencer::midiChannel)
        {
            slideDown = false;
            swallowNoteOff = -1;
        }
        juce::Synthesiser::allNotesOff (midiChannel, allowTailOff);
    }

    void handleController (int midiChannel, int controllerNumber, int controllerValue) override
    {
        if (midiChannel == StepSequencer::midiChannel && controllerNumber == StepSequencer::slideController)
        {
            const juce::ScopedLock sl (lock);
            slideDown = controllerValue >= 64;
            if (! slideDown)
                swallowNoteOff = -1;   // a slide's MIDI ends with CC 68 = 0: nothing is left to swallow
        }
        juce::Synthesiser::handleController (midiChannel, controllerNumber, controllerValue);
    }

    /**
        HOLD switched off (audio thread, before the held note-offs are sent): the keys HOLD kept
        back are no longer down, so they leave the stack first. Otherwise letting go of the
        sounding note would step the voice back through every key HOLD kept, gliding as it went.
    */
    void forgetKey (int note) noexcept
    {
        const juce::ScopedLock sl (lock);
        removeKey (note);
    }

    /** Processor reset: the next keyboard note has nothing to glide from (audio thread). */
    void forgetLastKeyboardNote() noexcept
    {
        const juce::ScopedLock sl (lock);
        lastKeyboardNote = -1;
    }

protected:
    /** How many voices, from the first, new notes may use (LimitedSynthesiser's VOICES). A
        legato step stays within them too. */
    virtual int getVoiceLimit() const noexcept   { return getNumVoices(); }

private:
    /** The most recently started voice holding a note on this channel with its key down
        (any note if `note` is -1), among the first getVoiceLimit() voices. */
    juce::SynthesiserVoice* findHeldVoice (int midiChannel, int note) const noexcept
    {
        juce::SynthesiserVoice* found = nullptr;
        const int limit = juce::jlimit (1, juce::jmax (1, voices.size()), getVoiceLimit());
        for (int i = 0; i < juce::jmin (limit, voices.size()); ++i)
        {
            auto* voice = voices.getUnchecked (i);
            if (voice->getCurrentlyPlayingNote() < 0 || ! voice->isPlayingChannel (midiChannel) || ! voice->isKeyDown())
                continue;
            if (note >= 0 && voice->getCurrentlyPlayingNote() != note)
                continue;
            if (found == nullptr || found->wasStartedBefore (*voice))
                found = voice;
        }
        return found;
    }

    /** Moves a sounding voice to a new note without restarting it (see NeonSynthVoice::prepareLegatoStep()). */
    void legatoStep (juce::SynthesiserVoice& voice, int midiChannel, int midiNoteNumber, float velocity)
    {
        // Every voice of this synth is a NeonSynthVoice (see the class comment).
        static_cast<NeonSynthVoice&> (voice).prepareLegatoStep();
        const auto sound = voice.getCurrentlyPlayingSound();   // startVoice() needs it; the ignored stop keeps it
        startVoice (&voice, sound.get(), midiChannel, midiNoteNumber, velocity);
    }

    void pushKey (int note) noexcept
    {
        removeKey (note);
        if (numKeysDown == (int) keysDown.size())   // full: forget the oldest
            removeKeyAt (0);
        keysDown[(size_t) numKeysDown++] = note;
    }

    void removeKey (int note) noexcept
    {
        for (int i = 0; i < numKeysDown; ++i)
        {
            if (keysDown[(size_t) i] == note)
            {
                removeKeyAt (i);
                return;
            }
        }
    }

    void removeKeyAt (int index) noexcept
    {
        for (int j = index + 1; j < numKeysDown; ++j)
            keysDown[(size_t) (j - 1)] = keysDown[(size_t) j];
        --numKeysDown;
    }

    bool monoLegato = false;
    std::array<int, 16> keysDown {};   // keyboard keys down, most recent last
    int numKeysDown = 0;
    int lastKeyboardNote = -1;   // the keyboard note played last, for GLIDE (-1 = none yet)
    bool slideDown = false;            // CC 68 is down on channel 16: a note-on now is a slide
    int swallowNoteOff = -1;           // a same-pitch slide's old note-off, to be ignored
};
