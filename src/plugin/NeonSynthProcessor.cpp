// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "plugin/NeonSynthProcessor.h"

NeonSynthProcessor::NeonSynthProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", Parameters::createParameterLayout())
{
    for (int i = 0; i < numVoices; ++i)
    {
        voices.push_back (static_cast<NeonSynthVoice*> (synth.addVoice (new NeonSynthVoice())));
        voices.back()->setVoiceIndex (i);
    }

    synth.addSound (new NeonSynthSound());

    // By default juce::Synthesiser handles any MIDI event that falls within 32
    // samples of its last render point immediately, so a note could start up to
    // 31 samples early. Splitting on every event keeps keyboard and sequencer
    // notes on their exact sample; the extra sub-blocks only appear when events
    // are dense, and cost little (see docs/DEVELOPMENT.md, "Performance").
    synth.setMinimumRenderingSubdivisionSize (1, true);

    SessionState::loadDefaultPattern (sequencer);   // the session has published the built-in pad already

    if (const auto path = MidiLogger::findMidiLogPath (wrapperType); path.isNotEmpty())
        midiLogger.start (MidiLogger::midiLogFileForNewInstance (path));
}

// The session's destructor waits for a sample load still running in the background, before the
// slot it publishes into goes away.
NeonSynthProcessor::~NeonSynthProcessor() = default;

void NeonSynthProcessor::reset()
{
    // Repeating a render from here gives the same grains (hosts call this before offline renders).
    for (auto* v : voices)
        v->resetSeeds();
    synth.forgetLastKeyboardNote();   // v1.11.0: nor does the first note glide from before the reset
    modulation.reset();   // LFO phase 0, bend and wheel centred
}

void NeonSynthProcessor::releaseResources()
{
    session.getSampleSlot().setAudioRunning (false);   // nothing reads the sample while stopped: replaced ones can be freed now
}

void NeonSynthProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    synth.setCurrentPlaybackSampleRate (sampleRate);

    for (auto* v : voices)
        v->prepare (sampleRate, samplesPerBlock);

    outputGain.reset (sampleRate, outputGainGlideSeconds);
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (parameters.gain->get()));

    midiLogger.setSampleRate (sampleRate);
    modulation.prepare (sampleRate, samplesPerBlock);

    sequencer.prepare (sampleRate);
    router.prepare();

    session.getSampleSlot().setAudioRunning (true);
}

void NeonSynthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();

    // The plugin has no input bus: the buffer arrives with no incoming audio.
    buffer.clear();
    midiLogger.push (midiMessages, samplesProcessed);   // lock-free; formatting/I/O happen elsewhere
    blockSample = session.getSampleSlot().beginBlock();   // the voices' sample for this block (Sample and Granular sources)

    NeonSynthVoice::Params params;
    params.waveform = parameters.waveform->getIndex();
    params.cutoffHz = parameters.cutoff->get();
    params.filterOn = parameters.filterEnabled->get();
    params.filterMode      = parameters.filterMode->getIndex();
    params.filterResonance = parameters.filterRes->get() * 0.01f;
    params.filterDrive     = parameters.filterDrive->get() * 0.01f;
    params.filterEnvOctaves = parameters.filterEnvAmt->get();
    params.filterEnv        = { parameters.filterAttack->get(), parameters.filterDecay->get(),
                                parameters.filterSustain->get(), parameters.filterRelease->get() };
    params.filterVelOctaves = parameters.filterVel->get();
    params.width    = parameters.width->get();
    params.adsr     = { parameters.attack->get(), parameters.decay->get(),
                        parameters.sustain->get(), parameters.release->get() };

    params.source = (NeonSynthVoice::Source) parameters.source->getIndex();
    params.sample = blockSample;
    params.sampleSettings = { parameters.smpRoot->get(), parameters.smpStart->get(), parameters.smpLoop->getIndex(),
                              parameters.smpLoopStart->get(), parameters.smpLoopEnd->get(), parameters.smpXfade->get() * 0.001f };
    params.grainSettings  = { parameters.grPos->get(), parameters.grScan->get(), parameters.grSize->get() * 0.001f,
                              parameters.grDensity->get(), parameters.grSpray->get(), parameters.grJitter->get() };
    params.chop = { chopDepths.load (std::memory_order_relaxed), parameters.chopComb->get(), parameters.chopOvertone->get() * 0.01f };
    params.sculpt = { parameters.noiseLevel->get() * 0.01f, parameters.foldDrive->get(), parameters.foldMix->get() * 0.01f };
    params.glideSeconds = parameters.glide->get() * 0.001f;   // v1.11.0 (region B): GLIDE, ms to seconds
    params.naive      = parameters.naive->get();               // v1.12.0 "Fourier"
    params.custom     = parameters.custom->get();
    params.harmonics  = parameters.harmonics->get();
    params.wavetables = &fourier.acquireForAudio();            // lock-free; the newest finished tables

    // Modulation (v1.9.0): the LFO, pitch bend and the mod wheel, worked out once for all voices.
    // Bend and wheel are read from the incoming MIDI, with their sample positions, so they reach
    // every voice (keyboard and sequencer notes alike) on the right sample.
    Modulation::Settings modSettings;
    modSettings.lfo = { (Lfo::Shape) parameters.lfoShape->getIndex(), parameters.lfoRate->get(),
                        parameters.lfoSync->get(), (Lfo::Division) parameters.lfoDivision->getIndex() };
    modSettings.lfoPitchSemitones  = parameters.lfoPitch->get();
    modSettings.lfoCutoffOctaves   = parameters.lfoCutoff->get();
    modSettings.lfoAmp             = parameters.lfoAmp->get() * 0.01f;
    modSettings.bendRangeSemitones = parameters.bendRange->get();
    modSettings.vibratoCents       = parameters.vibrato->get();
    modulation.setSettings (modSettings);
    synth.setVoiceLimit (parameters.voiceCount->get());   // new notes use only the first N voices
    synth.setMonoLegato (parameters.legato->get() && parameters.voiceCount->get() == 1);   // v1.11.0 (region B): LEGATO is MONO only
    modulation.handleMidi (midiMessages, numSamples);
    const auto modulationBlock = modulation.render (numSamples, lfoTransport());

    // Every voice shares the same settings and modulation.
    for (auto* v : voices)
    {
        v->setParameters (params);
        v->setModulation (modulationBlock);
    }

    // Voice assignment is automatic: juce::Synthesiser reads each note-on in
    // midiMessages, picks a free voice (stealing one if all 16 are busy)
    // and calls its startNote() with that note number and velocity.

    // Voices render straight into 'buffer', so any host block size is safe.
    // The synth plays the keyboard (filtered by the sequencer mode) plus the sequencer.
    const auto& playMidi = router.buildPlayMidi (midiMessages, numSamples, routerControls(), getPlayHead());
    synth.renderNextBlock (buffer, playMidi, 0, numSamples);
    modulation.applyAmplitude (buffer, numSamples);   // the LFO's tremolo, on the summed voices

    outputGain.setTargetValue (juce::Decibels::decibelsToGain (parameters.gain->get()));
    outputGain.applyGain (buffer, numSamples);

    // Headroom: 16 full-velocity voices can sum to about +12 dBFS. The soft
    // clipper keeps the output under -1 dBFS and is bit-transparent below -6 dBFS.
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* samples = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
            samples[i] = softClip (samples[i]);
    }

    pushScope (buffer.getReadPointer (0),
               buffer.getReadPointer (buffer.getNumChannels() > 1 ? 1 : 0),   // mono bus: L == R
               numSamples);
    publishVoiceViews();
    samplesProcessed += numSamples;
}

void NeonSynthProcessor::pushScope (const float* left, const float* right, int numFrames) noexcept
{
    int i = 0;
    scopeFifo.write (numFrames).forEach ([&] (int index)
    {
        scopeRing[2 * (size_t) index]     = left[i];
        scopeRing[2 * (size_t) index + 1] = right[i];
        ++i;
    });
}

void NeonSynthProcessor::publishVoiceViews() noexcept
{
    for (size_t i = 0; i < voices.size() && i < voiceViews.size(); ++i)
    {
        const auto t = voices[i]->getTelemetry();
        auto& v = voiceViews[i];
        v.active         .store (t.active,          std::memory_order_relaxed);
        v.level          .store (t.level,           std::memory_order_relaxed);
        v.frequencyHz    .store (t.frequencyHz,     std::memory_order_relaxed);
        v.secondsSinceOn .store (t.secondsSinceOn,  std::memory_order_relaxed);
        v.secondsSinceOff.store (t.secondsSinceOff, std::memory_order_relaxed);
        v.samplePosition .store (t.samplePosition,  std::memory_order_relaxed);
        for (size_t g = 0; g < v.grainPositions.size(); ++g)
            v.grainPositions[g].store ((int) g < t.numGrains ? t.grainPositions[g] : -1.0f, std::memory_order_relaxed);
        v.playingRandom.store (t.playingRandom, std::memory_order_relaxed);
        v.note         .store (t.note,          std::memory_order_relaxed);
        if (t.playingRandom)
            for (size_t k = 0; k < v.randomLevels.size(); ++k)
                v.randomLevels[k].store (t.randomLevels[k], std::memory_order_relaxed);
    }
}

int NeonSynthProcessor::drainScope (float* destLeft, float* destRight, int maxFrames) noexcept
{
    int i = 0;
    scopeFifo.read (juce::jmin (maxFrames, scopeFifo.getNumReady())).forEach ([&] (int index)
    {
        destLeft[i]  = scopeRing[2 * (size_t) index];
        destRight[i] = scopeRing[2 * (size_t) index + 1];
        ++i;
    });
    return i;
}

bool NeonSynthProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

//==============================================================================
// Step sequencer

void NeonSynthProcessor::setSequencerStep (int index, const StepSequencer::Step& step)
{
    // Message thread: the step grid's mouse handlers call this (and tests, from their main thread),
    // so the host can be told here directly.
    sequencer.setStep (index, step);
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

void NeonSynthProcessor::setSequencerPattern (const StepSequencer::Pattern& pattern)
{
    // v1.11.0: the pattern tools, undo and ENTER change several steps at once; the host is told
    // once, not once per step. Message thread, like setSequencerStep().
    bool changed = false;
    for (int i = 0; i < StepSequencer::numSteps; ++i)
    {
        if (! (sequencer.getStep (i) == pattern[(size_t) i]))
        {
            sequencer.setStep (i, pattern[(size_t) i]);
            changed = true;
        }
    }
    if (changed)
        updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

Lfo::Transport NeonSynthProcessor::lfoTransport() const
{
    // A playing host sets the LFO's phase from its song position (when SYNC is on); otherwise the
    // LFO free-runs, at the host's tempo when there is one, else at the Sequencer Tempo parameter.
    Lfo::Transport transport;
    transport.bpm = (double) parameters.seqTempo->get();

    if (auto* hostPlayHead = getPlayHead())
    {
        if (const auto position = hostPlayHead->getPosition(); position.hasValue())
        {
            if (const auto bpm = position->getBpm(); bpm.hasValue())
                transport.bpm = *bpm;

            // v1.11.0: the bar divisions follow the host's time signature (4/4 gives 4 beats,
            // exactly as before); Lfo::render() falls back to 4/4 for a signature that makes no sense.
            if (const auto signature = position->getTimeSignature(); signature.hasValue() && signature->numerator > 0 && signature->denominator > 0)
                transport.beatsPerBar = 4.0 * (double) signature->numerator / (double) signature->denominator;

            if (const auto ppq = position->getPpqPosition(); ppq.hasValue() && position->getIsPlaying())
            {
                transport.hostPlaying = true;
                transport.ppq = *ppq;

                if (const auto barStart = position->getPpqPositionOfLastBarStart(); barStart.hasValue())
                {
                    transport.hasBarStart = true;
                    transport.barStartPpq = *barStart;
                }
            }
        }
    }
    return transport;
}

MidiRouter::Controls NeonSynthProcessor::routerControls() const noexcept
{
    MidiRouter::Controls controls;
    controls.sequencerOn   = parameters.seqOn->get();
    controls.sequencerMode = (SequencerMode) parameters.seqMode->getIndex();
    controls.hold          = parameters.hold->get();
    controls.sync          = parameters.seqSync->get();
    controls.tempo         = parameters.seqTempo->get();
    controls.rate          = (StepSequencer::Rate) parameters.seqRate->getIndex();
    controls.swing         = parameters.seqSwing->get();
    controls.gate          = parameters.seqGate->get();
    controls.length        = parameters.seqLength->get();
    controls.sampleRate    = getSampleRate();
    return controls;
}

//==============================================================================
// CHOP

void NeonSynthProcessor::setChopDepth (int step, int depth) noexcept
{
    if (step < 0 || step >= Chop::stepsPerPattern)
        return;
    auto current = chopDepths.load();
    while (! chopDepths.compare_exchange_weak (current, ChopSettings::withDepth (current, step, depth)))
    {
    }
}

//==============================================================================
// v1.12.0 "Fourier"

void NeonSynthProcessor::setHarmonicSpectrum (const HarmonicSpectrum& spectrum)
{
    if (fourier.setSpectrum (spectrum))
        updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

void NeonSynthProcessor::setCycleDrawing (const std::optional<FourierState::Drawing>& drawing, const HarmonicSpectrum& spectrum)
{
    fourier.setDrawing (drawing);
    fourier.setSpectrum (spectrum);
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NeonSynthProcessor();
}
