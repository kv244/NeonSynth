// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "plugin/NeonSynthProcessor.h"
#include "dsp/Sculpt.h"
#include "dsp/CycleAnalysis.h"
#include "gui/NeonStyle.h"

/**
    One cycle of the Oscillator or Random wave as it leaves the Fold stage
    (v1.6.0), shown in the sample strip's place while either source plays.

    - The wave is drawn with CHOP and Fold applied; Noise is left out, since a
      single drawn cycle of noise would mislead more than it shows.
    - CHOP's 16 x Comb steps are marked along the cycle, the chopped ones shaded
      (faintly while Overtone is 0, when chopping changes nothing).
    - Click a step to toggle it; drag across steps to set them all the same way.
      Steps repeat with Comb, so a click toggles every copy of that step.
    - With Random, the drawing follows the 16 levels of the lowest sounding
      Random voice, which drift as Random re-rolls them; with none sounding it
      shows a fixed example set.

    It draws the ideal wave (no band-limiting): this is a picture of the shape,
    not of the exact samples. refresh() is called by the editor's 30 Hz timer
    and repaints only when something shown has changed. Message thread only.

    v1.12.0, CUSTOM and DRAW:
    - With CUSTOM on (and the Oscillator source) the wave is CUSTOM's: its spectrum's first
      HARMONICS harmonics, scaled as the voices' tables are, then CHOP and Fold as before.
    - If a cycle has been drawn, the drawn line is shown faintly behind that wave, so the
      rounding and ripple of 32 harmonics at a sharp corner (Gibbs) can be seen.
    - With DRAW on as well, the pointer draws the cycle instead of toggling CHOP's steps: a
      drag sets the 256 drawing points under it (filling those a fast drag skipped), and on
      release the drawing is analysed into 32 harmonics and handed to the processor once.
      A double-click clears the drawing back to a sine. With DRAW or CUSTOM off, or with
      Random, clicks toggle CHOP's steps as before.
*/
class ShapeView : public juce::Component,
                  public juce::SettableTooltipClient
{
public:
    ShapeView (NeonSynthProcessor& p, juce::AudioProcessorValueTreeState& state)
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

    /**
        v1.12.0, DRAW (the editor's switch, saved with the editor's view): with DRAW on and
        CUSTOM on, the pointer draws one cycle instead of toggling CHOP's steps.
    */
    void setDrawMode (bool on)
    {
        if (std::exchange (drawMode, on) != on)
        {
            refresh();
            repaint();   // the caption, even when nothing else changed
        }
    }

    bool isDrawMode() const noexcept   { return drawMode; }

    /** True while the pointer draws the cycle: DRAW and CUSTOM on, with the Oscillator source (v1.12.0). */
    bool isDrawing() const noexcept
    {
        return drawMode && customValue->load() >= 0.5f
            && juce::roundToInt (sourceValue->load()) != (int) NeonSynthVoice::Source::random;
    }

    /** Re-reads the settings and, for Random, the sounding levels. Message thread. */
    void refresh()
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

    void paint (juce::Graphics& g) override
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

    void mouseDown (const juce::MouseEvent& e) override
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

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging)   // v1.12.0: draw from the last point to this one, filling the points in between
        {
            const auto to = pointAt (e.position);
            const int span = std::abs (to.index - lastPoint.index);
            const int direction = to.index >= lastPoint.index ? 1 : -1;
            for (int n = 1; n <= span; ++n)
                pending[(size_t) (lastPoint.index + n * direction)] = juce::jmap ((float) n / (float) span, lastPoint.value, to.value);
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

    /** v1.12.0: a drawing is analysed and handed over once, when the drag ends. */
    void mouseUp (const juce::MouseEvent&) override
    {
        if (! std::exchange (dragging, false))
            return;
        processor.setCycleDrawing (pending, CycleAnalysis::analyse (pending));
        refresh();
        repaint();
    }

    /** v1.12.0: while drawing, a double-click clears the drawing back to a sine. */
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (! isDrawing())
            return;
        dragging = false;
        processor.setCycleDrawing (std::nullopt, HarmonicSpectrum::sine());
        refresh();
        repaint();
    }

private:
    static_assert (CycleAnalysis::numPoints == FourierState::drawingPoints, "a drawing is one analysed cycle");

    static constexpr const char* chopTooltip = "One cycle of the wave after CHOP and Fold. Click a step to chop it (or not); "
                                               "drag across steps to set several. OVERTONE sets how far chopped steps are cut.";
    static constexpr const char* drawTooltip = "Draw one cycle; it plays as its first 32 harmonics. "
                                               "Drag across the strip to draw; double-click for a sine.";
    static constexpr const char* drawCaption = "Draw one cycle; it plays as its first 32 harmonics";
    static constexpr const char* needsCustomCaption = "DRAW: switch CUSTOM on to draw a cycle";

    /** Points of CUSTOM's wave kept for drawing it; the strip is narrower than this, so the line stays smooth. */
    static constexpr int customWavePoints = 1024;

    /** A drawing point (0..255) and value (-1..1) under a position, clamped to the strip. */
    struct DrawPoint { int index = 0; float value = 0.0f; };

    DrawPoint pointAt (juce::Point<float> position) const noexcept
    {
        const auto inner = getLocalBounds().toFloat().reduced (4.0f, 3.0f);
        const float mid = inner.getCentreY(), halfHeight = inner.getHeight() * 0.5f / 1.2f;   // as paint() draws the wave
        const float fraction = (position.x - inner.getX()) / juce::jmax (1.0f, inner.getWidth());
        return { juce::jlimit (0, CycleAnalysis::numPoints - 1, juce::roundToInt (fraction * (float) CycleAnalysis::numPoints)),
                 juce::jlimit (-1.0f, 1.0f, (mid - position.y) / juce::jmax (1.0f, halfHeight)) };
    }

    /** The cycle now playing (before CHOP and Fold) at the drawing's points, to start a drawing from. */
    CycleAnalysis::Cycle playingCycle() const noexcept
    {
        CycleAnalysis::Cycle c {};
        CycleAnalysis::resynthesise (spectrum, shown.harmonics, c.data(), (int) c.size(), scale);
        for (auto& v : c)
            v = juce::jlimit (-1.0f, 1.0f, v);
        return c;
    }

    bool drawMode = false;   // v1.12.0
    struct Shown
    {
        bool random = false;
        int waveform = 0, comb = 1;
        float overtone = 0.0f, drive = 3.0f, foldMix = 0.0f;
        juce::uint16 pattern = 0;
        std::array<float, RandomStepOscillator::numSteps> levels {};
        bool custom = false, drawing = false;      // v1.12.0
        int harmonics = HarmonicSpectrum::numHarmonics;
        int fourierVersion = -1;                   // -1: not read yet

        bool operator== (const Shown& o) const noexcept
        {
            return random == o.random && waveform == o.waveform && comb == o.comb && overtone == o.overtone
                && drive == o.drive && foldMix == o.foldMix && pattern == o.pattern && levels == o.levels
                && custom == o.custom && drawing == o.drawing && harmonics == o.harmonics && fourierVersion == o.fourierVersion;
        }
    };

    /** The ideal wave at a phase, with CHOP's gain and Fold applied (no band-limiting, no noise). */
    float valueAt (float phase) const noexcept
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

    /** The pattern step (0..15) under x, or -1 outside the drawing. */
    int stepAt (float x) const noexcept
    {
        const auto inner = getLocalBounds().toFloat().reduced (4.0f, 3.0f);
        const float fraction = (x - inner.getX()) / juce::jmax (1.0f, inner.getWidth());
        if (fraction < 0.0f || fraction >= 1.0f)
            return -1;
        const int steps = Chop::stepsPerPattern * shown.comb;
        return juce::jmin ((int) (fraction * (float) steps), steps - 1) % Chop::stepsPerPattern;
    }

    NeonSynthProcessor& processor;
    std::atomic<float>* sourceValue = nullptr;
    std::atomic<float>* waveformValue = nullptr;
    std::atomic<float>* overtoneValue = nullptr;
    std::atomic<float>* combValue = nullptr;
    std::atomic<float>* driveValue = nullptr;
    std::atomic<float>* foldMixValue = nullptr;
    std::atomic<float>* customValue = nullptr;      // v1.12.0
    std::atomic<float>* harmonicsValue = nullptr;

    std::array<float, RandomStepOscillator::numSteps> exampleLevels {};
    Shown shown;
    bool paintChopped = true;
    int  lastStep = -1;

    // v1.12.0: CUSTOM's spectrum and drawing as last read (when getFourierVersion() moved), the
    // wave they play as, and the drawing in progress.
    HarmonicSpectrum spectrum = HarmonicSpectrum::defaultSpectrum();
    std::optional<FourierState::Drawing> drawing;
    float scale = HarmonicSpectrum::seriesGain;
    std::array<float, customWavePoints> customWave {};
    CycleAnalysis::Cycle pending {};
    bool dragging = false;
    DrawPoint lastPoint;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShapeView)
};
