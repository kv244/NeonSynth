// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "plugin/NeonSynthProcessor.h"
#include "gui/NeonStyle.h"

/**
    The HARMONICS view (v1.12.0), the third view beside SCOPE and SPECTRUM: CUSTOM's wave as
    32 harmonics, the lesson's second part.

    - 32 bars, one per harmonic, level 0..100%. Drag up or down to set a level, and across to
      set several in one stroke (every bar the pointer passes gets the height under it, so a
      quick stroke skips none). A double-click sets a bar to 0. Bars beyond HARMONICS (the
      count that plays) are dimmed.
    - Above each bar a small dial shows the harmonic's phase: a dot on a ring, at the angle of
      the phase as on a unit circle (right = 0, top = 90 degrees, counter-clockwise). Phases
      are set by the presets and ZERO PHASE only, never by dragging bars.
    - SINE, SAW, SQUARE, TRIANGLE load HarmonicSpectrum's textbook series (levels and phases).
    - ZERO PHASE sets every phase to 0 and keeps the levels: the cycle's shape changes (see the
      SHAPE strip), the sound does not. That is the lesson: the ear hears levels, not phases.
    - BUILD steps the HARMONICS parameter from 1 to 32, about three a second, so each bar
      lights as it joins and the wave is heard filling in. It is one host gesture from start
      to stop, so a DAW records it as one automation pass. Clicking BUILD again stops it, and
      so does anyone else moving HARMONICS meanwhile.
    - The readout under the bars says "16 of 32", or, with all 32 playing and the SQUARE or
      SAW series in place, "Gibbs: 9% overshoot at the jump" (the overshoot never shrinks,
      however many harmonics are added: it is about 9% of the jump's height).

    Every edit goes to processor.setCycleDrawing (std::nullopt, spectrum), which drops any drawn
    cycle, sanitises the spectrum, rebuilds the tables and tells the host. refresh() is called by the editor's 30 Hz timer and repaints
    only when the spectrum, the count or BUILD changed. Message thread only.
*/
class HarmonicsView : public juce::Component,
                      public juce::SettableTooltipClient,
                      private juce::Timer
{
public:
    static constexpr int numHarmonics      = HarmonicSpectrum::numHarmonics;
    static constexpr int buildStepsPerSecond = 3;   ///< BUILD's pace: slow enough to hear each harmonic join

    HarmonicsView (NeonSynthProcessor& p, juce::AudioProcessorValueTreeState& state);

    ~HarmonicsView() override;

    void resized() override;

    /** Called by the editor's 30 Hz timer: repaints when the spectrum, the count or BUILD changed. */
    void refresh();

    void paint (juce::Graphics& g) override;

    //==========================================================================
    void mouseDown (const juce::MouseEvent& e) override;

    void mouseDrag (const juce::MouseEvent& e) override;

    void mouseDoubleClick (const juce::MouseEvent& e) override;

    //==========================================================================
    /** True if the spectrum is the SQUARE or SAW series (levels and phases), whose jump shows Gibbs's overshoot. */
    static bool isGibbsSeries (const HarmonicSpectrum& s) noexcept;

    /** "16 of 32", or with all 32 playing and the SQUARE or SAW series in place, Gibbs's overshoot. */
    static juce::String readoutText (int harmonicsPlaying, const HarmonicSpectrum& s);

    bool isBuilding() const noexcept   { return building; }

    /** Where bar k (1..32) is, in the view's coordinates (tests drag it). */
    juce::Rectangle<float> barBounds (int k) const;

private:
    /** The bars' area: under the phase dials, above the harmonic numbers and the bottom row. */
    juce::Rectangle<float> barsArea() const;
    juce::Rectangle<float> dialRow() const     { const auto b = barsArea(); return { b.getX(), 34.0f, b.getWidth(), 16.0f }; }
    juce::Rectangle<float> numberRow() const   { const auto b = barsArea(); return { b.getX(), b.getBottom() + 3.0f, b.getWidth(), 14.0f }; }

    /** The harmonic (1..32) under x, or 0 outside the bars. */
    int harmonicAt (float x) const noexcept;

    float levelAt (float y) const noexcept;

    /** Sets every bar between two pointer positions to the height of the line joining them there. */
    void setLevelsAlong (juce::Point<float> from, juce::Point<float> to);

    /**
        Every edit here (a bar, a preset, ZERO PHASE) also drops a drawn cycle, so the SHAPE strip's
        faint drawn line never disagrees with what plays. BUILD moves only HARMONICS and keeps it.
    */
    void setSpectrum (const HarmonicSpectrum& s);

    static bool sameSeries (const HarmonicSpectrum& a, const HarmonicSpectrum& b) noexcept;

    //==========================================================================
    // BUILD

    int count() const;

    void setCount (int n);

    void startBuild();

    void stopBuild();

    /** One BUILD step: the next harmonic joins; at 32 (or if someone else moved HARMONICS) it stops. */
    void timerCallback() override;

    //==========================================================================
    struct Shown
    {
        int version = -1, count = 0;
        bool building = false, custom = false;
        bool operator== (const Shown& o) const noexcept
        {
            return version == o.version && count == o.count && building == o.building && custom == o.custom;
        }
    };

    NeonSynthProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    juce::RangedAudioParameter* countParameter = nullptr;
    std::atomic<float>* customValue = nullptr;

    Shown shown;
    HarmonicSpectrum spectrum;                    // as last read (when the version moved)
    bool building = false;
    int  builtCount = 0;                          // what BUILD last set HARMONICS to
    juce::Point<float> lastDrag;
    bool dragging = false;                        // a press on the bars, until the button is let go

    juce::TextButton sineButton { "SINE" }, sawButton { "SAW" }, squareButton { "SQUARE" }, triangleButton { "TRIANGLE" },
                     zeroPhaseButton { "ZERO PHASE" }, buildButton { "BUILD" };
    juce::Slider countSlider;
    juce::Label  readout;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> countAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HarmonicsView)
};
