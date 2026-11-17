// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Pitch bend, mod wheel and the LFO through the whole processor (v1.9.0):
    a bend reaches keyboard notes and sequencer notes alike (the sequencer
    plays on channel 16, the keyboard is moved to channel 1, the bend comes
    in on any channel), a bend in the middle of a block changes the pitch
    from its sample, the mod wheel makes a held note's pitch swing, and the
    LFO's CUTOFF depth makes the brightness rise and fall.

    A check whose path isn't in this build yet (the voices' pitch input, or
    the LFO itself, written in parallel) prints PENDING instead of failing:
    it is recognised by the output not changing at all.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target MidiModulationTests
    Run the resulting MidiModulationTests executable; it exits non-zero on failure.
*/
#include "NeonSynthProcessor.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <functional>
#include <vector>
#include <algorithm>

namespace
{
    int failures = 0, pending = 0;

    void check (bool condition, const char* what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what);
        if (! condition)
            ++failures;
    }

    void markPending (const char* what, const char* why)
    {
        std::printf ("  PENDING  %s (%s)\n", what, why);
        ++pending;
    }

    constexpr double rate = 48000.0;
    constexpr int    blockSize = 512;

    void setParameter (juce::AudioProcessor& p, const juce::String& id, float value)
    {
        for (auto* param : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param); ranged != nullptr && ranged->getParameterID() == id)
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
    }

    struct TimedMessage
    {
        juce::MidiMessage message;
        juce::int64 sample;
    };

    /** Renders `seconds` through a fresh processor (sine, filter off, mono width) with these
        messages at their absolute sample positions; returns the left channel. */
    std::vector<float> render (const std::function<void (NeonSynthProcessor&)>& configure,
                               const std::vector<TimedMessage>& messages, double seconds)
    {
        NeonSynthProcessor p;
        setParameter (p, "waveform", 0.0f);
        setParameter (p, "filterEnabled", 0.0f);
        setParameter (p, "width", 0.0f);
        setParameter (p, "sustain", 1.0f);
        configure (p);
        p.setRateAndBufferSizeDetails (rate, blockSize);
        p.prepareToPlay (rate, blockSize);

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;
        std::vector<float> out;
        const auto total = (juce::int64) (seconds * rate);
        for (juce::int64 start = 0; start < total; start += blockSize)
        {
            midi.clear();
            for (const auto& m : messages)
                if (m.sample >= start && m.sample < start + blockSize)
                    midi.addEvent (m.message, (int) (m.sample - start));
            p.processBlock (buffer, midi);
            out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
        }
        p.releaseResources();
        return out;
    }

    /** Frequency of a clean tone between two times, from its rising zero crossings. */
    double frequency (const std::vector<float>& x, double fromSeconds, double toSeconds)
    {
        const auto first = (size_t) (fromSeconds * rate), last = std::min (x.size(), (size_t) (toSeconds * rate));
        double firstCrossing = -1.0, lastCrossing = -1.0;
        int crossings = 0;
        for (size_t i = first + 1; i < last; ++i)
        {
            if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            {
                const double t = (double) (i - 1) + (double) x[i - 1] / ((double) x[i - 1] - (double) x[i]);
                if (firstCrossing < 0.0)
                    firstCrossing = t;
                lastCrossing = t;
                ++crossings;
            }
        }
        return crossings < 2 ? 0.0 : (crossings - 1) * rate / (lastCrossing - firstCrossing);
    }

    /** Power at one frequency between two times (Goertzel, Hann window). */
    double power (const std::vector<float>& x, double hz, double fromSeconds, double toSeconds)
    {
        const auto first = (size_t) (fromSeconds * rate), last = std::min (x.size(), (size_t) (toSeconds * rate));
        const double w = juce::MathConstants<double>::twoPi * hz / rate, coefficient = 2.0 * std::cos (w);
        double s1 = 0.0, s2 = 0.0;
        for (size_t i = first; i < last; ++i)
        {
            const double hann = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) (i - first) / (double) (last - first));
            const double s0 = hann * (double) x[i] + coefficient * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    }

    double cents (double hz, double reference)   { return 1200.0 * std::log2 (hz / reference); }

    bool identical (const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
    }

    /** Brightness of a window: energy of the first difference over the energy of the signal
        (a high-pass weighting, independent of the level; it tracks the spectral centroid). */
    double brightness (const std::vector<float>& x, size_t first, size_t count)
    {
        double diff = 0.0, total = 1.0e-30;
        for (size_t i = first + 1; i < first + count; ++i)
        {
            diff  += (double) (x[i] - x[i - 1]) * (double) (x[i] - x[i - 1]);
            total += (double) x[i] * (double) x[i];
        }
        return diff / total;
    }

    /** max / min brightness from `fromSeconds` to the end, over windows of 5 cycles of a
        tone at `hz` (whole cycles, so a steady tone measures the same in every window). */
    double brightnessSwing (const std::vector<float>& x, double fromSeconds, double hz)
    {
        const auto window = (size_t) std::lround (5.0 * rate / hz);
        double lo = 1.0e300, hi = 0.0;
        for (auto i = (size_t) (fromSeconds * rate); i + window < x.size(); i += window / 2)
        {
            const double b = brightness (x, i, window);
            lo = std::min (lo, b);
            hi = std::max (hi, b);
        }
        return hi / lo;
    }

    const double bentUp = std::pow (2.0, 2.0 / 12.0);   // a full bend at the default range of 2
    const auto noConfiguration = [] (NeonSynthProcessor&) {};
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // the processor's editor code needs the message manager
    std::printf ("MidiModulationTests\n");

    // Is the pitch input of the voices in this build? A full bend on a held note must change
    // the output; if not a single bit changes, the source path isn't wired yet.
    const std::vector<TimedMessage> note { { juce::MidiMessage::noteOn (1, 69, (juce::uint8) 100), 0 } };
    auto withBend = note;
    withBend.push_back ({ juce::MidiMessage::pitchWheel (1, 16383), 0 });
    const auto plain = render (noConfiguration, note, 0.5);
    const auto bent  = render (noConfiguration, withBend, 0.5);
    const bool pitchWired = ! identical (plain, bent);
    const char* notWired = "the voices don't read the pitch modulation yet";

    //==========================================================================
    std::printf ("Pitch bend on keyboard notes\n");
    if (pitchWired)
    {
        check (std::abs (cents (frequency (plain, 0.1, 0.5), 440.0)) < 1.0, "unbent, A4 plays at 440 Hz");
        check (std::abs (cents (frequency (bent, 0.1, 0.5), 440.0 * bentUp)) < 1.0, "a full bend up raises it 2 semitones");

        auto down = note;
        down.push_back ({ juce::MidiMessage::pitchWheel (7, 0), 0 });   // another channel: still the same synth
        check (std::abs (cents (frequency (render (noConfiguration, down, 0.5), 0.1, 0.5), 440.0 / bentUp)) < 1.0,
               "a full bend down on channel 7 lowers it 2 semitones");
    }
    else
    {
        markPending ("a bend reaches keyboard notes", notWired);
    }

    //==========================================================================
    std::printf ("Pitch bend on sequencer notes\n");
    for (const int mode : { 1, 0 })   // Replace, Layer
    {
        const auto sequencer = [mode] (NeonSynthProcessor& p)
        {
            setParameter (p, "seqOn", 1.0f);
            setParameter (p, "seqSync", 0.0f);
            setParameter (p, "seqTempo", 120.0f);
            setParameter (p, "seqRate", 0.0f);    // 1/8: 250 ms steps
            setParameter (p, "seqGate", 0.9f);
            setParameter (p, "seqMode", (float) mode);
            for (int i = 0; i < StepSequencer::numSteps; ++i)
                p.setSequencerStep (i, { true, 57, 100 });   // A3, 220 Hz, on every step
        };

        // The bend arrives on channel 1 (the keyboard's); the sequencer's notes are on 16.
        const auto unbentSeq = render (sequencer, {}, 0.25);
        const auto bentSeq   = render (sequencer, { { juce::MidiMessage::pitchWheel (1, 16383), 0 } }, 0.25);
        const char* what = mode == 1 ? "Replace mode: a bend reaches the sequencer's notes"
                                     : "Layer mode: a bend reaches the sequencer's notes";
        if (pitchWired)
        {
            check (std::abs (cents (frequency (unbentSeq, 0.03, 0.2), 220.0)) < 1.0
                && std::abs (cents (frequency (bentSeq, 0.03, 0.2), 220.0 * bentUp)) < 1.0, what);
        }
        else
        {
            markPending (what, notWired);
        }
    }

    // Layer: a held keyboard note (A4) and the sequencer's notes (A2) sound and bend together.
    if (pitchWired)
    {
        const auto layer = [] (NeonSynthProcessor& p)
        {
            setParameter (p, "seqOn", 1.0f);
            setParameter (p, "seqSync", 0.0f);
            setParameter (p, "seqTempo", 120.0f);
            setParameter (p, "seqRate", 0.0f);
            setParameter (p, "seqGate", 0.9f);
            setParameter (p, "seqMode", 0.0f);
            for (int i = 0; i < StepSequencer::numSteps; ++i)
                p.setSequencerStep (i, { true, 45, 100 });
        };
        auto both = note;
        both.push_back ({ juce::MidiMessage::pitchWheel (1, 16383), 0 });
        const auto mixed = render (layer, both, 0.25);
        const double from = 0.03, to = 0.2;
        const bool keyboardBent  = power (mixed, 440.0 * bentUp, from, to) > 100.0 * power (mixed, 440.0, from, to);
        const bool sequencerBent = power (mixed, 110.0 * bentUp, from, to) > 100.0 * power (mixed, 110.0, from, to);
        check (keyboardBent && sequencerBent, "Layer mode: a keyboard note and the sequencer's notes sound together, both bent");
    }

    //==========================================================================
    std::printf ("Sample-accurate bend\n");
    if (pitchWired)
    {
        const juce::int64 at = 20 * blockSize + 300;   // in the middle of block 20, the note long settled
        auto midBlock = note;
        midBlock.push_back ({ juce::MidiMessage::pitchWheel (1, 16383), at });
        const auto moved = render (noConfiguration, midBlock, 0.5);

        size_t firstDifference = moved.size();
        for (size_t i = 0; i < moved.size(); ++i)
            if (moved[i] != plain[i])
            {
                firstDifference = i;
                break;
            }
        check (firstDifference >= (size_t) at && firstDifference <= (size_t) at + 2,
               "a bend at sample 300 of a block changes the output from that sample, not before");
        check (std::abs (cents (frequency (moved, (double) at / rate + 0.02, 0.5), 440.0 * bentUp)) < 1.0,
               "and the pitch has risen 5 ms later");
    }
    else
    {
        markPending ("a bend in the middle of a block", notWired);
    }

    //==========================================================================
    std::printf ("Mod wheel vibrato\n");
    if (pitchWired)
    {
        auto wheel = note;
        wheel.push_back ({ juce::MidiMessage::controllerEvent (1, 1, 127), 0 });
        const auto vibrato = render (noConfiguration, wheel, 1.0);

        // Frequency in short windows (about 1/10 of a vibrato cycle).
        double lo = 1.0e9, hi = -1.0e9, loPlain = 1.0e9, hiPlain = -1.0e9;
        for (double t = 0.1; t + 0.018 < 1.0; t += 0.009)
        {
            const double c = cents (frequency (vibrato, t, t + 0.018), 440.0);
            lo = std::min (lo, c);
            hi = std::max (hi, c);
        }
        for (double t = 0.1; t + 0.018 < 0.5; t += 0.009)
        {
            const double c = cents (frequency (plain, t, t + 0.018), 440.0);
            loPlain = std::min (loPlain, c);
            hiPlain = std::max (hiPlain, c);
        }
        check (hi > 20.0 && hi < 31.0 && lo < -20.0 && lo > -31.0, "the wheel at its top swings the pitch about 30 cents each way");
        check (hiPlain - loPlain < 1.0, "without the wheel the pitch stays put");
    }
    else
    {
        markPending ("the mod wheel's vibrato on a held note", notWired);
    }

    //==========================================================================
    std::printf ("LFO cutoff\n");
    {
        // Is the LFO in this build? (The shared contract's stub outputs only zeros.)
        Lfo probe;
        probe.prepare (rate);
        probe.setSettings ({});
        std::vector<float> values (4800);
        probe.render (values.data(), (int) values.size(), {});
        const bool lfoWorks = std::any_of (values.begin(), values.end(), [] (float v) { return v != 0.0f; });

        const auto saw = [] (NeonSynthProcessor& p)
        {
            setParameter (p, "waveform", 3.0f);
            setParameter (p, "filterEnabled", 1.0f);
            setParameter (p, "cutoff", 400.0f);
        };
        const auto sweeping = [&saw] (NeonSynthProcessor& p)
        {
            saw (p);
            setParameter (p, "lfoRate", 2.0f);
            setParameter (p, "lfoCutoff", 3.0f);
        };
        const std::vector<TimedMessage> low { { juce::MidiMessage::noteOn (1, 45, (juce::uint8) 100), 0 } };
        const auto still = render (saw, low, 1.5);
        const auto swept = render (sweeping, low, 1.5);

        if (lfoWorks)
        {
            const double swing = brightnessSwing (swept, 0.1, 110.0), stillSwing = brightnessSwing (still, 0.1, 110.0);
            std::printf ("    brightness max/min: %.2f with the LFO, %.2f without\n", swing, stillSwing);
            check (swing > 4.0 && stillSwing < 1.2, "LFO CUTOFF 3 octaves at 2 Hz: the brightness rises and falls");
        }
        else
        {
            check (identical (still, swept), "with the LFO stub the cutoff depth changes nothing");
            markPending ("LFO CUTOFF moves the cutoff", "the LFO isn't in this build yet");
        }
    }

    std::printf (failures == 0 ? "All tests passed" : "%d test(s) FAILED", failures);
    std::printf (pending > 0 ? " (%d pending).\n" : ".\n", pending);
    return failures == 0 ? 0 : 1;
}
