// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "gui/NeonStyle.h"
#include "gui/SignalHistory.h"

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
