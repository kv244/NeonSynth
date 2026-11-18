// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "CrtTheme.h"
#include "Visualisers.h"

/*
    The CRT frame (v1.10.0): everything the editor paints under its controls. The warm
    grey bezel around the window, the green-black tube glass inside it, the panels as thin
    glowing phosphor lines, their "> TITLE" headings in VT323, the static labels in IBM
    Plex Mono, the NEON_SYNTH name and the GAIN fader's groove and dB marks.

    All of it is drawn once into the editor's cached backdrop (crt::CachedPainter) and
    afterwards only copied, so the gradients and the extra glow passes cost nothing per
    frame. Nothing here is drawn on the audio thread or per timer tick.
*/
namespace neon::crt::frame
{
    /** The hue lifted towards white, as phosphor of that colour looks lit: what titles and
        labels are written in, so they stay clearly readable on the near-black glass. */
    inline juce::Colour lit (juce::Colour hue)
    {
        return hue.interpolatedWith (juce::Colours::white, 0.45f);
    }

    /** The tube glass: the window less the bezel. */
    inline juce::Rectangle<float> glassArea (juce::Rectangle<float> window)
    {
        return window.reduced ((float) bezelWidth);
    }

    // The glass's corner radius. Small, because the panels come within a few units of the
    // glass's edge at the bottom corners and must not be cut off.
    constexpr float glassCorner = 8.0f;

    /** The outline of the glass, for clipping the panels' glow to it. */
    inline juce::Path glassShape (juce::Rectangle<float> window)
    {
        juce::Path p;
        p.addRoundedRectangle (glassArea (window), glassCorner);
        return p;
    }

    //==========================================================================
    /** The casing and bezel round the edge, then the glass: a radial gradient, brightest a
        little above the middle, and a soft darkening towards the edges (the tube's curve). */
    inline void drawBackdrop (juce::Graphics& g, juce::Rectangle<float> window)
    {
        // Casing and bezel: a dark rim at the window's edge, the warm grey plastic lit
        // slightly from above, and a near-black lip where the plastic meets the glass.
        g.setColour (bezelShadow);
        g.fillRect (window);
        const auto plastic = window.reduced (1.5f);
        g.setGradientFill (juce::ColourGradient (bezel.brighter (0.12f), 0.0f, plastic.getY(),
                                                 casing.darker (0.15f), 0.0f, plastic.getBottom(), false));
        g.fillRoundedRectangle (plastic, 4.0f);
        // A thin highlight along the bezel's top edge, as on moulded plastic.
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillRect (plastic.getX() + 4.0f, plastic.getY(), plastic.getWidth() - 8.0f, 1.0f);

        const auto glass = glassArea (window);
        g.setColour (juce::Colour (0xff0b0a09));
        g.fillRoundedRectangle (glass.expanded (2.5f), glassCorner + 2.5f);

        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (glassShape (window));

        // The glass: an elliptical radial gradient (a circle squashed to the glass's aspect).
        const auto centre = juce::Point<float> (glass.getCentreX(), glass.getY() + glass.getHeight() * 0.45f);
        const float radius = glass.getWidth() * 0.62f;
        juce::ColourGradient tube (screenCentre, centre.x, centre.y, screenEdge, centre.x + radius, centre.y, true);
        tube.addColour (0.6, juce::Colour (0xff040c08));
        g.setFillType (juce::FillType (tube).transformed (juce::AffineTransform::scale (1.0f, glass.getHeight() / glass.getWidth() * 1.25f,
                                                                                     centre.x, centre.y)));
        g.fillRect (glass);

        // The edges fall into shadow, as the inside of a curved tube does.
        constexpr float depth = 36.0f;
        const auto shade = juce::Colours::black.withAlpha (0.55f);
        const auto clear = juce::Colours::transparentBlack;
        g.setGradientFill (juce::ColourGradient (shade, 0.0f, glass.getY(), clear, 0.0f, glass.getY() + depth, false));
        g.fillRect (glass.withHeight (depth));
        g.setGradientFill (juce::ColourGradient (shade, 0.0f, glass.getBottom(), clear, 0.0f, glass.getBottom() - depth, false));
        g.fillRect (glass.withTrimmedTop (glass.getHeight() - depth));
        g.setGradientFill (juce::ColourGradient (shade, glass.getX(), 0.0f, clear, glass.getX() + depth, 0.0f, false));
        g.fillRect (glass.withWidth (depth));
        g.setGradientFill (juce::ColourGradient (shade, glass.getRight(), 0.0f, clear, glass.getRight() - depth, 0.0f, false));
        g.fillRect (glass.withTrimmedLeft (glass.getWidth() - depth));
    }

    //==========================================================================
    /** A panel: a 1 px line in its hue with a faint glow outside and in, nearly square
        corners, the glass showing through. headerHeight > 0 adds a faint rule under the
        title row. Translucent strokes only, no blur. */
    inline void drawSection (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour hue, float headerHeight)
    {
        constexpr float corner = 2.0f;

        // Outer glow: wider and fainter further out.
        for (const auto& [grow, alpha] : { std::pair { 5.0f, 0.035f }, std::pair { 3.0f, 0.07f }, std::pair { 1.5f, 0.12f } })
        {
            g.setColour (hue.withAlpha (alpha));
            g.drawRoundedRectangle (r.expanded (grow), corner + grow, 2.0f);
        }

        // A breath of the hue on the glass inside, then the inner glow along the edge.
        g.setColour (hue.withAlpha (0.025f));
        g.fillRoundedRectangle (r, corner);
        for (const auto& [shrink, alpha] : { std::pair { 6.0f, 0.02f }, std::pair { 3.5f, 0.035f }, std::pair { 1.5f, 0.06f } })
        {
            g.setColour (hue.withAlpha (alpha));
            g.drawRoundedRectangle (r.reduced (shrink), juce::jmax (0.0f, corner - shrink), 2.5f);
        }

        if (headerHeight > 0.0f)
        {
            // A dotted rule under the title row, like a terminal's divider.
            g.setColour (hue.withAlpha (0.28f));
            const float y = r.getY() + headerHeight - 1.0f;
            for (float x = r.getX() + 12.0f; x < r.getRight() - 12.0f; x += 4.0f)
                g.fillRect (x, y, 2.0f, 1.0f);
        }

        g.setColour (hue.interpolatedWith (juce::Colours::white, 0.25f).withAlpha (0.62f));
        g.drawRoundedRectangle (r.reduced (0.5f), corner, 1.0f);
    }

    /** The header band has no box (as in the reference): a double phosphor rule under it. */
    inline void drawHeaderRule (juce::Graphics& g, juce::Rectangle<float> header)
    {
        const float y = header.getBottom() + 4.0f;
        g.setColour (cyan.withAlpha (0.10f));
        g.fillRect (header.getX(), y - 1.0f, header.getWidth(), 4.0f);
        g.setColour (cyan.withAlpha (0.45f));
        g.fillRect (header.getX(), y, header.getWidth(), 1.0f);
        g.setColour (cyan.withAlpha (0.22f));
        g.fillRect (header.getX(), y + 2.0f, header.getWidth(), 1.0f);
    }

    //==========================================================================
    /** Text with a soft wide halo in `halo` under drawGlowText's tight one. */
    inline void drawHaloText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                              juce::Colour colour, juce::Colour halo, const juce::Font& face,
                              juce::Justification justification, float strength = 1.0f)
    {
        g.setFont (face);
        g.setColour (halo.withAlpha (0.10f * strength));
        for (const auto& [dx, dy] : { std::pair { -2.0f, 0.0f }, std::pair { 2.0f, 0.0f }, std::pair { 0.0f, -2.0f }, std::pair { 0.0f, 2.0f },
                                      std::pair { -1.5f, -1.5f }, std::pair { 1.5f, 1.5f }, std::pair { -1.5f, 1.5f }, std::pair { 1.5f, -1.5f } })
            g.drawText (text, area.translated (dx, dy), justification, false);
        drawGlowText (g, text, area, colour, face, justification, 0.30f * strength);
    }

    /** A panel title, "> NAME" in VT323 with a glow in the panel's hue. The font starts at
        24 units and shrinks (not below 16) only if the title would not fit `area`. */
    inline void drawTitle (juce::Graphics& g, const juce::String& name, juce::Rectangle<float> area, juce::Colour hue)
    {
        const auto text = "> " + name;
        float height = 24.0f;
        auto titleFont = displayFont (height);
        while (height > 16.0f && juce::GlyphArrangement::getStringWidth (titleFont, text) > area.getWidth())
            titleFont = displayFont (height -= 1.0f);
        drawHaloText (g, text, area.withSizeKeepingCentre (area.getWidth(), juce::jmax (area.getHeight(), height + 4.0f)),
                      lit (hue), hue, titleFont, juce::Justification::centredLeft);
    }

    /** A static label in Plex Mono (SemiBold), in the lit hue with a faint glow. */
    inline void drawLabel (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Colour hue,
                           juce::Justification justification = juce::Justification::centredLeft)
    {
        drawGlowText (g, text, area, lit (hue), font (12.0f, true), justification, 0.22f);
    }

    /** A hint (what the strip shows, "pitch bend"): Plex Mono Regular in the dim phosphor. */
    inline void drawHint (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                          juce::Justification justification = juce::Justification::centredLeft)
    {
        g.setColour (phosphorDim);
        g.setFont (font (12.0f));
        g.drawText (text, area, justification, false);
    }

    /** NEON_SYNTH, big in VT323 with a phosphor halo. */
    inline void drawName (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const auto face = juce::Font (juce::FontOptions (vt323()).withHeight (40.0f).withKerningFactor (0.08f));
        drawHaloText (g, "NEON_SYNTH", area, juce::Colour (0xff8dffd0), phosphorGlow, face, juce::Justification::centredLeft, 1.4f);
    }

    //==========================================================================
    /** The GAIN fader's groove: a dark slot with a dashed centre line for the block thumb to ride. */
    inline void drawFaderGroove (juce::Graphics& g, juce::Rectangle<float> slot)
    {
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRect (slot);
        g.setColour (cyan.withAlpha (0.30f));
        g.drawRect (slot, 1.0f);
        g.setColour (cyan.withAlpha (0.45f));
        const float x = std::round (slot.getCentreX()) - 0.5f;
        for (float y = slot.getY() + 4.0f; y < slot.getBottom() - 4.0f; y += 6.0f)
            g.fillRect (x, y, 1.0f, juce::jmin (3.0f, slot.getBottom() - 4.0f - y));
    }

    /** One dB mark: a tick ending at tickRight (longer and brighter at 0 dB) and, unless
        `numberArea` is empty, its number. */
    inline void drawDbMark (juce::Graphics& g, float y, float tickRight, bool unity, juce::Rectangle<float> numberArea, const juce::String& number)
    {
        const float length = unity ? 8.0f : 5.0f;
        g.setColour (unity ? lit (cyan) : cyan.withAlpha (0.55f));
        g.fillRect (tickRight - length, y - 0.5f, length, 1.0f);
        if (numberArea.isEmpty())
            return;
        if (unity)
            drawGlowText (g, number, numberArea, lit (cyan), font (12.0f), juce::Justification::centredRight, 0.22f);
        else
        {
            g.setColour (phosphorDim);
            g.setFont (font (12.0f));
            g.drawText (number, numberArea, juce::Justification::centredRight, false);
        }
    }
}
