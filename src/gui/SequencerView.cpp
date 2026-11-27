// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "gui/SequencerView.h"

StepGrid::StepGrid (NeonSynthProcessor& p)
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

void StepGrid::setEntryCursor (int step)
{
    if (step != entryCursor) { entryCursor = step; repaint(); }
}

StepSequencer::Scale StepGrid::scale() const noexcept
{
    return scaleValue != nullptr ? (StepSequencer::Scale) juce::jlimit (0, StepSequencer::numScales - 1, juce::roundToInt (scaleValue->load()))
                                 : StepSequencer::Scale::off;
}

int StepGrid::getLength() const noexcept
{
    return lengthValue != nullptr ? juce::jlimit (1, StepSequencer::numSteps, juce::roundToInt (lengthValue->load()))
                                  : StepSequencer::numSteps;
}

void StepGrid::refresh()
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

void StepGrid::paint (juce::Graphics& g)
{
    for (int i = 0; i < StepSequencer::numSteps; ++i)
        paintStep (g, i);
}

void StepGrid::mouseDown (const juce::MouseEvent& e)
{
    dragStep   = stepAt (e.position);
    dragged    = false;
    editNoted  = false;
    if (dragStep < 0) return;
    dragStart  = steps[(size_t) dragStep];
    dragStartY = e.position.y;
    dragZone   = zoneAt (dragStep, e.position);
}

void StepGrid::mouseDrag (const juce::MouseEvent& e)
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

void StepGrid::mouseUp (const juce::MouseEvent&)
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

void StepGrid::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
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

StepGrid::Zone StepGrid::zoneAt (int index, juce::Point<float> position) const
{
    const auto cell = cellBounds (index);
    if (position.y >= cell.getBottom() - markerHeight)
        return position.x < cell.getCentreX() ? Zone::tie : Zone::slide;
    if (position.y >= cell.getBottom() - barTop)
        return Zone::velocity;
    return Zone::body;
}

void StepGrid::paintStep (juce::Graphics& g, int i) const
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

void StepGrid::paintCell (juce::Graphics& g, int i) const
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

juce::Rectangle<float> StepGrid::cellBounds (int index) const
{
    const auto  bounds = getLocalBounds().toFloat();
    const float width  = bounds.getWidth() / (float) StepSequencer::numSteps;
    return { bounds.getX() + width * (float) index + 1.5f, bounds.getY(), width - 3.0f, bounds.getHeight() };
}

int StepGrid::stepAt (juce::Point<float> position) const
{
    const int index = (int) (position.x / ((float) getWidth() / (float) StepSequencer::numSteps));
    return juce::isPositiveAndBelow (index, StepSequencer::numSteps) ? index : -1;
}

void StepGrid::write (int index, const StepSequencer::Step& s)
{
    if (! editNoted && onBeforeEdit != nullptr)   // v1.11.0: once per gesture, before the change
        onBeforeEdit();
    editNoted = true;
    processor.setSequencerStep (index, s);
    steps[(size_t) index] = s;
    repaint();
}
