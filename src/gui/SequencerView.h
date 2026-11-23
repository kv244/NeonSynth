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
    explicit StepGrid (NeonSynthProcessor& p)
        : processor (p),
          lengthValue (p.getAPVTS().getRawParameterValue ("seqLength")),   // v1.11.0
          keyValue    (p.getAPVTS().getRawParameterValue ("seqKey")),
          scaleValue  (p.getAPVTS().getRawParameterValue ("seqScale"))
    {
        setTooltip ("Click: step on/off.  Drag up/down: note (in the scale when it is locked).  Drag the velocity bar "
                    "(or shift-drag): velocity, the step's accent.  TIE / SLD: hold into the next step / glide to it.  "
                    "Wheel: note (shift: velocity).");
        refresh();
    }

    /** v1.11.0: called once before each edit the grid makes, while the pattern is still as it was (undo). */
    std::function<void()> onBeforeEdit;

    /** v1.11.0: a click on a step's body; return true to take it (ENTER picks its first step) instead of toggling. */
    std::function<bool (int step)> onStepClicked;

    /** v1.11.0: the step ENTER writes next, outlined; -1 for none. */
    void setEntryCursor (int step)
    {
        if (step != entryCursor) { entryCursor = step; repaint(); }
    }

    /** v1.11.0: the pattern's length, as the grid shows it (steps beyond it are dim). */
    /** KEY and SCALE as the parameters hold them (v1.11.0); the pattern row's tools and ENTER
        read them here too, so the grid and the row always agree. */
    int key() const noexcept   { return keyValue != nullptr ? juce::jlimit (0, 11, juce::roundToInt (keyValue->load())) : 0; }

    StepSequencer::Scale scale() const noexcept
    {
        return scaleValue != nullptr ? (StepSequencer::Scale) juce::jlimit (0, StepSequencer::numScales - 1, juce::roundToInt (scaleValue->load()))
                                     : StepSequencer::Scale::off;
    }

    int getLength() const noexcept
    {
        return lengthValue != nullptr ? juce::jlimit (1, StepSequencer::numSteps, juce::roundToInt (lengthValue->load()))
                                      : StepSequencer::numSteps;
    }

    /** Re-reads the pattern and playhead; repaints if anything changed. Message thread. */
    void refresh()
    {
        bool changed = false;
        for (int i = 0; i < StepSequencer::numSteps; ++i)
        {
            const auto s = processor.getSequencerStep (i);
            if (! (s == steps[(size_t) i])) { steps[(size_t) i] = s; changed = true; }
        }

        const int head = processor.getSequencerPlayhead();
        if (head != playhead) { playhead = head; changed = true; }

        if (const int length = getLength(); length != shownLength) { shownLength = length; changed = true; }   // v1.11.0

        if (changed)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        for (int i = 0; i < StepSequencer::numSteps; ++i)
            paintStep (g, i);
    }

    //==========================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStep   = stepAt (e.position);
        dragged    = false;
        editNoted  = false;
        if (dragStep < 0) return;
        dragStart  = steps[(size_t) dragStep];
        dragStartY = e.position.y;
        dragZone   = zoneAt (dragStep, e.position);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragStep < 0) return;

        const float dy = dragStartY - e.position.y;   // up = positive
        if (! dragged && std::abs (dy) < 3.0f)        // small wobble still counts as a click
            return;
        dragged = true;

        auto s = dragStart;
        if (e.mods.isShiftDown() || e.mods.isRightButtonDown() || dragZone != Zone::body)   // v1.11.0: or on the velocity bar
            s.velocity = juce::jlimit (1, StepSequencer::maxMidiValue, dragStart.velocity + juce::roundToInt (dy));
        else
            s.note = StepSequencer::moveInScale (dragStart.note, key(), scale(), juce::roundToInt (dy / 6.0f));   // v1.11.0: snaps

        if (! (s == steps[(size_t) dragStep]))
            write (dragStep, s);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragStep >= 0 && ! dragged)
        {
            auto s = steps[(size_t) dragStep];
            if (dragZone == Zone::tie)         s.tie   = ! s.tie;     // v1.11.0
            else if (dragZone == Zone::slide)  s.slide = ! s.slide;
            else if (onStepClicked != nullptr && onStepClicked (dragStep)) { dragStep = -1; return; }
            else                               s.on = ! s.on;
            write (dragStep, s);
        }
        dragStep = -1;
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const int index = stepAt (e.position);
        if (index < 0 || wheel.deltaY == 0.0f) return;

        const int direction = wheel.deltaY > 0.0f ? 1 : -1;
        auto s = steps[(size_t) index];
        if (e.mods.isShiftDown()) s.velocity = juce::jlimit (1, StepSequencer::maxMidiValue, s.velocity + wheelVelocityStep * direction);
        else                      s.note     = StepSequencer::moveInScale (s.note, key(), scale(), direction);   // v1.11.0: snaps
        editNoted = false;   // each wheel notch is an edit of its own
        if (! (s == steps[(size_t) index]))
            write (index, s);
    }

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

    Zone zoneAt (int index, juce::Point<float> position) const
    {
        const auto cell = cellBounds (index);
        if (position.y >= cell.getBottom() - markerHeight)
            return position.x < cell.getCentreX() ? Zone::tie : Zone::slide;
        if (position.y >= cell.getBottom() - barTop)
            return Zone::velocity;
        return Zone::body;
    }


    void paintStep (juce::Graphics& g, int i) const
    {
        // v1.11.0: a step beyond LENGTH is drawn faded: it keeps its note but does not play.
        const bool beyond = i >= shownLength;
        if (beyond)
            g.beginTransparencyLayer (0.3f);

        paintCell (g, i);

        if (beyond)
            g.endTransparencyLayer();

        if (i == entryCursor)   // v1.11.0: where ENTER writes the next note
        {
            g.setColour (neon::cyan);
            g.drawRect (cellBounds (i).expanded (1.0f), 2.0f);
        }
    }

    void paintCell (juce::Graphics& g, int i) const
    {
        const auto  cell    = cellBounds (i);
        const auto& s       = steps[(size_t) i];
        const bool  playing = i == playhead;
        const float level   = (float) s.velocity / 127.0f;

        // Cell: square, a 1 px line in amber with a faint halo when on. The playing
        // step is a solid block with a soft bloom around it. A handful of flat
        // fills and strokes, no gradients, and only when a step or the playhead moves.
        if (playing)
        {
            // The bloom stays within the 3 px gap so it never touches the next cell.
            g.setColour (litGlow.withAlpha (0.35f));
            g.drawRect (cell.expanded (1.0f), 1.0f);
            g.setColour (headBlock.withAlpha (s.on ? 1.0f : 0.75f));
            g.fillRect (cell);
        }
        else if (s.on)
        {
            g.setColour (litGlow.withAlpha (0.05f + 0.10f * level));
            g.fillRect (cell);
            g.setColour (litGlow.withAlpha (0.12f));
            g.drawRect (cell.expanded (1.0f), 1.0f);
            g.setColour (litText.withAlpha (0.75f));
            g.drawRect (cell, 1.0f);
        }
        else
        {
            g.setColour (neon::amber.withAlpha (0.20f));
            g.drawRect (cell, 1.0f);
        }

        if (i % 4 == 0)   // beat mark
        {
            g.setColour (playing ? headText.withAlpha (0.7f) : neon::amber.withAlpha (s.on ? 0.9f : 0.45f));
            g.fillRect (cell.getX() + 4.0f, cell.getY() + 3.0f, cell.getWidth() - 8.0f, 2.0f);
        }

        // Note name: VT323, large enough to read across the room.
        const auto name  = juce::MidiMessage::getMidiNoteName (s.note, true, true, 4);
        const auto font  = neon::crt::displayFont (juce::jmin (40.0f, cell.getWidth() * 0.52f));
        const auto label = cell.withTrimmedBottom (barTop);
        g.setFont (font);
        if (playing)
        {
            g.setColour (headText);
            g.drawText (name, label, juce::Justification::centred, false);
        }
        else if (s.on)
        {
            // Halo: the text a pixel off in each direction, faint, then the text itself.
            g.setColour (litGlow.withAlpha (0.22f));
            for (const auto& [dx, dy] : { std::pair { -1.0f, 0.0f }, std::pair { 1.0f, 0.0f }, std::pair { 0.0f, -1.0f }, std::pair { 0.0f, 1.0f } })
                g.drawText (name, label.translated (dx, dy), juce::Justification::centred, false);
            g.setColour (litText);
            g.drawText (name, label, juce::Justification::centred, false);
        }
        else
        {
            g.setColour (dimText);
            g.drawText (name, label, juce::Justification::centred, false);
        }

        // Velocity: a row of blocks along the bottom. The last lit block is drawn
        // in proportion to what is left, so every change of velocity still shows.
        const auto  bar     = cell.reduced (5.0f, 0.0f).removeFromBottom (barTop).withTrimmedBottom (markerHeight + 3.0f).withTrimmedTop (2.0f);
        const int   blocks  = juce::jmax (1, (int) (bar.getWidth() / 5.0f));
        const float pitch   = bar.getWidth() / (float) blocks;
        const float filled  = level * (float) blocks;
        const auto  barLit  = playing ? headText : (s.on ? litText : neon::amber);
        const float barOn   = playing ? 0.85f : (s.on ? 0.9f : 0.35f);
        const float barOff  = playing ? 0.18f : 0.10f;
        for (int b = 0; b < blocks; ++b)
        {
            const float lit = juce::jlimit (0.0f, 1.0f, filled - (float) b);
            const juce::Rectangle<float> block { bar.getX() + pitch * (float) b, bar.getY(), pitch - 1.0f, bar.getHeight() };
            g.setColour (barLit.withAlpha (barOff + (barOn - barOff) * lit));
            g.fillRect (block);
        }

        // v1.11.0: TIE and SLD along the bottom edge, lit when set. A set tie also draws a line
        // across the gap to the next cell, so a tied run reads as one long note.
        const auto markers = cell.reduced (3.0f, 0.0f).removeFromBottom (markerHeight).withTrimmedBottom (1.0f);
        const auto markerFont = neon::crt::displayFont (markerHeight + 1.0f);
        g.setFont (markerFont);
        const auto drawMarker = [&] (juce::Rectangle<float> area, const char* markerName, bool set)
        {
            const auto ink = playing ? headText : (set ? litText : neon::amber);
            if (set)
            {
                g.setColour ((playing ? headText : litGlow).withAlpha (0.25f));
                g.fillRect (area.reduced (1.0f, 0.0f));
            }
            g.setColour (ink.withAlpha (set ? 1.0f : 0.40f));
            g.drawText (markerName, area, juce::Justification::centred, false);
        };
        drawMarker (markers.withWidth (markers.getWidth() * 0.5f), "TIE", s.tie);
        drawMarker (markers.withTrimmedLeft (markers.getWidth() * 0.5f), "SLD", s.slide);

        if (s.tie)
        {
            g.setColour (litText.withAlpha (0.8f));
            g.fillRect (cell.getRight() - 2.0f, markers.getCentreY() - 1.0f, 5.0f, 2.0f);   // into the 3 px gap
        }
    }

    juce::Rectangle<float> cellBounds (int index) const
    {
        const auto  bounds = getLocalBounds().toFloat();
        const float width  = bounds.getWidth() / (float) StepSequencer::numSteps;
        return { bounds.getX() + width * (float) index + 1.5f, bounds.getY(), width - 3.0f, bounds.getHeight() };
    }

    int stepAt (juce::Point<float> position) const
    {
        const int index = (int) (position.x / ((float) getWidth() / (float) StepSequencer::numSteps));
        return juce::isPositiveAndBelow (index, StepSequencer::numSteps) ? index : -1;
    }

    void write (int index, const StepSequencer::Step& s)
    {
        if (! editNoted && onBeforeEdit != nullptr)   // v1.11.0: once per gesture, before the change
            onBeforeEdit();
        editNoted = true;
        processor.setSequencerStep (index, s);
        steps[(size_t) index] = s;
        repaint();
    }

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
