// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Unit tests for MIDI pitch bend, mod wheel, vibrato, tremolo and the
    modulation buffers (Modulation.h): the bend's mapping, range and 5 ms
    glide, sample-accurate events, CC 121, the 14-bit wheel, the vibrato's
    depth and rate, null buffers when nothing modulates, the tremolo's range,
    smoothed depth changes, repeatability after reset() and blocks longer
    than the buffers. The LFO paths use known LFO values passed in through
    Modulation's test hook, so they don't depend on the LFO itself (LfoTests
    covers that).

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target ModulationTests
    Run the resulting ModulationTests executable; it exits non-zero on failure.
*/
#include "dsp/Modulation.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

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
    constexpr int    blockSize = 512;
    const Lfo::Transport transport;

    bool near (double a, double b, double tolerance)   { return std::abs (a - b) <= tolerance; }
    double semitones (float ratio)                     { return 12.0 * std::log2 ((double) ratio); }

    juce::MidiMessage bendMessage (int value14, int channel = 1)   { return juce::MidiMessage::pitchWheel (channel, value14); }
    juce::MidiMessage cc (int controller, int value, int channel = 1)
    {
        return juce::MidiMessage::controllerEvent (channel, controller, value);
    }

    /** A prepared Modulation with these settings (depths snapped to them). */
    void start (Modulation& m, const Modulation::Settings& s, int preparedBlock = blockSize)
    {
        m.prepare (rate, preparedBlock);
        m.setSettings (s);
    }

    /** One block with these messages (at their positions); returns the block. */
    ModulationBlock block (Modulation& m, std::initializer_list<std::pair<juce::MidiMessage, int>> messages, int numSamples = blockSize)
    {
        juce::MidiBuffer midi;
        for (const auto& [message, position] : messages)
            midi.addEvent (message, position);
        m.handleMidi (midi, numSamples);
        return m.render (numSamples, transport);
    }

    /** Renders `count` quiet blocks (smoothers settle); returns the last one. */
    ModulationBlock settle (Modulation& m, int count = 4)
    {
        ModulationBlock b;
        for (int i = 0; i < count; ++i)
            b = block (m, {});
        return b;
    }

    /** The bend's settled pitch, in semitones, for a 14-bit bend value and a range. */
    double settledBend (int value14, int range)
    {
        Modulation m;
        Modulation::Settings s;
        s.bendRangeSemitones = range;
        start (m, s);
        block (m, { { bendMessage (value14), 0 } });
        const auto b = settle (m);
        return b.pitchRatio == nullptr ? 0.0 : semitones (b.pitchRatio[blockSize - 1]);
    }
}

int main()
{
    std::printf ("ModulationTests\n");

    //==========================================================================
    std::printf ("Idle\n");
    {
        Modulation m;
        start (m, {});
        auto b = block (m, {});
        check (b.pitchRatio == nullptr && b.cutoffOctaves == nullptr && b.numSamples == blockSize,
               "with default settings and no MIDI both buffers are null");

        juce::AudioBuffer<float> audio (2, blockSize);
        audio.clear();
        audio.setSample (0, 7, 0.25f);
        m.applyAmplitude (audio, blockSize);
        check (audio.getSample (0, 7) == 0.25f, "applyAmplitude() leaves the audio untouched at AMP 0");

        b = block (m, { { bendMessage (8192), 10 }, { cc (1, 0), 20 } });
        check (b.pitchRatio == nullptr, "a centred bend and a wheel at 0 leave the pitch buffer null");
    }

    //==========================================================================
    std::printf ("Pitch bend\n");
    {
        check (near (settledBend (16383, 2), 2.0, 1.0e-4), "full bend up, range 2: +2 semitones");
        check (near (settledBend (0, 2), -2.0, 1.0e-4), "full bend down, range 2: -2 semitones");
        check (near (settledBend (16383, 24), 24.0, 1.0e-4), "full bend up, range 24: +24 semitones");
        check (near (settledBend (0, 24), -24.0, 1.0e-4), "full bend down, range 24: -24 semitones");
        check (settledBend (16383, 0) == 0.0, "range 0: the bend does nothing (and the buffer is null)");
        check (near (settledBend (8192 + 4096, 2), 2.0 * 4096.0 / 8191.0, 1.0e-4), "upper half maps over 8191 steps");
        check (near (settledBend (8192 - 4096, 2), -1.0, 1.0e-4), "lower half maps over 8192 steps");

        // Any channel: the sequencer plays on 16, the keyboard on 1.
        Modulation m;
        start (m, {});
        block (m, { { bendMessage (16383, 16), 0 } });
        const auto b = settle (m);
        check (b.pitchRatio != nullptr && near (semitones (b.pitchRatio[0]), 2.0, 1.0e-4), "a bend on channel 16 counts too");
        check (near (m.getBendPosition(), 1.0, 1.0e-6), "the panel's bend position is +1");
    }

    std::printf ("Bend glide and timing\n");
    {
        Modulation m;
        start (m, {});
        const auto b = block (m, { { bendMessage (16383), 100 } });
        const int glide = (int) (0.005 * rate);   // 240 samples

        bool before = true;
        for (int i = 0; i < 100; ++i)
            before = before && b.pitchRatio[i] == 1.0f;
        check (b.pitchRatio != nullptr && before, "an event at sample 100: the samples before it are untouched");
        check (b.pitchRatio[100] > 1.0f, "the bend starts on the event's sample");
        check (near (semitones (b.pitchRatio[100 + glide / 2 - 1]), 1.0, 0.02), "halfway through 5 ms it is halfway");
        check (semitones (b.pitchRatio[100 + glide - 2]) < 2.0 - 1.0e-4, "it hasn't arrived 1 sample before 5 ms");
        check (near (semitones (b.pitchRatio[100 + glide - 1]), 2.0, 1.0e-5), "it arrives after 5 ms");

        bool monotonic = true;
        for (int i = 101; i < 100 + glide; ++i)
            monotonic = monotonic && b.pitchRatio[i] >= b.pitchRatio[i - 1];
        check (monotonic, "the glide rises steadily");

        // Two events in one block: each starts at its own sample.
        Modulation m2;
        start (m2, {});
        const auto b2 = block (m2, { { bendMessage (16383), 0 }, { bendMessage (8192), 400 } });
        check (near (semitones (b2.pitchRatio[399]), 2.0, 1.0e-5) && b2.pitchRatio[400] < b2.pitchRatio[399],
               "a second event in the same block turns the glide back at its sample");
    }

    //==========================================================================
    std::printf ("Mod wheel and CC 121\n");
    {
        Modulation m;
        start (m, {});
        block (m, { { cc (1, 64), 0 } });
        settle (m);
        check (near (m.getWheelPosition(), 64.0 / 127.0, 1.0e-6), "CC 1 alone is a 7-bit value");

        block (m, { { cc (33, 100), 0 } });
        settle (m);
        check (near (m.getWheelPosition(), (64.0 * 128.0 + 100.0) / 16383.0, 1.0e-6), "CC 33 makes it a 14-bit value");

        block (m, { { cc (1, 127), 0 }, { cc (33, 127), 0 } });
        settle (m);
        check (near (m.getWheelPosition(), 1.0, 1.0e-6), "14-bit full scale is 1");

        block (m, { { bendMessage (16383), 0 } });
        auto b = settle (m);
        check (b.pitchRatio != nullptr, "wheel and bend up: the pitch is modulated");

        block (m, { { cc (121, 0, 5), 0 } });
        b = settle (m, 8);
        check (b.pitchRatio == nullptr, "CC 121 (any channel): bend centred, wheel at 0, the buffer null again");
        check (m.getBendPosition() == 0.0f && m.getWheelPosition() == 0.0f, "the panel shows them reset");

        // A block with more controller messages than the list holds ends on the latest value.
        juce::MidiBuffer midi;
        for (int i = 0; i < 300; ++i)
            midi.addEvent (bendMessage (i < 299 ? 0 : 16383), i);
        m.handleMidi (midi, blockSize);
        m.render (blockSize, transport);
        b = settle (m);
        check (b.pitchRatio != nullptr && near (semitones (b.pitchRatio[0]), 2.0, 1.0e-4), "overflowing the event list keeps the last value");
    }

    //==========================================================================
    std::printf ("Vibrato\n");
    {
        Modulation m;
        start (m, {});                               // 30 cents at the wheel's top
        block (m, { { cc (1, 127), 0 } });
        settle (m, 4);

        const int seconds = 2, n = seconds * (int) rate;
        double peak = 0.0, trough = 0.0;
        int upCrossings = 0;
        double previous = 0.0;
        for (int done = 0; done < n; done += blockSize)
        {
            const auto b = block (m, {});
            if (b.pitchRatio == nullptr)
                break;
            for (int i = 0; i < blockSize; ++i)
            {
                const double c = 100.0 * semitones (b.pitchRatio[i]);
                peak = std::max (peak, c);
                trough = std::min (trough, c);
                if (previous < 0.0 && c >= 0.0)
                    ++upCrossings;
                previous = c;
            }
        }
        check (near (peak, 30.0, 0.05) && near (trough, -30.0, 0.05), "wheel at 1: the pitch swings 30 cents each way");
        check (upCrossings >= 10 && upCrossings <= 12, "at 5.5 Hz (11 cycles in 2 s)");

        block (m, { { cc (1, 0), 0 } });
        const auto b = settle (m, 4);
        check (b.pitchRatio == nullptr, "wheel at 0: no vibrato, null buffer");

        Modulation off;
        Modulation::Settings s;
        s.vibratoCents = 0.0f;
        start (off, s);
        block (off, { { cc (1, 127), 0 } });
        check (settle (off).pitchRatio == nullptr, "Vibrato 0: the wheel does nothing");
    }

    //==========================================================================
    std::printf ("LFO depths (with known LFO values)\n");
    std::vector<float> lfoValues ((size_t) Modulation::minBufferSize);
    {
        for (size_t i = 0; i < lfoValues.size(); ++i)   // one cycle per 480 samples (100 Hz), -1..+1
            lfoValues[i] = (float) std::sin (juce::MathConstants<double>::twoPi * (double) i / 480.0 - juce::MathConstants<double>::halfPi);

        Modulation::Settings s;
        s.lfoAmp = 1.0f;
        Modulation m;
        start (m, s);
        m.setLfoValuesForTesting (lfoValues.data());
        auto b = block (m, {});
        check (b.pitchRatio == nullptr && b.cutoffOctaves == nullptr, "AMP alone leaves pitch and cutoff null");

        juce::AudioBuffer<float> audio (2, blockSize);
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (audio.getWritePointer (ch), 1.0f, blockSize);
        m.applyAmplitude (audio, blockSize);
        float top = 0.0f, bottom = 2.0f;
        for (int i = 0; i < blockSize; ++i)
        {
            top = std::max (top, audio.getSample (1, i));
            bottom = std::min (bottom, audio.getSample (1, i));
        }
        check (top <= 1.0f && near (top, 1.0, 1.0e-6), "the tremolo never goes above 1 (it reaches 1 at the LFO's top)");
        check (bottom == 0.0f && audio.getSample (0, 0) == 0.0f, "AMP 1 reaches 0 at the LFO's minimum");

        s.lfoAmp = 0.5f;
        Modulation half;
        start (half, s);
        half.setLfoValuesForTesting (lfoValues.data());
        block (half, {});
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (audio.getWritePointer (ch), 1.0f, blockSize);
        half.applyAmplitude (audio, blockSize);
        const auto range = audio.findMinMax (0, 0, blockSize);
        check (near (range.getStart(), 0.5, 1.0e-6) && range.getEnd() <= 1.0f, "AMP 0.5 dips to 0.5");

        s = {};
        s.lfoPitchSemitones = 12.0f;
        s.lfoCutoffOctaves = -3.0f;
        Modulation pc;
        start (pc, s);
        pc.setLfoValuesForTesting (lfoValues.data());
        b = block (pc, {});
        check (b.pitchRatio != nullptr && near (b.pitchRatio[240], 2.0, 1.0e-5) && near (b.pitchRatio[0], 0.5, 1.0e-5),
               "LFO PITCH 12: an octave up at the LFO's top, down at its bottom");
        check (b.cutoffOctaves != nullptr && near (b.cutoffOctaves[240], -3.0, 1.0e-5) && near (b.cutoffOctaves[0], 3.0, 1.0e-5),
               "LFO CUTOFF -3: the LFO times the depth, in octaves");
    }

    std::printf ("Depth smoothing\n");
    {
        std::vector<float> ones ((size_t) Modulation::minBufferSize, 1.0f);
        Modulation m;
        start (m, {});
        m.setLfoValuesForTesting (ones.data());
        Modulation::Settings s;
        s.lfoCutoffOctaves = 4.0f;
        m.setSettings (s);   // a change after the first block glides
        const auto b1 = m.render (blockSize, transport);   // (the buffers are reused by the next render)
        check (b1.cutoffOctaves != nullptr && b1.cutoffOctaves[0] > 0.0f && b1.cutoffOctaves[0] < 0.01f,
               "a depth change starts gently");
        const auto b2 = m.render (blockSize, transport);
        const int glide = (int) (0.02 * rate);   // 960 samples
        check (near (b2.cutoffOctaves[glide - blockSize - 1], 4.0, 1.0e-5) && b2.cutoffOctaves[glide - blockSize - 2] < 4.0f,
               "and arrives after 20 ms");

        s.lfoCutoffOctaves = 0.0f;
        m.setSettings (s);
        const auto b3 = m.render (blockSize, transport);
        check (b3.cutoffOctaves != nullptr && b3.cutoffOctaves[0] < 4.0f && b3.cutoffOctaves[0] > 3.9f,
               "turning it off fades it out (the buffer stays while it fades)");
        m.render (blockSize, transport);
        check (m.render (blockSize, transport).cutoffOctaves == nullptr, "and the buffer is null once it has faded");

        Modulation snapped;
        s.lfoCutoffOctaves = 4.0f;
        start (snapped, s);   // settings given right after prepare(): no fade in
        snapped.setLfoValuesForTesting (ones.data());
        check (snapped.render (blockSize, transport).cutoffOctaves[0] == 4.0f, "after prepare() the first settings apply at once");

        // The tremolo depth fades too, so turning AMP up doesn't click.
        Modulation amp;
        start (amp, {});
        std::vector<float> minusOnes ((size_t) Modulation::minBufferSize, -1.0f);
        amp.setLfoValuesForTesting (minusOnes.data());
        Modulation::Settings a;
        a.lfoAmp = 1.0f;
        amp.setSettings (a);
        amp.render (blockSize, transport);
        juce::AudioBuffer<float> audio (1, blockSize);
        juce::FloatVectorOperations::fill (audio.getWritePointer (0), 1.0f, blockSize);
        amp.applyAmplitude (audio, blockSize);
        check (audio.getSample (0, 0) > 0.99f && audio.getSample (0, blockSize - 1) < audio.getSample (0, 0), "AMP fades in over 20 ms");
    }

    //==========================================================================
    std::printf ("Repeatability\n");
    {
        Modulation::Settings s;
        s.lfoPitchSemitones = 1.0f;
        s.lfoAmp = 0.5f;
        Modulation m;
        start (m, s);

        auto run = [&m]
        {
            std::vector<float> out;
            for (int k = 0; k < 20; ++k)
            {
                const auto b = k == 2 ? block (m, { { bendMessage (12000), 37 }, { cc (1, 90), 200 } })
                             : k == 9 ? block (m, { { cc (121, 0), 5 } })
                                      : block (m, {});
                for (int i = 0; i < blockSize; ++i)
                    out.push_back (b.pitchRatio != nullptr ? b.pitchRatio[i] : 1.0f);
            }
            return out;
        };

        const auto first = run();
        m.reset();
        const auto second = run();
        check (first.size() == second.size() && std::memcmp (first.data(), second.data(), first.size() * sizeof (float)) == 0,
               "a render from reset() repeats exactly");
        check (*std::max_element (first.begin(), first.end()) > 1.0f, "(and the render was modulated)");
    }

    //==========================================================================
    std::printf ("Blocks longer than the buffers\n");
    {
        Modulation::Settings s;
        s.lfoAmp = 1.0f;
        Modulation m;
        start (m, s);   // capacity: 8192
        std::vector<float> minusOnes ((size_t) Modulation::minBufferSize, -1.0f);
        m.setLfoValuesForTesting (minusOnes.data());

        const int longBlock = 10000;
        juce::MidiBuffer midi;
        midi.addEvent (bendMessage (16383), 9000);   // beyond the capacity
        m.handleMidi (midi, longBlock);
        const auto b = m.render (longBlock, transport);
        check (b.numSamples == Modulation::minBufferSize && b.pitchRatio == nullptr, "the block is cut to the capacity (and the late bend isn't in it)");

        juce::AudioBuffer<float> audio (2, longBlock);
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (audio.getWritePointer (ch), 1.0f, longBlock);
        m.applyAmplitude (audio, longBlock);
        check (audio.getSample (1, longBlock - 1) == audio.getSample (1, Modulation::minBufferSize - 1) && audio.getSample (1, longBlock - 1) == 0.0f,
               "the tremolo holds its last gain beyond the capacity");

        const auto next = m.render (blockSize, transport);
        check (next.pitchRatio != nullptr && near (semitones (next.pitchRatio[blockSize - 1]), 2.0, 1.0e-4),
               "a bend beyond the capacity still applies in the next block");

        Modulation big;
        start (big, s, 16384);
        big.setLfoValuesForTesting (nullptr);
        check (big.render (16384, transport).numSamples == 16384, "a prepared block size above 8192 sizes the buffers to it");
    }

    std::printf (failures == 0 ? "All tests passed.\n" : "%d test(s) FAILED.\n", failures);
    return failures == 0 ? 0 : 1;
}
