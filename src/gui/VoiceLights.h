// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "plugin/NeonSynthProcessor.h"
#include "gui/AdsrView.h"
#include "gui/NeonStyle.h"

//==============================================================================
/** One light per voice; brightness follows that voice's envelope, with a short afterglow.
    A lit voice shows the note it plays, as a name with its octave (C4 = MIDI 60; v1.13.0). */
class VoiceLeds : public juce::Component,
                  public juce::SettableTooltipClient
{
public:
    /** Called with 1..16 when light N is clicked: the owner sets the Voices parameter (v1.9.0). */
    std::function<void (int)> onLightClicked;

    VoiceLeds()
    {
        setTooltip ("Voices in use. Click a light to set how many voices can play: 1 = mono");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    /** How many voices may play (the Voices parameter); the lights above it are drawn dimmed. */
    void setLimit (int newLimit)
    {
        newLimit = juce::jlimit (1, (int) glow.size(), newLimit);
        if (std::exchange (limit, newLimit) != newLimit)
            repaint();
    }

    /** Which light (1..16) an x position in this component falls on. */
    int lightAt (float x) const noexcept
    {
        const float step = (float) getWidth() / (float) glow.size();
        return juce::jlimit (1, (int) glow.size(), 1 + (int) std::floor (x / step));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (onLightClicked != nullptr)
            onLightClicked (lightAt (e.position.x));
    }

    void update (const AdsrView::VoiceViews& voices)
    {
        bool changed = false;
        for (size_t i = 0; i < voices.size(); ++i)
        {
            const float target = voices[i].active.load (std::memory_order_relaxed)
                               ? voices[i].level.load (std::memory_order_relaxed) : 0.0f;
            const float next = juce::jmax (target, glow[i] * glowDecay);
            if (std::abs (next - glow[i]) > repaintThreshold) { glow[i] = next; changed = true; }
            // The name stays while the light fades, even after the voice has let its note go.
            if (const int playing = voices[i].note.load (std::memory_order_relaxed); playing >= 0 && playing != notes[i])
            {
                notes[i] = playing;
                changed = true;
            }
        }
        if (changed)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        // Phosphor dots, as the terminal draws them: a lit voice is a glowing filled dot (brighter
        // the louder it is), an idle one that may play a dim ring, one above the limit a faint speck.
        const auto bounds  = getLocalBounds().toFloat();
        const float step   = bounds.getWidth() / (float) glow.size();
        const float radius = juce::jmin (step, bounds.getHeight()) * 0.30f;
        const juce::Colour idle { 0xff2f5c49 }, lit { 0xff8dffd0 };

        for (size_t i = 0; i < glow.size(); ++i)
        {
            const juce::Point<float> c (bounds.getX() + step * ((float) i + 0.5f), bounds.getCentreY());
            const float level = glow[i];
            const auto dot = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c);

            if (level > litThreshold && notes[i] >= 0)
            {
                // v1.13.0: a lit voice is a pill holding its note's name, dark on the phosphor.
                const auto pill = juce::Rectangle<float> (step - 1.0f, bounds.getHeight() - 1.0f).withCentre (c);
                g.setColour (neon::crt::phosphorGlow.withAlpha (0.40f * level));
                g.fillRoundedRectangle (pill.expanded (1.5f), 5.0f);
                g.setColour (idle.interpolatedWith (lit, 0.5f + 0.5f * std::sqrt (level)));
                g.fillRoundedRectangle (pill, 4.0f);
                g.setColour (neon::background);
                g.setFont (neon::font (12.0f, true));
                g.drawFittedText (noteName (notes[i]), pill.toNearestInt(), juce::Justification::centred, 1, 0.8f);
            }
            else if (level > litThreshold)
            {
                g.setColour (neon::crt::phosphorGlow.withAlpha (0.40f * level));
                g.fillEllipse (dot.expanded (radius * 1.2f));
                g.setColour (idle.interpolatedWith (lit, 0.5f + 0.5f * std::sqrt (level)));
                g.fillEllipse (dot);
            }
            else if ((int) i < limit)
            {
                g.setColour (idle.brighter (0.2f));
                g.drawEllipse (dot.reduced (0.6f), 1.2f);
            }
            else
            {
                // Above the limit: a faint speck, so the lights that can play stand out.
                g.setColour (idle.withAlpha (0.55f));
                g.fillEllipse (juce::Rectangle<float> (radius * 0.8f, radius * 0.8f).withCentre (c));
            }
        }
    }

    /** The note light i (0..15) shows, or -1 (for tests). */
    int getShownNote (int light) const noexcept
    {
        return juce::isPositiveAndBelow (light, (int) glow.size()) && glow[(size_t) light] > litThreshold ? notes[(size_t) light] : -1;
    }

    /** "C#4": the name and octave, middle C (MIDI 60) being C4, as the step sequencer writes it. */
    static juce::String noteName (int note)   { return juce::MidiMessage::getMidiNoteName (note, true, true, 4); }

private:
    static constexpr float glowDecay        = 0.75f;   // per timer tick, so a light fades in a few frames rather than blinking out
    static constexpr float repaintThreshold = 0.002f;  // changes smaller than this are not worth a repaint
    static constexpr float litThreshold     = 0.01f;   // a voice glows above this level; below it, it is drawn idle

    std::array<float, NeonSynthProcessor::numVoices> glow {};
    std::array<int, NeonSynthProcessor::numVoices> notes = [] { std::array<int, NeonSynthProcessor::numVoices> n {}; n.fill (-1); return n; }();
    int limit = NeonSynthProcessor::numVoices;
};
