// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "gui/NeonStyle.h"

//==============================================================================
/**
    Stereo output meter: an RMS bar per channel, a peak line, a 1.5 s peak hold
    and a shared clip light. The output never exceeds the soft clipper's -1 dBFS
    ceiling, so the light shows when the clipper is working hard (peaks above
    -2 dBFS) rather than digital overs. Click to reset the hold and the light.
*/
class LevelMeter : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    LevelMeter() { setTooltip ("Output level L / R (dBFS). Red light: the output soft clipper is near its -1 dBFS ceiling. "
                               "Click to reset the peak hold and the light."); }

    void update (const float* left, const float* right, int numSamples, int framesPerSecond)
    {
        const float fall = fallDbPerSecond / (float) framesPerSecond;
        bool clipped = false;

        channels[0].update (left,  numSamples, fall, framesPerSecond * 3 / 2, clipped);
        channels[1].update (right, numSamples, fall, framesPerSecond * 3 / 2, clipped);

        const bool wasLit = clipFrames > 0;
        if (clipped)             clipFrames = framesPerSecond * 2;
        else if (clipFrames > 0) --clipFrames;

        // Repaint only when a bar, a peak line or the light moved.
        bool changed = (clipFrames > 0) != wasLit;
        for (size_t ch = 0; ch < channels.size(); ++ch)
        {
            changed = changed || ! channels[ch].sameAs (shown[ch]);
            shown[ch] = channels[ch];
        }
        if (changed)
            repaint();
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        clipFrames = 0;
        for (auto& c : channels)
            c.heldPeak = minDb;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto layout = meterLayout();
        const auto yFor = [&layout] (float db)
        {
            return juce::jmap (juce::jlimit (minDb, maxDb, db), minDb, maxDb, layout.bars.getBottom(), layout.bars.getY());
        };

        // The inset, the unlit blocks, the captions and the 0 dBFS marks: drawn once, then copied.
        backdrop.paint (g, getLocalBounds(), [this, &layout, &yFor] (juce::Graphics& bg)
        {
            neon::drawPanel (bg, getLocalBounds().toFloat(), neon::cyan);
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < layout.segments; ++s)
                {
                    bg.setColour (segmentColour (layout, s).withAlpha (0.10f));
                    bg.fillRect (segmentRect (layout, ch, s));
                }

            bg.setFont (neon::font (12.0f));
            bg.setColour (neon::phosphorOf (neon::cyan).withAlpha (0.6f));
            auto captions = layout.captions;
            bg.drawText ("L", captions.removeFromLeft (captions.getWidth() * 0.5f), juce::Justification::centred);
            bg.drawText ("R", captions, juce::Justification::centred);

            // 0 dBFS: short red marks either side of each column, clear of the blocks
            const float y0 = std::round (yFor (0.0f));
            bg.setColour (neon::red.withAlpha (0.7f));
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto column = segmentRect (layout, ch, 0);
                bg.fillRect (juce::Rectangle<float> (column.getX() - 6.0f, y0, 4.0f, 1.0f));
                bg.fillRect (juce::Rectangle<float> (column.getRight() + 2.0f, y0, 4.0f, 1.0f));
            }

            bg.setColour (neon::red.withAlpha (0.16f));
            bg.fillRect (layout.led);
        });

        // Clip light: a red block, lit with a glow while the soft clipper works hard
        if (clipFrames > 0)
        {
            g.setColour (neon::red.withAlpha (0.3f));
            g.fillRect (layout.led.expanded (2.0f, 1.5f));
            g.setColour (neon::red);
            g.fillRect (layout.led);
            g.setColour (neon::phosphorOf (neon::red).withAlpha (0.8f));
            g.fillRect (layout.led.reduced (layout.led.getWidth() * 0.2f, layout.led.getHeight() * 0.3f));
        }

        // The blocks: lit up to the RMS level, half lit on up to the peak
        for (size_t ch = 0; ch < channels.size(); ++ch)
        {
            const auto& c = channels[ch];
            for (int s = 0; s < layout.segments; ++s)
            {
                const float mid = minDb + (maxDb - minDb) * ((float) s + 0.5f) / (float) layout.segments;
                const float level = c.rmsLevel >= mid ? 1.0f : c.peakLevel >= mid ? 0.45f : 0.0f;
                if (level <= 0.0f)
                    continue;
                const auto block  = segmentRect (layout, (int) ch, s);
                const auto colour = segmentColour (layout, s);
                g.setColour (colour.withAlpha (0.22f * level));
                g.fillRect (block.expanded (1.5f, 1.0f));
                g.setColour (colour.withAlpha (level));
                g.fillRect (block);
            }

            // The held peak, exact: a thin amber line across the column
            if (c.heldPeak > minDb)
            {
                const auto column = segmentRect (layout, (int) ch, 0);
                const float y = yFor (c.heldPeak);
                g.setColour (neon::amber.withAlpha (0.3f));
                g.fillRect (column.getX() - 2.0f, y - 2.0f, column.getWidth() + 4.0f, 4.0f);
                g.setColour (neon::phosphorOf (neon::amber));
                g.fillRect (column.getX() - 1.0f, y - 0.75f, column.getWidth() + 2.0f, 1.5f);
            }
        }
    }

    void resized() override { backdrop.invalidate(); }

    const neon::crt::CachedPainter& getBackdrop() const noexcept { return backdrop; }

private:
    /** Where the clip light, the two block columns and the captions go. */
    struct Layout
    {
        juce::Rectangle<float> led, bars, captions;
        float barWidth = 0.0f, pitch = 1.0f;
        int segments = 1;
    };

    Layout meterLayout() const
    {
        Layout l;
        auto bounds = getLocalBounds().toFloat().reduced (4.0f);
        l.led = bounds.removeFromTop (10.0f).reduced (bounds.getWidth() * 0.18f, 0.0f);
        bounds.removeFromTop (4.0f);
        l.captions = bounds.removeFromBottom (15.0f);
        l.bars = bounds;
        l.barWidth = bounds.getWidth() * 0.32f;
        // Blocks about 7 units apart (5 lit, 2 dark), so each is a few dB of the 66 dB range.
        l.segments = juce::jmax (8, juce::roundToInt (bounds.getHeight() / 7.0f));
        l.pitch = bounds.getHeight() / (float) l.segments;
        return l;
    }

    static juce::Rectangle<float> segmentRect (const Layout& l, int channel, int segment)
    {
        const float centre = l.bars.getX() + l.bars.getWidth() * (channel == 0 ? 0.27f : 0.73f);
        const float gap = juce::jmin (2.0f, l.pitch * 0.3f);
        return { centre - l.barWidth * 0.5f, l.bars.getBottom() - l.pitch * (float) (segment + 1) + gap * 0.5f,
                 l.barWidth, l.pitch - gap };
    }

    /** Phosphor green, amber from -12 dBFS, red from -2 dBFS (where the clip light comes on). */
    static juce::Colour segmentColour (const Layout& l, int segment)
    {
        const float mid = minDb + (maxDb - minDb) * ((float) segment + 0.5f) / (float) l.segments;
        return mid >= redFromDb ? juce::Colour (0xffff7a7a) : mid >= amberFromDb ? juce::Colour (0xffffd28a) : juce::Colour (0xff8dffd0);
    }

    neon::crt::CachedPainter backdrop;

    static constexpr float minDb = -60.0f;
    static constexpr float maxDb = 6.0f;
    static constexpr float clipLightThreshold = 0.79432823f;   // -2 dBFS: the soft clipper is close to its ceiling
    static constexpr float redFromDb   = -2.0f;      // segments turn red where the clip light comes on ...
    static constexpr float amberFromDb = -12.0f;     // ... and amber from here
    static constexpr float fallDbPerSecond = 24.0f;  // bars and peaks fall this fast

    struct Channel
    {
        float rmsLevel = minDb, peakLevel = minDb, heldPeak = minDb;
        int   holdFrames = 0;

        bool sameAs (const Channel& other) const noexcept
        {
            return rmsLevel == other.rmsLevel && peakLevel == other.peakLevel && heldPeak == other.heldPeak;
        }

        void update (const float* samples, int numSamples, float fall, int holdLength, bool& clipped)
        {
            // Peak with SIMD; the sum of squares stays a plain loop: without fast-math the
            // compiler may not reorder a running sum into vector lanes (that would change
            // its rounding), and JUCE has no vector sum. A few hundred samples a frame is
            // nothing either way.
            float peak = 0.0f, sumSquares = 0.0f;
            if (numSamples > 0)
            {
                const auto extremes = juce::FloatVectorOperations::findMinAndMax (samples, numSamples);
                peak = juce::jmax (-extremes.getStart(), extremes.getEnd(), 0.0f);
            }
            for (int i = 0; i < numSamples; ++i)
                sumSquares += samples[i] * samples[i];
            clipped = clipped || peak >= clipLightThreshold;

            const float rms    = numSamples > 0 ? std::sqrt (sumSquares / (float) numSamples) : 0.0f;
            const float peakDb = juce::Decibels::gainToDecibels (peak, minDb);

            rmsLevel  = juce::jmax (juce::Decibels::gainToDecibels (rms, minDb), rmsLevel - fall);
            peakLevel = juce::jmax (peakDb, peakLevel - fall);

            if (peakDb >= heldPeak)  { heldPeak = peakDb; holdFrames = holdLength; }
            else if (holdFrames > 0) { --holdFrames; }
            else                     { heldPeak = juce::jmax (peakDb, heldPeak - fall); }
        }
    };

    std::array<Channel, 2> channels {}, shown {};
    int clipFrames = 0;
};
