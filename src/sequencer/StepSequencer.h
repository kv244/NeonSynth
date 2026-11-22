// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

/**
    @file StepSequencer.h

    A 16-step, sample-accurate MIDI step sequencer.

    How it fits into the plugin
    ---------------------------
    The sequencer produces MIDI, not audio. Each block, the processor asks it
    to write note-on / note-off events into a MidiBuffer at exact sample
    offsets, and that buffer is then handed to juce::Synthesiser exactly like
    keyboard input. Voice allocation, envelopes, stereo and the visualisers all
    work on sequenced notes for free.

    Timing model
    ------------
    Time is measured in beats (quarter notes). The caller describes each block
    with a Transport: the beat position at the first sample, the tempo, and
    whether time is running. Step k starts at beat k * stepLength, where
    stepLength depends on the Rate (0.25 beats for 1/16). Odd-numbered steps
    are delayed by the swing amount. Any step whose start falls inside
    [blockStart, blockEnd) is triggered on the first whole sample at or after
    its exact position:

        offset = ceil ((stepBeat - blockStart) * samplesPerBeat).

    Because step positions are derived from the transport every block (never
    accumulated), the pattern stays locked to the host's bar grid, and rate
    or tempo changes take effect on the next step without drift.

    Transport continuity
    --------------------
    Hosts don't advance their beat position perfectly: a tempo change in the
    middle of a block, a tempo ramp with large buffers, or rounding in the
    host can move the next block's start by a few samples from where this
    block predicted. Drift up to jumpToleranceSeconds (2 ms) either way is
    treated as continuous playback: nothing is released, and the last step
    that fired is remembered so a small backward drift never fires it twice
    and a small forward drift never skips one (a step that would fall just
    before the block fires at sample 0 instead). Anything larger is a jump
    (a loop, a locate): held notes are released and timing restarts from the
    new position.

    Note-offs
    ---------
    Every note-on schedules a note-off `gate * stepLength` later. Those can
    land in a later block, so pending note-offs are kept in a small fixed
    queue and counted down block by block. When time stops or jumps (a host
    loop, a locate, the sequencer being switched off), every pending note is
    released immediately so nothing hangs.

    Tie and slide (v1.11.0; the contract between the sequencer and the voices)
    ------------------------------------------------------------------------
    Each step carries two more flags, stored in the same atomic word as the rest. A tied step
    keeps its note sounding into the next step: the next step sends no note-on of its own and
    the note-off moves to the end of the last step in the tie. A slide step's note-off is sent
    just after the next step's note-on (both on channel 16), so for a moment both are held;
    a voice that receives a channel-16 note-on while its own channel-16 note is still held and
    marked as sliding glides to the new pitch at GLIDE time instead of restarting, whatever the
    keyboard's mode. With GLIDE at 0 a slide is a plain legato step. Absent flags (every session
    saved before v1.11.0) mean off, so old patterns play exactly as before.

    How the sequencer keeps it, exactly (agent A, v1.11.0):
      - "Marked as sliding" is said in the MIDI itself, so it lands on the right sample: a
        slide is sent as four events on channel 16 at the same sample, in this order:
            CC 68 (legato footswitch) = 127, note-on (new note), note-off (old note), CC 68 = 0.
        So a voice sees the note-on while its old note is held and channel 16's legato
        footswitch is down; that is the mark. Nothing else ever sends CC 68 on channel 16
        (keyboard input is moved to channel 1), and juce::Synthesiser does nothing with it.
      - The tied (or sliding) note is held until the next step that fires, however long that
        is (swing, a tempo or rate change). A tie then ends `gate` after the start of the last
        step in the tie; a tie onto a step that is itself tied (or sliding) carries on. The
        next step's own on / note / velocity are ignored while a tie holds over it.
      - A slide onto a step that is off (or out of range) ends the held note at that step's
        start, as a full-length note. A slide onto the same pitch is a tie: no note-on.
      - A step with both flags is a tie (then the step the tie ends on may slide).
      - With triggers disabled (Transpose, no key held) a held note ends at the next step.
      - Stopping, a jump in time, or switching the sequencer off releases everything at once.

    Thread safety
    -------------
    process() runs on the audio thread and never allocates or locks. The
    pattern is stored as one std::atomic<uint32_t> per step, so the GUI can
    edit steps (setStep) while the audio thread reads them (getStep). The
    current playhead position is published the same way for the editor.

    Notes are sent on MIDI channel 16 so they can never cancel a note of the
    same pitch played on the keyboard (juce::Synthesiser matches note-offs by
    channel as well as note number). The processor moves all keyboard input to
    channel 1, so this holds whatever channel the keyboard sends on.
*/
class StepSequencer
{
public:
    static constexpr int numSteps    = 16;
    static constexpr int midiChannel = 16;

    /** Transport drift up to this much (either way) is not treated as a jump. */
    static constexpr double jumpToleranceSeconds = 0.002;

    //==========================================================================
    /** One step of the pattern. */
    struct Step
    {
        bool on       = false;   ///< whether the step plays
        int  note     = 60;      ///< MIDI note number, 0..127 (kept while the step is off)
        int  velocity = 100;     ///< MIDI velocity, 1..127
        bool tie      = false;   ///< v1.11.0: held into the next step, which does not retrigger
        bool slide    = false;   ///< v1.11.0: overlaps the next step, so the voice glides to its note

        bool operator== (const Step& other) const noexcept
        {
            return on == other.on && note == other.note && velocity == other.velocity
                && tie == other.tie && slide == other.slide;
        }
    };

    /** Step length choices, as offered by the Rate parameter (same order). */
    enum class Rate { eighth, sixteenth, thirtySecond, eighthTriplet, sixteenthTriplet };

    /** Length of one step in beats (quarter notes) for a given rate. */
    static double stepLengthInBeats (Rate rate) noexcept
    {
        switch (rate)
        {
            case Rate::eighth:           return 0.5;
            case Rate::thirtySecond:     return 0.125;
            case Rate::eighthTriplet:    return 1.0 / 3.0;
            case Rate::sixteenthTriplet: return 1.0 / 6.0;
            case Rate::sixteenth:
            default:                     return 0.25;
        }
    }

    /** Where we are in musical time at the first sample of a block. */
    struct Transport
    {
        bool   playing = false;   ///< false = stopped: release everything, trigger nothing
        double beat    = 0.0;     ///< position in quarter notes at sample 0 of the block
        double bpm     = 120.0;   ///< tempo, quarter notes per minute
    };

    /** Per-block playback settings, read from parameters by the processor. */
    struct Settings
    {
        Rate  rate      = Rate::sixteenth;
        float swing     = 0.0f;    ///< 0..1: delays odd steps by up to half a step (0.33 is a triplet feel)
        float gate      = 0.5f;    ///< 0.05..1: note length as a fraction of one step
        int   transpose = 0;       ///< semitones added to every step's note
        bool  triggersEnabled = true;  ///< false = keep time but start no new notes (Transpose mode, no key held)
        int   length    = numSteps;  ///< v1.11.0: 1..16, the pattern repeats after this many steps (16 = as before)
    };

    //==========================================================================
    StepSequencer()
    {
        for (auto& s : steps)
            s.store (pack ({}), std::memory_order_relaxed);
    }

    /** Call from prepareToPlay(). Clears any pending notes without emitting them. */
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        numPending = 0;
        carry = Carry::none;
        wasPlaying = false;
        lastFiredBeat = noStepFired;
        currentStep.store (-1, std::memory_order_relaxed);
    }

    //==========================================================================
    /** Pattern editing. Safe to call from any thread (lock-free). */
    void setStep (int index, const Step& step) noexcept
    {
        if (juce::isPositiveAndBelow (index, numSteps))
            steps[(size_t) index].store (pack (step), std::memory_order_relaxed);
    }

    /** Returns a step of the pattern. Safe to call from any thread. */
    Step getStep (int index) const noexcept
    {
        return juce::isPositiveAndBelow (index, numSteps)
                 ? unpack (steps[(size_t) index].load (std::memory_order_relaxed))
                 : Step {};
    }

    /** v1.11.0: all 16 steps at once, for the editor's tools and undo. Message thread. */
    using Pattern = std::array<Step, numSteps>;

    Pattern getPattern() const noexcept
    {
        Pattern p;
        for (int i = 0; i < numSteps; ++i)
            p[(size_t) i] = getStep (i);
        return p;
    }

    /** Index of the step that most recently started, or -1 while stopped. For the GUI playhead. */
    int getCurrentStep() const noexcept   { return currentStep.load (std::memory_order_relaxed); }

    //==========================================================================
    /**
        Writes this block's sequencer events into `midi` (appending to what is
        already there). Audio thread; does not allocate as long as `midi` has
        been given enough capacity with MidiBuffer::ensureSize().

        @param midi        destination buffer, events are added at sample offsets 0..numSamples-1
        @param numSamples  length of the block
        @param transport   musical time at the start of the block
        @param settings    rate, swing, gate, transpose for this block
    */
    void process (juce::MidiBuffer& midi, int numSamples,
                  const Transport& transport, const Settings& settings) noexcept
    {
        if (numSamples <= 0)
            return;

        // Stopped: release whatever is sounding and park the playhead. A tempo or position that is
        // not a finite number (a broken session, a host bug) counts as stopped too: written as
        // !(bpm > 0) so NaN lands here, since the step loop below would never end on it.
        if (! transport.playing || ! (transport.bpm > 0.0) || ! std::isfinite (transport.bpm) || ! std::isfinite (transport.beat))
        {
            releaseAll (midi, 0);
            wasPlaying = false;
            lastFiredBeat = noStepFired;
            currentStep.store (-1, std::memory_order_relaxed);
            return;
        }

        const double samplesPerBeat = sampleRate * 60.0 / transport.bpm;
        const double blockStart     = transport.beat;
        const double blockEnd       = blockStart + (double) numSamples / samplesPerBeat;

        // Continuous playback, or a jump in time? Small drift is tolerated (see
        // "Transport continuity" above); a real jump (host loop, locate) releases
        // held notes so nothing hangs across the discontinuity. Just starting is
        // neither: there is nothing to release and no step has fired yet.
        const double toleranceSamples = jumpToleranceSeconds * sampleRate;
        const bool   continuous = wasPlaying
                               && std::abs (blockStart - expectedBeat) * samplesPerBeat <= toleranceSamples;

        if (! continuous)
        {
            if (wasPlaying)
                releaseAll (midi, 0);
            lastFiredBeat = noStepFired;
            carry = Carry::none;
        }

        wasPlaying   = true;
        expectedBeat = blockEnd;

        const double stepLength  = stepLengthInBeats (settings.rate);
        const double swingDelay  = (double) juce::jlimit (0.0f, 1.0f, settings.swing) * 0.5 * stepLength;
        const int    gateSamples = juce::jmax (1, (int) std::lround ((double) juce::jlimit (0.05f, 1.0f, settings.gate)
                                                                     * stepLength * samplesPerBeat));

        // Candidate steps: start early enough to catch a swung odd step, or a step
        // skipped by forward drift, from just before the block; stop once a step
        // starts after the block.
        //
        // A step fires on the first whole sample at or after its exact position:
        // offset = ceil (distance in samples). Deciding on integer offsets (rather
        // than comparing beats) means every step lands in exactly one block and on
        // the same absolute sample whatever the block sizes are; the small epsilon
        // absorbs floating-point error such as 5999.9999999 for 6000.
        constexpr double epsilon = 1.0e-6;

        const double firstCandidate = blockStart - (continuous ? toleranceSamples / samplesPerBeat : 0.0);

        for (auto k = (juce::int64) std::floor (firstCandidate / stepLength) - 1; ; ++k)
        {
            const bool   odd      = (k & 1) != 0;
            const double stepBeat = (double) k * stepLength + (odd ? swingDelay : 0.0);
            const double distance = (stepBeat - blockStart) * samplesPerBeat;   // in samples, may be fractional
            auto         offset   = (juce::int64) std::ceil (distance - epsilon);

            if (offset >= numSamples)
                break;

            if (continuous)
            {
                // Already fired (it can reappear after a small backward drift)?
                if (stepBeat <= lastFiredBeat + epsilon / samplesPerBeat)
                    continue;
                // Skipped by a small forward drift: fire it now rather than never.
                if (offset < 0)
                {
                    if ((double) -offset > toleranceSamples)
                        continue;
                    offset = 0;
                }
            }
            else if (offset < 0)
            {
                continue;   // started (or jumped) after this step: don't play it late
            }

            lastFiredBeat = stepBeat;

            // v1.11.0: the pattern repeats after `length` steps; at 16 this is the same index as before.
            const int length = juce::jlimit (1, numSteps, settings.length);
            const int index  = (int) (((k % length) + length) % length);
            currentStep.store (index, std::memory_order_relaxed);

            const auto step = getStep (index);
            const int  note = step.note + settings.transpose;

            // v1.11.0: a tie or slide from the step before decides this step (see the contract
            // above). Without ties and slides `carry` is always none and this is skipped.
            if (carry != Carry::none && continueCarry (midi, step, note, settings.triggersEnabled, (int) offset, gateSamples))
                continue;

            if (step.on && settings.triggersEnabled && juce::isPositiveAndBelow (note, 128))
            {
                startNote (midi, note, step.velocity, (int) offset, gateSamples);
                if (step.tie || step.slide)
                    holdNewest (step);
            }
        }

        advancePendingNoteOffs (midi, numSamples);
    }

    /** Releases every sounding sequencer note at the given offset (e.g. when switched off). */
    void releaseAll (juce::MidiBuffer& midi, int offset) noexcept
    {
        for (int i = 0; i < numPending; ++i)
            midi.addEvent (juce::MidiMessage::noteOff (midiChannel, pending[(size_t) i].note), offset);
        numPending = 0;
        carry = Carry::none;
    }

    //==========================================================================
    /** v1.11.0: the scale lock (the Sequencer Scale parameter, same order; final once shipped). */
    enum class Scale { off, major, dorian, phrygian, lydian, mixolydian, naturalMinor, locrian,
                       harmonicMinor, majorPentatonic, minorPentatonic, blues, wholeTone };

    static constexpr int numScales = 13;

    /** The scale's notes as a 12-bit mask, bit i = i semitones above the key. Off = every note. */
    static int scaleMask (Scale scale) noexcept
    {
        constexpr auto bits = [] (std::initializer_list<int> degrees)
        {
            int mask = 0;
            for (int d : degrees)
                mask |= 1 << d;
            return mask;
        };

        switch (scale)
        {
            case Scale::major:           return bits ({ 0, 2, 4, 5, 7, 9, 11 });
            case Scale::dorian:          return bits ({ 0, 2, 3, 5, 7, 9, 10 });
            case Scale::phrygian:        return bits ({ 0, 1, 3, 5, 7, 8, 10 });
            case Scale::lydian:          return bits ({ 0, 2, 4, 6, 7, 9, 11 });
            case Scale::mixolydian:      return bits ({ 0, 2, 4, 5, 7, 9, 10 });
            case Scale::naturalMinor:    return bits ({ 0, 2, 3, 5, 7, 8, 10 });
            case Scale::locrian:         return bits ({ 0, 1, 3, 5, 6, 8, 10 });
            case Scale::harmonicMinor:   return bits ({ 0, 2, 3, 5, 7, 8, 11 });
            case Scale::majorPentatonic: return bits ({ 0, 2, 4, 7, 9 });
            case Scale::minorPentatonic: return bits ({ 0, 3, 5, 7, 10 });
            case Scale::blues:           return bits ({ 0, 3, 5, 6, 7, 10 });
            case Scale::wholeTone:       return bits ({ 0, 2, 4, 6, 8, 10 });
            case Scale::off:
            default:                     return 0xfff;
        }
    }

    /** True if `note` belongs to the scale on `key` (0 = C .. 11 = B). */
    static bool isInScale (int note, int key, Scale scale) noexcept
    {
        const int degree = (((note - key) % 12) + 12) % 12;
        return (scaleMask (scale) & (1 << degree)) != 0;
    }

    /** The scale note nearest `note` (0..127): with direction > 0 the nearest at or above,
        with direction < 0 at or below, with 0 the nearest either way (the lower one when two
        are as near). Used when a note is SET on a step; stored notes are never changed by
        the lock. If there is none that way inside 0..127, the search turns round. */
    static int snapToScale (int note, int key, Scale scale, int direction = 0) noexcept
    {
        note = juce::jlimit (0, 127, note);
        if (isInScale (note, key, scale))
            return note;

        const auto fits = [key, scale] (int n) { return juce::isPositiveAndBelow (n, 128) && isInScale (n, key, scale); };

        for (int d = 1; d < 12; ++d)
        {
            if (direction <= 0 && fits (note - d)) return note - d;
            if (direction >= 0 && fits (note + d)) return note + d;
        }
        for (int d = 1; d < 12; ++d)   // nothing that way: the other way
        {
            if (fits (note + d)) return note + d;
            if (fits (note - d)) return note - d;
        }
        return note;
    }

    /** `note` moved by `degrees` scale notes (semitones with the scale off), within 0..127.
        Dragging a step and the mouse wheel use this, so a locked step only ever lands in the scale. */
    static int moveInScale (int note, int key, Scale scale, int degrees) noexcept
    {
        const int direction = degrees > 0 ? 1 : -1;
        for (int i = 0; i < std::abs (degrees); ++i)
        {
            const int next = snapToScale (note + direction, key, scale, direction);
            if ((next - note) * direction <= 0)
                break;   // at the end of the range
            note = next;
        }
        return note;
    }

    //==========================================================================
    /** Packs a step into 32 bits: bit 16 = on, bits 8-15 = velocity, bits 0-7 = note. */
    static juce::uint32 pack (const Step& s) noexcept
    {
        return (s.on ? 0x10000u : 0u)
             | (s.tie   ? 0x20000u : 0u)
             | (s.slide ? 0x40000u : 0u)
             | ((juce::uint32) juce::jlimit (1, 127, s.velocity) << 8)
             |  (juce::uint32) juce::jlimit (0, 127, s.note);
    }

    /** Inverse of pack(). */
    static Step unpack (juce::uint32 bits) noexcept
    {
        Step s;
        s.on       = (bits & 0x10000u) != 0;
        s.tie      = (bits & 0x20000u) != 0;
        s.slide    = (bits & 0x40000u) != 0;
        s.velocity = (int) ((bits >> 8) & 0x7fu);
        s.note     = (int) (bits & 0x7fu);
        return s;
    }

private:
    /** A note-off that is due `samplesLeft` samples after the start of the current block. */
    struct PendingNoteOff
    {
        int  note = 0;
        int  samplesLeft = 0;
        bool held = false;   ///< v1.11.0: tied or sliding, held until the next step fires (samplesLeft unused)
    };

    /** v1.11.0: what the last step that fired hands to the next one. */
    enum class Carry { none, tie, slide };

    /** The CC that marks a slide (the legato footswitch); see the contract above. */
    static constexpr int slideController = 68;

    /** v1.11.0: the note just started is tied or sliding, so it is held until the next step. */
    void holdNewest (const Step& step) noexcept
    {
        auto& p = pending[(size_t) (numPending - 1)];
        p.held = true;
        carry = step.tie ? Carry::tie : Carry::slide;
        carryNote = p.note;
    }

    /** Index of the held note `carryNote` in the pending queue, or -1 (released meanwhile). */
    int findCarried() const noexcept
    {
        for (int i = 0; i < numPending; ++i)
            if (pending[(size_t) i].held && pending[(size_t) i].note == carryNote)
                return i;
        return -1;
    }

    /** v1.11.0: a step fires while the step before tied or slid into it. Returns true when it
        is dealt with; false to play it as an ordinary step (the held note has gone already). */
    bool continueCarry (juce::MidiBuffer& midi, const Step& step, int note, bool triggersEnabled,
                        int offset, int gateSamples) noexcept
    {
        const auto previous = carry;
        carry = Carry::none;

        const int held = findCarried();
        if (held < 0)
            return false;

        const auto endHeld = [&] (int i)
        {
            midi.addEvent (juce::MidiMessage::noteOff (midiChannel, pending[(size_t) i].note), offset);
            removePending (i);
        };

        if (! triggersEnabled)
        {
            endHeld (held);
            return true;
        }

        const bool sounds = step.on && juce::isPositiveAndBelow (note, 128);
        const bool tiesOn = previous == Carry::tie || (sounds && note == carryNote);   // a slide to the same pitch is a tie

        if (tiesOn)
        {
            // The held note becomes this step's note: no note-on. It carries on if this step
            // ties or slides too, otherwise it ends `gate` after this step's start.
            auto& p = pending[(size_t) held];
            if (step.tie || step.slide)
            {
                carry = step.tie ? Carry::tie : Carry::slide;
            }
            else
            {
                p.held = false;
                p.samplesLeft = offset + gateSamples;
            }
            return true;
        }

        if (! sounds)
        {
            endHeld (held);   // a slide into a rest: a full-length note
            return true;
        }

        // Slide: CC 68 on, the new note-on, the old note-off, CC 68 off, all at this sample.
        const int oldNote = pending[(size_t) held].note;
        removePending (held);
        midi.addEvent (juce::MidiMessage::controllerEvent (midiChannel, slideController, 127), offset);
        startNote (midi, note, step.velocity, offset, gateSamples);
        midi.addEvent (juce::MidiMessage::noteOff (midiChannel, oldNote), offset);
        midi.addEvent (juce::MidiMessage::controllerEvent (midiChannel, slideController, 0), offset);

        if (step.tie || step.slide)
            holdNewest (step);
        return true;
    }

    /** Emits a note-on, first releasing the same note if it is still sounding. */
    void startNote (juce::MidiBuffer& midi, int note, int velocity, int offset, int gateSamples) noexcept
    {
        for (int i = 0; i < numPending; ++i)
        {
            if (pending[(size_t) i].note == note)
            {
                midi.addEvent (juce::MidiMessage::noteOff (midiChannel, note),
                               juce::jmin (offset, pending[(size_t) i].samplesLeft));
                removePending (i);
                break;
            }
        }

        midi.addEvent (juce::MidiMessage::noteOn (midiChannel, note, (juce::uint8) juce::jlimit (1, 127, velocity)), offset);

        if (numPending == (int) pending.size())   // queue full: release the oldest note now
        {
            midi.addEvent (juce::MidiMessage::noteOff (midiChannel, pending[0].note), offset);
            removePending (0);
        }
        pending[(size_t) numPending++] = { note, offset + gateSamples };
    }

    /** Emits the note-offs that fall inside this block and counts the rest down. */
    void advancePendingNoteOffs (juce::MidiBuffer& midi, int numSamples) noexcept
    {
        for (int i = 0; i < numPending;)
        {
            auto& p = pending[(size_t) i];
            if (p.held)
            {
                ++i;   // v1.11.0: tied or sliding, ended by the next step
            }
            else if (p.samplesLeft < numSamples)
            {
                midi.addEvent (juce::MidiMessage::noteOff (midiChannel, p.note), p.samplesLeft);
                removePending (i);
            }
            else
            {
                p.samplesLeft -= numSamples;
                ++i;
            }
        }
    }

    void removePending (int i) noexcept
    {
        pending[(size_t) i] = pending[(size_t) (numPending - 1)];
        --numPending;
    }

    std::array<std::atomic<juce::uint32>, numSteps> steps;
    std::atomic<int> currentStep { -1 };

    std::array<PendingNoteOff, 32> pending {};
    int    numPending   = 0;
    Carry  carry        = Carry::none;   ///< v1.11.0: a tie or slide from the last step that fired
    int    carryNote    = 0;             ///< ...and the note it holds
    double sampleRate   = 44100.0;
    double expectedBeat = 0.0;
    bool   wasPlaying   = false;

    static constexpr double noStepFired = -1.0e300;
    double lastFiredBeat = noStepFired;   ///< beat of the most recent step that fired, while playing continuously
};

//==============================================================================
/**
    v1.11.0: the pattern tools behind the TOOLS menu, as pure functions on a pattern (message
    thread; nothing here touches the audio thread). Each acts on the first `length` steps, the
    ones that play: LENGTH shortens a pattern within its 16 steps, and the steps beyond it are
    kept as they are for when the pattern is made longer again.

    Ties and slides belong to the gap after their step, so the tools move them with that gap:
    shifting moves them with their steps, and reversing moves the flag of the gap between steps
    i and i + 1 to the step before the mirrored gap (the gap from the last step round to the
    first stays where it is).
*/
namespace PatternTools
{
    using Step    = StepSequencer::Step;
    using Pattern = StepSequencer::Pattern;
    using Scale   = StepSequencer::Scale;

    inline int clampLength (int length) noexcept   { return juce::jlimit (1, StepSequencer::numSteps, length); }

    /** Every step one place earlier; the first becomes the last. */
    inline Pattern shiftLeft (Pattern p, int length) noexcept
    {
        length = clampLength (length);
        std::rotate (p.begin(), p.begin() + 1, p.begin() + length);
        return p;
    }

    /** Every step one place later; the last becomes the first. */
    inline Pattern shiftRight (Pattern p, int length) noexcept
    {
        length = clampLength (length);
        std::rotate (p.begin(), p.begin() + (length - 1), p.begin() + length);
        return p;
    }

    /** Every note `semitones` higher (lower if negative), or the pattern unchanged if any step's
        note would leave 0..127, so octave up then down always gives the pattern back. */
    inline Pattern transpose (Pattern p, int length, int semitones) noexcept
    {
        length = clampLength (length);
        for (int i = 0; i < length; ++i)
            if (! juce::isPositiveAndBelow (p[(size_t) i].note + semitones, 128))
                return p;
        for (int i = 0; i < length; ++i)
            p[(size_t) i].note += semitones;
        return p;
    }

    inline Pattern octaveUp   (const Pattern& p, int length) noexcept   { return transpose (p, length, 12); }
    inline Pattern octaveDown (const Pattern& p, int length) noexcept   { return transpose (p, length, -12); }

    /** The steps backwards, with each tie and slide kept on the gap between the same two notes. */
    inline Pattern reverse (const Pattern& p, int length) noexcept
    {
        length = clampLength (length);
        Pattern r = p;
        for (int j = 0; j < length; ++j)
        {
            const auto& from = p[(size_t) (length - 1 - j)];
            const auto& gap  = p[(size_t) (j == length - 1 ? length - 1 : length - 2 - j)];
            r[(size_t) j] = { from.on, from.note, from.velocity, gap.tie, gap.slide };
        }
        return r;
    }

    /** Every step off, with no ties or slides; notes and velocities stay, for when a step is clicked on. */
    inline Pattern clear (Pattern p, int length) noexcept
    {
        length = clampLength (length);
        for (int i = 0; i < length; ++i)
        {
            auto& s = p[(size_t) i];
            s.on = s.tie = s.slide = false;
        }
        return p;
    }

    /** A new pattern in the scale (chromatic with the scale off): about three steps in four on,
        notes from the key's octave starting at C3 up to the key an octave higher, velocities
        64..127, no ties or slides. The same seed always gives the same pattern. */
    inline Pattern randomise (Pattern p, int length, int key, Scale scale, juce::int64 seed) noexcept
    {
        length = clampLength (length);
        key = juce::jlimit (0, 11, key);
        juce::Random random (seed);

        const int lowest = 48 + key;
        int notes[13] = {};
        int numNotes = 0;
        for (int n = lowest; n <= lowest + 12; ++n)
            if (StepSequencer::isInScale (n, key, scale))
                notes[numNotes++] = n;

        for (int i = 0; i < length; ++i)
        {
            auto& s    = p[(size_t) i];
            s.on       = random.nextInt (4) != 0;
            s.note     = notes[random.nextInt (numNotes)];
            s.velocity = 64 + random.nextInt (64);
            s.tie = s.slide = false;
        }
        return p;
    }
}

//==============================================================================
/**
    v1.11.0: pattern undo, 32 levels of whole-pattern snapshots (message thread only).

    remember() takes the pattern as it is just BEFORE a change; undo() hands back the newest
    snapshot that differs from the pattern now. A snapshot equal to the one before it is not
    stored twice, and the 33rd drops the oldest.
*/
class PatternHistory
{
public:
    static constexpr int maxLevels = 32;
    using Pattern = StepSequencer::Pattern;

    void remember (const Pattern& before) noexcept
    {
        if (count > 0 && snapshots[(size_t) newest()] == before)
            return;
        if (count == maxLevels)
        {
            first = (first + 1) % maxLevels;
            --count;
        }
        ++count;
        snapshots[(size_t) newest()] = before;
    }

    bool canUndo() const noexcept   { return count > 0; }
    int  size() const noexcept      { return count; }
    void clear() noexcept           { first = count = 0; }

    /** Sets `pattern` to the newest snapshot that differs from it; false if there is none. */
    bool undo (Pattern& pattern) noexcept
    {
        while (count > 0)
        {
            const auto snapshot = snapshots[(size_t) newest()];
            --count;
            if (! (snapshot == pattern))
            {
                pattern = snapshot;
                return true;
            }
        }
        return false;
    }

private:
    int newest() const noexcept   { return (first + count - 1) % maxLevels; }

    std::array<Pattern, maxLevels> snapshots {};
    int first = 0, count = 0;
};

//==============================================================================
/**
    v1.11.0: ENTER, step entry from the keyboard (message thread only; the editor's pattern row
    drives it with the notes NeonSynthProcessor::popEnteredNote() hands over).

    arm(), then start (step) when a step is clicked; until then played notes are ignored. Each
    note then writes the step under the cursor (on, the note snapped to the scale lock, the
    velocity as played, which is the step's accent; its tie and slide stay) and moves the cursor
    on. Writing the last step of the pattern's length ends the entry, as does disarm().
*/
class NoteEntry
{
public:
    enum class Result { ignored, written, finished };

    void arm() noexcept      { armed = true;  cursor = -1; }
    void disarm() noexcept   { armed = false; cursor = -1; }

    bool isArmed() const noexcept   { return armed; }
    int  getCursor() const noexcept { return cursor; }   ///< the step the next note writes, or -1

    /** A step was clicked while armed: entry starts (or restarts) there. */
    void start (int step) noexcept
    {
        if (armed && juce::isPositiveAndBelow (step, StepSequencer::numSteps))
            cursor = step;
    }

    /** One played note. `finished` means the last step was written and the entry has ended. */
    Result enter (StepSequencer::Pattern& pattern, int note, int velocity, int length,
                  int key, StepSequencer::Scale scale) noexcept
    {
        length = juce::jlimit (1, StepSequencer::numSteps, length);
        if (! armed || cursor < 0)
            return Result::ignored;
        if (cursor >= length)   // LENGTH was shortened under the cursor
        {
            disarm();
            return Result::finished;
        }

        auto& s    = pattern[(size_t) cursor];
        s.on       = true;
        s.note     = StepSequencer::snapToScale (note, key, scale);
        s.velocity = juce::jlimit (1, 127, velocity);

        if (++cursor >= length)
        {
            disarm();
            return Result::finished;
        }
        return Result::written;
    }

private:
    bool armed  = false;
    int  cursor = -1;
};
