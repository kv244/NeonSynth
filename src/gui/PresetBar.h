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
    messages. Child component IDs, for the tests: presetPrev, presetName, presetNext, presetSave.
*/
class PresetBar : public juce::Component,
                  private juce::ChangeListener
{
public:
    explicit PresetBar (PresetManager& manager)
        : presets (manager)
    {
        for (auto* button : { &prevButton, &nextButton, &saveButton })
        {
            button->setColour (juce::TextButton::buttonColourId,  juce::Colours::transparentBlack);
            button->setColour (juce::TextButton::textColourOffId, neon::cyan);
            button->setColour (juce::ComboBox::outlineColourId,   neon::cyan.withAlpha (0.6f));
            addAndMakeVisible (button);
        }
        prevButton.setTooltip ("Previous preset");
        nextButton.setTooltip ("Next preset");
        saveButton.setTooltip ("Save the current sound as a preset in Documents/NeonSynth/Presets");
        prevButton.onClick = [this] { presets.step (-1); refresh(); };
        nextButton.onClick = [this] { presets.step (+1); refresh(); };
        saveButton.onClick = [this] { askToSave(); };

        nameButton.onClick = [this] { showMenu(); };
        addAndMakeVisible (nameButton);

        prevButton.setComponentID ("presetPrev");
        nameButton.setComponentID ("presetName");
        nextButton.setComponentID ("presetNext");
        saveButton.setComponentID ("presetSave");

        presets.addChangeListener (this);
        refresh();
    }

    ~PresetBar() override
    {
        presets.removeChangeListener (this);
    }

    void resized() override
    {
        // 308 x 32 in the header: < (30), the name (166), > (30), SAVE (70), 4 px between.
        auto r = getLocalBounds();
        prevButton.setBounds (r.removeFromLeft (30));
        r.removeFromLeft (4);
        saveButton.setBounds (r.removeFromRight (70));
        r.removeFromRight (4);
        nextButton.setBounds (r.removeFromRight (30));
        r.removeFromRight (4);
        nameButton.setBounds (r);
    }

    /** What the name box shows now (for the tests). */
    juce::String getShownName() const       { return nameButton.name; }
    juce::String getShownCategory() const   { return nameButton.category; }

    /** Builds the preset menu (the tests read it; a click shows it). Item IDs: factory index + 1,
        user preset k at userItemBase + k, then the two commands. */
    juce::PopupMenu createMenu()
    {
        juce::PopupMenu menu;
        const int current = presets.getCurrentProgramIndex();
        menu.addSectionHeader ("FACTORY");
        for (const auto& category : PresetManager::categories())
        {
            juce::PopupMenu sub;
            for (int i = 0; i < PresetManager::numFactoryPresets; ++i)
                if (PresetManager::getFactoryCategory (i) == category)
                    sub.addItem (i + 1, PresetManager::getFactoryName (i), true, i == current);
            if (sub.getNumItems() > 0)
                menu.addSubMenu (category, sub);
        }

        menu.addSectionHeader ("USER");
        userFiles = presets.getUserPresets();
        const auto currentFile = presets.getCurrentFile();
        for (int k = 0; k < userFiles.size() && k < maxUserItems; ++k)
            menu.addItem (userItemBase + k, userFiles[k].getFileNameWithoutExtension(), true, userFiles[k] == currentFile);
        if (userFiles.isEmpty())
            menu.addItem (-1, "(none yet: SAVE adds one)", false);

        menu.addSeparator();
        menu.addItem (loadFileItem, "Load a preset file...");
        menu.addItem (showFolderItem, "Show the preset folder", presets.getUserFolder().isDirectory());
        return menu;
    }

    /** What choosing a menu item does (also for the tests). */
    void menuItemChosen (int id)
    {
        juce::String error;
        if (id >= 1 && id <= PresetManager::numFactoryPresets)
            presets.loadFactory (id - 1);
        else if (id >= userItemBase && id < userItemBase + userFiles.size())
        {
            if (! presets.loadFile (userFiles[id - userItemBase], error))
                showProblem (error);
        }
        else if (id == loadFileItem)
            chooseFile();
        else if (id == showFolderItem)
            presets.getUserFolder().revealToUser();
        refresh();
    }

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

    void refresh()
    {
        const auto name = presets.getCurrentName(), category = presets.getCurrentCategory();
        if (name != nameButton.name || category != nameButton.category)
        {
            nameButton.name = name;
            nameButton.category = category;
            nameButton.repaint();
        }
        const int program = presets.getCurrentProgramIndex();
        nameButton.setTooltip (program >= 0 ? PresetManager::getFactoryDescription (program)
                                            : juce::String ("Presets: click for the factory bank and your own"));
    }

    void showMenu()
    {
        createMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&nameButton),
                                    [safe = juce::Component::SafePointer<PresetBar> (this)] (int id)
                                    {
                                        if (safe != nullptr && id != 0)
                                            safe->menuItemChosen (id);
                                    });
    }

    void chooseFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Load a NeonSynth preset", presets.getUserFolder(),
                                                       juce::String ("*") + PresetManager::fileExtension);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe = juce::Component::SafePointer<PresetBar> (this)] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr || fc.getResult() == juce::File())
                                      return;
                                  juce::String error;
                                  if (! safe->presets.loadFile (fc.getResult(), error))
                                      safe->showProblem (error);
                                  safe->refresh();
                              });
    }

    /** SAVE: a small window asking for the name (the current one to start with) and the category. */
    void askToSave()
    {
        auto* window = new juce::AlertWindow ("SAVE PRESET", "Saved in " + presets.getUserFolder().getFullPathName(),
                                              juce::MessageBoxIconType::NoIcon, this);
        window->setLookAndFeel (&getLookAndFeel());
        window->addTextEditor ("name", presets.getCurrentName().isNotEmpty() ? presets.getCurrentName() : juce::String ("My preset"), "Name");
        juce::StringArray choices { "(none)" };
        choices.addArray (PresetManager::categories());
        window->addComboBox ("category", choices, "Category");
        if (auto* box = window->getComboBoxComponent ("category"))
            box->setSelectedItemIndex (juce::jmax (0, choices.indexOf (presets.getCurrentCategory())), juce::dontSendNotification);
        window->addButton ("SAVE", 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        window->enterModalState (true, juce::ModalCallbackFunction::create (
            [safe = juce::Component::SafePointer<PresetBar> (this), window] (int result)
            {
                const auto name = window->getTextEditorContents ("name").trim();
                const auto* box = window->getComboBoxComponent ("category");
                const auto category = box != nullptr && box->getSelectedItemIndex() > 0 ? box->getText() : juce::String();
                if (safe != nullptr && result == 1 && name.isNotEmpty())
                    safe->saveAs (name, category);
            }), true);   // deletes the window when it closes
    }

    void saveAs (const juce::String& name, const juce::String& category)
    {
        const auto file = presets.userFileFor (name);
        const auto save = [safe = juce::Component::SafePointer<PresetBar> (this), file, category]
        {
            if (safe == nullptr)
                return;
            juce::String error;
            if (! safe->presets.saveFile (file, category, error))
                safe->showProblem (error);
            safe->refresh();
        };

        if (! file.existsAsFile())
        {
            save();
            return;
        }
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle ("Replace preset?")
                                          .withMessage ("\"" + file.getFileNameWithoutExtension() + "\" already exists. Replace it?")
                                          .withButton ("REPLACE").withButton ("CANCEL")
                                          .withAssociatedComponent (this),
                                      [save] (int result) { if (result == 1) save(); });
    }

    void showProblem (const juce::String& error)
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("Preset")
                                          .withMessage (error)
                                          .withButton ("OK")
                                          .withAssociatedComponent (this),
                                      nullptr);
    }

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
