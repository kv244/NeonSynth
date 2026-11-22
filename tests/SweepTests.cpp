// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Sample-rate and block-size sweep (v1.11.0), through the whole processor.

    Every source (the Oscillator's four waveforms, Random, Sample and Granular, both playing a
    sine file so their pitch can be measured) and the sequencer are rendered at 44.1, 48, 88.2,
    96 and 192 kHz, each with host blocks of 1, 31, 64, 512 and 4096 samples and with a sequence
    of changing sizes, as some hosts send. For each:
      - the output is finite and stays under the soft clipper's ceiling;
      - the note sounds at its pitch (the strongest line within a semitone of it is the note's);
      - the audio is identical, sample for sample, whatever the block sizes: the engine places
        every MIDI event on its sample, advances its smoothers, filter updates, LFO and grain
        clock per sample, and never lets a block boundary change the arithmetic;
      - the sequencer's notes start on the exact sample its timing model gives
        (ceil (step beat x samples per beat)), on the internal clock and following a host
        transport at a tempo whose steps fall between samples.

    Hooks for the v1.11.0 integration (one function each, marked HOOK below): glide, tie, slide
    and pattern length are written by other agents in parallel, so their sweeps are added by
    the lead once the branches are merged. Each hook says what it should check; until then it
    prints HOOK and counts as neither a pass nor a failure.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target SweepTests
    Run the resulting SweepTests executable; it exits non-zero on failure.
*/
#include "plugin/NeonSynthProcessor.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <functional>
#include <vector>
#include <algorithm>

namespace
{
    int failures = 0;

    void check (bool condition, const juce::String& what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what.toRawUTF8());
        if (! condition)
            ++failures;
    }

    void hook (const char* what)
    {
        std::printf ("  HOOK  %s: added by the lead after the v1.11.0 integration\n", what);
    }

    const double sampleRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };

    /** Host block sizes: fixed ones, or (empty sizes) the changing sequence. */
    struct BlockPlan
    {
        juce::String name;
        std::vector<int> sizes;   // repeated in turn
    };

    const std::vector<BlockPlan>& blockPlans()
    {
        static const std::vector<BlockPlan> plans {
            { "1",    { 1 } },    { "31",   { 31 } },   { "64",   { 64 } },
            { "512",  { 512 } },  { "4096", { 4096 } },
            { "varying", { 1, 7, 512, 33, 4096, 128, 2, 999, 64, 31, 3000, 5 } } };
        return plans;
    }

    constexpr int maxBlockSize = 4096;

    void setParameter (juce::AudioProcessor& p, const juce::String& id, float value)
    {
        for (auto* param : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param); ranged != nullptr && ranged->getParameterID() == id)
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
    }

    /** A host transport that plays from beat 0 at a fixed tempo, in 4/4. */
    struct HostPlayHead : juce::AudioPlayHead
    {
        double sampleRate = 48000.0, bpm = 120.0;
        juce::int64 sample = 0;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setIsPlaying (true);
            info.setBpm (bpm);
            info.setTimeSignature (TimeSignature { 4, 4 });
            info.setTimeInSamples (sample);
            info.setPpqPosition ((double) sample / sampleRate * bpm / 60.0);
            return info;
        }
    };

    struct Note
    {
        double seconds;
        int note;
        bool on;
    };

    struct Scenario
    {
        juce::String name;
        std::function<void (NeonSynthProcessor&)> setup;
        std::vector<Note> notes;
        double seconds = 0.6;
        double hostBpm = 0.0;   // > 0: follow a host transport at this tempo
    };

    /** The sine file the Sample and Granular sources play: C4, 2 s, 48 kHz. */
    juce::File sineFile()
    {
        static const juce::File file = []
        {
            auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("NeonSynthSweepTests-C4.wav");
            f.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream> (f);
            auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions{}.withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (24));
            juce::AudioBuffer<float> b (1, 96000);
            const double hz = juce::MidiMessage::getMidiNoteInHertz (60);
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / 48000.0));
            writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            return f;
        }();
        return file;
    }

    /** Renders a scenario at one sample rate with one block plan; returns the left channel. */
    std::vector<float> render (const Scenario& scenario, double fs, const BlockPlan& plan)
    {
        NeonSynthProcessor p;
        HostPlayHead playHead;
        playHead.sampleRate = fs;
        playHead.bpm = scenario.hostBpm;
        if (scenario.hostBpm > 0.0)
            p.setPlayHead (&playHead);

        scenario.setup (p);
        p.setRateAndBufferSizeDetails (fs, maxBlockSize);
        p.prepareToPlay (fs, maxBlockSize);
        p.reset();

        const auto total = (juce::int64) std::llround (scenario.seconds * fs);
        std::vector<float> out ((size_t) total);
        juce::AudioBuffer<float> buffer (2, maxBlockSize);
        juce::MidiBuffer midi;
        size_t nextSize = 0;

        for (juce::int64 start = 0; start < total;)
        {
            const int n = (int) std::min<juce::int64> (plan.sizes[nextSize++ % plan.sizes.size()], total - start);
            midi.clear();
            for (const auto& note : scenario.notes)
            {
                const auto at = (juce::int64) std::llround (note.seconds * fs);
                if (at >= start && at < start + n)
                    midi.addEvent (note.on ? juce::MidiMessage::noteOn (1, note.note, (juce::uint8) 100)
                                           : juce::MidiMessage::noteOff (1, note.note), (int) (at - start));
            }
            playHead.sample = start;
            buffer.setSize (2, n, false, false, true);
            p.processBlock (buffer, midi);
            std::memcpy (out.data() + start, buffer.getReadPointer (0), (size_t) n * sizeof (float));
            start += n;
        }

        p.setPlayHead (nullptr);
        p.releaseResources();
        return out;
    }

    bool allFiniteAndClipped (const std::vector<float>& x)
    {
        for (auto v : x)
            if (! std::isfinite (v) || std::abs (v) > NeonSynthProcessor::softClipCeiling)
                return false;
        return true;
    }

    /** Power at one frequency between two times (Goertzel, Hann window). */
    double power (const std::vector<float>& x, double fs, double hz, double fromSeconds, double toSeconds)
    {
        const auto first = (size_t) (fromSeconds * fs), last = std::min (x.size(), (size_t) (toSeconds * fs));
        const double w = juce::MathConstants<double>::twoPi * hz / fs, coefficient = 2.0 * std::cos (w);
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

    /** Cents from `hz` to the strongest line within a semitone of it (5-cent steps). */
    int strongestLineCents (const std::vector<float>& x, double fs, double hz, double fromSeconds, double toSeconds)
    {
        int best = 0;
        double bestPower = -1.0;
        for (int c = -100; c <= 100; c += 5)
        {
            const double p = power (x, fs, hz * std::pow (2.0, c / 1200.0), fromSeconds, toSeconds);
            if (p > bestPower)
            {
                bestPower = p;
                best = c;
            }
        }
        return best;
    }

    /** Largest difference between two renders (0 = identical, bit for bit when sameBits is true). */
    bool sameBits (const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
    }

    float largestDifference (const std::vector<float>& a, const std::vector<float>& b)
    {
        float d = 0.0f;
        for (size_t i = 0; i < std::min (a.size(), b.size()); ++i)
            d = std::max (d, std::abs (a[i] - b[i]));
        return d;
    }

    //==========================================================================
    Scenario sourceScenario (const juce::String& name, int source, int waveform, int note)
    {
        Scenario s;
        s.name = name;
        s.setup = [source, waveform] (NeonSynthProcessor& p)
        {
            setParameter (p, "source", (float) source);
            setParameter (p, "waveform", (float) waveform);
            setParameter (p, "width", 0.0f);
            setParameter (p, "sustain", 1.0f);
            setParameter (p, "grJitter", 0.0f);
            if (source == 1 || source == 2)
            {
                juce::String error;
                if (! p.loadSampleFile (sineFile(), error))
                    std::printf ("    (the sine file did not load: %s)\n", error.toRawUTF8());
            }
        };
        // Starts between block boundaries at most rates and block sizes; a second note joins
        // later, and the first is released before the end.
        s.notes = { { 0.0503, note, true }, { 0.3001, note + 7, true }, { 0.5007, note, false } };
        return s;
    }

    void testSources()
    {
        std::printf ("Every source at every sample rate and block size\n");
        struct Case { const char* name; int source, waveform, note; };
        const Case cases[] = {
            { "Oscillator, sine",     0, 0, 69 }, { "Oscillator, square",   0, 1, 69 },
            { "Oscillator, triangle", 0, 2, 69 }, { "Oscillator, sawtooth", 0, 3, 69 },
            { "Random",               3, 0, 57 }, { "Sample (a C4 sine)",   1, 0, 72 },
            { "Granular (a C4 sine)", 2, 0, 72 } };

        for (const auto& c : cases)
        {
            const auto scenario = sourceScenario (c.name, c.source, c.waveform, c.note);
            const double hz = juce::MidiMessage::getMidiNoteInHertz (c.note);
            bool finite = true, inTune = true, identical = true;
            juce::String detail;

            for (auto fs : sampleRates)
            {
                std::vector<float> reference;
                for (const auto& plan : blockPlans())
                {
                    const auto out = render (scenario, fs, plan);
                    finite = finite && allFiniteAndClipped (out);

                    if (reference.empty())
                    {
                        reference = out;
                        // The first note alone, from 0.1 s to 0.3 s.
                        const int cents = strongestLineCents (out, fs, hz, 0.1, 0.3);
                        if (cents != 0)
                        {
                            inTune = false;
                            detail << " " << juce::String (fs / 1000.0, 1) << " kHz: " << cents << " cents;";
                        }
                    }
                    else if (! sameBits (reference, out))
                    {
                        identical = false;
                        detail << " " << juce::String (fs / 1000.0, 1) << " kHz, blocks of " << plan.name << ": differs by up to "
                               << juce::String (largestDifference (reference, out), 8) << ";";
                    }
                }
            }
            check (finite, juce::String (c.name) + ": finite and under the clipper's ceiling everywhere");
            check (inTune, juce::String (c.name) + ": at its pitch at every sample rate" + detail);
            check (identical, juce::String (c.name) + ": identical audio for every block plan" + (identical ? juce::String() : detail));
        }
    }

    //==========================================================================
    /** All 16 steps on, short notes of noise (so a note's first sample is never zero), silence
        between them: each onset is the first non-zero sample after silence. */
    Scenario sequencerScenario (double hostBpm, float tempo, int rateIndex)
    {
        Scenario s;
        s.name = hostBpm > 0.0 ? "host transport" : "internal clock";
        s.hostBpm = hostBpm;
        s.seconds = 1.2;
        s.setup = [tempo, rateIndex] (NeonSynthProcessor& p)
        {
            for (int k = 0; k < StepSequencer::numSteps; ++k)
                p.setSequencerStep (k, { true, 48 + (k % 7), 100 });
            setParameter (p, "seqOn", 1.0f);
            setParameter (p, "seqSync", 1.0f);
            setParameter (p, "seqTempo", tempo);
            setParameter (p, "seqRate", (float) rateIndex);
            setParameter (p, "seqGate", 0.5f);
            setParameter (p, "noiseLevel", 100.0f);
            setParameter (p, "attack", 0.001f);
            setParameter (p, "release", 0.001f);
            setParameter (p, "filterEnabled", 0.0f);
        };
        return s;
    }

    std::vector<juce::int64> onsets (const std::vector<float>& x, int silence)
    {
        std::vector<juce::int64> found;
        int quiet = silence;
        for (size_t i = 0; i < x.size(); ++i)
        {
            if (x[i] != 0.0f)
            {
                if (quiet >= silence)
                    found.push_back ((juce::int64) i);
                quiet = 0;
            }
            else
            {
                ++quiet;
            }
        }
        return found;
    }

    /** Where the sequencer's timing model puts each step start inside `seconds`. */
    std::vector<juce::int64> expectedOnsets (double fs, double bpm, double stepBeats, double seconds)
    {
        std::vector<juce::int64> expected;
        const double samplesPerBeat = fs * 60.0 / bpm;
        for (int k = 0; ; ++k)
        {
            const auto at = (juce::int64) std::ceil ((double) k * stepBeats * samplesPerBeat - 1.0e-6);
            if (at >= (juce::int64) std::llround (seconds * fs))
                break;
            expected.push_back (at);
        }
        return expected;
    }

    void testSequencer()
    {
        std::printf ("Sequencer notes on their samples\n");
        struct Case { double hostBpm; float tempo; int rateIndex; double stepBeats; const char* rateName; };
        const Case cases[] = {
            { 0.0,   120.0f, 1, 0.25,       "1/16" },    // internal clock: 120 bpm
            { 0.0,   137.0f, 3, 1.0 / 3.0,  "1/8T" },    // internal clock, steps between samples
            { 133.0, 120.0f, 1, 0.25,       "1/16" },    // following a host at 133 bpm
            { 133.0, 120.0f, 2, 0.125,      "1/32" } };

        for (const auto& c : cases)
        {
            const auto scenario = sequencerScenario (c.hostBpm, c.tempo, c.rateIndex);
            const double bpm = c.hostBpm > 0.0 ? c.hostBpm : (double) c.tempo;
            bool onTime = true, identical = true, finite = true;
            juce::String detail;

            for (auto fs : sampleRates)
            {
                const auto expected = expectedOnsets (fs, bpm, c.stepBeats, scenario.seconds);
                std::vector<float> reference;
                for (const auto& plan : blockPlans())
                {
                    const auto out = render (scenario, fs, plan);
                    finite = finite && allFiniteAndClipped (out);
                    const auto got = onsets (out, (int) (0.002 * fs));
                    if (got != expected)
                    {
                        if (onTime)
                        {
                            const auto n = std::min (got.size(), expected.size());
                            size_t first = 0;
                            while (first < n && got[first] == expected[first])
                                ++first;
                            detail << " " << juce::String (fs / 1000.0, 1) << " kHz, blocks of " << plan.name << ": "
                                   << (int) got.size() << " notes for " << (int) expected.size() << ", the first wrong one at "
                                   << (first < got.size() ? juce::String (got[first]) : juce::String ("-")) << " for "
                                   << (first < expected.size() ? juce::String (expected[first]) : juce::String ("-")) << ";";
                        }
                        onTime = false;
                    }
                    if (reference.empty())
                        reference = out;
                    else
                        identical = identical && sameBits (reference, out);
                }
            }

            const auto name = juce::String (scenario.name) + ", " + c.rateName + " at " + juce::String (bpm, 0) + " bpm";
            check (finite, name + ": finite and under the clipper's ceiling");
            check (onTime, name + ": every step starts on its sample at every rate and block plan" + detail);
            check (identical, name + ": identical audio for every block plan");
        }
    }

    //==========================================================================
    // HOOKs for the lead, after integration. Each runs the same sweep (sampleRates x
    // blockPlans(), render() and the helpers above) for one v1.11.0 feature.

    /** GLIDE (agent B): a note gliding to another at GLIDE 200 ms reaches the target pitch in
        200 ms at every rate, and the glide is identical for every block plan. */
    void sweepGlide()   { hook ("glide reaches its pitch in GLIDE ms, identical across block sizes"); }

    /** TIE (agent A): a tied step sends no note-on (one onset where two steps are tied) and the
        note ends at the last tied step's gate, on its sample, at every rate and block plan. */
    void sweepTie()     { hook ("tie: no retrigger, the note-off on its sample"); }

    /** SLIDE (agents A and B): the next step's note-on comes before the sliding note's note-off,
        and the voice glides instead of restarting (one onset), at every rate and block plan. */
    void sweepSlide()   { hook ("slide: overlap in the contract's order, one onset, the glide"); }

    /** LENGTH (agent A): with LENGTH 5 and 7 the steps wrap after 5 or 7 and still start on
        their samples (expectedOnsets() with the wrapped pattern) at every rate and block plan. */
    void sweepLength()  { hook ("pattern length: wraps at LENGTH, steps on their samples"); }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // the processor's sample loader posts to the message thread

    testSources();
    testSequencer();

    std::printf ("v1.11.0 features (after integration)\n");
    sweepGlide();
    sweepTie();
    sweepSlide();
    sweepLength();

    sineFile().deleteFile();

    if (failures == 0)
        std::printf ("SweepTests: all passed\n");
    else
        std::printf ("SweepTests: %d FAILED\n", failures);

    return failures == 0 ? 0 : 1;
}
