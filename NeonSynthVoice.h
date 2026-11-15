// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "VoiceEnvelope.h"
#include "Oscillator.h"
#include "Sculpt.h"
#include "SampleReader.h"
#include "GrainCloud.h"
#include "CytomicSvf.h"

class NeonSynthSound : public juce::SynthesiserSound
{
public:
    bool appliesToNote    (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
};

/**
    One synth voice: sound source -> ADSR (VoiceEnvelope) -> TPT low-pass -> pan.

    The source is chosen per note (see Source): the band-limited oscillator
    or the Random step wave (Oscillator.h), the loaded sample
    (SampleReader.h) or grains of it (GrainCloud.h). Sources plug in at nextSourceSample(), so nothing after
    it changes. A voice retriggered or stolen while playing the sample
    crossfades from its old read position to the new one over 2 ms; grains
    already sounding always play out, so a granular voice never clicks
    either.

    Stereo: each voice is panned by its pitch (C4 in the centre, two octaves
    either side reaching the edges), scaled by the Width parameter. The pan law
    is constant-power, normalised so a centred voice keeps unity gain on both
    channels - at Width 0 the output is identical to the old mono voice.

    Real-time safe: no allocation, no std::function, renders straight into the
    output buffer (no scratch buffer to overrun), and every parameter that can
    click is smoothed or latched at a phase wrap.
*/
class NeonSynthVoice : public juce::SynthesiserVoice
{
public:
    /** Where a note's sound comes from (the Source parameter's order). */
    enum class Source { oscillator = 0, sample = 1, granular = 2, random = 3 };

    /** Parameter snapshot, read once per block on the audio thread. */
    struct Params
    {
        int   waveform = 0;            // PolyBlepOscillator::Waveform: 0 Sine, 1 Square, 2 Triangle, 3 Sawtooth
        float cutoffHz = 2000.0f;
        bool  filterOn = true;

        // Filter character (v1.8.0); the defaults are the v1.7.1 filter.
        int   filterMode = 0;          // CytomicSvf::Mode: 0 Low-pass, 1 High-pass, 2 Band-pass, 3 Notch
        float filterResonance = 0.0f;  // 0..1
        float filterDrive = 0.0f;      // 0..1

        // Filter envelope and velocity (v1.8.0), in octaves added to the cutoff; 0 = none.
        float filterEnvOctaves = 0.0f;           // -5..+5, at the envelope's peak
        VoiceEnvelope::Parameters filterEnv { 0.01f, 0.3f, 0.5f, 0.3f };
        float filterVelOctaves = 0.0f;           // 0..+4, at full velocity

        float width = 0.0f;            // stereo width, 0 = mono .. 1 = full spread
        juce::ADSR::Parameters adsr;

        Source source = Source::oscillator;       // latched at each note-on
        const SampleData* sample = nullptr;       // this block's sample (SampleSlot::beginBlock)
        SampleReader::Settings sampleSettings;    // latched at each note-on
        GrainCloud::Settings grainSettings;       // live: applies to the next grains
        ChopSettings chop;                        // Oscillator and Random; pattern and Comb take effect at a cycle boundary
        SculptSettings sculpt;                    // Oscillator and Random: Noise, then Fold
    };

    /** What the editor's visualisers need to know about this voice. */
    struct Telemetry
    {
        bool  active = false;
        float level = 0.0f;              // current envelope value, 0..1
        float frequencyHz = 0.0f;
        float secondsSinceOn = 0.0f;
        float secondsSinceOff = -1.0f;   // -1 while the key is held
        float samplePosition = -1.0f;    // read position 0..1 while playing the sample, else -1
        static constexpr int maxShownGrains = 8;
        std::array<float, maxShownGrains> grainPositions {};   // read positions 0..1 of up to 8 sounding grains
        int numGrains = 0;
        bool playingRandom = false;      // the note plays the Random source
        std::array<float, RandomStepOscillator::numSteps> randomLevels {};   // its current 16 levels (for the shape view)
    };

    Telemetry getTelemetry() const noexcept
    {
        Telemetry t;
        t.active          = adsr.isActive();
        t.level           = t.active ? lastEnvelope : 0.0f;
        t.frequencyHz     = frequencyHz;
        t.secondsSinceOn  = (float) ((double) samplesSinceOn / sampleRate);
        t.secondsSinceOff = samplesSinceOff < 0 ? -1.0f : (float) ((double) samplesSinceOff / sampleRate);
        t.samplePosition  = t.active && source == Source::sample && reader.isActive() && ! reader.hasFinished()
                              ? reader.getPosition() : -1.0f;
        t.numGrains       = t.active && cloud.isActive() ? cloud.getGrainPositions (t.grainPositions.data(), Telemetry::maxShownGrains) : 0;
        t.playingRandom   = t.active && source == Source::random;
        if (t.playingRandom)
            t.randomLevels = randomOscillator.getLevels();
        return t;
    }

    // -12 dB per voice. A full-velocity 8-note square chord would peak at about
    // +5 dBFS; the processor's output soft clipper keeps the output under -1 dBFS.
    static constexpr float voiceGain = 0.25f;

    bool canPlaySound (juce::SynthesiserSound* s) override
    {
        return dynamic_cast<NeonSynthSound*> (s) != nullptr;
    }

    void prepare (double newSampleRate, int maxBlockSize)
    {
        // Audio is stopped while preparing: let go of any sample still held.
        reader.end();
        fadingReader.end();
        fadingSamplesLeft = 0;
        cloud.end();
        resetSeeds();

        sampleRate = newSampleRate;
        adsr.setSampleRate (newSampleRate);
        adsr.reset();
        oscillator.prepare (newSampleRate);        // CHOP's Overtone glide
        randomOscillator.prepare (newSampleRate);
        sculpt.prepare (newSampleRate);

        filter.prepare (newSampleRate);
        cloudBuffer.assign ((size_t) juce::jmax (1, maxBlockSize), 0.0f);   // the granular source renders a block at a time

        level.reset (newSampleRate, 0.002);       // 2 ms: a retriggered or stolen voice changes velocity without a click
        level.setCurrentAndTargetValue (level.getTargetValue());

        cutoff.reset (newSampleRate, 0.02);       // 20 ms, multiplicative (even in octaves)
        cutoff.setCurrentAndTargetValue (clampCutoff (cutoff.getTargetValue()));
        resonance.reset (newSampleRate, 0.02);    // 20 ms, as the cutoff
        resonance.setCurrentAndTargetValue (resonance.getTargetValue());
        drive.reset (newSampleRate, 0.02);
        drive.setCurrentAndTargetValue (drive.getTargetValue());
        modeFade.reset (newSampleRate, 0.01);     // 10 ms crossfade when the mode changes
        modeFade.setCurrentAndTargetValue (1.0f);
        setFilterCoefficients();

        filterMix.reset (newSampleRate, 0.01);    // 10 ms crossfade for the on/off toggle

        pan.reset (newSampleRate, 0.02);          // 20 ms so Width changes don't zipper
        pan.setCurrentAndTargetValue (notePan * width);
        updatePanGains (pan.getCurrentValue());
    }

    /** Audio thread, once per block before rendering. */
    void setParameters (const Params& p) noexcept
    {
        adsrParams = p.adsr;
        if (! hardStopping)
            adsr.setParameters (adsrParams);

        oscillator.setWaveform (p.waveform);              // applied at the next cycle boundary
        oscillator.setChop (p.chop);
        randomOscillator.setChop (p.chop);
        sculpt.set (p.sculpt);
        cutoff.setTargetValue (clampCutoff (p.cutoffHz));
        filterMix.setTargetValue (p.filterOn ? 1.0f : 0.0f);
        resonance.setTargetValue (juce::jlimit (0.0f, 1.0f, p.filterResonance));
        drive.setTargetValue (juce::jlimit (0.0f, 1.0f, p.filterDrive));
        setFilterMode (juce::jlimit (0, 3, p.filterMode));

        width = p.width;
        pan.setTargetValue (notePan * width);

        pendingSource  = p.source;
        blockSample    = p.sample;
        sampleSettings = p.sampleSettings;
        cloud.setSettings (p.grainSettings);
        pendingGrainSettings = p.grainSettings;
    }

    void startNote (int midiNote, float velocity, juce::SynthesiserSound*, int) override
    {
        // Set the pitch directly: no glide from the previous note.
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
        notePan = juce::jlimit (-1.0f, 1.0f, (float) (midiNote - 60) / 24.0f);
        const float newLevel = velocity * voiceGain;

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

    void stopNote (float, bool allowTailOff) override
    {
        samplesSinceOff = 0;

        if (allowTailOff)
        {
            reader.keyReleased();   // Loop until release: play on to the end
            adsr.noteOff();
            return;
        }

        // Hard stop (voice stolen, all sound off): 5 ms fade instead of a cut.
        // juce::Synthesiser keeps calling renderNextBlock() on free voices, so the fade still plays.
        hardStopping = true;
        adsr.setParameters ({ 0.001f, 0.001f, 1.0f, 0.005f });
        adsr.noteOff();
        clearCurrentNote();
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    using SynthesiserVoice::renderNextBlock;   // keep the double-precision overload visible

    /**
        Renders the voice one sample at a time, in one fused loop, in one of
        two versions:

          steady   nothing is gliding (velocity level, cutoff, filter on/off
                   mix and pan all at rest, and the filter's coefficients
                   already match the cutoff - almost always the case): their
                   values are read once and every "is it smoothing?" branch
                   is out of the loop;
          gliding  any of them is moving: each is advanced per sample.

        A smoother at rest stays at rest for the whole call (targets only
        change between calls), so the choice holds for the block. Both versions
        do the same operations in the same order, so the steady version is
        bit-identical to the single loop this replaced.

        The filter's coefficients (a tan() and a division) are not updated
        every sample: while the cutoff moves they are set every 16 samples
        from modulatedCutoffHz(), and at rest not at all. The cutoff smoother
        itself still advances every sample, so its timing is unchanged. The
        16-sample countdown runs on across calls (the steady version advances
        it arithmetically), so the update points, and with them the output,
        don't depend on the host's block size or on where MIDI events split
        a block. When the cutoff comes to rest between two update points the
        last update is still pending; the voice stays in the gliding version
        until it has been made.

        The gliding version advances the level and filter-mix smoothers only
        inside its loop, once per sample. (Up to v1.7.1 it also read them once
        before the loop, so each call moved those two glides one sample
        further than it should - a velocity glide or filter switch ended a few
        samples early. Fixed in v1.8.0; the steady version, which reads them
        once on purpose, is unchanged.)

        Why not render stage by stage (whole buffers through the oscillator,
        then the envelope, then the filter...) so the compiler can vectorise
        the simple stages? That was tried, and measured 37% slower: the
        oscillator phase, the envelope and the filter state are three
        independent chains where each sample depends on the one before, and
        in one fused loop the CPU overlaps the three chains' latencies. Split
        into separate passes, each chain's latency is exposed in turn, which
        costs far more than vectorising the cheap multiplies saves. Making a
        synth voice faster in earnest means vectorising across voices (several
        voices' chains side by side in SIMD lanes), not within one.
    */
    void renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples) override
    {
        if (! adsr.isActive())
            return;

        auto* left  = output.getWritePointer (0);
        auto* right = output.getNumChannels() > 1 ? output.getWritePointer (1) : nullptr;

        // The grain cloud renders the whole stretch up front (block-wise is several times
        // cheaper than per sample); the loop below reads it like any other source.
        const bool cloudSounds = cloud.isActive();
        if (cloudSounds)
            renderCloud (numSamples);

        const bool steady = ! level.isSmoothing() && ! cutoff.isSmoothing() && ! filterMix.isSmoothing() && ! pan.isSmoothing()
                         && ! resonance.isSmoothing() && ! drive.isSmoothing() && ! modeFade.isSmoothing()
                         && ! filterUpdatePending();
        const int rendered = steady ? renderSamples<true>  (left, right, startSample, numSamples, cloudSounds)
                                    : renderSamples<false> (left, right, startSample, numSamples, cloudSounds);

        // The gliding version counts down per sample; the steady one skips ahead here.
        if (steady)
            coefficientCountdown = (coefficientCountdown - 1 + coefficientInterval - rendered % coefficientInterval)
                                       % coefficientInterval + 1;

        samplesSinceOn += rendered;
        if (samplesSinceOff >= 0)
            samplesSinceOff += rendered;
    }

private:
    /** The render loop (see renderNextBlock()); returns how many samples it rendered. */
    template <bool steady>
    int renderSamples (float* left, float* right, int startSample, int numSamples, bool cloudSounds) noexcept
    {
        // Steady: the values that would otherwise be advanced every sample, read once.
        // (getNextValue() of a smoother at rest just returns its value.) Gliding doesn't
        // touch them here: its loop advances them, once per sample.
        const float steadyLevel = steady ? level.getNextValue()     : 0.0f;
        const float steadyMix   = steady ? filterMix.getNextValue() : 0.0f;
        const float steadyLeft = gainLeft, steadyRight = gainRight;

        int rendered = 0;
        for (int i = startSample; i < startSample + numSamples; ++i, ++rendered)
        {
            if constexpr (! steady)
            {
                if (cutoff.isSmoothing())
                    cutoff.getNextValue();   // per sample, so the glide's timing is unchanged
                if (resonance.isSmoothing())
                    resonance.getNextValue();
                if (drive.isSmoothing())
                    drive.getNextValue();

                if (--coefficientCountdown == 0)
                {
                    coefficientCountdown = coefficientInterval;
                    if (filterUpdatePending())
                        setFilterCoefficients();
                }
            }

            lastEnvelope    = adsr.getNextSample();
            const float levelNow = steady ? steadyLevel : level.getNextValue();
            const float dry = nextSourceSample (cloudSounds) * levelNow * lastEnvelope;   // this order: v1.7's bits
            const float wet = filterStep<steady> (dry, levelNow * lastEnvelope);   // always run: state stays warm
            const float out = dry + (steady ? steadyMix : filterMix.getNextValue()) * (wet - dry);

            if constexpr (! steady)
                if (pan.isSmoothing())
                    updatePanGains (pan.getNextValue());

            if (right != nullptr)
            {
                left[i]  += out * (steady ? steadyLeft  : gainLeft);
                right[i] += out * (steady ? steadyRight : gainRight);
            }
            else
            {
                left[i] += out;   // mono bus: no panning
            }

            if (! adsr.isActive())
            {
                if (isVoiceActive())
                    clearCurrentNote();
                filter.reset();
                modeFade.setCurrentAndTargetValue (1.0f);
                lastEnvelope = 0.0f;
                reader.end();          // silent now: let go of the sample
                fadingReader.end();
                fadingSamplesLeft = 0;
                cloud.end();
                break;
            }

            // A one-shot sample that has played to its end: fade the voice out as for a steal.
            if (source == Source::sample && reader.hasFinished() && ! hardStopping)
                stopNote (0.0f, false);
        }
        return rendered;
    }

    /** Constant-power pan law, scaled so the centre is unity gain on both sides. */
    void updatePanGains (float p) noexcept
    {
        const float angle = (p + 1.0f) * juce::MathConstants<float>::pi * 0.25f;   // 0 .. pi/2
        gainLeft  = juce::MathConstants<float>::sqrt2 * std::cos (angle);
        gainRight = juce::MathConstants<float>::sqrt2 * std::sin (angle);
    }

    float clampCutoff (float hz) const noexcept
    {
        return juce::jlimit (20.0f, 0.45f * (float) sampleRate, hz);   // stay well below Nyquist
    }

    //==============================================================================
    // The filter. Everything that touches it goes through the functions below,
    // so the filter itself can be replaced without touching the render loop.

    /** The cutoff the filter should have right now: the (smoothed) knob, plus
        modulation in octaves, clamped. The one place the cutoff is worked out. */
    float modulatedCutoffHz() const noexcept
    {
        const float knobHz = cutoff.getCurrentValue();   // already clamped by setParameters()

        // Modulation, in octaves, adds up here: the filter envelope and velocity
        // (v1.8), the LFO and pitch bend (v1.9). Nothing yet. Anything that can
        // move it per sample must also count as "moving" in renderNextBlock(),
        // so that block renders in the gliding version.
        const float modulationOctaves = 0.0f;

        // Unmodulated, the knob value goes through untouched: a round trip through
        // log2 and exp2 can move it by a bit, and the default sound must not change.
        if (modulationOctaves == 0.0f)
            return knobHz;

        return clampCutoff (std::exp2 (std::log2 (knobHz) + modulationOctaves));
    }

    /** True if the coefficients lag the cutoff, resonance or drive (one of them moved since the last update). */
    bool filterUpdatePending() const noexcept
    {
        return modulatedCutoffHz() != coefficientCutoffHz
            || resonance.getCurrentValue() != coefficientResonance
            || drive.getCurrentValue() != coefficientDrive;
    }

    /** Sets the filter's coefficients from modulatedCutoffHz(), the resonance and
        the drive, and restarts the 16-sample countdown. The render loop calls it
        every 16 samples while one of them moves; prepare() and startNote() call it
        to set them at once. */
    void setFilterCoefficients() noexcept
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

    /** A new Filter Mode: crossfades from the old mode's output over 10 ms.
        Both come from the same filter state, so the old one costs nothing to keep. */
    void setFilterMode (int newMode) noexcept
    {
        const auto m = (CytomicSvf::Mode) newMode;
        if (m == mode)
            return;
        previousMode = mode;
        mode = m;
        modeFade.setCurrentAndTargetValue (0.0f);
        modeFade.setTargetValue (1.0f);
    }

    /** One sample through the drive and the filter, with the current coefficients.
        gain is the velocity level times the amp envelope: the limiter's ceiling
        and the drive's scale follow it, so self-oscillation and drive follow the
        envelope (see CytomicSvf.h). At resonance 0 and drive 0 this is the
        filter of v1.7, bit for bit. */
    template <bool steady>
    float filterStep (float dry, float gain) noexcept
    {
        // The floor keeps the ceiling above zero when the envelope is; below -120 dB nothing is heard.
        const float ceiling = std::max (gain, 1.0e-6f);

        float x = dry;
        if (coefficientDrive > 0.0f)
            x = filter.drive (dry, ceiling, driveSettings);
        else
            filter.trackInput (dry);

        const auto o = coefficientResonance > 0.0f ? filter.processNonlinear (x, ceiling)
                                                   : filter.processLinear (x);
        const float y = CytomicSvf::select (o, mode);

        if constexpr (steady)
            return y;   // the steady version runs only while no mode change is fading

        if (! modeFade.isSmoothing())
            return y;

        const float fade = modeFade.getNextValue();
        return CytomicSvf::select (o, previousMode) + fade * (y - CytomicSvf::select (o, previousMode));
    }

    /** Picks the note's source and starts it. */
    void startSource (int midiNote) noexcept
    {
        auto newSource = pendingSource;
        if (blockSample == nullptr && (newSource == Source::sample || newSource == Source::granular))
            newSource = Source::oscillator;

        // Retrigger or steal while the sample sounds: fade the old read position out over
        // 2 ms instead of jumping (whatever the new note plays).
        if (source == Source::sample && reader.isActive() && ! reader.hasFinished() && adsr.isActive())
        {
            reader.swapWith (fadingReader);   // fadingReader now holds the old note and its sample reference
            fadingSamplesLeft = fadingSamplesTotal = juce::jmax (1, (int) (0.002 * sampleRate));
        }

        source = newSource;
        ++noteCounter;

        if (source == Source::sample)
            reader.begin (blockSample, midiNote, sampleRate, sampleSettings);
        else
            reader.end();

        if (source == Source::granular)
        {
            const auto seed = (juce::uint32) midiNote * 2654435761u ^ (juce::uint32) (voiceIndex + 1) * 40503u
                            ^ (juce::uint32) noteCounter * 2246822519u;
            cloud.begin (blockSample, midiNote, sampleSettings.rootNote, sampleRate, cloudSettings(), seed);
        }
        else
        {
            cloud.stopSpawning();   // grains still sounding play out
        }
    }

    GrainCloud::Settings cloudSettings() const noexcept   { return pendingGrainSettings; }

    /** Renders the cloud into cloudBuffer, in chunks if the host's block is larger than prepared. */
    void renderCloud (int numSamples) noexcept
    {
        cloudSamples = juce::jmin (numSamples, (int) cloudBuffer.size());
        cloud.render (cloudBuffer.data(), cloudSamples);
        cloudRead = 0;
        if (! cloud.isSounding())
            cloud.end();
    }

    /** The current note's sound source, one sample. */
    float nextSourceSample (bool cloudSounds) noexcept
    {
        float out = source == Source::oscillator ? sculpt.process (oscillator.nextSample())      // CHOP is inside the oscillators;
                  : source == Source::sample     ? reader.nextSample()
                  : source == Source::random     ? sculpt.process (randomOscillator.nextSample()) // Noise and Fold follow it
                                                 : 0.0f;

        if (cloudSounds)
        {
            if (cloudRead >= cloudSamples && cloud.isActive())   // a block larger than prepareToPlay said
                renderCloud (cloudSamples);
            out += cloudRead < cloudSamples ? cloudBuffer[(size_t) cloudRead++] : 0.0f;
        }

        if (fadingSamplesLeft > 0)
        {
            out += fadingReader.nextSample() * ((float) fadingSamplesLeft / (float) fadingSamplesTotal);
            if (--fadingSamplesLeft == 0)
                fadingReader.end();
        }
        return out;
    }

    double sampleRate = 44100.0;
    Source source = Source::oscillator, pendingSource = Source::oscillator;
    PolyBlepOscillator oscillator;
    RandomStepOscillator randomOscillator;   // the Random source
    Sculpt sculpt;                           // Noise and Fold, after CHOP

    // Sample source
    const SampleData* blockSample = nullptr;   // this block's sample, from setParameters()
    SampleReader::Settings sampleSettings;
    SampleReader reader, fadingReader;         // fadingReader: the previous note, fading out after a retrigger
    int fadingSamplesLeft = 0, fadingSamplesTotal = 1;

    // Granular source
    GrainCloud cloud;
    GrainCloud::Settings pendingGrainSettings;
    std::vector<float> cloudBuffer = std::vector<float> (512, 0.0f);   // sized in prepare()
    int cloudSamples = 0, cloudRead = 0;
    int voiceIndex = 0, noteCounter = 0;   // seed the grains, so the same MIDI renders the same audio

public:
    /** Which voice this is (0..15), for seeding the grains and the Random source. Set once by the processor. */
    void setVoiceIndex (int index) noexcept   { voiceIndex = index; resetSeeds(); }

    /** Restarts the grain seeds and the Random source (the processor's reset()), so a render can be repeated exactly. */
    void resetSeeds() noexcept
    {
        noteCounter = 0;
        randomOscillator.reseed ((juce::uint32) (voiceIndex + 1) * 2654435761u);
        sculpt.reseed ((juce::uint32) (voiceIndex + 1) * 2246822519u);
    }

private:
    bool   hardStopping = false;

    // Telemetry for the GUI (written here on the audio thread, copied out by the processor).
    float       frequencyHz = 0.0f, lastEnvelope = 0.0f;
    juce::int64 samplesSinceOn = 0, samplesSinceOff = -1;

    VoiceEnvelope adsr;
    VoiceEnvelope::Parameters adsrParams;
    juce::SmoothedValue<float> level { 0.0f };   // velocity * voiceGain
    CytomicSvf filter;
    juce::SmoothedValue<float> resonance { 0.0f }, drive { 0.0f };   // 0..1
    CytomicSvf::Drive driveSettings;                                  // for coefficientDrive
    CytomicSvf::Mode mode = CytomicSvf::Mode::lowpass, previousMode = CytomicSvf::Mode::lowpass;
    juce::SmoothedValue<float> modeFade { 1.0f };                     // 0 -> 1 from previousMode to mode
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> cutoff { 2000.0f };
    static constexpr int coefficientInterval = 16;   // samples between coefficient updates while the cutoff moves
    int   coefficientCountdown = coefficientInterval;  // samples until the next one, 1..16; runs on across calls
    float coefficientCutoffHz = 0.0f;                  // the cutoff the coefficients were last set for
    float coefficientResonance = 0.0f, coefficientDrive = 0.0f;   // and the resonance and drive
    juce::SmoothedValue<float> filterMix { 1.0f };

    // Stereo placement
    float notePan = 0.0f, width = 0.0f, gainLeft = 1.0f, gainRight = 1.0f;
    juce::SmoothedValue<float> pan { 0.0f };
};
