// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "state/PresetManager.h"
#include "gui/NeonStyle.h"
#include "gui/CrtTheme.h"

/**
    The preset bar in the header (v1.11.0): [<] NAME  CATEGORY [>] [SAVE].

    Clicking the name opens the menu: the factory bank by category, then the user's presets
    (Documents/NeonSynth/Presets), loading a preset file from anywhere, and showing the folder.
    < and > step through the factory presets and then the user's, wrapping around. SAVE asks for
    a name and a category and writes the current sound into the user folder.

    One self-contained component, so a later page layout can move it whole. It talks only to the
    PresetManager, and follows it (a session restored by the host included) through its change
    messages. Its children carry no component IDs on purpose: the bar is one control ("presetBar"),
    and EditorTests checks that no two named controls overlap, parents and children included.
    Children, in order: <, >, SAVE, the name.
*/
class PresetBar : public juce::Component,
                  private juce::ChangeListener
{
public:
    explicit PresetBar (PresetManager& manager);

    ~PresetBar() override;

    void resized() override;

    /** What the name box shows now (for the tests). */
    juce::String getShownName() const       { return nameButton.name; }
    juce::String getShownCategory() const   { return nameButton.category; }

    /** Builds the preset menu (the tests read it; a click shows it). Item IDs: factory index + 1,
        user preset k at userItemBase + k, then the two commands. */
    juce::PopupMenu createMenu();

    /** What choosing a menu item does (also for the tests). */
    void menuItemChosen (int id);

private:
    /** The name box: the preset's name, its category on the right, a small arrow for the menu. */
    struct NameButton : juce::Button
    {
        NameButton() : juce::Button ("preset") { setTooltip ("Presets: click for the factory bank and your own"); }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            const auto area = getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (neon::displayInset);
            g.fillRect (area);
            g.setColour (neon::cyan.withAlpha (down ? 0.25f : highlighted ? 0.12f : 0.0f));
            g.fillRect (area);
            g.setColour (neon::cyan.withAlpha (highlighted ? 0.8f : 0.5f));
            g.drawRect (area, 1.0f);

            auto textArea = area.reduced (8.0f, 0.0f);
            const auto arrow = textArea.removeFromRight (10.0f);
            juce::Path p;
            p.addTriangle (arrow.getX(), arrow.getCentreY() - 2.5f, arrow.getRight(), arrow.getCentreY() - 2.5f,
                           arrow.getCentreX(), arrow.getCentreY() + 3.0f);
            g.setColour (neon::cyan.withAlpha (0.7f));
            g.fillPath (p);
            textArea.removeFromRight (4.0f);

            if (category.isNotEmpty())
            {
                const auto font = neon::crt::labelFont (10.0f, true);
                const auto width = juce::GlyphArrangement::getStringWidth (font, category.toUpperCase()) + 2.0f;
                neon::crt::drawGlowText (g, category.toUpperCase(), textArea.removeFromRight (width),
                                         neon::crt::phosphorDim, font, juce::Justification::centredRight, 0.0f);
                textArea.removeFromRight (6.0f);
            }
            neon::crt::drawGlowText (g, name.isNotEmpty() ? name.toUpperCase() : juce::String ("DEFAULT"), textArea,
                                     name.isNotEmpty() ? neon::cyan : neon::cyan.withAlpha (0.55f),
                                     neon::crt::displayFont (20.0f), juce::Justification::centredLeft, 0.35f);
        }

        juce::String name, category;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override   { refresh(); }

    void refresh();

    void showMenu();

    void chooseFile();

    /** SAVE: a small window asking for the name (the current one to start with) and the category. */
    void askToSave();

    void saveAs (const juce::String& name, const juce::String& category);

    void showProblem (const juce::String& error);

    static constexpr int userItemBase   = 1000;
    static constexpr int maxUserItems   = 2000;
    static constexpr int loadFileItem   = 9001;
    static constexpr int showFolderItem = 9002;

    PresetManager& presets;
    juce::TextButton prevButton { "<" }, nextButton { ">" }, saveButton { "SAVE" };
    NameButton nameButton;
    juce::Array<juce::File> userFiles;   // as listed in the menu last shown
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBar)
};
