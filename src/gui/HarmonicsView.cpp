// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "gui/HarmonicsView.h"

HarmonicsView::HarmonicsView (NeonSynthProcessor& p, juce::AudioProcessorValueTreeState& state)
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

HarmonicsView::~HarmonicsView()
{
    stopBuild();   // ends the host gesture if BUILD was running
}

void HarmonicsView::resized()
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

void HarmonicsView::refresh()
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

void HarmonicsView::paint (juce::Graphics& g)
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

void HarmonicsView::mouseDown (const juce::MouseEvent& e)
{
    // Only a press on the bars (with their dials and numbers) edits; a drag may then roam.
    dragging = barsArea().withTop (dialRow().getY()).withBottom (numberRow().getBottom()).contains (e.position);
    lastDrag = e.position;
    if (dragging)
        setLevelsAlong (e.position, e.position);
}

void HarmonicsView::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;
    setLevelsAlong (lastDrag, e.position);
    lastDrag = e.position;
}

void HarmonicsView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (const int k = harmonicAt (e.position.x); k > 0)
    {
        auto s = processor.getHarmonicSpectrum();
        s.levels[(size_t) (k - 1)] = 0.0f;
        setSpectrum (s);
    }
}

bool HarmonicsView::isGibbsSeries (const HarmonicSpectrum& s) noexcept
{
    return sameSeries (s, HarmonicSpectrum::square()) || sameSeries (s, HarmonicSpectrum::saw());
}

juce::String HarmonicsView::readoutText (int harmonicsPlaying, const HarmonicSpectrum& s)
{
    if (harmonicsPlaying >= numHarmonics && isGibbsSeries (s))
        return "Gibbs: 9% overshoot at the jump";
    return juce::String (harmonicsPlaying) + " of " + juce::String (numHarmonics);
}

juce::Rectangle<float> HarmonicsView::barBounds (int k) const
{
    const auto bars = barsArea();
    const float pitch = bars.getWidth() / (float) numHarmonics;
    return { bars.getX() + (float) (k - 1) * pitch, bars.getY(), pitch, bars.getHeight() };
}

juce::Rectangle<float> HarmonicsView::barsArea() const
{
    return { 12.0f, 54.0f, (float) getWidth() - 24.0f, (float) getHeight() - 54.0f - 50.0f };
}

int HarmonicsView::harmonicAt (float x) const noexcept
{
    const auto bars = barsArea();
    const float fraction = (x - bars.getX()) / juce::jmax (1.0f, bars.getWidth());
    if (fraction < 0.0f || fraction >= 1.0f)
        return 0;
    return juce::jmin ((int) (fraction * (float) numHarmonics), numHarmonics - 1) + 1;
}

float HarmonicsView::levelAt (float y) const noexcept
{
    const auto bars = barsArea();
    return juce::jlimit (0.0f, 1.0f, (bars.getBottom() - y) / juce::jmax (1.0f, bars.getHeight()));
}

void HarmonicsView::setLevelsAlong (juce::Point<float> from, juce::Point<float> to)
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

void HarmonicsView::setSpectrum (const HarmonicSpectrum& s)
{
    if (s != processor.getHarmonicSpectrum() || processor.getCycleDrawing().has_value())
        processor.setCycleDrawing (std::nullopt, s);
    refresh();
}

bool HarmonicsView::sameSeries (const HarmonicSpectrum& a, const HarmonicSpectrum& b) noexcept
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

int HarmonicsView::count() const
{
    return juce::roundToInt (countParameter->convertFrom0to1 (countParameter->getValue()));
}

void HarmonicsView::setCount (int n)
{
    countParameter->setValueNotifyingHost (countParameter->convertTo0to1 ((float) n));
    builtCount = n;
}

void HarmonicsView::startBuild()
{
    building = true;
    countParameter->beginChangeGesture();
    setCount (1);
    startTimer (1000 / buildStepsPerSecond);
    refresh();
}

void HarmonicsView::stopBuild()
{
    stopTimer();
    if (std::exchange (building, false))
        countParameter->endChangeGesture();
    buildButton.setToggleState (false, juce::dontSendNotification);
}

void HarmonicsView::timerCallback()
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
