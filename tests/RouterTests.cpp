// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    The MIDI router on its own (v1.11.1): a step sequencer, a synthesiser with no voices and
    the router between them, with no processor, host or parameters. PlayingTests covers the
    same features through the whole processor, by ear; this checks the MIDI itself.

      - Keyboard input moves to channel 1, whatever channel it arrived on.
      - HOLD keeps the keyboard's note-offs back and sends them, at the block's first sample,
        when it is switched off; note-ons pass; the sequencer's notes never pass through it;
        in Transpose mode the released key goes on steering the pattern until HOLD goes off.
      - ENTER: while armed, keyboard note-ons (also in Replace mode, where they are not played)
        are queued with their velocities, in order; note-offs are not; a full queue drops
        notes instead of growing; disarmed, nothing is queued.
      - The internal clock: steps land on their exact samples (at 120 BPM and 1/16, every 6000
        samples at 48 kHz) whatever the block size, from one sample up; a tempo change carries
        on from the beat reached; switching the sequencer on restarts it at beat 0; with a host
        transport and SYNC on the host's beat and tempo are followed, with SYNC off they are not.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target RouterTests
    Run the resulting RouterTests executable; it exits non-zero on failure.
*/
#include "plugin/MidiRouter.h"
#include <cstdio>
#include <vector>

namespace
{
    int failures = 0;

    void check (bool condition, const char* what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what);
        if (! condition)
            ++failures;
    }

    constexpr double rate = 48000.0;

    /** A MIDI event the router sent, at an absolute sample. */
    struct Sent
    {
        juce::int64 sample;
        juce::MidiMessage message;
    };

    /** A sequencer, a synthesiser and the router between them, as the processor holds them. */
    struct Rig
    {
        StepSequencer sequencer;
        PlayingSynthesiser synth;
        MidiRouter router { sequencer, synth };
        MidiRouter::Controls controls;
        juce::AudioPlayHead* playHead = nullptr;
        juce::int64 now = 0;
        std::vector<Sent> sent;

        Rig()
        {
            for (int i = 0; i < StepSequencer::numSteps; ++i)
                sequencer.setStep (i, { true, 48, 100 });
            sequencer.prepare (rate);
            router.prepare();
            controls.sampleRate = rate;
        }

        /** Routes one block with `input` (sample positions within the block) and keeps what came out. */
        void block (int numSamples, const juce::MidiBuffer& input = {})
        {
            for (const auto event : router.buildPlayMidi (input, numSamples, controls, playHead))
                sent.push_back ({ now + event.samplePosition, event.getMessage() });
            now += numSamples;
        }

        void run (int numSamples, int blockSize)
        {
            for (int done = 0; done < numSamples; done += blockSize)
                block (juce::jmin (blockSize, numSamples - done));
        }

        /** Absolute samples of the note-ons on `channel`. */
        std::vector<juce::int64> noteOns (int channel) const
        {
            std::vector<juce::int64> samples;
            for (const auto& s : sent)
                if (s.message.isNoteOn() && s.message.getChannel() == channel)
                    samples.push_back (s.sample);
            return samples;
        }

        /** Notes of the note-ons on `channel` that start at or after `from`. */
        std::vector<int> notesFrom (juce::int64 from, int channel) const
        {
            std::vector<int> notes;
            for (const auto& s : sent)
                if (s.sample >= from && s.message.isNoteOn() && s.message.getChannel() == channel)
                    notes.push_back (s.message.getNoteNumber());
            return notes;
        }

        int count (bool (*is) (const juce::MidiMessage&), int channel, juce::int64 from = 0) const
        {
            int n = 0;
            for (const auto& s : sent)
                n += s.sample >= from && is (s.message) && s.message.getChannel() == channel ? 1 : 0;
            return n;
        }
    };

    bool isNoteOn  (const juce::MidiMessage& m) { return m.isNoteOn(); }
    bool isNoteOff (const juce::MidiMessage& m) { return m.isNoteOff(); }

    juce::MidiBuffer midi (std::initializer_list<std::pair<int, juce::MidiMessage>> events)
    {
        juce::MidiBuffer buffer;
        for (const auto& [position, message] : events)
            buffer.addEvent (message, position);
        return buffer;
    }

    constexpr int keyboard  = MidiRouter::keyboardChannel;
    constexpr int sequenced = StepSequencer::midiChannel;

    //==========================================================================
    void holdTests()
    {
        std::printf ("HOLD\n");

        {
            Rig rig;
            rig.block (256, midi ({ { 10, juce::MidiMessage::noteOn (5, 60, (juce::uint8) 100) },
                                    { 100, juce::MidiMessage::noteOff (5, 60) } }));
            check (rig.sent.size() == 2 && rig.sent[0].message.isNoteOn() && rig.sent[0].sample == 10
                       && rig.sent[1].message.isNoteOff() && rig.sent[1].sample == 100,
                   "HOLD off: the note-on and note-off pass, on their samples");
            check (rig.sent.size() == 2 && rig.sent[0].message.getChannel() == keyboard && rig.sent[1].message.getChannel() == keyboard,
                   "keyboard input arriving on channel 5 is played on channel 1");
        }

        {
            Rig rig;
            rig.controls.hold = true;
            rig.block (256, midi ({ { 10, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
                                    { 20, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100) },
                                    { 100, juce::MidiMessage::noteOff (1, 60) },
                                    { 120, juce::MidiMessage::noteOn (1, 67, (juce::uint8) 0) } }));   // a note-off as velocity 0
            check (rig.count (isNoteOn, keyboard) == 2 && rig.count (isNoteOff, keyboard) == 0,
                   "HOLD on: note-ons pass, note-offs (also as velocity 0) are kept back");
            rig.block (256);
            check (rig.count (isNoteOff, keyboard) == 0, "and stay kept back while HOLD is on");

            rig.controls.hold = false;
            const auto releasedAt = rig.now;
            rig.block (256);
            std::vector<int> released;
            bool atFirstSample = true;
            for (const auto& s : rig.sent)
            {
                if (s.message.isNoteOff())
                {
                    released.push_back (s.message.getNoteNumber());
                    atFirstSample = atFirstSample && s.sample == releasedAt && s.message.getChannel() == keyboard;
                }
            }
            check (released == std::vector<int> ({ 60, 67 }), "switching HOLD off sends the kept-back note-offs, once each");
            check (atFirstSample, "at the block's first sample, on the keyboard's channel");
            check (released.size() == 2 && rig.count (isNoteOff, keyboard, releasedAt + 1) == 0,
                   "the key HOLD never saw go up (64) is not released");

            rig.block (256, midi ({ { 5, juce::MidiMessage::noteOff (1, 64) } }));
            check (rig.count (isNoteOff, keyboard, releasedAt + 256) == 1, "after HOLD, note-offs pass again");
        }

        {
            Rig rig;   // the sequencer's own notes, in Layer mode
            rig.controls.sequencerOn = true;
            rig.controls.hold = true;
            rig.run (48000, 512);
            check (rig.count (isNoteOn, sequenced) == 8 && rig.count (isNoteOff, sequenced) == 8,
                   "the sequencer's note-offs never pass through HOLD (8 steps, 8 note-offs in a second)");
        }

        {
            Rig rig;   // Transpose mode latches
            rig.controls.sequencerOn = true;
            rig.controls.sequencerMode = MidiRouter::SequencerMode::transpose;
            rig.controls.hold = true;
            rig.block (512, midi ({ { 0, juce::MidiMessage::noteOn (1, 62, (juce::uint8) 100) },
                                    { 300, juce::MidiMessage::noteOff (1, 62) } }));
            check (rig.count (isNoteOn, keyboard) == 0, "Transpose: the key is not played itself");
            rig.run (24000, 512);
            const auto notes = rig.notesFrom (512, sequenced);
            bool allUp2 = ! notes.empty();
            for (auto n : notes)
                allUp2 = allUp2 && n == 48 + 2;
            check (allUp2, "with HOLD on, the released key goes on transposing the pattern (+2)");

            rig.controls.hold = false;
            const auto releasedAt = rig.now;
            rig.run (24000, 512);
            check (rig.notesFrom (releasedAt, sequenced).empty(), "switching HOLD off lets go of the key: the pattern stops");
        }
    }

    //==========================================================================
    void enterTests()
    {
        std::printf ("ENTER\n");

        Rig rig;
        int note = 0, velocity = 0;

        rig.block (64, midi ({ { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) } }));
        check (! rig.router.popEnteredNote (note, velocity), "not armed: nothing is queued");

        rig.router.setNoteEntryArmed (true);
        rig.block (64, midi ({ { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
                               { 10, juce::MidiMessage::noteOff (1, 60) },
                               { 20, juce::MidiMessage::noteOn (3, 64, (juce::uint8) 90) },
                               { 30, juce::MidiMessage::noteOn (1, 67, (juce::uint8) 0) } }));
        bool first  = rig.router.popEnteredNote (note, velocity) && note == 60 && velocity == 100;
        bool second = rig.router.popEnteredNote (note, velocity) && note == 64 && velocity == 90;
        check (first && second, "armed: note-ons are queued with their velocities, in order");
        check (! rig.router.popEnteredNote (note, velocity), "note-offs (and velocity-0 note-ons) are not");

        rig.controls.sequencerOn = true;
        rig.controls.sequencerMode = MidiRouter::SequencerMode::replace;
        const auto before = rig.now;
        rig.block (64, midi ({ { 0, juce::MidiMessage::noteOn (1, 72, (juce::uint8) 80) } }));
        check (rig.count (isNoteOn, keyboard, before) == 0, "Replace mode: the key is not played");
        check (rig.router.popEnteredNote (note, velocity) && note == 72 && velocity == 80, "but it is still queued for ENTER");
        rig.controls.sequencerOn = false;

        juce::MidiBuffer many;
        for (int i = 0; i < 100; ++i)
            many.addEvent (juce::MidiMessage::noteOn (1, 20 + i, (juce::uint8) 100), i);
        rig.block (128, many);
        int queued = 0;
        bool inOrder = true;
        while (rig.router.popEnteredNote (note, velocity))
            inOrder = inOrder && note == 20 + queued++;
        check (queued == 63 && inOrder, "a full queue keeps the first 63 notes and drops the rest");

        rig.router.setNoteEntryArmed (false);
        rig.block (64, midi ({ { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) } }));
        check (! rig.router.popEnteredNote (note, velocity), "disarmed: nothing is queued");
    }

    //==========================================================================
    /** A host transport that is always playing, at a fixed tempo, from a fixed beat. */
    struct Host : juce::AudioPlayHead
    {
        double beat = 0.0, bpm = 90.0;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setIsPlaying (true);
            info.setPpqPosition (beat);
            info.setBpm (bpm);
            return info;
        }
    };

    void clockTests()
    {
        std::printf ("Internal clock\n");

        std::vector<juce::int64> expected;
        for (juce::int64 s = 0; s < 48000; s += 6000)
            expected.push_back (s);

        {
            Rig rig;
            rig.controls.sequencerOn = true;
            rig.run (48000, 4096);
            check (rig.noteOns (sequenced) == expected, "120 BPM, 1/16 at 48 kHz: a step every 6000 samples, from sample 0");
            check (juce::exactlyEqual (rig.router.getSequencerTempo(), 120.0f) && ! rig.router.isSequencerFollowingHost(),
                   "shows the internal tempo, not following a host (there is none)");
        }

        bool sameForEveryBlockSize = true;
        for (int blockSize : { 1, 7, 32, 480, 997, 4096 })
        {
            Rig rig;
            rig.controls.sequencerOn = true;
            rig.run (48000, blockSize);
            sameForEveryBlockSize = sameForEveryBlockSize && rig.noteOns (sequenced) == expected;
        }
        check (sameForEveryBlockSize, "the same samples for blocks of 1, 7, 32, 480, 997 and 4096");

        {
            Rig rig;
            rig.controls.sequencerOn = true;
            rig.run (9000, 1000);   // beat 0.375: steps at 0 and 6000 have played
            rig.controls.tempo = 60.0f;
            rig.run (20000, 1000);
            // The next step (beat 0.5) is 0.125 beats on, at 60 BPM 6000 samples: sample 15000.
            const auto ons = rig.noteOns (sequenced);
            check (ons.size() >= 3 && ons[2] == 15000, "a tempo change carries on from the beat reached (next step at 15000)");
            check (juce::exactlyEqual (rig.router.getSequencerTempo(), 60.0f), "and the new tempo is shown");
        }

        {
            Rig rig;
            rig.controls.sequencerOn = true;
            rig.run (10000, 1000);
            rig.controls.sequencerOn = false;
            rig.run (3000, 1000);
            rig.controls.sequencerOn = true;
            rig.run (7000, 1000);
            const auto ons = rig.noteOns (sequenced);
            check (ons == std::vector<juce::int64> ({ 0, 6000, 13000, 19000 }), "switching the sequencer on restarts the clock at beat 0");
        }

        {
            Host host;
            host.beat = 4.125;   // half way through a step: the next starts at 4.25
            host.bpm = 90.0;
            Rig rig;
            rig.controls.sequencerOn = true;
            rig.playHead = &host;
            rig.block (48000);
            // 0.125 beats at 90 BPM = 0.125 x 32000 samples = 4000.
            const auto ons = rig.noteOns (sequenced);
            check (! ons.empty() && ons[0] == 4000, "SYNC on: the host's beat and tempo place the steps");
            check (juce::exactlyEqual (rig.router.getSequencerTempo(), 90.0f) && rig.router.isSequencerFollowingHost(),
                   "and the host's tempo is shown, following the host");

            Rig unsynced;
            unsynced.controls.sequencerOn = true;
            unsynced.controls.sync = false;
            unsynced.playHead = &host;
            unsynced.run (48000, 4096);
            check (unsynced.noteOns (sequenced) == expected && ! unsynced.router.isSequencerFollowingHost(),
                   "SYNC off: the internal clock plays, whatever the host does");
        }
    }
}

int main()
{
    holdTests();
    enterTests();
    clockTests();

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
