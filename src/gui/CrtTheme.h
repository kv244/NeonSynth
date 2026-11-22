// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "NeonFonts.h"

/*
    The retro CRT theme (v1.10.0): the one place the look's shared pieces live.

    Palette: phosphor white-green text on a near-black green tube, inside a warm grey
    bezel. The four panel hues (cyan, magenta, amber, green) stay as they were; they live
    in NeonStyle.h (namespace neon) with the rest of the v1.9 palette.

    Fonts: VT323, a bitmap terminal face, for titles and values at 16 px and up; IBM Plex
    Mono (Regular and SemiBold) for everything smaller, where VT323 stops being legible.
    Both are under the SIL Open Font Licence (fonts/OFL-*.txt) and are compiled into the
    plugin as binary data, so the window looks the same on every machine.

    CachedPainter: draws something that rarely changes once into an image at the
    screen's real pixel scale, then just blits it. The backdrop, the panels and the
    scanlines are drawn this way, so the extra texture costs nothing per frame.

    Everything here runs on the message thread.
*/
namespace neon::crt
{
    // ── Palette ─────────────────────────────────────────────────────────────
    inline const juce::Colour phosphor     { 0xffc8ffe6 };   // text
    inline const juce::Colour phosphorDim  { 0xff6fae95 };   // secondary text, units, hints
    inline const juce::Colour phosphorGlow { 0xff3dffb0 };   // the halo behind lit text
    inline const juce::Colour screenCentre { 0xff07170f };   // the tube glass, brightest in the middle...
    inline const juce::Colour screenEdge   { 0xff010402 };   // ...falling to almost black at the corners
    inline const juce::Colour bezel        { 0xff35302a };   // the plastic around the glass
    inline const juce::Colour bezelShadow  { 0xff0f0d0b };
    inline const juce::Colour casing       { 0xff2a2723 };

    constexpr int   bezelWidth       = 12;      // design units, inside the window (decision 6)
    constexpr float scanlineStrength = 0.12f;   // darkness of each scanline, 0..1 (decision 4)
    constexpr int   scanlinePitch    = 3;       // design units from one scanline to the next
    constexpr float afterglowSeconds = 0.15f;   // the scope and spectrum's phosphor decay (decision 5)

    /** View setting (saved with the session, not a parameter, not automatable): scanlines on or off. */
    inline const juce::Identifier scanlinesPropertyId { "scanlines" };

    // ── Fonts ───────────────────────────────────────────────────────────────
    inline juce::Typeface::Ptr vt323()
    {
        static auto face = juce::Typeface::createSystemTypefaceFor (NeonFonts::VT323Regular_ttf, NeonFonts::VT323Regular_ttfSize);
        return face;
    }

    inline juce::Typeface::Ptr plexMono (bool semiBold = false)
    {
        static auto regular = juce::Typeface::createSystemTypefaceFor (NeonFonts::IBMPlexMonoRegular_ttf, NeonFonts::IBMPlexMonoRegular_ttfSize);
        static auto bold    = juce::Typeface::createSystemTypefaceFor (NeonFonts::IBMPlexMonoSemiBold_ttf, NeonFonts::IBMPlexMonoSemiBold_ttfSize);
        return semiBold ? bold : regular;
    }

    /** VT323: titles and values, 16 px and up. */
    inline juce::Font displayFont (float height)
    {
        return juce::Font (juce::FontOptions (vt323()).withHeight (height));
    }

    /** IBM Plex Mono: labels and small text. */
    inline juce::Font labelFont (float height, bool semiBold = false)
    {
        return juce::Font (juce::FontOptions (plexMono (semiBold)).withHeight (height));
    }

    /** Text with a phosphor halo: one faint wide pass under the text itself. */
    inline void drawGlowText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                              juce::Colour colour, const juce::Font& font, juce::Justification justification,
                              float glow = 0.35f)
    {
        g.setFont (font);
        if (glow > 0.0f)
        {
            g.setColour (colour.withMultipliedAlpha (glow * 0.5f));
            for (const auto& [dx, dy] : { std::pair { -1.0f, 0.0f }, std::pair { 1.0f, 0.0f }, std::pair { 0.0f, -1.0f }, std::pair { 0.0f, 1.0f } })
                g.drawText (text, area.translated (dx, dy), justification, false);
        }
        g.setColour (colour);
        g.drawText (text, area, justification, false);
    }

    //==========================================================================
    /**
        Draws something that rarely changes once, into an image at the screen's real
        pixel scale (the editor's zoom times the display's), and afterwards only copies
        that image. invalidate() when what it shows changes; a change of scale redraws
        it by itself.
    */
    class CachedPainter
    {
    public:
        /** Paints `area` (design units) from the cache, calling `draw` first if the cache is stale.
            `draw` paints in the same design units, with (0, 0) at the area's top-left corner. */
        template <typename Draw>
        void paint (juce::Graphics& g, juce::Rectangle<int> area, Draw&& draw)
        {
            const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
            if (! image.isValid() || ! juce::approximatelyEqual (scale, imageScale) || area.getWidth() != size.x || area.getHeight() != size.y)
            {
                imageScale = scale;
                size = { area.getWidth(), area.getHeight() };
                image = juce::Image (juce::Image::ARGB, juce::jmax (1, juce::roundToInt ((float) size.x * scale)),
                                     juce::jmax (1, juce::roundToInt ((float) size.y * scale)), true);
                juce::Graphics ig (image);
                ig.addTransform (juce::AffineTransform::scale (scale));
                draw (ig);
                ++redraws;
            }
            g.drawImageTransformed (image, juce::AffineTransform::scale (1.0f / imageScale)
                                               .translated ((float) area.getX(), (float) area.getY()));
        }

        void invalidate() noexcept { image = {}; }

        /** How many times the image has been drawn (EditorTests checks the cache is reused). */
        int getRedrawCount() const noexcept { return redraws; }

    private:
        juce::Image image;
        float imageScale = 0.0f;
        juce::Point<int> size;
        int redraws = 0;
    };

    //==========================================================================
    /**
        The scanlines: a component laid over the whole window that draws faint dark
        lines every scanlinePitch units across the glass (not the bezel), and a faint
        reflection high on the left, from a cached image, and lets every click
        through to the controls underneath. Its visibility is the SCANLINES switch.
    */
    class ScanlineOverlay : public juce::Component
    {
    public:
        ScanlineOverlay()
        {
            setInterceptsMouseClicks (false, false);
            setOpaque (false);
        }

        void paint (juce::Graphics& g) override
        {
            cache.paint (g, getLocalBounds(), [this] (juce::Graphics& ig)
            {
                // Only the glass has scanlines; the bezel round it is plastic.
                const auto glass = getLocalBounds().reduced (bezelWidth);
                ig.reduceClipRegion (glass);
                ig.setColour (juce::Colours::black.withAlpha (scanlineStrength));
                for (int y = glass.getY() + scanlinePitch - 1; y < glass.getBottom(); y += scanlinePitch)
                    ig.fillRect (glass.getX(), y, glass.getWidth(), 1);

                // A faint reflection on the glass, high on the left, as a room light would
                // make; faint enough that nothing under it gets harder to read.
                const auto glassF = glass.toFloat();
                const juce::Point<float> spot (glassF.getX() + glassF.getWidth() * 0.3f, glassF.getY() + glassF.getHeight() * 0.2f);
                const float reach = glassF.getWidth() * 0.4f;
                juce::ColourGradient sheen (juce::Colour (0xffb4ffdc).withAlpha (0.045f), spot.x, spot.y,
                                            juce::Colours::transparentBlack, spot.x + reach, spot.y, true);
                ig.setFillType (juce::FillType (sheen).transformed (juce::AffineTransform::scale (1.0f, 0.6f, spot.x, spot.y)));
                ig.fillRect (glassF);
            });
        }

        void resized() override { cache.invalidate(); }

    private:
        CachedPainter cache;
    };
}
