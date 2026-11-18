// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "CrtTheme.h"

/*
    The CRT look for every stock JUCE control in the editor (v1.10.0).

    Each control keeps the colour IDs the editor gives it (its panel's hue), so this
    class decides only HOW a control is drawn, never which colour it is: a knob drawn
    here uses rotarySliderFillColourId, a button its text colours, and so on.

    - Knobs: a glowing arc ring over a dark centre; the unlit part of the ring is the
      outline colour, faded further, and a short pointer marks the value even at zero.
      A knob whose range spans zero lights from the centre, so -x and +x read alike.
    - Linear sliders: a bar of phosphor blocks with a cap at the exact value. Only the
      drawing changes: the thumb radius (and so the travel, the drag mapping and the
      GAIN fader's dB marks) is LookAndFeel_V4's.
    - Buttons: bracketed text, "[SCOPE]"; a latched button is filled with its on colour
      and glows. The filter switch is a terminal tick box, "[■] ON".
    - Combo boxes: a thin square outline with a block arrow; the pop-up is a dark tube
      page whose highlighted row is inverted and whose current item has a "> " prompt.
    - Labels (readouts, combo text): a faint phosphor halo under the text.

    Glow is a few translucent passes, never a blur, and a disabled control loses it: the
    editor already dims those with setAlpha, so the colours themselves stay legible.

    Fonts: every control's text is IBM Plex Mono (labels) or VT323 (values), from CrtTheme.h.

    Set on the editor's content component, so every child inherits it; the editor resets
    it before the controls are destroyed.
*/
class CrtLookAndFeel : public juce::LookAndFeel_V4
{
public:
    CrtLookAndFeel()
    {
        using namespace neon::crt;
        setDefaultSansSerifTypeface (plexMono());

        // Pop-up menus and tooltips: phosphor on the dark tube; the highlighted row is
        // inverted (a lit block with dark text), like a terminal's selection.
        setColour (juce::PopupMenu::backgroundColourId,            screenCentre);
        setColour (juce::PopupMenu::textColourId,                  phosphor);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, phosphor.withAlpha (0.9f));
        setColour (juce::PopupMenu::highlightedTextColourId,       screenEdge);
        setColour (juce::TooltipWindow::backgroundColourId,        screenCentre);
        setColour (juce::TooltipWindow::textColourId,              phosphor);
        setColour (juce::TooltipWindow::outlineColourId,           phosphorDim);

        // Defaults only: a control that sets its own colour keeps it.
        setColour (juce::ComboBox::backgroundColourId, screenEdge.withAlpha (0.6f));
    }

    juce::Font getLabelFont (juce::Label& label) override
    {
        return neon::crt::labelFont (label.getFont().getHeight(), label.getFont().isBold());
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override       { return neon::crt::labelFont (13.0f, true); }
    juce::Font getPopupMenuFont() override                      { return neon::crt::labelFont (14.0f); }
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return neon::crt::labelFont (juce::jmin (14.0f, (float) buttonHeight * 0.55f), true);
    }

    //==========================================================================
    // Knobs

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        const auto area = juce::Rectangle<int> (x, y, width, height).toFloat();
        const float diameter = juce::jmin (area.getWidth(), area.getHeight()) - 6.0f;   // room for the glow
        if (diameter < 8.0f)
            return;

        const float ring   = juce::jmax (3.0f, diameter * 0.11f);
        const float radius = (diameter - ring) * 0.5f;
        const auto centre  = area.getCentre();
        const bool enabled = slider.isEnabled();

        const auto lit   = slider.findColour (juce::Slider::rotarySliderFillColourId);
        const auto unlit = slider.findColour (juce::Slider::rotarySliderOutlineColourId);
        const auto thumb = slider.findColour (juce::Slider::thumbColourId);

        // The dark centre: the tube glass seen through the ring.
        g.setColour (neon::crt::screenEdge.interpolatedWith (neon::crt::screenCentre, 0.5f));
        g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre));

        const auto arc = [&] (float from, float to)
        {
            juce::Path p;
            p.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, from, to, true);
            return p;
        };

        // The whole travel, unlit (the outline colour is the hue at 30%; this brings it to ~18%,
        // a little above the mock-up's 12% so a knob at zero still shows its ring over the glass).
        g.setColour (unlit.withMultipliedAlpha (0.6f));
        g.strokePath (arc (startAngle, endAngle), juce::PathStrokeType (ring, juce::PathStrokeType::curved, juce::PathStrokeType::butt));

        // The lit part: from the start, or from zero for a range that spans it.
        const float angle = startAngle + sliderPos * (endAngle - startAngle);
        float origin = startAngle;
        if (slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0)
            origin = startAngle + (float) slider.valueToProportionOfLength (0.0) * (endAngle - startAngle);

        if (std::abs (angle - origin) > 0.01f)
        {
            const auto litArc = arc (juce::jmin (origin, angle), juce::jmax (origin, angle));
            if (enabled)
                strokeGlow (g, litArc, lit, ring);
            g.setColour (lit);
            g.strokePath (litArc, juce::PathStrokeType (ring, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
        }

        // The pointer: a short line into the centre, so the value reads even at the start.
        const auto direction = juce::Point<float> (std::sin (angle), -std::cos (angle));
        const auto inner = centre + direction * (radius * 0.35f);
        const auto outer = centre + direction * (radius - ring * 0.5f - 1.0f);
        const juce::Line<float> pointer (inner, outer);
        if (enabled)
        {
            g.setColour (thumb.withMultipliedAlpha (0.25f));
            g.drawLine (pointer, 4.0f);
        }
        g.setColour (thumb);
        g.drawLine (pointer, 1.6f);
    }

    //==========================================================================
    // Linear sliders: phosphor block bars

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                           float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearVertical)
        {
            juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
            return;
        }

        const bool horizontal = style == juce::Slider::LinearHorizontal;
        const bool enabled = slider.isEnabled();
        const auto lit   = slider.findColour (juce::Slider::thumbColourId);
        const auto unlit = slider.findColour (juce::Slider::trackColourId);
        const auto well  = slider.findColour (juce::Slider::backgroundColourId);
        const auto travel = juce::Rectangle<int> (x, y, width, height).toFloat();

        // The bar: a strip across the middle of the travel, along it.
        const float thickness = horizontal ? juce::jlimit (6.0f, 12.0f, travel.getHeight() * 0.42f)
                                           : juce::jlimit (6.0f, 14.0f, travel.getWidth() * 0.4f);
        const auto bar = horizontal ? travel.withSizeKeepingCentre (travel.getWidth(), thickness)
                                    : travel.withSizeKeepingCentre (thickness, travel.getHeight());

        g.setColour (well.withMultipliedAlpha (0.55f));
        g.fillRect (bar.expanded (2.0f));

        // Blocks about 6 units long with a gap between, a whole number of them along the bar.
        const float length = horizontal ? bar.getWidth() : bar.getHeight();
        const int count = juce::jmax (1, juce::roundToInt (length / 6.0f));
        const float pitch = length / (float) count;
        const float gap = juce::jmin (1.5f, pitch * 0.3f);

        float litStart = 0.0f, litEnd = 0.0f;   // along the bar, for the glow behind the lit blocks
        for (int i = 0; i < count; ++i)
        {
            const float a = (float) i * pitch;
            const auto block = horizontal ? juce::Rectangle<float> (bar.getX() + a + gap * 0.5f, bar.getY(), pitch - gap, bar.getHeight())
                                          : juce::Rectangle<float> (bar.getX(), bar.getBottom() - a - pitch + gap * 0.5f, bar.getWidth(), pitch - gap);
            // Lit when the value has passed the block's middle (from the left, or from the bottom).
            const bool on = horizontal ? block.getCentreX() < sliderPos : block.getCentreY() > sliderPos;
            g.setColour (on ? lit : unlit.withMultipliedAlpha (0.5f));
            g.fillRect (block);
            if (on)
                litEnd = a + pitch;
        }

        if (enabled && litEnd > litStart)
        {
            const auto glow = horizontal ? bar.withWidth (litEnd) : bar.withTop (bar.getBottom() - litEnd);
            g.setColour (lit.withMultipliedAlpha (0.14f));
            g.fillRect (glow.expanded (2.0f));
        }

        // The cap at the exact value, with a dark line through it for precision.
        const auto cap = horizontal ? juce::Rectangle<float> (5.0f, thickness + 8.0f).withCentre ({ sliderPos, bar.getCentreY() })
                                    : juce::Rectangle<float> (juce::jmin (26.0f, travel.getWidth() - 4.0f), 8.0f).withCentre ({ bar.getCentreX(), sliderPos });
        if (enabled)
        {
            g.setColour (lit.withMultipliedAlpha (0.22f));
            g.fillRect (cap.expanded (2.5f));
            g.setColour (lit.withMultipliedAlpha (0.35f));
            g.fillRect (cap.expanded (1.0f));
        }
        g.setColour (lit.interpolatedWith (neon::crt::phosphor, 0.35f));
        g.fillRect (cap);
        g.setColour (neon::crt::screenEdge);
        if (horizontal)
            g.fillRect (juce::Rectangle<float> (1.0f, cap.getHeight() - 4.0f).withCentre (cap.getCentre()));
        else
            g.fillRect (juce::Rectangle<float> (cap.getWidth() - 6.0f, 1.0f).withCentre (cap.getCentre()));
    }

    //==========================================================================
    // Buttons: bracketed text, lit when latched on

    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                               bool highlighted, bool down) override
    {
        const auto area = button.getLocalBounds().toFloat().reduced (0.5f);
        const auto outline = button.findColour (juce::ComboBox::outlineColourId);
        const bool on = button.getToggleState();

        if (on)
        {
            g.setColour (backgroundColour);
            g.fillRect (area);
            if (button.isEnabled())
            {
                g.setColour (outline.withMultipliedAlpha (0.25f));
                g.drawRect (area.expanded (1.0f), 1.0f);
            }
            g.setColour (outline);
            g.drawRect (area, 1.0f);
        }
        else if (down || highlighted)
        {
            // No box at rest (the brackets are the button); a faint one under the mouse.
            g.setColour (outline.withMultipliedAlpha (down ? 0.25f : 0.1f));
            g.fillRect (area);
            g.setColour (outline.withMultipliedAlpha (0.6f));
            g.drawRect (area, 1.0f);
        }
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool highlighted, bool /*down*/) override
    {
        const bool on = button.getToggleState();
        const bool enabled = button.isEnabled();
        const auto colour = button.findColour (on ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                                  .withMultipliedAlpha (enabled ? 1.0f : 0.5f);
        const auto text = "[" + button.getButtonText() + "]";
        const auto area = button.getLocalBounds().toFloat().reduced (2.0f, 0.0f);
        const auto font = fitted (getTextButtonFont (button, button.getHeight()), text, area.getWidth());

        drawGlowing (g, text, area, colour, font, juce::Justification::centred,
                     ! enabled ? 0.0f : on ? 0.55f : highlighted ? 0.3f : 0.0f);
    }

    /** The filter switch: a terminal tick box, "[■] ON" lit, "[ ] ON" dim. */
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down) override
    {
        const bool on = button.getToggleState();
        const bool enabled = button.isEnabled();
        const auto textColour = button.findColour (juce::ToggleButton::textColourId);
        const auto tick = button.findColour (enabled ? juce::ToggleButton::tickColourId : juce::ToggleButton::tickDisabledColourId);

        if (down || highlighted)
        {
            g.setColour (tick.withMultipliedAlpha (down ? 0.2f : 0.08f));
            g.fillRect (button.getLocalBounds().toFloat().reduced (0.5f));
        }

        const auto area = button.getLocalBounds().toFloat().reduced (2.0f, 0.0f);
        const auto text = "[ ] " + button.getButtonText();
        const auto font = fitted (neon::crt::labelFont (juce::jmin (14.0f, (float) button.getHeight() * 0.55f), true),
                                  text, area.getWidth());
        const auto colour = on ? textColour : textColour.withMultipliedAlpha (0.55f);

        drawGlowing (g, text, area, colour.withMultipliedAlpha (enabled ? 1.0f : 0.5f), font, juce::Justification::centred,
                     enabled && on ? 0.55f : enabled && highlighted ? 0.3f : 0.0f);

        if (on)
        {
            // The block sits in the second character cell (the font is monospaced).
            const float textWidth = juce::GlyphArrangement::getStringWidth (font, text);
            const float cell = juce::GlyphArrangement::getStringWidth (font, "[");
            const float left = area.getCentreX() - textWidth * 0.5f + cell;
            const auto block = juce::Rectangle<float> (cell * 0.8f, font.getAscent() * 0.7f)
                                   .withCentre ({ left + cell * 0.5f, area.getCentreY() });
            if (enabled)
            {
                g.setColour (tick.withMultipliedAlpha (0.3f));
                g.fillRect (block.expanded (1.5f));
            }
            g.setColour (tick);
            g.fillRect (block);
        }
    }

    //==========================================================================
    // Combo boxes and their pop-up menus

    void drawComboBox (juce::Graphics& g, int width, int height, bool down,
                       int, int, int, int, juce::ComboBox& box) override
    {
        const auto area = juce::Rectangle<int> (width, height).toFloat().reduced (0.5f);
        const auto outline = box.findColour (juce::ComboBox::outlineColourId);
        const bool enabled = box.isEnabled();
        const bool hover = box.isMouseOver (true);

        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRect (area);
        if (down)
        {
            g.setColour (outline.withMultipliedAlpha (0.15f));
            g.fillRect (area);
        }

        if (enabled)
        {
            g.setColour (outline.withMultipliedAlpha (hover ? 0.3f : 0.15f));
            g.drawRect (area.expanded (1.0f), 1.0f);
        }
        g.setColour (outline.withMultipliedAlpha (hover || down ? 0.9f : 0.6f));
        g.drawRect (area, 1.0f);

        // A solid block arrow pointing down.
        const auto zone = arrowZone (box);
        const float w = juce::jmin (9.0f, (float) zone.getWidth() * 0.5f);
        juce::Path arrow;
        arrow.addTriangle ((float) zone.getCentreX() - w * 0.5f, (float) zone.getCentreY() - w * 0.3f,
                           (float) zone.getCentreX() + w * 0.5f, (float) zone.getCentreY() - w * 0.3f,
                           (float) zone.getCentreX(),            (float) zone.getCentreY() + w * 0.35f);
        const auto arrowColour = box.findColour (juce::ComboBox::arrowColourId).withMultipliedAlpha (enabled ? 1.0f : 0.4f);
        if (enabled)
        {
            g.setColour (arrowColour.withMultipliedAlpha (0.3f));
            g.strokePath (arrow, juce::PathStrokeType (2.0f));
        }
        g.setColour (arrowColour);
        g.fillPath (arrow);
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (4, 1, juce::jmax (0, arrowZone (box).getX() - 4), box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
    {
        g.fillAll (findColour (juce::PopupMenu::backgroundColourId));

        // The pop-up is its own window, outside the editor's scanline overlay, so it carries its own.
        g.setColour (juce::Colours::black.withAlpha (neon::crt::scanlineStrength));
        for (int y = neon::crt::scanlinePitch - 1; y < height; y += neon::crt::scanlinePitch)
            g.fillRect (0, y, width, 1);

        g.setColour (neon::crt::phosphorDim);
        g.drawRect (0, 0, width, height);
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColourToUse) override
    {
        if (isSeparator || icon != nullptr || hasSubMenu || shortcutKeyText.isNotEmpty())
        {
            // Not used by the editor's menus; the stock drawing (in our colours) covers them.
            juce::LookAndFeel_V4::drawPopupMenuItem (g, area, isSeparator, isActive, isHighlighted, isTicked, hasSubMenu,
                                                     text, shortcutKeyText, icon, textColourToUse);
            return;
        }

        auto r = area.reduced (1);
        auto colour = textColourToUse != nullptr ? *textColourToUse : findColour (juce::PopupMenu::textColourId);
        const bool lit = isHighlighted && isActive;

        if (lit)
        {
            g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
            g.fillRect (r);
            colour = findColour (juce::PopupMenu::highlightedTextColourId);
        }
        else if (! isActive)
        {
            colour = colour.withMultipliedAlpha (0.5f);
        }

        auto font = getPopupMenuFont();
        font.setHeight (juce::jmin (font.getHeight(), (float) r.getHeight() / 1.3f));
        r.removeFromLeft (6);
        r.removeFromRight (4);

        // The current choice carries the terminal's prompt; the others line up after it.
        const auto prompt = r.removeFromLeft (juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, "> ")));
        if (isTicked)
            drawGlowing (g, ">", prompt.toFloat(), colour, font, juce::Justification::centredLeft, lit ? 0.0f : 0.4f);

        g.setColour (colour);
        g.setFont (font);
        g.drawFittedText (text, r, juce::Justification::centredLeft, 1);
    }

    //==========================================================================
    // Labels: readouts and combo text, with a faint halo

    void drawLabel (juce::Graphics& g, juce::Label& label) override
    {
        if (label.isBeingEdited())
        {
            juce::LookAndFeel_V4::drawLabel (g, label);
            return;
        }

        g.fillAll (label.findColour (juce::Label::backgroundColourId));

        const bool enabled = label.isEnabled();
        const auto font = getLabelFont (label);
        const auto area = getLabelBorderSize (label).subtractedFrom (label.getLocalBounds());
        const auto colour = label.findColour (juce::Label::textColourId).withMultipliedAlpha (enabled ? 1.0f : 0.5f);
        const int lines = juce::jmax (1, (int) ((float) area.getHeight() / font.getHeight()));
        const float minScale = label.getMinimumHorizontalScale();

        g.setFont (font);
        if (enabled)
        {
            g.setColour (colour.withMultipliedAlpha (0.14f));
            for (const auto& [dx, dy] : { std::pair { -1, 0 }, std::pair { 1, 0 }, std::pair { 0, -1 }, std::pair { 0, 1 } })
                g.drawFittedText (label.getText(), area.translated (dx, dy), label.getJustificationType(), lines, minScale);
        }
        g.setColour (colour);
        g.drawFittedText (label.getText(), area, label.getJustificationType(), lines, minScale);

        const auto outline = label.findColour (juce::Label::outlineColourId);
        if (! outline.isTransparent())
        {
            g.setColour (outline.withMultipliedAlpha (enabled ? 1.0f : 0.5f));
            g.drawRect (label.getLocalBounds());
        }
    }

private:
    /** Where a combo box's arrow sits: a square-ish zone at its right. */
    static juce::Rectangle<int> arrowZone (const juce::ComboBox& box)
    {
        const int w = juce::jlimit (12, 22, juce::roundToInt ((float) box.getHeight() * 0.8f));
        return { box.getWidth() - w - 2, 0, w, box.getHeight() };
    }

    /** The font, made smaller if needed so `text` fits in `width` (it never grows). */
    static juce::Font fitted (juce::Font font, const juce::String& text, float width)
    {
        const float natural = juce::GlyphArrangement::getStringWidth (font, text);
        if (natural > width && width > 0.0f)
            font.setHeight (juce::jmax (7.0f, font.getHeight() * width / natural));
        return font;
    }

    /** One line of text with a phosphor halo of strength `glow` (0 for none), never clipped with dots. */
    static void drawGlowing (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Colour colour,
                             const juce::Font& font, juce::Justification justification, float glow)
    {
        g.setFont (font);
        if (glow > 0.0f)
        {
            g.setColour (colour.withMultipliedAlpha (glow * 0.45f));
            for (const auto& [dx, dy] : { std::pair { -1.0f, 0.0f }, std::pair { 1.0f, 0.0f }, std::pair { 0.0f, -1.0f }, std::pair { 0.0f, 1.0f } })
                g.drawText (text, area.translated (dx, dy), justification, false);
            g.setColour (colour.withMultipliedAlpha (glow * 0.2f));
            for (const auto& [dx, dy] : { std::pair { -2.0f, 0.0f }, std::pair { 2.0f, 0.0f } })
                g.drawText (text, area.translated (dx, dy), justification, false);
        }
        g.setColour (colour);
        g.drawText (text, area, justification, false);
    }

    /** A lit stroke's halo: two wider, fainter passes under it. */
    static void strokeGlow (juce::Graphics& g, const juce::Path& path, juce::Colour colour, float width)
    {
        const auto stroke = [&] (float w, float alpha)
        {
            g.setColour (colour.withMultipliedAlpha (alpha));
            g.strokePath (path, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
        };
        stroke (width + 5.0f, 0.10f);
        stroke (width + 2.0f, 0.22f);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrtLookAndFeel)
};
