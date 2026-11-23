// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "gui/CrtTheme.h"
#include <deque>

/*
    The neon palette and the drawing helpers every view shares (the panel inset, the
    graticule, phosphor glow, the afterglow of moving traces) and the text formats of the
    readouts. Split out of Visualisers.h in v1.11.0, with the views each in their own header
    (ScopeView, SpectrumView, LevelMeter, AdsrView, VoiceLights; SignalHistory feeds the first two).

    Everything in the views runs on the message thread. Audio reaches them only through the
    processor's lock-free scope FIFO and the per-voice atomics, both read from the editor's
    timer. Repaint economy: the timer calls update() 30 times a second, but each view repaints
    only when what it would draw has changed; when the synth is silent and the displays have
    settled, nothing repaints at all.
*/

namespace neon
{
    inline const juce::Colour cyan    { 0xff19e6ff };
    inline const juce::Colour magenta { 0xffff4fd8 };
    inline const juce::Colour amber   { 0xffffb319 };
    inline const juce::Colour green   { 0xff5cff6a };   // modulation: the LFO, pitch bend and the mod wheel (v1.9.0)
    inline const juce::Colour red     { 0xffff3355 };
    inline const juce::Colour panel   { 0xff0b0e17 };   // the dark inset behind a view

    inline const juce::Colour background  { 0xff0a0c13 };   // the window
    inline const juce::Colour section     { 0xff121624 };   // a panel of controls
    inline const juce::Colour sectionEdge { 0xff232a3d };
    inline const juce::Colour text        { 0xffe6ebf5 };
    inline const juce::Colour muted       { 0xffa3adc2 };

    /** The small-text font (v1.10.0: IBM Plex Mono, embedded; SemiBold for "bold"). */
    inline juce::Font font (float height, bool bold = false)
    {
        return crt::labelFont (height, bold);
    }

    /** The glass behind a display (v1.10.0): darker than the tube around it, so a view reads as
        a screen set into the panel. */
    inline const juce::Colour displayInset { 0xff020906 };

    /** A display's inset (v1.10.0 CRT look): the dark glass, square corners, a thin 1 px edge in
        the view's hue and a faint glow just inside it. Flat fills and strokes only: cheap enough
        for the views that repaint every frame. */
    inline void drawPanel (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour edge)
    {
        g.setColour (displayInset);
        g.fillRect (r);
        g.setColour (edge.withAlpha (0.05f));
        g.drawRect (r.reduced (2.0f), 2.0f);
        g.setColour (edge.withAlpha (0.10f));
        g.drawRect (r.reduced (1.0f), 1.0f);
        g.setColour (edge.withAlpha (0.55f));
        g.drawRect (r, 1.0f);
    }

    /** The colour a trace's core burns at: its hue pushed most of the way to white, as phosphor
        looks where the beam is brightest (keeps the colour's alpha, so dimmed traces stay dim). */
    inline juce::Colour phosphorOf (juce::Colour hue)
    {
        return hue.interpolatedWith (juce::Colours::white.withAlpha (hue.getFloatAlpha()), 0.62f);
    }

    /** A scope graticule: faint division lines, a brighter centre cross with minor ticks (five
        per division) like a real scope's. Static: callers draw it into a crt::CachedPainter. */
    inline void drawGraticule (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour hue, int columns, int rows)
    {
        g.setColour (hue.withAlpha (0.09f));
        for (int i = 1; i < columns; ++i)
            g.fillRect (juce::Rectangle<float> (std::round (area.getX() + area.getWidth() * (float) i / (float) columns), area.getY(), 1.0f, area.getHeight()));
        for (int i = 1; i < rows; ++i)
            g.fillRect (juce::Rectangle<float> (area.getX(), std::round (area.getY() + area.getHeight() * (float) i / (float) rows), area.getWidth(), 1.0f));

        const float cx = std::round (area.getCentreX()), cy = std::round (area.getCentreY());
        g.setColour (hue.withAlpha (0.18f));
        g.fillRect (juce::Rectangle<float> (cx, area.getY(), 1.0f, area.getHeight()));
        g.fillRect (juce::Rectangle<float> (area.getX(), cy, area.getWidth(), 1.0f));

        g.setColour (hue.withAlpha (0.26f));
        constexpr int ticksPerDivision = 5;   // minor ticks on the centre lines, as on an oscilloscope's graticule
        const float dx = area.getWidth() / (float) (columns * ticksPerDivision), dy = area.getHeight() / (float) (rows * ticksPerDivision);
        for (int i = 1; i < columns * ticksPerDivision; ++i)
            g.fillRect (juce::Rectangle<float> (std::round (area.getX() + dx * (float) i), cy - 2.0f, 1.0f, 5.0f));
        for (int i = 1; i < rows * ticksPerDivision; ++i)
            g.fillRect (juce::Rectangle<float> (cx - 2.0f, std::round (area.getY() + dy * (float) i), 5.0f, 1.0f));
    }

    /** Strokes a path as a phosphor trace: two wide faint passes in the hue (the halo) under a
        thin core burnt nearly white. Three strokes, no blur. */
    inline void strokeGlow (juce::Graphics& g, const juce::Path& p, juce::Colour c, float width = 1.4f)
    {
        const auto stroke = [&] (juce::Colour colour, float w)
        {
            g.setColour (colour);
            g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };
        stroke (c.withMultipliedAlpha (0.09f), width * 5.0f);
        stroke (c.withMultipliedAlpha (0.30f), width * 2.4f);
        stroke (phosphorOf (c), width);
    }

    /** A phosphor dot (a voice on its envelope, the LFO's position): a halo in the hue, a near-white core. */
    inline void drawPhosphorDot (juce::Graphics& g, juce::Point<float> centre, juce::Colour hue, float coreSize, float alpha = 1.0f)
    {
        g.setColour (hue.withMultipliedAlpha (0.18f * alpha));
        g.fillEllipse (juce::Rectangle<float> (coreSize * 2.8f, coreSize * 2.8f).withCentre (centre));
        g.setColour (hue.withMultipliedAlpha (0.40f * alpha));
        g.fillEllipse (juce::Rectangle<float> (coreSize * 1.7f, coreSize * 1.7f).withCentre (centre));
        g.setColour (phosphorOf (hue).withMultipliedAlpha (alpha));
        g.fillEllipse (juce::Rectangle<float> (coreSize, coreSize).withCentre (centre));
    }

    /** A vertical marker line with a glow (sample start, playheads, the cutoff): a faint wider bar under it. */
    inline void drawGlowLine (juce::Graphics& g, float x, float top, float bottom, juce::Colour colour, float width = 2.0f)
    {
        g.setColour (colour.withMultipliedAlpha (0.22f));
        g.fillRect (juce::Rectangle<float> (x - width * 1.5f, top, width * 3.0f, bottom - top));
        g.setColour (colour);
        g.fillRect (juce::Rectangle<float> (x - width * 0.5f, top, width, bottom - top));
    }

    //==========================================================================
    /**
        The phosphor's afterglow (v1.10.0) for the scope and the spectrum: the last few traces,
        each fading out over crt::afterglowSeconds after it left the screen. Kept as paths, not
        as an image, so it costs one thin stroke per remembered trace and stays sharp at every
        zoom. Once the last one has faded, advance() says so and the view can stop repainting.
    */
    class Afterglow
    {
    public:
        /** Keeps a trace that is leaving the screen (nowMs: juce::Time::getMillisecondCounterHiRes()). */
        void push (const juce::Path& trace, double nowMs)
        {
            if (trace.isEmpty())
                return;
            if (ghosts.size() >= maxGhosts)
                ghosts.pop_back();
            ghosts.push_front ({ trace, nowMs });
        }

        /** Drops the traces that have faded. True if any was still glowing: the view must
            repaint once more to fade it further (or to clear the last one). */
        bool advance (double nowMs)
        {
            const bool glowing = ! ghosts.empty();
            while (! ghosts.empty() && nowMs - ghosts.back().bornMs >= lifetimeMs)
                ghosts.pop_back();
            return glowing;
        }

        void clear() { ghosts.clear(); }

        /** Strokes the remembered traces, oldest faintest. */
        void paint (juce::Graphics& g, juce::Colour hue, float width) const
        {
            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            for (auto it = ghosts.rbegin(); it != ghosts.rend(); ++it)
            {
                const float left = 1.0f - (float) ((nowMs - it->bornMs) / lifetimeMs);
                if (left <= 0.0f)
                    continue;
                g.setColour (hue.withMultipliedAlpha (0.45f * left * left));
                g.strokePath (it->trace, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }

    private:
        struct Ghost { juce::Path trace; double bornMs = 0.0; };
        static constexpr size_t maxGhosts = 6;   // 0.15 s at the editor's 30 Hz is under five frames
        static constexpr double lifetimeMs = 1000.0 * (double) crt::afterglowSeconds;
        std::deque<Ghost> ghosts;
    };

    inline juce::String formatTime (float seconds)
    {
        return seconds < 1.0f ? juce::String (juce::roundToInt (seconds * 1000.0f)) + " ms"
                              : juce::String (seconds, 2) + " s";
    }

    inline juce::String formatHz (float hz)
    {
        return hz < 1000.0f ? juce::String (juce::roundToInt (hz)) + " Hz"
                            : juce::String (hz / 1000.0f, hz < 10000.0f ? 2 : 1) + " kHz";
    }
}
