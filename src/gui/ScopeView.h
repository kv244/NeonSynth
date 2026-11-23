// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "gui/NeonStyle.h"
#include "gui/SignalHistory.h"

//==============================================================================
/**
    Triggered oscilloscope.

    Starts each frame on a rising zero crossing so the trace stands still. When
    notes are held it locks to the lowest note's period (the synth knows it
    exactly) and shows two cycles; otherwise it free-runs over ~20 ms.
*/
class OscilloscopeView : public juce::Component
{
public:
    void update (const SignalHistory& history, double sampleRate, float lowestNoteHz)
    {
        // Window lengths scale with the sample rate, so the scope shows the same span of time at every rate.
        const int factor = displayRateFactor (sampleRate);
        const int searchLength = baseSearchLength * factor, maxDisplay = baseMaxDisplay * factor;
        history.copyLatest (recent.data(), searchLength);

        const double period = (lowestNoteHz > 0.0f && sampleRate > 0.0) ? sampleRate / lowestNoteHz : 0.0;
        displayLength = period > 0.0 ? juce::jlimit (minDisplayLength, maxDisplay, (int) std::lround (periodsShown * period)) : baseFreeRunLength * factor;

        // Walk back from the newest possible start to the nearest rising zero crossing.
        const int latestStart = searchLength - displayLength;
        const int span        = period > 0.0 ? (int) std::ceil (period) + 2 : baseFreeRunLength * factor;
        int start = latestStart;

        for (int i = latestStart; i > juce::jmax (1, latestStart - span); --i)
        {
            if (recent[(size_t) i - 1] < 0.0f && recent[(size_t) i] >= 0.0f)
            {
                start = i;
                break;
            }
        }

        // JUCE's FloatVectorOperations use SSE / NEON for these whole-array jobs: a few
        // times faster than the element loops they replace, for one line each. (GUI
        // thread, so this is tidiness more than a needed speed-up.)
        juce::FloatVectorOperations::copy (display.data(), recent.data() + start, displayLength);
        const auto extremes = juce::FloatVectorOperations::findMinAndMax (display.data(), displayLength);
        const float peak = juce::jmax (-extremes.getStart(), extremes.getEnd(), 0.0f);

        // Auto-range with a slow release so the trace keeps a steady size.
        const float newRange = juce::jmax (peak, range * 0.92f, 0.02f);

        // Silence that is already on screen needs no repaint (the flat line and
        // its label don't change while the range shrinks towards its floor),
        // once the afterglow of what was there before has faded.
        const double now = juce::Time::getMillisecondCounterHiRes();
        const bool glowing = afterglow.advance (now);
        const bool silent = peak == 0.0f;
        if (silent && shownSilent && lowestNoteHz == lockHz)
        {
            range = newRange;
            if (glowing)
                repaint();
            return;
        }

        range       = newRange;
        lockHz      = lowestNoteHz;
        shownSilent = silent;
        afterglow.push (trace, now);   // the trace leaving the screen fades out behind the new one
        buildTrace();
        repaint();
    }

    void resized() override
    {
        afterglow.clear();   // the old traces were drawn for the old size
        buildTrace();
    }

    void paint (juce::Graphics& g) override
    {
        // The inset and graticule never change: drawn once, then copied.
        backdrop.paint (g, getLocalBounds(), [this] (juce::Graphics& bg)
        {
            const auto bounds = getLocalBounds().toFloat();
            neon::drawPanel (bg, bounds, neon::cyan);
            neon::drawGraticule (bg, bounds.reduced (traceInset), neon::cyan, 8, 4);
        });

        auto area = traceArea();
        afterglow.paint (g, neon::cyan, 2.0f);
        neon::strokeGlow (g, trace, neon::cyan);

        juce::String label ("free run");
        if (lockHz > 0.0f)
        {
            const int note = juce::roundToInt (12.0f * std::log2 (lockHz / 440.0f)) + 69;
            label = juce::MidiMessage::getMidiNoteName (note, true, true, 4) + "  " + neon::formatHz (lockHz);
        }
        neon::crt::drawGlowText (g, label, area.removeFromTop (16.0f), neon::phosphorOf (neon::cyan).withAlpha (0.8f),
                                 neon::font (12.0f), juce::Justification::topRight, 0.3f);
    }

    /** How many times the graticule has been drawn (it should be once per size and scale). */
    const neon::crt::CachedPainter& getBackdrop() const noexcept { return backdrop; }

private:
    juce::Rectangle<float> traceArea() const { return getLocalBounds().toFloat().reduced (traceInset); }

    /** The trace for the samples in `display`, built once per new frame (paint only strokes it). */
    void buildTrace()
    {
        trace.clear();
        const auto area = traceArea();
        if (area.isEmpty())
            return;

        const float midY   = area.getCentreY();
        const float yScale = area.getHeight() * 0.45f / range;
        const int   columns = juce::jmax (2, juce::roundToInt (area.getWidth()));

        if (displayLength <= 2 * columns)
        {
            for (int i = 0; i < displayLength; ++i)
            {
                const float x = area.getX() + area.getWidth() * (float) i / (float) (displayLength - 1);
                const float y = midY - display[(size_t) i] * yScale;
                if (i == 0) trace.startNewSubPath (x, y);
                else        trace.lineTo (x, y);
            }
        }
        else
        {
            // More samples than pixels: one min/max pair per pixel column keeps
            // every peak while stroking at most 2 points per column (the glow
            // strokes the path three times, so this is where the GUI time goes).
            for (int c = 0; c < columns; ++c)
            {
                const int first = c * displayLength / columns;
                const int last  = juce::jmax (first + 1, (c + 1) * displayLength / columns);
                const auto column = juce::FloatVectorOperations::findMinAndMax (display.data() + first, last - first);   // SIMD min/max
                const float lo = column.getStart(), hi = column.getEnd();
                const float x = area.getX() + area.getWidth() * (float) c / (float) (columns - 1);
                if (c == 0) trace.startNewSubPath (x, midY - hi * yScale);
                else        trace.lineTo (x, midY - hi * yScale);
                if (lo != hi)
                    trace.lineTo (x, midY - lo * yScale);
            }
        }
    }

    static constexpr int baseSearchLength = 4096;   // at 48 kHz; times displayRateFactor()
    static constexpr int baseMaxDisplay   = 2048;
    static constexpr int baseFreeRunLength = 1024;   // shown with no note to lock to (and at first), times displayRateFactor()
    static constexpr int minDisplayLength  = 128;    // the shortest trace, for the highest notes
    static constexpr double periodsShown   = 2.0;    // a locked trace shows two cycles of the lowest note
    static constexpr float traceInset      = 8.0f;   // the graticule and trace sit this far inside the panel

    std::array<float, static_cast<std::size_t> (baseSearchLength) * maxDisplayRateFactor> recent {};
    std::array<float, static_cast<std::size_t> (baseMaxDisplay) * maxDisplayRateFactor>   display {};
    int   displayLength = baseFreeRunLength;
    float range = 0.25f;
    float lockHz = 0.0f;
    bool  shownSilent = false;   // the last repaint showed silence
    juce::Path trace;            // the frame on screen
    neon::Afterglow afterglow;   // the frames before it, fading
    neon::crt::CachedPainter backdrop;
};
