// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>

/**
    The host parameters (v1.11.1: moved out of NeonSynthProcessor). createParameterLayout()
    defines them, in the order they have always had: hosts and saved sessions find a parameter
    by its ID, and automation by its index, so a new one is only ever appended (tests/
    parameter-layout.txt and ParameterLayoutTests hold the list). The fields are typed pointers
    into the processor's APVTS, looked up once when the processor is built, so the audio thread
    reads a value without a lookup by name.
*/
struct Parameters
{
    /** Looks every parameter up in `state`, which was built from createParameterLayout(). */
    explicit Parameters (juce::AudioProcessorValueTreeState& state);

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioParameterChoice* waveform      = nullptr;
    juce::AudioParameterFloat*  cutoff        = nullptr;
    juce::AudioParameterBool*   filterEnabled = nullptr;
    juce::AudioParameterFloat*  attack        = nullptr;
    juce::AudioParameterFloat*  decay         = nullptr;
    juce::AudioParameterFloat*  sustain       = nullptr;
    juce::AudioParameterFloat*  release       = nullptr;
    juce::AudioParameterFloat*  gain          = nullptr;
    juce::AudioParameterFloat*  width         = nullptr;

    juce::AudioParameterBool*   seqOn         = nullptr;
    juce::AudioParameterBool*   seqSync       = nullptr;
    juce::AudioParameterFloat*  seqTempo      = nullptr;
    juce::AudioParameterChoice* seqRate       = nullptr;
    juce::AudioParameterFloat*  seqSwing      = nullptr;
    juce::AudioParameterFloat*  seqGate       = nullptr;
    juce::AudioParameterChoice* seqMode       = nullptr;

    juce::AudioParameterChoice* source        = nullptr;
    juce::AudioParameterInt*    smpRoot       = nullptr;
    juce::AudioParameterFloat*  smpStart      = nullptr;
    juce::AudioParameterChoice* smpLoop       = nullptr;
    juce::AudioParameterFloat*  smpLoopStart  = nullptr;
    juce::AudioParameterFloat*  smpLoopEnd    = nullptr;
    juce::AudioParameterFloat*  smpXfade      = nullptr;

    juce::AudioParameterFloat*  grPos         = nullptr;
    juce::AudioParameterFloat*  grScan        = nullptr;
    juce::AudioParameterFloat*  grSize        = nullptr;
    juce::AudioParameterFloat*  grDensity     = nullptr;
    juce::AudioParameterFloat*  grSpray       = nullptr;
    juce::AudioParameterFloat*  grJitter      = nullptr;
    juce::AudioParameterFloat*  chopOvertone  = nullptr;
    juce::AudioParameterInt*    chopComb      = nullptr;
    juce::AudioParameterFloat*  noiseLevel    = nullptr;
    juce::AudioParameterFloat*  foldDrive     = nullptr;
    juce::AudioParameterFloat*  foldMix       = nullptr;
    juce::AudioParameterChoice* filterMode    = nullptr;
    juce::AudioParameterFloat*  filterRes     = nullptr;
    juce::AudioParameterFloat*  filterDrive   = nullptr;
    juce::AudioParameterFloat*  filterEnvAmt  = nullptr;
    juce::AudioParameterFloat*  filterAttack  = nullptr;
    juce::AudioParameterFloat*  filterDecay   = nullptr;
    juce::AudioParameterFloat*  filterSustain = nullptr;
    juce::AudioParameterFloat*  filterRelease = nullptr;
    juce::AudioParameterFloat*  filterVel     = nullptr;
    juce::AudioParameterChoice* lfoShape      = nullptr;
    juce::AudioParameterFloat*  lfoRate       = nullptr;
    juce::AudioParameterBool*   lfoSync       = nullptr;
    juce::AudioParameterChoice* lfoDivision   = nullptr;
    juce::AudioParameterFloat*  lfoPitch      = nullptr;
    juce::AudioParameterFloat*  lfoCutoff     = nullptr;
    juce::AudioParameterFloat*  lfoAmp        = nullptr;
    juce::AudioParameterInt*    bendRange     = nullptr;
    juce::AudioParameterFloat*  vibrato       = nullptr;
    juce::AudioParameterInt*    voiceCount    = nullptr;

    // v1.11.0: playing (hold, glide, legato) and the sequencer's pattern length and scale lock.
    juce::AudioParameterBool*   hold          = nullptr;
    juce::AudioParameterFloat*  glide         = nullptr;
    juce::AudioParameterBool*   legato        = nullptr;
    juce::AudioParameterInt*    seqLength     = nullptr;
    juce::AudioParameterChoice* seqKey        = nullptr;
    juce::AudioParameterChoice* seqScale      = nullptr;

private:
    /** The parameter with this ID as its own type; stops the program if there is none (see the .cpp). */
    template <typename ParamType>
    static ParamType* getTypedParameter (juce::AudioProcessorValueTreeState& state, const char* id);
};
