// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "plugin/NeonSynthProcessor.h"
#include "gui/NeonStyle.h"

/**
    The HARMONICS view (v1.12.0), the third view beside SCOPE and SPECTRUM: CUSTOM's 32
    harmonic levels as bars, their phases as dots, the presets, BUILD and ZERO PHASE.

    v1.12.0 base: a placeholder that only draws the bars of the current spectrum; the bars'
    editing, the presets, BUILD and ZERO PHASE are agent B's (see the v1.12 plan).
    refresh() is called by the editor's 30 Hz timer. Message thread only.
*/
class HarmonicsView : public juce::Component
{
public:
    HarmonicsView (NeonSynthProcessor& p, juce::AudioProcessorValueTreeState& state)
        : processor (p), apvts (state)
    {
        // HARMONICS: how many of the 32 play (the host parameter). Base placeholder; agent B lays it out.
        countSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        countSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 40, 18);
        countSlider.setComponentID ("harmonics");
        countSlider.setTooltip ("Harmonics: how many of the 32 play, counted from the fundamental");
        addAndMakeVisible (countSlider);
        countAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, "harmonics", countSlider);
    }

    void resized() override
    {
        countSlider.setBounds (getLocalBounds().removeFromBottom (24).reduced (8, 2));
    }

    /** Repaints when the spectrum changed. */
    void refresh()
    {
        if (const int v = processor.getFourierVersion(); v != shownVersion)
        {
            shownVersion = v;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto spectrum = processor.getHarmonicSpectrum();
        auto area = getLocalBounds().toFloat().reduced (8.0f);
        area.removeFromBottom (24.0f);
        const float w = area.getWidth() / (float) HarmonicSpectrum::numHarmonics;
        g.setColour (neon::cyan);
        for (int k = 0; k < HarmonicSpectrum::numHarmonics; ++k)
        {
            const float h = spectrum.levels[(size_t) k] * area.getHeight();
            g.fillRect (area.getX() + (float) k * w + 1.0f, area.getBottom() - h, w - 2.0f, h);
        }
    }

private:
    NeonSynthProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    int shownVersion = -1;
    juce::Slider countSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> countAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HarmonicsView)
};
