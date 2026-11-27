// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "sequencer/StepSequencer.h"

void StepSequencer::process (juce::MidiBuffer& midi, int numSamples,
                             const Transport& transport, const Settings& settings) noexcept
{
    if (numSamples <= 0)
        return;

    // Stopped: release whatever is sounding and park the playhead. A tempo or position that is
    // not a finite number (a broken session, a host bug) counts as stopped too: written as
    // !(bpm > 0) so NaN lands here, since the step loop below would never end on it.
    // So does a tempo so small that a beat lasts longer than a double can count samples.
    const double samplesPerBeat = sampleRate * 60.0 / transport.bpm;
    if (! transport.playing || ! (transport.bpm > 0.0) || ! std::isfinite (transport.bpm) || ! std::isfinite (transport.beat)
        || ! std::isfinite (samplesPerBeat))
    {
        releaseAll (midi, 0);
        wasPlaying = false;
        lastFiredBeat = noStepFired;
        currentStep.store (-1, std::memory_order_relaxed);
        return;
    }

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
    // Non-finite settings (they come from parameters, so only a bug could send them) fall
    // back to the defaults rather than turn positions into NaN. The gate is worked out in
    // doubles and capped, so a very slow host tempo cannot overflow an int (v1.11.0).
    const float  swing       = std::isfinite (settings.swing) ? juce::jlimit (0.0f, 1.0f, settings.swing) : 0.0f;
    const float  gate        = std::isfinite (settings.gate)  ? juce::jlimit (minGate, 1.0f, settings.gate) : defaultGate;
    const double swingDelay  = (double) swing * 0.5 * stepLength;
    const int    gateSamples = (int) juce::jlimit (1.0, (double) maxGateSamples, std::round ((double) gate * stepLength * samplesPerBeat));

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
    constexpr double sameSampleTolerance = 0.5;   // samples: a swung step this close to its grid place was the one that fired

    const double firstCandidate = blockStart - (continuous ? toleranceSamples / samplesPerBeat : 0.0);

    for (auto k = (juce::int64) std::floor (firstCandidate / stepLength) - 1; ; ++k)
    {
        const bool   odd      = (k & 1) != 0;
        const double gridBeat = (double) k * stepLength;   // where the step is without swing
        const double stepBeat = gridBeat + (odd ? swingDelay : 0.0);
        const double distance = (stepBeat - blockStart) * samplesPerBeat;   // in samples, may be fractional
        auto         offset   = (juce::int64) std::ceil (distance - epsilon);

        if (offset >= numSamples)
            break;

        if (continuous)
        {
            // Already fired (it can reappear after a small backward drift)? Compared on the
            // grid, not the swung position: turning SWING up just after an odd step fired
            // would otherwise move that step past the mark and fire it again (v1.11.0).
            if (gridBeat <= lastFiredBeat + epsilon / samplesPerBeat)
                continue;
            // Skipped by a small forward drift: fire it now rather than never. Likewise an
            // odd step that SWING, turned down, has just moved into the past while the step
            // before it was the last to fire: it is late, but it must not be lost.
            if (offset < 0)
            {
                const bool movedBackBySwing = odd && std::abs (lastFiredBeat - (gridBeat - stepLength)) * samplesPerBeat < sameSampleTolerance;
                if ((double) -offset > toleranceSamples && ! movedBackBySwing)
                    continue;
                offset = 0;
            }
        }
        else if (offset < 0)
        {
            continue;   // started (or jumped) after this step: don't play it late
        }

        lastFiredBeat = gridBeat;

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

        if (step.on && settings.triggersEnabled && juce::isPositiveAndBelow (note, numMidiNotes))
        {
            startNote (midi, note, step.velocity, (int) offset, gateSamples);
            if (step.tie || step.slide)
                holdNewest (step);
        }
    }

    advancePendingNoteOffs (midi, numSamples);
}

void StepSequencer::releaseAll (juce::MidiBuffer& midi, int offset) noexcept
{
    for (int i = 0; i < numPending; ++i)
        midi.addEvent (juce::MidiMessage::noteOff (midiChannel, pending[(size_t) i].note), offset);
    numPending = 0;
    carry = Carry::none;
}

int StepSequencer::scaleMask (Scale scale) noexcept
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

bool StepSequencer::isInScale (int note, int key, Scale scale) noexcept
{
    const int degree = (((note - key) % 12) + 12) % 12;
    return (scaleMask (scale) & (1 << degree)) != 0;
}

int StepSequencer::snapToScale (int note, int key, Scale scale, int direction) noexcept
{
    note = juce::jlimit (0, maxMidiValue, note);
    if (isInScale (note, key, scale))
        return note;

    const auto fits = [key, scale] (int n) { return juce::isPositiveAndBelow (n, numMidiNotes) && isInScale (n, key, scale); };

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

int StepSequencer::moveInScale (int note, int key, Scale scale, int degrees) noexcept
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

void StepSequencer::holdNewest (const Step& step) noexcept
{
    auto& p = pending[(size_t) (numPending - 1)];
    p.held = true;
    carry = step.tie ? Carry::tie : Carry::slide;
    carryNote = p.note;
}

int StepSequencer::findCarried() const noexcept
{
    for (int i = 0; i < numPending; ++i)
        if (pending[(size_t) i].held && pending[(size_t) i].note == carryNote)
            return i;
    return -1;
}

void StepSequencer::releaseSamePitch (juce::MidiBuffer& midi, int note, int offset) noexcept
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
}

void StepSequencer::startNote (juce::MidiBuffer& midi, int note, int velocity, int offset, int gateSamples) noexcept
{
    releaseSamePitch (midi, note, offset);

    if (numPending == (int) pending.size())   // queue full: release the oldest note, at its own time if that is sooner
    {
        midi.addEvent (juce::MidiMessage::noteOff (midiChannel, pending[0].note), juce::jmin (offset, pending[0].samplesLeft));
        removePending (0);
    }

    midi.addEvent (juce::MidiMessage::noteOn (midiChannel, note, (juce::uint8) juce::jlimit (1, maxMidiValue, velocity)), offset);
    pending[(size_t) numPending++] = { note, offset + gateSamples };
}

void StepSequencer::advancePendingNoteOffs (juce::MidiBuffer& midi, int numSamples) noexcept
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

void StepSequencer::removePending (int i) noexcept
{
    pending[(size_t) i] = pending[(size_t) (numPending - 1)];
    --numPending;
}
