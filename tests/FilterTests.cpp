// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Unit tests for the voice filter (CytomicSvf.h): at resonance 0 the linear
    path matches juce::dsp::StateVariableTPTFilter bit for bit, the measured
    response matches the analytic one, the resonance mapping hits its Q
    targets, the nonlinear path is stable and self-oscillates below its
    ceiling, and scaling input and ceiling together scales the output.
    It also prints (without failing) the aliasing added by the saturator.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target FilterTests
    Run the resulting FilterTests executable; it exits non-zero on failure.
*/
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "../CytomicSvf.h"
#include "../Oscillator.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <functional>

namespace
{
    int failures = 0;

    void check (bool condition, const char* what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what);
        if (! condition)
            ++failures;
    }

    using Mode = CytomicSvf::Mode;
    const Mode allModes[] = { Mode::lowpass, Mode::highpass, Mode::bandpass, Mode::notch };
    std::vector<float> noise (int n, juce::int64 seed, float amplitude = 1.0f)
    {
        juce::Random random (seed);
        std::vector<float> x ((size_t) n);
        for (auto& v : x)
            v = amplitude * (2.0f * random.nextFloat() - 1.0f);
        return x;
    }

    std::vector<float> saw (double hz, double rate, int n)
    {
        std::vector<float> x ((size_t) n);
        double phase = 0.0;
        for (auto& v : x)
        {
            v = (float) (2.0 * phase - 1.0);
            phase += hz / rate;
            if (phase >= 1.0) phase -= 1.0;
        }
        return x;
    }

    /** Cutoff in Hz, log-uniform between 20 Hz and 0.45 fs (the voice's clamp range). */
    float randomCutoff (juce::Random& random, double rate)
    {
        const double lo = std::log (20.0), hi = std::log (0.45 * rate);
        return (float) std::exp (lo + (hi - lo) * random.nextDouble());
    }

    bool sameBits (float a, float b)   { return std::memcmp (&a, &b, sizeof (float)) == 0; }
    bool finite (float v)              { return std::isfinite (v); }

    void fft (std::vector<double>& re, std::vector<double>& im)
    {
        const size_t n = re.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const double a = -juce::MathConstants<double>::twoPi / (double) len;
            for (size_t i = 0; i < n; i += len)
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const double c = std::cos (a * (double) k), s = std::sin (a * (double) k);
                    const double xr = re[i + k + len / 2] * c - im[i + k + len / 2] * s;
                    const double xi = re[i + k + len / 2] * s + im[i + k + len / 2] * c;
                    re[i + k + len / 2] = re[i + k] - xr;  im[i + k + len / 2] = im[i + k] - xi;
                    re[i + k] += xr;                       im[i + k] += xi;
                }
        }
    }

    /** Energy away from the harmonics of hz, relative to the total, in dB (as in SculptTests). */
    double aliasingDb (const std::vector<float>& x, double hz, double rate)
    {
        const size_t n = x.size();
        std::vector<double> re (n), im (n, 0.0);
        for (size_t i = 0; i < n; ++i)
        {
            const double p = juce::MathConstants<double>::twoPi * (double) i / (double) (n - 1);
            re[i] = x[i] * (0.35875 - 0.48829 * std::cos (p) + 0.14128 * std::cos (2 * p) - 0.01168 * std::cos (3 * p));
        }
        fft (re, im);
        double total = 0.0, off = 0.0;
        for (size_t k = 1; k < n / 2; ++k)
        {
            const double e = re[k] * re[k] + im[k] * im[k], f = (double) k * rate / (double) n;
            const double h = f / hz, nearest = std::round (h);
            total += e;
            if (nearest < 1.0 || std::abs (h - nearest) * hz > 6.0 * rate / (double) n)
                off += e;
        }
        return 10.0 * std::log10 (off / total);
    }

    //==============================================================================
    /** Runs CytomicSvf (resonance 0, linear) and three JUCE filters (low-, band-, high-pass)
        side by side; nextCutoff (i) returns the cutoff for sample i, or 0 to keep it. */
    bool matchesJuce (const std::vector<float>& input, double rate, float firstCutoff,
                      const std::function<float (int)>& nextCutoff)
    {
        using Juce = juce::dsp::StateVariableTPTFilter<float>;
        using Type = juce::dsp::StateVariableTPTFilterType;
        Juce lp, bp, hp;
        const Type types[] = { Type::lowpass, Type::bandpass, Type::highpass };
        Juce* filters[] = { &lp, &bp, &hp };
        for (int i = 0; i < 3; ++i)
        {
            // As NeonSynthVoice::prepare does it
            filters[i]->prepare ({ rate, 512, 1 });
            filters[i]->setType (types[i]);
            filters[i]->setResonance (1.0f / juce::MathConstants<float>::sqrt2);
            filters[i]->setCutoffFrequency (firstCutoff);
        }

        CytomicSvf svf;
        svf.prepare (rate);
        const float k = CytomicSvf::resonanceToK (0.0f);
        svf.setCoefficients (firstCutoff, k);

        for (size_t i = 0; i < input.size(); ++i)
        {
            const float fc = nextCutoff ((int) i);
            if (fc > 0.0f)
            {
                for (auto* f : filters)
                    f->setCutoffFrequency (fc);
                svf.setCoefficients (fc, k);
            }
            const auto o = svf.processLinear (input[i]);
            if (! sameBits (o.lp, lp.processSample (0, input[i]))
                || ! sameBits (o.bp, bp.processSample (0, input[i]))
                || ! sameBits (o.hp, hp.processSample (0, input[i]))
                || ! sameBits (CytomicSvf::select (o, Mode::lowpass), o.lp))
            {
                std::printf ("    first difference at sample %d (%.1f Hz)\n", (int) i, rate);
                return false;
            }
        }
        return true;
    }

    /** Gain of the linear filter for a sine at hz, by projecting the settled output on sin and cos. */
    double measuredGain (CytomicSvf svf, Mode m, double hz, double rate)
    {
        svf.reset();
        const int settle = (int) rate, measure = (int) rate;
        double c = 0.0, s = 0.0;
        for (int i = 0; i < settle + measure; ++i)
        {
            const double p = juce::MathConstants<double>::twoPi * hz * i / rate;
            const float y = CytomicSvf::select (svf.processLinear ((float) std::sin (p)), m);
            if (i >= settle)
            {
                c += y * std::cos (p);
                s += y * std::sin (p);
            }
        }
        return 2.0 * std::sqrt (c * c + s * s) / measure;
    }

    double textbookMagnitude (Mode m, double hz, double cutoff, double k, double rate)
    {
        const double pi = juce::MathConstants<double>::pi;
        // prewarped; the filter holds tan (pi fc / fs) rounded to float (JUCE's g), so the formula uses that too
        const double w = std::tan (pi * hz / rate) / (double) (float) std::tan (pi * cutoff / rate);
        const double den = std::sqrt ((1.0 - w * w) * (1.0 - w * w) + k * k * w * w);
        switch (m)
        {
            case Mode::highpass: return w * w / den;
            case Mode::bandpass: return w / den;
            case Mode::notch:    return std::abs (1.0 - w * w) / den;
            case Mode::lowpass:
            default:             return 1.0 / den;
        }
    }

    double toDb (double g)   { return 20.0 * std::log10 (std::max (g, 1.0e-12)); }
}

int main()
{
    char what[256];

    std::printf ("Bit-equality with juce::dsp::StateVariableTPTFilter at resonance 0\n");
    {
        check (sameBits (CytomicSvf::resonanceToK (0.0f), (float) (1.0 / (double) (1.0f / juce::MathConstants<float>::sqrt2))),
               "resonanceToK (0) is JUCE's R2 for setResonance (1 / sqrt2)");
        check (sameBits (CytomicSvf::driveToGain (0.0f), 1.0f), "driveToGain (0) is exactly 1");

        for (const double rate : { 44100.0, 48000.0, 96000.0 })
        {
            const int n = (int) rate;   // 1 s
            std::vector<float> input = noise (n, 7);
            const auto s = saw (110.0, rate, n);
            input.insert (input.end(), s.begin(), s.end());
            const auto s2 = saw (1234.5, rate, n / 2);
            input.insert (input.end(), s2.begin(), s2.end());

            bool fixed = true;
            for (const float fc : { 20.0f, 55.5f, 200.0f, 1000.0f, 3333.3f, 8000.0f, 15000.0f, 0.45f * (float) rate })
                fixed = fixed && matchesJuce (input, rate, fc, [] (int) { return 0.0f; });
            std::snprintf (what, sizeof what, "%.1f kHz: fixed cutoffs 20 Hz .. 0.45 fs, noise + saw, LP/BP/HP identical", rate / 1000.0);
            check (fixed, what);

            juce::Random random (42);
            check (matchesJuce (input, rate, 1000.0f, [&] (int) { return randomCutoff (random, rate); }),
                   "  cutoff jumping every sample: identical");
            check (matchesJuce (input, rate, 1000.0f, [&] (int i) { return i % 16 == 0 ? randomCutoff (random, rate) : 0.0f; }),
                   "  cutoff jumping every 16 samples: identical");
            check (matchesJuce (input, rate, 20.0f, [&] (int i)
                   {
                       // a smooth exponential sweep up and down, as a smoother would glide
                       const double t = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / rate);
                       return (float) (20.0 * std::pow (0.45 * rate / 20.0, t));
                   }), "  cutoff gliding every sample: identical");
        }
    }

    std::printf ("\nResponse (linear path, 48 kHz): measured vs magnitudeAt vs textbook\n");
    {
        constexpr double rate = 48000.0;
        double worstMeasured = 0.0, worstTextbook = 0.0;
        for (const float res : { 0.0f, 0.5f, 0.9f })
            for (const float fc : { 100.0f, 1000.0f, 5000.0f })
            {
                CytomicSvf svf;
                svf.prepare (rate);
                svf.setCoefficients (fc, CytomicSvf::resonanceToK (res));
                for (const Mode m : allModes)
                    for (const double hz : { fc * 0.5, (double) fc, fc * 2.0 })
                    {
                        const double analytic = svf.magnitudeAt (hz, m);
                        const double textbook = textbookMagnitude (m, hz, fc, (double) svf.getK(), rate);
                        worstTextbook = std::max (worstTextbook, std::abs (analytic - textbook) / std::max (textbook, 1.0e-3));   // absolute near the notch's zero
                        if (m == Mode::notch && hz == (double) fc)
                            continue;   // the zero itself: -inf dB, compared below
                        const double measured = measuredGain (svf, m, hz, rate);
                        worstMeasured = std::max (worstMeasured, std::abs (toDb (measured) - toDb (analytic)));
                    }
                CytomicSvf notch = svf;
                if (measuredGain (notch, Mode::notch, fc, rate) > 0.01)
                    check (false, "notch at the cutoff below -40 dB");
            }
        std::snprintf (what, sizeof what, "measured gain at cutoff and +-1 octave within 0.1 dB of magnitudeAt (worst %.4f dB), 4 modes x Q x cutoff", worstMeasured);
        check (worstMeasured < 0.1, what);
        std::snprintf (what, sizeof what, "magnitudeAt matches the textbook prewarped 2-pole SVF (worst relative %.2e)", worstTextbook);
        check (worstTextbook < 1.0e-5, what);
    }

    std::printf ("\nResonance mapping\n");
    {
        std::printf ("    resonance      k        Q\n");
        for (const float r : { 0.0f, 0.25f, 0.5f, 0.75f, 0.9f, 0.95f, 0.98f, 1.0f })
        {
            const float k = CytomicSvf::resonanceToK (r);
            std::printf ("    %5.0f%%   %8.4f   %s\n", r * 100.0f, (double) k,
                         k > 0.0f ? juce::String (1.0 / k, 2).toRawUTF8() : "self-osc");
        }
        const float k50 = CytomicSvf::resonanceToK (0.5f), k90 = CytomicSvf::resonanceToK (0.9f);
        check (std::abs (1.0f / k50 - 2.0f) < 0.2f, "Q about 2 at 50%");
        check (std::abs (1.0f / k90 - 10.0f) < 1.0f, "Q about 10 at 90%");
        check (CytomicSvf::resonanceToK (1.0f) < 0.0f, "k < 0 at 100% (self-oscillation)");
        check (CytomicSvf::resonanceToK (0.97f) > 0.0f, "k > 0 at 97% (self-oscillation starts near 98%)");
        bool monotonic = true;
        for (int i = 1; i <= 1000; ++i)
            monotonic = monotonic && CytomicSvf::resonanceToK ((float) i / 1000.0f) < CytomicSvf::resonanceToK ((float) (i - 1) / 1000.0f);
        check (monotonic, "k falls monotonically over 0..1");
    }

    std::printf ("\nStability (nonlinear path, ceiling 1)\n");
    {
        constexpr double rate = 48000.0;
        constexpr int n = 12000;
        const auto noiseIn = noise (n, 3);
        const auto sawIn = saw (110.0, rate, n);
        bool allFinite = true;
        float worst = 0.0f;
        juce::Random random (9);
        int runs = 0;
        for (const float fc : { 20.0f, 80.0f, 300.0f, 1000.0f, 4000.0f, 12000.0f, 0.45f * (float) rate, -1.0f })   // -1: jumping
            for (const float res : { 0.0f, 0.3f, 0.6f, 0.9f, 0.97f, 0.99f, 1.0f })
                for (const float drive : { 0.0f, 0.3f, 0.7f, 1.0f })
                    for (const auto* input : { &noiseIn, &sawIn })
                    {
                        CytomicSvf svf;
                        svf.prepare (rate);
                        const float k = CytomicSvf::resonanceToK (res);
                        const auto d = CytomicSvf::makeDrive (drive);
                        const bool linear = res == 0.0f;
                        svf.setCoefficients (fc > 0.0f ? fc : 1000.0f, k);
                        for (int i = 0; i < n; ++i)
                        {
                            if (fc < 0.0f && i % 16 == 0)
                                svf.setCoefficients (randomCutoff (random, rate), k);
                            const float dry = (*input)[(size_t) i];
                            const float x = drive > 0.0f ? svf.drive (dry, 1.0f, d) : dry;
                            const auto o = linear ? svf.processLinear (x) : svf.processNonlinear (x, 1.0f);
                            for (const Mode m : allModes)
                            {
                                const float y = CytomicSvf::select (o, m);
                                allFinite = allFinite && finite (y);
                                worst = std::max (worst, std::abs (y));
                            }
                        }
                        ++runs;
                    }
        std::snprintf (what, sizeof what, "%d runs (cutoff incl. jumps every 16 samples x resonance x drive x noise/saw): no NaN or inf", runs);
        check (allFinite, what);
        std::snprintf (what, sizeof what, "output bounded (peak %.2f for input peak 1, limit 20)", (double) worst);
        check (worst < 20.0f, what);
    }

    std::printf ("\nSelf-oscillation (resonance 100%%, burst then 2 s of silence)\n");
    {
        constexpr double rate = 48000.0;
        bool allBounded = true, allRinging = true, allPitched = true;
        double worstRatio = 0.0, worstPitch = 0.0, quietest = 1.0e9, flatLow = 1.0e9, flatHigh = 0.0;
        for (const float ceiling : { 1.0f, 0.25f })
            for (const float drive : { 0.0f, 1.0f })
                for (const float fc : { 100.0f, 200.0f, 1000.0f, 5000.0f, 10000.0f, 15000.0f })
                {
                    CytomicSvf svf;
                    svf.prepare (rate);
                    svf.setCoefficients (fc, CytomicSvf::resonanceToK (1.0f));
                    const auto d = CytomicSvf::makeDrive (drive);
                    const float limit = ceiling;
                    const auto burst = noise (64, 5, 0.1f * ceiling);
                    const int total = (int) (2.5 * rate);
                    float peak = 0.0f, earlyPeak = 0.0f, lastPeak = 0.0f;
                    int crossings = 0;
                    float previous = 0.0f;
                    for (int i = 0; i < total; ++i)
                    {
                        const float dry = i < 64 ? burst[(size_t) i] : 0.0f;
                        const auto o = svf.processNonlinear (drive > 0.0f ? svf.drive (dry, ceiling, d) : dry, ceiling);
                        if (i >= (int) (0.5 * rate))   // the 2 s after the build-up
                            for (const Mode m : allModes)
                                peak = std::max (peak, std::abs (CytomicSvf::select (o, m)));
                        if (i >= (int) (0.5 * rate) && i < (int) rate)
                            earlyPeak = std::max (earlyPeak, std::abs (o.lp));
                        if (i >= total - (int) (0.5 * rate))
                        {
                            lastPeak = std::max (lastPeak, std::abs (o.lp));
                            if ((previous < 0.0f) != (o.lp < 0.0f))
                                ++crossings;
                            previous = o.lp;
                        }
                    }
                    const double hz = crossings / 2.0 / 0.5;
                    worstRatio = std::max (worstRatio, (double) (peak / limit));
                    worstPitch = std::max (worstPitch, std::abs (hz / fc - 1.0));
                    quietest = std::min (quietest, (double) (lastPeak / limit));
                    if (fc <= 5000.0f)
                    {
                        flatLow  = std::min (flatLow,  (double) (lastPeak / limit));
                        flatHigh = std::max (flatHigh, (double) (lastPeak / limit));
                    }
                    allBounded = allBounded && peak <= 1.1f * limit;
                    allRinging = allRinging && lastPeak > 0.95f * earlyPeak && lastPeak > 0.02f * limit;
                    allPitched = allPitched && std::abs (hz / fc - 1.0) < 0.05;
                    std::printf ("    L %.2f drive %.0f%% cutoff %5.0f Hz: peak %.3f x L/D, last 0.5 s peak %.3f x L/D, %.1f Hz\n",
                                 (double) ceiling, drive * 100.0, (double) fc, (double) (peak / limit), (double) (lastPeak / limit), hz);
                }
        std::snprintf (what, sizeof what, "peak of all four outputs <= 1.1 x ceiling (worst %.3f)", worstRatio);
        check (allBounded, what);
        std::snprintf (what, sizeof what, "still ringing after 2 s, not decaying (last 0.5 s >= 95%% of 0.5..1 s; quietest %.3f x ceiling)", quietest);
        check (allRinging, what);
        std::snprintf (what, sizeof what, "rings at the cutoff within 5%% (worst %.2f%%)", worstPitch * 100.0);
        check (allPitched, what);
        const double spreadDb = 20.0 * std::log10 (flatHigh / flatLow);
        std::snprintf (what, sizeof what, "level x L the same at every cutoff from 100 Hz to 5 kHz, within 1 dB (spread %.2f dB)", spreadDb);
        check (spreadDb <= 1.0, what);
    }

    std::printf ("\nHomogeneity (input a*x with ceiling a*L gives a * output)\n");
    {
        constexpr double rate = 48000.0;
        const auto x = noise (24000, 11);
        const auto s = saw (220.0, rate, 24000);
        bool ok = true;
        double worst = 0.0;
        for (const float res : { 0.5f, 0.9f, 0.97f })
            for (const float drive : { 0.0f, 1.0f })
                for (const auto* input : { &x, &s })
                {
                    CytomicSvf ref;
                    ref.prepare (rate);
                    ref.setCoefficients (800.0f, CytomicSvf::resonanceToK (res));
                    std::vector<CytomicSvf::Outputs> base;
                    const auto d = CytomicSvf::makeDrive (drive);
                    for (const float v : *input)
                        base.push_back (ref.processNonlinear (drive > 0.0f ? ref.drive (v, 0.3f, d) : v, 0.3f));
                    float scale = 0.0f;
                    for (const auto& o : base)
                        scale = std::max ({ scale, std::abs (o.lp), std::abs (o.bp), std::abs (o.hp) });

                    for (const float a : { 0.1f, 0.5f, 1.0f })
                    {
                        CytomicSvf svf;
                        svf.prepare (rate);
                        svf.setCoefficients (800.0f, CytomicSvf::resonanceToK (res));
                        for (size_t i = 0; i < input->size(); ++i)
                        {
                            const float v = a * (*input)[i];
                            const auto o = svf.processNonlinear (drive > 0.0f ? svf.drive (v, a * 0.3f, d) : v, a * 0.3f);
                            for (const auto& [got, want] : { std::pair<float, float> { o.lp, base[i].lp }, { o.bp, base[i].bp }, { o.hp, base[i].hp } })
                            {
                                const double err = std::abs ((double) got - (double) a * want) / ((double) a * scale);
                                worst = std::max (worst, err);
                                ok = ok && err < 1.0e-4;
                            }
                        }
                    }
                }
        std::snprintf (what, sizeof what, "a in {0.1, 0.5, 1}, resonance 50/90/97%%, drive 0/100%%: within 1e-4 of full scale (worst %.2e)", worst);
        check (ok, what);
    }

    std::printf ("\nDrive (ahead of the filter)\n");
    {
        const auto x = noise (24000, 13);
        bool bounded = true, smallGain = true, off = true;
        double worstPeak = 0.0, worstGain = 0.0;
        for (const float drive : { 0.0f, 0.02f, 0.3f, 0.7f, 1.0f })
            for (const float ceiling : { 1.0f, 0.2f })
            {
                CytomicSvf svf;
                svf.prepare (48000.0);
                const auto d = CytomicSvf::makeDrive (drive);
                for (const float v : x)
                {
                    const float y = svf.drive (ceiling * v, ceiling, d);   // input peak <= the ceiling
                    worstPeak = std::max (worstPeak, (double) (std::abs (y) / ceiling));
                    bounded = bounded && std::abs (y) <= 1.001f * ceiling;
                    off = off && (drive > 0.0f || y == ceiling * v);
                }
                // A small, slow signal: gain = blend * makeup * D / 4 + (1 - blend)
                CytomicSvf quiet;
                quiet.prepare (48000.0);
                float y = 0.0f;
                for (int i = 0; i < 64; ++i)
                    y = quiet.drive (1.0e-3f * ceiling, ceiling, d);
                const double want = d.blend * d.makeup * d.gain + (1.0 - d.blend);
                worstGain = std::max (worstGain, std::abs ((double) y / (1.0e-3 * ceiling) / want - 1.0));
                smallGain = smallGain && std::abs ((double) y / (1.0e-3 * ceiling) / want - 1.0) < 1.0e-3;
            }
        check (off, "drive 0 passes the input through unchanged");
        std::snprintf (what, sizeof what, "input within the ceiling stays within it at every drive (worst %.4f x L)", worstPeak);
        check (bounded, what);
        std::snprintf (what, sizeof what, "small-signal gain as designed (worst relative error %.1e)", worstGain);
        check (smallGain, what);
        std::printf ("    small-signal gain at drive 100%%: %+.1f dB\n", 20.0 * std::log10 (CytomicSvf::makeDrive (1.0f).makeup * CytomicSvf::makeDrive (1.0f).gain));
    }

    std::printf ("\nAliasing (report only): saw, 48 kHz\n");
    {
        // 2093 Hz (C7), not 2 kHz: at 48 kHz a 2 kHz saw's aliases fold exactly onto its own harmonics
        // (48000 / 2000 is a whole number), so no measurement could see them.
        constexpr double rate = 48000.0, hz = 2093.0;
        constexpr int n = 1 << 16, skip = 4800;
        PolyBlepOscillator osc;
        osc.prepare (rate);
        osc.setWaveform (PolyBlepOscillator::sawtooth);
        osc.setFrequency ((float) hz, rate);
        std::vector<float> in ((size_t) (n + skip));
        for (auto& v : in)
            v = osc.nextSample();
        std::vector<float> sawIn (in.begin() + skip, in.end());

        // naive: the same drive curve without ADAA, to show what ADAA buys
        const auto run = [&] (float cutoff, float res, float drive, bool naive)
        {
            CytomicSvf svf;
            svf.prepare (rate);
            svf.setCoefficients (cutoff, CytomicSvf::resonanceToK (res));
            const auto d = CytomicSvf::makeDrive (drive);
            std::vector<float> out;
            out.reserve ((size_t) n);
            for (size_t i = 0; i < in.size(); ++i)
            {
                float v = in[i];
                if (drive > 0.0f)
                {
                    if (naive)
                    {
                        const double u = d.gain * v;
                        v = (float) (v + d.blend * (d.makeup * u / std::sqrt (1.0 + u * u) - v));
                    }
                    else
                    {
                        v = svf.drive (v, 1.0f, d);
                    }
                }
                const auto o = res > 0.0f ? svf.processNonlinear (v, 1.0f) : svf.processLinear (v);
                if (i >= (size_t) skip)
                    out.push_back (cutoff > 20000.0f ? v : o.lp);
            }
            return aliasingDb (out, hz, rate);
        };
        std::printf ("    PolyBLEP saw alone:                               %7.1f dB\n", aliasingDb (sawIn, hz, rate));
        std::printf ("    drive 100%% alone, naive / ADAA:                   %7.1f / %.1f dB\n", run (21000.0f, 0.0f, 1.0f, true), run (21000.0f, 0.0f, 1.0f, false));
        std::printf ("    drive  50%% alone, naive / ADAA:                   %7.1f / %.1f dB\n", run (21000.0f, 0.0f, 0.5f, true), run (21000.0f, 0.0f, 0.5f, false));
        std::printf ("    low-pass 5 kHz, resonance 0, drive 0:              %7.1f dB\n", run (5000.0f, 0.0f, 0.0f, false));
        std::printf ("    low-pass 5 kHz, resonance 90%%, drive 0:            %7.1f dB\n", run (5000.0f, 0.9f, 0.0f, false));
        std::printf ("    low-pass 5 kHz, resonance 90%%, drive 100%%:         %7.1f dB\n", run (5000.0f, 0.9f, 1.0f, false));
        std::printf ("    (non-harmonic energy relative to total; lower is cleaner)\n");
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
