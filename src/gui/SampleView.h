// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "plugin/NeonSynthProcessor.h"
#include "gui/NeonStyle.h"

/**
    The sample's waveform in the editor, with its markers and live playheads.

    - Start (cyan line), and the loop (amber region between two brackets,
      shown unless the loop mode is One-shot).
    - Drag a marker to move it; click anywhere else to set Start there.
      Markers are parameters, so the drag is one undoable, automatable gesture.
    - A thin white line per voice playing the sample shows where it reads.
    - With Source = Granular it shows the cloud instead: Position (magenta
      line, draggable), the band Spray can reach, and a tick per sounding
      grain (up to 8 per voice).

    refresh() is called by the editor's 30 Hz timer; it repaints only when
    something shown has changed. Message thread only.
*/
class SampleView : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    SampleView (NeonSynthProcessor& p, juce::AudioProcessorValueTreeState& state);

    /** Re-reads the sample, markers and playheads. Message thread. */
    void refresh();

    void paint (juce::Graphics& g) override;

    void mouseDown (const juce::MouseEvent& e) override;

    void mouseDrag (const juce::MouseEvent& e) override;

    void mouseUp (const juce::MouseEvent&) override;

private:
    static constexpr int numBuckets = 512;

    struct Markers
    {
        float start = 0.0f, loopStart = 0.0f, loopEnd = 1.0f;
        int loopMode = 1;
        NeonSynthVoice::Source source = NeonSynthVoice::Source::oscillator;
        float grainPos = 0.5f, spray = 0.1f;
        bool operator== (const Markers& o) const noexcept
        {
            return start == o.start && loopStart == o.loopStart && loopEnd == o.loopEnd
                && loopMode == o.loopMode && source == o.source && grainPos == o.grainPos && spray == o.spray;
        }
    };

    float xFor (float fraction) const noexcept;

    float fractionAt (float x) const noexcept;

    /** A loop bracket; grow > 0 draws it fatter, for the glow pass under it. */
    static void drawBracket (juce::Graphics& g, float x, juce::Rectangle<float> r, bool opening, float grow);

    NeonSynthProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    std::atomic<float>* startValue = nullptr;
    std::atomic<float>* loopStartValue = nullptr;
    std::atomic<float>* loopEndValue = nullptr;
    std::atomic<float>* loopModeValue = nullptr;
    std::atomic<float>* sourceValue = nullptr;
    std::atomic<float>* grainPosValue = nullptr;
    std::atomic<float>* grainSprayValue = nullptr;

    juce::String shownHash;
    bool shownDefault = false;
    std::vector<std::pair<float, float>> peaks = std::vector<std::pair<float, float>> ((size_t) numBuckets);
    Markers markers;
    std::array<float, NeonSynthProcessor::numVoices> playheads {};
    static constexpr size_t numShownGrains = (size_t) NeonSynthProcessor::numVoices * NeonSynthVoice::Telemetry::maxShownGrains;
    std::array<float, numShownGrains> grains {};
    juce::String dragging;
    neon::crt::CachedPainter waveform;   // the inset and the waveform
    bool waveformActive = true;          // the dimming the cached waveform was drawn with

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleView)
};
