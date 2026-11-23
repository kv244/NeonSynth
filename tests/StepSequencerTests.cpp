// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Unit tests for StepSequencer (sample-accurate timing, gate, swing,
    transport stop / jump, tolerance of small transport drift, block-size
    independence, triggers disabled, retriggering and step packing), and a
    seeded property test with random settings and block sizes.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target StepSequencerTests
    Run the resulting StepSequencerTests executable; it exits non-zero on failure.
*/
#include <juce_audio_basics/juce_audio_basics.h>
#include "sequencer/StepSequencer.h"
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

    struct Event { juce::int64 sample; bool on; int note; int velocity; int controller = -1; int value = 0; };   // controller >= 0: a CC (the slide mark)

    constexpr double testSampleRate = 48000.0;
    constexpr double testBpm        = 120.0;                         // 24000 samples per beat
    constexpr double testSamplesPerBeat = testSampleRate * 60.0 / testBpm;

    /** Runs the sequencer over `totalSamples` with the given block sizes, collecting absolute event times. */
    std::vector<Event> run (StepSequencer& seq, juce::int64 totalSamples, const StepSequencer::Settings& settings,
                            const std::vector<int>& blockSizes = { 512 }, double startBeat = 0.0)
    {
        std::vector<Event> events;
        juce::MidiBuffer midi;
        midi.ensureSize (4096);

        juce::int64 position = 0;
        size_t blockIndex = 0;

        while (position < totalSamples)
        {
            const int n = (int) std::min<juce::int64> (blockSizes[blockIndex++ % blockSizes.size()], totalSamples - position);
            StepSequencer::Transport t { true, startBeat + (double) position / testSamplesPerBeat, testBpm };

            midi.clear();
            seq.process (midi, n, t, settings);

            for (const auto m : midi)
            {
                const auto msg = m.getMessage();
                if (msg.isController())
                    events.push_back ({ position + m.samplePosition, false, -1, 0, msg.getControllerNumber(), msg.getControllerValue() });
                else
                    events.push_back ({ position + m.samplePosition, msg.isNoteOn(), msg.getNoteNumber(), msg.getVelocity() });
            }
            position += n;
        }
        return events;
    }

    /** Prepares a sequencer with every step on, notes 60..75 (StepSequencer holds atomics, so it is not copyable). */
    void makeAllOn (StepSequencer& seq)
    {
        seq.prepare (testSampleRate);
        for (int i = 0; i < StepSequencer::numSteps; ++i)
            seq.setStep (i, { true, 60 + i, 100 });
    }

    /** v1.11.0: only the note-offs. */
    std::vector<Event> offsOnly (const std::vector<Event>& events)
    {
        std::vector<Event> out;
        for (const auto& e : events)
            if (! e.on && e.controller < 0) out.push_back (e);
        return out;
    }

    bool sameEvents (const std::vector<Event>& a, const std::vector<Event>& b)
    {
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); ++i)
            same = a[i].sample == b[i].sample && a[i].on == b[i].on && a[i].note == b[i].note
                && a[i].controller == b[i].controller && a[i].value == b[i].value;
        return same;
    }

    /** v1.11.0: a sequencer playing `pattern`. */
    void load (StepSequencer& seq, const StepSequencer::Pattern& pattern)
    {
        seq.prepare (testSampleRate);
        for (int i = 0; i < StepSequencer::numSteps; ++i)
            seq.setStep (i, pattern[(size_t) i]);
    }

    StepSequencer::Pattern allOnPattern()
    {
        StepSequencer::Pattern p;
        for (int i = 0; i < StepSequencer::numSteps; ++i)
            p[(size_t) i] = { true, 60 + i, 100 };
        return p;
    }

    std::vector<Event> onsOnly (const std::vector<Event>& events)
    {
        std::vector<Event> out;
        for (const auto& e : events)
            if (e.on) out.push_back (e);
        return out;
    }
}

int main()
{
    StepSequencer::Settings settings;   // 1/16, no swing, 50% gate

    std::printf ("Timing\n");
    {
        StepSequencer seq; makeAllOn (seq);
        const auto ons = onsOnly (run (seq, (juce::int64) (4 * testSamplesPerBeat), settings));   // one bar
        bool exact = ons.size() == 16;
        for (size_t i = 0; exact && i < ons.size(); ++i)
            exact = ons[i].sample == (juce::int64) i * 6000 && ons[i].note == 60 + (int) i;
        check (exact, "16 steps per bar at 1/16, each exactly 6000 samples apart, notes in order");
    }

    std::printf ("Gate\n");
    {
        StepSequencer seq; makeAllOn (seq);
        const auto events = run (seq, (juce::int64) (4 * testSamplesPerBeat), settings);
        bool ok = true;
        for (const auto& e : events)
            if (! e.on && e.note == 60) ok = (e.sample == 3000);
        check (ok, "50% gate: note-off lands half a step (3000 samples) after the note-on");

        int ons = 0, offs = 0;
        for (const auto& e : events) (e.on ? ons : offs)++;
        check (ons == 16 && offs == 16, "every note-on has a matching note-off within the bar");
    }

    std::printf ("Swing\n");
    {
        StepSequencer seq; makeAllOn (seq);
        auto swung = settings;
        swung.swing = 0.5f;   // odd steps delayed by 0.5 * half a step = 1500 samples
        const auto ons = onsOnly (run (seq, (juce::int64) (4 * testSamplesPerBeat), swung));
        bool ok = ons.size() == 16;
        for (size_t i = 0; ok && i < ons.size(); ++i)
            ok = ons[i].sample == (juce::int64) i * 6000 + ((i & 1) ? 1500 : 0);
        check (ok, "swing 50% delays only the odd steps, by 1500 samples");

        // Swing moved while the pattern plays (v1.11.0 fix): each step fires exactly once.
        // Raising swing just after an odd step fired used to move it past the "already fired"
        // mark and fire it again; lowering it before an odd step fired could drop that step.
        auto changingSwing = [&] (float before, float after, juce::int64 changeAt)
        {
            StepSequencer changing; makeAllOn (changing);
            juce::MidiBuffer midi;
            int notesOn = 0;
            for (juce::int64 position = 0; position < (juce::int64) (4 * testSamplesPerBeat); position += 256)
            {
                auto s2 = settings;
                s2.swing = position < changeAt ? before : after;
                midi.clear();
                changing.process (midi, 256, { true, (double) position / testSamplesPerBeat, testBpm }, s2);
                for (const auto m : midi)
                    notesOn += m.getMessage().isNoteOn() ? 1 : 0;
            }
            return notesOn;
        };
        check (changingSwing (0.0f, 0.9f, 6000 + 512) == 16, "swing raised just after an odd step fired: 16 notes in the bar, none twice");
        check (changingSwing (0.9f, 0.0f, 6000 + 512) == 16, "swing lowered before an odd step fired: 16 notes in the bar, none lost");
    }

    std::printf ("Rates\n");
    {
        StepSequencer seq; makeAllOn (seq);
        auto eighths = settings;
        eighths.rate = StepSequencer::Rate::eighth;
        check (onsOnly (run (seq, (juce::int64) (4 * testSamplesPerBeat), eighths)).size() == 8, "1/8: 8 steps per bar");

        StepSequencer seq2; makeAllOn (seq2);
        auto triplets = settings;
        triplets.rate = StepSequencer::Rate::sixteenthTriplet;
        check (onsOnly (run (seq2, (juce::int64) (4 * testSamplesPerBeat), triplets)).size() == 24, "1/16T: 24 steps per bar");
    }

    std::printf ("Block-size independence\n");
    {
        StepSequencer a; makeAllOn (a);
        StepSequencer b; makeAllOn (b);
        const auto even   = run (a, (juce::int64) (8 * testSamplesPerBeat), settings, { 512 });
        const auto uneven = run (b, (juce::int64) (8 * testSamplesPerBeat), settings, { 37, 1000, 4096, 1, 777 });
        bool same = even.size() == uneven.size();
        for (size_t i = 0; same && i < even.size(); ++i)
            same = even[i].sample == uneven[i].sample && even[i].on == uneven[i].on && even[i].note == uneven[i].note;
        check (same, "identical events with 512-sample blocks and irregular blocks (37/1000/4096/1/777)");
    }

    std::printf ("Transport\n");
    {
        StepSequencer seq; makeAllOn (seq);
        juce::MidiBuffer midi;
        midi.ensureSize (1024);

        seq.process (midi, 512, { true, 0.0, testBpm }, settings);              // step 0 starts, note-off pending
        midi.clear();
        seq.process (midi, 512, { false, 0.0, testBpm }, settings);             // transport stops
        int offs = 0;
        for (const auto m : midi) if (m.getMessage().isNoteOff() && m.samplePosition == 0) ++offs;
        check (offs == 1 && seq.getCurrentStep() == -1, "stopping releases the sounding note at once and parks the playhead");

        midi.clear();
        seq.process (midi, 512, { true, 1.0, testBpm }, settings);              // restart mid-bar (beat 1 = step 4)
        midi.clear();
        seq.process (midi, 512, { true, 0.0, testBpm }, settings);              // host loops back to beat 0
        bool releasedBeforeRestart = false;
        for (const auto m : midi)
            if (m.getMessage().isNoteOff() && m.samplePosition == 0) releasedBeforeRestart = true;
        check (releasedBeforeRestart && seq.getCurrentStep() == 0, "a jump in time releases notes, then playback resumes at the new position");
    }

    std::printf ("Transport drift\n");
    {
        // Host tempo changes from 120 to 126 BPM halfway through block 20. The host's
        // beat position follows the real tempo; each block reports the tempo at its
        // start, so the sequencer's prediction is about 26 samples short once.
        // Legato gate (100%), so a note is always sounding when that happens.
        StepSequencer seq; makeAllOn (seq);
        auto legato = settings;
        legato.gate = 1.0f;
        juce::MidiBuffer midi;
        midi.ensureSize (4096);
        const int block = 1024;
        double hostBeat = 0.0, bpm = testBpm;
        int ons = 0, cutShort = 0;
        std::vector<juce::int64> onTimes;
        juce::int64 noteOnAt[128] = {};
        juce::int64 position = 0;

        for (int b = 0; b < 200; ++b, position += block)
        {
            midi.clear();
            seq.process (midi, block, { true, hostBeat, bpm }, legato);
            for (const auto m : midi)
            {
                const auto msg = m.getMessage();
                const auto at  = position + m.samplePosition;
                if (msg.isNoteOn()) { ++ons; onTimes.push_back (at); noteOnAt[msg.getNoteNumber()] = at; }
                else if (at - noteOnAt[msg.getNoteNumber()] < 5600)   // a full step is 5714-6000 samples
                    ++cutShort;
            }

            const double spb = testSampleRate * 60.0 / bpm;
            if (b == 20)   // tempo change at the block's midpoint
            {
                hostBeat += (block / 2) / spb;
                bpm = 126.0;
                hostBeat += (block / 2) / (testSampleRate * 60.0 / bpm);
            }
            else
            {
                hostBeat += block / spb;
            }
        }
        bool noDuplicates = true;
        for (size_t i = 1; i < onTimes.size(); ++i)
            noDuplicates = noDuplicates && onTimes[i] - onTimes[i - 1] > 4000;   // steps are ~5700-6000 samples apart
        check (cutShort == 0 && noDuplicates && ons > 30,
               "a tempo change inside a block is not mistaken for a jump: no notes cut, none doubled");

        // Beat position jittering by up to +/-1 sample every block (host rounding).
        StepSequencer jit; makeAllOn (jit);
        juce::Random random (1234);
        int jitOns = 0;
        std::vector<juce::int64> jitTimes;
        position = 0;
        for (int b = 0; b < 188; ++b, position += 512)   // 96256 samples: four beats (16 steps) plus a little
        {
            const double jitter = b == 0 ? 0.0 : (random.nextFloat() * 2.0 - 1.0) / testSamplesPerBeat;
            midi.clear();
            jit.process (midi, 512, { true, (double) position / testSamplesPerBeat + jitter, testBpm }, settings);
            for (const auto m : midi)
            {
                const auto msg = m.getMessage();
                const auto at  = position + m.samplePosition;
                if (msg.isNoteOn()) { ++jitOns; jitTimes.push_back (at); noteOnAt[msg.getNoteNumber()] = at; }
                else if (std::abs (at - noteOnAt[msg.getNoteNumber()] - 3000) > 2) ++cutShort;   // 50% gate
            }
        }
        bool onGrid = jitOns == 17;   // steps 0..16
        for (size_t i = 0; onGrid && i < jitTimes.size(); ++i)
            onGrid = std::abs (jitTimes[i] - (juce::int64) i * 6000) <= 2;
        check (onGrid && cutShort == 0,
               "+/-1 sample of beat jitter: every step fires once, within 2 samples of its slot, at full length");

        // A small backward drift right after a step must not fire that step again.
        StepSequencer back; makeAllOn (back);
        midi.clear();
        back.process (midi, 512, { true, 5490.0 / testSamplesPerBeat, testBpm }, settings);       // step 1 (6000) at offset 510
        midi.clear();
        back.process (midi, 512, { true, 5999.0 / testSamplesPerBeat, testBpm }, settings);       // 3 samples back: 6000 is in this block too
        int again = 0;
        for (const auto m : midi) if (m.getMessage().isNoteOn()) ++again;
        check (again == 0, "a 3-sample backward drift does not retrigger the step that just fired");
    }

    std::printf ("Triggers disabled\n");
    {
        StepSequencer seq; makeAllOn (seq);
        juce::MidiBuffer midi;
        midi.ensureSize (1024);
        seq.process (midi, 512, { true, 0.0, testBpm }, settings);              // note-on at 0, off due at 3000
        auto quiet = settings;
        quiet.triggersEnabled = false;
        int ons = 0, offs = 0;
        for (int block = 1; block < 24; ++block)
        {
            midi.clear();
            seq.process (midi, 512, { true, block * 512 / testSamplesPerBeat, testBpm }, quiet);
            for (const auto m : midi) (m.getMessage().isNoteOn() ? ons : offs)++;
        }
        check (ons == 0 && offs == 1, "with triggers disabled no new notes start, but the pending note-off still arrives");
    }

    std::printf ("Retrigger\n");
    {
        StepSequencer seq;
        seq.prepare (testSampleRate);
        for (int i = 0; i < StepSequencer::numSteps; ++i)
            seq.setStep (i, { true, 64, 90 });                              // same note on every step
        auto legato = settings;
        legato.gate = 1.0f;                                                 // note-off exactly at the next step
        const auto events = run (seq, 12000, legato);                       // two steps
        // Expect: on@0, then at 6000 an off followed by an on (off first so the voice is released)
        bool ok = events.size() >= 3 && events[0].on && events[0].sample == 0
               && ! events[1].on && events[1].sample == 6000
               && events[2].on && events[2].sample == 6000;
        check (ok, "retriggering a still-sounding note sends its note-off first, at the same sample");
    }

    std::printf ("Packing\n");
    {
        bool ok = true;
        for (int note : { 0, 60, 127 })
            for (int vel : { 1, 64, 127 })
                for (bool on : { false, true })
                    ok = ok && StepSequencer::unpack (StepSequencer::pack ({ on, note, vel })) == StepSequencer::Step { on, note, vel };
        check (ok, "pack/unpack round-trips every on/note/velocity combination tested");
    }

    //==========================================================================
    // v1.11.0
    std::printf ("Pattern length (v1.11.0)\n");
    {
        bool wraps = true;
        for (int length = 1; length <= StepSequencer::numSteps; ++length)
        {
            StepSequencer seq; makeAllOn (seq);
            auto s = settings;
            s.length = length;
            const auto ons = onsOnly (run (seq, (juce::int64) (8 * testSamplesPerBeat), s, { 37, 512, 1000 }));   // 32 steps
            bool ok = ons.size() == 32;
            for (size_t k = 0; ok && k < ons.size(); ++k)
                ok = ons[k].sample == (juce::int64) k * 6000 && ons[k].note == 60 + (int) k % length;
            if (! ok) std::printf ("        length %d wrong\n", length);
            wraps = wraps && ok;
        }
        check (wraps, "every length 1..16 repeats after that many steps, each step on its sample");

        StepSequencer a; makeAllOn (a);
        StepSequencer b; makeAllOn (b);
        auto sixteen = settings;
        sixteen.length = 16;
        StepSequencer::Settings plain;   // the default, as every session before v1.11.0
        check (sameEvents (run (a, (juce::int64) (16 * testSamplesPerBeat), plain, { 333, 4096 }),
                           run (b, (juce::int64) (16 * testSamplesPerBeat), sixteen, { 333, 4096 })),
               "length 16 plays exactly as before (the default)");

        StepSequencer c; makeAllOn (c);
        auto odd = settings;
        odd.length = 5;
        odd.swing  = 0.5f;
        const auto ons = onsOnly (run (c, (juce::int64) (4 * testSamplesPerBeat), odd));
        bool swingOnBeat = ons.size() == 16;
        for (size_t k = 0; swingOnBeat && k < ons.size(); ++k)
            swingOnBeat = ons[k].sample == (juce::int64) k * 6000 + ((k & 1) ? 1500 : 0);
        check (swingOnBeat, "swing follows the beat grid, not the pattern, at an odd length");

        auto outOfRange = settings;
        outOfRange.length = 0;
        StepSequencer d; makeAllOn (d);
        const auto clamped = onsOnly (run (d, 12000, outOfRange));
        check (clamped.size() == 2 && clamped[1].note == 60, "a length below 1 counts as 1");
    }

    std::printf ("Tie (v1.11.0)\n");
    {
        auto p = allOnPattern();
        p[0].tie = true;                       // 60 held through step 1 (whose own note, 61, is not played)
        p[2].tie = true; p[3].tie = true;      // 62 held through steps 3 and 4
        p[4].on  = false;                      // a tie holds over a step that is off, too
        StepSequencer seq; load (seq, p);
        const auto events = run (seq, (juce::int64) (4 * testSamplesPerBeat), settings, { 100, 4096, 7 });
        const auto ons = onsOnly (events), offs = offsOnly (events);

        bool noRetrigger = true;
        for (const auto& e : ons)
            noRetrigger = noRetrigger && e.note != 61 && e.note != 63 && e.note != 64;
        check (noRetrigger && ons.size() == 13, "a tied-over step sends no note-on of its own (13 of 16 steps start notes)");

        juce::int64 off60 = -1, off62 = -1;
        for (const auto& e : offs)
        {
            if (e.note == 60) off60 = e.sample;
            if (e.note == 62) off62 = e.sample;
        }
        check (off60 == 6000 + 3000, "a tie over one step ends half a step (the gate) after that step starts");
        check (off62 == 4 * 6000 + 3000, "a tie over two steps ends with the gate of the last step");

        // The last step tied into the first, at a length of 3.
        auto q = allOnPattern();
        q[2].tie = true;
        StepSequencer wrap; load (wrap, q);
        auto three = settings;
        three.length = 3;
        const auto wrapped = run (wrap, (juce::int64) (2 * testSamplesPerBeat), three);   // 8 steps: 60 61 62~ 61 62~ 61
        int wrapOns = 0; bool noStep0 = true;
        for (const auto& e : onsOnly (wrapped)) { ++wrapOns; noStep0 = noStep0 && (e.sample == 0 || e.note != 60); }
        check (wrapOns == 6 && noStep0, "at length 3 the third step ties round into the first");

        // Triggers disabled (Transpose, key released) while a tie holds: it ends at the next step.
        StepSequencer quietSeq; load (quietSeq, p);
        juce::MidiBuffer midi; midi.ensureSize (1024);
        quietSeq.process (midi, 6000, { true, 0.0, testBpm }, settings);
        auto quiet = settings; quiet.triggersEnabled = false;
        midi.clear();
        quietSeq.process (midi, 6000, { true, 6000.0 / testSamplesPerBeat, testBpm }, quiet);
        int quietOffs = 0, quietAt = -1;
        for (const auto m : midi) if (m.getMessage().isNoteOff()) { ++quietOffs; quietAt = m.samplePosition; }
        check (quietOffs == 1 && quietAt == 0, "with triggers disabled a held tie ends at the next step");

        // Stopping while a tie holds releases it.
        StepSequencer stopSeq; load (stopSeq, p);
        midi.clear();
        stopSeq.process (midi, 512, { true, 0.0, testBpm }, settings);
        midi.clear();
        stopSeq.process (midi, 512, { false, 0.0, testBpm }, settings);
        int stopOffs = 0;
        for (const auto m : midi) if (m.getMessage().isNoteOff() && m.samplePosition == 0) ++stopOffs;
        check (stopOffs == 1, "stopping releases a held tie at once");

        // Every step tied: one note-on, and the note sounds until the transport stops.
        auto all = allOnPattern();
        for (auto& st : all) st.tie = true;
        StepSequencer allSeq; load (allSeq, all);
        const auto held = run (allSeq, (juce::int64) (8 * testSamplesPerBeat), settings, { 999 });
        check (onsOnly (held).size() == 1 && offsOnly (held).empty(), "with every step tied the first note holds and nothing retriggers");
    }

    std::printf ("Slide (v1.11.0)\n");
    {
        auto p = allOnPattern();
        p[0].slide = true;                                // 60 slides to 61 at step 1
        p[4].slide = true; p[5].on = false;               // a slide into a rest
        p[8].slide = true; p[9].note = p[8].note;         // a slide onto the same pitch is a tie
        p[12].slide = true; p[13].slide = true;           // two slides in a row: 72 -> 73 -> 74
        StepSequencer seq; load (seq, p);
        const auto events = run (seq, (juce::int64) (4 * testSamplesPerBeat), settings, { 6000 });   // a block boundary on every step

        // At step 1 (sample 6000): CC 68 on, note-on 61, note-off 60, CC 68 off, in that order.
        std::vector<Event> at6000;
        for (const auto& e : events) if (e.sample == 6000) at6000.push_back (e);
        const bool order = at6000.size() == 4
            && at6000[0].controller == 68 && at6000[0].value == 127
            && at6000[1].on && at6000[1].note == 61
            && ! at6000[2].on && at6000[2].controller < 0 && at6000[2].note == 60
            && at6000[3].controller == 68 && at6000[3].value == 0;
        check (order, "a slide sends CC 68 on, the next note-on, the old note-off, CC 68 off, on the same sample");

        bool noEarlyOff = true;
        for (const auto& e : offsOnly (events)) noEarlyOff = noEarlyOff && ! (e.note == 60 && e.sample < 6000);
        check (noEarlyOff, "the sliding note is not released at its gate");

        juce::int64 off64 = -1, off68 = -1, on69 = -1, off72 = -1, off73 = -1, on73 = -1, on74 = -1;
        int ccs = 0;
        for (const auto& e : events)
        {
            if (e.controller >= 0) { ++ccs; continue; }
            if (! e.on && e.note == 64) off64 = e.sample;
            if (! e.on && e.note == 68) off68 = e.sample;
            if (  e.on && e.note == 69) on69  = e.sample;
            if (! e.on && e.note == 72) off72 = e.sample;
            if (! e.on && e.note == 73) off73 = e.sample;
            if (  e.on && e.note == 73) on73  = e.sample;
            if (  e.on && e.note == 74) on74  = e.sample;
        }
        check (off64 == 5 * 6000, "a slide into a rest ends at the rest (a full-length note)");
        check (on69 == -1 && off68 == 9 * 6000 + 3000, "a slide onto the same pitch holds the note (no note-on) and ends with the gate");
        check (on73 == 13 * 6000 && off72 == 13 * 6000 && on74 == 14 * 6000 && off73 == 14 * 6000,
               "two slides in a row hand the note on twice");
        check (ccs == 2 * 3, "each slide to a new pitch is marked once (on and off), and nothing else is");

        StepSequencer a; load (a, p);
        StepSequencer b; load (b, p);
        check (sameEvents (run (a, (juce::int64) (8 * testSamplesPerBeat), settings, { 512 }),
                           run (b, (juce::int64) (8 * testSamplesPerBeat), settings, { 1, 6000, 37, 4096, 5999 })),
               "ties and slides give identical events whatever the block sizes");

        // Packing keeps tie and slide.
        bool packs = true;
        for (bool tie : { false, true })
            for (bool slide : { false, true })
            {
                const StepSequencer::Step st { true, 61, 99, tie, slide };
                packs = packs && StepSequencer::unpack (StepSequencer::pack (st)) == st;
            }
        check (packs, "pack/unpack round-trips tie and slide");
    }

    std::printf ("Scale lock (v1.11.0)\n");
    {
        using Scale = StepSequencer::Scale;
        const int expectedSize[StepSequencer::numScales] = { 12, 7, 7, 7, 7, 7, 7, 7, 7, 5, 5, 6, 6 };
        bool sizes = true, nearest = true, keepsInScale = true, directions = true;

        for (int sc = 0; sc < StepSequencer::numScales; ++sc)
        {
            const auto scale = (Scale) sc;
            int count = 0;
            for (int d = 0; d < 12; ++d) count += (StepSequencer::scaleMask (scale) >> d) & 1;
            sizes = sizes && count == expectedSize[sc] && StepSequencer::isInScale (0, 0, scale);

            for (int key = 0; key < 12; ++key)
                for (int note = 0; note < 128; ++note)
                {
                    const int snapped = StepSequencer::snapToScale (note, key, scale);
                    keepsInScale = keepsInScale && StepSequencer::isInScale (snapped, key, scale)
                                && (! StepSequencer::isInScale (note, key, scale) || snapped == note);

                    // Nothing in the scale is nearer, and a tie goes down.
                    for (int other = 0; other < 128; ++other)
                        if (StepSequencer::isInScale (other, key, scale))
                        {
                            const int a = std::abs (other - note), b = std::abs (snapped - note);
                            nearest = nearest && (a > b || (a == b && snapped <= other));
                        }

                    const int up = StepSequencer::snapToScale (note, key, scale, 1);
                    const int down = StepSequencer::snapToScale (note, key, scale, -1);
                    directions = directions && StepSequencer::isInScale (up, key, scale) && StepSequencer::isInScale (down, key, scale)
                              && (up >= note || note > 120) && (down <= note || note < 7);
                }
        }
        check (sizes, "the 13 scales have 12, 7 (x8), 5, 5, 6 and 6 notes, each with its key");
        check (keepsInScale, "a snapped note is in the scale, and a note already in it stays");
        check (nearest, "the snap picks the nearest scale note (the lower one when two are as near)");
        check (directions, "snapping up or down stays on that side, except at the ends of the range");

        check (StepSequencer::snapToScale (61, 0, Scale::major) == 60 && StepSequencer::snapToScale (66, 0, Scale::major) == 65
               && StepSequencer::snapToScale (61, 1, Scale::major) == 61 && StepSequencer::snapToScale (65, 9, Scale::minorPentatonic) == 64,
               "C# in C major is C, F# is F; C# in C# major stays; F in A minor pentatonic is E");

        bool moves = true;
        for (int n = 0; n < 128; ++n)
            moves = moves && StepSequencer::moveInScale (n, 0, Scale::off, 5) == juce::jmin (127, n + 5)
                          && StepSequencer::moveInScale (n, 0, Scale::off, -5) == juce::jmax (0, n - 5);
        check (moves, "with the scale off a move is in semitones, stopping at 0 and 127");
        check (StepSequencer::moveInScale (60, 0, Scale::major, 1) == 62 && StepSequencer::moveInScale (64, 0, Scale::major, 1) == 65
               && StepSequencer::moveInScale (60, 0, Scale::major, 7) == 72 && StepSequencer::moveInScale (61, 0, Scale::major, -1) == 60
               && StepSequencer::moveInScale (60, 0, Scale::majorPentatonic, -2) == 55,
               "moves in C major go C-D, E-F, seven degrees an octave; down from C# is C; pentatonic C down two is G");
    }

    std::printf ("Pattern tools (v1.11.0)\n");
    {
        juce::Random random (1122);
        bool shifts = true, reverses = true, octaves = true, others = true;

        for (int trial = 0; trial < 200; ++trial)
        {
            StepSequencer::Pattern p;
            for (auto& st : p)
                st = { random.nextBool(), random.nextInt (128), 1 + random.nextInt (127), random.nextBool(), random.nextBool() };
            const int length = 1 + random.nextInt (StepSequencer::numSteps);

            shifts = shifts && PatternTools::shiftRight (PatternTools::shiftLeft (p, length), length) == p
                            && PatternTools::shiftLeft (PatternTools::shiftRight (p, length), length) == p;
            auto turned = p;
            for (int i = 0; i < length; ++i) turned = PatternTools::shiftLeft (turned, length);
            shifts = shifts && turned == p;

            reverses = reverses && PatternTools::reverse (PatternTools::reverse (p, length), length) == p;

            const auto up = PatternTools::octaveUp (p, length);
            const auto down = PatternTools::octaveDown (up, length);
            bool fits = true;
            for (int i = 0; i < length; ++i) fits = fits && p[(size_t) i].note + 12 < 128;
            octaves = octaves && (fits ? (down == p && ! (up == p)) : up == p);

            const auto cleared = PatternTools::clear (p, length);
            for (int i = 0; i < StepSequencer::numSteps; ++i)
            {
                const auto& c = cleared[(size_t) i];
                const auto& o = p[(size_t) i];
                others = others && (i < length ? (! c.on && ! c.tie && ! c.slide && c.note == o.note && c.velocity == o.velocity) : c == o);
            }
            // Steps beyond the length are left alone by every tool.
            for (const auto& q : { PatternTools::shiftLeft (p, length), PatternTools::reverse (p, length), up })
                for (int i = length; i < StepSequencer::numSteps; ++i)
                    others = others && q[(size_t) i] == p[(size_t) i];
        }
        check (shifts, "shift left then right (and right then left) gives the pattern back; length shifts go round once");
        check (reverses, "reversing twice gives the pattern back");
        check (octaves, "octave up then down gives the pattern back, and does nothing if a note would leave 0..127");
        check (others, "clear turns the steps off but keeps their notes; no tool touches the steps beyond the length");

        // Ties move with their gap: 60~61 62 63 reversed is 63 62 61~60.
        auto p = allOnPattern();
        p[0].tie = true;
        const auto r = PatternTools::reverse (p, 4);
        check (r[0].note == 63 && r[2].note == 61 && r[2].tie && ! r[0].tie && ! r[3].tie,
               "reversing keeps a tie between the same two notes");
        const auto shifted = PatternTools::shiftRight (p, 4);
        check (shifted[1].tie && shifted[1].note == 60 && shifted[0].note == 63, "shifting moves a tie with its step");

        bool inScale = true, repeatable = true, varies = false;
        for (int key = 0; key < 12; ++key)
            for (int sc = 0; sc < StepSequencer::numScales; ++sc)
            {
                const auto scale = (StepSequencer::Scale) sc;
                const auto a = PatternTools::randomise (p, 12, key, scale, 77 + key);
                repeatable = repeatable && a == PatternTools::randomise (p, 12, key, scale, 77 + key);
                varies = varies || ! (a == PatternTools::randomise (p, 12, key, scale, 78 + key));
                for (int i = 0; i < 12; ++i)
                    inScale = inScale && StepSequencer::isInScale (a[(size_t) i].note, key, scale)
                                      && a[(size_t) i].note >= 48 && a[(size_t) i].note <= 71
                                      && a[(size_t) i].velocity >= 64 && ! a[(size_t) i].tie && ! a[(size_t) i].slide;
                for (int i = 12; i < 16; ++i)
                    inScale = inScale && a[(size_t) i] == p[(size_t) i];
            }
        check (inScale, "randomise keeps every note in the scale, in the key's octave from C3, and leaves the steps beyond the length");
        check (repeatable && varies, "randomise gives the same pattern for the same seed, another for another seed");
    }

    std::printf ("Undo (v1.11.0)\n");
    {
        PatternHistory history;
        auto pattern = allOnPattern();
        check (! history.canUndo(), "nothing to undo at first");

        std::vector<StepSequencer::Pattern> before;
        for (int edit = 0; edit < 40; ++edit)
        {
            history.remember (pattern);
            history.remember (pattern);   // remembered twice (two gestures that did nothing): stored once
            before.push_back (pattern);
            pattern[(size_t) (edit % 16)].note += 1;
        }
        check (history.size() == PatternHistory::maxLevels, "40 edits keep the newest 32 snapshots");

        bool backInOrder = true;
        int undone = 0;
        while (history.undo (pattern))
        {
            backInOrder = backInOrder && pattern == before[before.size() - 1 - (size_t) undone];
            ++undone;
        }
        check (backInOrder && undone == 32, "undo steps back through the last 32 patterns in order, then stops");

        PatternHistory skip;
        auto now = allOnPattern();
        auto older = now; older[3].on = false;
        skip.remember (older);
        skip.remember (now);   // remembered before a change that did not happen
        check (skip.undo (now) && now == older, "undo skips a snapshot equal to the pattern now");
    }

    std::printf ("ENTER (v1.11.0)\n");
    {
        NoteEntry entry;
        auto p = allOnPattern();
        for (auto& st : p) st.on = false;
        p[4].tie = true;

        check (entry.enter (p, 64, 90, 16, 0, StepSequencer::Scale::off) == NoteEntry::Result::ignored, "not armed: a note is ignored");
        entry.arm();
        check (entry.enter (p, 64, 90, 16, 0, StepSequencer::Scale::off) == NoteEntry::Result::ignored && ! p[0].on,
               "armed but no step clicked yet: ignored");

        entry.start (3);
        const auto r1 = entry.enter (p, 61, 90, 6, 0, StepSequencer::Scale::major);   // C# snapped to C
        const auto r2 = entry.enter (p, 67, 30, 6, 0, StepSequencer::Scale::major);
        check (r1 == NoteEntry::Result::written && r2 == NoteEntry::Result::written && entry.getCursor() == 5
               && p[3].on && p[3].note == 60 && p[3].velocity == 90 && p[4].note == 67 && p[4].velocity == 30 && p[4].tie,
               "each note writes the step (snapped, its velocity as the accent, the tie kept) and moves on");
        check (entry.enter (p, 62, 100, 6, 0, StepSequencer::Scale::major) == NoteEntry::Result::finished && ! entry.isArmed()
               && p[5].note == 62, "writing the last step of the length ends the entry");
        check (entry.enter (p, 62, 100, 6, 0, StepSequencer::Scale::major) == NoteEntry::Result::ignored && ! p[6].on,
               "after the end, notes are ignored");

        entry.arm(); entry.start (10);
        check (entry.enter (p, 70, 100, 8, 0, StepSequencer::Scale::off) == NoteEntry::Result::finished && ! p[10].on,
               "a cursor beyond a shortened length ends the entry without writing");
        entry.arm(); entry.start (0); entry.disarm();
        check (entry.enter (p, 70, 100, 16, 0, StepSequencer::Scale::off) == NoteEntry::Result::ignored, "disarming ends the entry");
    }

    std::printf ("Random settings (property test)\n");
    {
        // Seeded random patterns played through blocks of random sizes (1..2048 samples) while
        // swing, gate, rate, tempo and transpose change at random (now and then out of range),
        // triggers come and go as when keys are pressed and released in Transpose mode, and the
        // transport sometimes jumps or stops. Whatever happens:
        //   - every event lies inside its block, on channel 16, with a note in 0..127 (and a
        //     note-on velocity in 1..127);
        //   - a note-on never starts a note that is already sounding, and every note-off ends one
        //     that is;
        //   - each note ends no later than its gate length (as it was when it started);
        //   - stopping the transport releases everything.
        // v1.11.0: the length changes at random too, and every other run has random ties and
        // slides (step 1 never has either, so every tie ends within a pass of the pattern). In
        // those runs a note may last longer than its gate, but it still ends within a pass of
        // the longest possible steps, and every slide is marked by CC 68 on and off round exactly
        // one note-on and one note-off of another note, on the same sample.
        juce::Random random (20261118);
        juce::MidiBuffer midi;
        midi.ensureSize (8192);

        bool inBlock = true, inRange = true, paired = true, inTime = true, allReleased = true, slidesMarked = true;
        int noteOns = 0, slides = 0;

        for (int trial = 0; trial < 300; ++trial)
        {
            StepSequencer seq;
            seq.prepare (trial % 3 == 0 ? 44100.0 : testSampleRate);
            const double rate = trial % 3 == 0 ? 44100.0 : testSampleRate;
            const bool tiesAndSlides = trial % 2 == 1;   // v1.11.0
            for (int i = 0; i < StepSequencer::numSteps; ++i)
                seq.setStep (i, { random.nextBool(), random.nextInt (128), 1 + random.nextInt (127),
                                  tiesAndSlides && i > 0 && random.nextInt (3) == 0,
                                  tiesAndSlides && i > 0 && random.nextInt (3) == 0 });
            // The longest a held note can last: 16 steps of 1/8 at 20 BPM, plus swing and a gate.
            const auto longestHold = (juce::int64) (18.0 * 0.5 * 60.0 / 20.0 * rate);

            StepSequencer::Settings s;
            double bpm = 120.0, beat = random.nextDouble() * 8.0;
            bool playing = true;
            juce::int64 now = 0;
            juce::int64 mustEndBy[128] = {};
            bool sounding[128] = {};

            const auto randomise = [&random, &s, &bpm]
            {
                switch (random.nextInt (7))
                {
                    case 0:  s.swing = random.nextInt (10) == 0 ? random.nextFloat() * 3.0f - 1.0f : random.nextFloat(); break;
                    case 1:  s.gate  = random.nextInt (10) == 0 ? random.nextFloat() * 3.0f - 1.0f : random.nextFloat(); break;
                    case 2:  s.rate  = (StepSequencer::Rate) random.nextInt (5); break;
                    case 3:  bpm = 20.0 + random.nextDouble() * 380.0; break;
                    case 5:  s.length = random.nextInt (20) == 0 ? random.nextInt (40) - 10 : 1 + random.nextInt (16); break;   // v1.11.0, now and then out of range
                    case 4:  s.transpose = random.nextInt (49) - 24 + (random.nextInt (8) == 0 ? 24 * (random.nextInt (3) - 1) : 0); break;   // a key held, now and then far away
                    default: s.triggersEnabled = random.nextInt (4) != 0; break;   // keys released and pressed again
                }
            };

            for (int block = 0; block < 1000; ++block)
            {
                const int n = random.nextInt (8) == 0 ? 1 + random.nextInt (16) : 1 + random.nextInt (2048);
                for (int changes = random.nextInt (3); --changes >= 0;)
                    randomise();

                const int event = random.nextInt (100);
                if (event == 0)      playing = ! playing;                       // stop or start
                else if (event == 1) beat = random.nextDouble() * 16.0;         // a host loop or locate
                else if (event == 2) beat += (random.nextDouble() - 0.5) * 2.0 * StepSequencer::jumpToleranceSeconds * bpm / 60.0;   // small drift

                midi.clear();
                seq.process (midi, n, { playing, beat, bpm }, s);

                const double samplesPerBeat = rate * 60.0 / bpm;
                const double stepLength = StepSequencer::stepLengthInBeats (s.rate);
                const auto gateSamples = (juce::int64) juce::jmax (1L, std::lround ((double) juce::jlimit (0.05f, 1.0f, s.gate) * stepLength * samplesPerBeat));

                // v1.11.0: each slide mark is CC 68 = 127, a note-on, a note-off of another note, CC 68 = 0.
                {
                    std::vector<juce::MidiMessageMetadata> list;
                    for (const auto m : midi) list.push_back (m);
                    for (size_t i = 0; i < list.size(); ++i)
                    {
                        const auto msg = list[i].getMessage();
                        if (! msg.isController())
                            continue;
                        const bool opens = msg.getControllerNumber() == 68 && msg.getControllerValue() == 127 && i + 3 < list.size();
                        if (! opens) { slidesMarked = false; continue; }
                        const auto on = list[i + 1].getMessage(), off = list[i + 2].getMessage(), close = list[i + 3].getMessage();
                        slidesMarked = slidesMarked && on.isNoteOn() && off.isNoteOff() && on.getNoteNumber() != off.getNoteNumber()
                                    && close.isController() && close.getControllerNumber() == 68 && close.getControllerValue() == 0
                                    && list[i + 3].samplePosition == list[i].samplePosition;
                        ++slides;
                        i += 3;
                    }
                }

                for (const auto m : midi)
                {
                    const auto msg = m.getMessage();
                    const int note = msg.getNoteNumber();
                    const auto at = now + m.samplePosition;
                    inBlock = inBlock && m.samplePosition >= 0 && m.samplePosition < n;
                    if (msg.isController())
                    {
                        inRange = inRange && msg.getChannel() == StepSequencer::midiChannel && msg.getControllerNumber() == 68 && tiesAndSlides;
                        continue;
                    }
                    inRange = inRange && msg.getChannel() == StepSequencer::midiChannel && note >= 0 && note < 128
                                      && (msg.isNoteOn() || msg.isNoteOff());
                    if (! juce::isPositiveAndBelow (note, 128))
                        continue;

                    if (msg.isNoteOn())
                    {
                        ++noteOns;
                        inRange = inRange && msg.getVelocity() >= 1 && msg.getVelocity() <= 127;
                        paired = paired && ! sounding[note];
                        sounding[note] = true;
                        mustEndBy[note] = at + (tiesAndSlides ? longestHold : gateSamples);
                    }
                    else
                    {
                        paired = paired && sounding[note];
                        inTime = inTime && (! sounding[note] || at <= mustEndBy[note]);
                        sounding[note] = false;
                    }
                }

                // A note still sounding past its end is late too (its note-off went missing).
                for (int note = 0; note < 128; ++note)
                    inTime = inTime && ! (sounding[note] && mustEndBy[note] < now + n);

                if (playing)
                    beat += (double) n / samplesPerBeat;
                now += n;
            }

            midi.clear();
            seq.process (midi, 64, { false, beat, bpm }, s);   // transport stops
            for (const auto m : midi)
                if (m.getMessage().isNoteOff() && m.samplePosition == 0)
                    sounding[m.getMessage().getNoteNumber()] = false;
            for (bool b : sounding)
                allReleased = allReleased && ! b;
        }

        std::printf ("        (300 runs of 1000 blocks, %d note-ons, %d slides)\n", noteOns, slides);
        check (noteOns > 2000, "the random runs play plenty of notes");
        check (inBlock, "every event lies inside its block");
        check (inRange, "every event is a note-on or note-off on channel 16, note 0..127, velocity 1..127");
        check (paired, "no note starts while it is already sounding, and every note-off ends a sounding note");
        check (inTime, "every note ends no later than its gate length");
        check (allReleased, "stopping the transport releases every sounding note");
        check (slidesMarked && slides > 50, "v1.11.0: every slide is marked CC 68 on / note-on / note-off of the old note / CC 68 off, on one sample");
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
