// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "plugin/PlayingSynthesiser.h"

/*
    The voice allocator (v1.11.1: moved here from NeonSynthProcessor.h, unchanged). The
    processor owns one and renders the voices through it; PlayingSynthesiser.h adds LEGATO and
    the sequencer's SLIDE underneath.
*/

/**
    juce::Synthesiser limited to the first N voices (the Voices parameter, v1.9.0): 1 is mono,
    16 is the full pool. At 16 it defers to juce::Synthesiser's own voice choice, so sessions
    saved before v1.9.0 allocate and steal voices exactly as before. Lowering N lets the voices
    above it finish their notes; new notes only go to the first N.
    v1.11.0 (region B): built on PlayingSynthesiser, which adds LEGATO and the sequencer's SLIDE.
*/
class LimitedSynthesiser : public PlayingSynthesiser
{
public:
    void setVoiceLimit (int newLimit) noexcept   { voiceLimit = newLimit; }

protected:
    int getVoiceLimit() const noexcept override   { return voiceLimit; }

    juce::SynthesiserVoice* findFreeVoice (juce::SynthesiserSound* sound, int midiChannel,
                                           int midiNoteNumber, bool stealIfNoneAvailable) const override
    {
        const int limit = juce::jlimit (1, getNumVoices(), voiceLimit);
        if (limit >= getNumVoices())
            return juce::Synthesiser::findFreeVoice (sound, midiChannel, midiNoteNumber, stealIfNoneAvailable);

        for (int i = 0; i < limit; ++i)
            if (auto* v = getVoice (i); ! v->isVoiceActive() && v->canPlaySound (sound))
                return v;

        if (! stealIfNoneAvailable)
            return nullptr;

        // Steal within the first N: the oldest voice already playing this note, else the oldest
        // released voice, else the oldest of all. Released means in its release: key up and
        // not held by the sustain pedal (v1.11.0 fix: a pedal-held note counted as released,
        // and of two voices on the same note the lower one was taken, not the older;
        // VoiceStealingTests).
        juce::SynthesiserVoice* oldestSameNote = nullptr;
        juce::SynthesiserVoice* oldestReleased = nullptr;
        juce::SynthesiserVoice* oldest = nullptr;
        for (int i = 0; i < limit; ++i)
        {
            auto* v = getVoice (i);
            if (! v->canPlaySound (sound))
                continue;
            if (v->getCurrentlyPlayingNote() == midiNoteNumber && (oldestSameNote == nullptr || v->wasStartedBefore (*oldestSameNote)))
                oldestSameNote = v;
            if (v->isPlayingButReleased() && (oldestReleased == nullptr || v->wasStartedBefore (*oldestReleased)))
                oldestReleased = v;
            if (oldest == nullptr || v->wasStartedBefore (*oldest))
                oldest = v;
        }
        return oldestSameNote != nullptr ? oldestSameNote : oldestReleased != nullptr ? oldestReleased : oldest;
    }

private:
    int voiceLimit = 16;
};
