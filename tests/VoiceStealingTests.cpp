// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Which voice a new note takes (v1.11.0), through the whole processor.

    With the Voices limit at N below 16 (NeonSynthProcessor's LimitedSynthesiser), a note goes to
    the first free voice among the first N; with none free it steals, in this order:
      1. the oldest voice already playing the same note,
      2. else the oldest released voice (key up and not held by the sustain pedal),
      3. else the oldest voice.
    Voices above N never start a note. At 16 the choice is juce::Synthesiser's own, unchanged
    since before the limit existed (v1.9.0), so old sessions allocate exactly as they did.

    Hand-written scenarios check each rule at 1, 4 and 15 voices; then seeded random sequences
    of note-ons, note-offs and sustain pedal moves (with repeated pitches) are played through the
    processor and through a reference: a plain juce::Synthesiser with probe voices, whose
    findFreeVoice is the rule above written out again here (left as JUCE's at 16). Both must pick
    the same voice for every note. The probes stay active until stolen, like the processor's
    voices with a 10 s release, so the reference keeps JUCE's own key, pedal and age book-keeping.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target VoiceStealingTests
    Run the resulting VoiceStealingTests executable; it exits non-zero on failure.
*/
#include "plugin/NeonSynthProcessor.h"
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    int failures = 0;

    void check (bool condition, const juce::String& what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what.toRawUTF8());
        if (! condition)
            ++failures;
    }

    constexpr double rate = 48000.0;
    constexpr int    blockSize = 64;   // one event per block, so every note has its own age
    constexpr int    channel = 1;      // the processor moves keyboard input to channel 1

    void setParameter (juce::AudioProcessor& p, const juce::String& id, float value)
    {
        for (auto* param : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param); ranged != nullptr && ranged->getParameterID() == id)
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
    }

    struct Event
    {
        enum Type { on, off, pedalDown, pedalUp, wait } type;
        int note = 0;   // for wait: how many blocks of silence

        juce::MidiMessage message() const
        {
            switch (type)
            {
                case on:        return juce::MidiMessage::noteOn (channel, note, (juce::uint8) 100);
                case off:       return juce::MidiMessage::noteOff (channel, note);
                case pedalDown: return juce::MidiMessage::controllerEvent (channel, 64, 127);
                case pedalUp:   return juce::MidiMessage::controllerEvent (channel, 64, 0);
                case wait:      break;
            }
            return {};
        }
    };

    Event on (int note)   { return { Event::on, note }; }
    Event off (int note)  { return { Event::off, note }; }
    const Event pedalDown { Event::pedalDown, 0 };
    const Event pedalUp   { Event::pedalUp, 0 };
    Event waitSeconds (double seconds)   { return { Event::wait, (int) std::ceil (seconds * rate / blockSize) }; }

    //==========================================================================
    /** The processor under test, one event per block; reports the voice each note-on took. */
    struct ProcessorRun
    {
        explicit ProcessorRun (int voiceLimit, float releaseSeconds = 10.0f)
        {
            setParameter (p, "voiceCount", (float) voiceLimit);
            setParameter (p, "attack", 0.001f);
            setParameter (p, "sustain", 1.0f);
            setParameter (p, "release", releaseSeconds);   // 10 s: released notes keep sounding for the whole test
            p.setRateAndBufferSizeDetails (rate, blockSize);
            p.prepareToPlay (rate, blockSize);
        }

        ~ProcessorRun()   { p.releaseResources(); }

        /** Plays one event at the block's first sample; for a note-on, returns the voice that
            started it (the sounding voice that has run for exactly this block), else -1. */
        int play (const Event& e)
        {
            juce::MidiBuffer midi;
            if (e.type == Event::wait)
            {
                for (int b = 0; b < e.note; ++b)
                    p.processBlock (buffer, midi);
                return -1;
            }
            midi.addEvent (e.message(), 0);
            p.processBlock (buffer, midi);
            if (e.type != Event::on)
                return -1;

            const auto& views = p.getVoiceViews();
            const float justStarted = (float) blockSize / (float) rate;
            const float hz = (float) juce::MidiMessage::getMidiNoteInHertz (e.note);
            int found = -1;
            for (size_t i = 0; i < views.size(); ++i)
                if (views[i].active.load() && juce::exactlyEqual (views[i].secondsSinceOn.load(), justStarted)
                      && std::abs (views[i].frequencyHz.load() - hz) < 0.01f)
                    found = found == -1 ? (int) i : -2;   // -2: more than one, which is a failure too
            return found;
        }

        int numSounding() const
        {
            int n = 0;
            for (const auto& v : p.getVoiceViews())
                n += v.active.load() ? 1 : 0;
            return n;
        }

        bool anyAbove (int limit) const
        {
            const auto& views = p.getVoiceViews();
            for (size_t i = (size_t) limit; i < views.size(); ++i)
                if (views[i].active.load())
                    return true;
            return false;
        }

        NeonSynthProcessor p;
        juce::AudioBuffer<float> buffer { 2, blockSize };
    };

    //==========================================================================
    // The reference: juce::Synthesiser with probe voices. JUCE does the key, pedal and age
    // book-keeping; only the choice of voice below 16 is written out here, from the rule.

    int lastStarted = -1;

    struct ProbeSound : juce::SynthesiserSound
    {
        bool appliesToNote (int) override    { return true; }
        bool appliesToChannel (int) override { return true; }
    };

    struct ProbeVoice : juce::SynthesiserVoice
    {
        explicit ProbeVoice (int i) : index (i) {}
        bool canPlaySound (juce::SynthesiserSound*) override { return true; }
        void startNote (int, float, juce::SynthesiserSound*, int) override { lastStarted = index; }
        void stopNote (float, bool allowTailOff) override
        {
            if (! allowTailOff)
                clearCurrentNote();   // stolen; a released note stays active (a long release)
        }
        void pitchWheelMoved (int) override {}
        void controllerMoved (int, int) override {}
        using SynthesiserVoice::renderNextBlock;   // keep the double-precision overload visible
        void renderNextBlock (juce::AudioBuffer<float>&, int, int) override {}
        const int index;
    };

    struct ReferenceSynth : juce::Synthesiser
    {
        explicit ReferenceSynth (int voiceLimit) : limit (voiceLimit)
        {
            for (int i = 0; i < NeonSynthProcessor::numVoices; ++i)
                addVoice (new ProbeVoice (i));
            addSound (new ProbeSound());
            setCurrentPlaybackSampleRate (rate);
        }

        int play (const Event& e)
        {
            lastStarted = -1;
            juce::MidiBuffer midi;
            midi.addEvent (e.message(), 0);
            juce::AudioBuffer<float> buffer (2, blockSize);
            renderNextBlock (buffer, midi, 0, blockSize);
            return e.type == Event::on ? lastStarted : -1;
        }

        juce::SynthesiserVoice* findFreeVoice (juce::SynthesiserSound* sound, int midiChannel,
                                               int midiNoteNumber, bool steal) const override
        {
            if (limit >= getNumVoices())
                return juce::Synthesiser::findFreeVoice (sound, midiChannel, midiNoteNumber, steal);

            for (int i = 0; i < limit; ++i)
                if (! getVoice (i)->isVoiceActive())
                    return getVoice (i);

            const auto oldestWhere = [this] (auto&& condition) -> juce::SynthesiserVoice*
            {
                juce::SynthesiserVoice* oldest = nullptr;
                for (int i = 0; i < limit; ++i)
                    if (auto* v = getVoice (i); condition (v) && (oldest == nullptr || v->wasStartedBefore (*oldest)))
                        oldest = v;
                return oldest;
            };

            if (auto* v = oldestWhere ([&] (auto* x) { return x->getCurrentlyPlayingNote() == midiNoteNumber; }))
                return v;
            if (auto* v = oldestWhere ([] (auto* x) { return x->isPlayingButReleased(); }))
                return v;
            return oldestWhere ([] (auto*) { return true; });
        }

        const int limit;
    };

    //==========================================================================
    /** Plays `events` and returns the voice each note-on took (-1 for the other events). */
    std::vector<int> voicesTaken (int limit, const std::vector<Event>& events, float releaseSeconds = 10.0f)
    {
        ProcessorRun run (limit, releaseSeconds);
        std::vector<int> taken;
        for (const auto& e : events)
            taken.push_back (run.play (e));
        return taken;
    }

    juce::String limitName (int limit)   { return juce::String (limit) + (limit == 1 ? " voice" : " voices"); }

    void testFreeVoicesFirst()
    {
        std::printf ("Free voices first, never above the limit\n");
        for (int limit : { 1, 4, 15, 16 })
        {
            ProcessorRun run (limit);
            bool inOrder = true;
            for (int i = 0; i < limit; ++i)
                inOrder = inOrder && run.play (on (40 + i)) == i;
            check (inOrder && run.numSounding() == limit, limitName (limit) + ": notes take voices 1.." + juce::String (limit) + " in turn");

            // Twice as many notes again: only the first N ever sound.
            for (int i = 0; i < 2 * limit; ++i)
                run.play (on (70 + i));
            check (! run.anyAbove (limit) && run.numSounding() == limit, limitName (limit) + ": no voice above the limit starts");
        }
    }

    void testStealingOrder()
    {
        std::printf ("Stealing order\n");
        for (int limit : { 1, 4, 15 })
        {
            const auto name = limitName (limit);
            std::vector<Event> fill;
            for (int i = 0; i < limit; ++i)
                fill.push_back (on (40 + i));   // voice i plays note 40 + i, voice 0 the oldest

            // All keys held: the oldest voice goes.
            {
                auto events = fill;
                events.push_back (on (90));
                check (voicesTaken (limit, events).back() == 0, name + ", all held: the oldest voice is stolen");
            }

            // The newest note played again while an older note is released: the same pitch wins.
            {
                auto events = fill;
                events.push_back (off (40));
                events.push_back (on (40 + limit - 1));
                check (voicesTaken (limit, events).back() == limit - 1, name + ": the same note is reused before a released voice");
            }

            if (limit == 1)
                continue;   // the rules below need two voices to choose between

            // Two released voices: the older of them, not the oldest of all (which is held).
            {
                auto events = fill;
                events.push_back (off (40 + limit - 1));
                events.push_back (off (41));
                events.push_back (on (90));
                check (voicesTaken (limit, events).back() == 1, name + ": the oldest released voice is stolen before held ones");
            }

            // A note held by the sustain pedal is not released: a truly released one goes first,
            // even when the pedal-held note is older.
            {
                std::vector<Event> events;
                events.push_back (on (40));               // voice 0: will be held by the pedal
                events.push_back (on (41));               // voice 1: released before the pedal
                events.push_back (off (41));
                events.push_back (pedalDown);
                events.push_back (off (40));              // key up, but the pedal holds it
                for (int i = 2; i < limit; ++i)
                    events.push_back (on (40 + i));
                events.push_back (on (90));
                check (voicesTaken (limit, events).back() == 1, name + ": a pedal-held note is kept; the released voice is stolen");
            }

            // Two voices on the same pitch, the newer one on the lower voice: the older is reused.
            // (A 50 ms release here, so a voice can end and be free again.)
            {
                std::vector<Event> events;
                events.push_back (on (40));               // voice 0
                events.push_back (on (50));               // voice 1
                for (int i = 2; i < limit; ++i)
                    events.push_back (on (60 + i));       // the rest, held
                events.push_back (off (40));
                events.push_back (waitSeconds (0.2));     // voice 0 has ended: free
                events.push_back (on (50));               // 50 again: voice 1 releases, voice 0 (free) takes it
                events.push_back (on (50));               // and again, all busy: the older 50 is voice 1
                const auto taken = voicesTaken (limit, events, 0.05f);
                check (taken[taken.size() - 2] == 0 && taken.back() == 1,
                       name + ": of two voices on the same note, the older is reused (not the lower one)");
            }
        }
    }

    /** A seeded random performance: note-ons (a narrow range, so pitches repeat, sometimes with
        no note-off in between), note-offs of sounding keys, and the sustain pedal. */
    std::vector<Event> randomPerformance (juce::int64 seed, int length)
    {
        juce::Random random (seed);
        std::vector<Event> events;
        std::vector<int> down;
        bool pedal = false;
        for (int n = 0; n < length; ++n)
        {
            const int r = random.nextInt (100);
            if (r < 8)
            {
                events.push_back (pedal ? pedalUp : pedalDown);
                pedal = ! pedal;
            }
            else if (r < 50 && ! down.empty())
            {
                const auto k = (size_t) random.nextInt ((int) down.size());
                events.push_back (off (down[k]));
                down.erase (down.begin() + (std::ptrdiff_t) k);
            }
            else
            {
                const int note = 48 + random.nextInt (20);
                events.push_back (on (note));
                down.push_back (note);
            }
        }
        return events;
    }

    void testAgainstReference()
    {
        std::printf ("Random performances against the reference\n");
        for (int limit : { 1, 4, 15, 16 })
        {
            int notes = 0, mismatches = 0, steals = 0;
            for (juce::int64 seed = 1; seed <= 6; ++seed)
            {
                const auto events = randomPerformance (seed * 7919 + limit, 120);
                ProcessorRun run (limit);
                ReferenceSynth reference (limit);
                for (const auto& e : events)
                {
                    const int sounding = run.numSounding();
                    const int got = run.play (e);
                    const int expected = reference.play (e);
                    if (e.type != Event::on)
                        continue;
                    ++notes;
                    steals += sounding >= limit ? 1 : 0;
                    if (got != expected)
                    {
                        if (mismatches++ == 0)
                            std::printf ("    seed %d, note %d: voice %d, the rule says %d\n", (int) seed, e.note, got + 1, expected + 1);
                    }
                }
            }
            check (mismatches == 0 && steals > 20,
                   limitName (limit) + ": " + juce::String (notes) + " notes (" + juce::String (steals) + " steals), "
                       + juce::String (mismatches) + " on another voice than " + (limit == 16 ? "juce::Synthesiser's" : "the rule's"));
        }
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // the processor's sample loader posts to the message thread

    testFreeVoicesFirst();
    testStealingOrder();
    testAgainstReference();

    if (failures == 0)
        std::printf ("VoiceStealingTests: all passed\n");
    else
        std::printf ("VoiceStealingTests: %d FAILED\n", failures);

    return failures == 0 ? 0 : 1;
}
