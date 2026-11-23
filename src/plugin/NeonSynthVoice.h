// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "dsp/VoiceEnvelope.h"
#include "dsp/Oscillator.h"
#include "dsp/Sculpt.h"
#include "dsp/SampleReader.h"
#include "dsp/GrainCloud.h"
#include "dsp/CytomicSvf.h"
#include "dsp/Modulation.h"
#include "dsp/Wavetable.h"
#include "sequencer/StepSequencer.h"

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

    The render path (renderNextBlock() and what its loop runs every sample) is
    here, where every file that uses it sees it inline. Preparing, starting and
    stopping notes, the per-block parameters and the telemetry are in
    NeonSynthVoice.cpp (v1.11.1).
*/
class NeonSynthVoice : public juce::SynthesiserVoice
{
public:
    /** The hard stop's fade (a stolen or cut voice): the envelope's linear release, and the tail
        the processor reports to the host on top of RELEASE. */
    static constexpr double hardStopFadeSeconds = 0.005;

    /** The crossfade between two read positions of the sample (a retrigger, or a move to
        another copy of the sample), short enough not to smear an attack. */
    static constexpr double sampleCrossfadeSeconds = 0.002;

    /** A note's velocity level glides this long when a sounding voice is retriggered or stolen. */
    static constexpr double levelGlideSeconds = 0.002;

    /** Cutoff, resonance, drive, the filter's modulation amounts and the pan glide this long (20 ms). */
    static constexpr double controlGlideSeconds = 0.02;

    /** Switching the filter on or off, or its mode, crossfades this long (10 ms). */
    static constexpr double filterSwitchSeconds = 0.01;

    /** The cutoff before the parameters arrive (the Cutoff parameter's default). */
    static constexpr float defaultCutoffHz = 2000.0f;

    /** The cutoff stays inside 20 Hz .. 0.45 x the sample rate: well below Nyquist, where the filter's tan() blows up. */
    static constexpr float minCutoffHz = 20.0f;
    static constexpr float maxCutoffFraction = 0.45f;

    /** The filter envelope's range in octaves (Filter Env, -5..+5) and velocity's (Filter Vel, 0..+4). */
    static constexpr float maxFilterEnvOctaves = 5.0f;
    static constexpr float maxFilterVelOctaves = 4.0f;

    /** Pan by pitch: C4 (note 60) in the centre, the edges two octaves either side. */
    static constexpr int   panCentreNote = 60;
    static constexpr float panSpreadSemitones = 24.0f;

    /** Where a note's sound comes from (the Source parameter's order). */
    enum class Source { oscillator = 0, sample = 1, granular = 2, random = 3 };

    /** Parameter snapshot, read once per block on the audio thread. */
    struct Params
    {
        int   waveform = 0;            // PolyBlepOscillator::Waveform: 0 Sine, 1 Square, 2 Triangle, 3 Sawtooth
        float cutoffHz = defaultCutoffHz;
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

        float glideSeconds = 0.0f;                // v1.11.0, GLIDE: time to slide to a new note; 0 = jump (v1.10.1)

        // v1.12.0 "Fourier" (Oscillator source; NAIVE also Random and CHOP). Both off = v1.11's path.
        bool  naive = false;                      // NAIVE: no band-limiting, so harmonics above Nyquist alias
        bool  custom = false;                     // CUSTOM: the wavetable wave instead of `waveform`
        int   harmonics = HarmonicSpectrum::numHarmonics;   // CUSTOM: how many of the 32 play
        const WavetableSet* wavetables = nullptr; // CUSTOM: this block's tables (FourierState::acquireForAudio)
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

    Telemetry getTelemetry() const noexcept;

    // -12 dB per voice. A full-velocity 8-note square chord would peak at about
    // +5 dBFS; the processor's output soft clipper keeps the output under -1 dBFS.
    static constexpr float voiceGain = 0.25f;

    bool canPlaySound (juce::SynthesiserSound* s) override
    {
        return dynamic_cast<NeonSynthSound*> (s) != nullptr;
    }

    void prepare (double newSampleRate, int maxBlockSize);

    /** Audio thread, once per block before rendering. */
    void setParameters (const Params& p) noexcept;

    /** Audio thread, once per block after setParameters(): this block's modulation buffers (v1.9.0).
        Indexed like the block, so renderNextBlock()'s startSample is the index of its first sample.
        Null buffers mean nothing modulates; the voice then renders exactly as v1.8.0 did.
        The source path reads modulation.pitchRatio (oscillators, sample, grains);
        modulatedCutoffHz() reads modulation.cutoffOctaves. */
    void setModulation (const ModulationBlock& block) noexcept   { modulation = block; modulationIndex = 0; }

    /**
        v1.11.0, LEGATO and SLIDE: makes the next start of this voice a legato step. The synth
        (PlayingSynthesiser) calls this just before juce::Synthesiser::startVoice(), which sends
        a hard stopNote() and then startNote(): the stop is ignored, and startNote() glides the
        sounding note to the new pitch at GLIDE time (0 = a jump) without restarting the
        envelopes, the source or the velocity. A voice that has gone silent starts normally.
    */
    void prepareLegatoStep() noexcept   { legatoStepPending = true; }

    /**
        v1.11.0, GLIDE with several voices: the keyboard note played before the one about to
        start (-1 = none yet), set on every voice by PlayingSynthesiser just before a keyboard
        note-on. A keyboard note glides from there, so the glide starts from the last note you
        played, whichever voice played it (Julian, 23 Nov 2026).
    */
    void setKeyboardGlideFrom (int note) noexcept   { keyboardGlideFrom = note; }

    void startNote (int midiNote, float velocity, juce::SynthesiserSound*, int) override;

    void stopNote (float, bool allowTailOff) override;

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    using SynthesiserVoice::renderNextBlock;   // keep the double-precision overload visible

    /**
        Renders the voice one sample at a time, in one fused loop, in one of
        two versions:

          steady   nothing is gliding (velocity level, cutoff, filter on/off
                   mix and pan all at rest, the filter envelope constant or
                   set to amount 0, and the filter's coefficients
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

        // The filter envelope (see renderNextBlock()); constant ones are skipped.
        const bool advanceFilterEnvelope = ! steady && ! filterEnvelope.isConstant();

        int rendered = 0;
        for (int i = startSample; i < startSample + numSamples; ++i, ++rendered)
        {
            if constexpr (! steady)
                modulationIndex = i;   // the coefficient updates read the LFO cutoff of this sample (v1.9.0)

            if constexpr (! steady)
                advanceFilterControls (advanceFilterEnvelope);

            lastEnvelope    = adsr.getNextSample();
            const float levelNow = steady ? steadyLevel : level.getNextValue();
            const float dry = nextSourceSample (cloudSounds, i) * levelNow * lastEnvelope;   // this order: v1.7's bits
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
                endVoice();
                break;
            }

            // A one-shot sample that has played to its end: fade the voice out as for a steal.
            if (source == Source::sample && reader.hasFinished() && ! hardStopping)
                stopNote (0.0f, false);
        }
        return rendered;
    }

    /** The gliding version's per-sample control work: the smoothers that feed the filter
        advance one step, and every 16th sample the coefficients follow them. */
    void advanceFilterControls (bool advanceFilterEnvelope) noexcept
    {
        if (cutoff.isSmoothing())
            cutoff.getNextValue();   // per sample, so the glide's timing is unchanged
        if (resonance.isSmoothing())
            resonance.getNextValue();
        if (drive.isSmoothing())
            drive.getNextValue();
        if (filterEnvOctaves.isSmoothing())
            filterEnvOctaves.getNextValue();
        if (filterVelOctaves.isSmoothing())
            filterVelOctaves.getNextValue();
        if (advanceFilterEnvelope)
            filterEnvelope.getNextSample();   // per sample; read only at the update points below

        if (--coefficientCountdown == 0)
        {
            coefficientCountdown = coefficientInterval;
            if (filterUpdatePending())
                setFilterCoefficients();
        }
    }

    /** The amp envelope has ended: the voice goes silent and lets go of what it holds. */
    void endVoice() noexcept
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

    /** Constant-power pan law, scaled so the centre is unity gain on both sides. */
    void updatePanGains (float p) noexcept
    {
        const float angle = (p + 1.0f) * juce::MathConstants<float>::pi * 0.25f;   // 0 .. pi/2
        gainLeft  = juce::MathConstants<float>::sqrt2 * std::cos (angle);
        gainRight = juce::MathConstants<float>::sqrt2 * std::sin (angle);
    }

    float clampCutoff (float hz) const noexcept
    {
        return juce::jlimit (minCutoffHz, maxCutoffFraction * (float) sampleRate, hz);   // stay well below Nyquist
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
        // (v1.8) and the LFO (v1.9). Anything that can move it per
        // sample must also count as "moving" in renderNextBlock() (see
        // filterEnvelopeMovesCutoff()), so that block renders in the gliding
        // version. Velocity is constant for a note: a retriggered voice's new
        // velocity is seen by filterUpdatePending() and applied at the next update.
        float modulationOctaves = filterEnvOctaves.getCurrentValue() * filterEnvelope.getLevel()
                                + filterVelOctaves.getCurrentValue() * noteVelocity;

        // The LFO CUTOFF (v1.9), at the sample being rendered. With no LFO cutoff this block the
        // buffer is null and nothing is added: the sum above is v1.8's, bit for bit.
        if (modulation.cutoffOctaves != nullptr && modulation.numSamples > 0)
            modulationOctaves += modulation.cutoffOctaves[juce::jlimit (0, modulation.numSamples - 1, modulationIndex)];

        // Unmodulated, the knob value goes through untouched: a round trip through
        // log2 and exp2 can move it by a bit, and the default sound must not change.
        if (modulationOctaves == 0.0f)
            return knobHz;

        return clampCutoff (std::exp2 (std::log2 (knobHz) + modulationOctaves));
    }

    /** True while the filter envelope sweeps the cutoff (it is in its attack, decay or release,
        or its sustain level slews, and its amount isn't 0), or either amount glides to a new
        setting, or the LFO moves the cutoff this block (v1.9): coefficients must follow. */
    bool filterEnvelopeMovesCutoff() const noexcept
    {
        return (filterEnvOctaves.getCurrentValue() != 0.0f && ! filterEnvelope.isConstant())
            || filterEnvOctaves.isSmoothing() || filterVelOctaves.isSmoothing()
            || modulation.cutoffOctaves != nullptr;
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
    void setFilterMode (int newMode) noexcept;

    /** One sample through the drive and the filter, with the current coefficients.
        gain is the velocity level times the amp envelope: the limiter's ceiling
        and the drive's scale follow it, so self-oscillation and drive follow the
        envelope (see CytomicSvf.h). At resonance 0 and drive 0 this is the
        filter of v1.7, bit for bit. */
    template <bool steady>
    float filterStep (float dry, float gain) noexcept
    {
        // The floor keeps the ceiling above zero when the envelope is; below -120 dB nothing is heard.
        constexpr float minCeiling = 1.0e-6f;
        const float ceiling = std::max (gain, minCeiling);

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
    void startSource (int midiNote) noexcept;

    /**
        Renders the cloud into cloudBuffer, in chunks if the host's block is larger than
        prepared. firstIndex is the block index of its first sample, for the pitch ratio.

        Under pitch modulation the cloud is rendered 32 samples at a time, each chunk at
        the ratio of its first sample: new grains start at that pitch and sounding ones
        follow it, smoothly enough for vibrato and bends (32 samples is under 1 ms) while
        the grain loop keeps its long stretches. Without it, one call as in v1.8.0.
    */
    void renderCloud (int firstIndex, int numSamples) noexcept
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

    bool isModulated() const noexcept        { return modulation.pitchRatio != nullptr && modulation.numSamples > 0; }

    /** True if anything moves the voice's pitch away from its note: the modulation (v1.9.0) or a glide (v1.11.0). */
    bool isPitchModulated() const noexcept   { return isModulated() || glideActive(); }

    /** The modulation's pitch ratio at a block index (only while isModulated()); past the buffer, its last value. */
    float pitchAt (int index) const noexcept
    {
        return modulation.pitchRatio[juce::jlimit (0, modulation.numSamples - 1, index)];
    }

    /**
        The voice's shared pitch, as a ratio to its note, at a block index (only while
        isPitchModulated()): the modulation times the glide, `samplesAhead` samples after the
        glide's current position. Every source reads it (oscillators, sample, grains), so they
        all glide together (v1.11.0). Without a glide it is pitchAt() exactly, bit for bit.
    */
    float pitchRatioAt (int index, int samplesAhead) const noexcept
    {
        const float modulated = isModulated() ? pitchAt (index) : 1.0f;
        if (! glideActive())
            return modulated;
        const float glide = samplesAhead == 0 ? glideRatio : std::exp2 (glideOctavesAhead (samplesAhead));
        return modulated * glide;
    }

    /** The current note's sound source, one sample; index is its position in the processor's block. */
    float nextSourceSample (bool cloudSounds, int index) noexcept
    {
        if (! isPitchModulated())
            return sourceSample<false> (cloudSounds, 1.0f, index);   // v1.8.0's path, operation for operation
        const float ratio = pitchRatioAt (index, 0);
        advanceGlide();   // v1.11.0: one sample on (nothing happens unless a glide is moving)
        return sourceSample<true> (cloudSounds, ratio, index);
    }

    //==============================================================================
    // GLIDE (v1.11.0). The pitch is kept as the note the source started on (glideBaseNote,
    // whose frequency the oscillators and the sample reader were set to) plus an offset in
    // octaves, which moves in a straight line to its target: equal steps in octaves, so a
    // glide sounds even, and a step size worked out from the distance, so any interval takes
    // GLIDE time. A normal note starts at the offset of where the voice was and glides to 0;
    // a legato step keeps its source and glides the offset to the new note. At rest on 0
    // (always, with GLIDE at 0 and LEGATO off) the voice renders exactly as v1.10.1 did.

    bool glideActive() const noexcept   { return glideOctaves != 0.0f || glideTargetOctaves != 0.0f; }

    /** The pitch the voice is at now, as a (fractional) MIDI note. */
    float currentPitchNote() const noexcept   { return (float) glideBaseNote + 12.0f * glideOctaves; }

    /** Starts a glide of the offset from `fromOctaves` to `toOctaves` in glideSeconds (0 = jump). */
    void beginGlide (float fromOctaves, float toOctaves) noexcept;

    /** A legato step: the sounding note glides to midiNote, nothing restarts. */
    void glideToNote (int midiNote) noexcept;

    /** One sample along the glide. */
    void advanceGlide() noexcept
    {
        if (glideOctaves == glideTargetOctaves)
            return;
        glideOctaves = glideOctavesAhead (1);
        glideRatio = glideOctaves == 0.0f ? 1.0f : std::exp2 (glideOctaves);
    }

    /** The offset `samples` samples from now (the grain cloud renders ahead of the loop). */
    float glideOctavesAhead (int samples) const noexcept
    {
        const float distance = glideTargetOctaves - glideOctaves;
        const float travel = glideStepOctaves * (float) samples;
        if (std::abs (distance) <= travel)
            return glideTargetOctaves;
        return glideOctaves + (distance > 0.0f ? travel : -travel);
    }

    /** Jumps to where the glide is going (the voice went silent, or the sample rate changed). */
    void finishGlide() noexcept
    {
        glideOctaves = glideTargetOctaves;
        glideRatio = glideOctaves == 0.0f ? 1.0f : std::exp2 (glideOctaves);
    }

    /** One sample of the source: at the note's own pitch, or (pitched) times `ratio`. */
    template <bool pitched>
    float sourceSample (bool cloudSounds, float ratio, int index) noexcept
    {
        if constexpr (pitched)
        {
            // The read rate has crossed into another copy's range: move there (see settleSampleCopy()).
            if (source == Source::sample && reader.wantsOtherLevel (ratio))
                settleSampleCopy (ratio, true);
            sampleNoteFresh = false;
        }

        float out = source == Source::oscillator ? sculpt.process (pitched ? oscillator.nextSample (ratio) : oscillator.nextSample())    // CHOP is inside the oscillators;
                  : source == Source::sample     ? readSample<pitched> (ratio)
                  : source == Source::random     ? sculpt.process (pitched ? randomOscillator.nextSample (ratio) : randomOscillator.nextSample())   // Noise and Fold follow it
                                                 : 0.0f;

        if (cloudSounds)
        {
            if (cloudRead >= cloudSamples && cloud.isActive())   // a block larger than prepareToPlay said
                renderCloud (index, cloudSamples);
            out += cloudRead < cloudSamples ? cloudBuffer[(size_t) cloudRead++] : 0.0f;
        }

        if (fadingSamplesLeft > 0)
        {
            out += (pitched ? fadingReader.nextSample (ratio) : fadingReader.nextSample()) * ((float) fadingSamplesLeft / (float) fadingSamplesTotal);
            if (--fadingSamplesLeft == 0)
            {
                fadingReader.end();
                fadingCopy = false;
            }
        }
        return out;
    }

    /** The sample source, one sample; while it moves to another copy it fades in as the old copy fades out. */
    template <bool pitched>
    float readSample (float ratio) noexcept
    {
        const float x = pitched ? reader.nextSample (ratio) : reader.nextSample();
        if (! fadingCopy)
            return x;
        return x * (1.0f - (float) fadingSamplesLeft / (float) fadingSamplesTotal);
    }

    /**
        Moves the sample to the copy that suits `ratio` if it isn't on it (with the
        hysteresis, or without it: back exactly to the copy the note started on).

        A note that hasn't sounded yet just moves. A sounding one crossfades over
        2 ms through the retrigger crossfade's reader: the old copy fades out in
        fadingReader while the new one fades in, both reading the same point in the
        sound at the same rate (the copies are sample-aligned, so the fade has
        nothing to smear). If that crossfade is busy, the move waits for it.
        Switching rather than reading on from the old copy keeps a bend up from
        aliasing, and a bend down from losing its top end.
    */
    void settleSampleCopy (float ratio, bool withHysteresis) noexcept
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
    bool fadingCopy = false;                   // the fade is a move to another copy: reader fades in as fadingReader fades out
    bool sampleNoteFresh = false;              // the note hasn't rendered a sample yet: a copy move needs no crossfade

    // Granular source
    GrainCloud cloud;
    static constexpr size_t unpreparedCloudSize = 512;   // until prepare() sizes it for the host's block
    std::vector<float> cloudBuffer = std::vector<float> (unpreparedCloudSize, 0.0f);   // sized in prepare()
    int cloudSamples = 0, cloudRead = 0;
    static constexpr int cloudChunk = 32;   // samples per cloud render under pitch modulation
    int voiceIndex = 0, noteCounter = 0;   // seed the grains, so the same MIDI renders the same audio

public:
    /** Which voice this is (0..15), for seeding the grains and the Random source. Set once by the processor. */
    void setVoiceIndex (int index) noexcept   { voiceIndex = index; resetSeeds(); }

    /** Restarts the grain seeds and the Random source (the processor's reset()), so a render can be repeated exactly. */
    void resetSeeds() noexcept;

private:
    bool   hardStopping = false;

    // GLIDE and legato (v1.11.0); see beginGlide().
    float glideSeconds = 0.0f;                      // from the GLIDE parameter
    static constexpr int firstGlideBaseNote = 60;   // before a voice's first note, and after resetSeeds(): unused until then
    int   glideBaseNote = firstGlideBaseNote;       // the note the source was started on
    bool  hasGlidePitch = false;                    // the voice has played a note to glide from
    float glideOctaves = 0.0f, glideTargetOctaves = 0.0f;   // offset from glideBaseNote, now and where it goes
    float glideStepOctaves = 0.0f;                  // per sample
    float glideRatio = 1.0f;                        // exp2 (glideOctaves)
    bool  legatoStepPending = false;                // see prepareLegatoStep()
    int   keyboardGlideFrom = -1;                   // see setKeyboardGlideFrom()

    // Telemetry for the GUI (written here on the audio thread, copied out by the processor).
    float       frequencyHz = 0.0f, lastEnvelope = 0.0f;
    float       shownPitchRatio = 1.0f;   // the pitch ratio at the end of the last block, for frequencyHz in the telemetry
    juce::int64 samplesSinceOn = 0, samplesSinceOff = -1;

    VoiceEnvelope adsr;
    VoiceEnvelope::Parameters adsrParams;
    VoiceEnvelope filterEnvelope;                    // gated with adsr, but never ends the voice
    juce::SmoothedValue<float> filterEnvOctaves { 0.0f }, filterVelOctaves { 0.0f };   // octaves at the envelope's peak / full velocity
    float noteVelocity = 0.0f;                       // the current note's velocity, 0..1
    juce::SmoothedValue<float> level { 0.0f };   // velocity * voiceGain
    CytomicSvf filter;
    juce::SmoothedValue<float> resonance { 0.0f }, drive { 0.0f };   // 0..1
    CytomicSvf::Drive driveSettings;                                  // for coefficientDrive
    CytomicSvf::Mode mode = CytomicSvf::Mode::lowpass, previousMode = CytomicSvf::Mode::lowpass;
    juce::SmoothedValue<float> modeFade { 1.0f };                     // 0 -> 1 from previousMode to mode
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> cutoff { defaultCutoffHz };
    static constexpr int coefficientInterval = 16;   // samples between coefficient updates while the cutoff moves
    int   coefficientCountdown = coefficientInterval;  // samples until the next one, 1..16; runs on across calls
    float coefficientCutoffHz = 0.0f;                  // the cutoff the coefficients were last set for
    float coefficientResonance = 0.0f, coefficientDrive = 0.0f;   // and the resonance and drive
    juce::SmoothedValue<float> filterMix { 1.0f };

    // Stereo placement
    float notePan = 0.0f, width = 0.0f, gainLeft = 1.0f, gainRight = 1.0f;
    juce::SmoothedValue<float> pan { 0.0f };

    // This block's modulation (v1.9.0), from setModulation().
    ModulationBlock modulation;
    int modulationIndex = 0;   // the sample of the block being rendered (or about to be): where modulatedCutoffHz() reads the LFO
};
