// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Session save and restore of the sample (getStateInformation / setStateInformation).

    Embedded sample (the <Sample flac="..."> text, up to about 13 MB):
      - after a session is loaded, the processor's parameter tree (apvts.state) no longer
        holds the Pattern, Chop or Sample child, so the text isn't kept there;
      - saving that session again gives the same bytes, and they restore the same sample
        and parameters;
      - the text is encoded at most once per sample: re-saving a loaded session encodes
        nothing, and saving a file-loaded sample twice encodes it once.

    Sample saved by path (too large to embed): the file at the path is loaded, and if it is
    not the audio that was saved (its hash differs) the status says "sample changed".

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target SessionStateTests
    Run the resulting SessionStateTests executable; it exits non-zero on failure.
*/
#include "NeonSynthProcessor.h"
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

    void setParameter (juce::AudioProcessor& p, const juce::String& id, float value)
    {
        for (auto* param : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param); ranged != nullptr && ranged->getParameterID() == id)
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
    }

    /** Every parameter's normalised value, in layout order. */
    std::vector<float> parameterValues (juce::AudioProcessor& p)
    {
        std::vector<float> values;
        for (auto* param : p.getParameters())
            values.push_back (param->getValue());
        return values;
    }

    /** A mono 24-bit WAV: noise (incompressible, so its FLAC is about as large as it gets) plus a tone. */
    void writeWav (const juce::File& file, double seconds, juce::int64 seed)
    {
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions{}.withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (24));

        juce::Random random (seed);
        juce::AudioBuffer<float> b (1, (int) (seconds * 48000.0));
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.3f * (random.nextFloat() * 2.0f - 1.0f)
                               + 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / 48000.0));
        writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    juce::String currentHash (NeonSynthProcessor& p)
    {
        return p.withCurrentSample ([] (const SampleData* s) { return s != nullptr ? s->getHash() : juce::String(); });
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

    /** The state with the sample's embedded copy removed: what a session with a sample too large to embed looks like. */
    juce::MemoryBlock withoutEmbeddedCopy (const juce::MemoryBlock& state)
    {
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        if (auto* sample = xml->getChildByName ("Sample"))
            sample->removeAttribute ("flac");
        juce::MemoryBlock out;
        juce::AudioProcessor::copyXmlToBinary (*xml, out);
        return out;
    }

    double millisecondsFor (const std::function<void()>& fn)
    {
        const auto start = juce::Time::getMillisecondCounterHiRes();
        fn();
        return juce::Time::getMillisecondCounterHiRes() - start;
    }

    //==========================================================================
    // Sessions saved by earlier releases (tests/fixtures/state). Each fixture is the XML that
    // release's getStateInformation() wrote; the test wraps it the way JUCE stores it.
    juce::MemoryBlock fixture (const juce::String& name)
    {
        const auto file = juce::File (NEONSYNTH_TESTS_DIR).getChildFile ("fixtures/state").getChildFile (name);
        juce::MemoryBlock block;
        if (auto xml = juce::XmlDocument::parse (file))
            juce::AudioProcessor::copyXmlToBinary (*xml, block);
        return block;
    }

    float plainValue (NeonSynthProcessor& p, const juce::String& id)
    {
        return p.getAPVTS().getRawParameterValue (id)->load();
    }

    /** Renders two seconds of a chord, stereo, and returns it (the fixed note sequence of the checks). */
    juce::AudioBuffer<float> renderChord (NeonSynthProcessor& p)
    {
        p.setRateAndBufferSizeDetails (48000.0, 512);
        p.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> all (2, 48000 * 2), block (2, 512);
        all.clear();
        for (int start = 0; start + 512 <= all.getNumSamples(); start += 512)
        {
            juce::MidiBuffer midi;
            if (start == 0)
                for (int note : { 48, 55, 64 })
                    midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0);
            if (start == 512 * 120)
                for (int note : { 48, 55, 64 })
                    midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
            block.clear();
            p.processBlock (block, midi);
            for (int ch = 0; ch < 2; ++ch)
                all.copyFrom (ch, start, block, ch, 0, 512);
        }
        p.releaseResources();
        return all;
    }

    bool finiteAndAudible (const juce::AudioBuffer<float>& b)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const float x = b.getSample (ch, i);
                if (! std::isfinite (x))
                    return false;
                peak = juce::jmax (peak, std::abs (x));
            }
        return peak > 0.01f;
    }

    bool identical (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < 2; ++ch)
            if (std::memcmp (a.getReadPointer (ch), b.getReadPointer (ch), sizeof (float) * (size_t) a.getNumSamples()) != 0)
                return false;
        return true;
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // SessionStateTests --write-fixture FILE: saves the session the fixtures are cut from (see
    // tests/fixtures/state/README.md). Run it only to make a fixture for a new state format.
    if (argc > 2 && juce::String (argv[1]) == "--write-fixture")
    {
        const auto wav = juce::File::createTempFile (".wav");
        writeWav (wav, 0.25, 7);
        NeonSynthProcessor p;
        juce::String error;
        if (! p.loadSampleFile (wav, error))
            return 1;
        setParameter (p, "waveform", 3.0f);      // Sawtooth
        setParameter (p, "cutoff", 900.0f);
        setParameter (p, "filterEnabled", 1.0f);
        setParameter (p, "attack", 0.05f);
        setParameter (p, "release", 0.4f);
        setParameter (p, "gain", -3.0f);
        setParameter (p, "width", 0.2f);
        setParameter (p, "seqTempo", 100.0f);
        setParameter (p, "smpRoot", 57.0f);
        setParameter (p, "chopOvertone", 50.0f);
        for (int i = 0; i < StepSequencer::numSteps; ++i)
            p.setSequencerStep (i, { i % 3 != 2, 45 + i, 60 + 4 * i });
        p.setChopStep (0, true);
        p.setChopStep (1, false);
        const auto state = save (p);
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        wav.deleteFile();
        return xml != nullptr && xml->writeTo (juce::File (argv[2])) ? 0 : 1;
    }

    const auto dir = juce::File::createTempFile ("neonsynth-session");
    dir.createDirectory();
    const auto wavFile = dir.getChildFile ("noise.wav");
    writeWav (wavFile, 20.0, 1);   // about 2.9 MB of FLAC, 3.9 MB of base64

    //==========================================================================
    std::printf ("Embedded sample\n");
    juce::MemoryBlock original;
    juce::String originalHash;
    {
        NeonSynthProcessor p;
        juce::String error;
        check (p.loadSampleFile (wavFile, error), "the test file loads");
        originalHash = currentHash (p);
        setParameter (p, "cutoff", 1234.0f);
        setParameter (p, "source", 1.0f);
        p.setChopStep (3, false);
        p.getAPVTS().state.setProperty ("view", 2, nullptr);   // what the editor keeps there

        original = save (p);
        check (p.getNumEmbeddedSampleEncodes() == 1, "the first save of a file-loaded sample encodes it");
        const auto again = save (p);
        check (p.getNumEmbeddedSampleEncodes() == 1, "a second save of the same sample does not encode it again");
        check (again == original, "and gives the same bytes");

        auto xml = juce::AudioProcessor::getXmlFromBinary (original.getData(), (int) original.getSize());
        check (xml != nullptr && xml->getChildByName ("Sample") != nullptr
                   && xml->getChildByName ("Sample")->getStringAttribute ("flac").length() > 1000000,
               "the saved state embeds the sample");
    }

    std::vector<float> savedParameters;
    juce::MemoryBlock resaved;
    {
        NeonSynthProcessor p;
        load (p, original);
        savedParameters = parameterValues (p);
        check (currentHash (p) == originalHash, "loading the session restores the embedded sample");

        const auto& state = p.getAPVTS().state;
        check (! state.getChildWithName ("Sample").isValid() && ! state.getChildWithName ("Pattern").isValid()
                   && ! state.getChildWithName ("Chop").isValid(),
               "the parameter tree keeps no Sample, Pattern or Chop child after loading");
        check (state.toXmlString().length() < 100000, "so it no longer holds the embedded text (it is a few kB)");
        check ((int) state.getProperty ("view", 0) == 2, "the editor's properties stay in the parameter tree");

        juce::MemoryBlock third;
        const double firstSave  = millisecondsFor ([&] { resaved = save (p); });
        const double secondSave = millisecondsFor ([&] { third = save (p); });
        check (p.getNumEmbeddedSampleEncodes() == 0, "re-saving a loaded session reuses its text: nothing is encoded");
        check (resaved == original, "re-saving an unchanged session gives the same bytes as the session it came from");
        check (third == original, "and so does saving it again");
        std::printf ("        (saves took %.1f ms and %.1f ms)\n", firstSave, secondSave);
    }
    {
        NeonSynthProcessor p;
        load (p, resaved);
        check (currentHash (p) == originalHash, "the re-saved session restores the same sample (hash)");
        check (parameterValues (p) == savedParameters, "and the same parameters");
        check ((p.getChopPattern() & (1u << 3)) == 0, "and the same CHOP pattern (step 4 off)");
        check (p.getSampleStatus().problem.isEmpty(), "with no problem shown");
    }
    {
        // Loading a different sample afterwards drops the kept text: its own text is encoded.
        NeonSynthProcessor p;
        load (p, original);
        p.useDefaultSample();
        const auto padState = save (p);
        auto xml = juce::AudioProcessor::getXmlFromBinary (padState.getData(), (int) padState.getSize());
        check (xml != nullptr && xml->getChildByName ("Sample") != nullptr
                   && xml->getChildByName ("Sample")->getBoolAttribute ("default") && ! xml->getChildByName ("Sample")->hasAttribute ("flac"),
               "after switching to the pad, the session saves the pad, not the old text");
        juce::String error;
        p.loadSampleFile (wavFile, error);
        check (save (p) == original && p.getNumEmbeddedSampleEncodes() == 1, "loading the file again encodes it once and saves the same bytes");
    }

    //==========================================================================
    std::printf ("Sample saved by path\n");
    const auto byPath = withoutEmbeddedCopy (original);
    {
        NeonSynthProcessor p;
        load (p, byPath);
        check (currentHash (p) == originalHash, "an unchanged file at the saved path loads the saved sample");
        check (! p.getSampleStatus().problem.contains ("changed"), "and the status does not say changed");
        check (p.getSampleStatus().problem.isEmpty(), "(no problem at all)");
    }

    writeWav (wavFile, 20.0, 2);   // the user edits the file
    juce::String changedHash;
    {
        NeonSynthProcessor p;
        load (p, byPath);
        changedHash = currentHash (p);
        check (changedHash.isNotEmpty() && changedHash != originalHash, "a changed file at the saved path is still loaded");
        check (p.getSampleStatus().problem == "sample changed", "and the status says \"sample changed\"");
        check (p.getSampleStatus().name == "noise.wav", "beside the file's name");

        load (p, byPath);   // a host sending the same state again
        check (p.getSampleStatus().problem == "sample changed", "sending the same state again still says changed");

        juce::String error;
        p.loadSampleFile (wavFile, error);
        check (p.getSampleStatus().problem.isEmpty(), "loading a file clears it");
    }
    {
        // The embedded copy wins over the file: the changed file doesn't matter then.
        NeonSynthProcessor p;
        load (p, original);
        check (currentHash (p) == originalHash && p.getSampleStatus().problem.isEmpty(),
               "a session with the sample embedded ignores the changed file");
    }

    dir.deleteRecursively();

    //==========================================================================
    std::printf ("\nSessions saved by earlier releases\n");
    {
        NeonSynthProcessor fresh;   // the defaults: pattern, CHOP, sample
        auto patternIs = [] (NeonSynthProcessor& a, auto&& expected)
        {
            for (int i = 0; i < StepSequencer::numSteps; ++i)
            {
                const auto x = a.getSequencerStep (i), y = expected (i);
                if (x.on != y.on || x.note != y.note || x.velocity != y.velocity)
                    return false;
            }
            return true;
        };
        auto defaultPattern = [&fresh] (int i) { return fresh.getSequencerStep (i); };
        auto savedPattern = [] (int i) { return StepSequencer::Step { i % 3 != 2, 45 + i, 60 + 4 * i }; };

        struct Case { const char* file; const char* release; bool hasPattern, hasSample, hasChop; };
        for (const auto& c : { Case { "v1.2.0.xml",  "v1.2 (no version, no pattern)",       false, false, false },
                               Case { "v1.4.0.xml",  "v1.4 (stateVersion 2, pattern)",       true,  false, false },
                               Case { "v1.5.0.xml",  "v1.5 (stateVersion 3, embedded sample)", true, true,  false },
                               Case { "v1.10.0.xml", "v1.10 (stateVersion 4, CHOP and all)", true,  true,  true } })
        {
            const auto state = fixture (c.file);
            const juce::String name (c.release);
            if (state.getSize() == 0)
            {
                check (false, (name + ": fixture " + c.file + " found and parsed").toRawUTF8());
                continue;
            }

            NeonSynthProcessor p;
            load (p, state);
            check (juce::approximatelyEqual (plainValue (p, "cutoff"), 900.0f) && juce::approximatelyEqual (plainValue (p, "release"), 0.4f)
                       && juce::approximatelyEqual (plainValue (p, "gain"), -3.0f) && juce::approximatelyEqual (plainValue (p, "waveform"), 3.0f),
                   (name + ": its parameters load").toRawUTF8());
            check (juce::approximatelyEqual (plainValue (p, "lfoCutoff"), plainValue (fresh, "lfoCutoff"))
                       && juce::approximatelyEqual (plainValue (p, "filterEnvAmt"), plainValue (fresh, "filterEnvAmt")),
                   (name + ": parameters added since keep their neutral defaults").toRawUTF8());
            check (c.hasPattern ? patternIs (p, savedPattern) : patternIs (p, defaultPattern),
                   (name + (c.hasPattern ? ": its pattern loads" : ": the default pattern loads")).toRawUTF8());
            check (c.hasSample ? p.getSampleStatus().name.endsWith (".wav") && p.getSampleStatus().problem.isEmpty()
                               : p.getSampleStatus().name == "Default pad",
                   (name + (c.hasSample ? ": its embedded sample loads" : ": the built-in pad plays")).toRawUTF8());
            check (p.getChopPattern() == (c.hasChop ? (juce::uint16) ((fresh.getChopPattern() | 1u) & ~2u) : fresh.getChopPattern()),
                   (name + (c.hasChop ? ": its CHOP pattern loads" : ": the default CHOP pattern")).toRawUTF8());

            // It plays, and saving and reopening it changes nothing that sounds.
            const auto first = renderChord (p);
            NeonSynthProcessor reopened;
            load (reopened, save (p));
            check (finiteAndAudible (first) && identical (first, renderChord (reopened)),
                   (name + ": it renders (finite, audible), and saved again it renders the same").toRawUTF8());
        }
    }

    std::printf (failures == 0 ? "All session state tests passed.\n" : "%d session state test(s) FAILED.\n", failures);
    return failures == 0 ? 0 : 1;
}
