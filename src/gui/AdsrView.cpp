// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "gui/AdsrView.h"

AdsrView::AdsrView()
{
    setTooltip ("Drag the points: A, D and R left or right for the times, S up or down for the level. Double-click a point to reset it.");
}

void AdsrView::attach (juce::RangedAudioParameter* attack, juce::RangedAudioParameter* decay,
                       juce::RangedAudioParameter* sustain, juce::RangedAudioParameter* release)
{
    parameters = { attack, decay, sustain, release };
}

juce::Point<float> AdsrView::handlePosition (int handle) const
{
    const auto area = plotArea();
    const auto at = [&area] (float x01, float level)
    {
        return juce::Point<float> (area.getX() + area.getWidth() * x01, area.getBottom() - area.getHeight() * level);
    };
    switch (handle)
    {
        case attackHandle:  return at (attackW, 1.0f);
        case decayHandle:   return at (sustainStart(), sustainLevel);
        case sustainHandle: return at (0.5f * (sustainStart() + releaseStart()), sustainLevel);
        default:            return at (1.0f, 0.0f);
    }
}

void AdsrView::mouseDown (const juce::MouseEvent& e)
{
    dragging = handleAt (e.position);
    if (auto* param = parameterFor (dragging))
    {
        setHover (dragging);
        dragStartValue = param->getValue();
        param->beginChangeGesture();
    }
    else
    {
        dragging = -1;
    }
}

void AdsrView::mouseDrag (const juce::MouseEvent& e)
{
    auto* param = parameterFor (dragging);
    if (param == nullptr)
        return;

    const auto area = plotArea();
    // S follows the pointer's height. The times move by the distance dragged: the whole width
    // of the view sweeps a time's whole range (the shape itself is not to scale, see above).
    const float value = dragging == sustainHandle
                      ? param->convertTo0to1 (juce::jlimit (0.0f, 1.0f, (area.getBottom() - e.position.y) / area.getHeight()))
                      : dragStartValue + (e.position.x - e.mouseDownPosition.x) / area.getWidth();
    param->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, value));
    repaint();
}

void AdsrView::mouseUp (const juce::MouseEvent& e)
{
    if (auto* param = parameterFor (dragging))
        param->endChangeGesture();
    dragging = -1;
    setHover (handleAt (e.position));
}

void AdsrView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (auto* param = parameterFor (handleAt (e.position)))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->getDefaultValue());
        param->endChangeGesture();
        repaint();
    }
}

void AdsrView::setAccent (juce::Colour colour)
{
    accent = colour;
    repaint();
}

void AdsrView::update (float attack, float decay, float sustain, float release, const VoiceViews* voices)
{
    constexpr float sustainWidth = 0.22f;
    constexpr float minStageWidth = 0.04f;   // a stage of 0 s still gets a sliver, so its handle can be grabbed
    const float a = std::sqrt (attack), d = std::sqrt (decay), r = std::sqrt (release);
    const float total = a + d + r;
    attackW  = juce::jmax (minStageWidth, (1.0f - sustainWidth) * a / total);
    decayW   = juce::jmax (minStageWidth, (1.0f - sustainWidth) * d / total);
    releaseW = juce::jmax (minStageWidth, (1.0f - sustainWidth) * r / total);
    const float scale = (1.0f - sustainWidth) / (attackW + decayW + releaseW);
    attackW *= scale;  decayW *= scale;  releaseW *= scale;
    sustainLevel = sustain;

    const auto previousShape   = std::make_tuple (shownAttackW, shownDecayW, shownReleaseW, shownSustain);
    const auto previousMarkers = markers;
    const int  previousCount   = numMarkers;

    numMarkers = 0;
    for (size_t i = 0; voices != nullptr && i < voices->size(); ++i)
    {
        const auto& v = (*voices)[i];
        if (! v.active.load (std::memory_order_relaxed))
            continue;

        const float on  = v.secondsSinceOn.load (std::memory_order_relaxed);
        const float off = v.secondsSinceOff.load (std::memory_order_relaxed);
        float x;

        if (off >= 0.0f)            x = releaseStart() + releaseW * juce::jmin (1.0f, off / release);
        else if (on < attack)       x = attackW * on / attack;
        else if (on < attack + decay) x = attackW + decayW * (on - attack) / decay;
        else                        x = sustainStart() + sustainWidth * 0.9f * juce::jmin (1.0f, (on - attack - decay) / 2.0f);

        markers[(size_t) numMarkers++] = { x, v.level.load (std::memory_order_relaxed) };
    }

    // Repaint only when the shape or a voice dot moved.
    bool changed = std::make_tuple (attackW, decayW, releaseW, sustainLevel) != previousShape
                || numMarkers != previousCount;
    for (int i = 0; ! changed && i < numMarkers; ++i)
        changed = markers[(size_t) i].x != previousMarkers[(size_t) i].x
               || markers[(size_t) i].level != previousMarkers[(size_t) i].level;

    shownAttackW = attackW;  shownDecayW = decayW;  shownReleaseW = releaseW;  shownSustain = sustainLevel;
    if (changed)
        repaint();
}

void AdsrView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    neon::drawPanel (g, bounds, accent);
    const auto area = plotArea();

    const auto pointAt = [&] (float x01, float level)
    {
        return juce::Point<float> (area.getX() + area.getWidth() * x01,
                                   area.getBottom() - area.getHeight() * level);
    };

    juce::Path shape;
    shape.startNewSubPath (pointAt (0.0f, 0.0f));
    shape.lineTo (pointAt (attackW, 1.0f));
    shape.lineTo (pointAt (sustainStart(), sustainLevel));
    shape.lineTo (pointAt (releaseStart(), sustainLevel));
    shape.lineTo (pointAt (1.0f, 0.0f));

    auto fill = shape;
    fill.closeSubPath();
    g.setColour (accent.withAlpha (0.08f));
    g.fillPath (fill);

    // Stage boundaries: dotted, like a graticule's
    g.setColour (accent.withAlpha (0.22f));
    for (const float x : { attackW, sustainStart(), releaseStart() })
    {
        const float px = std::round (pointAt (x, 0.0f).x);
        for (float y = area.getY(); y < area.getBottom(); y += 4.0f)
            g.fillRect (juce::Rectangle<float> (px, y, 1.0f, 2.0f));
    }
    g.setColour (accent.withAlpha (0.18f));
    g.fillRect (juce::Rectangle<float> (area.getX(), std::round (area.getBottom()), area.getWidth(), 1.0f));   // the zero line

    neon::strokeGlow (g, shape, accent, 1.3f);

    for (int i = 0; i < numMarkers; ++i)
        neon::drawPhosphorDot (g, pointAt (markers[(size_t) i].x, markers[(size_t) i].level), accent, 5.0f);

    // The draggable points: round for the times (left / right), square for the level (up / down).
    // Dark centres with a glowing ring; filled with the hue while hovered or dragged.
    static constexpr const char* letters[numHandles] = { "A", "D", "S", "R" };
    for (int i = 0; i < numHandles; ++i)
    {
        const auto p = handlePosition (i);
        const bool active = i == hover || i == dragging;
        const auto box = juce::Rectangle<float> (handleSize, handleSize).withCentre (p);
        const auto paintShape = [&g, i] (juce::Rectangle<float> r, float stroke)
        {
            if (i == sustainHandle) { if (stroke > 0.0f) g.drawRect (r, stroke); else g.fillRect (r); }
            else                    { if (stroke > 0.0f) g.drawEllipse (r, stroke); else g.fillEllipse (r); }
        };

        g.setColour (accent.withAlpha (active ? 0.35f : 0.18f));
        paintShape (box.expanded (2.5f), 0.0f);
        g.setColour (active ? neon::phosphorOf (accent) : neon::displayInset);
        paintShape (box, 0.0f);
        g.setColour (accent);
        paintShape (box.reduced (0.75f), 1.5f);

        // The letter sits inside the plot: below A, above the others, left of R.
        const auto letter = juce::Rectangle<float> (16.0f, 14.0f)
                                .withCentre (i == attackHandle  ? p.translated (14.0f, 10.0f)
                                           : i == releaseHandle ? p.translated (-14.0f, -10.0f)
                                           :                      p.translated (0.0f, p.y - area.getY() > 24.0f ? -15.0f : 15.0f));
        neon::crt::drawGlowText (g, letters[i], letter, neon::phosphorOf (accent).withAlpha (active ? 1.0f : 0.75f),
                                 neon::font (12.0f, true), juce::Justification::centred, active ? 0.4f : 0.2f);
    }

    // While a point is hovered or dragged, say what it is and its value.
    const int shown = dragging >= 0 ? dragging : hover;
    if (const auto* param = parameterFor (shown))
    {
        static constexpr const char* names[numHandles] = { "ATTACK", "DECAY", "SUSTAIN", "RELEASE" };
        const float value = param->convertFrom0to1 (param->getValue());
        neon::crt::drawGlowText (g, juce::String (names[shown]) + "  " + (shown == sustainHandle ? juce::String (juce::roundToInt (value * 100.0f)) + "%"
                                                                                                    : neon::formatTime (value)),
                                 area.withTrimmedTop (2.0f).withTrimmedRight (4.0f), neon::phosphorOf (accent),
                                 neon::font (12.0f), juce::Justification::topRight, 0.35f);
    }
}

juce::RangedAudioParameter* AdsrView::parameterFor (int handle) const
{
    return handle >= 0 && handle < numHandles ? parameters[(size_t) handle] : nullptr;
}

int AdsrView::handleAt (juce::Point<float> position) const
{
    int nearest = -1;
    float best = grabRadius;
    for (int i = 0; i < numHandles; ++i)
        if (const float d = handlePosition (i).getDistanceFrom (position); d <= best && parameters[(size_t) i] != nullptr)
        {
            best = d;
            nearest = i;
        }
    return nearest;
}

void AdsrView::setHover (int handle)
{
    if (std::exchange (hover, handle) != handle)
    {
        setMouseCursor (handle < 0 ? juce::MouseCursor::NormalCursor
                      : handle == sustainHandle ? juce::MouseCursor::UpDownResizeCursor
                      :                           juce::MouseCursor::LeftRightResizeCursor);
        repaint();
    }
}
