// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "plugin/NeonSynthProcessor.h"
#include "gui/NeonStyle.h"
#include <tuple>

//==============================================================================
/**
    ADSR shape drawn from the four knobs, with a dot for every sounding voice
    showing where it is in its envelope and how loud.

    Segment widths follow the square root of each time so a 10 s release does
    not squash a 10 ms attack into nothing.

    The editor has two: the amp envelope (amber, with the voice dots) and the
    filter envelope (magenta). The voices report where they are in the amp
    envelope only, so the filter's view draws the shape without dots.
*/
class AdsrView : public juce::Component,
                 public juce::SettableTooltipClient
{
public:
    using VoiceViews = std::array<NeonSynthProcessor::VoiceView, NeonSynthProcessor::numVoices>;

    /** The four points that can be dragged, in the order of attach()'s arguments. */
    enum Handle { attackHandle = 0, decayHandle, sustainHandle, releaseHandle, numHandles };

    AdsrView();

    /** The parameters the points move. Without them the view only displays. */
    void attach (juce::RangedAudioParameter* attack, juce::RangedAudioParameter* decay,
                 juce::RangedAudioParameter* sustain, juce::RangedAudioParameter* release);

    /** Where a point is drawn, in this component's coordinates. */
    juce::Point<float> handlePosition (int handle) const;

    void mouseMove (const juce::MouseEvent& e) override   { setHover (handleAt (e.position)); }
    void mouseExit (const juce::MouseEvent&) override     { if (dragging < 0) setHover (-1); }

    void mouseDown (const juce::MouseEvent& e) override;

    void mouseDrag (const juce::MouseEvent& e) override;

    void mouseUp (const juce::MouseEvent& e) override;

    void mouseDoubleClick (const juce::MouseEvent& e) override;

    /** The colour of the shape, the points and the text (amber unless set). */
    void setAccent (juce::Colour colour);

    /** Reshapes the view from the four values; voices: where each voice is in this envelope, or
        nullptr for no dots (the filter envelope: the voices report the amp envelope only). */
    void update (float attack, float decay, float sustain, float release, const VoiceViews* voices);

    void paint (juce::Graphics& g) override;

private:
    struct Marker { float x = 0.0f, level = 0.0f; };

    static constexpr float handleSize = 13.0f;
    static constexpr float grabRadius = 14.0f;

    juce::Rectangle<float> plotArea() const   { return getLocalBounds().toFloat().reduced (10.0f, 9.0f); }

    juce::RangedAudioParameter* parameterFor (int handle) const;

    /** The nearest point within reach of the pointer, or -1. */
    int handleAt (juce::Point<float> position) const;

    void setHover (int handle);

    std::array<juce::RangedAudioParameter*, numHandles> parameters {};
    int hover = -1, dragging = -1;
    float dragStartValue = 0.0f;

    float sustainStart() const noexcept { return attackW + decayW; }
    float releaseStart() const noexcept { return 1.0f - releaseW; }

    float attackW = 0.2f, decayW = 0.2f, releaseW = 0.38f, sustainLevel = 0.7f;
    float shownAttackW = -1.0f, shownDecayW = -1.0f, shownReleaseW = -1.0f, shownSustain = -1.0f;   // last repainted shape
    std::array<Marker, NeonSynthProcessor::numVoices> markers {};
    int numMarkers = 0;
    juce::Colour accent = neon::amber;
};
