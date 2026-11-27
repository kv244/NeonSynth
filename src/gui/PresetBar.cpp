// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "gui/PresetBar.h"

PresetBar::PresetBar (PresetManager& manager)
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

    presets.addChangeListener (this);
    refresh();
}

PresetBar::~PresetBar()
{
    presets.removeChangeListener (this);
}

void PresetBar::resized()
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

juce::PopupMenu PresetBar::createMenu()
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

void PresetBar::menuItemChosen (int id)
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

void PresetBar::refresh()
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

void PresetBar::showMenu()
{
    createMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&nameButton),
                                [safe = juce::Component::SafePointer<PresetBar> (this)] (int id)
                                {
                                    if (safe != nullptr && id != 0)
                                        safe->menuItemChosen (id);
                                });
}

void PresetBar::chooseFile()
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

void PresetBar::askToSave()
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

void PresetBar::saveAs (const juce::String& name, const juce::String& category)
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

void PresetBar::showProblem (const juce::String& error)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::WarningIcon)
                                      .withTitle ("Preset")
                                      .withMessage (error)
                                      .withButton ("OK")
                                      .withAssociatedComponent (this),
                                  nullptr);
}
