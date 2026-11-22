// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "plugin/NeonSynthProcessor.h"
#include "gui/NeonStyle.h"

//==============================================================================
/** One light per voice; brightness follows that voice's envelope, with a short afterglow. */
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
            const float next = juce::jmax (target, glow[i] * 0.75f);
            if (std::abs (next - glow[i]) > 0.002f) { glow[i] = next; changed = true; }
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

            if (level > 0.01f)
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

private:
    std::array<float, NeonSynthProcessor::numVoices> glow {};
    int limit = NeonSynthProcessor::numVoices;
};
