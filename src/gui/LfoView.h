// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "dsp/Lfo.h"
#include "gui/NeonStyle.h"

/**
    The MODULATION panel's LFO picture (v1.9.0): one cycle of the chosen shape,
    with a dot at the LFO's current phase.

    - Sine, Triangle, Square and Saw are drawn from shapeAt() below; the dot
      rides the drawn curve at the phase the processor reports.
    - Random has no fixed cycle, so it is drawn as a few held steps (a fixed
      example set) and the dot sits at the LFO's actual last value, so it moves
      to each new random level as it is picked.
    - While all three depths are 0 the LFO changes nothing, so the picture is
      dimmed: a hint that turning PITCH, CUTOFF or AMP is what brings it in.

    update() is called by the editor's 30 Hz timer and repaints only when what
    is shown has changed. Message thread only.
*/
class LfoView : public juce::Component,
                public juce::SettableTooltipClient
{
public:
    LfoView()
    {
        setTooltip ("The LFO: one cycle of its shape, the dot at where it is now. "
                    "PITCH, CUTOFF and AMP set how far it moves each; at 0 it changes nothing.");
    }

    /** The value of a shape at a phase from 0 to 1, from -1 to +1: the LFO's cycle as drawn here. */
    static float shapeAt (Lfo::Shape shape, float phase) noexcept
    {
        phase -= std::floor (phase);
        switch (shape)
        {
            case Lfo::Shape::sine:     return std::sin (juce::MathConstants<float>::twoPi * phase);
            case Lfo::Shape::triangle: return phase < 0.25f ? 4.0f * phase
                                            : phase < 0.75f ? 2.0f - 4.0f * phase
                                                            : 4.0f * phase - 4.0f;
            case Lfo::Shape::square:   return phase < 0.5f ? 1.0f : -1.0f;
            case Lfo::Shape::saw:      return 2.0f * phase - 1.0f;
            case Lfo::Shape::random:   break;
        }
        // Random: a fixed example of held steps, only for the picture.
        constexpr float steps[] = { 0.35f, -0.6f, 0.85f, -0.15f, -0.9f, 0.55f };
        return steps[juce::jlimit (0, (int) std::size (steps) - 1, (int) (phase * (float) std::size (steps)))];
    }

    void update (Lfo::Shape shape, float phase, float value, bool active)
    {
        const Shown now { shape, phase, value, active };
        if (now != shown)
        {
            shown = now;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        neon::drawPanel (g, bounds, neon::green);
        const auto plot = bounds.reduced (6.0f, 8.0f);

        const auto xFor = [&plot] (float phase) { return plot.getX() + plot.getWidth() * phase; };
        const auto yFor = [&plot] (float v)     { return plot.getCentreY() - 0.5f * plot.getHeight() * v; };

        // The zero line
        g.setColour (neon::green.withAlpha (0.18f));
        g.drawHorizontalLine (juce::roundToInt (plot.getCentreY()), plot.getX(), plot.getRight());

        // One cycle. Steps jump straight up or down, so the square and the held levels draw square.
        juce::Path path;
        const int points = juce::jmax (2, juce::roundToInt (plot.getWidth()));
        float last = shapeAt (shown.shape, 0.0f);
        path.startNewSubPath (xFor (0.0f), yFor (last));
        for (int i = 1; i <= points; ++i)
        {
            const float phase = (float) i / (float) points;
            const float v = shapeAt (shown.shape, juce::jmin (phase, 0.9999f));
            if (std::abs (v - last) > 0.5f)
                path.lineTo (xFor (phase), yFor (last));   // a jump: across, then up or down
            path.lineTo (xFor (phase), yFor (v));
            last = v;
        }
        const float alpha = shown.active ? 1.0f : 0.45f;
        neon::strokeGlow (g, path, neon::green.withMultipliedAlpha (alpha), 1.3f);

        // Where the LFO is now
        const float y = shown.shape == Lfo::Shape::random ? yFor (juce::jlimit (-1.0f, 1.0f, shown.value))
                                                          : yFor (shapeAt (shown.shape, shown.phase));
        neon::drawPhosphorDot (g, { xFor (shown.phase), y }, neon::green, 4.5f, alpha);
    }

private:
    struct Shown
    {
        Lfo::Shape shape = Lfo::Shape::sine;
        float phase = 0.0f, value = 0.0f;
        bool  active = false;
        bool operator!= (const Shown& o) const noexcept
        {
            return shape != o.shape || phase != o.phase || value != o.value || active != o.active;
        }
    };
    Shown shown;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LfoView)
};

/**
    A small bar showing where a MIDI controller is now (v1.9.0): the pitch bend
    (centred, -1 to +1, lit from the middle out) or the mod wheel (0 to 1, lit
    from the left). It only shows; the controller itself moves it.
*/
class ControllerBar : public juce::Component,
                      public juce::SettableTooltipClient
{
public:
    explicit ControllerBar (bool isCentred) : centred (isCentred) {}

    void update (float newValue)
    {
        if (newValue != value)
        {
            value = newValue;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        neon::drawPanel (g, bounds, neon::green);
        const auto inner = bounds.reduced (2.0f);

        juce::Rectangle<float> lit;
        if (centred)
        {
            const float v = juce::jlimit (-1.0f, 1.0f, value);
            const float mid = inner.getCentreX(), end = mid + 0.5f * inner.getWidth() * v;
            lit = { juce::jmin (mid, end), inner.getY(), std::abs (end - mid), inner.getHeight() };
        }
        else
        {
            lit = inner.withWidth (inner.getWidth() * juce::jlimit (0.0f, 1.0f, value));
        }
        // A block bar: the lit part glows, then thin dark gaps cut it into blocks. The lit edge
        // stays exact (the gaps are drawn over it), so the bar reads as finely as before.
        g.setColour (neon::green.withAlpha (0.10f));
        g.fillRect (inner);
        if (! lit.isEmpty())
        {
            g.setColour (neon::green.withAlpha (0.25f));
            g.fillRect (lit.expanded (1.0f, 0.5f).getIntersection (bounds));
            g.setColour (neon::phosphorOf (neon::green).withAlpha (0.9f));
            g.fillRect (lit);
        }
        g.setColour (neon::displayInset);
        for (float x = inner.getX() + blockPitch; x < inner.getRight() - 1.0f; x += blockPitch)
            g.fillRect (juce::Rectangle<float> (x - 0.5f, inner.getY(), 1.0f, inner.getHeight()));
        if (centred)
        {
            g.setColour (neon::green.withAlpha (0.6f));
            g.fillRect (inner.getCentreX() - 0.5f, bounds.getY() + 1.0f, 1.0f, bounds.getHeight() - 2.0f);   // centre mark, on top
        }
    }

private:
    static constexpr float blockPitch = 4.0f;   // design units from one block to the next

    const bool centred;
    float value = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControllerBar)
};
