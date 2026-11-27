// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "gui/CrtTheme.h"

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
    CrtLookAndFeel();

    juce::Font getLabelFont (juce::Label& label) override;

    juce::Font getComboBoxFont (juce::ComboBox&) override       { return neon::crt::labelFont (13.0f, true); }
    juce::Font getPopupMenuFont() override                      { return neon::crt::labelFont (14.0f); }
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    //==========================================================================
    // Knobs

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                           float startAngle, float endAngle, juce::Slider& slider) override;

    //==========================================================================
    // Linear sliders: phosphor block bars

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                           float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style, juce::Slider& slider) override;

    //==========================================================================
    // Buttons: bracketed text, lit when latched on

    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                               bool highlighted, bool down) override;

    void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool highlighted, bool /*down*/) override;

    /** The filter switch: a terminal tick box, "[■] ON" lit, "[ ] ON" dim. */
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down) override;

    //==========================================================================
    // Combo boxes and their pop-up menus

    void drawComboBox (juce::Graphics& g, int width, int height, bool down,
                       int, int, int, int, juce::ComboBox& box) override;

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override;

    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override;

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColourToUse) override;

    //==========================================================================
    // Labels: readouts and combo text, with a faint halo

    void drawLabel (juce::Graphics& g, juce::Label& label) override;

private:
    // How strongly a button's text glows: switched on, and under the mouse (text and toggle buttons alike).
    static constexpr float onGlow    = 0.55f;
    static constexpr float hoverGlow = 0.3f;

    /** Where a combo box's arrow sits: a square-ish zone at its right. */
    static juce::Rectangle<int> arrowZone (const juce::ComboBox& box);

    /** The font, made smaller if needed so `text` fits in `width` (it never grows). */
    static juce::Font fitted (juce::Font font, const juce::String& text, float width);

    /** One line of text with a phosphor halo of strength `glow` (0 for none), never clipped with dots. */
    static void drawGlowing (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Colour colour,
                             const juce::Font& font, juce::Justification justification, float glow);

    /** A lit stroke's halo: two wider, fainter passes under it. */
    static void strokeGlow (juce::Graphics& g, const juce::Path& path, juce::Colour colour, float width);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrtLookAndFeel)
};
