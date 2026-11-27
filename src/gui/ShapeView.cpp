// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "gui/ShapeView.h"

ShapeView::ShapeView (NeonSynthProcessor& p, juce::AudioProcessorValueTreeState& state)
    : processor (p)
{
    sourceValue   = state.getRawParameterValue ("source");
    waveformValue = state.getRawParameterValue ("waveform");
    overtoneValue = state.getRawParameterValue ("chopOvertone");
    combValue     = state.getRawParameterValue ("chopComb");
    driveValue    = state.getRawParameterValue ("foldDrive");
    foldMixValue  = state.getRawParameterValue ("foldMix");

    customValue    = state.getRawParameterValue ("custom");
    harmonicsValue = state.getRawParameterValue ("harmonics");

    RandomStepOscillator example;   // a fixed set of levels for when no Random voice sounds
    example.reseed (1);
    exampleLevels = example.getLevels();

    setTooltip (chopTooltip);
    refresh();   // v1.12.0: reads CUSTOM's spectrum before the first paint
}

void ShapeView::setDrawMode (bool on)
{
    if (std::exchange (drawMode, on) != on)
    {
        refresh();
        repaint();   // the caption, even when nothing else changed
    }
}

bool ShapeView::isDrawing() const noexcept
{
    return drawMode && customValue->load() >= 0.5f
        && juce::roundToInt (sourceValue->load()) != (int) NeonSynthVoice::Source::random;
}

void ShapeView::refresh()
{
    Shown now;
    now.random   = juce::roundToInt (sourceValue->load()) == (int) NeonSynthVoice::Source::random;
    now.waveform = juce::roundToInt (waveformValue->load());
    now.overtone = overtoneValue->load() * 0.01f;
    now.comb     = juce::jlimit (1, ChopSettings::maxComb, juce::roundToInt (combValue->load()));
    now.drive    = driveValue->load();
    now.foldMix  = foldMixValue->load() * 0.01f;
    now.pattern  = processor.getChopPattern();
    now.levels   = exampleLevels;
    now.custom    = customValue->load() >= 0.5f;                                   // v1.12.0
    now.harmonics = juce::jlimit (1, HarmonicSpectrum::numHarmonics, juce::roundToInt (harmonicsValue->load()));
    now.fourierVersion = processor.getFourierVersion();
    now.drawing   = isDrawing();

    if (now.random)
    {
        float lowestHz = 0.0f;
        for (const auto& v : processor.getVoiceViews())
        {
            const float hz = v.frequencyHz.load (std::memory_order_relaxed);
            if (v.active.load (std::memory_order_relaxed) && v.playingRandom.load (std::memory_order_relaxed)
                && hz > 0.0f && (lowestHz == 0.0f || hz < lowestHz))
            {
                lowestHz = hz;
                for (size_t k = 0; k < now.levels.size(); ++k)
                    now.levels[k] = v.randomLevels[k].load (std::memory_order_relaxed);
            }
        }
    }

    if (! (now == shown))
    {
        const bool newSpectrum = now.fourierVersion != shown.fourierVersion;
        const bool newWave = newSpectrum || now.harmonics != shown.harmonics;
        if (now.drawing != shown.drawing)
            setTooltip (now.drawing ? drawTooltip : chopTooltip);
        shown = now;
        if (newSpectrum)
        {
            spectrum = processor.getHarmonicSpectrum();
            drawing = processor.getCycleDrawing();
            scale = CycleAnalysis::playingScale (spectrum);
        }
        if (newWave)
            CycleAnalysis::resynthesise (spectrum, shown.harmonics, customWave.data(), (int) customWave.size(), scale);
        repaint();
    }
}

void ShapeView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    neon::drawPanel (g, bounds, neon::cyan);
    const auto inner = bounds.reduced (4.0f, 3.0f);
    const int steps = Chop::stepsPerPattern * shown.comb;
    const float stepWidth = inner.getWidth() / (float) steps;

    // Chopped steps, shaded; step edges as faint lines (only where they are far enough apart to read).
    // Chopped steps also get an amber block along the bottom edge, like a lit step in a row of them.
    for (int s = 0; s < steps; ++s)
        if (((shown.pattern >> (s % Chop::stepsPerPattern)) & 1u) != 0)
        {
            const float strength = shown.overtone > 0.0f ? 1.0f : 0.4f;
            const auto column = juce::Rectangle<float> (inner.getX() + (float) s * stepWidth, inner.getY(), stepWidth, inner.getHeight());
            g.setColour (neon::amber.withAlpha (0.18f * strength));
            g.fillRect (column);
            g.setColour (neon::amber.withAlpha (0.75f * strength));
            g.fillRect (column.withTop (column.getBottom() - 4.0f).reduced (stepWidth >= 6.0f ? 1.0f : 0.0f, 0.0f));
        }
    if (stepWidth >= 3.0f)
    {
        g.setColour (neon::cyan.withAlpha (0.12f));
        for (int s = 1; s < steps; ++s)
            g.drawVerticalLine ((int) (inner.getX() + (float) s * stepWidth), inner.getY(), inner.getBottom());
    }

    // The centre line and the wave.
    const float mid = inner.getCentreY(), halfHeight = inner.getHeight() * 0.5f / 1.2f;   // room for a little overshoot
    g.setColour (neon::cyan.withAlpha (0.2f));
    g.drawHorizontalLine ((int) mid, inner.getX(), inner.getRight());

    // v1.12.0: the drawn cycle, faintly behind the wave it plays as (brighter while being drawn).
    const bool customShown = shown.custom && ! shown.random;
    if (const auto* line = dragging ? &pending : (customShown && drawing.has_value() ? &*drawing : nullptr))
    {
        juce::Path drawn;
        for (int i = 0; i <= CycleAnalysis::numPoints; ++i)   // back to point 0, to close the cycle
        {
            const float x = inner.getX() + inner.getWidth() * (float) i / (float) CycleAnalysis::numPoints;
            const float y = mid - (*line)[(size_t) (i % CycleAnalysis::numPoints)] * halfHeight;
            if (i == 0) drawn.startNewSubPath (x, y);
            else        drawn.lineTo (x, y);
        }
        g.setColour (neon::magenta.withAlpha (dragging ? 0.9f : 0.6f));
        g.strokePath (drawn, juce::PathStrokeType (1.5f));
    }

    juce::Path wave;
    const int points = juce::jmax (2, (int) inner.getWidth());
    for (int i = 0; i < points; ++i)
    {
        const float phase = ((float) i + 0.5f) / (float) points;
        const float y = mid - valueAt (phase) * halfHeight;
        const float x = inner.getX() + inner.getWidth() * phase;
        if (i == 0) wave.startNewSubPath (x, y);
        else        wave.lineTo (x, y);
    }
    neon::strokeGlow (g, wave, neon::cyan, 1.5f);

    // v1.12.0: while DRAW is on, say what drawing does (or what it needs).
    // On a dark plate, so it reads over the wave where the two cross.
    if (drawMode && ! shown.random)
    {
        const auto font = neon::font (12.0f);
        const juce::String caption (shown.drawing ? drawCaption : needsCustomCaption);
        const auto plate = juce::Rectangle<float> (inner.getX() + 2.0f, inner.getY() + 2.0f,
                                                   juce::GlyphArrangement::getStringWidth (font, caption) + 10.0f, 16.0f);
        g.setColour (neon::displayInset.withAlpha (0.85f));
        g.fillRect (plate);
        neon::crt::drawGlowText (g, caption, plate, neon::phosphorOf (neon::magenta).withAlpha (shown.drawing ? 1.0f : 0.7f),
                                 font, juce::Justification::centred, 0.3f);
    }
}

void ShapeView::mouseDown (const juce::MouseEvent& e)
{
    if (isDrawing())   // v1.12.0
    {
        if (e.getNumberOfClicks() > 1)
            return;   // the second press of a double-click: mouseDoubleClick clears the drawing
        pending = drawing.has_value() ? *drawing : playingCycle();
        dragging = true;
        lastPoint = pointAt (e.position);
        pending[(size_t) lastPoint.index] = lastPoint.value;
        repaint();
        return;
    }

    const int step = stepAt ((float) e.x);
    if (step < 0)
        return;
    paintChopped = ((processor.getChopPattern() >> step) & 1u) == 0;   // the click flips this step; a drag copies that
    lastStep = step;
    processor.setChopStep (step, paintChopped);
    refresh();
}

void ShapeView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging)   // v1.12.0: draw from the last point to this one, filling the points in between
    {
        const auto to = pointAt (e.position);
        const int span = std::abs (to.index - lastPoint.index);
        const int direction = to.index >= lastPoint.index ? 1 : -1;
        for (int n = 1; n <= span; ++n)
        {
            const int index = lastPoint.index + n * direction;
            pending[(size_t) index] = juce::jmap ((float) n / (float) span, lastPoint.value, to.value);
        }
        pending[(size_t) to.index] = to.value;
        lastPoint = to;
        repaint();
        return;
    }

    const int step = stepAt ((float) e.x);
    if (step < 0 || step == lastStep)
        return;
    lastStep = step;
    processor.setChopStep (step, paintChopped);
    refresh();
}

void ShapeView::mouseUp (const juce::MouseEvent&)
{
    if (! std::exchange (dragging, false))
        return;
    processor.setCycleDrawing (pending, CycleAnalysis::analyse (pending));
    refresh();
    repaint();
}

void ShapeView::mouseDoubleClick (const juce::MouseEvent&)
{
    if (! isDrawing())
        return;
    dragging = false;
    processor.setCycleDrawing (std::nullopt, HarmonicSpectrum::sine());
    refresh();
    repaint();
}

ShapeView::DrawPoint ShapeView::pointAt (juce::Point<float> position) const noexcept
{
    const auto inner = getLocalBounds().toFloat().reduced (4.0f, 3.0f);
    const float mid = inner.getCentreY(), halfHeight = inner.getHeight() * 0.5f / 1.2f;   // as paint() draws the wave
    const float fraction = (position.x - inner.getX()) / juce::jmax (1.0f, inner.getWidth());
    return { juce::jlimit (0, CycleAnalysis::numPoints - 1, juce::roundToInt (fraction * (float) CycleAnalysis::numPoints)),
             juce::jlimit (-1.0f, 1.0f, (mid - position.y) / juce::jmax (1.0f, halfHeight)) };
}

CycleAnalysis::Cycle ShapeView::playingCycle() const noexcept
{
    CycleAnalysis::Cycle c {};
    CycleAnalysis::resynthesise (spectrum, shown.harmonics, c.data(), (int) c.size(), scale);
    for (auto& v : c)
        v = juce::jlimit (-1.0f, 1.0f, v);
    return c;
}

float ShapeView::valueAt (float phase) const noexcept
{
    float x;
    if (shown.random)
        x = shown.levels[(size_t) juce::jmin ((int) (phase * (float) RandomStepOscillator::numSteps), RandomStepOscillator::numSteps - 1)];
    else if (shown.custom)   // v1.12.0: CUSTOM's wave, between its precomputed points
    {
        const float position = phase * (float) customWavePoints;
        const int i = juce::jlimit (0, customWavePoints - 1, (int) position);
        x = juce::jmap (position - (float) i, customWave[(size_t) i], customWave[(size_t) ((i + 1) % customWavePoints)]);
    }
    else switch (shown.waveform)
    {
        case PolyBlepOscillator::square:   x = phase < 0.5f ? 1.0f : -1.0f; break;
        case PolyBlepOscillator::triangle: x = phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase; break;
        case PolyBlepOscillator::sawtooth: x = 2.0f * phase - 1.0f; break;
        default:                           x = std::sin (juce::MathConstants<float>::twoPi * phase); break;
    }

    const int segment = juce::jmin ((int) (phase * (float) (Chop::stepsPerPattern * shown.comb)), Chop::stepsPerPattern * shown.comb - 1);
    if (((shown.pattern >> (segment % Chop::stepsPerPattern)) & 1u) != 0)
        x *= 1.0f - shown.overtone;

    if (shown.foldMix > 0.0f)
        x += shown.foldMix * (Sculpt::fastSin (juce::MathConstants<float>::halfPi * shown.drive * x) - x);
    return x;
}

int ShapeView::stepAt (float x) const noexcept
{
    const auto inner = getLocalBounds().toFloat().reduced (4.0f, 3.0f);
    const float fraction = (x - inner.getX()) / juce::jmax (1.0f, inner.getWidth());
    if (fraction < 0.0f || fraction >= 1.0f)
        return -1;
    const int steps = Chop::stepsPerPattern * shown.comb;
    return juce::jmin ((int) (fraction * (float) steps), steps - 1) % Chop::stepsPerPattern;
}
