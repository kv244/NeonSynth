// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Unit tests for pitch modulation in the four sources (v1.9.0): the
    oscillator, Random, the sample and the grains follow the per-sample pitch
    ratio the voice gets from setModulation().

    The modulation buffers are built here by hand (a constant ratio, a sweep),
    so these tests don't depend on how Modulation makes them. They check that
      - each source sounds at the note's frequency times the ratio;
      - an all-1 buffer renders the oscillators and the sample exactly as no buffer does;
      - the sample's move between its band-limited copies, under a 0 to +24
        semitone sweep, is click-free (no step larger than an ideal sweep's
        largest step) and keeps the read position;
      - vibrato near a copy boundary doesn't make the sample switch back and forth.
    That a null buffer renders exactly what v1.8.0 did is checked by
    tests/compare_versions.py against the v1.8.0 build.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target PitchTests
    Run the resulting PitchTests executable; it exits non-zero on failure.
*/
#include "NeonSynthProcessor.h"
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    int failures = 0;

    void check (bool condition, const char* what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what);
        if (! condition)
            ++failures;
    }

    constexpr double rate = 48000.0;
    constexpr int blockSize = 256;

    std::unique_ptr<SampleData> sineSample (double hz, double seconds)
    {
        juce::AudioBuffer<float> b (1, (int) (seconds * rate));
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate));
        juce::String error;
        return SampleData::create (b, rate, { "sine", {}, false }, error);
    }

    const char* nameOf (NeonSynthVoice::Source s)
    {
        switch (s)
        {
            case NeonSynthVoice::Source::oscillator: return "Oscillator";
            case NeonSynthVoice::Source::sample:     return "Sample";
            case NeonSynthVoice::Source::granular:   return "Granular";
            case NeonSynthVoice::Source::random:     return "Random";
        }
        return "?";
    }

    /**
        Plays one held note on a voice for `seconds`, with the pitch ratio of each
        sample from ratioAt (n) (n counted from the note-on), or no pitch buffer at
        all if ratioAt is null. Returns the left channel (width 0: mono).
    */
    template <typename RatioAt>
    std::vector<float> renderNote (NeonSynthVoice::Source source, const SampleData* sample, int note,
                                   double seconds, RatioAt&& ratioAt, bool modulated)
    {
        NeonSynthVoice voice;
        voice.setVoiceIndex (0);
        voice.prepare (rate, blockSize);

        NeonSynthVoice::Params p;
        p.waveform = PolyBlepOscillator::sine;
        p.filterOn = false;
        p.adsr = { 0.001f, 0.1f, 1.0f, 0.1f };
        p.source = source;
        p.sample = sample;
        p.sampleSettings.rootNote = 60;
        p.sampleSettings.loopMode = SampleReader::loop;
        p.sampleSettings.start = 0.0f;
        p.sampleSettings.loopStart = 0.0f;
        p.sampleSettings.loopEnd = 1.0f;
        p.grainSettings.sizeSeconds = 0.08f;
        p.grainSettings.density = 30.0f;
        p.grainSettings.spray = 0.3f;
        voice.setParameters (p);

        NeonSynthSound sound;
        voice.startNote (note, 1.0f, &sound, 0);

        const int total = (int) (seconds * rate);
        std::vector<float> out;
        out.reserve ((size_t) total);
        juce::AudioBuffer<float> buffer (2, blockSize);
        std::vector<float> ratios ((size_t) blockSize, 1.0f);

        for (int done = 0; done < total; done += blockSize)
        {
            const int n = std::min (blockSize, total - done);
            voice.setParameters (p);
            ModulationBlock block;
            block.numSamples = n;
            if (modulated)
            {
                for (int i = 0; i < n; ++i)
                    ratios[(size_t) i] = ratioAt (done + i);
                block.pitchRatio = ratios.data();
            }
            voice.setModulation (block);
            buffer.clear();
            voice.renderNextBlock (buffer, 0, n);
            for (int i = 0; i < n; ++i)
                out.push_back (buffer.getSample (0, i));
        }
        return out;
    }

    /** Fundamental by normalised autocorrelation, searched over periods 0.7 .. 1.4 x the expected one. */
    double autocorrelationHz (const std::vector<float>& x, size_t from, double expectedHz)
    {
        const double expectedLag = rate / expectedHz;
        const int lo = (int) (0.7 * expectedLag), hi = (int) (1.4 * expectedLag) + 1;
        const size_t n = 16384;
        std::vector<double> r ((size_t) hi + 2, 0.0);
        for (int lag = lo - 1; lag <= hi + 1; ++lag)
        {
            double sum = 0.0, e1 = 0.0, e2 = 0.0;
            for (size_t i = from; i < from + n; ++i)
            {
                sum += (double) x[i] * x[i + (size_t) lag];
                e1  += (double) x[i] * x[i];
                e2  += (double) x[i + (size_t) lag] * x[i + (size_t) lag];
            }
            r[(size_t) lag] = sum / std::sqrt (e1 * e2 + 1e-30);
        }
        int best = lo;
        for (int lag = lo; lag <= hi; ++lag)
            if (r[(size_t) lag] > r[(size_t) best])
                best = lag;
        // Parabolic interpolation around the peak.
        const double a = r[(size_t) best - 1], b = r[(size_t) best], c = r[(size_t) best + 1];
        const double denom = a - 2.0 * b + c;
        const double offset = denom != 0.0 ? 0.5 * (a - c) / denom : 0.0;
        return rate / ((double) best + offset);
    }

    double energyAt (const std::vector<float>& x, size_t from, double hz)
    {
        const double w = juce::MathConstants<double>::twoPi * hz / rate, c = 2.0 * std::cos (w);
        double s1 = 0.0, s2 = 0.0;
        for (size_t i = from; i < x.size(); ++i)
        {
            const double s0 = x[i] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return s1 * s1 + s2 * s2 - c * s1 * s2;
    }

    /** The strongest frequency between 50 Hz and 4 kHz, on a 0.25% grid. */
    double dominantHz (const std::vector<float>& x, size_t from)
    {
        double best = 50.0, bestEnergy = 0.0;
        for (double hz = 50.0; hz < 4000.0; hz *= 1.0025)
        {
            const double e = energyAt (x, from, hz);
            if (e > bestEnergy)
            {
                bestEnergy = e;
                best = hz;
            }
        }
        return best;
    }

    float largestStep (const std::vector<float>& x, size_t from, size_t to)
    {
        float most = 0.0f;
        for (size_t i = from + 1; i < to; ++i)
            most = std::max (most, std::abs (x[i] - x[i - 1]));
        return most;
    }
}

int main()
{
    char what[256];
    juce::ScopedJuceInitialiser_GUI juce;   // the voice's JUCE classes expect it

    auto sine = sineSample (440.0, 4.0);   // root 60: plays 440 Hz at note 60
    using Source = NeonSynthVoice::Source;

    std::printf ("Each source follows a constant pitch ratio\n");
    for (const auto source : { Source::oscillator, Source::random, Source::sample, Source::granular })
    {
        // The oscillators play A3 (220 Hz); the sample sources play note 60 (440 Hz, the sample's own pitch).
        const bool fromSample = source == Source::sample || source == Source::granular;
        const int note = fromSample ? 60 : 57;
        const double noteHz = fromSample ? 440.0 : 220.0;

        for (const double semitones : { 0.0, 12.0, -7.0 })
        {
            const auto ratio = (float) std::pow (2.0, semitones / 12.0);
            const auto out = renderNote (source, sine.get(), note, 1.0, [ratio] (int) { return ratio; }, true);
            const double expected = noteHz * (double) ratio;
            const size_t from = 9600;   // after the attack (and the cloud filling up)
            if (source == Source::granular)
            {
                // Overlapping windowed grains with random phases: the spectrum's peak is broad
                // (an 80 ms grain is about 12 Hz wide), so compare energies, as GranularTests does.
                const double onPitch = energyAt (out, from, expected);
                const double neighbour = std::max (energyAt (out, from, expected * 1.12), energyAt (out, from, expected / 1.12));
                const double measured = dominantHz (out, from);
                const double errorCents = 1200.0 * std::log2 (measured / expected);
                std::snprintf (what, sizeof (what), "%s at %+.0f semitones peaks at %.1f Hz (expected %.1f Hz, %+.1f cents; %.0f dB above two semitones off)",
                               nameOf (source), semitones, measured, expected, errorCents, 10.0 * std::log10 (onPitch / neighbour));
                check (std::abs (errorCents) < 25.0 && onPitch > 100.0 * neighbour, what);
                continue;
            }
            const double measured = autocorrelationHz (out, from, expected);
            const double errorCents = 1200.0 * std::log2 (measured / expected);
            std::snprintf (what, sizeof (what), "%s at %+.0f semitones sounds at %.1f Hz (expected %.1f Hz, %+.1f cents)",
                           nameOf (source), semitones, measured, expected, errorCents);
            check (std::abs (errorCents) < 3.0, what);
        }
    }

    std::printf ("An all-1 pitch buffer renders as no buffer does\n");
    for (const auto source : { Source::oscillator, Source::random, Source::sample })
    {
        const auto ones = [] (int) { return 1.0f; };
        const auto a = renderNote (source, sine.get(), 64, 0.5, ones, false);
        const auto b = renderNote (source, sine.get(), 64, 0.5, ones, true);
        std::snprintf (what, sizeof (what), "%s: ratio 1 everywhere is bit-identical to no pitch buffer", nameOf (source));
        check (a == b, what);
    }

    std::printf ("Sample: moving between copies under a 0 to +24 semitone sweep\n");
    {
        // A 220 Hz sample played at its root, swept up two octaves over 2 s: it passes the
        // copy boundaries at +3 and +15 semitones, each a 2 ms crossfade between copies.
        auto low = sineSample (220.0, 8.0);
        const double seconds = 2.6;   // the sweep, then 0.5 s held at the top
        const int sweepStart = 4800, sweepLength = 96000;
        const auto ratioAt = [=] (int n)
        {
            const double t = juce::jlimit (0.0, 1.0, (double) (n - sweepStart) / (double) sweepLength);
            return (float) std::pow (2.0, 24.0 * t / 12.0);
        };
        const auto out = renderNote (Source::sample, low.get(), 60, seconds, ratioAt, true);

        // The clean reference: an ideal sine following the same ratio, at the voice's peak level.
        float peak = 0.0f;
        for (size_t i = (size_t) sweepStart; i < out.size(); ++i)
            peak = std::max (peak, std::abs (out[i]));
        std::vector<float> reference (out.size());
        double phase = 0.0;
        for (size_t i = 0; i < reference.size(); ++i)
        {
            reference[i] = peak * (float) std::sin (phase);
            phase += juce::MathConstants<double>::twoPi * 220.0 * (double) ratioAt ((int) i) / rate;
        }

        const float stepOut = largestStep (out, (size_t) sweepStart, out.size());
        const float stepRef = largestStep (reference, (size_t) sweepStart, reference.size());
        std::snprintf (what, sizeof (what), "no click: largest step %.5f, the ideal sweep's %.5f", stepOut, stepRef);
        check (stepOut <= stepRef * 1.01f, what);

        // The level stays even through the crossfades (a misaligned copy would dip or beat).
        float lowest = 1.0f;
        for (size_t i = (size_t) sweepStart; i + 480 < out.size(); i += 240)
        {
            float m = 0.0f;
            for (size_t k = i; k < i + 480; ++k)   // 10 ms windows: at least two cycles
                m = std::max (m, std::abs (out[k]));
            lowest = std::min (lowest, m);
        }
        std::snprintf (what, sizeof (what), "the level holds through the copy changes (lowest 10 ms peak %.3f of %.3f)", lowest, peak);
        check (lowest > 0.97f * peak, what);

        const double endHz = autocorrelationHz (out, out.size() - 16384 - 1000, 880.0);
        std::snprintf (what, sizeof (what), "the sweep ends two octaves up (%.1f Hz, expected 880)", endHz);
        check (std::abs (1200.0 * std::log2 (endHz / 880.0)) < 3.0, what);
    }

    std::printf ("SampleReader: copies and hysteresis\n");
    {
        SampleReader::Settings settings;
        settings.rootNote = 60;
        settings.loopMode = SampleReader::loop;
        settings.start = 0.25f;

        const int usersBefore = sine->users.load();   // (the voices above still hold theirs: a voice never touches the sample when destroyed)
        SampleReader a, b;
        a.begin (sine.get(), 60, rate, settings);
        for (int i = 0; i < 1000; ++i)
            a.nextSample (1.5f);
        a.copyNoteTo (b);
        b.moveToLevel (1);
        std::snprintf (what, sizeof (what), "a note moved to the half-rate copy keeps its place (%.6f, %.6f) and gets level 1",
                       a.getPosition(), b.getPosition());
        check (b.getLevel() == 1 && std::abs (a.getPosition() - b.getPosition()) < 1.0e-5f, what);

        float worst = 0.0f;
        for (int i = 0; i < 2000; ++i)
            worst = std::max (worst, std::abs (a.nextSample (1.5f) - b.nextSample (1.5f)));
        std::snprintf (what, sizeof (what), "both copies then play the same sound (largest difference %.4f of 0.5)", worst);
        check (worst < 0.01f, what);
        check (sine->users.load() == usersBefore + 2, "each reader holds its own reference on the sample");
        a.end(); b.end();
        check (sine->users.load() == usersBefore, "and lets go of it at end()");

        // Vibrato around the +3 semitone boundary: raise at once above it, return only a semitone below.
        a.begin (sine.get(), 60, rate, settings);
        const auto at = [] (double semitones) { return (float) std::pow (2.0, semitones / 12.0); };
        check (a.levelFor (at (2.9)) == 0 && a.levelFor (at (3.1)) == 1, "the full-rate copy is left just above +3 semitones");
        a.moveToLevel (a.levelFor (at (3.1)));
        check (a.levelFor (at (2.5)) == 1 && ! a.wantsOtherLevel (at (2.5)), "vibrato dipping to +2.5 stays on the half-rate copy");
        check (a.levelFor (at (1.9)) == 0 && a.wantsOtherLevel (at (1.9)), "below +2 it goes back to the full-rate copy");
        check (a.levelFor (at (2.5), false) == 0, "without hysteresis the note-on rule picks the full-rate copy");
        check (a.levelFor (at (27.5)) == 2, "+27.5 semitones reads the quarter-rate copy");
        a.end();
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
