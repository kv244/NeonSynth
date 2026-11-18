// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "NeonSynthProcessor.h"
#include "CrtTheme.h"
#include <tuple>
#include <deque>

/*
    GUI-side visualisers for Neon Synth.

    Everything here runs on the message thread. Audio reaches it only through
    the processor's lock-free scope FIFO and the per-voice atomics, both read
    from the editor's timer - nothing in this file touches the audio thread.

    Repaint economy: the timer calls update() 30 times a second, but each view
    repaints only when what it would draw has changed. When the synth is silent
    and the displays have settled, nothing repaints at all.
*/

namespace neon
{
    inline const juce::Colour cyan    { 0xff19e6ff };
    inline const juce::Colour magenta { 0xffff4fd8 };
    inline const juce::Colour amber   { 0xffffb319 };
    inline const juce::Colour green   { 0xff5cff6a };   // modulation: the LFO, pitch bend and the mod wheel (v1.9.0)
    inline const juce::Colour red     { 0xffff3355 };
    inline const juce::Colour panel   { 0xff0b0e17 };   // the dark inset behind a view

    inline const juce::Colour background  { 0xff0a0c13 };   // the window
    inline const juce::Colour section     { 0xff121624 };   // a panel of controls
    inline const juce::Colour sectionEdge { 0xff232a3d };
    inline const juce::Colour text        { 0xffe6ebf5 };
    inline const juce::Colour muted       { 0xffa3adc2 };

    /** The small-text font (v1.10.0: IBM Plex Mono, embedded; SemiBold for "bold"). */
    inline juce::Font font (float height, bool bold = false)
    {
        return crt::labelFont (height, bold);
    }

    /** The glass behind a display (v1.10.0): darker than the tube around it, so a view reads as
        a screen set into the panel. */
    inline const juce::Colour displayInset { 0xff020906 };

    /** A display's inset (v1.10.0 CRT look): the dark glass, square corners, a thin 1 px edge in
        the view's hue and a faint glow just inside it. Flat fills and strokes only: cheap enough
        for the views that repaint every frame. */
    inline void drawPanel (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour edge)
    {
        g.setColour (displayInset);
        g.fillRect (r);
        g.setColour (edge.withAlpha (0.05f));
        g.drawRect (r.reduced (2.0f), 2.0f);
        g.setColour (edge.withAlpha (0.10f));
        g.drawRect (r.reduced (1.0f), 1.0f);
        g.setColour (edge.withAlpha (0.55f));
        g.drawRect (r, 1.0f);
    }

    /** The colour a trace's core burns at: its hue pushed most of the way to white, as phosphor
        looks where the beam is brightest (keeps the colour's alpha, so dimmed traces stay dim). */
    inline juce::Colour phosphorOf (juce::Colour hue)
    {
        return hue.interpolatedWith (juce::Colours::white.withAlpha (hue.getFloatAlpha()), 0.62f);
    }

    /** A scope graticule: faint division lines, a brighter centre cross with minor ticks (five
        per division) like a real scope's. Static: callers draw it into a crt::CachedPainter. */
    inline void drawGraticule (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour hue, int columns, int rows)
    {
        g.setColour (hue.withAlpha (0.09f));
        for (int i = 1; i < columns; ++i)
            g.fillRect (juce::Rectangle<float> (std::round (area.getX() + area.getWidth() * (float) i / (float) columns), area.getY(), 1.0f, area.getHeight()));
        for (int i = 1; i < rows; ++i)
            g.fillRect (juce::Rectangle<float> (area.getX(), std::round (area.getY() + area.getHeight() * (float) i / (float) rows), area.getWidth(), 1.0f));

        const float cx = std::round (area.getCentreX()), cy = std::round (area.getCentreY());
        g.setColour (hue.withAlpha (0.18f));
        g.fillRect (juce::Rectangle<float> (cx, area.getY(), 1.0f, area.getHeight()));
        g.fillRect (juce::Rectangle<float> (area.getX(), cy, area.getWidth(), 1.0f));

        g.setColour (hue.withAlpha (0.26f));
        const float dx = area.getWidth() / (float) (columns * 5), dy = area.getHeight() / (float) (rows * 5);
        for (int i = 1; i < columns * 5; ++i)
            g.fillRect (juce::Rectangle<float> (std::round (area.getX() + dx * (float) i), cy - 2.0f, 1.0f, 5.0f));
        for (int i = 1; i < rows * 5; ++i)
            g.fillRect (juce::Rectangle<float> (cx - 2.0f, std::round (area.getY() + dy * (float) i), 5.0f, 1.0f));
    }

    /** Strokes a path as a phosphor trace: two wide faint passes in the hue (the halo) under a
        thin core burnt nearly white. Three strokes, no blur. */
    inline void strokeGlow (juce::Graphics& g, const juce::Path& p, juce::Colour c, float width = 1.4f)
    {
        const auto stroke = [&] (juce::Colour colour, float w)
        {
            g.setColour (colour);
            g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };
        stroke (c.withMultipliedAlpha (0.09f), width * 5.0f);
        stroke (c.withMultipliedAlpha (0.30f), width * 2.4f);
        stroke (phosphorOf (c), width);
    }

    /** A phosphor dot (a voice on its envelope, the LFO's position): a halo in the hue, a near-white core. */
    inline void drawPhosphorDot (juce::Graphics& g, juce::Point<float> centre, juce::Colour hue, float coreSize, float alpha = 1.0f)
    {
        g.setColour (hue.withMultipliedAlpha (0.18f * alpha));
        g.fillEllipse (juce::Rectangle<float> (coreSize * 2.8f, coreSize * 2.8f).withCentre (centre));
        g.setColour (hue.withMultipliedAlpha (0.40f * alpha));
        g.fillEllipse (juce::Rectangle<float> (coreSize * 1.7f, coreSize * 1.7f).withCentre (centre));
        g.setColour (phosphorOf (hue).withMultipliedAlpha (alpha));
        g.fillEllipse (juce::Rectangle<float> (coreSize, coreSize).withCentre (centre));
    }

    /** A vertical marker line with a glow (sample start, playheads, the cutoff): a faint wider bar under it. */
    inline void drawGlowLine (juce::Graphics& g, float x, float top, float bottom, juce::Colour colour, float width = 2.0f)
    {
        g.setColour (colour.withMultipliedAlpha (0.22f));
        g.fillRect (juce::Rectangle<float> (x - width * 1.5f, top, width * 3.0f, bottom - top));
        g.setColour (colour);
        g.fillRect (juce::Rectangle<float> (x - width * 0.5f, top, width, bottom - top));
    }

    //==========================================================================
    /**
        The phosphor's afterglow (v1.10.0) for the scope and the spectrum: the last few traces,
        each fading out over crt::afterglowSeconds after it left the screen. Kept as paths, not
        as an image, so it costs one thin stroke per remembered trace and stays sharp at every
        zoom. Once the last one has faded, advance() says so and the view can stop repainting.
    */
    class Afterglow
    {
    public:
        /** Keeps a trace that is leaving the screen (nowMs: juce::Time::getMillisecondCounterHiRes()). */
        void push (const juce::Path& trace, double nowMs)
        {
            if (trace.isEmpty())
                return;
            if (ghosts.size() >= maxGhosts)
                ghosts.pop_back();
            ghosts.push_front ({ trace, nowMs });
        }

        /** Drops the traces that have faded. True if any was still glowing: the view must
            repaint once more to fade it further (or to clear the last one). */
        bool advance (double nowMs)
        {
            const bool glowing = ! ghosts.empty();
            while (! ghosts.empty() && nowMs - ghosts.back().bornMs >= lifetimeMs)
                ghosts.pop_back();
            return glowing;
        }

        void clear() { ghosts.clear(); }

        /** Strokes the remembered traces, oldest faintest. */
        void paint (juce::Graphics& g, juce::Colour hue, float width) const
        {
            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            for (auto it = ghosts.rbegin(); it != ghosts.rend(); ++it)
            {
                const float left = 1.0f - (float) ((nowMs - it->bornMs) / lifetimeMs);
                if (left <= 0.0f)
                    continue;
                g.setColour (hue.withMultipliedAlpha (0.45f * left * left));
                g.strokePath (it->trace, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }

    private:
        struct Ghost { juce::Path trace; double bornMs = 0.0; };
        static constexpr size_t maxGhosts = 6;   // 0.15 s at the editor's 30 Hz is under five frames
        static constexpr double lifetimeMs = 1000.0 * (double) crt::afterglowSeconds;
        std::deque<Ghost> ghosts;
    };

    inline juce::String formatTime (float seconds)
    {
        return seconds < 1.0f ? juce::String (juce::roundToInt (seconds * 1000.0f)) + " ms"
                              : juce::String (seconds, 2) + " s";
    }

    inline juce::String formatHz (float hz)
    {
        return hz < 1000.0f ? juce::String (juce::roundToInt (hz)) + " Hz"
                            : juce::String (hz / 1000.0f, hz < 10000.0f ? 2 : 1) + " kHz";
    }
}

//==============================================================================
/**
    How much longer the displays' windows are at this sample rate than at 48 kHz:
    1 up to 48 kHz, 2 at 88.2 and 96 kHz, 4 at 176.4 and 192 kHz. Scaling by it
    keeps the scope showing the same time span, and the spectrum the same
    frequency resolution, at every sample rate.
*/
inline int displayRateFactor (double sampleRate) noexcept
{
    return sampleRate > 120000.0 ? 4 : sampleRate > 60000.0 ? 2 : 1;
}

/** Rolling copy of the most recent output samples, filled from the scope FIFO. */
class SignalHistory
{
public:
    static constexpr int size = 1 << 15;   // power of two; holds the largest FFT (2^14, at 192 kHz) twice over

    void push (const float* samples, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            data[(size_t) writePos] = samples[i];
            writePos = (writePos + 1) & (size - 1);
        }
    }

    /** Copies the newest n samples (n <= size) into dest, oldest first. */
    void copyLatest (float* dest, int n) const noexcept
    {
        const int start = (writePos - n) & (size - 1);
        for (int i = 0; i < n; ++i)
            dest[i] = data[(size_t) ((start + i) & (size - 1))];
    }

private:
    std::array<float, size> data {};
    int writePos = 0;
};

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
        displayLength = period > 0.0 ? juce::jlimit (128, maxDisplay, (int) std::lround (2.0 * period)) : 1024 * factor;

        // Walk back from the newest possible start to the nearest rising zero crossing.
        const int latestStart = searchLength - displayLength;
        const int span        = period > 0.0 ? (int) std::ceil (period) + 2 : 1024 * factor;
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
            neon::drawGraticule (bg, bounds.reduced (8.0f), neon::cyan, 8, 4);
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
    juce::Rectangle<float> traceArea() const { return getLocalBounds().toFloat().reduced (8.0f); }

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

    std::array<float, baseSearchLength * 4> recent {};
    std::array<float, baseMaxDisplay * 4>   display {};
    int   displayLength = 1024;
    float range = 0.25f;
    float lockHz = 0.0f;
    bool  shownSilent = false;   // the last repaint showed silence
    juce::Path trace;            // the frame on screen
    neon::Afterglow afterglow;   // the frames before it, fading
    neon::crt::CachedPainter backdrop;
};

//==============================================================================
/**
    Spectrum analyser (log frequency axis; a 4096-point FFT at 48 kHz, scaled
    with the sample rate to keep the same resolution, up to 16384 points at
    192 kHz) with the filter's response drawn on top. The filter curve is
    computed from the parameters (cutoff, mode, resonance) with the voice
    filter's own coefficients (CytomicSvf::magnitudeAt), so it moves the
    instant a knob does and shows the response the audio uses, the
    resonant peak included. Drive and the resonance limiter are not linear,
    so they have no curve; the analyser above shows what they do.
*/
class SpectrumView : public juce::Component
{
public:
    SpectrumView()
    {
        setFftOrder (baseFftOrder);
        levels.fill (minDb);
    }

    void update (const SignalHistory& history, double newSampleRate, float newCutoffHz, bool newFilterOn,
                 int newMode = 0, float newResonance = 0.0f)
    {
        const bool filterChanged = newCutoffHz != cutoffHz || newFilterOn != filterOn || newSampleRate != sampleRate
                                || newMode != filterMode || newResonance != resonance;
        const double rate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        if (! juce::approximatelyEqual (rate, sampleRate))
            backdrop.invalidate();   // the frequency axis ends at the new Nyquist
        sampleRate = rate;
        cutoffHz   = newCutoffHz;
        filterOn   = newFilterOn;
        filterMode = juce::jlimit (0, 3, newMode);
        resonance  = newResonance;

        const int order = baseFftOrder + (displayRateFactor (sampleRate) == 4 ? 2 : displayRateFactor (sampleRate) == 2 ? 1 : 0);
        if (order != fftOrder)
            setFftOrder (order);   // message thread: rebuilding the window here is fine
        const int fftSize = 1 << fftOrder;

        history.copyLatest (fftData.data(), fftSize);
        juce::FloatVectorOperations::multiply (fftData.data(), window.data(), fftSize);   // Hann window, SIMD
        std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);

        ffts[(size_t) (fftOrder - baseFftOrder)].performFrequencyOnlyForwardTransform (fftData.data());

        const double binHz = sampleRate / fftSize;
        const float  norm  = 2.0f / windowSum;   // full-scale sine -> 0 dBFS
        constexpr float fallPerFrame = 1.5f;     // dB, gives peaks a gentle decay
        bool levelsChanged = false;

        for (int i = 0; i < numPoints; ++i)
        {
            const double lo = frequencyAt ((double) i / numPoints) / binHz;
            const double hi = frequencyAt ((double) (i + 1) / numPoints) / binHz;
            float magnitude = 0.0f;

            if (hi - lo < 1.0)   // narrower than a bin: interpolate at the band centre
            {
                const double centre = 0.5 * (lo + hi);
                const int    k      = juce::jlimit (0, fftSize / 2 - 2, (int) centre);
                const float  frac   = (float) (centre - k);
                magnitude = fftData[(size_t) k] + frac * (fftData[(size_t) k + 1] - fftData[(size_t) k]);
            }
            else                 // wider: take the loudest bin in the band
            {
                const int first = juce::jlimit (0, fftSize / 2 - 1, (int) std::ceil (lo));
                const int last  = juce::jlimit (0, fftSize / 2 - 1, (int) std::floor (hi));
                if (last >= first)
                    magnitude = juce::jmax (magnitude, juce::FloatVectorOperations::findMaximum (fftData.data() + first, last - first + 1));
            }

            const float db   = juce::Decibels::gainToDecibels (magnitude * norm, minDb);
            const float next = juce::jmax (db, levels[(size_t) i] - fallPerFrame, minDb);
            levelsChanged = levelsChanged || next != levels[(size_t) i];
            levels[(size_t) i] = next;
        }

        // Once silence has decayed to the floor, and the afterglow of the last
        // frames has faded, the picture stops changing.
        const double now = juce::Time::getMillisecondCounterHiRes();
        const bool glowing = afterglow.advance (now);
        if (levelsChanged)
        {
            afterglow.push (outline, now);
            buildOutline();
        }
        if (levelsChanged || filterChanged || glowing)
            repaint();
    }

    void resized() override
    {
        afterglow.clear();
        buildOutline();
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = plotArea();

        // Inset, grid and labels never change (but for the sample rate): drawn once, then copied.
        backdrop.paint (g, getLocalBounds(), [this, area] (juce::Graphics& bg)
        {
            neon::drawPanel (bg, getLocalBounds().toFloat(), neon::cyan);
            bg.setFont (neon::font (12.0f));
            for (const float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
            {
                if (f >= maxHz()) continue;
                const float x = std::round (xFor (area, f));
                bg.setColour (neon::cyan.withAlpha (0.09f));
                bg.fillRect (juce::Rectangle<float> (x, area.getY(), 1.0f, area.getHeight()));
                bg.setColour (neon::cyan.withAlpha (0.3f));
                bg.fillRect (juce::Rectangle<float> (x, area.getBottom() - 3.0f, 1.0f, 3.0f));   // a tick on the axis
                bg.setColour (neon::phosphorOf (neon::cyan).withAlpha (0.55f));
                bg.drawText (f < 1000.0f ? juce::String ((int) f) : juce::String ((int) (f / 1000.0f)) + "k",
                             juce::Rectangle<float> (x - 18.0f, area.getBottom() + 1.0f, 36.0f, 14.0f),
                             juce::Justification::centred);
            }
            for (const float db : { -24.0f, -48.0f, -72.0f })
            {
                const float y = std::round (yFor (area, db));
                bg.setColour (neon::cyan.withAlpha (0.09f));
                bg.fillRect (juce::Rectangle<float> (area.getX(), y, area.getWidth(), 1.0f));
                bg.setColour (neon::phosphorOf (neon::cyan).withAlpha (0.45f));
                bg.drawText (juce::String ((int) db), juce::Rectangle<float> (area.getX() + 2.0f, y - 15.0f, 36.0f, 14.0f),
                             juce::Justification::left);
            }
            bg.setColour (neon::cyan.withAlpha (0.22f));
            bg.fillRect (juce::Rectangle<float> (area.getX(), area.getBottom(), area.getWidth(), 1.0f));   // the axis
        });

        // Spectrum: a faint flat fill (no per-frame gradient) under the glowing outline and its afterglow
        if (! outline.isEmpty())
        {
            auto fill = outline;
            fill.lineTo (area.getRight(), area.getBottom());
            fill.lineTo (area.getX(), area.getBottom());
            fill.closeSubPath();
            g.setColour (neon::cyan.withAlpha (0.10f));
            g.fillPath (fill);
        }
        afterglow.paint (g, neon::cyan, 1.6f);
        neon::strokeGlow (g, outline, neon::cyan, 1.1f);

        // Filter response, from the same coefficients as the voices' filter
        const float fc = juce::jlimit (20.0f, 0.45f * (float) sampleRate, cutoffHz);
        CytomicSvf svf;
        svf.prepare (sampleRate);
        svf.setCoefficients (fc, CytomicSvf::resonanceToK (resonance));
        const auto  colour = filterOn ? neon::magenta : neon::magenta.withAlpha (0.3f);
        // The view's axis ends at 0 dB, and a resonant peak rises above it: the
        // curve is lowered by its peak's height above 0 dB (up to 30 dB), so the
        // peak stays in view. At 0% resonance nothing moves: the low-pass peaks at 0 dB.
        std::array<float, numPoints + 1> curve {};
        float peakDb = 0.0f;
        for (int i = 0; i <= numPoints; ++i)
        {
            float db = 0.0f;
            if (filterOn)
            {
                // At 98% and above the linear response has no finite peak (the filter
                // rings on its own); 30 dB stands in for it.
                const double magnitude = svf.magnitudeAt (frequencyAt ((double) i / numPoints), (CytomicSvf::Mode) filterMode);
                db = juce::jmin (juce::Decibels::gainToDecibels ((float) magnitude, minDb), 30.0f);
            }
            curve[(size_t) i] = db;
            peakDb = juce::jmax (peakDb, db);
        }
        juce::Path response;
        for (int i = 0; i <= numPoints; ++i)
        {
            const float x = area.getX() + area.getWidth() * (float) i / (float) numPoints;
            const float y = yFor (area, curve[(size_t) i] - peakDb);
            if (i == 0) response.startNewSubPath (x, y);
            else        response.lineTo (x, y);
        }
        neon::strokeGlow (g, response, colour, 1.2f);

        if (filterOn)
        {
            const float x = xFor (area, fc);
            neon::drawGlowLine (g, x, area.getY(), area.getBottom(), neon::magenta.withAlpha (0.4f), 1.0f);
            const bool labelLeft = x > area.getRight() - 80.0f;
            neon::crt::drawGlowText (g, neon::formatHz (fc),
                                     juce::Rectangle<float> (labelLeft ? x - 74.0f : x + 4.0f, area.getY(), 70.0f, 15.0f),
                                     neon::phosphorOf (neon::magenta), neon::font (12.0f),
                                     labelLeft ? juce::Justification::right : juce::Justification::left, 0.35f);
        }
    }

    const neon::crt::CachedPainter& getBackdrop() const noexcept { return backdrop; }

private:
    juce::Rectangle<float> plotArea() const
    {
        auto area = getLocalBounds().toFloat().reduced (8.0f);
        area.removeFromBottom (16.0f);   // room for frequency labels
        return area;
    }

    /** The outline for the current levels, built once per new frame (paint only strokes it). */
    void buildOutline()
    {
        outline.clear();
        const auto area = plotArea();
        if (area.isEmpty())
            return;
        for (int i = 0; i < numPoints; ++i)
        {
            const float x = area.getX() + area.getWidth() * ((float) i + 0.5f) / (float) numPoints;
            const float y = yFor (area, levels[(size_t) i]);
            if (i == 0) outline.startNewSubPath (x, y);
            else        outline.lineTo (x, y);
        }
    }

    static constexpr int   baseFftOrder = 12;   // 4096 points at 48 kHz
    static constexpr int   maxFftSize   = 1 << (baseFftOrder + 2);
    static constexpr int   numPoints = 160;

    void setFftOrder (int order)
    {
        fftOrder = order;
        const int size = 1 << order;
        windowSum = 0.0f;
        for (int n = 0; n < size; ++n)
        {
            window[(size_t) n] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) n / (float) (size - 1));
            windowSum += window[(size_t) n];
        }
    }
    static constexpr float minDb     = -96.0f;
    static constexpr float minHz     = 20.0f;

    double maxHz() const noexcept { return juce::jmin (20000.0, 0.5 * sampleRate); }

    double frequencyAt (double proportion) const noexcept
    {
        return minHz * std::pow (maxHz() / minHz, proportion);
    }

    float xFor (juce::Rectangle<float> area, float hz) const noexcept
    {
        const double p = std::log (hz / minHz) / std::log (maxHz() / minHz);
        return area.getX() + area.getWidth() * (float) p;
    }

    static float yFor (juce::Rectangle<float> area, float db) noexcept
    {
        return juce::jmap (juce::jlimit (minDb, 0.0f, db), minDb, 0.0f, area.getBottom(), area.getY());
    }

    std::array<juce::dsp::FFT, 3> ffts { juce::dsp::FFT (baseFftOrder), juce::dsp::FFT (baseFftOrder + 1), juce::dsp::FFT (baseFftOrder + 2) };
    int fftOrder = baseFftOrder;
    std::array<float, 2 * maxFftSize> fftData {};
    std::array<float, maxFftSize> window {};
    float windowSum = 0.0f;
    std::array<float, numPoints> levels {};

    double sampleRate = 44100.0;
    float  cutoffHz = 2000.0f;
    bool   filterOn = true;
    int    filterMode = 0;          // CytomicSvf::Mode
    float  resonance = 0.0f;        // 0..1

    juce::Path outline;             // the frame on screen
    neon::Afterglow afterglow;      // the frames before it, fading
    neon::crt::CachedPainter backdrop;
};

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
        const float fall = 24.0f / (float) framesPerSecond;   // 24 dB per second
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
        return mid >= -2.0f ? juce::Colour (0xffff7a7a) : mid >= -12.0f ? juce::Colour (0xffffd28a) : juce::Colour (0xff8dffd0);
    }

    neon::crt::CachedPainter backdrop;

    static constexpr float minDb = -60.0f;
    static constexpr float maxDb = 6.0f;
    static constexpr float clipLightThreshold = 0.79432823f;   // -2 dBFS: the soft clipper is close to its ceiling

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

//==============================================================================
/**
    ADSR shape drawn from the four knobs, with a dot for every sounding voice
    showing where it is in its envelope and how loud.

    Segment widths follow the square root of each time so a 10 s release does
    not squash a 10 ms attack into nothing.

    The editor has two: the amp envelope (amber, with the voice dots) and the
    filter envelope (magenta). The voices report where they are in the amp
    envelope only, so the filter's view draws the shape without dots.
*/
class AdsrView : public juce::Component,
                 public juce::SettableTooltipClient
{
public:
    using VoiceViews = std::array<NeonSynthProcessor::VoiceView, NeonSynthProcessor::numVoices>;

    /** The four points that can be dragged, in the order of attach()'s arguments. */
    enum Handle { attackHandle = 0, decayHandle, sustainHandle, releaseHandle, numHandles };

    AdsrView()
    {
        setTooltip ("Drag the points: A, D and R left or right for the times, S up or down for the level. Double-click a point to reset it.");
    }

    /** The parameters the points move. Without them the view only displays. */
    void attach (juce::RangedAudioParameter* attack, juce::RangedAudioParameter* decay,
                 juce::RangedAudioParameter* sustain, juce::RangedAudioParameter* release)
    {
        parameters = { attack, decay, sustain, release };
    }

    /** Where a point is drawn, in this component's coordinates. */
    juce::Point<float> handlePosition (int handle) const
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

    void mouseMove (const juce::MouseEvent& e) override   { setHover (handleAt (e.position)); }
    void mouseExit (const juce::MouseEvent&) override     { if (dragging < 0) setHover (-1); }

    void mouseDown (const juce::MouseEvent& e) override
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

    void mouseDrag (const juce::MouseEvent& e) override
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

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (auto* param = parameterFor (dragging))
            param->endChangeGesture();
        dragging = -1;
        setHover (handleAt (e.position));
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (auto* param = parameterFor (handleAt (e.position)))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->getDefaultValue());
            param->endChangeGesture();
            repaint();
        }
    }

    /** The colour of the shape, the points and the text (amber unless set). */
    void setAccent (juce::Colour colour)
    {
        accent = colour;
        repaint();
    }

    /** Reshapes the view from the four values; voices: where each voice is in this envelope, or
        nullptr for no dots (the filter envelope: the voices report the amp envelope only). */
    void update (float attack, float decay, float sustain, float release, const VoiceViews* voices)
    {
        constexpr float sustainWidth = 0.22f;
        const float a = std::sqrt (attack), d = std::sqrt (decay), r = std::sqrt (release);
        const float total = a + d + r;
        attackW  = juce::jmax (0.04f, (1.0f - sustainWidth) * a / total);
        decayW   = juce::jmax (0.04f, (1.0f - sustainWidth) * d / total);
        releaseW = juce::jmax (0.04f, (1.0f - sustainWidth) * r / total);
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

    void paint (juce::Graphics& g) override
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

private:
    struct Marker { float x = 0.0f, level = 0.0f; };

    static constexpr float handleSize = 13.0f;
    static constexpr float grabRadius = 14.0f;

    juce::Rectangle<float> plotArea() const   { return getLocalBounds().toFloat().reduced (10.0f, 9.0f); }

    juce::RangedAudioParameter* parameterFor (int handle) const
    {
        return handle >= 0 && handle < numHandles ? parameters[(size_t) handle] : nullptr;
    }

    /** The nearest point within reach of the pointer, or -1. */
    int handleAt (juce::Point<float> position) const
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

    void setHover (int handle)
    {
        if (std::exchange (hover, handle) != handle)
        {
            setMouseCursor (handle < 0 ? juce::MouseCursor::NormalCursor
                          : handle == sustainHandle ? juce::MouseCursor::UpDownResizeCursor
                          :                           juce::MouseCursor::LeftRightResizeCursor);
            repaint();
        }
    }

    std::array<juce::RangedAudioParameter*, numHandles> parameters {};
    int hover = -1, dragging = -1;
    float dragStartValue = 0.0f;

    float sustainStart() const noexcept { return attackW + decayW; }
    float releaseStart() const noexcept { return 1.0f - releaseW; }

    float attackW = 0.2f, decayW = 0.2f, releaseW = 0.38f, sustainLevel = 0.7f;
    float shownAttackW = -1.0f, shownDecayW = -1.0f, shownReleaseW = -1.0f, shownSustain = -1.0f;   // last repainted shape
    std::array<Marker, NeonSynthProcessor::numVoices> markers {};
    int numMarkers = 0;
    juce::Colour accent = neon::amber;
};

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
