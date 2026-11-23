// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Render benchmark: times NeonSynthProcessor::processBlock directly,
    with no host or Python in the way, so the numbers are the plugin's own cost.

    Scenarios (48 kHz, 512-sample blocks, 60 s of audio each):
      idle      - no notes
      one note  - a single held note
      16 voices - 16 notes held at full velocity (soft clipper working hard)
      filter env 16 - the same 16 notes with the filter envelope at +3 octaves (decay 0.5 s,
                  sustain 0.3, v1.8.0): every voice's coefficients follow it through the decay,
                  then the voices hold at sustain in the steady version
      lfo cutoff 16 / vibrato 16 / bend granular - v1.9.0 modulation on the 16 held notes: the LFO on
                  the cutoff (every voice glides), the mod wheel's vibrato, and a slow bend on the granular source
      sweep 16  - the same 16 notes with the cutoff knob moved every block (a slow
                  200 Hz..8 kHz sweep), so every voice's filter glides all the time
      resonant 16 - the same 16 notes with resonance 90% and the filter's drive at 50%
                  (v1.8.0): the nonlinear filter path and the drive stage in every voice
      sample 16 - the same 16 notes playing the built-in pad (Source = Sample, looped)
      random 16 - the same 16 notes on the Random source
      sculpt 16 - the same 16 notes on a sawtooth with CHOP, Noise and Fold all on (v1.6.0)
      granular  - the same 16 notes on the granular source at 200 grains/s of 250 ms,
                  so every voice runs its full 32 grains (the worst case)
      dense     - sequencer at 1/32 and 240 BPM plus an 8-note chord every 250 ms

    It first times creating a processor (what a host does when it loads the plugin):
    the first instance in the process, then the average of the next ten.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --config Release --target RenderBenchmark
    Prints, per scenario, the render time and the share of one CPU core used
    in real time. Exits non-zero if any scenario renders a NaN or an infinity
    (checked outside the timed calls).

    RenderBenchmark --grains [rounds] (v1.11.1) times only the two granular scenarios,
    once per grain loop: AVX2 reading with the gather instruction, AVX2 reading with
    scalar loads, SSE2 and scalar (GrainCloud::SimdPath). The paths take turns, `rounds`
    times (default 3), and the fastest round of each counts, so a busy moment on the
    machine doesn't decide the result. It says which AVX2 read is faster on this CPU;
    the default (NEONSYNTH_GRAINS_AVX2_GATHER) is chosen from these numbers.
*/
#include "plugin/NeonSynthProcessor.h"
#include "dsp/GrainCloud.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace
{
    constexpr double benchSampleRate = 48000.0;
    constexpr int    benchBlockSize  = 512;
    constexpr double benchSeconds    = 60.0;

    void setParameter (juce::AudioProcessor& p, const juce::String& id, float value)
    {
        for (auto* param : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param); ranged != nullptr && ranged->getParameterID() == id)
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
    }

    using BlockHook = std::function<void (juce::AudioProcessor&, juce::int64 blockStart)>;

    // Whether the last render() gave only finite samples, and the scenarios that did not. The check
    // runs after each block's timing stops, so it costs the measurement nothing; a NaN or an
    // infinity in any scenario fails the program, so CI and scripts notice.
    bool lastRenderFinite = true;
    juce::StringArray nonFiniteScenarios;

    bool allFinite (const juce::AudioBuffer<float>& buffer)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (! std::isfinite (buffer.getSample (ch, i)))
                    return false;
        return true;
    }

    /** Renders `benchSeconds` of audio; `addMidi` fills each block's MIDI and `beforeBlock`, if given,
        changes parameters before each block (not timed). Returns milliseconds spent in processBlock. */
    double render (const std::function<void (juce::MidiBuffer&, juce::int64 blockStart)>& addMidi,
                   const std::function<void (juce::AudioProcessor&)>& configure,
                   const BlockHook& beforeBlock = {})
    {
        NeonSynthProcessor processor;
        configure (processor);
        processor.setRateAndBufferSizeDetails (benchSampleRate, benchBlockSize);
        processor.prepareToPlay (benchSampleRate, benchBlockSize);

        juce::AudioBuffer<float> buffer (2, benchBlockSize);
        juce::MidiBuffer midi;
        double total = 0.0;
        float peak = 0.0f;
        lastRenderFinite = true;

        const auto blocks = (juce::int64) (benchSeconds * benchSampleRate / benchBlockSize);
        for (juce::int64 b = 0; b < blocks; ++b)
        {
            midi.clear();
            addMidi (midi, b * benchBlockSize);
            if (beforeBlock)
                beforeBlock (processor, b * benchBlockSize);
            const auto start = std::chrono::steady_clock::now();
            processor.processBlock (buffer, midi);
            total += std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
            peak = juce::jmax (peak, buffer.getMagnitude (0, benchBlockSize));
            lastRenderFinite = lastRenderFinite && allFinite (buffer);
        }
        processor.releaseResources();
        std::printf ("        (output peak %.1f dBFS)\n", juce::Decibels::gainToDecibels (peak));
        return total;
    }

    void report (const char* name, double ms)
    {
        std::printf ("  %-13s %8.0f ms for %.0f s   %5.2f%% of one core\n", name, ms, benchSeconds, 100.0 * ms / (benchSeconds * 1000.0));
        if (! lastRenderFinite)
        {
            std::printf ("  FAIL  %s rendered a NaN or an infinity\n", name);
            nonFiniteScenarios.add (name);
        }
    }

    /** Adds the events of a note that is held for the whole render (note-on in the first block only). */
    std::function<void (juce::MidiBuffer&, juce::int64)> held (std::initializer_list<int> notes, int velocity)
    {
        std::vector<int> list (notes);
        return [list, velocity] (juce::MidiBuffer& midi, juce::int64 blockStart)
        {
            if (blockStart == 0)
                for (int n : list)
                    midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) velocity), 0);
        };
    }

    // The two granular scenarios, shared by the full run and --grains.
    void configureGranular (juce::AudioProcessor& p)
    {
        setParameter (p, "source", 2.0f);       // 16 voices x 32 grains: the worst case
        setParameter (p, "grDensity", 200.0f);
        setParameter (p, "grSize", 250.0f);
        setParameter (p, "grSpray", 0.5f);
        setParameter (p, "grJitter", 20.0f);
    }

    void configureBendGranular (juce::AudioProcessor& p)
    {
        setParameter (p, "source", 2.0f);   // the cloud renders in 32-sample chunks while bent
        setParameter (p, "grDensity", 200.0f);
        setParameter (p, "grSize", 250.0f);
        setParameter (p, "bendRange", 12.0f);
    }

    std::function<void (juce::MidiBuffer&, juce::int64)> heldSixteen()
    {
        return held ({ 48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74 }, 127);
    }

    std::function<void (juce::MidiBuffer&, juce::int64)> bentSixteen()
    {
        return [sixteen = heldSixteen()] (juce::MidiBuffer& midi, juce::int64 blockStart)
        {
            sixteen (midi, blockStart);
            const double t = (double) blockStart / benchSampleRate;   // a slow bend up and down, every block
            midi.addEvent (juce::MidiMessage::pitchWheel (1, 8192 + (int) (8000.0 * std::sin (t))), 0);
        };
    }

    /** --grains: the granular scenarios on each grain loop, to choose between gather and scalar loads. */
    int benchmarkGrainPaths (int rounds)
    {
        using Path = GrainCloud::SimdPath;
        struct Candidate { Path path; const char* name; double granular, bend; };
        std::vector<Candidate> candidates { { Path::avx2,      "avx2 gather", 1.0e300, 1.0e300 },
                                            { Path::avx2Loads, "avx2 loads",  1.0e300, 1.0e300 },
                                            { Path::sse2,      "sse2",        1.0e300, 1.0e300 },
                                            { Path::scalar,    "scalar",      1.0e300, 1.0e300 } };

        std::printf ("Grain loops (48 kHz, %d-sample blocks, %.0f s per scenario, best of %d rounds)\n", benchBlockSize, benchSeconds, rounds);
        if (! GrainCloud::cpuHasAVX2())
        {
            std::printf ("  This CPU has no AVX2: both AVX2 rows would run SSE2, so they are left out.\n");
            candidates.erase (candidates.begin(), candidates.begin() + 2);
        }

        for (int round = 1; round <= rounds; ++round)
        {
            std::printf ("Round %d of %d\n", round, rounds);
            for (auto& c : candidates)
            {
                GrainCloud::forceSimdPath (c.path);
                std::printf ("  %s\n", c.name);
                const double granular = render (heldSixteen(), configureGranular);
                if (! lastRenderFinite) nonFiniteScenarios.add (juce::String (c.name) + " granular");
                const double bend = render (bentSixteen(), configureBendGranular);
                if (! lastRenderFinite) nonFiniteScenarios.add (juce::String (c.name) + " bend granular");
                c.granular = juce::jmin (c.granular, granular);
                c.bend     = juce::jmin (c.bend, bend);
            }
        }
        GrainCloud::forceSimdPath (Path::automatic);

        const auto share = [] (double ms) { return 100.0 * ms / (benchSeconds * 1000.0); };
        std::printf ("\n  %-12s %10s %8s %14s %8s\n", "grain loop", "granular", "", "bend granular", "");
        for (const auto& c : candidates)
            std::printf ("  %-12s %7.0f ms %6.2f%% %11.0f ms %6.2f%%\n", c.name, c.granular, share (c.granular), c.bend, share (c.bend));

        if (GrainCloud::cpuHasAVX2())
        {
            const auto& gather = candidates[0];
            const auto& loads  = candidates[1];
            const double total = (gather.granular + gather.bend) / (loads.granular + loads.bend);
            std::printf ("\n  gather / loads: granular %.3f, bend granular %.3f, both %.3f\n",
                         gather.granular / loads.granular, gather.bend / loads.bend, total);
            std::printf ("  On this CPU %s (default now: %s; ratios within about 0.03 of 1 are noise)\n",
                         total < 1.0 ? "gather is faster" : "scalar loads are faster",
                         NEONSYNTH_GRAINS_AVX2_GATHER ? "gather" : "scalar loads");
        }

        if (! nonFiniteScenarios.isEmpty())
        {
            std::printf ("\nFAILED: non-finite output in %s\n", nonFiniteScenarios.joinIntoString (", ").toRawUTF8());
            return 1;
        }
        return 0;
    }
}

// cppcheck-suppress constParameter ; main's signature is fixed by the language
int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juce;   // the processor's editor code needs the message manager

    if (argc > 1 && std::strcmp (argv[1], "--grains") == 0)
        return benchmarkGrainPaths (argc > 2 ? juce::jlimit (1, 20, std::atoi (argv[2])) : 3);

    const auto noChange = [] (juce::AudioProcessor&) {};

    const auto millisecondsToCreate = []
    {
        const auto start = std::chrono::steady_clock::now();
        { NeonSynthProcessor processor; }
        return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
    };
    const double firstInstance = millisecondsToCreate();
    double laterInstances = 0.0;
    for (int i = 0; i < 10; ++i)
        laterInstances += millisecondsToCreate() / 10.0;
    std::printf ("New instance: %.1f ms the first time in the process, %.1f ms after that\n\n", firstInstance, laterInstances);

    std::printf ("Render benchmark (48 kHz, %d-sample blocks)\n", benchBlockSize);

    report ("idle",     render ([] (juce::MidiBuffer&, juce::int64) {}, noChange));
    report ("one note", render (held ({ 60 }, 100), noChange));
    report ("16 voices", render (held ({ 48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74 }, 127), noChange));
    report ("filter env 16", render (held ({ 48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74 }, 127),
                                     [] (juce::AudioProcessor& p)
                                     {
                                         setParameter (p, "filterEnvAmt", 3.0f);
                                         setParameter (p, "filterDecay", 0.5f);
                                         setParameter (p, "filterSustain", 0.3f);
                                     }));
    report ("sweep 16", render (held ({ 48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74 }, 127), noChange,
                                [] (juce::AudioProcessor& p, juce::int64 blockStart)
                                {
                                    // A new cutoff every block (10.7 ms), less than the 20 ms the cutoff
                                    // takes to glide, so the filters never come to rest: a 4 s cycle, in octaves.
                                    const double phase = (double) blockStart / benchSampleRate / 4.0;
                                    const double octaves = 2.66 * (1.0 - std::cos (juce::MathConstants<double>::twoPi * phase));
                                    setParameter (p, "cutoff", (float) (200.0 * std::exp2 (octaves)));
                                }));
    report ("resonant 16", render (held ({ 48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74 }, 127),
                                   [] (juce::AudioProcessor& p)
                                   {
                                       setParameter (p, "filterRes", 90.0f);
                                       setParameter (p, "filterDrive", 50.0f);
                                   }));
    report ("sample 16", render (held ({ 48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74 }, 127),
                                 [] (juce::AudioProcessor& p) { setParameter (p, "source", 1.0f); }));   // the built-in pad, looped
    report ("random 16", render (held ({ 48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74 }, 127),
                                 [] (juce::AudioProcessor& p) { setParameter (p, "source", 3.0f); }));   // the Random source
    report ("sculpt 16", render (held ({ 48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74 }, 127),
                                 [] (juce::AudioProcessor& p)
                                 {
                                     setParameter (p, "waveform", 3.0f);         // sawtooth
                                     setParameter (p, "chopOvertone", 100.0f);   // CHOP, Noise and Fold all on
                                     setParameter (p, "chopComb", 4.0f);
                                     setParameter (p, "noiseLevel", 30.0f);
                                     setParameter (p, "foldDrive", 5.0f);
                                     setParameter (p, "foldMix", 100.0f);
                                 }));
    report ("granular", render (heldSixteen(), configureGranular));

    // v1.9.0 modulation, on the same 16 held notes.
    const auto sixteen = held ({ 48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74 }, 127);
    report ("lfo cutoff 16", render (sixteen, [] (juce::AudioProcessor& p)
                                     {
                                         setParameter (p, "lfoCutoff", 2.0f);   // every voice glides, coefficients every 16 samples
                                         setParameter (p, "lfoRate", 4.0f);
                                     }));
    report ("vibrato 16", render ([sixteen] (juce::MidiBuffer& midi, juce::int64 blockStart)
                                  {
                                      sixteen (midi, blockStart);
                                      if (blockStart == 0)
                                          midi.addEvent (juce::MidiMessage::controllerEvent (1, 1, 127), 0);   // mod wheel up: pitch moves every sample
                                  }, noChange));
    report ("bend granular", render (bentSixteen(), configureBendGranular));

    const auto dense = [] (juce::MidiBuffer& midi, juce::int64 blockStart)
    {
        // An 8-note chord every 250 ms (12000 samples), each note 200 ms long.
        for (juce::int64 s = blockStart; s < blockStart + benchBlockSize; ++s)
        {
            const auto inChord = s % 12000;
            for (int k = 0; k < 8; ++k)
            {
                if (inChord == k * 144)        midi.addEvent (juce::MidiMessage::noteOn  (1, 48 + 3 * k, (juce::uint8) 100), (int) (s - blockStart));
                if (inChord == 9600 + k * 144) midi.addEvent (juce::MidiMessage::noteOff (1, 48 + 3 * k), (int) (s - blockStart));
            }
        }
    };
    report ("dense", render (dense, [] (juce::AudioProcessor& p)
    {
        setParameter (p, "seqOn", 1.0f);
        setParameter (p, "seqSync", 0.0f);
        setParameter (p, "seqRate", 2.0f);      // 1/32
        setParameter (p, "seqTempo", 240.0f);
    }));

    if (! nonFiniteScenarios.isEmpty())
    {
        std::printf ("\nFAILED: non-finite output in %s\n", nonFiniteScenarios.joinIntoString (", ").toRawUTF8());
        return 1;
    }
    std::printf ("\nEvery scenario's output was finite.\n");
    return 0;
}
