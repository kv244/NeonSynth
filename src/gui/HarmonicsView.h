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

    HarmonicsView (NeonSynthProcessor& p, juce::AudioProcessorValueTreeState& state)
        : processor (p), apvts (state)
    {
        countParameter = apvts.getParameter ("harmonics");
        customValue    = apvts.getRawParameterValue ("custom");
        jassert (countParameter != nullptr && customValue != nullptr);

        setTooltip ("Harmonics: drag the bars to set each harmonic's level (across, to set several); double-click "
                    "a bar for 0. The dial above a bar is its phase (right = 0, top = 90 degrees). "
                    "Heard with CUSTOM on");

        const auto setUpButton = [this] (juce::TextButton& b, const char* id, const char* tip)
        {
            b.setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
            b.setColour (juce::TextButton::buttonOnColourId, neon::cyan.withAlpha (0.25f));
            b.setColour (juce::TextButton::textColourOffId,  neon::cyan.withAlpha (0.7f));
            b.setColour (juce::TextButton::textColourOnId,   neon::cyan);
            b.setColour (juce::ComboBox::outlineColourId,    neon::cyan.withAlpha (0.6f));
            b.setComponentID (id);
            b.setTooltip (tip);
            addAndMakeVisible (b);
        };
        setUpButton (sineButton,      "harmonicsSine",     "Sine: the fundamental alone");
        setUpButton (sawButton,       "harmonicsSaw",      "Saw: every harmonic, level 1/k");
        setUpButton (squareButton,    "harmonicsSquare",   "Square: the odd harmonics, level 1/k");
        setUpButton (triangleButton,  "harmonicsTriangle", "Triangle: the odd harmonics, level 1/k squared, alternating in phase");
        setUpButton (zeroPhaseButton, "zeroPhase",         "Zero phase: every phase to 0, levels kept. The cycle's shape changes; the sound does not");
        setUpButton (buildButton,     "build",             "Build: add the harmonics one by one, 1 to 32, three a second (click again to stop). "
                                                           "Recorded by the host as HARMONICS automation");
        buildButton.setColour (juce::TextButton::buttonOnColourId, neon::amber.withAlpha (0.25f));
        buildButton.setColour (juce::TextButton::textColourOnId,   neon::amber);

        sineButton.onClick      = [this] { setSpectrum (HarmonicSpectrum::sine()); };
        sawButton.onClick       = [this] { setSpectrum (HarmonicSpectrum::saw()); };
        squareButton.onClick    = [this] { setSpectrum (HarmonicSpectrum::square()); };
        triangleButton.onClick  = [this] { setSpectrum (HarmonicSpectrum::triangle()); };
        zeroPhaseButton.onClick = [this]
        {
            auto s = processor.getHarmonicSpectrum();
            s.phases.fill (0.0f);
            setSpectrum (s);
        };
        buildButton.onClick = [this] { if (building) stopBuild(); else startBuild(); };

        // HARMONICS: how many of the 32 play (the host parameter); its value is in the readout.
        countSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        countSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        countSlider.setColour (juce::Slider::thumbColourId,      neon::cyan);
        countSlider.setColour (juce::Slider::trackColourId,      neon::cyan.withAlpha (0.3f));
        countSlider.setColour (juce::Slider::backgroundColourId, juce::Colours::black);
        countSlider.setComponentID ("harmonics");
        countSlider.setTooltip ("Harmonics: how many of the 32 play, counted from the fundamental");
        addAndMakeVisible (countSlider);
        countAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, "harmonics", countSlider);

        readout.setComponentID ("harmonicsReadout");
        readout.setFont (neon::font (13.0f));
        readout.setColour (juce::Label::textColourId, neon::cyan);
        readout.setJustificationType (juce::Justification::centredLeft);
        readout.setBorderSize ({});
        readout.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (readout);
        refresh();
    }

    ~HarmonicsView() override
    {
        stopBuild();   // ends the host gesture if BUILD was running
    }

    void resized() override
    {
        // Top row: the four series on the left, ZERO PHASE and BUILD on the right.
        sineButton.setBounds (8, 6, 52, 22);
        sawButton.setBounds (64, 6, 48, 22);
        squareButton.setBounds (116, 6, 68, 22);
        triangleButton.setBounds (188, 6, 84, 22);
        buildButton.setBounds (getWidth() - 72, 6, 64, 22);
        zeroPhaseButton.setBounds (buildButton.getX() - 106, 6, 102, 22);

        // Bottom row: HARMONICS (its name is painted), the count, the readout.
        const int row = getHeight() - 28;
        countSlider.setBounds (86, row, 176, 22);
        readout.setBounds (274, row, getWidth() - 282, 22);
    }

    /** Called by the editor's 30 Hz timer: repaints when the spectrum, the count or BUILD changed. */
    void refresh()
    {
        const Shown now { processor.getFourierVersion(), count(), building, customValue->load() >= 0.5f };
        if (now == shown)
            return;
        const bool spectrumChanged = now.version != shown.version;
        shown = now;
        if (spectrumChanged)
            spectrum = processor.getHarmonicSpectrum();
        readout.setText (readoutText (shown.count, spectrum), juce::dontSendNotification);
        readout.setColour (juce::Label::textColourId, isGibbsSeries (spectrum) && shown.count == numHarmonics ? neon::amber : neon::cyan);
        buildButton.setToggleState (building, juce::dontSendNotification);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        neon::drawPanel (g, getLocalBounds().toFloat(), neon::cyan);
        const auto bars = barsArea();
        const float pitch = bars.getWidth() / (float) numHarmonics;

        // Grid: 25 / 50 / 75 / 100%, with the percentages at the right.
        for (int quarter = 1; quarter <= 4; ++quarter)
        {
            const float y = std::round (bars.getBottom() - bars.getHeight() * (float) quarter / 4.0f);
            g.setColour (neon::cyan.withAlpha (quarter == 4 ? 0.16f : 0.08f));
            g.fillRect (juce::Rectangle<float> (bars.getX(), y, bars.getWidth(), 1.0f));
        }
        g.setColour (neon::cyan.withAlpha (0.25f));
        g.fillRect (juce::Rectangle<float> (bars.getX(), bars.getBottom(), bars.getWidth(), 1.0f));   // the axis

        const auto dials = dialRow();
        for (int k = 1; k <= numHarmonics; ++k)
        {
            const float level = spectrum.levels[(size_t) (k - 1)];
            const bool playing = k <= shown.count;
            const bool newest = shown.building && k == shown.count;   // BUILD: the bar that just joined flares
            const float x = bars.getX() + (float) (k - 1) * pitch;
            const auto column = juce::Rectangle<float> (x + 2.0f, bars.getY(), pitch - 4.0f, bars.getHeight());

            if (level > 0.0f)
            {
                const auto bar = column.withTop (column.getBottom() - level * column.getHeight());
                const auto hue = newest ? neon::amber : neon::cyan;
                const float alpha = playing ? 1.0f : 0.22f;
                g.setColour (hue.withAlpha (0.16f * alpha));
                g.fillRect (bar.expanded (1.5f, 0.0f));
                g.setColour (hue.withAlpha (0.55f * alpha));
                g.fillRect (bar);
                g.setColour (neon::phosphorOf (hue).withMultipliedAlpha (alpha));
                g.fillRect (bar.withHeight (juce::jmin (2.0f, bar.getHeight())));   // a bright cap at the level
            }
            else
            {
                g.setColour (neon::cyan.withAlpha (playing ? 0.22f : 0.08f));   // an empty slot: a stub on the axis
                g.fillRect (column.withTop (column.getBottom() - 2.0f));
            }

            // The phase dial: a faint ring, a dot at the phase's angle (dim where the phase changes nothing).
            const juce::Point<float> centre { x + pitch * 0.5f, dials.getCentreY() };
            const float radius = juce::jmin (5.0f, pitch * 0.5f - 2.0f);
            const float phase = spectrum.phases[(size_t) (k - 1)];
            const float strength = level > 0.0f && playing ? 1.0f : 0.3f;
            g.setColour (neon::magenta.withAlpha (0.28f * strength));
            g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre), 1.0f);
            neon::drawPhosphorDot (g, centre + juce::Point<float> (radius * std::cos (phase), -radius * std::sin (phase)),
                                   neon::magenta, 3.0f, strength);
        }

        // Harmonic numbers under the bars, and the count control's name.
        const auto numbers = numberRow();
        g.setFont (neon::font (12.0f));
        for (const int k : { 1, 4, 8, 12, 16, 20, 24, 28, 32 })
        {
            const float cx = bars.getX() + ((float) k - 0.5f) * pitch;
            g.setColour (neon::phosphorOf (neon::cyan).withAlpha (k <= shown.count ? 0.6f : 0.3f));
            g.drawText (juce::String (k), juce::Rectangle<float> (cx - 12.0f, numbers.getY(), 24.0f, numbers.getHeight()),
                        juce::Justification::centred);
        }
        neon::crt::drawGlowText (g, "HARMONICS", countSlider.getBounds().toFloat().withX (8.0f).withWidth (76.0f),
                                 neon::phosphorOf (neon::cyan).withAlpha (0.8f), neon::font (12.0f, true),
                                 juce::Justification::centredLeft, 0.2f);
        if (! shown.custom)
            neon::crt::drawGlowText (g, "CUSTOM is off: WAVEFORM plays", bars.withHeight (16.0f).translated (0.0f, 2.0f),
                                     neon::phosphorOf (neon::amber).withAlpha (0.75f), neon::font (12.0f),
                                     juce::Justification::topRight, 0.2f);
    }

    //==========================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        // Only a press on the bars (with their dials and numbers) edits; a drag may then roam.
        dragging = barsArea().withTop (dialRow().getY()).withBottom (numberRow().getBottom()).contains (e.position);
        lastDrag = e.position;
        if (dragging)
            setLevelsAlong (e.position, e.position);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging)
            return;
        setLevelsAlong (lastDrag, e.position);
        lastDrag = e.position;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (const int k = harmonicAt (e.position.x); k > 0)
        {
            auto s = processor.getHarmonicSpectrum();
            s.levels[(size_t) (k - 1)] = 0.0f;
            setSpectrum (s);
        }
    }

    //==========================================================================
    /** True if the spectrum is the SQUARE or SAW series (levels and phases), whose jump shows Gibbs's overshoot. */
    static bool isGibbsSeries (const HarmonicSpectrum& s) noexcept
    {
        return sameSeries (s, HarmonicSpectrum::square()) || sameSeries (s, HarmonicSpectrum::saw());
    }

    /** "16 of 32", or with all 32 playing and the SQUARE or SAW series in place, Gibbs's overshoot. */
    static juce::String readoutText (int harmonicsPlaying, const HarmonicSpectrum& s)
    {
        if (harmonicsPlaying >= numHarmonics && isGibbsSeries (s))
            return "Gibbs: 9% overshoot at the jump";
        return juce::String (harmonicsPlaying) + " of " + juce::String (numHarmonics);
    }

    bool isBuilding() const noexcept   { return building; }

    /** Where bar k (1..32) is, in the view's coordinates (tests drag it). */
    juce::Rectangle<float> barBounds (int k) const
    {
        const auto bars = barsArea();
        const float pitch = bars.getWidth() / (float) numHarmonics;
        return { bars.getX() + (float) (k - 1) * pitch, bars.getY(), pitch, bars.getHeight() };
    }

private:
    /** The bars' area: under the phase dials, above the harmonic numbers and the bottom row. */
    juce::Rectangle<float> barsArea() const
    {
        return { 12.0f, 54.0f, (float) getWidth() - 24.0f, (float) getHeight() - 54.0f - 50.0f };
    }
    juce::Rectangle<float> dialRow() const     { const auto b = barsArea(); return { b.getX(), 34.0f, b.getWidth(), 16.0f }; }
    juce::Rectangle<float> numberRow() const   { const auto b = barsArea(); return { b.getX(), b.getBottom() + 3.0f, b.getWidth(), 14.0f }; }

    /** The harmonic (1..32) under x, or 0 outside the bars. */
    int harmonicAt (float x) const noexcept
    {
        const auto bars = barsArea();
        const float fraction = (x - bars.getX()) / juce::jmax (1.0f, bars.getWidth());
        if (fraction < 0.0f || fraction >= 1.0f)
            return 0;
        return juce::jmin ((int) (fraction * (float) numHarmonics), numHarmonics - 1) + 1;
    }

    float levelAt (float y) const noexcept
    {
        const auto bars = barsArea();
        return juce::jlimit (0.0f, 1.0f, (bars.getBottom() - y) / juce::jmax (1.0f, bars.getHeight()));
    }

    /** Sets every bar between two pointer positions to the height of the line joining them there. */
    void setLevelsAlong (juce::Point<float> from, juce::Point<float> to)
    {
        const auto bars = barsArea();
        auto s = processor.getHarmonicSpectrum();
        const int a = harmonicAt (juce::jlimit (bars.getX(), bars.getRight() - 0.01f, from.x));
        const int b = harmonicAt (juce::jlimit (bars.getX(), bars.getRight() - 0.01f, to.x));
        if (a == 0 || b == 0)
            return;
        for (int k = juce::jmin (a, b); k <= juce::jmax (a, b); ++k)
        {
            float y = to.y;
            if (a != b)
            {
                const float centre = barBounds (k).getCentreX();
                const float t = juce::jlimit (0.0f, 1.0f, (centre - from.x) / (to.x - from.x));
                y = from.y + t * (to.y - from.y);
            }
            if (k == b)
                y = to.y;   // the bar under the pointer follows it exactly
            s.levels[(size_t) (k - 1)] = levelAt (y);
        }
        setSpectrum (s);
    }

    /**
        Every edit here (a bar, a preset, ZERO PHASE) also drops a drawn cycle, so the SHAPE strip's
        faint drawn line never disagrees with what plays. BUILD moves only HARMONICS and keeps it.
    */
    void setSpectrum (const HarmonicSpectrum& s)
    {
        if (s != processor.getHarmonicSpectrum() || processor.getCycleDrawing().has_value())
            processor.setCycleDrawing (std::nullopt, s);
        refresh();
    }

    static bool sameSeries (const HarmonicSpectrum& a, const HarmonicSpectrum& b) noexcept
    {
        constexpr float tolerance = 1.0e-4f;
        for (size_t i = 0; i < (size_t) numHarmonics; ++i)
        {
            if (std::abs (a.levels[i] - b.levels[i]) > tolerance)
                return false;
            if (b.levels[i] > 0.0f)   // a phase matters only where there is a level
            {
                const float d = std::remainder (a.phases[i] - b.phases[i], juce::MathConstants<float>::twoPi);
                if (std::abs (d) > tolerance)
                    return false;
            }
        }
        return true;
    }

    //==========================================================================
    // BUILD

    int count() const
    {
        return juce::roundToInt (countParameter->convertFrom0to1 (countParameter->getValue()));
    }

    void setCount (int n)
    {
        countParameter->setValueNotifyingHost (countParameter->convertTo0to1 ((float) n));
        builtCount = n;
    }

    void startBuild()
    {
        building = true;
        countParameter->beginChangeGesture();
        setCount (1);
        startTimer (1000 / buildStepsPerSecond);
        refresh();
    }

    void stopBuild()
    {
        stopTimer();
        if (std::exchange (building, false))
            countParameter->endChangeGesture();
        buildButton.setToggleState (false, juce::dontSendNotification);
    }

    /** One BUILD step: the next harmonic joins; at 32 (or if someone else moved HARMONICS) it stops. */
    void timerCallback() override
    {
        const int now = count();
        if (now != builtCount)        // someone else moved HARMONICS: they have it now
            stopBuild();
        else if (now < numHarmonics)
            setCount (now + 1);
        if (builtCount >= numHarmonics)
            stopBuild();
        refresh();
    }

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
