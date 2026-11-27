// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "dsp/CytomicSvf.h"
#include "dsp/Oscillator.h"
#include "dsp/Wavetable.h"
#include <vector>
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

    v1.12.0: with NAIVE on and the Oscillator playing, red marks show where the
    harmonics above Nyquist of the lowest sounding note land once they fold back
    (see aliasMarks()), at the height their series gives them relative to the
    loudest peak on screen, so they sit on the aliased peaks the analyser shows.
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
        const double rate = newSampleRate > 0.0 ? newSampleRate : placeholderSampleRate;
        if (! juce::approximatelyEqual (rate, sampleRate))
            backdrop.invalidate();   // the frequency axis ends at the new Nyquist
        sampleRate = rate;
        cutoffHz   = newCutoffHz;
        filterOn   = newFilterOn;
        filterMode = juce::jlimit ((int) CytomicSvf::Mode::lowpass, (int) CytomicSvf::Mode::notch, newMode);
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

    //==========================================================================
    // v1.12.0: the aliasing marks

    /** What decides the aliasing marks: the editor fills one from the parameters and the voices each tick. */
    struct AliasSource
    {
        bool  shown = false;          ///< NAIVE on, the Oscillator source, and a note sounding
        bool  custom = false;         ///< CUSTOM: the harmonics below, else WAVEFORM's series
        int   waveform = PolyBlepOscillator::sine;
        int   harmonics = HarmonicSpectrum::numHarmonics;   ///< CUSTOM's count
        float fundamentalHz = 0.0f;   ///< the lowest sounding note
        std::array<float, HarmonicSpectrum::numHarmonics> levels {};   ///< CUSTOM's levels

        bool operator== (const AliasSource& o) const noexcept
        {
            return shown == o.shown && custom == o.custom && waveform == o.waveform && harmonics == o.harmonics
                && fundamentalHz == o.fundamentalHz && levels == o.levels;
        }
    };

    /** One harmonic above Nyquist, where it lands after folding, and its level relative to the loudest partial heard. */
    struct AliasMark
    {
        int    harmonic = 0;
        double hz = 0.0;
        float  relativeLevel = 0.0f;   ///< 0..1 (linear), against the loudest harmonic below Nyquist
    };

    static constexpr int maxAliasMarks = 48;   ///< the first 48 aliased partials: enough to see the pattern, few enough to read

    /** Where a frequency lands after sampling at `sampleRate`: folded into 0 .. sampleRate / 2. */
    static double foldedFrequency (double hz, double sampleRate) noexcept
    {
        double f = std::fmod (std::abs (hz), sampleRate);
        if (f > 0.5 * sampleRate)
            f = sampleRate - f;
        return f;
    }

    /**
        The aliased partials of a note: every harmonic k with k f0 above Nyquist that the wave has,
        folded back (f = |k f0 - m fs| in 0 .. fs/2), lowest k first, at most maxAliasMarks.
        Which k and how loud: CUSTOM, k up to HARMONICS with a level above 0, at that level; otherwise
        WAVEFORM's series: saw every k at 1/k, square every odd k at 1/k, triangle every odd k at
        1/k squared, sine none. Levels are relative to the loudest harmonic below Nyquist.
    */
    static std::vector<AliasMark> aliasMarks (const AliasSource& source, double sampleRate)
    {
        std::vector<AliasMark> marks;
        const double f0 = source.fundamentalHz, nyquist = 0.5 * sampleRate;
        if (! source.shown || f0 <= 0.0 || sampleRate <= 0.0)
            return marks;

        // The level of harmonic k in the wave's series (0 if the wave has none).
        const auto levelOf = [&source] (int k) -> double
        {
            if (source.custom)
                return k <= juce::jlimit (1, HarmonicSpectrum::numHarmonics, source.harmonics) ? (double) source.levels[(size_t) (k - 1)] : 0.0;
            switch (source.waveform)
            {
                case PolyBlepOscillator::sawtooth: return 1.0 / k;
                case PolyBlepOscillator::square:   return (k % 2) == 1 ? 1.0 / k : 0.0;
                case PolyBlepOscillator::triangle: return (k % 2) == 1 ? 1.0 / ((double) k * k) : 0.0;
                default:                           return 0.0;   // a sine has no harmonics to alias
            }
        };
        const int lastK = source.custom ? HarmonicSpectrum::numHarmonics
                                        : (int) std::ceil (nyquist / f0) + 2 * maxAliasMarks + 1;   // odd series reach 48 within this

        double loudest = 0.0;   // among the harmonics heard where they belong
        for (int k = 1; k <= lastK && k * f0 <= nyquist; ++k)
            loudest = juce::jmax (loudest, levelOf (k));
        if (loudest <= 0.0)     // nothing below Nyquist (or all of it at 0): compare with the loudest overall
            for (int k = 1; k <= lastK; ++k)
                loudest = juce::jmax (loudest, levelOf (k));
        if (loudest <= 0.0)
            return marks;

        for (int k = 1; k <= lastK && (int) marks.size() < maxAliasMarks; ++k)
            if (const double level = levelOf (k); k * f0 > nyquist && level > 0.0)
                marks.push_back ({ k, foldedFrequency (k * f0, sampleRate), (float) (level / loudest) });
        return marks;
    }

    /** The marks to draw (none while source.shown is false). Message thread; repaints if they changed. */
    void setAliasSource (const AliasSource& source)
    {
        if (source == aliasSource && sampleRate == aliasRate)
            return;
        aliasSource = source;
        aliasRate = sampleRate;
        auto marks = aliasMarks (source, sampleRate);
        const bool changed = marks.size() != aliases.size()
                          || ! std::equal (marks.begin(), marks.end(), aliases.begin(),
                                           [] (const AliasMark& a, const AliasMark& b) { return a.harmonic == b.harmonic && a.hz == b.hz && a.relativeLevel == b.relativeLevel; });
        aliases = std::move (marks);
        if (changed)
            repaint();
    }

    const std::vector<AliasMark>& getAliasMarks() const noexcept   { return aliases; }

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

        paintAliasMarks (g, area);

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
                // rings on its own); maxResponseDb (30 dB) stands in for it.
                const double magnitude = svf.magnitudeAt (frequencyAt ((double) i / numPoints), (CytomicSvf::Mode) filterMode);
                db = juce::jmin (juce::Decibels::gainToDecibels ((float) magnitude, minDb), maxResponseDb);
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
    /**
        v1.12.0: the aliasing marks, a small red diamond on a short stem at each folded harmonic,
        as high as its series puts it below the loudest peak now on screen (so a mark sits on the
        aliased peak it explains), and the legend.
    */
    void paintAliasMarks (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        if (aliases.empty())
            return;
        const float peakDb = *std::max_element (levels.begin(), levels.end());
        constexpr float silentDb = -90.0f;   // nothing on screen to measure against: no marks
        if (peakDb <= silentDb)
            return;

        bool drawn = false;
        for (const auto& mark : aliases)
        {
            if (mark.hz < minHz || mark.hz > maxHz())
                continue;   // off the axis
            const float db = peakDb + juce::Decibels::gainToDecibels (mark.relativeLevel, minDb);
            if (db <= minDb)
                continue;
            const float x = std::round (xFor (area, (float) mark.hz)) + 0.5f;
            const float y = yFor (area, db);
            g.setColour (neon::red.withAlpha (0.35f));
            g.fillRect (juce::Rectangle<float> (x - 0.5f, y, 1.0f, juce::jmin (14.0f, area.getBottom() - y)));   // the stem, down from the mark
            juce::Path diamond;
            diamond.addQuadrilateral (x, y - 4.0f, x + 3.5f, y, x, y + 4.0f, x - 3.5f, y);
            g.setColour (neon::red.withAlpha (0.25f));
            g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ x, y }));   // halo
            g.setColour (neon::red);
            g.fillPath (diamond);
            drawn = true;
        }
        if (drawn)
            neon::crt::drawGlowText (g, "red: harmonics folded back from above Nyquist",
                                     juce::Rectangle<float> (area.getX() + 40.0f, area.getY() + 16.0f, area.getWidth() - 44.0f, 14.0f),   // under the filter's 0 dB line and label
                                     neon::red, neon::font (12.0f), juce::Justification::left, 0.3f);
    }

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
    static constexpr double topHz    = 20000.0;   // the axis ends here, or at Nyquist below 40 kHz
    static constexpr float maxResponseDb = 30.0f; // the filter curve's ceiling (self-oscillation has no finite peak)
    static constexpr double placeholderSampleRate = 44100.0;   // until update() gives a rate (or when it gives none)

    double maxHz() const noexcept { return juce::jmin (topHz, 0.5 * sampleRate); }

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
    std::array<float, 2 * static_cast<std::size_t> (maxFftSize)> fftData {};
    std::array<float, maxFftSize> window {};
    float windowSum = 0.0f;
    std::array<float, numPoints> levels {};

    double sampleRate = placeholderSampleRate;
    float  cutoffHz = 2000.0f;
    bool   filterOn = true;
    int    filterMode = 0;          // CytomicSvf::Mode
    float  resonance = 0.0f;        // 0..1

    AliasSource aliasSource;        // v1.12.0: what the marks were made from
    double aliasRate = 0.0;
    std::vector<AliasMark> aliases;

    juce::Path outline;             // the frame on screen
    neon::Afterglow afterglow;      // the frames before it, fading
    neon::crt::CachedPainter backdrop;
};
