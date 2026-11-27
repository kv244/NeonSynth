// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "plugin/NeonSynthProcessor.h"
#include "gui/NeonStyle.h"

/**
    @file SequencerView.h

    The 16-step grid shown in the editor's sequencer strip.

    Each cell shows one step as a block of amber phosphor (v1.10.0 CRT look):
    the note name in the middle in VT323, glowing when the step is on and dim
    when it is off (an off step keeps its note), and the velocity as a row of
    blocks along the bottom. The playing step is drawn inverted, an amber block
    with dark text, like a terminal cursor; every fourth cell has a mark along
    its top edge for the beats.

    Editing (all on the message thread, written to the processor lock-free):
      - click                     toggle the step on / off
      - drag up / down            change the note, one semitone per 6 px (with the scale lock
                                  on, one note of the scale per 6 px, so it lands in the scale)
      - drag the velocity bar     change the velocity, 1 per px (v1.11.0: velocity is the accent)
      - shift-drag or right-drag  change the velocity from anywhere in the cell
      - mouse wheel               note +/- 1 semitone or scale note (with shift: velocity +/- 4)
      - click TIE / SLD           v1.11.0: the two markers on the cell's bottom edge toggle the
                                  step's tie and slide

    v1.11.0: steps beyond the pattern's LENGTH are drawn dim (they keep their notes and come
    back when the pattern is made longer), and while ENTER waits for notes the step the next
    note goes to is outlined. The pattern row (PatternRow.h) hooks in through onBeforeEdit
    (for undo) and onStepClicked (for ENTER).

    The grid keeps no pattern of its own: refresh(), called from the editor's
    timer, re-reads the 16 steps and the playhead from the processor and
    repaints only when something changed. That also keeps it in sync when a
    host loads a preset or a session.
*/
class StepGrid : public juce::Component,
                 public juce::SettableTooltipClient
{
public:
    explicit StepGrid (NeonSynthProcessor& p);

    /** v1.11.0: called once before each edit the grid makes, while the pattern is still as it was (undo). */
    std::function<void()> onBeforeEdit;

    /** v1.11.0: a click on a step's body; return true to take it (ENTER picks its first step) instead of toggling. */
    std::function<bool (int step)> onStepClicked;

    /** v1.11.0: the step ENTER writes next, outlined; -1 for none. */
    void setEntryCursor (int step);

    /** v1.11.0: the pattern's length, as the grid shows it (steps beyond it are dim). */
    /** KEY and SCALE as the parameters hold them (v1.11.0); the pattern row's tools and ENTER
        read them here too, so the grid and the row always agree. */
    int key() const noexcept   { return keyValue != nullptr ? juce::jlimit (0, 11, juce::roundToInt (keyValue->load())) : 0; }

    StepSequencer::Scale scale() const noexcept;

    int getLength() const noexcept;

    /** Re-reads the pattern and playhead; repaints if anything changed. Message thread. */
    void refresh();

    void paint (juce::Graphics& g) override;

    //==========================================================================
    void mouseDown (const juce::MouseEvent& e) override;

    void mouseDrag (const juce::MouseEvent& e) override;

    void mouseUp (const juce::MouseEvent&) override;

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

private:
    static constexpr int wheelVelocityStep = 4;   // a mouse-wheel notch with Shift moves the velocity this much

    // Amber phosphor shades (from the CRT mock-up): lit text, its halo, the
    // dim text of an off step, and the inverted block of the playing step.
    static inline const juce::Colour litText   { 0xffffd28a };
    static inline const juce::Colour litGlow   { 0xffffb03c };
    static inline const juce::Colour dimText   { 0xff8a6c38 };
    static inline const juce::Colour headBlock { 0xffffc15e };
    static inline const juce::Colour headText  { 0xff1a0f00 };

    // v1.11.0: the bottom of each cell, from the bottom up: the TIE / SLD markers, then the velocity bar.
    static constexpr float markerHeight = 12.0f;
    static constexpr float barTop       = 26.0f;   // the velocity bar's zone: from here to the markers

    enum class Zone { body, velocity, tie, slide };

    Zone zoneAt (int index, juce::Point<float> position) const;


    void paintStep (juce::Graphics& g, int i) const;

    void paintCell (juce::Graphics& g, int i) const;

    juce::Rectangle<float> cellBounds (int index) const;

    int stepAt (juce::Point<float> position) const;

    void write (int index, const StepSequencer::Step& s);

    NeonSynthProcessor& processor;
    std::atomic<float>* lengthValue = nullptr;   // v1.11.0: the pattern row's parameters
    std::atomic<float>* keyValue    = nullptr;
    std::atomic<float>* scaleValue  = nullptr;
    std::array<StepSequencer::Step, StepSequencer::numSteps> steps {};
    int   playhead = -1;
    int   shownLength = StepSequencer::numSteps;
    int   entryCursor = -1;
    Zone  dragZone = Zone::body;
    bool  editNoted = false;   // onBeforeEdit has been called for this gesture

    int   dragStep = -1;
    bool  dragged = false;
    float dragStartY = 0.0f;
    StepSequencer::Step dragStart;
};
