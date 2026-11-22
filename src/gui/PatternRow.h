// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "plugin/NeonSynthProcessor.h"
#include "gui/NeonStyle.h"
#include "gui/SequencerView.h"

/**
    @file PatternRow.h

    v1.11.0: the sequencer's pattern row, one self-contained component above the step grid:
    LENGTH, KEY and SCALE (parameters), TOOLS (a menu of pattern tools), ENTER (step entry from
    the keyboard) and UNDO. It is one component, not loose controls in the editor, so a later
    page layout can move it whole (the "Three schools" rule). The row itself has no component
    ID; its six controls keep the IDs seqLength, seqKey, seqScale, seqTools, seqEnter, seqUndo.

    Everything here runs on the message thread. The pattern logic lives in StepSequencer.h
    (PatternTools, PatternHistory, NoteEntry), where the unit tests reach it; this file only
    wires it to the controls, the step grid and the processor:

      - UNDO (and Ctrl+Z, through the editor's keyPressed) steps back through 32 whole-pattern
        snapshots. A snapshot is taken before each grid gesture, each tool and each ENTER run.
      - ENTER arms the processor's note queue; click a step to start, then each key played
        writes that step (snapped to the scale lock) and moves on. It ends on the last step of
        LENGTH or when ENTER is clicked off.
      - The scale lock only acts when a note is set (dragging, the wheel, ENTER, randomise);
        notes already in the pattern stay as they are.
*/
class PatternRow : public juce::Component,
                   private juce::Timer
{
public:
    enum class Tool { shiftLeft = 1, shiftRight, octaveUp, octaveDown, reverse, clear, randomise };

    PatternRow (NeonSynthProcessor& p, StepGrid& stepGrid)
        : processor (p), apvts (p.getAPVTS()), grid (stepGrid)
    {
        for (auto* combo : { &lengthCombo, &keyCombo, &scaleCombo })
        {
            combo->setColour (juce::ComboBox::outlineColourId, neon::amber);
            combo->setColour (juce::ComboBox::textColourId,    neon::amber);
            combo->setColour (juce::ComboBox::arrowColourId,   neon::amber);
            addAndMakeVisible (combo);
        }
        for (int n = 1; n <= StepSequencer::numSteps; ++n)
            lengthCombo.addItem (juce::String (n) + (n == 1 ? " step" : " steps"), n);
        keyCombo.addItemList ({ "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 1);
        scaleCombo.addItemList ({ "Scale off", "Major", "Dorian", "Phrygian", "Lydian", "Mixolydian",
                                  "Natural minor", "Locrian", "Harmonic minor", "Major pentatonic",
                                  "Minor pentatonic", "Blues", "Whole tone" }, 1);   // same order as the parameter
        lengthCombo.setTooltip ("Pattern length: the pattern repeats after this many steps (try 5 or 7 against the beat)");
        keyCombo.setTooltip ("Key of the scale lock");
        scaleCombo.setTooltip ("Scale lock: notes you set on the steps snap to this scale. Notes already there stay");
        lengthAttachment = std::make_unique<ComboBoxAttachment> (apvts, "seqLength", lengthCombo);
        keyAttachment    = std::make_unique<ComboBoxAttachment> (apvts, "seqKey",    keyCombo);
        scaleAttachment  = std::make_unique<ComboBoxAttachment> (apvts, "seqScale",  scaleCombo);

        for (auto* button : { &toolsButton, &enterButton, &undoButton })
        {
            button->setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
            button->setColour (juce::TextButton::buttonOnColourId, neon::amber.withAlpha (0.30f));
            button->setColour (juce::TextButton::textColourOffId,  neon::amber.withAlpha (0.8f));
            button->setColour (juce::TextButton::textColourOnId,   neon::amber);
            button->setColour (juce::ComboBox::outlineColourId,    neon::amber.withAlpha (0.6f));
            addAndMakeVisible (button);
        }
        enterButton.setClickingTogglesState (true);
        toolsButton.setTooltip ("Pattern tools: shift, octave up or down, reverse, clear, randomise in the scale");
        enterButton.setTooltip ("Enter notes from the keyboard: click a step, then play; each key fills a step and moves on");
        undoButton.setTooltip ("Undo the last change to the pattern (Ctrl+Z)");

        toolsButton.onClick = [this] { showToolsMenu(); };
        enterButton.onClick = [this] { setArmed (enterButton.getToggleState()); };
        undoButton.onClick  = [this] { undo(); };

        const std::pair<juce::Component*, const char*> ids[] = {
            { &lengthCombo, "seqLength" }, { &keyCombo, "seqKey" }, { &scaleCombo, "seqScale" },
            { &toolsButton, "seqTools" }, { &enterButton, "seqEnter" }, { &undoButton, "seqUndo" } };
        for (const auto& [component, id] : ids)
            component->setComponentID (id);

        grid.onBeforeEdit  = [this] { rememberPattern(); };
        grid.onStepClicked = [this] (int step) { return startEntryAt (step); };

        updateUndoButton();
    }

    ~PatternRow() override
    {
        stopTimer();
        processor.setNoteEntryArmed (false);   // no editor left to read the queue
        grid.onBeforeEdit  = nullptr;
        grid.onStepClicked = nullptr;
    }

    void resized() override
    {
        // Design units, relative to the row's place (150, 690, 604 x 24) in the editor.
        lengthCombo.setBounds (0,   0, 104, 24);
        keyCombo.setBounds    (112, 0, 60,  24);
        scaleCombo.setBounds  (176, 0, 168, 24);
        toolsButton.setBounds (352, 0, 84,  24);
        enterButton.setBounds (444, 0, 80,  24);
        undoButton.setBounds  (532, 0, 72,  24);
    }

    //==========================================================================
    /** Ctrl+Z (Cmd+Z on a Mac) from the editor's keyPressed. Returns true if it was that key. */
    bool handleUndoKey (const juce::KeyPress& key)
    {
        if (key != juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0))
            return false;
        undo();
        return true;
    }

    /** Steps the pattern back to the newest snapshot that differs from it. */
    void undo()
    {
        auto pattern = currentPattern();
        if (history.undo (pattern))
            writePattern (pattern);
        updateUndoButton();
    }

    /** Takes a snapshot of the pattern as it is now, before a change. */
    void rememberPattern()
    {
        history.remember (currentPattern());
        updateUndoButton();
    }

    /** Applies one of the TOOLS menu's entries (the menu calls this; so can a test). */
    void applyTool (Tool tool)
    {
        const auto before = currentPattern();
        const int  length = grid.getLength();
        StepSequencer::Pattern after;

        switch (tool)
        {
            case Tool::shiftLeft:  after = PatternTools::shiftLeft  (before, length); break;
            case Tool::shiftRight: after = PatternTools::shiftRight (before, length); break;
            case Tool::octaveUp:   after = PatternTools::octaveUp   (before, length); break;
            case Tool::octaveDown: after = PatternTools::octaveDown (before, length); break;
            case Tool::reverse:    after = PatternTools::reverse    (before, length); break;
            case Tool::clear:      after = PatternTools::clear      (before, length); break;
            case Tool::randomise:
            default:               after = PatternTools::randomise  (before, length, key(), scale(),
                                                                     juce::Random::getSystemRandom().nextInt64()); break;
        }

        if (after == before)
            return;   // e.g. an octave up with a note already at the top: nothing to undo either
        history.remember (before);
        writePattern (after);
        updateUndoButton();
    }

    bool canUndo() const noexcept           { return history.canUndo(); }

private:
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void showToolsMenu()
    {
        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());
        menu.addItem ((int) Tool::shiftLeft,  "Shift left");
        menu.addItem ((int) Tool::shiftRight, "Shift right");
        menu.addSeparator();
        menu.addItem ((int) Tool::octaveUp,   "Octave up");
        menu.addItem ((int) Tool::octaveDown, "Octave down");
        menu.addItem ((int) Tool::reverse,    "Reverse");
        menu.addSeparator();
        menu.addItem ((int) Tool::randomise,  scale() == StepSequencer::Scale::off ? "Randomise" : "Randomise in the scale");
        menu.addItem ((int) Tool::clear,      "Clear");

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&toolsButton),
                            [safe = juce::Component::SafePointer<PatternRow> (this)] (int result)
                            {
                                if (safe != nullptr && result >= (int) Tool::shiftLeft && result <= (int) Tool::randomise)
                                    safe->applyTool ((Tool) result);
                            });
    }

    //==========================================================================
    // ENTER

    void setArmed (bool armed)
    {
        if (armed)
        {
            entry.arm();
            enteredSinceArmed = false;
            int note = 0, velocity = 0;
            while (processor.popEnteredNote (note, velocity)) {}   // drop anything left from before
            startTimerHz (60);
        }
        else
        {
            entry.disarm();
            stopTimer();
        }
        processor.setNoteEntryArmed (armed);
        enterButton.setToggleState (armed, juce::dontSendNotification);
        grid.setEntryCursor (entry.getCursor());
    }

    /** The grid's click while armed: entry starts at that step. */
    bool startEntryAt (int step)
    {
        if (! entry.isArmed())
            return false;
        entry.start (step);
        grid.setEntryCursor (entry.getCursor());
        return true;
    }

    void timerCallback() override
    {
        int note = 0, velocity = 0;
        while (entry.isArmed() && processor.popEnteredNote (note, velocity))
        {
            auto pattern = currentPattern();
            const auto before = pattern;
            const auto result = entry.enter (pattern, note, velocity, grid.getLength(), key(), scale());
            if (result == NoteEntry::Result::ignored)
                continue;   // no step clicked yet

            if (! enteredSinceArmed)   // one undo level for the whole run of entered notes
            {
                history.remember (before);
                enteredSinceArmed = true;
                updateUndoButton();
            }
            writePattern (pattern);

            if (result == NoteEntry::Result::finished)
                setArmed (false);
        }
        grid.setEntryCursor (entry.getCursor());
    }

    //==========================================================================
    /** Writes the steps that differ (each one tells the host the state changed). */
    void writePattern (const StepSequencer::Pattern& pattern)
    {
        processor.setSequencerPattern (pattern);   // one host notification, however many steps change
        grid.refresh();
    }

    StepSequencer::Pattern currentPattern() const
    {
        StepSequencer::Pattern pattern;
        for (int i = 0; i < StepSequencer::numSteps; ++i)
            pattern[(size_t) i] = processor.getSequencerStep (i);
        return pattern;
    }

    void updateUndoButton()   { undoButton.setEnabled (history.canUndo()); }

    int key() const   { return juce::jlimit (0, 11, keyCombo.getSelectedItemIndex()); }

    StepSequencer::Scale scale() const
    {
        return (StepSequencer::Scale) juce::jlimit (0, StepSequencer::numScales - 1, scaleCombo.getSelectedItemIndex());
    }

    NeonSynthProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    StepGrid& grid;

    juce::ComboBox   lengthCombo, keyCombo, scaleCombo;
    juce::TextButton toolsButton { "TOOLS" }, enterButton { "ENTER" }, undoButton { "UNDO" };
    std::unique_ptr<ComboBoxAttachment> lengthAttachment, keyAttachment, scaleAttachment;

    PatternHistory history;
    NoteEntry      entry;
    bool           enteredSinceArmed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatternRow)
};
