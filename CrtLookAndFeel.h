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

        // Pop-up menus and tooltips: phosphor on the dark tube.
        setColour (juce::PopupMenu::backgroundColourId,            screenCentre);
        setColour (juce::PopupMenu::textColourId,                  phosphor);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, phosphorGlow.withAlpha (0.25f));
        setColour (juce::PopupMenu::highlightedTextColourId,       phosphor);
        setColour (juce::TooltipWindow::backgroundColourId,        screenCentre);
        setColour (juce::TooltipWindow::textColourId,              phosphor);
        setColour (juce::TooltipWindow::outlineColourId,           phosphorDim);
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

    // Agent B: drawRotarySlider, drawLinearSlider, drawComboBox, drawButtonBackground,
    // drawButtonText, drawToggleButton, drawPopupMenuBackground, drawPopupMenuItem.

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrtLookAndFeel)
};
