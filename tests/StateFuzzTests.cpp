// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    setStateInformation() against damaged and hostile sessions.

    A host hands the plugin whatever bytes it saved, and a session file can be truncated by a
    full disk, damaged in a copy, edited by hand or written by someone else on purpose. None of
    that may crash the host or leave the plugin unable to play. Every case below feeds one such
    state to a single processor (the same one throughout, as a host would) and then requires:
      - no crash, no exception (and, in the sanitizer build, no memory error or undefined behaviour);
      - the processor still renders: a chord (and the sequencer, where the state turns it on)
        gives finite audio no louder than the output clipper's ceiling.
    The corpus: the processor's own valid v4 and v3 states (with the built-in pad, and with an
    embedded sample) as seeds; truncated at many points; seeded random byte flips; damaged FLAC
    inside valid XML; not-XML garbage and an empty block; an embedded sample over the size cap;
    flac text that is not base64; a Pattern of 10 000 steps; out-of-range notes and velocities;
    missing children; nonsense parameter values (NaN, infinity, 1e30, text); a future
    stateVersion; very long names and paths; paths to files that are not audio. At the end the
    seeds must still load and save back byte for byte.

    The random cases are seeded, so a failure always reproduces. The cases in bugCases() crashed
    or hung v1.10.0 and are fixed since v1.10.1; --verbose names every case before it is loaded,
    so a crash points at its case.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target StateFuzzTests
    v1.11.0: preset files (PresetManager::loadFile) get the same treatment: truncated, flipped,
    not XML, nested deep, over the size cap, a folder, hostile category / name / program
    attributes; and stepping through a folder of damaged presets.

    Run the resulting StateFuzzTests executable; it exits non-zero on failure.
*/
#include "plugin/NeonSynthProcessor.h"
#include <cmath>
#include <cstdio>
#include <cstring>
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

    constexpr double testRate      = 48000.0;
    constexpr int    testBlockSize = 256;

    void setParameter (juce::AudioProcessor& p, const juce::String& id, float value)
    {
        for (auto* param : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param); ranged != nullptr && ranged->getParameterID() == id)
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
    }

    juce::MemoryBlock save (NeonSynthProcessor& p)
    {
        juce::MemoryBlock block;
        p.getStateInformation (block);
        return block;
    }

    void load (NeonSynthProcessor& p, const juce::MemoryBlock& state)
    {
        p.setStateInformation (state.getData(), (int) state.getSize());
    }

    std::unique_ptr<juce::XmlElement> toXml (const juce::MemoryBlock& state)
    {
        return juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
    }

    juce::MemoryBlock toState (const juce::XmlElement& xml)
    {
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (xml, block);
        return block;
    }

    /** Wraps any text the way copyXmlToBinary() wraps XML (magic number, length, text, NUL), so
        text that is not XML, or not complete XML, reaches the parser instead of being refused
        at the header. */
    juce::MemoryBlock wrapText (const juce::String& text)
    {
        juce::MemoryBlock block;
        {
            juce::MemoryOutputStream out (block, false);
            out.writeInt (0x21324356);   // JUCE's magicXmlNumber
            out.writeInt ((int) text.getNumBytesAsUTF8());
            out.write (text.toRawUTF8(), text.getNumBytesAsUTF8());
            out.writeByte (0);
        }
        return block;
    }

    /** A copy of a state, edited as XML. */
    juce::MemoryBlock edited (const juce::MemoryBlock& state, const std::function<void (juce::XmlElement&)>& edit)
    {
        auto xml = toXml (state);
        jassert (xml != nullptr);
        edit (*xml);
        return toState (*xml);
    }

    /** A mono 24-bit WAV, 0.25 s of a tone with a little noise (seeded). */
    void writeWav (const juce::File& file)
    {
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions{}.withSampleRate (testRate).withNumChannels (1).withBitsPerSample (24));

        juce::Random random (7);
        juce::AudioBuffer<float> b (1, (int) (0.25 * testRate));
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.05f * (random.nextFloat() * 2.0f - 1.0f)
                               + 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / testRate));
        writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    //==========================================================================
    /** The processor every case is loaded into, prepared once like a host would. */
    struct Harness
    {
        NeonSynthProcessor processor;
        juce::AudioBuffer<float> buffer { 2, testBlockSize };
        juce::MidiBuffer midi;
        int  cases = 0, unusable = 0;
        juce::String firstUnusable;
        bool verbose = false;   // --verbose: name each case before loading it (to find the one that crashes)

        Harness()
        {
            processor.setRateAndBufferSizeDetails (testRate, testBlockSize);
            processor.prepareToPlay (testRate, testBlockSize);
        }

        /** Plays a chord for a few blocks and releases it; true if every sample is finite and
            within the clipper's ceiling (anything else means the state broke the processor). */
        bool rendersFinite (int numBlocks = 6)
        {
            bool ok = true;
            for (int b = 0; b < numBlocks; ++b)
            {
                midi.clear();
                for (int note : { 48, 55, 60, 67 })
                {
                    if (b == 0)             midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0);
                    if (b == numBlocks - 2) midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
                }
                processor.processBlock (buffer, midi);

                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    for (int i = 0; i < testBlockSize; ++i)
                    {
                        const float v = buffer.getSample (ch, i);
                        ok = ok && std::isfinite (v) && std::abs (v) <= 1.0f;
                    }
            }
            return ok;
        }

        /** Loads one broken state; counts (and remembers the first) case that leaves the processor unusable. */
        void feed (const juce::String& name, const juce::MemoryBlock& state, int numBlocks = 6)
        {
            ++cases;
            if (verbose)
                std::printf ("        case: %s\n", name.substring (0, 200).toRawUTF8());
            std::fflush (stdout);
            processor.setStateInformation (state.getData(), (int) state.getSize());
            if (! rendersFinite (numBlocks))
            {
                if (unusable++ == 0)
                    firstUnusable = name;
                std::printf ("        unusable after: %s\n", name.toRawUTF8());
            }
        }

        /** Checks the group of cases fed since the last call. */
        void checkGroup (const char* what)
        {
            std::printf ("        (%d cases)\n", cases);
            check (unusable == 0, what);
            cases = unusable = 0;
            firstUnusable = {};
        }
    };

    /** Cases that crashed or hung v1.10.0 (an uncaught exception, a stack overflow, an endless
        sequencer loop, a crash in the grain reader), found by this test and fixed in v1.10.1. */
    void bugCases (Harness& h, const juce::MemoryBlock& pad, const juce::MemoryBlock& embedded)
    {
        std::printf ("Cases that broke v1.10.0\n");

        // MemoryBlock::fromBase64Encoding() sizes its block from the number before the dot, without
        // checking it against the text: "-1." asks for SIZE_MAX bytes (std::bad_alloc escapes
        // setStateInformation; ASan aborts), so does a number that wraps to a negative int, and
        // "1500000000." asks for 1.5 GB, which loadEmbeddedFlac() then copies once more into its
        // MemoryInputStream.
        for (const char* prefix : { "-1.", "9999999999999999999999.", "1500000000." })
            h.feed (juce::String ("flac size prefix ") + prefix,
                    edited (embedded, [prefix] (juce::XmlElement& xml)
                    {
                        auto* sample = xml.getChildByName ("Sample");
                        sample->removeAttribute ("hash");
                        const auto text = sample->getStringAttribute ("flac");
                        sample->setAttribute ("flac", prefix + text.fromFirstOccurrenceOf (".", false, false));
                    }));
        h.checkGroup ("a flac attribute whose size prefix is negative or huge does not crash");

        // juce::XmlDocument parses child elements recursively, with no depth limit (and
        // ValueTree::fromXml and ~XmlElement recurse the same way): elements nested a few hundred
        // thousand deep overflow the stack.
        juce::String deep;
        deep << "<Parameters stateVersion=\"4\">" << juce::String::repeatedString ("<a>", 300000)
             << juce::String::repeatedString ("</a>", 300000) << "</Parameters>";
        h.feed ("elements nested 300 000 deep", wrapText (deep));
        h.checkGroup ("deeply nested XML does not crash");

        // A parameter value of "nan" in the session reaches the parameter unchecked (the APVTS
        // parses it as NaN and NormalisableRange passes NaN through), and from there the DSP:
        // seqTempo = nan hangs processBlock (StepSequencer::process never leaves its step loop),
        // grPos, grScan, grSpray, grJitter, lfoPitch or seqSwing = nan crash it (an out-of-range
        // read in GrainCloud), and cutoff, sustain, width, filterRes and others give NaN audio.
        // Each case runs in turn, so the first hang or crash stops the program.
        const auto xml = toXml (pad);
        for (auto* param : xml->getChildWithTagNameIterator ("PARAM"))
        {
            const auto id = param->getStringAttribute ("id");
            h.feed (id + " = nan", edited (pad, [&id] (juce::XmlElement& e)
            {
                for (auto* p : e.getChildWithTagNameIterator ("PARAM"))
                    if (p->getStringAttribute ("id") == id)
                        p->setAttribute ("value", "nan");
            }), 3);
        }
        h.checkGroup ("a parameter value of NaN in a session leaves a usable processor");
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // the processor's editor code needs the message manager
    bool verbose = false;
    for (int i = 1; i < argc; ++i)
    {
        verbose     = verbose     || juce::String (argv[i]) == "--verbose";
    }
    const auto started = juce::Time::getMillisecondCounterHiRes();

    const auto dir = juce::File::createTempFile ("neonsynth-fuzz");
    dir.createDirectory();

    //==========================================================================
    // Seeds, all written by the processor itself. The sequencer is on (1/32 at 200 BPM, internal
    // clock) in every seed, so the pattern is played as well as the chord.
    const auto configure = [] (NeonSynthProcessor& p)
    {
        setParameter (p, "seqOn", 1.0f);
        setParameter (p, "seqSync", 0.0f);
        setParameter (p, "seqRate", 2.0f);
        setParameter (p, "seqTempo", 200.0f);
    };

    juce::MemoryBlock padState, embeddedState, v3State;
    {
        NeonSynthProcessor p;
        configure (p);
        padState = save (p);   // v4, the built-in pad

        const auto wav = dir.getChildFile ("tone.wav");
        writeWav (wav);
        juce::String error;
        const bool loaded = p.loadSampleFile (wav, error);
        setParameter (p, "source", 2.0f);   // granular: the sample is read all over
        embeddedState = save (p);           // v4, sample embedded as FLAC
        wav.deleteFile();                   // so a damaged embedded copy can't fall back to the file
        check (loaded, "the seed sample loads");
    }
    v3State = edited (embeddedState, [] (juce::XmlElement& xml)
    {
        xml.setAttribute ("stateVersion", 3);   // v1.5: no Chop child
        xml.deleteAllChildElementsWithTagName ("Chop");
    });
    {
        const auto padXml = toXml (padState), embeddedXml = toXml (embeddedState);
        check (padXml != nullptr && padXml->getIntAttribute ("stateVersion") == 4 && padXml->getChildByName ("Pattern") != nullptr,
               "seed: a v4 state with the built-in pad");
        check (embeddedXml != nullptr && embeddedXml->getChildByName ("Sample") != nullptr
                   && embeddedXml->getChildByName ("Sample")->getStringAttribute ("flac").length() > 10000,
               "seed: a v4 state with an embedded sample");
    }

    Harness h;
    h.verbose = verbose;
    for (const auto* seed : { &padState, &embeddedState, &v3State })
        h.feed ("seed", *seed);
    h.checkGroup ("the seeds themselves load and render");

    //==========================================================================
    std::printf ("Truncated\n");
    for (const auto* seed : { &padState, &embeddedState })
    {
        const auto size = (int) seed->getSize();
        for (int cut = 0; cut < 16; ++cut)   // inside and just after the header
            h.feed ("cut " + juce::String (cut), juce::MemoryBlock (seed->getData(), (size_t) cut), 2);
        for (int i = 1; i < 120; ++i)
        {
            const int cut = (int) ((juce::int64) size * i / 120);
            h.feed ("cut " + juce::String (cut), juce::MemoryBlock (seed->getData(), (size_t) cut), 2);
        }
    }
    {
        // The header says the text is longer than the block (as when the end is missing).
        auto block = wrapText (toXml (padState)->toString (juce::XmlElement::TextFormat().singleLine()));
        block.setSize (block.getSize() / 2);
        h.feed ("text shorter than its header says", block);
    }
    h.checkGroup ("a state truncated anywhere loads without harm");

    //==========================================================================
    std::printf ("Random byte flips\n");
    {
        juce::Random random (20261118);
        for (const auto* seed : { &padState, &embeddedState, &v3State })
        {
            for (int i = 0; i < 150; ++i)
            {
                auto block = *seed;
                auto* bytes = static_cast<juce::uint8*> (block.getData());
                const int flips = 1 + random.nextInt (4);
                for (int f = 0; f < flips; ++f)
                    bytes[random.nextInt ((int) block.getSize())] ^= (juce::uint8) (1 + random.nextInt (255));
                h.feed ("flip " + juce::String (i), block, 3);
            }
        }
        // Flips confined to the XML text but inside the parameters (the first part of the text),
        // where most of them leave valid XML with different values.
        for (int i = 0; i < 150; ++i)
        {
            auto block = padState;   // NOLINT(performance-unnecessary-copy-initialization): the copy is damaged below
            auto* bytes = static_cast<juce::uint8*> (block.getData());
            const int textBytes = (int) block.getSize() - 9;
            bytes[8 + random.nextInt (textBytes)] = (juce::uint8) "0123456789.-+eE nanif\""[random.nextInt (22)];
            h.feed ("text flip " + juce::String (i), block, 3);
        }
    }
    h.checkGroup ("seeded random byte flips never crash or break the processor");

    //==========================================================================
    std::printf ("Damaged embedded FLAC\n");
    {
        // Damage the FLAC itself, re-encode it, and drop the hash so the processor can't recognise
        // the sample as the one already loaded: every case goes through the FLAC decoder.
        auto xml = toXml (embeddedState);
        juce::MemoryBlock flac;
        flac.fromBase64Encoding (xml->getChildByName ("Sample")->getStringAttribute ("flac"));
        const auto withFlac = [&] (const juce::MemoryBlock& data)
        {
            return edited (embeddedState, [&] (juce::XmlElement& e)
            {
                auto* sample = e.getChildByName ("Sample");
                sample->removeAttribute ("hash");
                sample->setAttribute ("flac", data.toBase64Encoding());
            });
        };

        h.feed ("re-encoded intact", withFlac (flac));
        const bool intactLoads = h.processor.withCurrentSample ([] (const SampleData* s) { return s != nullptr && ! s->getOrigin().isDefault; });

        for (int i = 1; i < 60; ++i)
            h.feed ("flac cut", withFlac (juce::MemoryBlock (flac.getData(), flac.getSize() * (size_t) i / 60)), 3);

        juce::Random random (42);
        for (int i = 0; i < 150; ++i)
        {
            auto damaged = flac;   // NOLINT(performance-unnecessary-copy-initialization): the copy is damaged below
            auto* bytes = static_cast<juce::uint8*> (damaged.getData());
            // Mostly the first kilobyte (STREAMINFO and the first frame headers), where a decoder is
            // most easily misled about sizes; the rest anywhere.
            const int range = i < 100 ? juce::jmin (1024, (int) damaged.getSize()) : (int) damaged.getSize();
            const int flips = 1 + random.nextInt (8);
            for (int f = 0; f < flips; ++f)
                bytes[random.nextInt (range)] ^= (juce::uint8) (1 + random.nextInt (255));
            h.feed ("flac flip " + juce::String (i), withFlac (damaged), 3);
        }
        check (intactLoads, "the FLAC re-encoded intact still loads (so the damaged copies reach the decoder)");
    }
    h.checkGroup ("damaged embedded FLAC never crashes or breaks the processor");

    //==========================================================================
    std::printf ("Not XML\n");
    {
        h.feed ("empty block", juce::MemoryBlock());
        h.processor.setStateInformation (nullptr, 0);
        h.feed ("nine zero bytes", juce::MemoryBlock (9, true));
        h.feed ("plain text", wrapText ("this is not a NeonSynth session"));
        h.feed ("angle brackets", wrapText ("<<<<>>>> </Parameters> <Parameters <Parameters/> &&&;"));
        h.feed ("other root", wrapText ("<SomethingElse><Pattern/></SomethingElse>"));
        h.feed ("unclosed root", wrapText ("<Parameters stateVersion=\"4\"><PARAM id=\"cutoff\" value=\"1000\"/>"));
        h.feed ("bad entity", wrapText ("<Parameters name=\"&#xFFFFFFFF;&bogus;\"/>"));
        h.feed ("raw json", wrapText ("{\"Parameters\": [1, 2, 3]}"));

        juce::Random random (99);
        for (int i = 0; i < 40; ++i)
        {
            juce::MemoryBlock noise ((size_t) (1 + random.nextInt (4096)));
            random.fillBitsRandomly (noise.getData(), noise.getSize());
            h.feed ("noise", noise, 2);

            // The same with a valid header, so it reaches the XML parser.
            if (noise.getSize() >= 9)
            {
                static_cast<juce::uint32*> (noise.getData())[0] = juce::ByteOrder::swapIfBigEndian ((juce::uint32) 0x21324356);
                h.feed ("noise with header", noise, 2);
            }
        }

        // A header claiming a length far beyond the block, and one with the high bit set.
        for (const juce::uint32 claimed : { 0x7fffffffu, 0xffffffffu, 0x80000000u })
        {
            auto block = wrapText ("<Parameters/>");
            static_cast<juce::uint32*> (block.getData())[1] = juce::ByteOrder::swapIfBigEndian (claimed);
            h.feed ("length field " + juce::String::toHexString ((juce::int64) claimed), block);
        }
    }
    h.checkGroup ("garbage, an empty block and wrong headers are ignored safely");

    //==========================================================================
    std::printf ("Hostile Sample child\n");
    {
        const auto withSample = [&] (const std::function<void (juce::XmlElement&)>& edit)
        {
            return edited (embeddedState, [&] (juce::XmlElement& xml)
            {
                auto* sample = xml.getChildByName ("Sample");
                sample->removeAttribute ("hash");
                edit (*sample);
            });
        };

        // Over the cap: text longer than any embedded sample getStateInformation writes.
        juce::String huge = "15000000.";
        huge = huge.paddedRight ('A', (int) (SampleLoader::maxEmbeddedBytes / 3 * 4 + 1000));
        const auto hugeStarted = juce::Time::getMillisecondCounterHiRes();
        h.feed ("flac over the cap", withSample ([&] (juce::XmlElement& s) { s.setAttribute ("flac", huge); }));
        const auto hugeMs = juce::Time::getMillisecondCounterHiRes() - hugeStarted;
        const bool padAfterHuge = h.processor.withCurrentSample ([] (const SampleData* s) { return s != nullptr && s->getOrigin().isDefault; });
        huge = {};
        std::printf ("        (the over-the-cap state took %.0f ms to load)\n", hugeMs);
        check (padAfterHuge, "an embedded sample over the cap is refused: the built-in pad plays");

        for (const char* text : { "not base64 at all!", "....", ".", "0.", "12.!!!!", "3.AAAA", "100.%%%%%%",
                                  "  77.\xc3\xa9\xc3\xa9\xc3\xa9", "abc.def.ghi" })
            h.feed (juce::String ("flac \"") + text + "\"", withSample ([text] (juce::XmlElement& s) { s.setAttribute ("flac", juce::String::fromUTF8 (text)); }));

        const auto longText = juce::String::repeatedString ("x", 200000);
        h.feed ("long name", withSample ([&] (juce::XmlElement& s) { s.removeAttribute ("flac"); s.setAttribute ("name", longText); }));
        h.feed ("long path", withSample ([&] (juce::XmlElement& s) { s.removeAttribute ("flac"); s.setAttribute ("path", "/" + longText + ".wav"); }));
        h.feed ("deep path", withSample ([&] (juce::XmlElement& s)
                                         {
                                             s.removeAttribute ("flac");
                                             s.setAttribute ("path", juce::String::repeatedString ("/a", 50000) + "/s.wav");
                                         }));
        h.feed ("long name and path", withSample ([&] (juce::XmlElement& s)
                                                  {
                                                      s.setAttribute ("flac", "garbage");
                                                      s.setAttribute ("name", longText);
                                                      s.setAttribute ("path", longText);
                                                  }));
        h.feed ("long hash", withSample ([&] (juce::XmlElement& s) { s.setAttribute ("hash", longText); }));

        for (const auto& path : { dir.getFullPathName(), juce::String ("/"), juce::String ("/dev/null"),
                                  dir.getChildFile ("missing.wav").getFullPathName() })
            h.feed ("path " + path, withSample ([path] (juce::XmlElement& s) { s.removeAttribute ("flac"); s.setAttribute ("path", path); }));

        // Files at the path that claim to be audio but are not (or only partly).
        juce::Random random (5);
        {
            juce::MemoryBlock noise (4096);
            random.fillBitsRandomly (noise.getData(), noise.getSize());
            for (const char* name : { "noise.wav", "noise.aiff", "noise.flac" })
                dir.getChildFile (name).replaceWithData (noise.getData(), noise.getSize());

            const auto wav = dir.getChildFile ("full.wav");
            writeWav (wav);
            juce::MemoryBlock whole;
            wav.loadFileAsData (whole);
            for (const int keep : { 12, 44, 100, 1000 })
                dir.getChildFile ("cut" + juce::String (keep) + ".wav").replaceWithData (whole.getData(), (size_t) keep);

            // A valid header whose data chunk claims to be 4 GB long.
            auto* bytes = static_cast<juce::uint8*> (whole.getData());
            for (size_t i = 12; i + 8 < whole.getSize(); ++i)
                if (std::memcmp (bytes + i, "data", 4) == 0)
                {
                    std::memset (bytes + i + 4, 0xff, 4);
                    break;
                }
            dir.getChildFile ("liar.wav").replaceWithData (whole.getData(), whole.getSize());
        }
        for (const char* name : { "noise.wav", "noise.aiff", "noise.flac", "cut12.wav", "cut44.wav", "cut100.wav", "cut1000.wav", "liar.wav" })
        {
            const auto path = dir.getChildFile (name).getFullPathName();
            h.feed (juce::String ("file ") + name, withSample ([path] (juce::XmlElement& s) { s.removeAttribute ("flac"); s.setAttribute ("path", path); }));
        }

        h.feed ("sample without attributes", withSample ([] (juce::XmlElement& s) { s.removeAllAttributes(); }));
        h.feed ("default=\"banana\"", withSample ([] (juce::XmlElement& s) { s.removeAllAttributes(); s.setAttribute ("default", "banana"); }));
        h.feed ("two Sample children", edited (embeddedState, [] (juce::XmlElement& xml)
                                               {
                                                   xml.addChildElement (new juce::XmlElement (*xml.getChildByName ("Sample")));
                                               }));
    }
    h.checkGroup ("hostile Sample children (sizes, encodings, names, paths, files) are survived");

    //==========================================================================
    std::printf ("Pattern and Chop\n");
    {
        const auto withPattern = [&] (const std::function<void (juce::XmlElement&)>& edit)
        {
            return edited (padState, [&] (juce::XmlElement& xml) { edit (*xml.getChildByName ("Pattern")); });
        };

        h.feed ("10 000 steps", withPattern ([] (juce::XmlElement& pattern)
                                             {
                                                 for (int i = 0; i < 10000; ++i)
                                                 {
                                                     auto* step = pattern.createNewChildElement ("Step");
                                                     step->setAttribute ("on", 1);
                                                     step->setAttribute ("note", i);
                                                     step->setAttribute ("velocity", -i);
                                                 }
                                             }));

        const char* values[] = { "-1", "-128", "128", "1000", "2147483647", "-2147483647", "99999999999999999999",
                                 "1e30", "nan", "inf", "", "sixty", "60.9", "0x7f" };
        for (const char* note : values)
            for (const char* velocity : { "0", "-5", "128", "1e30", "nan", "" })
                h.feed (juce::String ("note ") + note + " velocity " + velocity,
                        withPattern ([note, velocity] (juce::XmlElement& pattern)
                        {
                            for (auto* step : pattern.getChildIterator())
                            {
                                step->setAttribute ("on", "1");
                                step->setAttribute ("note", note);
                                step->setAttribute ("velocity", velocity);
                            }
                        }), 3);

        h.feed ("steps without attributes", withPattern ([] (juce::XmlElement& pattern)
                                                         {
                                                             for (auto* step : pattern.getChildIterator())
                                                                 step->removeAllAttributes();
                                                         }));
        h.feed ("steps of another kind", withPattern ([] (juce::XmlElement& pattern)
                                                      {
                                                          for (auto* step : pattern.getChildIterator())
                                                              step->setTagName ("Banana");
                                                      }));
        h.feed ("empty pattern", withPattern ([] (juce::XmlElement& pattern) { pattern.deleteAllChildElements(); }));

        for (const char* chop : { "", "0", "01010101010101010", "0101010101010x01", "1111111111111111111111", "nan" })
            h.feed (juce::String ("chop \"") + chop + "\"",
                    edited (padState, [chop] (juce::XmlElement& xml) { xml.getChildByName ("Chop")->setAttribute ("pattern", chop); }));

        for (const char* child : { "Pattern", "Chop", "Sample" })
            for (const auto* seed : { &padState, &embeddedState })
                h.feed (juce::String ("no ") + child, edited (*seed, [child] (juce::XmlElement& xml) { xml.deleteAllChildElementsWithTagName (child); }));
        h.feed ("no children at all", edited (embeddedState, [] (juce::XmlElement& xml) { xml.deleteAllChildElements(); }));
    }
    h.checkGroup ("odd patterns, steps and CHOP text are clamped or ignored");

    //==========================================================================
    std::printf ("Parameters\n");
    {
        const auto parameterIds = [&]
        {
            juce::StringArray ids;
            const auto xml = toXml (padState);   // kept alive while its children are iterated
            for (auto* param : xml->getChildWithTagNameIterator ("PARAM"))
                ids.add (param->getStringAttribute ("id"));
            return ids;
        }();
        const auto withValue = [&] (const juce::String& onlyId, const juce::String& value)
        {
            return edited (padState, [&] (juce::XmlElement& xml)
            {
                for (auto* param : xml.getChildWithTagNameIterator ("PARAM"))
                    if (onlyId.isEmpty() || param->getStringAttribute ("id") == onlyId)
                        param->setAttribute ("value", value);
            });
        };

        // (NaN, "nan" in the text, is in openBugCases(): it still breaks the processor.)
        const char* nonsense[] = { "inf", "-inf", "1e30", "-1e30", "1e400", "", "abc", "0x10", "--1" };
        for (const char* value : nonsense)
            h.feed (juce::String ("every parameter = \"") + value + "\"", withValue ({}, value));

        // One at a time, so a failure names the parameter.
        for (const auto& id : parameterIds)
            for (const char* value : { "inf", "-inf", "1e30", "-1e30" })
                h.feed (id + " = " + value, withValue (id, value), 3);

        h.feed ("PARAM without id", edited (padState, [] (juce::XmlElement& xml)
                                            {
                                                for (auto* param : xml.getChildWithTagNameIterator ("PARAM"))
                                                    param->removeAttribute ("id");
                                            }));
        h.feed ("unknown and duplicate PARAMs", edited (padState, [] (juce::XmlElement& xml)
                                                        {
                                                            for (int i = 0; i < 1000; ++i)
                                                            {
                                                                auto* extra = xml.createNewChildElement ("PARAM");
                                                                extra->setAttribute ("id", i % 2 == 0 ? juce::String ("cutoff") : "nope" + juce::String (i));
                                                                extra->setAttribute ("value", i % 3 == 0 ? "nan" : "1e30");
                                                            }
                                                        }));

        for (const char* version : { "5", "99", "2147483647", "-1", "0", "three", "" })
            for (const auto* seed : { &padState, &embeddedState })
                h.feed (juce::String ("stateVersion ") + version,
                        edited (*seed, [version] (juce::XmlElement& xml) { xml.setAttribute ("stateVersion", version); }));
        h.feed ("future children", edited (padState, [] (juce::XmlElement& xml)
                                           {
                                               xml.setAttribute ("stateVersion", 7);
                                               xml.createNewChildElement ("Wavetable")->setAttribute ("data", juce::String::repeatedString ("z", 10000));
                                               xml.createNewChildElement ("Pattern")->createNewChildElement ("Step")->setAttribute ("note", 1);
                                           }));
    }
    h.checkGroup ("nonsense parameter values and versions leave a usable processor");

    //==========================================================================
    // v1.11.0: preset files. A .neonpreset is a session's XML in a file someone may have sent, so
    // loading one goes through the same checks; these cases come in through PresetManager::loadFile.
    std::printf ("Preset files\n");
    {
        auto& presets = h.processor.getPresetManager();
        const auto presetDir = dir.getChildFile ("presets");
        presetDir.createDirectory();
        presets.setUserFolder (presetDir);
        int fileCount = 0;
        const auto feedFile = [&] (const juce::String& name, const std::function<void (const juce::File&)>& write, int numBlocks = 3)
        {
            ++h.cases;
            if (h.verbose)
                std::printf ("        case: preset %s\n", name.substring (0, 200).toRawUTF8());
            std::fflush (stdout);
            const auto file = presetDir.getChildFile ("case" + juce::String (fileCount++) + ".neonpreset");
            write (file);
            juce::String error;
            const bool loaded = presets.loadFile (file, error);
            const auto category = presets.getCurrentCategory();
            const bool metaSane = (category.isEmpty() || PresetManager::categories().contains (category))
                               && presets.getCurrentName().length() <= 128
                               && presets.getCurrentProgramIndex() >= -1 && presets.getCurrentProgramIndex() < PresetManager::numFactoryPresets
                               && (loaded || error.isNotEmpty());
            file.deleteFile();
            if (! metaSane || ! h.rendersFinite (numBlocks))
            {
                if (h.unusable++ == 0)
                    h.firstUnusable = name;
                std::printf ("        unusable after: preset %s\n", name.toRawUTF8());
            }
        };
        const auto writeText = [] (const juce::String& text) { return [text] (const juce::File& f) { f.replaceWithText (text); }; };
        const auto writeBlock = [] (const juce::MemoryBlock& block)
        {
            return [block] (const juce::File& f) { f.replaceWithData (block.getData(), block.getSize()); };
        };

        // The seeds as preset files (the XML text, without JUCE's binary header), and a factory preset.
        const auto padText = toXml (padState)->toString();
        const auto embeddedText = toXml (embeddedState)->toString();
        const auto factoryText = presets.createFactoryXml (11)->toString();
        for (const auto* text : { &padText, &embeddedText, &factoryText })
            feedFile ("seed", writeText (*text), 6);

        // Truncated anywhere, and random bytes flipped.
        for (const auto* text : { &padText, &factoryText })
            for (int i = 0; i < 60; ++i)
                feedFile ("cut", writeText (text->substring (0, text->length() * i / 60)), 2);
        {
            juce::Random random (1122);
            for (int i = 0; i < 120; ++i)
            {
                juce::MemoryBlock block (factoryText.toRawUTF8(), factoryText.getNumBytesAsUTF8());
                auto* bytes = static_cast<juce::uint8*> (block.getData());
                for (int f = 0, flips = 1 + random.nextInt (4); f < flips; ++f)
                    bytes[random.nextInt ((int) block.getSize())] ^= (juce::uint8) (1 + random.nextInt (255));
                feedFile ("flip " + juce::String (i), writeBlock (block), 2);
            }
        }

        // Not presets at all.
        feedFile ("empty file", writeText ({}));
        feedFile ("binary noise", [] (const juce::File& f)
        {
            juce::MemoryBlock noise (5000);
            juce::Random (8).fillBitsRandomly (noise.getData(), noise.getSize());
            f.replaceWithData (noise.getData(), noise.getSize());
        });
        feedFile ("a session with JUCE's binary header", writeBlock (padState));
        feedFile ("other root", writeText ("<NotNeonSynth><PARAM id=\"cutoff\" value=\"100\"/></NotNeonSynth>"));
        feedFile ("byte order mark", writeText (juce::String::fromUTF8 ("\xef\xbb\xbf") + padText));
        {
            juce::String deep;
            deep << "<Parameters>" << juce::String::repeatedString ("<a>", 300000) << juce::String::repeatedString ("</a>", 300000) << "</Parameters>";
            feedFile ("nested 300 000 deep", writeText (deep));
        }
        feedFile ("over the size cap", [] (const juce::File& f)
        {
            juce::FileOutputStream out (f);
            out.writeText ("<Parameters stateVersion=\"4\" preset=\"", false, false, nullptr);
            juce::HeapBlock<char> chunk (1 << 20);
            std::memset (chunk.get(), 'z', 1 << 20);
            for (juce::int64 written = 0; written <= PresetManager::maxFileBytes; written += 1 << 20)
                out.write (chunk.get(), 1 << 20);
            out.writeText ("\"/>", false, false, nullptr);
        });
        feedFile ("a folder named like a preset", [] (const juce::File& f) { f.createDirectory(); });

        // Hostile preset attributes on an otherwise good file.
        const auto withRoot = [&] (const std::function<void (juce::XmlElement&)>& edit)
        {
            return [edit, factoryText] (const juce::File& f)
            {
                auto xml = juce::XmlDocument::parse (factoryText);
                edit (*xml);
                xml->writeTo (f);
            };
        };
        for (const char* program : { "-5", "16", "99999", "nan", "1e30", "abc", "" })
            feedFile (juce::String ("program ") + program, withRoot ([program] (juce::XmlElement& x) { x.setAttribute ("program", program); }));
        for (const char* category : { "Banana", "", "bass ", "FX\nLead", "\xc3\xa9" })
            feedFile (juce::String ("category ") + category,
                      withRoot ([category] (juce::XmlElement& x) { x.setAttribute ("category", juce::String::fromUTF8 (category)); }));
        feedFile ("long category and name", withRoot ([] (juce::XmlElement& x)
        {
            x.setAttribute ("category", juce::String::repeatedString ("c", 100000));
            x.setAttribute ("preset", juce::String::repeatedString ("n", 100000));
        }));
        feedFile ("factory pattern of 10 000 steps", withRoot ([] (juce::XmlElement& x)
        {
            auto* pattern = x.getChildByName ("Pattern");
            for (int i = 0; i < 10000; ++i)
                pattern->createNewChildElement ("Step")->setAttribute ("slide", i % 2);
        }));
        feedFile ("every step tied and sliding", withRoot ([] (juce::XmlElement& x)
        {
            for (auto* step : x.getChildByName ("Pattern")->getChildIterator())
            {
                step->setAttribute ("tie", "1");
                step->setAttribute ("slide", "1");
            }
        }), 8);

        // Previous and next over a folder of damaged files must not hang or crash either.
        for (int i = 0; i < 5; ++i)
            presetDir.getChildFile ("bad" + juce::String (i) + ".neonpreset").replaceWithText (padText.substring (0, 40 * i));
        for (int i = 0; i < 40; ++i)
        {
            ++h.cases;
            presets.step (i % 3 == 0 ? -1 : 1);
            if (! h.rendersFinite (2) && h.unusable++ == 0)
                h.firstUnusable = "stepping over damaged files";
        }
        presetDir.deleteRecursively();
    }
    h.checkGroup ("damaged and hostile preset files are refused or survived, and the preset name and category stay sane");

    //==========================================================================
    bugCases (h, padState, embeddedState);

    //==========================================================================
    std::printf ("Afterwards\n");
    {
        load (h.processor, padState);
        check (save (h.processor) == padState && h.rendersFinite(), "the pad seed still loads, saves back byte for byte and plays");
        load (h.processor, embeddedState);
        check (save (h.processor) == embeddedState && h.rendersFinite(), "and so does the embedded-sample seed");
    }

    dir.deleteRecursively();
    std::printf ("\n(%.1f s)\n", (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0);
    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
