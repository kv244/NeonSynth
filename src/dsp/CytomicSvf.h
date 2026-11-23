// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <juce_core/juce_core.h>
#include <cmath>
#include <complex>

/**
    @file CytomicSvf.h

    The voice filter from v1.8.0: Andrew Simper's (Cytomic) trapezoidal
    state-variable filter, with a resonance that reaches self-oscillation,
    amplitude-dependent damping that bounds it, and a drive control.

    Why JUCE's arithmetic, operation for operation: before v1.8.0 the voice ran
    juce::dsp::StateVariableTPTFilter. Default settings must still render
    bit-identical audio, so the linear path (processLinear, setCoefficients)
    repeats JUCE's update() and processSample() exactly: g from a double tan of
    the float cutoff, h from a double sum of the float products R2 * g and
    g * g, and the same float operations in the same order per sample. Any
    "equivalent" rewrite (k * g + g * g as one product, s1 = 2 * yBP - s1, ...)
    rounds differently. FilterTests checks the bits against JUCE.

    The limiter is in the damping, not a clip: the nonlinear step uses
    k + c * (A / L)^2 in place of k (a van der Pol oscillator's
    amplitude-dependent damping, bounded above by +2), with A the amplitude
    read from both integrator states, A^2 = (s1^2 + s2^2) / 4. At the cutoff
    the two states are equal in size and a quarter-cycle apart, so A does not
    ripple with the phase. Small signals see k; loud resonance is damped
    harder, smoothly, and stays bounded. Two simpler versions were measured
    and dropped (13 Nov 2026): clipping the band-pass state aliased at
    -15 dB and its ringing level grew 13 dB with the cutoff; damping from the
    previous band-pass sample alone blew up near a quarter of the sample
    rate, where every other sample of it is close to zero.
    With k < 0 (resonance near 100%) the filter self-oscillates and settles
    where the damping balances, with a peak of about 1.05 L, the same within
    1 dB from 100 Hz to 5 kHz, then falling towards Nyquist (-2.1 dB at
    10 kHz, -5.1 dB at 15 kHz at 48 kHz, against the level at 100 Hz;
    FilterTests prints the peaks: 1.056, 0.831 and 0.585 x L).
    Because the filter is linear and the damping term depends only on
    A / L, scaling the input and L together scales the output by the same
    factor. The voice therefore passes L = L0 * velocity level * envelope and
    the self-oscillation follows the amp envelope with no extra gain stage.

    Drive sits ahead of the filter (decided 13 Nov 2026, after measuring):
    drive inside the loop, as a lower ceiling, made inharmonic content at
    -25 to -40 dB that 2x oversampling did not remove, because the loop
    makes it, not the sampling. Ahead of the filter it is a plain soft clip,
    y = L * sat (D x / 4L) / sat (D / 4), D from 1 to 10 (0 to 20 dB):
    a signal at the ceiling L keeps its peak at every drive, quieter parts
    are raised (by up to 8.6 dB), so turning drive up makes the sound denser
    and brighter, not louder. Its harmonics then meet the resonance. It is
    anti-aliased by first-order ADAA (the antiderivative of
    u / sqrt (1 + u^2) is sqrt (1 + u^2)), as FOLD is, in double because the
    difference of two square roots near 1 loses float precision. ADAA delays
    by half a sample, so the first 5% of the knob crossfades from the dry
    signal. Same homogeneity: it scales with L, so it follows the envelope.

    Notch = low-pass + high-pass: the textbook (s^2 + 1) / (s^2 + k s + 1),
    zeros exactly at the cutoff whatever k is. The other form, x - k * bp,
    needs k and the undriven input, which the nonlinear path does not keep.

    Audio thread: no allocation, no locks. One channel per instance.
*/
class CytomicSvf
{
public:
    /** In the order of the Filter Mode parameter's choices. */
    enum class Mode { lowpass = 0, highpass = 1, bandpass = 2, notch = 3 };

    struct Outputs { float lp, bp, hp; };

    CytomicSvf() noexcept   { setCoefficients (cutoffHz, R2); }   // as JUCE's constructor

    static float select (const Outputs& o, Mode m) noexcept
    {
        switch (m)
        {
            case Mode::highpass:  return o.hp;
            case Mode::bandpass:  return o.bp;
            case Mode::notch:     return o.lp + o.hp;
            case Mode::lowpass:
            default:              return o.lp;
        }
    }

    /** Keeps the last cutoff and damping, as JUCE's prepare() does. */
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate;
        reset();
        setCoefficients (cutoffHz, R2);
    }

    void reset() noexcept   { s1 = 0.0f; s2 = 0.0f; lastInput = 0.0f; }

    /** The damping JUCE holds as R2 for setResonance (1 / sqrt2), the voice's Q before v1.8.0. */
    static float butterworthK() noexcept
    {
        return (float) (1.0 / (double) (1.0f / std::sqrt (2.0f)));
    }

    /** Resonance 0..1 -> damping k (JUCE's R2 = 1 / Q).
        0 gives butterworthK() exactly (0 * anything = 0). The cubic passes
        through Q 2 at 50%, Q 10 at 90% and k = 0 at 98%, and reaches
        k = -0.0275 at 100%: the filter self-oscillates from about 98% and the
        nonlinear damping holds the level. It falls monotonically over 0..1.
        The self-oscillation settles where the nonlinear damping balances the
        negative k, at a peak of about 1.05 L (FilterTests prints it). */
    static float resonanceToK (float resonance01) noexcept
    {
        const float r = juce::jlimit (0.0f, 1.0f, resonance01);
        return butterworthK() - r * (2.9504896f + r * (-2.979486f + r * 1.4707224f));
    }

    /** Drive 0..1 -> D, 1 (exactly) at 0 up to 10 (20 dB) at 1, exponential. */
    static float driveToGain (float drive01) noexcept
    {
        return std::pow (maxDriveGain, juce::jlimit (0.0f, 1.0f, drive01));
    }

    /** D at the top of the knob: 10, 20 dB. */
    static constexpr float maxDriveGain = 10.0f;

    /** The first 5% of the knob crossfades from the dry signal: blend = 20 x drive, up to 1. */
    static constexpr double dryBlendSlope = 20.0;

    /** Below this input step the ADAA quotient is 0 / 0 in the limit: the midpoint is used instead. */
    static constexpr double adaaMinStep = 1.0e-5;

    /** The drive stage's per-setting values, computed once when the knob moves. */
    struct Drive
    {
        double gain = 1.0;     // D / 4: the input scale per unit of ceiling
        double makeup = 1.0;   // 1 / sat (D / 4): keeps a signal at the ceiling at the ceiling
        double blend = 0.0;    // 0..1 over the first 5% of the knob, from the dry signal
    };

    static Drive makeDrive (float drive01) noexcept
    {
        const double d = juce::jlimit (0.0f, 1.0f, drive01);
        const double quarter = 0.25 * (double) driveToGain ((float) d);
        return { quarter, 1.0 / sat (quarter), std::min (1.0, dryBlendSlope * d) };
    }

    /** The drive stage, ahead of the filter: ceiling > 0. Call trackInput()
        instead on samples where drive is off, so it starts cleanly. */
    float drive (float x, float ceiling, const Drive& d) noexcept
    {
        const double scale = d.gain / (double) ceiling;
        const double u = scale * (double) x, uPrev = scale * (double) lastInput;
        const double du = u - uPrev;
        const double shaped = std::abs (du) > adaaMinStep ? (std::sqrt (1.0 + u * u) - std::sqrt (1.0 + uPrev * uPrev)) / du
                                                     : sat (0.5 * (u + uPrev));
        lastInput = x;
        return (float) ((double) x + d.blend * ((double) ceiling * d.makeup * shaped - (double) x));
    }

    void trackInput (float x) noexcept   { lastInput = x; }

    /** The nonlinear damping's strength c (see the file comment); with it,
        self-oscillation at 100% resonance peaks at about 1.05 L. */
    static constexpr float dampingCurve = 0.1f;

    /** The extra damping saturates towards this bound, so a very loud input
        cannot make k, and with it h, jump by orders of magnitude in one step. */
    static constexpr float maxExtraDamping = 2.0f;

    /** JUCE's update(), with k in place of R2. cutoffHz must be below half the sample rate. */
    void setCoefficients (float newCutoffHz, float k) noexcept
    {
        cutoffHz = newCutoffHz;
        R2 = k;
        g  = (float) std::tan (juce::MathConstants<double>::pi * cutoffHz / sampleRate);
        h  = (float) (1.0 / (1.0 + R2 * g + g * g));
    }

    /** JUCE's processSample(), operation for operation (bit-identical at the same g, R2, h). */
    Outputs processLinear (float x) noexcept
    {
        const float yHP = h * (x - s1 * (g + R2) - s2);

        const float yBP = yHP * g + s1;
        s1              = yHP * g + yBP;

        const float yLP = yBP * g + s2;
        s2              = yBP * g + yLP;

        return { yLP, yBP, yHP };
    }

    /** The step with amplitude-dependent damping k + c * (A / ceiling)^2
        (see the file comment). ceiling > 0. */
    Outputs processNonlinear (float x, float ceiling) noexcept
    {
        const float a2 = (s1 * s1 + s2 * s2) * 0.25f / (ceiling * ceiling);   // (amplitude / ceiling)^2 from both states
        const float q  = dampingCurve * a2;
        const float k  = R2 + maxExtraDamping * q / (maxExtraDamping + q);   // c u^2 when small, never above +2
        const float hk = 1.0f / (1.0f + k * g + g * g);

        const float yHP = hk * (x - s1 * (g + k) - s2);

        const float yBP = yHP * g + s1;
        s1              = yHP * g + yBP;

        const float yLP = yBP * g + s2;
        s2              = yBP * g + yLP;

        return { yLP, yBP, yHP };
    }

    /** Magnitude of the linear filter at hz with the current g and k: the
        bilinear transform of the analog prototype, s = j tan (pi hz / fs) / g. */
    double magnitudeAt (double hz, Mode m) const noexcept
    {
        const double w = std::tan (juce::MathConstants<double>::pi * hz / sampleRate) / (double) g;
        const std::complex<double> s (0.0, w);
        const std::complex<double> den = s * s + (double) R2 * s + 1.0;
        std::complex<double> num;
        switch (m)
        {
            case Mode::highpass:  num = s * s; break;
            case Mode::bandpass:  num = s; break;
            case Mode::notch:     num = s * s + 1.0; break;
            case Mode::lowpass:
            default:              num = 1.0; break;
        }
        return std::abs (num / den);
    }

    float getK() const noexcept   { return R2; }

private:
    // As JUCE initialises them: 1 kHz at 44.1 kHz, Q 1 / sqrt2.
    double sampleRate = 44100.0;
    float cutoffHz = 1000.0f;
    float R2 = butterworthK();
    float g = 0.0f, h = 0.0f;
    float s1 = 0.0f, s2 = 0.0f;
    float lastInput = 0.0f;   // the drive stage's previous input, for ADAA

    static double sat (double u) noexcept   { return u / std::sqrt (1.0 + u * u); }
};
