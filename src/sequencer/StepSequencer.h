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

    Gate and swing
    --------------
    The gate is a fraction of the nominal step (stepLength), not of the time
    until the next step actually starts. Swing moves only where odd steps
    start, so an odd step's note still lasts gate x stepLength: with a long
    gate it runs on past the start of the following (even, unswung) step, by
    up to (gate + swingDelay / stepLength - 1) x stepLength. At gate 1 and
    full swing that is half a step of overlap. On different pitches both notes
    sound together for that time (two voices); on the same pitch the new
    note-on first ends the old note (releaseSamePitch), since
    juce::Synthesiser would otherwise end the new note with the old one's
    note-off. Even steps are never delayed, so they never overlap the next
    (odd) step, and with no swing gate 1 is legato (to within a sample of
    rounding). Ties and slides are separate: their notes are held until the
    next step fires, whatever the gate.

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

    The contract, member by member (v1.11.1):
      - prepare(), process(), releaseAll(): the audio thread only (prepare()
        from prepareToPlay(), which a host never runs at the same time as
        processBlock()). They share the note-off queue and the timing state,
        which are plain members: never call them from two threads.
      - setStep(), getStep(), getCurrentStep(): any thread, at any time. Each
        step is one atomic word, so a reader sees a step either before or after
        an edit, never half of one; edits to several steps are not one atomic
        change (the audio thread may play a mix of old and new steps for one
        block). Relaxed ordering is enough: a step carries no other data.
      - The static helpers (stepLengthInBeats, the scale functions) touch no
        state and are safe anywhere.

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
    static constexpr int slideController = 68;   ///< CC 68, the Legato Footswitch: marks a slide (see the contract above)

    /** Transport drift up to this much (either way) is not treated as a jump. */
    static constexpr double jumpToleranceSeconds = 0.002;

    /** MIDI's note numbers are 0..127, and so are velocities and controller values. */
    static constexpr int numMidiNotes = 128;
    static constexpr int maxMidiValue = 127;

    /** The gate's range is 5%..100% of a step; 50% is the default, and what a broken value falls back to. */
    static constexpr float minGate = 0.05f;
    static constexpr float defaultGate = 0.5f;

    /** The rate assumed until prepare() gives the real one (or when it gives none). */
    static constexpr double placeholderSampleRate = 44100.0;

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
        float swing     = 0.0f;    ///< 0..1: delays odd steps by up to half a step (about 0.67 is a triplet feel)
        float gate      = defaultGate;    ///< 0.05..1: note length as a fraction of one nominal step (see "Gate and swing")
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
        sampleRate = newSampleRate > 0.0 ? newSampleRate : placeholderSampleRate;
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

    /** v1.11.0: all 16 steps at once, as the pattern tools and undo work on them. */
    using Pattern = std::array<Step, numSteps>;

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
                  const Transport& transport, const Settings& settings) noexcept;

    /** Releases every sounding sequencer note at the given offset (e.g. when switched off). */
    void releaseAll (juce::MidiBuffer& midi, int offset) noexcept;

    //==========================================================================
    /** v1.11.0: the scale lock (the Sequencer Scale parameter, same order; final once shipped). */
    enum class Scale { off, major, dorian, phrygian, lydian, mixolydian, naturalMinor, locrian,
                       harmonicMinor, majorPentatonic, minorPentatonic, blues, wholeTone };

    static constexpr int numScales = 13;

    /** The scale's notes as a 12-bit mask, bit i = i semitones above the key. Off = every note. */
    static int scaleMask (Scale scale) noexcept;

    /** True if `note` belongs to the scale on `key` (0 = C .. 11 = B). */
    static bool isInScale (int note, int key, Scale scale) noexcept;

    /** The scale note nearest `note` (0..127): with direction > 0 the nearest at or above,
        with direction < 0 at or below, with 0 the nearest either way (the lower one when two
        are as near). Used when a note is SET on a step; stored notes are never changed by
        the lock. If there is none that way inside 0..127, the search turns round. */
    static int snapToScale (int note, int key, Scale scale, int direction = 0) noexcept;

    /** `note` moved by `degrees` scale notes (semitones with the scale off), within 0..127.
        Dragging a step and the mouse wheel use this, so a locked step only ever lands in the scale. */
    static int moveInScale (int note, int key, Scale scale, int degrees) noexcept;

    //==========================================================================
    /** Packs a step into 32 bits: bits 0-7 = note (0-127), bits 8-14 = velocity (1-127, bit 15
        unused), bit 16 = on, bit 17 = tie, bit 18 = slide (the last two since v1.11.0). */
    static juce::uint32 pack (const Step& s) noexcept
    {
        return (s.on ? onBit : 0u)
             | (s.tie   ? tieBit : 0u)
             | (s.slide ? slideBit : 0u)
             | ((juce::uint32) juce::jlimit (1, maxMidiValue, s.velocity) << velocityShift)
             |  (juce::uint32) juce::jlimit (0, maxMidiValue, s.note);
    }

    /** Inverse of pack(). */
    static Step unpack (juce::uint32 bits) noexcept
    {
        Step s;
        s.on       = (bits & onBit) != 0;
        s.tie      = (bits & tieBit) != 0;
        s.slide    = (bits & slideBit) != 0;
        s.velocity = (int) ((bits >> velocityShift) & sevenBits);
        s.note     = (int) (bits & sevenBits);
        return s;
    }

    // The packed step's layout (see pack()).
    static constexpr juce::uint32 onBit     = 0x10000u;   // bit 16
    static constexpr juce::uint32 tieBit    = 0x20000u;   // bit 17
    static constexpr juce::uint32 slideBit  = 0x40000u;   // bit 18
    static constexpr juce::uint32 sevenBits = 0x7fu;      // a note or a velocity
    static constexpr int velocityShift = 8;               // the velocity's lowest bit

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


    /** v1.11.0: the note just started is tied or sliding, so it is held until the next step. */
    void holdNewest (const Step& step) noexcept;

    /** Index of the held note `carryNote` in the pending queue, or -1 (released meanwhile). */
    int findCarried() const noexcept;

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

        const bool sounds = step.on && juce::isPositiveAndBelow (note, numMidiNotes);
        const bool tiesOn = previous == Carry::tie || (sounds && note == carryNote);   // a slide to the same pitch is a tie

        if (tiesOn)
        {
            // The held note becomes this step's note: no note-on. It carries on if this step
            // ties or slides too, otherwise it ends `gate` after this step's start.
            if (step.tie || step.slide)
            {
                carry = step.tie ? Carry::tie : Carry::slide;
            }
            else
            {
                auto& p = pending[(size_t) held];
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
        releaseSamePitch (midi, note, offset);   // before the mark: nothing may come between its four events
        midi.addEvent (juce::MidiMessage::controllerEvent (midiChannel, slideController, maxMidiValue), offset);
        startNote (midi, note, step.velocity, offset, gateSamples);
        midi.addEvent (juce::MidiMessage::noteOff (midiChannel, oldNote), offset);
        midi.addEvent (juce::MidiMessage::controllerEvent (midiChannel, slideController, 0), offset);

        if (step.tie || step.slide)
            holdNewest (step);
        return true;
    }

    /** Emits a note-on, first releasing the same note if it is still sounding. */
    /** Ends a pending note of this pitch, if any (by `offset`, or earlier if it was due earlier),
        so the new note-on never lands on a note still sounding. */
    void releaseSamePitch (juce::MidiBuffer& midi, int note, int offset) noexcept;

    void startNote (juce::MidiBuffer& midi, int note, int velocity, int offset, int gateSamples) noexcept;

    /** Emits the note-offs that fall inside this block and counts the rest down. */
    void advancePendingNoteOffs (juce::MidiBuffer& midi, int numSamples) noexcept;

    void removePending (int i) noexcept;

    std::array<std::atomic<juce::uint32>, numSteps> steps;
    std::atomic<int> currentStep { -1 };

    static constexpr size_t maxPendingNotes = 32;   ///< note-offs waiting at once; a 33rd ends the oldest early
    std::array<PendingNoteOff, maxPendingNotes> pending {};
    int    numPending   = 0;
    Carry  carry        = Carry::none;   ///< v1.11.0: a tie or slide from the last step that fired
    int    carryNote    = 0;             ///< ...and the note it holds
    double sampleRate   = placeholderSampleRate;
    double expectedBeat = 0.0;
    bool   wasPlaying   = false;

    static constexpr double noStepFired = -1.0e300;
    double lastFiredBeat = noStepFired;   ///< grid beat (unswung) of the most recent step that fired, while playing continuously
    static constexpr int maxGateSamples = 1 << 30;   ///< caps a note's length (samples) at extreme tempos
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
            if (! juce::isPositiveAndBelow (p[(size_t) i].note + semitones, StepSequencer::numMidiNotes))
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

        constexpr int lowestC = 48;      // C3: the notes start in this octave
        constexpr int onOdds = 4;        // a step is off one time in this many
        constexpr int minVelocity = 64;  // velocities are minVelocity .. minVelocity + velocityRange - 1
        constexpr int velocityRange = 64;

        const int lowest = lowestC + key;
        int notes[13] = {};   // at most every note of the octave, both ends included
        int numNotes = 0;
        for (int n = lowest; n <= lowest + 12; ++n)
            if (StepSequencer::isInScale (n, key, scale))
                notes[numNotes++] = n;

        for (int i = 0; i < length; ++i)
        {
            auto& s    = p[(size_t) i];
            s.on       = random.nextInt (onOdds) != 0;
            s.note     = notes[random.nextInt (numNotes)];
            s.velocity = minVelocity + random.nextInt (velocityRange);
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
        s.velocity = juce::jlimit (1, StepSequencer::maxMidiValue, velocity);

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
