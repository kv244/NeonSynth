// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "plugin/NeonSynthProcessor.h"
#include "gui/NeonSynthEditor.h"

namespace
{
    /**
        Log path from NEONSYNTH_MIDILOG, or (Standalone only) from "--midilog <file>".
        BASICSYNTH_MIDILOG, the name before v1.4, still works.
    */
    juce::String findMidiLogPath (juce::AudioProcessor::WrapperType wrapper)
    {
        auto path = juce::SystemStats::getEnvironmentVariable ("NEONSYNTH_MIDILOG", {});
        if (path.isEmpty())
            path = juce::SystemStats::getEnvironmentVariable ("BASICSYNTH_MIDILOG", {});

        if (path.isEmpty() && wrapper == juce::AudioProcessor::wrapperType_Standalone)
        {
            const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
            const auto index = args.indexOf ("--midilog");

            if (index >= 0 && index + 1 < args.size())
                path = args[index + 1].unquoted();
        }

        return path;
    }

    /**
        Each plugin instance that logs gets its own file, so instances never
        delete or overwrite each other's log: the first uses the path as given
        (relative to the working directory), later ones in the same process add
        "-2", "-3", ... before the extension (midi.log, midi-2.log, ...).
    */
    juce::File midiLogFileForNewInstance (const juce::String& path)
    {
        static std::atomic<int> loggingInstances { 0 };
        const int instance = ++loggingInstances;

        auto file = juce::File::getCurrentWorkingDirectory().getChildFile (path);
        if (instance > 1)
            file = file.getSiblingFile (file.getFileNameWithoutExtension() + "-" + juce::String (instance)
                                        + file.getFileExtension());
        return file;
    }

    template <typename ParamType>
    ParamType* getTypedParameter (juce::AudioProcessorValueTreeState& state, const char* id)
    {
        auto* p = dynamic_cast<ParamType*> (state.getParameter (id));
        if (p == nullptr)
        {
            // A wrong ID or type here is a programming error that would otherwise surface as a
            // null dereference somewhere in the audio code, in release builds too. This runs once,
            // in the constructor, so stopping loudly is safe: every test that builds the processor
            // (ParameterLayoutTests first) then fails at once, never a user's session.
            jassertfalse;
            std::fprintf (stderr, "NeonSynth: no parameter \"%s\" of the expected type\n", id);
            std::abort();
        }
        return p;
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout NeonSynthProcessor::createParameterLayout()
{
    using namespace juce;

    AudioProcessorValueTreeState::ParameterLayout layout;

    // Choice instead of Int: hosts show names. The stored value is still 0-3, so sessions load unchanged.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "waveform", 1 }, "Waveform",
                                                         StringArray { "Sine", "Square", "Triangle", "Sawtooth" }, 0));

    NormalisableRange<float> cutoffRange (20.0f, 20000.0f);
    cutoffRange.setSkewForCentre (1000.0f);   // half the knob travel below 1 kHz
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "cutoff", 1 }, "Cutoff", cutoffRange, 2000.0f));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "filterEnabled", 1 }, "Filter Enabled", true));

    // No unit labels: pedalboard (test_midi.py) appends them to parameter names ("cutoff_hz").
    const auto timeRange = [] (float max) { return NormalisableRange<float> (0.001f, max, 0.001f, 0.3f); };

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "attack",  1 }, "Attack",  timeRange (5.0f),  0.01f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "decay",   1 }, "Decay",   timeRange (5.0f),  0.10f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "sustain", 1 }, "Sustain", 0.0f, 1.0f, 0.7f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "release", 1 }, "Release", timeRange (10.0f), 0.30f));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "gain", 1 }, "Output Gain (dB)",
                                                        NormalisableRange<float> (-48.0f, 6.0f, 0.1f), 0.0f));

    // Stereo spread of voices by pitch: 0 = mono, 1 = notes two octaves from middle C hard-panned.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "width", 1 }, "Stereo Width", 0.0f, 1.0f, 0.6f));

    // Step sequencer. Off by default so the plugin behaves exactly as before until enabled.
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { "seqOn",   1 }, "Sequencer On",   false));
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { "seqSync", 1 }, "Sequencer Sync", true));    // follow the host transport when it has one
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "seqTempo", 1 }, "Sequencer Tempo",
                                                         NormalisableRange<float> (40.0f, 240.0f, 0.1f), 120.0f));   // BPM, internal clock
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "seqRate", 1 }, "Sequencer Rate",
                                                         StringArray { "1/8", "1/16", "1/32", "1/8T", "1/16T" }, 1));  // order matches StepSequencer::Rate
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "seqSwing", 1 }, "Sequencer Swing", 0.0f, 1.0f, 0.0f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "seqGate",  1 }, "Sequencer Gate",
                                                         NormalisableRange<float> (0.05f, 1.0f), 0.5f));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "seqMode", 1 }, "Sequencer Mode",
                                                         StringArray { "Layer", "Replace", "Transpose" }, 0));        // order matches SequencerMode

    // Sound source (v1.5.0). A separate parameter rather than more Waveform choices: adding
    // items to Waveform would move its existing choices' normalised values and break
    // automation recorded with v1.4. New parameters go at the end so old sessions map cleanly.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "source", 1 }, "Source",
                                                         StringArray { "Oscillator", "Sample", "Granular", "Random" }, 0));   // order matches NeonSynthVoice::Source

    // Sample playback; positions are fractions of the sample's length.
    layout.add (std::make_unique<AudioParameterInt>    (ParameterID { "smpRoot", 1 }, "Sample Root", 0, 127, 60));   // MIDI note the file plays at its own pitch
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "smpStart", 1 }, "Sample Start", 0.0f, 1.0f, 0.0f));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "smpLoop", 1 }, "Sample Loop",
                                                         StringArray { "One-shot", "Loop", "Loop until release" }, 1));  // order matches SampleReader::LoopMode
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "smpLoopStart", 1 }, "Sample Loop Start", 0.0f, 1.0f, 0.0f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "smpLoopEnd", 1 }, "Sample Loop End", 0.0f, 1.0f, 1.0f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "smpXfade", 1 }, "Sample Crossfade",
                                                         NormalisableRange<float> (5.0f, 50.0f, 0.1f), 10.0f));       // milliseconds

    // Granular. Position stays live; the others shape each grain as it starts.
    NormalisableRange<float> grainSizeRange (5.0f, 500.0f, 0.1f);
    grainSizeRange.setSkewForCentre (80.0f);
    NormalisableRange<float> grainDensityRange (1.0f, 200.0f, 0.1f);
    grainDensityRange.setSkewForCentre (30.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "grPos", 1 }, "Grain Position", 0.0f, 1.0f, 0.5f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "grScan", 1 }, "Grain Scan",
                                                        NormalisableRange<float> (-2.0f, 2.0f, 0.01f), 0.0f));          // x the file's speed; 0 freezes
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "grSize", 1 }, "Grain Size", grainSizeRange, 80.0f));            // ms
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "grDensity", 1 }, "Grain Density", grainDensityRange, 30.0f));   // grains per second
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "grSpray", 1 }, "Grain Spray", 0.0f, 1.0f, 0.1f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "grJitter", 1 }, "Grain Pitch Jitter",
                                                        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f));          // cents

    // Waveform sculpting (v1.6.0), for the Oscillator and Random sources. Each default leaves the
    // sound as it was, so sessions saved before v1.6.0 render the same.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "chopOvertone", 1 }, "Chop Overtone",
                                                        NormalisableRange<float> (0.0f, 200.0f, 0.1f), 0.0f));          // %: chopped steps x (1 - Overtone/100)
    layout.add (std::make_unique<AudioParameterInt>   (ParameterID { "chopComb", 1 }, "Chop Comb", 1, 32, 1));      // pattern repeats per cycle
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "noiseLevel", 1 }, "Noise",
                                                        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f));          // %: crossfade to white noise
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "foldDrive", 1 }, "Fold Drive",
                                                        NormalisableRange<float> (1.0f, 10.0f, 0.01f), 3.0f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "foldMix", 1 }, "Fold Mix",
                                                        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f));          // %: 0 skips the fold

    // Filter character (v1.8.0). The defaults (Low-pass, resonance 0, drive 0) are the filter of
    // v1.7.1, so sessions saved before v1.8.0 render the same.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "filterMode", 1 }, "Filter Mode",
                                                         StringArray { "Low-pass", "High-pass", "Band-pass", "Notch" }, 0));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "filterRes", 1 }, "Filter Resonance",
                                                         NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f));         // %: self-oscillates near the top
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "filterDrive", 1 }, "Filter Drive",
                                                         NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f));         // %: 0 keeps the filter linear

    // Filter envelope and velocity (v1.8.0): octaves added to the cutoff. Both amounts default
    // to 0, where the cutoff is the knob's alone; the envelope's times then change nothing.
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "filterEnvAmt", 1 }, "Filter Env Amount",
                                                         NormalisableRange<float> (-5.0f, 5.0f, 0.01f), 0.0f));        // octaves at the envelope's peak
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "filterAttack",  1 }, "Filter Attack",  timeRange (5.0f),  0.01f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "filterDecay",   1 }, "Filter Decay",   timeRange (5.0f),  0.30f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "filterSustain", 1 }, "Filter Sustain", 0.0f, 1.0f, 0.5f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "filterRelease", 1 }, "Filter Release", timeRange (10.0f), 0.30f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "filterVel", 1 }, "Filter Velocity",
                                                         NormalisableRange<float> (0.0f, 4.0f, 0.01f), 0.0f));         // octaves at full velocity

    // LFO (v1.9.0): one for the whole synth (Lfo.h, Modulation.h). Its three depths default to 0,
    // where it changes nothing, so sessions saved before v1.9.0 render the same.
    NormalisableRange<float> lfoRateRange (0.02f, 20.0f, 0.001f);
    lfoRateRange.setSkewForCentre (1.0f);
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "lfoShape", 1 }, "LFO Shape",
                                                         StringArray { "Sine", "Triangle", "Square", "Saw", "Random" }, 0));   // order matches Lfo::Shape
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "lfoRate", 1 }, "LFO Rate", lfoRateRange, 1.0f));          // Hz
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { "lfoSync", 1 }, "LFO Sync", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "lfoDivision", 1 }, "LFO Division",
                                                         StringArray { "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/8T", "1/16T" }, 4));   // order matches Lfo::Division
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "lfoPitch", 1 }, "LFO Pitch",
                                                         NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f));      // semitones
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "lfoCutoff", 1 }, "LFO Cutoff",
                                                         NormalisableRange<float> (-5.0f, 5.0f, 0.01f), 0.0f));        // octaves
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "lfoAmp", 1 }, "LFO Amp",
                                                         NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f));        // %: tremolo depth, only dips

    // Pitch bend and mod wheel (v1.9.0). They act only on incoming MIDI pitch bend and CC 1,
    // so sessions without those messages render the same.
    layout.add (std::make_unique<AudioParameterInt>    (ParameterID { "bendRange", 1 }, "Pitch Bend Range", 0, 24, 2));   // semitones each way
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "vibrato", 1 }, "Vibrato Depth",
                                                         NormalisableRange<float> (0.0f, 100.0f, 0.1f), 30.0f));      // cents at the wheel's top

    // Voices (v1.9.0): how many of the 16 voices may play, 1 = mono. Set by clicking the VOICES
    // lights. The default, 16, is the full pool, so sessions saved before v1.9.0 play the same.
    layout.add (std::make_unique<AudioParameterInt>    (ParameterID { "voiceCount", 1 }, "Voices", 1, NeonSynthProcessor::numVoices, NeonSynthProcessor::numVoices));

    // v1.11.0: playing (hold, glide, legato) and the sequencer's pattern length and scale lock.
    // Every default leaves sessions saved before v1.11.0 sounding exactly as they did.
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { "hold", 1 }, "Hold", false));          // keyboard notes held after release
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "glide", 1 }, "Glide",                 // ms to slide to a new note; 0 = off
                                                         NormalisableRange<float> (0.0f, 2000.0f, 1.0f, 0.4f), 0.0f));
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { "legato", 1 }, "Legato", false));      // with Voices at 1: overlapping keys glide, no retrigger
    layout.add (std::make_unique<AudioParameterInt>    (ParameterID { "seqLength", 1 }, "Sequencer Length", 1, StepSequencer::numSteps, StepSequencer::numSteps));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "seqKey", 1 }, "Sequencer Key",
                                                         StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "seqScale", 1 }, "Sequencer Scale",   // Off = no lock; order matches StepSequencer::Scale
                                                         StringArray { "Off", "Major", "Dorian", "Phrygian", "Lydian", "Mixolydian",
                                                                       "Natural minor", "Locrian", "Harmonic minor", "Major pentatonic",
                                                                       "Minor pentatonic", "Blues", "Whole tone" }, 0));   // complete as shipped (roadmap rule 3)
    return layout;
}

NeonSynthProcessor::NeonSynthProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createParameterLayout())
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

    waveformParameter      = getTypedParameter<juce::AudioParameterChoice> (apvts, "waveform");
    cutoffParameter        = getTypedParameter<juce::AudioParameterFloat>  (apvts, "cutoff");
    filterEnabledParameter = getTypedParameter<juce::AudioParameterBool>   (apvts, "filterEnabled");
    attackParameter        = getTypedParameter<juce::AudioParameterFloat>  (apvts, "attack");
    decayParameter         = getTypedParameter<juce::AudioParameterFloat>  (apvts, "decay");
    sustainParameter       = getTypedParameter<juce::AudioParameterFloat>  (apvts, "sustain");
    releaseParameter       = getTypedParameter<juce::AudioParameterFloat>  (apvts, "release");
    gainParameter          = getTypedParameter<juce::AudioParameterFloat>  (apvts, "gain");
    widthParameter         = getTypedParameter<juce::AudioParameterFloat>  (apvts, "width");
    seqOnParameter         = getTypedParameter<juce::AudioParameterBool>   (apvts, "seqOn");
    seqSyncParameter       = getTypedParameter<juce::AudioParameterBool>   (apvts, "seqSync");
    seqTempoParameter      = getTypedParameter<juce::AudioParameterFloat>  (apvts, "seqTempo");
    seqRateParameter       = getTypedParameter<juce::AudioParameterChoice> (apvts, "seqRate");
    seqSwingParameter      = getTypedParameter<juce::AudioParameterFloat>  (apvts, "seqSwing");
    seqGateParameter       = getTypedParameter<juce::AudioParameterFloat>  (apvts, "seqGate");
    seqModeParameter       = getTypedParameter<juce::AudioParameterChoice> (apvts, "seqMode");
    sourceParameter        = getTypedParameter<juce::AudioParameterChoice> (apvts, "source");
    smpRootParameter       = getTypedParameter<juce::AudioParameterInt>    (apvts, "smpRoot");
    smpStartParameter      = getTypedParameter<juce::AudioParameterFloat>  (apvts, "smpStart");
    smpLoopParameter       = getTypedParameter<juce::AudioParameterChoice> (apvts, "smpLoop");
    smpLoopStartParameter  = getTypedParameter<juce::AudioParameterFloat>  (apvts, "smpLoopStart");
    smpLoopEndParameter    = getTypedParameter<juce::AudioParameterFloat>  (apvts, "smpLoopEnd");
    smpXfadeParameter      = getTypedParameter<juce::AudioParameterFloat>  (apvts, "smpXfade");
    grPosParameter         = getTypedParameter<juce::AudioParameterFloat>  (apvts, "grPos");
    grScanParameter        = getTypedParameter<juce::AudioParameterFloat>  (apvts, "grScan");
    grSizeParameter        = getTypedParameter<juce::AudioParameterFloat>  (apvts, "grSize");
    grDensityParameter     = getTypedParameter<juce::AudioParameterFloat>  (apvts, "grDensity");
    grSprayParameter       = getTypedParameter<juce::AudioParameterFloat>  (apvts, "grSpray");
    grJitterParameter      = getTypedParameter<juce::AudioParameterFloat>  (apvts, "grJitter");
    chopOvertoneParameter  = getTypedParameter<juce::AudioParameterFloat>  (apvts, "chopOvertone");
    chopCombParameter      = getTypedParameter<juce::AudioParameterInt>    (apvts, "chopComb");
    noiseLevelParameter    = getTypedParameter<juce::AudioParameterFloat>  (apvts, "noiseLevel");
    foldDriveParameter     = getTypedParameter<juce::AudioParameterFloat>  (apvts, "foldDrive");
    foldMixParameter       = getTypedParameter<juce::AudioParameterFloat>  (apvts, "foldMix");
    filterModeParameter    = getTypedParameter<juce::AudioParameterChoice> (apvts, "filterMode");
    filterResParameter     = getTypedParameter<juce::AudioParameterFloat>  (apvts, "filterRes");
    filterDriveParameter   = getTypedParameter<juce::AudioParameterFloat>  (apvts, "filterDrive");
    filterEnvAmtParameter  = getTypedParameter<juce::AudioParameterFloat>  (apvts, "filterEnvAmt");
    filterAttackParameter  = getTypedParameter<juce::AudioParameterFloat>  (apvts, "filterAttack");
    filterDecayParameter   = getTypedParameter<juce::AudioParameterFloat>  (apvts, "filterDecay");
    filterSustainParameter = getTypedParameter<juce::AudioParameterFloat>  (apvts, "filterSustain");
    filterReleaseParameter = getTypedParameter<juce::AudioParameterFloat>  (apvts, "filterRelease");
    filterVelParameter     = getTypedParameter<juce::AudioParameterFloat>  (apvts, "filterVel");
    lfoShapeParameter      = getTypedParameter<juce::AudioParameterChoice> (apvts, "lfoShape");
    lfoRateParameter       = getTypedParameter<juce::AudioParameterFloat>  (apvts, "lfoRate");
    lfoSyncParameter       = getTypedParameter<juce::AudioParameterBool>   (apvts, "lfoSync");
    lfoDivisionParameter   = getTypedParameter<juce::AudioParameterChoice> (apvts, "lfoDivision");
    lfoPitchParameter      = getTypedParameter<juce::AudioParameterFloat>  (apvts, "lfoPitch");
    lfoCutoffParameter     = getTypedParameter<juce::AudioParameterFloat>  (apvts, "lfoCutoff");
    lfoAmpParameter        = getTypedParameter<juce::AudioParameterFloat>  (apvts, "lfoAmp");
    bendRangeParameter     = getTypedParameter<juce::AudioParameterInt>    (apvts, "bendRange");
    vibratoParameter       = getTypedParameter<juce::AudioParameterFloat>  (apvts, "vibrato");
    voiceCountParameter    = getTypedParameter<juce::AudioParameterInt>    (apvts, "voiceCount");
    holdParameter          = getTypedParameter<juce::AudioParameterBool>   (apvts, "hold");
    glideParameter         = getTypedParameter<juce::AudioParameterFloat>  (apvts, "glide");
    legatoParameter        = getTypedParameter<juce::AudioParameterBool>   (apvts, "legato");
    seqLengthParameter     = getTypedParameter<juce::AudioParameterInt>    (apvts, "seqLength");
    seqKeyParameter        = getTypedParameter<juce::AudioParameterChoice> (apvts, "seqKey");
    seqScaleParameter      = getTypedParameter<juce::AudioParameterChoice> (apvts, "seqScale");

    loadDefaultPattern();
    publishSample (SampleLoader::makeDefaultSample());

    if (const auto path = findMidiLogPath (wrapperType); path.isNotEmpty())
        midiLogger.start (midiLogFileForNewInstance (path));
}

NeonSynthProcessor::~NeonSynthProcessor()
{
    sampleLoader.removeAllJobs (true, 10000);   // a running load finishes before the slot it publishes into goes away
}

void NeonSynthProcessor::reset()
{
    // Repeating a render from here gives the same grains (hosts call this before offline renders).
    for (auto* v : voices)
        v->resetSeeds();
    modulation.reset();   // LFO phase 0, bend and wheel centred
}

void NeonSynthProcessor::releaseResources()
{
    sampleSlot.setAudioRunning (false);   // nothing reads the sample while stopped: replaced ones can be freed now
}

void NeonSynthProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    synth.setCurrentPlaybackSampleRate (sampleRate);

    for (auto* v : voices)
        v->prepare (sampleRate, samplesPerBlock);

    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (gainParameter->get()));

    midiLogger.setSampleRate (sampleRate);
    modulation.prepare (sampleRate, samplesPerBlock);

    sequencer.prepare (sampleRate);
    playMidi.ensureSize (8192);   // bytes; far more than a block of keyboard + sequencer events needs
    internalBeat   = 0.0;
    numHeldKeys    = 0;

    sampleSlot.setAudioRunning (true);
}

void NeonSynthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();

    // The plugin has no input bus: the buffer arrives with no incoming audio.
    buffer.clear();
    midiLogger.push (midiMessages, samplesProcessed);   // lock-free; formatting/I/O happen elsewhere
    blockSample = sampleSlot.beginBlock();   // the voices' sample for this block (Sample and Granular sources)

    NeonSynthVoice::Params params;
    params.waveform = waveformParameter->getIndex();
    params.cutoffHz = cutoffParameter->get();
    params.filterOn = filterEnabledParameter->get();
    params.filterMode      = filterModeParameter->getIndex();
    params.filterResonance = filterResParameter->get() * 0.01f;
    params.filterDrive     = filterDriveParameter->get() * 0.01f;
    params.filterEnvOctaves = filterEnvAmtParameter->get();
    params.filterEnv        = { filterAttackParameter->get(), filterDecayParameter->get(),
                                filterSustainParameter->get(), filterReleaseParameter->get() };
    params.filterVelOctaves = filterVelParameter->get();
    params.width    = widthParameter->get();
    params.adsr     = { attackParameter->get(), decayParameter->get(),
                        sustainParameter->get(), releaseParameter->get() };

    params.source = (NeonSynthVoice::Source) sourceParameter->getIndex();
    params.sample = blockSample;
    params.sampleSettings = { smpRootParameter->get(), smpStartParameter->get(), smpLoopParameter->getIndex(),
                              smpLoopStartParameter->get(), smpLoopEndParameter->get(), smpXfadeParameter->get() * 0.001f };
    params.grainSettings  = { grPosParameter->get(), grScanParameter->get(), grSizeParameter->get() * 0.001f,
                              grDensityParameter->get(), grSprayParameter->get(), grJitterParameter->get() };
    params.chop = { chopPattern.load (std::memory_order_relaxed), chopCombParameter->get(), chopOvertoneParameter->get() * 0.01f };
    params.sculpt = { noiseLevelParameter->get() * 0.01f, foldDriveParameter->get(), foldMixParameter->get() * 0.01f };
    params.glideSeconds = glideParameter->get() * 0.001f;   // v1.11.0 (region B): GLIDE, ms to seconds

    // Modulation (v1.9.0): the LFO, pitch bend and the mod wheel, worked out once for all voices.
    // Bend and wheel are read from the incoming MIDI, with their sample positions, so they reach
    // every voice (keyboard and sequencer notes alike) on the right sample.
    Modulation::Settings modSettings;
    modSettings.lfo = { (Lfo::Shape) lfoShapeParameter->getIndex(), lfoRateParameter->get(),
                        lfoSyncParameter->get(), (Lfo::Division) lfoDivisionParameter->getIndex() };
    modSettings.lfoPitchSemitones  = lfoPitchParameter->get();
    modSettings.lfoCutoffOctaves   = lfoCutoffParameter->get();
    modSettings.lfoAmp             = lfoAmpParameter->get() * 0.01f;
    modSettings.bendRangeSemitones = bendRangeParameter->get();
    modSettings.vibratoCents       = vibratoParameter->get();
    modulation.setSettings (modSettings);
    synth.setVoiceLimit (voiceCountParameter->get());   // new notes use only the first N voices
    synth.setMonoLegato (legatoParameter->get() && voiceCountParameter->get() == 1);   // v1.11.0 (region B): LEGATO is MONO only
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
    buildPlayMidi (midiMessages, numSamples);
    synth.renderNextBlock (buffer, playMidi, 0, numSamples);
    modulation.applyAmplitude (buffer, numSamples);   // the LFO's tremolo, on the summed voices

    outputGain.setTargetValue (juce::Decibels::decibelsToGain (gainParameter->get()));
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
        scopeRing[(size_t) (2 * index)]     = left[i];
        scopeRing[(size_t) (2 * index + 1)] = right[i];
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
        destLeft[i]  = scopeRing[(size_t) (2 * index)];
        destRight[i] = scopeRing[(size_t) (2 * index + 1)];
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

namespace
{
    // State tree layout for the pattern: <Pattern><Step on="1" note="48" velocity="110"/> x16</Pattern>
    const juce::Identifier patternId  { "Pattern" };
    const juce::Identifier stepId     { "Step" };
    const juce::Identifier onId       { "on" };
    const juce::Identifier noteId     { "note" };
    const juce::Identifier velocityId { "velocity" };
    const juce::Identifier tieId      { "tie" };     // v1.11.0; written only when set, absent = off
    const juce::Identifier slideId    { "slide" };   // v1.11.0; written only when set, absent = off

    /**
        Saved-state format version, written as an attribute of the state tree.
          (none) v1.0 - v1.3: parameters, the editor's view and, from v1.3, the pattern
          2      v1.4: same content; the attribute exists so later format changes
                 can tell old sessions apart and migrate them
          3      v1.5: adds the Sample child (the sample, embedded or by path);
                 older states load with the built-in pad
          4      v1.6+: adds the Chop child (CHOP's 16-step pattern); older
                 states load with the default pattern (every second step)
    */
    const juce::Identifier stateVersionId { "stateVersion" };
    constexpr int currentStateVersion = 4;

    // State tree layout for CHOP's pattern (v1.6.0+): <Chop pattern="0101010101010101"/>,
    // one character per step, step 1 first, "1" = chopped.
    const juce::Identifier chopId        { "Chop" };
    const juce::Identifier chopPatternId { "pattern" };

    juce::String chopPatternToString (juce::uint16 pattern)
    {
        juce::String text;
        for (int i = 0; i < Chop::stepsPerPattern; ++i)
            text << (((pattern >> i) & 1u) != 0 ? "1" : "0");
        return text;
    }

    juce::uint16 chopPatternFromString (const juce::String& text, juce::uint16 fallback)
    {
        if (text.length() != Chop::stepsPerPattern || ! text.containsOnly ("01"))
            return fallback;
        juce::uint16 pattern = 0;
        for (int i = 0; i < Chop::stepsPerPattern; ++i)
            if (text[i] == '1')
                pattern = (juce::uint16) (pattern | (1u << i));
        return pattern;
    }

    // State tree layout for the sample (v1.5.0+):
    //   <Sample default="1"/>                                  the built-in pad
    //   <Sample name="pad.wav" path="C:/..." hash="..." flac="<base64>"/>
    // flac is the sample itself as 24-bit FLAC, left out above 10 MB (the path is used then).
    const juce::Identifier sampleId        { "Sample" };
    const juce::Identifier sampleDefaultId { "default" };
    const juce::Identifier sampleNameId    { "name" };
    const juce::Identifier samplePathId    { "path" };
    const juce::Identifier sampleHashId    { "hash" };
    const juce::Identifier sampleFlacId    { "flac" };

    /** Keyboard input is played on this channel; the sequencer has StepSequencer::midiChannel (16). */
    constexpr int keyboardChannel = 1;
    static_assert (keyboardChannel != StepSequencer::midiChannel);

    /** Notes are transposed relative to this key in Transpose mode (C4). */
    constexpr int transposeReferenceNote = 60;
}

void NeonSynthProcessor::loadDefaultPattern() noexcept
{
    // A C-minor bassline around C3 with octave jumps; notes are kept on the
    // "off" steps too, so switching a step on gives a sensible pitch.
    const StepSequencer::Step pattern[StepSequencer::numSteps] =
    {
        { true, 48, 112 }, { false, 48, 90 }, { true, 60, 80 },  { true, 48, 100 },
        { true, 51, 112 }, { false, 51, 90 }, { true, 55, 90 },  { true, 58, 80 },
        { true, 48, 112 }, { false, 48, 90 }, { true, 60, 80 },  { true, 48, 100 },
        { true, 53, 112 }, { false, 53, 90 }, { true, 55, 90 },  { true, 51, 80 },
    };

    for (int i = 0; i < StepSequencer::numSteps; ++i)
        sequencer.setStep (i, pattern[i]);
}

void NeonSynthProcessor::setSequencerStep (int index, const StepSequencer::Step& step)
{
    // Message thread: the step grid's mouse handlers call this (and tests, from their main thread),
    // so the host can be told here directly.
    sequencer.setStep (index, step);
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

void NeonSynthProcessor::trackHeldKey (int note, bool isDown) noexcept
{
    // Remove the key if present, then (for a key-down) append it as the most recent.
    for (int i = 0; i < numHeldKeys; ++i)
    {
        if (heldKeys[(size_t) i] == note)
        {
            for (int j = i + 1; j < numHeldKeys; ++j)
                heldKeys[(size_t) (j - 1)] = heldKeys[(size_t) j];
            --numHeldKeys;
            break;
        }
    }

    if (isDown && numHeldKeys < (int) heldKeys.size())
        heldKeys[(size_t) numHeldKeys++] = note;
}

void NeonSynthProcessor::releaseHeldNotes (SequencerMode mode) noexcept
{
    // v1.11.0 (region B): HOLD switched off. Each kept-back note-off goes to the synth (which
    // ignores one for a note no longer sounding) and, in Transpose mode, lets go of the key.
    for (int note = 0; note < (int) notesHeldByHold.size(); ++note)
    {
        if (! notesHeldByHold[(size_t) note])
            continue;
        notesHeldByHold[(size_t) note] = false;
        playMidi.addEvent (juce::MidiMessage::noteOff (keyboardChannel, note), 0);
        if (mode == SequencerMode::transpose)
            trackHeldKey (note, false);
    }
}

Lfo::Transport NeonSynthProcessor::lfoTransport() const
{
    // A playing host sets the LFO's phase from its song position (when SYNC is on); otherwise the
    // LFO free-runs, at the host's tempo when there is one, else at the Sequencer Tempo parameter.
    Lfo::Transport transport { false, 0.0, (double) seqTempoParameter->get() };

    if (auto* hostPlayHead = getPlayHead())
    {
        if (const auto position = hostPlayHead->getPosition(); position.hasValue())
        {
            if (const auto bpm = position->getBpm(); bpm.hasValue())
                transport.bpm = *bpm;

            if (const auto ppq = position->getPpqPosition(); ppq.hasValue() && position->getIsPlaying())
            {
                transport.hostPlaying = true;
                transport.ppq = *ppq;
            }
        }
    }
    return transport;
}

void NeonSynthProcessor::pushEnteredNote (int note, int velocity) noexcept
{
    const auto scope = enteredNotesFifo.write (1);
    if (scope.blockSize1 > 0)
        enteredNotes[(size_t) scope.startIndex1] = (juce::uint16) ((velocity << 8) | note);
}

bool NeonSynthProcessor::popEnteredNote (int& note, int& velocity) noexcept
{
    const auto scope = enteredNotesFifo.read (1);
    if (scope.blockSize1 == 0)
        return false;
    const auto packed = enteredNotes[(size_t) scope.startIndex1];
    note = packed & 0x7f;
    velocity = (packed >> 8) & 0x7f;
    return true;
}

void NeonSynthProcessor::buildPlayMidi (const juce::MidiBuffer& input, int numSamples) noexcept
{
    playMidi.clear();

    const bool sequencerOn = seqOnParameter->get();
    const auto mode = sequencerOn ? (SequencerMode) seqModeParameter->getIndex() : SequencerMode::layer;

    if (mode != SequencerMode::transpose)
        numHeldKeys = 0;   // forget keys held under another mode

    // v1.11.0 (region B), HOLD: like a sustain pedal for the keyboard. While it is on, keyboard
    // note-offs are kept back (below); switching it off sends them all, at the block's first
    // sample. The sequencer's notes are made after this and never pass through it. Off (the
    // default), nothing here runs and the keyboard path is exactly v1.10.1's.
    const bool holding = holdParameter->get();
    if (wasHolding && ! holding)
        releaseHeldNotes (mode);
    wasHolding = holding;

    // 1. Keyboard input. Layer passes everything through; Replace and Transpose
    //    keep controllers, pitch bend and so on, but take note-ons out of the
    //    synth's path (Transpose uses them to steer the pattern instead).
    //
    //    Note-offs reach the synth in every mode: a key pressed in Layer and
    //    released after switching to Replace or Transpose (or after turning the
    //    sequencer on) must still stop its voice. juce::Synthesiser ignores a
    //    note-off for a note that isn't sounding, so this is harmless.
    //    SysEx is dropped: the synth ignores it, and a large dump could
    //    outgrow playMidi's preallocated capacity on the audio thread.
    //
    //    Keyboard messages are moved to MIDI channel 1, whatever channel they
    //    arrived on. The sequencer owns channel 16; juce::Synthesiser matches
    //    note-offs by channel and note, so a keyboard on channel 16 would
    //    otherwise have its notes cut off by the sequencer playing the same
    //    pitch. The synth has no per-channel behaviour of its own, so nothing
    //    else changes (a sustain pedal still holds the keyboard's notes).
    for (const auto event : input)
    {
        if (event.numBytes <= 0 || event.data[0] == 0xf0)
            continue;   // empty or SysEx

        juce::uint8 bytes[3] = {};
        const int numBytes = juce::jmin (event.numBytes, 3);
        std::memcpy (bytes, event.data, (size_t) numBytes);

        if (event.numBytes <= 3 && bytes[0] >= 0x80 && bytes[0] < 0xf0)
            bytes[0] = (juce::uint8) ((bytes[0] & 0xf0) | (keyboardChannel - 1));

        const int  status  = numBytes >= 3 ? (bytes[0] & 0xf0) : 0;
        const bool noteOn  = status == 0x90 && bytes[2] > 0;
        const bool noteOff = status == 0x80 || (status == 0x90 && bytes[2] == 0);

        // v1.11.0 (region B), HOLD: the key stays down for the synth, and for Transpose mode too
        // (the pattern goes on playing, latched), until HOLD is switched off.
        if (noteOff && holding)
        {
            notesHeldByHold[(size_t) (bytes[1] & 0x7f)] = true;
            continue;
        }

        if ((noteOn || noteOff) && mode == SequencerMode::transpose)
            trackHeldKey (bytes[1], noteOn);

        if (noteOn && noteEntryArmed.load (std::memory_order_relaxed))
            pushEnteredNote (bytes[1], bytes[2]);

        if (! noteOn || mode == SequencerMode::layer)
        {
            if (event.numBytes <= 3)
                playMidi.addEvent (bytes, numBytes, event.samplePosition);
            else
                playMidi.addEvent (event.data, event.numBytes, event.samplePosition);   // longer system messages, unchanged
        }
    }

    // 2. Musical time: the host transport when Sync is on and the host reports
    //    one (a DAW), otherwise the internal clock at the Tempo parameter
    //    (the Standalone, or Sync off). The internal clock restarts at beat 0
    //    each time the sequencer is switched on.
    if (sequencerOn && ! wasSequencerOn)
        internalBeat = 0.0;
    wasSequencerOn = sequencerOn;

    StepSequencer::Transport transport;
    bool followsHost = false;

    if (seqSyncParameter->get())
    {
        if (auto* hostPlayHead = getPlayHead())
        {
            if (const auto position = hostPlayHead->getPosition(); position.hasValue())
            {
                const auto ppq = position->getPpqPosition();
                const auto bpm = position->getBpm();

                if (ppq.hasValue() && bpm.hasValue())
                {
                    transport   = { position->getIsPlaying(), *ppq, *bpm };
                    followsHost = true;
                }
            }
        }
    }

    if (! followsHost)
    {
        const double tempo = seqTempoParameter->get();
        transport = { true, internalBeat, tempo };
        internalBeat += (double) numSamples * tempo / (60.0 * getSampleRate());
    }

    transport.playing = transport.playing && sequencerOn;   // switching off releases sounding notes
    sequencerTempo.store ((float) transport.bpm, std::memory_order_relaxed);
    sequencerFollowsHost.store (followsHost, std::memory_order_relaxed);

    // 3. The sequencer's own events.
    StepSequencer::Settings settings;
    settings.rate  = (StepSequencer::Rate) seqRateParameter->getIndex();
    settings.swing = seqSwingParameter->get();
    settings.gate  = seqGateParameter->get();
    settings.length = seqLengthParameter->get();   // v1.11.0 (region A); 16, the default, plays as before

    if (mode == SequencerMode::transpose)
    {
        // Steered by the most recently pressed key that is still held; silent with no key down.
        settings.triggersEnabled = numHeldKeys > 0;
        settings.transpose = numHeldKeys > 0 ? heldKeys[(size_t) (numHeldKeys - 1)] - transposeReferenceNote : 0;
    }

    sequencer.process (playMidi, numSamples, transport, settings);
}

//==============================================================================
// Sample

void NeonSynthProcessor::publishSample (std::unique_ptr<SampleData> sample, const SampleData::Origin* missing, bool changedSinceSaved)
{
    {
        const juce::ScopedLock sl (sampleInfoLock);
        missingSample      = missing != nullptr ? *missing : SampleData::Origin{};
        missingSampleHash  = {};
        sampleProblem      = missing != nullptr ? "sample missing: " + missing->name
                           : changedSinceSaved  ? juce::String ("sample changed")
                                                : juce::String();
        embeddedSampleText = {};   // belongs to the sample being replaced (restoreSample() sets it again for an embedded one)
    }
    sampleSlot.publish (std::move (sample));
}

void NeonSynthProcessor::loadSampleFileAsync (const juce::File& file)
{
    const int generation = ++sampleLoadGeneration;
    backgroundLoadGeneration = generation;

    sampleLoader.addJob ([this, file, generation]
    {
        auto result = SampleLoader::loadFile (file);

        if (generation != sampleLoadGeneration.load())
            return;   // overtaken by a newer request, which owns the status now

        if (result.sample != nullptr)
            publishSample (std::move (result.sample));
        else
        {
            const juce::ScopedLock sl (sampleInfoLock);
            sampleProblem = file.getFileName() + ": " + result.error;
        }

        int expected = generation;
        backgroundLoadGeneration.compare_exchange_strong (expected, 0);   // done, unless a newer load has started

        // Hosts expect to be told about state changes on the message thread, and this is the
        // loader's thread: post it there. The AsyncUpdater is a member, so a notification still
        // pending when the processor goes is cancelled rather than delivered to a dead object.
        hostNotifier.triggerAsyncUpdate();
    });
}

bool NeonSynthProcessor::loadSampleFile (const juce::File& file, juce::String& error)
{
    ++sampleLoadGeneration;   // supersedes any background load still running
    auto result = SampleLoader::loadFile (file);

    if (result.sample == nullptr)
    {
        error = result.error;
        return false;
    }

    publishSample (std::move (result.sample));
    return true;
}

void NeonSynthProcessor::useDefaultSample()
{
    ++sampleLoadGeneration;
    publishSample (SampleLoader::makeDefaultSample());
}

NeonSynthProcessor::SampleStatus NeonSynthProcessor::getSampleStatus() const
{
    SampleStatus status;
    sampleSlot.withCurrent ([&] (const SampleData* sample)
    {
        if (sample != nullptr)
        {
            status.name    = sample->getOrigin().name;
            status.seconds    = sample->getLengthSeconds();
            status.sampleRate = sample->getSampleRate();
            status.path       = sample->getOrigin().path;
        }
    });

    const juce::ScopedLock sl (sampleInfoLock);
    status.problem = sampleProblem;
    const int loading = backgroundLoadGeneration.load();
    status.loading = loading != 0 && loading == sampleLoadGeneration.load();
    return status;
}

//==============================================================================
// State

void NeonSynthProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Parameters and GUI state come from the APVTS; the pattern is added as a
    // child tree because its 16 steps are not host parameters.
    // (apvts.state holds no Pattern, Chop or Sample child: setStateInformation() leaves them out.)
    auto state = apvts.copyState();
    state.setProperty (stateVersionId, currentStateVersion, nullptr);

    juce::ValueTree pattern (patternId);
    for (int i = 0; i < StepSequencer::numSteps; ++i)
    {
        const auto s = sequencer.getStep (i);
        juce::ValueTree step (stepId);
        step.setProperty (onId,       s.on,       nullptr);
        step.setProperty (noteId,     s.note,     nullptr);
        step.setProperty (velocityId, s.velocity, nullptr);
        if (s.tie)   step.setProperty (tieId,   true, nullptr);   // so a session without ties or
        if (s.slide) step.setProperty (slideId, true, nullptr);   // slides is saved as before
        pattern.appendChild (step, nullptr);
    }
    state.appendChild (pattern, nullptr);

    juce::ValueTree chop (chopId);
    chop.setProperty (chopPatternId, chopPatternToString (chopPattern.load()), nullptr);
    state.appendChild (chop, nullptr);

    juce::ValueTree sampleTree (sampleId);
    sampleSlot.withCurrent ([&] (const SampleData* sample)
    {
        const juce::ScopedLock sl (sampleInfoLock);

        if (missingSample.path.isNotEmpty())
        {
            // The session's sample couldn't be found: keep pointing at it, so saving doesn't lose it.
            sampleTree.setProperty (sampleNameId, missingSample.name, nullptr);
            sampleTree.setProperty (samplePathId, missingSample.path, nullptr);
            if (missingSampleHash.isNotEmpty())
                sampleTree.setProperty (sampleHashId, missingSampleHash, nullptr);
        }
        else if (sample == nullptr || sample->getOrigin().isDefault)
        {
            sampleTree.setProperty (sampleDefaultId, true, nullptr);
        }
        else
        {
            sampleTree.setProperty (sampleNameId, sample->getOrigin().name, nullptr);
            sampleTree.setProperty (samplePathId, sample->getOrigin().path, nullptr);
            sampleTree.setProperty (sampleHashId, sample->getHash(), nullptr);
            if (sample->getEmbeddedFlac().getSize() > 0)
            {
                // Encoded once per sample; the tree shares the kept text (juce::String is reference-counted).
                if (! embeddedSampleText.isFor (*sample))
                {
                    embeddedSampleText = { sample->getEmbeddedFlac().toBase64Encoding(), sample->getHash(), sample->getSampleRate() };
                    ++numEmbeddedSampleEncodes;
                }
                sampleTree.setProperty (sampleFlacId, embeddedSampleText.text, nullptr);
            }
        }
    });
    state.appendChild (sampleTree, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

namespace
{
    /** How deeply the elements in a state's XML nest, stopping once past `limit`. juce::XmlDocument
        parses nested elements recursively, so a hostile session nested a few hundred thousand deep
        would overflow the stack; ours nest three deep. A rough count is enough: '<' starts an
        element unless '/', '?' or '!' follows, "</" and "/>" end one; attribute values cannot
        contain '<' in well-formed XML, and anything else fails to parse later anyway. */
    int xmlNestingDepth (const void* data, int sizeInBytes, int limit) noexcept
    {
        const auto* text = static_cast<const char*> (data);
        int depth = 0, deepest = 0;
        for (int i = 0; i + 1 < sizeInBytes && deepest <= limit; ++i)
        {
            if (text[i] == '<')
            {
                const char next = text[i + 1];
                if (next == '/')
                    --depth;
                else if (next != '?' && next != '!')
                    deepest = juce::jmax (deepest, ++depth);
            }
            else if (text[i] == '/' && text[i + 1] == '>')
            {
                --depth;
            }
        }
        return deepest;
    }
}

void NeonSynthProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // A session is untrusted input: from a host, a project file, a preset someone sent.
    constexpr int maxNesting = 64;
    if (data == nullptr || sizeInBytes <= 0 || xmlNestingDepth (data, sizeInBytes, maxNesting) > maxNesting)
        return;

    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    const auto tree = juce::ValueTree::fromXml (*xml);

    // The pattern, CHOP and sample children are read into the sequencer, chopPattern and the
    // sample slot below, and getStateInformation() writes them from there. apvts.state keeps
    // only what the APVTS and the editor use (the parameters and the editor's view properties):
    // the Sample child can carry about 13 MB of base64 FLAC that would otherwise stay in memory.
    auto parameters = tree.createCopy();   // cheap: juce::String values are shared, not copied
    for (int i = parameters.getNumChildren(); --i >= 0;)
        if (const auto type = parameters.getChild (i).getType(); type == patternId || type == chopId || type == sampleId)
            parameters.removeChild (i, nullptr);

    // A value that is not a finite number ("nan" parses as NaN and the APVTS passes it through
    // unchecked) would reach the DSP as NaN, where it can stall the sequencer, crash the grain
    // reader or poison a voice for good. It falls back to the parameter's default; infinities
    // are clamped by the parameter's range, but they are not real settings either.
    for (int i = 0; i < parameters.getNumChildren(); ++i)
    {
        auto param = parameters.getChild (i);
        if (auto* p = apvts.getParameter (param.getProperty ("id").toString()); p != nullptr && param.hasProperty ("value")
                                                                               && ! std::isfinite ((double) param.getProperty ("value")))
            param.setProperty ("value", p->convertFrom0to1 (p->getDefaultValue()), nullptr);
    }
    apvts.replaceState (parameters);

    // A state without a pattern (sessions saved before v1.3, or a preset that
    // only carries parameters) gets the default pattern, so loading it always
    // sounds the same whatever was loaded before.
    loadDefaultPattern();

    const auto pattern = tree.getChildWithName (patternId);
    for (int i = 0; i < juce::jmin (StepSequencer::numSteps, pattern.getNumChildren()); ++i)
    {
        const auto step = pattern.getChild (i);
        sequencer.setStep (i, { (bool) step.getProperty (onId, false),
                                (int)  step.getProperty (noteId, 60),
                                (int)  step.getProperty (velocityId, 100),
                                (bool) step.getProperty (tieId, false),
                                (bool) step.getProperty (slideId, false) });
    }

    // CHOP's pattern; states from before v1.6.0 have none and get the default.
    chopPattern.store (chopPatternFromString (tree.getChildWithName (chopId).getProperty (chopPatternId).toString(),
                                              ChopSettings().pattern));

    restoreSample (tree.getChildWithName (sampleId));
}

void NeonSynthProcessor::setChopStep (int step, bool chopped) noexcept
{
    if (step < 0 || step >= Chop::stepsPerPattern)
        return;
    const auto bit = (juce::uint16) (1u << step);
    auto current = chopPattern.load();
    while (! chopPattern.compare_exchange_weak (current, chopped ? (juce::uint16) (current | bit) : (juce::uint16) (current & ~bit)))
    {
    }
}

void NeonSynthProcessor::restoreSample (const juce::ValueTree& sampleTree)
{
    ++sampleLoadGeneration;   // a background load started before this state arrived must not win

    // No Sample child (states before v1.5.0) or the default: the built-in pad.
    if (! sampleTree.isValid() || (bool) sampleTree.getProperty (sampleDefaultId, false))
    {
        if (! sampleSlot.withCurrent ([] (const SampleData* current) { return current != nullptr && current->getOrigin().isDefault; }))
        {
            publishSample (SampleLoader::makeDefaultSample());
        }
        else
        {
            const juce::ScopedLock sl (sampleInfoLock);   // already the pad: just forget any missing-sample note
            missingSample = {};
            missingSampleHash = {};
            sampleProblem = {};
        }
        return;
    }

    const SampleData::Origin origin { sampleTree.getProperty (sampleNameId).toString(),
                                      sampleTree.getProperty (samplePathId).toString(), false };
    const auto hash = sampleTree.getProperty (sampleHashId).toString();

    // Already loaded (a host re-sending the same state): nothing to do.
    if (sampleSlot.withCurrent ([&] (const SampleData* current)
            { return current != nullptr && hash.isNotEmpty() && current->getHash() == hash && ! current->getOrigin().isDefault; }))
    {
        const juce::ScopedLock sl (sampleInfoLock);
        missingSample = {};
        sampleProblem = {};
        return;
    }

    // 1. The embedded copy.
    // Its base64 text is at most 4/3 of SampleLoader::maxEmbeddedBytes plus a short size prefix (getStateInformation writes
    // no more); a longer one comes from a damaged or hostile session and is not decoded at all.
    // JUCE's encoding starts with the decoded size and a dot ("35242.lwTX..."), and
    // fromBase64Encoding() allocates whatever that number says before checking it against the
    // text, so a negative or huge one would throw bad_alloc or take gigabytes: it must be a plain
    // number, no larger than the cap, and no larger than the text after the dot can hold.
    auto plausibleEncoding = [] (const juce::String& encoded)
    {
        const int dot = encoded.indexOfChar ('.');
        if (dot <= 0 || dot > 9 || ! encoded.substring (0, dot).containsOnly ("0123456789"))
            return false;
        const auto size = (size_t) encoded.substring (0, dot).getLargeIntValue();
        const auto payload = (size_t) (encoded.length() - dot - 1);
        return size > 0 && size <= SampleLoader::maxEmbeddedBytes && size <= payload * 3 / 4 + 3;
    };

    if (const auto encoded = sampleTree.getProperty (sampleFlacId).toString();
        encoded.isNotEmpty() && (size_t) encoded.length() <= SampleLoader::maxEmbeddedBytes / 3 * 4 + 64
            && plausibleEncoding (encoded))
    {
        juce::MemoryBlock flac;
        if (flac.fromBase64Encoding (encoded))
        {
            if (auto result = SampleLoader::loadEmbeddedFlac (flac, origin); result.sample != nullptr)
            {
                // Keep the session's text for saving, so it isn't encoded again.
                EmbeddedSampleText text { encoded, result.sample->getHash(), result.sample->getSampleRate() };
                publishSample (std::move (result.sample));
                const juce::ScopedLock sl (sampleInfoLock);
                embeddedSampleText = std::move (text);
                return;
            }
        }
    }

    // 2. The file, where it was saved from (only a full path: a relative one means nothing here,
    // and juce::File asserts on it).
    if (origin.path.isNotEmpty() && juce::File::isAbsolutePath (origin.path))
    {
        if (auto result = SampleLoader::loadFile (juce::File (origin.path)); result.sample != nullptr)
        {
            // A different file may be there now (edited, or replaced). Load it anyway, since the
            // user may have changed it on purpose, but say so rather than change the sound silently.
            const bool changed = hash.isNotEmpty() && result.sample->getHash() != hash;
            publishSample (std::move (result.sample), nullptr, changed);
            return;
        }
    }

    // 3. Neither: play the built-in pad, but remember the sample so saving keeps the reference.
    publishSample (SampleLoader::makeDefaultSample(), &origin);
    const juce::ScopedLock sl (sampleInfoLock);
    missingSampleHash = hash;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NeonSynthProcessor();
}
