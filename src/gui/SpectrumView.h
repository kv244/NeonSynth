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
    SpectrumView();

    void update (const SignalHistory& history, double newSampleRate, float newCutoffHz, bool newFilterOn,
                 int newMode = 0, float newResonance = 0.0f);

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
    static double foldedFrequency (double hz, double sampleRate) noexcept;

    /**
        The aliased partials of a note: every harmonic k with k f0 above Nyquist that the wave has,
        folded back (f = |k f0 - m fs| in 0 .. fs/2), lowest k first, at most maxAliasMarks.
        Which k and how loud: CUSTOM, k up to HARMONICS with a level above 0, at that level; otherwise
        WAVEFORM's series: saw every k at 1/k, square every odd k at 1/k, triangle every odd k at
        1/k squared, sine none. Levels are relative to the loudest harmonic below Nyquist.
    */
    static std::vector<AliasMark> aliasMarks (const AliasSource& source, double sampleRate);

    /** The marks to draw (none while source.shown is false). Message thread; repaints if they changed. */
    void setAliasSource (const AliasSource& source);

    const std::vector<AliasMark>& getAliasMarks() const noexcept   { return aliases; }

    void resized() override;

    void paint (juce::Graphics& g) override;

    const neon::crt::CachedPainter& getBackdrop() const noexcept { return backdrop; }

private:
    /**
        v1.12.0: the aliasing marks, a small red diamond on a short stem at each folded harmonic,
        as high as its series puts it below the loudest peak now on screen (so a mark sits on the
        aliased peak it explains), and the legend.
    */
    void paintAliasMarks (juce::Graphics& g, juce::Rectangle<float> area) const;

    juce::Rectangle<float> plotArea() const;

    /** The outline for the current levels, built once per new frame (paint only strokes it). */
    void buildOutline();

    static constexpr int   baseFftOrder = 12;   // 4096 points at 48 kHz
    static constexpr int   maxFftSize   = 1 << (baseFftOrder + 2);
    static constexpr int   numPoints = 160;

    void setFftOrder (int order);
    static constexpr float minDb     = -96.0f;
    static constexpr float minHz     = 20.0f;
    static constexpr double topHz    = 20000.0;   // the axis ends here, or at Nyquist below 40 kHz
    static constexpr float maxResponseDb = 30.0f; // the filter curve's ceiling (self-oscillation has no finite peak)
    static constexpr double placeholderSampleRate = 44100.0;   // until update() gives a rate (or when it gives none)

    double maxHz() const noexcept { return juce::jmin (topHz, 0.5 * sampleRate); }

    double frequencyAt (double proportion) const noexcept;

    float xFor (juce::Rectangle<float> area, float hz) const noexcept;

    static float yFor (juce::Rectangle<float> area, float db) noexcept;

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
