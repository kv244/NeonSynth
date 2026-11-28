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

    PatternRow (NeonSynthProcessor& p, StepGrid& stepGrid);

    ~PatternRow() override;

    void resized() override;

    //==========================================================================
    /** Ctrl+Z (Cmd+Z on a Mac) from the editor's keyPressed. Returns true if it was that key. */
    bool handleUndoKey (const juce::KeyPress& key);

    /** Steps the pattern back to the newest snapshot that differs from it. */
    void undo();

    /** Takes a snapshot of the pattern as it is now, before a change. */
    void rememberPattern();

    /** v1.13.0: the user chose a scale or key: every step snaps to it (one undo level). */
    void snapPatternToScale();

    /** Applies one of the TOOLS menu's entries (the menu calls this; so can a test). */
    void applyTool (Tool tool);

    bool canUndo() const noexcept           { return history.canUndo(); }

private:
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void showToolsMenu();

    //==========================================================================
    // ENTER

    void setArmed (bool armed);

    /** The grid's click while armed: entry starts at that step. */
    bool startEntryAt (int step);

    void timerCallback() override;

    //==========================================================================
    /** Writes the steps that differ (each one tells the host the state changed). */
    void writePattern (const StepSequencer::Pattern& pattern);

    StepSequencer::Pattern currentPattern() const;

    void updateUndoButton()   { undoButton.setEnabled (history.canUndo()); }

public:
    /** The editor's timer: a session or preset was loaded since the last call, so the undo
        history belongs to another sound; it starts afresh (v1.11.0). */
    void forgetHistoryIfStateLoaded();

private:
    static constexpr int entryPollHz = 60;   // while ENTER is armed, played notes are picked up this often
    int seenStateLoads = 0;

    int key() const                  { return grid.key(); }
    StepSequencer::Scale scale() const { return grid.scale(); }

    NeonSynthProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    StepGrid& grid;

    /**
        A menu that knows when the user picked from it (v1.13.0). A host's automation, a
        session or a preset changes the parameter, and through the attachment the menu, the
        same way; only a change that follows the menu's own popup counts as the user's.
    */
    class PickedComboBox : public juce::ComboBox, private juce::Timer
    {
    public:
        /** Called after the user picks a new item (the parameter is already set). */
        std::function<void()> onUserPick;

        void showPopup() override
        {
            picking = true;
            closedTicks = 0;
            juce::ComboBox::showPopup();
            startTimerHz (30);
        }

        /** The ComboBox's own onChange, routed here; true if the user made this change. */
        bool takeUserPick() noexcept   { return std::exchange (picking, false); }

    private:
        void timerCallback() override
        {
            // The popup closed with nothing picked: after two quiet ticks, stop listening.
            if (isPopupActive())
                closedTicks = 0;
            else if (++closedTicks >= 2)
            {
                picking = false;
                stopTimer();
            }
        }

        bool picking = false;
        int  closedTicks = 0;
    };

    PickedComboBox   lengthCombo, keyCombo, scaleCombo;   // LENGTH's pick changes no steps
    juce::TextButton toolsButton { "TOOLS" }, enterButton { "ENTER" }, undoButton { "UNDO" };
    std::unique_ptr<ComboBoxAttachment> lengthAttachment, keyAttachment, scaleAttachment;

    PatternHistory history;
    NoteEntry      entry;
    bool           enteredSinceArmed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatternRow)
};
