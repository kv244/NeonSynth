// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "plugin/Parameters.h"
#include "plugin/NeonSynthProcessor.h"   // numVoices, the range of the Voices parameter
#include "dsp/Wavetable.h"                // HarmonicSpectrum::numHarmonics, the range of Harmonics
#include <cstdio>
#include <cstdlib>

template <typename ParamType>
ParamType* Parameters::getTypedParameter (juce::AudioProcessorValueTreeState& state, const char* id)
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

juce::AudioProcessorValueTreeState::ParameterLayout Parameters::createParameterLayout()
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

    // v1.13.0: "Output Gain", with "dB" as the parameter's label (it was "Output Gain (dB)", the only
    // name carrying its unit). Sessions and host automation use the ID, so they are unaffected, and
    // pedalboard joins name and label, so Python scripts still read it as output_gain_db.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "gain", 1 }, "Output Gain",
                                                        NormalisableRange<float> (-48.0f, 6.0f, 0.1f), 0.0f,
                                                        AudioParameterFloatAttributes().withLabel ("dB")));

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

    // v1.12.0 "Fourier" (FourierState, Wavetable.h). A switch beside WAVEFORM rather than a fifth
    // choice, because a new choice would move the existing ones' normalised values (roadmap
    // rule 3). Both switches off is the oscillator of v1.11, so older sessions render the same.
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { "naive", 1 }, "Naive", false));     // no band-limiting: hear aliasing
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { "custom", 1 }, "Custom", false));   // play the 32-harmonic wave instead of WAVEFORM
    layout.add (std::make_unique<AudioParameterInt>    (ParameterID { "harmonics", 1 }, "Harmonics", 1,
                                                         HarmonicSpectrum::numHarmonics, HarmonicSpectrum::numHarmonics));   // how many of the 32 play
    return layout;
}

Parameters::Parameters (juce::AudioProcessorValueTreeState& state)
{
    waveform      = getTypedParameter<juce::AudioParameterChoice> (state, "waveform");
    cutoff        = getTypedParameter<juce::AudioParameterFloat> (state, "cutoff");
    filterEnabled = getTypedParameter<juce::AudioParameterBool>  (state, "filterEnabled");
    attack        = getTypedParameter<juce::AudioParameterFloat> (state, "attack");
    decay         = getTypedParameter<juce::AudioParameterFloat> (state, "decay");
    sustain       = getTypedParameter<juce::AudioParameterFloat> (state, "sustain");
    release       = getTypedParameter<juce::AudioParameterFloat> (state, "release");
    gain          = getTypedParameter<juce::AudioParameterFloat> (state, "gain");
    width         = getTypedParameter<juce::AudioParameterFloat> (state, "width");
    seqOn         = getTypedParameter<juce::AudioParameterBool>  (state, "seqOn");
    seqSync       = getTypedParameter<juce::AudioParameterBool>  (state, "seqSync");
    seqTempo      = getTypedParameter<juce::AudioParameterFloat> (state, "seqTempo");
    seqRate       = getTypedParameter<juce::AudioParameterChoice> (state, "seqRate");
    seqSwing      = getTypedParameter<juce::AudioParameterFloat> (state, "seqSwing");
    seqGate       = getTypedParameter<juce::AudioParameterFloat> (state, "seqGate");
    seqMode       = getTypedParameter<juce::AudioParameterChoice> (state, "seqMode");
    source        = getTypedParameter<juce::AudioParameterChoice> (state, "source");
    smpRoot       = getTypedParameter<juce::AudioParameterInt>   (state, "smpRoot");
    smpStart      = getTypedParameter<juce::AudioParameterFloat> (state, "smpStart");
    smpLoop       = getTypedParameter<juce::AudioParameterChoice> (state, "smpLoop");
    smpLoopStart  = getTypedParameter<juce::AudioParameterFloat> (state, "smpLoopStart");
    smpLoopEnd    = getTypedParameter<juce::AudioParameterFloat> (state, "smpLoopEnd");
    smpXfade      = getTypedParameter<juce::AudioParameterFloat> (state, "smpXfade");
    grPos         = getTypedParameter<juce::AudioParameterFloat> (state, "grPos");
    grScan        = getTypedParameter<juce::AudioParameterFloat> (state, "grScan");
    grSize        = getTypedParameter<juce::AudioParameterFloat> (state, "grSize");
    grDensity     = getTypedParameter<juce::AudioParameterFloat> (state, "grDensity");
    grSpray       = getTypedParameter<juce::AudioParameterFloat> (state, "grSpray");
    grJitter      = getTypedParameter<juce::AudioParameterFloat> (state, "grJitter");
    chopOvertone  = getTypedParameter<juce::AudioParameterFloat> (state, "chopOvertone");
    chopComb      = getTypedParameter<juce::AudioParameterInt>   (state, "chopComb");
    noiseLevel    = getTypedParameter<juce::AudioParameterFloat> (state, "noiseLevel");
    foldDrive     = getTypedParameter<juce::AudioParameterFloat> (state, "foldDrive");
    foldMix       = getTypedParameter<juce::AudioParameterFloat> (state, "foldMix");
    filterMode    = getTypedParameter<juce::AudioParameterChoice> (state, "filterMode");
    filterRes     = getTypedParameter<juce::AudioParameterFloat> (state, "filterRes");
    filterDrive   = getTypedParameter<juce::AudioParameterFloat> (state, "filterDrive");
    filterEnvAmt  = getTypedParameter<juce::AudioParameterFloat> (state, "filterEnvAmt");
    filterAttack  = getTypedParameter<juce::AudioParameterFloat> (state, "filterAttack");
    filterDecay   = getTypedParameter<juce::AudioParameterFloat> (state, "filterDecay");
    filterSustain = getTypedParameter<juce::AudioParameterFloat> (state, "filterSustain");
    filterRelease = getTypedParameter<juce::AudioParameterFloat> (state, "filterRelease");
    filterVel     = getTypedParameter<juce::AudioParameterFloat> (state, "filterVel");
    lfoShape      = getTypedParameter<juce::AudioParameterChoice> (state, "lfoShape");
    lfoRate       = getTypedParameter<juce::AudioParameterFloat> (state, "lfoRate");
    lfoSync       = getTypedParameter<juce::AudioParameterBool>  (state, "lfoSync");
    lfoDivision   = getTypedParameter<juce::AudioParameterChoice> (state, "lfoDivision");
    lfoPitch      = getTypedParameter<juce::AudioParameterFloat> (state, "lfoPitch");
    lfoCutoff     = getTypedParameter<juce::AudioParameterFloat> (state, "lfoCutoff");
    lfoAmp        = getTypedParameter<juce::AudioParameterFloat> (state, "lfoAmp");
    bendRange     = getTypedParameter<juce::AudioParameterInt>   (state, "bendRange");
    vibrato       = getTypedParameter<juce::AudioParameterFloat> (state, "vibrato");
    voiceCount    = getTypedParameter<juce::AudioParameterInt>   (state, "voiceCount");
    hold          = getTypedParameter<juce::AudioParameterBool>  (state, "hold");
    glide         = getTypedParameter<juce::AudioParameterFloat> (state, "glide");
    legato        = getTypedParameter<juce::AudioParameterBool>  (state, "legato");
    seqLength     = getTypedParameter<juce::AudioParameterInt>   (state, "seqLength");
    seqKey        = getTypedParameter<juce::AudioParameterChoice> (state, "seqKey");
    seqScale      = getTypedParameter<juce::AudioParameterChoice> (state, "seqScale");
    naive         = getTypedParameter<juce::AudioParameterBool>  (state, "naive");
    custom        = getTypedParameter<juce::AudioParameterBool>  (state, "custom");
    harmonics     = getTypedParameter<juce::AudioParameterInt>   (state, "harmonics");
}
