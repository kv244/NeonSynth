// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

// The voice's work outside the sample loop (v1.11.1): preparing, starting and stopping notes,
// taking each block's parameters, and the telemetry for the editor. Since v1.13.0 also the
// per-block code around the loop: renderNextBlock() (which picks the steady or gliding loop),
// endVoice(), setFilterCoefficients(), renderCloud() and settleSampleCopy(). The per-sample loop
// itself (renderSamples<steady>() and everything it calls) stays in NeonSynthVoice.h, so the
// compiler sees it inline without relying on link-time optimisation.

#include "plugin/NeonSynthVoice.h"

namespace
{
    // Multipliers that spread a voice index, note or note count over all 32 bits of a seed
    // (Knuth's multiplicative hash, xxHash's second prime, and 40503, about 2^16 divided by the
    // golden ratio, the 16-bit Fibonacci-hashing multiplier), so nearby
    // voices and notes get unrelated random streams.
    constexpr juce::uint32 seedHashA = 2654435761u;
    constexpr juce::uint32 seedHashB = 2246822519u;
    constexpr juce::uint32 seedHashC = 40503u;

    // The hard stop sets the envelope's attack and decay this short; only its release (the fade) is heard.
    constexpr float hardStopAttackDecaySeconds = 0.001f;
}

NeonSynthVoice::Telemetry NeonSynthVoice::getTelemetry() const noexcept
{
    Telemetry t;
    t.active          = adsr.isActive();
    t.level           = t.active ? lastEnvelope : 0.0f;
    t.frequencyHz     = frequencyHz * shownPitchRatio;   // with bend, vibrato and LFO pitch: display only
    t.secondsSinceOn  = (float) ((double) samplesSinceOn / sampleRate);
    t.secondsSinceOff = samplesSinceOff < 0 ? -1.0f : (float) ((double) samplesSinceOff / sampleRate);
    t.samplePosition  = t.active && source == Source::sample && reader.isActive() && ! reader.hasFinished()
                          ? reader.getPosition() : -1.0f;
    t.numGrains       = t.active && cloud.isActive() ? cloud.getGrainPositions (t.grainPositions.data(), Telemetry::maxShownGrains) : 0;
    t.playingRandom   = t.active && source == Source::random;
    t.note            = t.active ? getCurrentlyPlayingNote() : -1;
    if (t.playingRandom)
        t.randomLevels = randomOscillator.getLevels();
    return t;
}

void NeonSynthVoice::prepare (double newSampleRate, int maxBlockSize)
{
    // Audio is stopped while preparing: let go of any sample still held.
    reader.end();
    fadingReader.end();
    fadingSamplesLeft = 0;
    fadingCopy = false;
    cloud.end();
    resetSeeds();

    sampleRate = newSampleRate;   // (resetSeeds() above also forgets any glide, v1.11.0: its step is per sample)
    adsr.setSampleRate (newSampleRate);
    adsr.reset();
    filterEnvelope.setSampleRate (newSampleRate);
    filterEnvelope.reset();
    oscillator.prepare (newSampleRate);        // CHOP's Overtone glide
    randomOscillator.prepare (newSampleRate);
    sculpt.prepare (newSampleRate);

    filter.prepare (newSampleRate);
    cloudBuffer.assign ((size_t) juce::jmax (1, maxBlockSize), 0.0f);   // the granular source renders a block at a time

    level.reset (newSampleRate, levelGlideSeconds);       // 2 ms: a retriggered or stolen voice changes velocity without a click
    level.setCurrentAndTargetValue (level.getTargetValue());

    cutoff.reset (newSampleRate, controlGlideSeconds);       // 20 ms, multiplicative (even in octaves)
    cutoff.setCurrentAndTargetValue (clampCutoff (cutoff.getTargetValue()));
    resonance.reset (newSampleRate, controlGlideSeconds);    // 20 ms, as the cutoff
    resonance.setCurrentAndTargetValue (resonance.getTargetValue());
    drive.reset (newSampleRate, controlGlideSeconds);
    drive.setCurrentAndTargetValue (drive.getTargetValue());
    for (auto* amount : { &filterEnvOctaves, &filterVelOctaves })   // 20 ms: automating them doesn't step the cutoff
    {
        amount->reset (newSampleRate, controlGlideSeconds);
        amount->setCurrentAndTargetValue (amount->getTargetValue());
    }
    modeFade.reset (newSampleRate, filterSwitchSeconds);     // 10 ms crossfade when the mode changes
    modeFade.setCurrentAndTargetValue (1.0f);
    setFilterCoefficients();

    filterMix.reset (newSampleRate, filterSwitchSeconds);    // 10 ms crossfade for the on/off toggle

    pan.reset (newSampleRate, controlGlideSeconds);          // 20 ms so Width changes don't zipper
    pan.setCurrentAndTargetValue (notePan * width);
    updatePanGains (pan.getCurrentValue());
}

void NeonSynthVoice::setParameters (const Params& p) noexcept
{
    adsrParams = p.adsr;
    if (! hardStopping)
        adsr.setParameters (adsrParams);

    oscillator.setWaveform (p.waveform);              // applied at the next cycle boundary
    oscillator.setNaive (p.naive);                    // v1.12.0: NAIVE and CUSTOM too; a new set of
    oscillator.setCustom (p.custom, p.harmonics, p.wavetables);   // tables fades in from the old wave
    oscillator.setChop (p.chop);
    randomOscillator.setNaive (p.naive);
    randomOscillator.setChop (p.chop);
    sculpt.set (p.sculpt);
    cutoff.setTargetValue (clampCutoff (p.cutoffHz));
    filterMix.setTargetValue (p.filterOn ? 1.0f : 0.0f);
    resonance.setTargetValue (juce::jlimit (0.0f, 1.0f, p.filterResonance));
    drive.setTargetValue (juce::jlimit (0.0f, 1.0f, p.filterDrive));
    setFilterMode (juce::jlimit ((int) CytomicSvf::Mode::lowpass, (int) CytomicSvf::Mode::notch, p.filterMode));
    filterEnvelope.setParameters (p.filterEnv);
    filterEnvOctaves.setTargetValue (juce::jlimit (-maxFilterEnvOctaves, maxFilterEnvOctaves, p.filterEnvOctaves));
    filterVelOctaves.setTargetValue (juce::jlimit (0.0f, maxFilterVelOctaves, p.filterVelOctaves));

    width = p.width;
    pan.setTargetValue (notePan * width);

    pendingSource  = p.source;
    blockSample    = p.sample;
    sampleSettings = p.sampleSettings;
    cloud.setSettings (p.grainSettings);
    glideSeconds = juce::jmax (0.0f, p.glideSeconds);   // v1.11.0: latched by each note that glides
}

void NeonSynthVoice::startNote (int midiNote, float velocity, juce::SynthesiserSound*, int)
{
    // v1.11.0: a legato step keeps everything sounding and only moves the pitch.
    if (legatoStepPending)
    {
        legatoStepPending = false;
        if (adsr.isActive() && ! hardStopping)
        {
            glideToNote (midiNote);
            return;
        }
    }

    // v1.11.0, GLIDE: where the pitch was (the note playing, or the one this voice last played),
    // as an offset in octaves from the new note; the glide starts there. With GLIDE at 0, or on
    // a voice's first note, it is 0 and the note starts exactly at its pitch, as in v1.10.1.
    // The sequencer's notes never glide here: like a 303, a step glides only when the step
    // before it is marked SLIDE (the legato step above), so the other steps keep their attack
    // whatever GLIDE is set to (Julian, 22 Nov 2026). Keyboard notes glide whenever GLIDE > 0.
    // A keyboard note glides from the last keyboard note played: from where this voice's pitch
    // is now if it played that note (so a glide still moving carries on smoothly), otherwise
    // from that note. The first keyboard note has nothing to glide from.
    const bool sequencerNote = isPlayingChannel (StepSequencer::midiChannel);
    float glideFrom = 0.0f;
    if (glideSeconds > 0.0f && ! sequencerNote && keyboardGlideFrom >= 0)
    {
        const float fromNote = hasGlidePitch && glideBaseNote == keyboardGlideFrom ? currentPitchNote()
                                                                                 : (float) keyboardGlideFrom;
        glideFrom = (fromNote - (float) midiNote) / 12.0f;
    }
    glideBaseNote = midiNote;
    hasGlidePitch = true;
    beginGlide (glideFrom, 0.0f);

    // The note's own pitch; a glide (above) multiplies it towards 1 from where it was.
    frequencyHz     = (float) juce::MidiMessage::getMidiNoteInHertz (midiNote);
    oscillator.setFrequency (frequencyHz, sampleRate);
    randomOscillator.setFrequency (frequencyHz, sampleRate);
    startSource (midiNote);
    samplesSinceOn  = 0;
    samplesSinceOff = -1;

    // A sounding voice (retrigger or steal) glides to its new velocity and
    // pan. A silent voice starts exactly at the current settings: its
    // smoothers stopped advancing when it went quiet, so without the snap a
    // cutoff or filter-toggle change made while it was idle would sweep
    // audibly over the first 10-20 ms of the note.
    notePan = juce::jlimit (-1.0f, 1.0f, (float) (midiNote - panCentreNote) / panSpreadSemitones);
    const float newLevel = velocity * voiceGain;

    // The filter envelope attacks from its current level, like the amp envelope. Both it and
    // the velocity enter modulatedCutoffHz(): a silent voice sets its coefficients from them
    // below, at once; a sounding one picks them up at the next 16-sample update.
    noteVelocity = juce::jlimit (0.0f, 1.0f, velocity);
    filterEnvelope.noteOn();

    if (adsr.isActive())
    {
        level.setTargetValue (newLevel);
        pan.setTargetValue (notePan * width);
    }
    else
    {
        level.setCurrentAndTargetValue (newLevel);
        pan.setCurrentAndTargetValue (notePan * width);
        updatePanGains (pan.getCurrentValue());
        cutoff.setCurrentAndTargetValue (cutoff.getTargetValue());
        resonance.setCurrentAndTargetValue (resonance.getTargetValue());
        drive.setCurrentAndTargetValue (drive.getTargetValue());
        filterEnvOctaves.setCurrentAndTargetValue (filterEnvOctaves.getTargetValue());
        filterVelOctaves.setCurrentAndTargetValue (filterVelOctaves.getTargetValue());
        modeFade.setCurrentAndTargetValue (1.0f);
        setFilterCoefficients();   // at once, not at the next 16-sample boundary
        filterMix.setCurrentAndTargetValue (filterMix.getTargetValue());
    }

    if (hardStopping)
    {
        hardStopping = false;
        adsr.setParameters (adsrParams);
    }

    // The envelope attacks from its current level, and the phase and filter state
    // are kept, so retriggering or stealing a sounding voice doesn't click.
    adsr.noteOn();
}

void NeonSynthVoice::stopNote (float, bool allowTailOff)
{
    // v1.11.0: the hard stop juce::Synthesiser::startVoice() sends ahead of a legato step.
    if (legatoStepPending && ! allowTailOff)
        return;

    samplesSinceOff = 0;

    if (allowTailOff)
    {
        reader.keyReleased();   // Loop until release: play on to the end
        adsr.noteOff();
        filterEnvelope.noteOff();
        return;
    }

    // Hard stop (voice stolen, all sound off): 5 ms fade instead of a cut.
    // juce::Synthesiser keeps calling renderNextBlock() on free voices, so the fade still plays.
    // The filter envelope is left as it is: a sweep during a 5 ms fade would only be heard as
    // a click, a stolen voice's next note retriggers it from where it is, and when the fade
    // ends the voice resets it.
    hardStopping = true;
    adsr.setParameters ({ hardStopAttackDecaySeconds, hardStopAttackDecaySeconds, 1.0f, (float) hardStopFadeSeconds });
    adsr.noteOff();
    clearCurrentNote();
}

void NeonSynthVoice::setFilterMode (int newMode) noexcept
{
    const auto m = (CytomicSvf::Mode) newMode;
    if (m == mode)
        return;
    previousMode = mode;
    mode = m;
    modeFade.setCurrentAndTargetValue (0.0f);
    modeFade.setTargetValue (1.0f);
}

void NeonSynthVoice::startSource (int midiNote) noexcept
{
    auto newSource = pendingSource;
    if (blockSample == nullptr && (newSource == Source::sample || newSource == Source::granular))
        newSource = Source::oscillator;

    // Retrigger or steal while the sample sounds: fade the old read position out over
    // 2 ms instead of jumping (whatever the new note plays).
    if (source == Source::sample && reader.isActive() && ! reader.hasFinished() && adsr.isActive())
    {
        reader.swapWith (fadingReader);   // fadingReader now holds the old note and its sample reference
        fadingSamplesLeft = fadingSamplesTotal = juce::jmax (1, (int) (sampleCrossfadeSeconds * sampleRate));
    }
    fadingCopy = false;   // the new note starts at full level, whatever was fading

    source = newSource;
    ++noteCounter;

    if (source == Source::sample)
        reader.begin (blockSample, midiNote, sampleRate, sampleSettings);
    else
        reader.end();
    sampleNoteFresh = true;

    if (source == Source::granular)
    {
        const auto seed = (juce::uint32) midiNote * seedHashA ^ (juce::uint32) (voiceIndex + 1) * seedHashC
                        ^ (juce::uint32) noteCounter * seedHashB;
        cloud.begin (blockSample, midiNote, sampleSettings.rootNote, sampleRate, cloud.getSettings(), seed);
    }
    else
    {
        cloud.stopSpawning();   // grains still sounding play out
    }
}

void NeonSynthVoice::beginGlide (float fromOctaves, float toOctaves) noexcept
{
    glideTargetOctaves = toOctaves;
    if (glideSeconds > 0.0f && fromOctaves != toOctaves)
    {
        glideOctaves = fromOctaves;
        glideStepOctaves = std::abs (toOctaves - fromOctaves) / juce::jmax (1.0f, glideSeconds * (float) sampleRate);
    }
    else
    {
        glideOctaves = toOctaves;
    }
    glideRatio = glideOctaves == 0.0f ? 1.0f : std::exp2 (glideOctaves);
}

void NeonSynthVoice::glideToNote (int midiNote) noexcept
{
    beginGlide (glideOctaves, (float) (midiNote - glideBaseNote) / 12.0f);
    notePan = juce::jlimit (-1.0f, 1.0f, (float) (midiNote - panCentreNote) / panSpreadSemitones);   // the pan follows (20 ms smoothing)
    pan.setTargetValue (notePan * width);
    samplesSinceOff = -1;   // a key holds it again (a return to a held key)
}

void NeonSynthVoice::resetSeeds() noexcept
{
    noteCounter = 0;
    randomOscillator.reseed ((juce::uint32) (voiceIndex + 1) * seedHashA);
    sculpt.reseed ((juce::uint32) (voiceIndex + 1) * seedHashB);

    // v1.11.0: forget the last pitch too, so a repeated render's first notes don't glide.
    hasGlidePitch = false;
    glideBaseNote = firstGlideBaseNote;
    glideOctaves = glideTargetOctaves = 0.0f;
    glideRatio = 1.0f;
}

bool NeonSynthVoice::canPlaySound (juce::SynthesiserSound* s)
{
    return dynamic_cast<NeonSynthSound*> (s) != nullptr;
}

void NeonSynthVoice::renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples)
{
    // Where the block stands after this call: a note started before the next call sets its
    // filter coefficients there, with the LFO cutoff of that sample (v1.9.0).
    const int nextModulationIndex = startSample + numSamples;

    if (! adsr.isActive())
    {
        modulationIndex = nextModulationIndex;
        return;
    }

    auto* left  = output.getWritePointer (0);
    auto* right = output.getNumChannels() > 1 ? output.getWritePointer (1) : nullptr;

    // The grain cloud renders the whole stretch up front (block-wise is several times
    // cheaper than per sample); the loop below reads it like any other source.
    const bool cloudSounds = cloud.isActive();
    if (cloudSounds)
        renderCloud (startSample, numSamples);

    // Pitch modulation ended (or never started) with the sample on another copy than
    // the note's own: go back to it. Never happens without pitch modulation.
    if (modulation.pitchRatio == nullptr && ! glideActive())   // (v1.11.0: nor a glide)
        settleSampleCopy (1.0f, false);

    const bool steady = ! level.isSmoothing() && ! cutoff.isSmoothing() && ! filterMix.isSmoothing() && ! pan.isSmoothing()
                     && ! resonance.isSmoothing() && ! drive.isSmoothing() && ! modeFade.isSmoothing()
                     && ! filterEnvelopeMovesCutoff() && ! filterUpdatePending();
    // The filter envelope advances one step per sample whichever version renders, so its
    // timing never depends on which one ran - also at amount 0, so it is in step if the
    // amount is turned up mid-note. A constant envelope (sustaining, or idle) is skipped:
    // getNextSample() would change nothing, and it stays constant for the whole call (it
    // only starts to move at a note-on or note-off, or with new parameters, all between
    // calls). The gliding version steps it inside its loop, where the coefficient updates
    // read it. The steady version never reads it (it runs only while the envelope is
    // constant or its amount is 0), so it is stepped here, after the loop: the steady
    // loop stays exactly what it was before the filter envelope existed.
    const bool stepFilterEnvelopeAfter = steady && ! filterEnvelope.isConstant();

    const int rendered = steady ? renderSamples<true>  (left, right, startSample, numSamples, cloudSounds)
                                : renderSamples<false> (left, right, startSample, numSamples, cloudSounds);

    if (stepFilterEnvelopeAfter && adsr.isActive())   // (a voice that ended has reset it)
        for (int n = 0; n < rendered; ++n)
            filterEnvelope.getNextSample();

    // The gliding version counts down per sample; the steady one skips ahead here.
    if (steady)
        coefficientCountdown = (coefficientCountdown - 1 + coefficientInterval - rendered % coefficientInterval)
                                   % coefficientInterval + 1;

    samplesSinceOn += rendered;
    if (samplesSinceOff >= 0)
        samplesSinceOff += rendered;
    modulationIndex = nextModulationIndex;

    if (rendered > 0)
    {
        sampleNoteFresh = false;
        shownPitchRatio = isPitchModulated() ? pitchRatioAt (startSample + rendered - 1, 0) : 1.0f;
    }
}

void NeonSynthVoice::endVoice() noexcept
{
    if (isVoiceActive())
        clearCurrentNote();
    finishGlide();   // v1.11.0: the next note glides from where this one was going
    filter.reset();
    filterEnvelope.reset();
    modeFade.setCurrentAndTargetValue (1.0f);
    lastEnvelope = 0.0f;
    reader.end();          // silent now: let go of the sample
    fadingReader.end();
    fadingSamplesLeft = 0;
    fadingCopy = false;
    cloud.end();
}

void NeonSynthVoice::setFilterCoefficients() noexcept
{
    coefficientCutoffHz  = modulatedCutoffHz();
    coefficientResonance = resonance.getCurrentValue();
    if (coefficientDrive != drive.getCurrentValue())
    {
        coefficientDrive = drive.getCurrentValue();
        driveSettings = CytomicSvf::makeDrive (coefficientDrive);
    }
    filter.setCoefficients (coefficientCutoffHz, CytomicSvf::resonanceToK (coefficientResonance));
    coefficientCountdown = coefficientInterval;
}

void NeonSynthVoice::renderCloud (int firstIndex, int numSamples) noexcept
{
    cloudSamples = juce::jmin (numSamples, (int) cloudBuffer.size());
    if (! isPitchModulated())
    {
        cloud.setPitchRatio (1.0f);   // grains started under modulation glide back to the note's pitch
        cloud.render (cloudBuffer.data(), cloudSamples);
    }
    else
    {
        for (int done = 0; done < cloudSamples;)
        {
            const int chunk = juce::jmin (cloudChunk, cloudSamples - done);
            cloud.setPitchRatio (pitchRatioAt (firstIndex + done, done));
            cloud.render (cloudBuffer.data() + done, chunk);
            done += chunk;
        }
    }
    cloudRead = 0;
    if (! cloud.isSounding())
        cloud.end();
}

void NeonSynthVoice::settleSampleCopy (float ratio, bool withHysteresis) noexcept
{
    if (source != Source::sample || ! reader.isActive() || reader.hasFinished())
        return;

    const int wanted = reader.levelFor (ratio, withHysteresis);
    if (wanted == reader.getLevel())
        return;

    if (sampleNoteFresh)
    {
        reader.moveToLevel (wanted);
        return;
    }
    if (fadingSamplesLeft > 0)
        return;

    reader.copyNoteTo (fadingReader);   // the old copy, fading out from where the note is now
    reader.moveToLevel (wanted);
    fadingSamplesLeft = fadingSamplesTotal = juce::jmax (1, (int) (sampleCrossfadeSeconds * sampleRate));
    fadingCopy = true;
}
