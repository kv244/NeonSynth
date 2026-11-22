// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    The playing features of v1.11.0: GLIDE, LEGATO, HOLD and the voice side of the
    sequencer's SLIDE.

      - GLIDE reaches the new pitch in the set time, in a straight line in octaves, for
        several intervals up and down (the voice's pitch, read every 32-sample block, and
        the audio's frequency once it has arrived); a glide is sounded by every source.
      - LEGATO (Voices at 1) keeps the envelope running when a second key overlaps, plays
        the new key, and returns to the key still held; with more Voices it does nothing.
      - HOLD keeps keyboard notes after their keys go up and releases them all when it is
        switched off; the sequencer's notes pass through it; in Transpose mode it latches.
      - SLIDE: the sequencer's slide MIDI (channel 16, one sample: CC 68 = 127, the new
        note-on, the old note-off, CC 68 = 0) glides the voice holding the old note instead
        of starting another, also to the same pitch; unmarked overlaps play exactly as before.
      - At their defaults (GLIDE 0, LEGATO off, HOLD off) nothing changes: the renders are
        identical to ones that never touch them, and LEGATO with 16 voices, or HOLD switched
        on and off with no key down, change nothing either. (That the defaults render
        exactly as v1.10.1 did is checked by tests/compare_versions.py against the release.)
      - Click tests: switching GLIDE, LEGATO and HOLD while notes sound, a legato step and
        a slide never make a step in the output larger than the steady tone's own.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target PlayingTests
    Run the resulting PlayingTests executable; it exits non-zero on failure.
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

    void check (bool condition, const char* what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what);
        if (! condition)
            ++failures;
    }

    constexpr double rate = 48000.0;
    constexpr int    blockSize = 32;   // small, so the voice views show the pitch finely

    void setParameter (juce::AudioProcessor& p, const juce::String& id, float value)
    {
        for (auto* param : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param); ranged != nullptr && ranged->getParameterID() == id)
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
    }

    /** Something that happens at an absolute sample: a MIDI message, or a parameter change
        (applied before the block that starts at or after it). */
    struct Event
    {
        juce::int64 sample;
        juce::MidiMessage message;
        juce::String parameter {};
        float value = 0.0f;
    };

    Event noteOn  (double seconds, int note, int velocity = 100) { return { (juce::int64) (seconds * rate), juce::MidiMessage::noteOn (1, note, (juce::uint8) velocity), {}, 0.0f }; }
    Event noteOff (double seconds, int note)                     { return { (juce::int64) (seconds * rate), juce::MidiMessage::noteOff (1, note), {}, 0.0f }; }
    Event change  (double seconds, const juce::String& id, float value)
    {
        return { (juce::int64) (seconds * rate), juce::MidiMessage(), id, value };
    }

    struct Render
    {
        std::vector<float> audio;     // left channel
        std::vector<float> pitchHz;   // voice 0's frequency after each block (its voice view)
        std::vector<int>   voicesOn;  // active voices after each block
    };

    /** Renders `seconds` through a fresh processor: sine, filter off, width 0, sustain 1,
        then `configure`, then the events at their samples. */
    Render render (const std::function<void (NeonSynthProcessor&)>& configure, const std::vector<Event>& events, double seconds)
    {
        NeonSynthProcessor p;
        setParameter (p, "waveform", 0.0f);
        setParameter (p, "filterEnabled", 0.0f);
        setParameter (p, "width", 0.0f);
        setParameter (p, "sustain", 1.0f);
        setParameter (p, "release", 0.05f);
        configure (p);
        p.setRateAndBufferSizeDetails (rate, blockSize);
        p.prepareToPlay (rate, blockSize);

        Render r;
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;
        const auto total = (juce::int64) (seconds * rate);
        for (juce::int64 start = 0; start < total; start += blockSize)
        {
            midi.clear();
            for (const auto& e : events)
            {
                if (e.parameter.isNotEmpty())
                {
                    if (e.sample > start - blockSize && e.sample <= start)
                        setParameter (p, e.parameter, e.value);
                }
                else if (e.sample >= start && e.sample < start + blockSize)
                {
                    midi.addEvent (e.message, (int) (e.sample - start));
                }
            }
            p.processBlock (buffer, midi);
            r.audio.insert (r.audio.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
            r.pitchHz.push_back (p.getVoiceViews()[0].frequencyHz.load());
            int on = 0;
            for (const auto& v : p.getVoiceViews())
                on += v.active.load() ? 1 : 0;
            r.voicesOn.push_back (on);
        }
        p.releaseResources();
        return r;
    }

    bool identical (const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
    }

    double midiHz (double note)   { return 440.0 * std::pow (2.0, (note - 69.0) / 12.0); }
    double cents (double hz, double reference)   { return 1200.0 * std::log2 (hz / reference); }

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

    double peak (const std::vector<float>& x, double fromSeconds, double toSeconds)
    {
        float most = 0.0f;
        for (auto i = (size_t) (fromSeconds * rate); i < std::min (x.size(), (size_t) (toSeconds * rate)); ++i)
            most = std::max (most, std::abs (x[i]));
        return (double) most;
    }

    float largestStep (const std::vector<float>& x, double fromSeconds, double toSeconds)
    {
        float most = 0.0f;
        for (auto i = (size_t) (fromSeconds * rate) + 1; i < std::min (x.size(), (size_t) (toSeconds * rate)); ++i)
            most = std::max (most, std::abs (x[i] - x[i - 1]));
        return most;
    }

    /** Voice 0's pitch (from its voice view) at a time, as a MIDI note. */
    double pitchAt (const Render& r, double seconds)
    {
        const auto block = std::min (r.pitchHz.size() - 1, (size_t) (seconds * rate) / (size_t) blockSize);
        return 69.0 + 12.0 * std::log2 ((double) r.pitchHz[block] / 440.0);
    }

    //==========================================================================
    void glideTests()
    {
        std::printf ("GLIDE reaches the new note in the set time, in a straight line in octaves\n");
        char what[256];
        const double glideMs = 200.0, t0 = 0.3;

        for (const int interval : { 12, -12, 7, -19, 24, 1 })
        {
            const int from = 57, to = from + interval;
            // Voices at 1: the second note lands on the voice that played the first.
            const auto r = render ([&] (NeonSynthProcessor& p)
                                   {
                                       setParameter (p, "voiceCount", 1.0f);
                                       setParameter (p, "glide", (float) glideMs);
                                   },
                                   { noteOn (0.0, from), noteOff (0.25, from), noteOn (t0, to), noteOff (1.0, to) }, 1.1);

            // Halfway through, half the interval (in semitones, so in octaves); a little before
            // the end not there yet; from GLIDE time on, there.
            const double half = pitchAt (r, t0 + 0.5 * glideMs * 0.001);
            const double nearEnd = pitchAt (r, t0 + 0.85 * glideMs * 0.001);
            const double after = pitchAt (r, t0 + glideMs * 0.001 + 0.002);
            const double heard = frequency (r.audio, t0 + glideMs * 0.001 + 0.05, 0.95);

            std::snprintf (what, sizeof (what), "%+3d semitones: halfway %.2f (expected %.2f), at 85%% %.2f, after %.3f (%d); %.1f Hz heard (%+.2f cents)",
                           interval, half, from + 0.5 * interval, nearEnd, after, to, heard, cents (heard, midiHz (to)));
            const double blockSemitones = std::abs ((double) interval) * blockSize / (glideMs * 0.001 * rate);   // one block's travel
            check (std::abs (half - (from + 0.5 * interval)) <= blockSemitones + 0.01
                     && std::abs (nearEnd - to) >= 0.15 * std::abs ((double) interval) - blockSemitones - 0.01
                     && std::abs (after - to) < 0.001
                     && std::abs (cents (heard, midiHz (to))) < 1.0,
                   what);
        }

        // The first note a voice plays has nothing to glide from: exactly as with GLIDE at 0.
        const std::vector<Event> line { noteOn (0.0, 60), noteOff (0.2, 60) };
        const auto plain = render ([] (NeonSynthProcessor&) {}, line, 0.4);
        const auto glided = render ([] (NeonSynthProcessor& p) { setParameter (p, "glide", 300.0f); }, line, 0.4);
        check (identical (plain.audio, glided.audio), "a voice's first note starts on its pitch, whatever GLIDE is (bit for bit)");

        // Every source glides: the sample and the grains follow the same pitch as the oscillators.
        for (const float source : { 1.0f, 3.0f })   // Sample, Random
        {
            const auto r = render ([&] (NeonSynthProcessor& p)
                                   {
                                       setParameter (p, "voiceCount", 1.0f);
                                       setParameter (p, "glide", 100.0f);
                                       setParameter (p, "source", source);
                                   },
                                   { noteOn (0.0, 48), noteOn (0.3, 60), noteOff (0.8, 60), noteOff (0.8, 48) }, 0.9);
            std::snprintf (what, sizeof (what), "the %s source glides too (halfway %.2f, then %.2f)",
                           source == 1.0f ? "Sample" : "Random", pitchAt (r, 0.35), pitchAt (r, 0.6));
            check (std::abs (pitchAt (r, 0.35) - 54.0) < 0.5 && std::abs (pitchAt (r, 0.6) - 60.0) < 0.001, what);
        }
    }

    //==========================================================================
    void legatoTests()
    {
        std::printf ("LEGATO (Voices at 1) keeps the envelope running and returns to the key still held\n");
        char what[256];

        // Decay to a low sustain first: a retrigger would attack back up to full level.
        const auto configure = [] (bool legato, float voices, float glideMs)
        {
            return [=] (NeonSynthProcessor& p)
            {
                setParameter (p, "voiceCount", voices);
                setParameter (p, "legato", legato ? 1.0f : 0.0f);
                setParameter (p, "glide", glideMs);
                setParameter (p, "attack", 0.005f);
                setParameter (p, "decay", 0.1f);
                setParameter (p, "sustain", 0.3f);
            };
        };
        const std::vector<Event> overlap { noteOn (0.0, 57), noteOn (0.3, 64), noteOff (0.6, 64), noteOff (0.9, 57) };

        for (const float glideMs : { 0.0f, 50.0f })
        {
            const auto legato = render (configure (true, 1.0f, glideMs), overlap, 1.1);
            const auto plain  = render (configure (false, 1.0f, glideMs), overlap, 1.1);

            const double before = peak (legato.audio, 0.25, 0.3);
            const double during = peak (legato.audio, 0.3, 0.4);
            const double retriggered = peak (plain.audio, 0.3, 0.4);
            std::snprintf (what, sizeof (what), "GLIDE %.0f ms: level stays at %.3f -> %.3f (retriggered: %.3f)", glideMs, before, during, retriggered);
            check (std::abs (during - before) < 0.02 * before && retriggered > 2.0 * before, what);

            const double second = frequency (legato.audio, 0.4, 0.59), back = frequency (legato.audio, 0.7, 0.89);
            std::snprintf (what, sizeof (what), "GLIDE %.0f ms: plays the new key (%.1f Hz) and returns to the held one (%.1f Hz)", glideMs, second, back);
            check (std::abs (cents (second, midiHz (64))) < 2.0 && std::abs (cents (back, midiHz (57))) < 2.0, what);

            const double returned = peak (legato.audio, 0.6, 0.85);
            std::snprintf (what, sizeof (what), "GLIDE %.0f ms: the return doesn't retrigger either (%.3f) and the last key up releases (%.4f after)", glideMs, returned, peak (legato.audio, 1.0, 1.1));
            check (std::abs (returned - before) < 0.02 * before && peak (legato.audio, 1.0, 1.1) < 1.0e-6, what);
        }

        // Releasing a key that isn't sounding changes nothing; pressing three and letting go of the
        // newest twice walks back down the stack.
        {
            const auto r = render (configure (true, 1.0f, 0.0f),
                                   { noteOn (0.0, 50), noteOn (0.1, 55), noteOn (0.2, 62), noteOff (0.3, 55), noteOff (0.4, 62), noteOff (0.6, 50) }, 0.7);
            const double third = frequency (r.audio, 0.3, 0.39), last = frequency (r.audio, 0.45, 0.59);
            std::snprintf (what, sizeof (what), "a key in the middle of the stack goes up: still %.1f Hz; then back to the first key, %.1f Hz", third, last);
            check (std::abs (cents (third, midiHz (62))) < 2.0 && std::abs (cents (last, midiHz (50))) < 2.0, what);
        }

        // With more voices LEGATO does nothing.
        for (const float voices : { 4.0f, 16.0f })
        {
            const auto on  = render (configure (true,  voices, 50.0f), overlap, 1.1);
            const auto off = render (configure (false, voices, 50.0f), overlap, 1.1);
            std::snprintf (what, sizeof (what), "with Voices at %.0f, LEGATO changes nothing (bit for bit)", voices);
            check (identical (on.audio, off.audio), what);
        }
    }

    //==========================================================================
    void holdTests()
    {
        std::printf ("HOLD keeps keyboard notes and releases them when switched off\n");
        char what[256];

        const auto r = render ([] (NeonSynthProcessor& p) { setParameter (p, "hold", 1.0f); },
                               { noteOn (0.0, 60), noteOn (0.05, 64), noteOff (0.1, 60), noteOff (0.15, 64),
                                 change (0.5, "hold", 0.0f) }, 0.8);
        const auto held = r.voicesOn[(size_t) (0.45 * rate) / blockSize];
        std::snprintf (what, sizeof (what), "both notes still sound after their keys went up (%d voices, level %.3f)", held, peak (r.audio, 0.4, 0.5));
        check (held == 2 && peak (r.audio, 0.4, 0.5) > 0.1, what);
        std::snprintf (what, sizeof (what), "switching HOLD off releases them (%d voices, level %.5f after the release)",
                       r.voicesOn.back(), peak (r.audio, 0.6, 0.8));
        check (r.voicesOn.back() == 0 && peak (r.audio, 0.6, 0.8) < 1.0e-6, what);

        // A key still down when HOLD goes off keeps sounding until it goes up.
        const auto down = render ([] (NeonSynthProcessor& p) { setParameter (p, "hold", 1.0f); },
                                  { noteOn (0.0, 60), change (0.2, "hold", 0.0f), noteOff (0.4, 60) }, 0.6);
        check (down.voicesOn[(size_t) (0.3 * rate) / blockSize] == 1 && down.voicesOn.back() == 0,
               "a key still down when HOLD goes off sounds until it is let go");

        // The sequencer's notes pass: its pattern sounds the same with HOLD on.
        const auto sequenced = [] (bool hold)
        {
            return render ([=] (NeonSynthProcessor& p)
                           {
                               setParameter (p, "seqOn", 1.0f);
                               setParameter (p, "seqSync", 0.0f);
                               setParameter (p, "hold", hold ? 1.0f : 0.0f);
                           }, {}, 1.5);
        };
        check (identical (sequenced (true).audio, sequenced (false).audio), "the sequencer's notes are not held (bit for bit)");

        // Transpose mode: HOLD latches the key, so the pattern goes on after it is let go.
        const auto transpose = render ([] (NeonSynthProcessor& p)
                                       {
                                           setParameter (p, "seqOn", 1.0f);
                                           setParameter (p, "seqSync", 0.0f);
                                           setParameter (p, "seqMode", 2.0f);
                                           setParameter (p, "hold", 1.0f);
                                       },
                                       { noteOn (0.0, 60), noteOff (0.1, 60), change (1.5, "hold", 0.0f) }, 2.2);
        std::snprintf (what, sizeof (what), "Transpose mode: the pattern plays on after the key goes up (level %.3f) and stops when HOLD goes off (%.5f)",
                       peak (transpose.audio, 0.5, 1.4), peak (transpose.audio, 1.8, 2.2));
        check (peak (transpose.audio, 0.5, 1.4) > 0.05 && peak (transpose.audio, 1.8, 2.2) < 1.0e-6, what);
    }

    //==========================================================================
    /** The voice side of SLIDE, on a PlayingSynthesiser of its own (the processor's input is the
        keyboard, which never reaches channel 16). */
    struct SlideRig
    {
        PlayingSynthesiser synth;
        std::vector<NeonSynthVoice*> voices;
        NeonSynthVoice::Params params;

        explicit SlideRig (float glideSeconds)
        {
            for (int i = 0; i < 4; ++i)
            {
                auto* v = new NeonSynthVoice();
                v->setVoiceIndex (i);
                synth.addVoice (v);
                voices.push_back (v);
            }
            synth.addSound (new NeonSynthSound());
            synth.setCurrentPlaybackSampleRate (rate);
            for (auto* v : voices)
                v->prepare (rate, 512);
            params.waveform = PolyBlepOscillator::sine;
            params.filterOn = false;
            params.adsr = { 0.005f, 0.1f, 0.3f, 0.05f };
            params.glideSeconds = glideSeconds;
        }

        /** Renders with the messages at their absolute samples; returns the left channel and, per
            512-sample block, how many voices sound. */
        std::vector<float> run (const std::vector<std::pair<juce::int64, juce::MidiMessage>>& messages, double seconds, std::vector<int>* voicesOn = nullptr)
        {
            std::vector<float> out;
            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            for (juce::int64 start = 0; start < (juce::int64) (seconds * rate); start += 512)
            {
                midi.clear();
                for (const auto& [sample, message] : messages)
                    if (sample >= start && sample < start + 512)
                        midi.addEvent (message, (int) (sample - start));
                for (auto* v : voices)
                {
                    v->setParameters (params);
                    v->setModulation ({});
                }
                buffer.clear();
                synth.renderNextBlock (buffer, midi, 0, 512);
                out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + 512);
                if (voicesOn != nullptr)
                {
                    int on = 0;
                    for (const auto* v : voices)
                        on += v->getTelemetry().active ? 1 : 0;
                    voicesOn->push_back (on);
                }
            }
            return out;
        }
    };

    std::vector<std::pair<juce::int64, juce::MidiMessage>> slideMessages (bool marked, int to = 55)
    {
        // Step 1 (note 48) slides into step 2 (`to`), exactly as the sequencer sends it, all at one
        // sample: CC 68 down, the new note-on, step 1's note-off, CC 68 up.
        const auto t = (juce::int64) (0.3 * rate);
        std::vector<std::pair<juce::int64, juce::MidiMessage>> m { { 0, juce::MidiMessage::noteOn (16, 48, (juce::uint8) 100) } };
        if (marked)
            m.push_back ({ t, juce::MidiMessage::controllerEvent (16, 68, 127) });
        m.push_back ({ t, juce::MidiMessage::noteOn (16, to, (juce::uint8) 100) });
        m.push_back ({ t, juce::MidiMessage::noteOff (16, 48) });
        if (marked)
            m.push_back ({ t, juce::MidiMessage::controllerEvent (16, 68, 0) });
        m.push_back ({ (juce::int64) (0.7 * rate), juce::MidiMessage::noteOff (16, to) });
        return m;
    }

    void slideTests()
    {
        std::printf ("SLIDE: a marked channel-16 overlap glides the sounding voice\n");
        char what[256];

        for (const float glide : { 0.0f, 0.08f })
        {
            std::vector<int> on;
            SlideRig rig (glide);
            const auto out = rig.run (slideMessages (true), 0.9, &on);
            const double before = peak (out, 0.25, 0.3), during = peak (out, 0.3, 0.4);
            const double reached = frequency (out, 0.45, 0.65);
            const int most = *std::max_element (on.begin(), on.end());
            std::snprintf (what, sizeof (what), "GLIDE %.0f ms: one voice (%d), the envelope runs on (%.3f -> %.3f), reaches %.1f Hz (%+.2f cents)",
                           glide * 1000.0f, most, before, during, reached, cents (reached, midiHz (55)));
            check (most == 1 && std::abs (during - before) < 0.02 * before && std::abs (cents (reached, midiHz (55))) < 2.0, what);
        }

        {
            SlideRig rig (0.08f);
            const auto out = rig.run (slideMessages (true), 0.9);
            const double halfway = frequency (out, 0.328, 0.352);   // 24 ms around the glide's middle (0.34 s)
            std::snprintf (what, sizeof (what), "GLIDE 80 ms: halfway through, halfway in pitch (%.1f Hz, expected about %.1f Hz)", halfway, midiHz (51.5));
            check (std::abs (cents (halfway, midiHz (51.5))) < 60.0, what);
        }

        // Unmarked: the same overlap retriggers, exactly as a plain juce::Synthesiser plays it.
        {
            SlideRig rig (0.08f);
            const auto out = rig.run (slideMessages (false), 0.9);

            juce::Synthesiser plain;
            std::vector<NeonSynthVoice*> voices;
            for (int i = 0; i < 4; ++i)
            {
                auto* v = new NeonSynthVoice();
                v->setVoiceIndex (i);
                plain.addVoice (v);
                voices.push_back (v);
            }
            plain.addSound (new NeonSynthSound());
            plain.setCurrentPlaybackSampleRate (rate);
            for (auto* v : voices)
                v->prepare (rate, 512);
            NeonSynthVoice::Params params = rig.params;
            params.glideSeconds = 0.0f;
            std::vector<float> reference;
            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            const auto messages = slideMessages (false);
            for (juce::int64 start = 0; start < (juce::int64) (0.9 * rate); start += 512)
            {
                midi.clear();
                for (const auto& [sample, message] : messages)
                    if (sample >= start && sample < start + 512)
                        midi.addEvent (message, (int) (sample - start));
                for (auto* v : voices)
                {
                    v->setParameters (params);
                    v->setModulation ({});
                }
                buffer.clear();
                plain.renderNextBlock (buffer, midi, 0, 512);
                reference.insert (reference.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + 512);
            }
            check (identical (out, reference), "an unmarked channel-16 overlap plays as before v1.11.0 (bit for bit, GLIDE on: new voices start on their pitch)");
        }

        // A slide to the same pitch: one voice, no retrigger, and the old note-off doesn't end it.
        {
            SlideRig rig (0.08f);
            std::vector<int> on;
            const auto out = rig.run (slideMessages (true, 48), 0.9, &on);
            std::snprintf (what, sizeof (what), "a slide to the same pitch goes on (%.3f -> %.3f, one voice) and ends at its own note-off (%.5f after)",
                           peak (out, 0.25, 0.3), peak (out, 0.3, 0.65), peak (out, 0.8, 0.9));
            check (*std::max_element (on.begin(), on.end()) == 1 && std::abs (peak (out, 0.3, 0.65) - peak (out, 0.25, 0.3)) < 0.02 * peak (out, 0.25, 0.3)
                     && peak (out, 0.8, 0.9) < 1.0e-6, what);
        }

        // A slide whose old note has already ended starts the new one normally.
        {
            SlideRig rig (0.08f);
            std::vector<int> on;
            const auto t = (juce::int64) (0.3 * rate);
            const auto out = rig.run ({ { 0, juce::MidiMessage::noteOn (16, 48, (juce::uint8) 100) }, { t - 100, juce::MidiMessage::noteOff (16, 48) },
                                        { t, juce::MidiMessage::controllerEvent (16, 68, 127) },
                                        { t, juce::MidiMessage::noteOn (16, 55, (juce::uint8) 100) },
                                        { t, juce::MidiMessage::noteOff (16, 48) },
                                        { t, juce::MidiMessage::controllerEvent (16, 68, 0) } }, 0.6, &on);
            std::snprintf (what, sizeof (what), "a slide after its old note was released starts the next note normally (it attacks to %.3f)", peak (out, 0.3, 0.35));
            check (on.back() == 1 && peak (out, 0.3, 0.35) > 2.0 * peak (out, 0.25, 0.29), what);
        }

        // An unmarked sequencer step never glides, whatever GLIDE is (like a 303: only SLIDE
        // steps glide; Julian, 22 Nov 2026), even on a voice that played an earlier step: six
        // steps on four voices, so voices are reused.
        {
            std::vector<std::pair<juce::int64, juce::MidiMessage>> steps;
            const int notes[] = { 48, 55, 51, 58, 46, 53 };
            for (int k = 0; k < 6; ++k)
            {
                const auto at = (juce::int64) (0.12 * k * rate);
                steps.push_back ({ at, juce::MidiMessage::noteOn (16, notes[k], (juce::uint8) 100) });
                steps.push_back ({ at + (juce::int64) (0.06 * rate), juce::MidiMessage::noteOff (16, notes[k]) });
            }
            SlideRig gliding (0.08f), still (0.0f);
            check (identical (gliding.run (steps, 0.9), still.run (steps, 0.9)),
                   "an unmarked sequencer step starts on its own pitch with GLIDE on (bit for bit as GLIDE 0)");
        }
    }

    //==========================================================================
    void defaultTests()
    {
        std::printf ("At their defaults the playing controls change nothing\n");
        const std::vector<Event> notes { noteOn (0.0, 48), noteOn (0.1, 60), noteOff (0.3, 48), noteOn (0.35, 55), noteOff (0.5, 60), noteOff (0.6, 55),
                                         noteOn (0.7, 67), noteOff (0.8, 67) };
        for (const float voices : { 1.0f, 16.0f })
        {
            const auto untouched = render ([=] (NeonSynthProcessor& p) { setParameter (p, "voiceCount", voices); }, notes, 1.0);
            const auto explicitDefaults = render ([=] (NeonSynthProcessor& p)
                                                  {
                                                      setParameter (p, "voiceCount", voices);
                                                      setParameter (p, "glide", 0.0f);
                                                      setParameter (p, "legato", 0.0f);
                                                      setParameter (p, "hold", 0.0f);
                                                  }, notes, 1.0);
            check (identical (untouched.audio, explicitDefaults.audio), voices == 1.0f ? "Voices 1: GLIDE 0, LEGATO off, HOLD off (bit for bit)"
                                                                                       : "Voices 16: GLIDE 0, LEGATO off, HOLD off (bit for bit)");
        }

        // HOLD switched on and off again before any key: nothing to release, nothing changes.
        auto shifted = notes;
        for (auto& e : shifted)
            e.sample += (juce::int64) (0.1 * rate);
        shifted.push_back (change (0.0, "hold", 1.0f));
        shifted.push_back (change (0.05, "hold", 0.0f));
        const auto toggled = render ([] (NeonSynthProcessor&) {}, shifted, 1.1);
        for (auto& e : shifted)
            if (e.parameter.isNotEmpty())
                e.value = 0.0f;
        const auto plain = render ([] (NeonSynthProcessor&) {}, shifted, 1.1);
        check (identical (toggled.audio, plain.audio), "HOLD on and off with no key down changes nothing (bit for bit)");
    }

    //==========================================================================
    void clickTests()
    {
        std::printf ("Click tests: switching GLIDE, LEGATO and HOLD while notes sound\n");
        char what[256];

        // The largest step a steady sine of the highest note makes, plus 10%: anything above it is a click.
        const auto steady = render ([] (NeonSynthProcessor& p) { setParameter (p, "voiceCount", 1.0f); },
                                    { noteOn (0.0, 69, 127) }, 0.5);
        const float limit = 1.1f * largestStep (steady.audio, 0.1, 0.5);

        struct Case { const char* name; std::function<void (NeonSynthProcessor&)> configure; std::vector<Event> events; };
        const std::vector<Case> cases
        {
            { "GLIDE turned while a note sounds and glides",
              [] (NeonSynthProcessor& p) { setParameter (p, "voiceCount", 1.0f); setParameter (p, "glide", 300.0f); },
              { noteOn (0.0, 57, 127), noteOn (0.2, 69, 127), change (0.3, "glide", 2000.0f), change (0.4, "glide", 0.0f),
                noteOn (0.5, 57, 127), change (0.55, "glide", 120.0f), noteOn (0.6, 69, 127), noteOff (0.9, 69), noteOff (0.9, 57) } },
            { "LEGATO switched on and off with two keys down",
              [] (NeonSynthProcessor& p) { setParameter (p, "voiceCount", 1.0f); setParameter (p, "glide", 50.0f); },
              { noteOn (0.0, 57, 127), change (0.1, "legato", 1.0f), noteOn (0.2, 69, 127), change (0.35, "legato", 0.0f),
                noteOff (0.45, 69), change (0.5, "legato", 1.0f), noteOff (0.7, 57) } },
            { "legato steps at GLIDE 0 (a jump in pitch, not in the wave)",
              [] (NeonSynthProcessor& p) { setParameter (p, "voiceCount", 1.0f); setParameter (p, "legato", 1.0f); },
              { noteOn (0.0, 57, 127), noteOn (0.1, 69, 127), noteOn (0.2, 62, 127), noteOff (0.3, 62), noteOff (0.4, 69), noteOff (0.5, 57) } },
            { "HOLD switched on and off while keys sound",
              [] (NeonSynthProcessor& p) { setParameter (p, "voiceCount", 1.0f); },
              { noteOn (0.0, 57, 127), change (0.1, "hold", 1.0f), noteOff (0.2, 57), noteOn (0.3, 69, 127), noteOff (0.35, 69),
                change (0.5, "hold", 0.0f), change (0.6, "hold", 1.0f), noteOn (0.6, 64, 127), change (0.7, "hold", 0.0f), noteOff (0.8, 64) } },
        };

        for (const auto& c : cases)
        {
            const auto r = render (c.configure, c.events, 1.0);
            const float step = largestStep (r.audio, 0.0, 1.0);
            std::snprintf (what, sizeof (what), "%s: largest step %.4f (a steady A4 makes %.4f)", c.name, step, limit / 1.1f);
            check (step <= limit, what);
        }

        // A slide (on its own synth): the same limit, from the same voice gain at velocity 100.
        SlideRig rig (0.0f);
        rig.params.adsr = { 0.005f, 0.1f, 1.0f, 0.05f };
        auto messages = slideMessages (true);
        const auto out = rig.run (messages, 0.9);
        SlideRig steadyRig (0.0f);
        steadyRig.params.adsr = rig.params.adsr;
        const auto steadyOut = steadyRig.run ({ { 0, juce::MidiMessage::noteOn (16, 55, (juce::uint8) 100) } }, 0.5);
        const float slideLimit = 1.1f * largestStep (steadyOut, 0.1, 0.5);
        std::snprintf (what, sizeof (what), "a slide at GLIDE 0: largest step %.4f (a steady G3 makes %.4f)", largestStep (out, 0.0, 0.9), slideLimit / 1.1f);
        check (largestStep (out, 0.0, 0.9) <= slideLimit, what);
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;

    glideTests();
    legatoTests();
    holdTests();
    slideTests();
    defaultTests();
    clickTests();

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
