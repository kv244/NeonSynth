// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Presets (v1.11.0): PresetManager.h, FactoryPresets.h, PresetBar.h and the host program list.

      - The factory data: 16 presets, unique names, known categories, every parameter ID real and
        every value inside its range, patterns of 16 valid steps (a slide always into a step that
        plays), CHOP text of 16 steps.
      - Every factory preset loads: each parameter at the preset's value or its default, the
        built-in pad, the pattern; and renders finite, under the output soft clipper (-6 dBFS),
        and ends in silence (a sequenced one once its sequencer is stopped). The table printed
        gives each preset's peak, for the listening pass.
      - Save then load gives back the session (parameters, pattern with tie and slide, CHOP, the
        embedded sample); the file is the session's XML with "category" and "preset"; a category
        that is not one of ours is dropped; the window's view settings are kept across a load.
      - The user folder is created on the first save, listed sorted; previous and next run
        through the factory bank, then the user's presets, and wrap; a damaged file is skipped.
      - Programs: 16 names, setCurrentProgram loads, the host is told (programChanged) on the
        message thread, and a restored session reports the same program (so a VST3 host's own
        copy of the program number does not load a preset over it).
      - Broken preset files are refused and change nothing.
      - The preset bar: its controls, its menu (factory by category, the user's presets), and
        stepping through it; it paints.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target PresetTests
    Run the resulting PresetTests executable; it exits non-zero on failure.
*/
#include "plugin/NeonSynthProcessor.h"
#include "gui/PresetBar.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace
{
    int failures = 0;

    void check (bool condition, const juce::String& what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what.toRawUTF8());
        if (! condition)
            ++failures;
    }

    constexpr double testRate  = 48000.0;
    constexpr int    blockSize = 512;

    juce::RangedAudioParameter* findParameter (juce::AudioProcessor& p, const juce::String& id)
    {
        for (auto* param : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param); ranged != nullptr && ranged->getParameterID() == id)
                return ranged;
        return nullptr;
    }

    void setParameter (juce::AudioProcessor& p, const juce::String& id, float value)
    {
        if (auto* ranged = findParameter (p, id))
            ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
    }

    float plainValue (juce::AudioProcessor& p, const juce::String& id)
    {
        auto* ranged = findParameter (p, id);
        return ranged != nullptr ? ranged->convertFrom0to1 (ranged->getValue()) : std::nanf ("");
    }

    juce::MemoryBlock save (juce::AudioProcessor& p)
    {
        juce::MemoryBlock block;
        p.getStateInformation (block);
        return block;
    }

    /** A saved state as XML text, with the preset attributes taken off (what "the same session" means). */
    juce::String sessionText (juce::AudioProcessor& p)
    {
        const auto block = save (p);
        auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize());
        if (xml == nullptr)
            return {};
        for (const auto& id : { PresetManager::categoryId, PresetManager::presetId, PresetManager::programId })
            xml->removeAttribute (id);
        return xml->toString();
    }

    /** A mono 24-bit WAV: a tone with a little noise. */
    void writeWav (const juce::File& file)
    {
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions{}.withSampleRate (testRate).withNumChannels (1).withBitsPerSample (24));
        juce::Random random (3);
        juce::AudioBuffer<float> b (1, (int) (0.3 * testRate));
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.05f * (random.nextFloat() * 2.0f - 1.0f)
                               + 0.4f * (float) std::sin (juce::MathConstants<double>::twoPi * 330.0 * i / testRate));
        writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    /** Counts the host notifications a processor sends. */
    struct HostListener : juce::AudioProcessorListener
    {
        int programChanges = 0, stateChanges = 0;
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details) override
        {
            programChanges += details.programChanged ? 1 : 0;
            stateChanges   += details.nonParameterStateChanged ? 1 : 0;
        }
    };

    void pumpMessages()
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
    }

    //==========================================================================
    void testFactoryData()
    {
        std::printf ("Factory data\n");
        NeonSynthProcessor p;
        const auto& presets = FactoryPresets::all();
        check (presets.size() == (size_t) PresetManager::numFactoryPresets, "16 factory presets");

        std::set<juce::String> names;
        bool categoriesKnown = true, idsKnown = true, valuesInRange = true, patternsValid = true, slidesPlay = true, chopValid = true;
        juce::String problem;
        for (const auto& preset : presets)
        {
            names.insert (juce::String (preset.name).toLowerCase());
            categoriesKnown = categoriesKnown && PresetManager::validCategory (preset.category) == preset.category;
            for (const auto& [id, value] : preset.values)
            {
                auto* parameter = findParameter (p, id);
                if (parameter == nullptr)
                {
                    idsKnown = false;
                    problem << preset.name << ": no parameter " << id << "; ";
                    continue;
                }
                const auto range = parameter->getNormalisableRange();
                if (! (value >= range.start && value <= range.end))
                {
                    valuesInRange = false;
                    problem << preset.name << ": " << id << " = " << value << " is outside its range; ";
                }
            }
            if (preset.pattern != nullptr)
            {
                const auto steps = PresetManager::parsePattern (preset.pattern);
                patternsValid = patternsValid && steps.size() == (size_t) StepSequencer::numSteps;
                for (size_t i = 0; i < steps.size(); ++i)
                {
                    patternsValid = patternsValid && steps[i].valid;
                    // The contract: a slide overlaps the next step's note, so that step must play.
                    if (steps[i].slide)
                        slidesPlay = slidesPlay && steps[i].on && steps[(i + 1) % steps.size()].on;
                }
            }
            if (preset.chop != nullptr)
                chopValid = chopValid && juce::String (preset.chop).length() == 16 && juce::String (preset.chop).containsOnly ("01");
        }
        if (problem.isNotEmpty())
            std::printf ("        %s\n", problem.toRawUTF8());
        check (names.size() == presets.size(), "the names are unique");
        check (categoriesKnown, "every category is one of Bass, Lead, Pad, Keys, Sequence, Percussion, FX");
        check (idsKnown, "every parameter a preset sets exists");
        check (valuesInRange, "every value is inside its parameter's range");
        check (patternsValid, "every pattern has 16 valid steps");
        check (slidesPlay, "every slide is on a playing step and slides into one");
        check (chopValid, "every CHOP pattern has 16 steps of 0 and 1");

        const auto step = PresetManager::parseStep ("-D#3:90/");
        check (! step.on && step.note == 51 && step.velocity == 90 && step.slide && ! step.tie && step.valid,
               "the pattern notation: -D#3:90/ is an off step, MIDI 51, velocity 90, sliding");
        check (PresetManager::parseStep ("C-1:1~").note == 0 && PresetManager::parseStep ("C-1:1~").tie
                   && PresetManager::parseStep ("G9:127").note == 127 && ! PresetManager::parseStep ("H2").valid,
               "C-1 is 0, G9 is 127, a tie is ~, H2 is not a note");
    }

    //==========================================================================
    /** What to play for each preset: a phrase that suits it (see each description). */
    std::vector<int> notesFor (const juce::String& name)
    {
        static const std::map<juce::String, std::vector<int>> notes
        {
            { "Glass Bell", { 84, 88, 91 } }, { "Comb Chime", { 60, 64, 67 } }, { "Marimba Chop", { 60, 64, 67 } },
            { "Warm Sub", { 36 } }, { "Folded Growl", { 36 } }, { "Chopped Lead", { 60 } },
            { "Sampled Pad", { 48, 55, 60, 64 } }, { "Shimmer Cloud", { 60, 67, 72 } }, { "Frozen Cloud", { 48, 55, 60 } },
            { "Wind Sweep", { 36 } }, { "Gale", { 36, 43 } }, { "Drifting Cloud", { 48, 55, 60 } },
        };
        const auto found = notes.find (name);
        return found != notes.end() ? found->second : std::vector<int> {};
    }

    struct RenderResult
    {
        bool finite = true;
        float peak = 0.0f, tail = 0.0f;
        double seconds = 0.0;
    };

    /**
        Plays a preset as its description says: held notes for `holdSeconds` (or the sequencer on its
        own clock, with no notes), then the notes released (or the sequencer stopped), then up to
        `tailSeconds` more. The tail is the loudest sample of the last 100 ms.
    */
    RenderResult renderPreset (NeonSynthProcessor& p, const std::vector<int>& notes, bool sequenced,
                               double holdSeconds, double tailSeconds)
    {
        RenderResult result;
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;
        const int holdBlocks = (int) (holdSeconds * testRate / blockSize);
        const int totalBlocks = holdBlocks + (int) (tailSeconds * testRate / blockSize);
        const int tailBlocks = (int) (0.1 * testRate / blockSize) + 1;

        for (int b = 0; b < totalBlocks; ++b)
        {
            midi.clear();
            if (b == 0)
                for (int i = 0; i < (int) notes.size(); ++i)
                    midi.addEvent (juce::MidiMessage::noteOn (1, notes[(size_t) i], (juce::uint8) 110), i * 40);
            if (b == holdBlocks)
            {
                for (int note : notes)
                    midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
                if (sequenced)
                    setParameter (p, "seqOn", 0.0f);
            }
            p.processBlock (buffer, midi);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                {
                    const float v = buffer.getSample (ch, i);
                    result.finite = result.finite && std::isfinite (v);
                    result.peak = juce::jmax (result.peak, std::abs (v));
                    if (b >= totalBlocks - tailBlocks)
                        result.tail = juce::jmax (result.tail, std::abs (v));
                }
        }
        result.seconds = (double) totalBlocks * blockSize / testRate;
        return result;
    }

    void testFactoryPresetsLoadAndRender()
    {
        std::printf ("Every factory preset loads and renders\n");
        NeonSynthProcessor p;
        p.setRateAndBufferSizeDetails (testRate, blockSize);
        p.prepareToPlay (testRate, blockSize);
        auto& presets = p.getPresetManager();

        // Start from a sound far from every preset, so a parameter a preset forgot shows up.
        juce::String error;
        for (auto* parameter : p.getParameters())
            parameter->setValueNotifyingHost (0.77f);

        bool valuesRight = true, allFinite = true, allUnderClipper = true, allSilent = true, programsRight = true, padLoaded = true;
        std::printf ("        %-16s %-11s %9s %12s\n", "preset", "category", "peak dBFS", "tail dBFS");
        for (int index = 0; index < PresetManager::numFactoryPresets; ++index)
        {
            const auto& preset = FactoryPresets::all()[(size_t) index];
            p.reset();
            const bool loaded = presets.loadFactory (index);
            programsRight = programsRight && loaded && p.getCurrentProgram() == index && presets.getCurrentName() == preset.name
                         && presets.getCurrentCategory() == preset.category;

            for (auto* parameter : p.getParameters())
            {
                auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter);
                if (ranged == nullptr)
                    continue;
                float expected = ranged->convertFrom0to1 (ranged->getDefaultValue());
                for (const auto& [id, v] : preset.values)
                    if (ranged->getParameterID() == id)
                        expected = ranged->convertFrom0to1 (ranged->convertTo0to1 (v));   // as the range snaps it
                const float actual = ranged->convertFrom0to1 (ranged->getValue());
                if (std::abs (actual - expected) > 1.0e-4f * juce::jmax (1.0f, std::abs (expected)))
                {
                    valuesRight = false;
                    std::printf ("        %s: %s is %g, expected %g\n", preset.name, ranged->getParameterID().toRawUTF8(), actual, expected);
                }
            }
            padLoaded = padLoaded && p.withCurrentSample ([] (const SampleData* s) { return s != nullptr && s->getOrigin().isDefault; });
            if (preset.pattern != nullptr)
            {
                const auto steps = PresetManager::parsePattern (preset.pattern);
                for (int i = 0; i < StepSequencer::numSteps; ++i)
                {
                    const auto s = p.getSequencerStep (i);
                    const auto& e = steps[(size_t) i];
                    valuesRight = valuesRight && s.on == e.on && s.note == e.note && s.velocity == e.velocity
                               && s.tie == e.tie && s.slide == e.slide;
                }
            }

            const bool sequenced = plainValue (p, "seqOn") > 0.5f;
            const double attack = plainValue (p, "attack"), release = plainValue (p, "release");
            const auto result = renderPreset (p, sequenced ? std::vector<int> {} : notesFor (preset.name), sequenced,
                                              sequenced ? 4.0 : juce::jmax (2.0, attack + 2.0), release + 4.0);
            const auto db = [] (float v) { return v > 0.0f ? juce::Decibels::gainToDecibels (v) : -200.0f; };
            std::printf ("        %-16s %-11s %9.1f %12.1f\n", preset.name, preset.category, db (result.peak), db (result.tail));

            allFinite = allFinite && result.finite;
            allUnderClipper = allUnderClipper && result.peak > 0.0f && result.peak < NeonSynthProcessor::softClipKnee;
            allSilent = allSilent && result.tail < 1.0e-5f;
            if (! (result.peak > 0.001f))
            {
                allUnderClipper = false;
                std::printf ("        %s is (almost) silent\n", preset.name);
            }
        }
        check (programsRight, "each loads as the current program, with its name and category");
        check (valuesRight, "each sets every parameter to its value or its default, and its pattern");
        check (padLoaded, "each plays the built-in pad");
        check (allFinite, "each renders finite audio");
        check (allUnderClipper, "each sounds, and peaks under the output soft clipper (-6 dBFS)");
        check (allSilent, "each ends in silence once released (sequenced ones: once the sequencer stops)");

        // A preset is the whole session: what was loaded before it leaves nothing behind.
        presets.loadFactory (13);
        presets.loadFactory (2);
        const auto afterWind = sessionText (p);
        presets.loadFactory (10);
        setParameter (p, "cutoff", 50.0f);
        p.setChopStep (5, true);
        presets.loadFactory (2);
        check (sessionText (p) == afterWind, "a preset loads the same session whatever came before it");
    }

    //==========================================================================
    void testSaveAndLoad (const juce::File& dir)
    {
        std::printf ("Save and load\n");
        NeonSynthProcessor p;
        p.setRateAndBufferSizeDetails (testRate, blockSize);
        p.prepareToPlay (testRate, blockSize);
        auto& presets = p.getPresetManager();
        presets.setUserFolder (dir.getChildFile ("not yet/Presets"));

        // A session far from the defaults: parameters, a pattern with a tie and a slide, CHOP, a sample.
        setParameter (p, "cutoff", 456.0f);
        setParameter (p, "source", 2.0f);
        setParameter (p, "seqLength", 7.0f);
        setParameter (p, "glide", 120.0f);
        p.setSequencerStep (3, { true, 41, 99, true, false });
        p.setSequencerStep (4, { true, 43, 77, false, true });
        p.setChopStep (0, true);
        const auto wav = dir.getChildFile ("tone.wav");
        writeWav (wav);
        juce::String error;
        check (p.loadSampleFile (wav, error), "the test sample loads");
        p.getAPVTS().state.setProperty ("view", 0, nullptr);
        const auto before = sessionText (p);

        check (! presets.getUserFolder().exists(), "the user folder does not exist before the first save");
        const auto file = presets.userFileFor ("Deep: Bass/1?");
        const bool saved = presets.saveFile (file, "Bass", error);
        check (saved && file.existsAsFile() && file.getParentDirectory() == presets.getUserFolder(),
               "SAVE creates the user folder and writes the file there (" + file.getFileName() + ")");
        check (file.getFileName().endsWith (".neonpreset") && ! file.getFileNameWithoutExtension().containsAnyOf ("/:?"),
               "the file name is made safe for every file system");
        check (presets.getCurrentName() == file.getFileNameWithoutExtension() && presets.getCurrentCategory() == "Bass"
                   && presets.getCurrentProgramIndex() == -1 && p.getCurrentProgram() == 0,
               "the saved file is now the current preset (not a host program)");

        const auto xml = juce::XmlDocument::parse (file);
        check (xml != nullptr && xml->hasTagName ("Parameters") && xml->getStringAttribute ("category") == "Bass"
                   && xml->getStringAttribute ("preset") == file.getFileNameWithoutExtension() && ! xml->hasAttribute ("program")
                   && xml->getChildByName ("Pattern") != nullptr && xml->getChildByName ("Chop") != nullptr
                   && xml->getChildByName ("Sample") != nullptr && xml->getChildByName ("Sample")->getStringAttribute ("flac").isNotEmpty(),
               "the file is the session's XML with category and preset (pattern, CHOP, the embedded sample)");

        // Something else, then the file again: the session comes back.
        presets.loadFactory (5);
        check (sessionText (p) != before, "a factory preset changes the session");
        p.getAPVTS().state.setProperty ("view", 1, nullptr);
        wav.deleteFile();   // the embedded copy must be what comes back
        check (presets.loadFile (file, error), "the preset file loads");
        p.getAPVTS().state.setProperty ("view", 0, nullptr);   // (the view is compared separately below)
        if (sessionText (p) != before) { before.substring(0, 3000).toStdString(); juce::File ("/tmp/claude-0/-home-claude/b4908674-0ab9-5223-8f78-dfae9416cf95/scratchpad/a.xml").replaceWithText (before.substring (0, 20000)); juce::File ("/tmp/claude-0/-home-claude/b4908674-0ab9-5223-8f78-dfae9416cf95/scratchpad/b.xml").replaceWithText (sessionText (p).substring (0, 20000)); }
        check (sessionText (p) == before, "save then load gives back the same session: parameters, pattern, CHOP, sample");
        check (presets.getCurrentName() == file.getFileNameWithoutExtension() && presets.getCurrentCategory() == "Bass",
               "with the file's name and category");

        // The window's view is the listener's choice, not the preset's.
        p.getAPVTS().state.setProperty ("view", 1, nullptr);
        p.getAPVTS().state.setProperty ("scanlines", false, nullptr);
        presets.loadFile (file, error);
        const bool viewKept = (int) p.getAPVTS().state.getProperty ("view") == 1 && ! (bool) p.getAPVTS().state.getProperty ("scanlines", true);
        presets.loadFactory (0);
        check (viewKept && (int) p.getAPVTS().state.getProperty ("view") == 1 && ! (bool) p.getAPVTS().state.getProperty ("scanlines", true),
               "loading a user or a factory preset keeps the window's view settings (SPECTRUM, SCANLINES)");

        // Categories.
        const auto plain = presets.userFileFor ("Plain");
        presets.saveFile (plain, {}, error);
        const auto odd = presets.userFileFor ("Odd");
        presets.saveFile (odd, "Banana", error);
        const auto plainXml = juce::XmlDocument::parse (plain), oddXml = juce::XmlDocument::parse (odd);
        check (plainXml != nullptr && ! plainXml->hasAttribute ("category") && oddXml != nullptr && ! oddXml->hasAttribute ("category"),
               "no category, or one that is not ours, is saved as none");
        auto edited = juce::XmlDocument::parse (plain);
        edited->setAttribute ("category", "  pad ");
        edited->setAttribute ("program", 4);
        const auto hand = dir.getChildFile ("hand.neonpreset");
        edited->writeTo (hand);
        presets.loadFile (hand, error);
        check (presets.getCurrentCategory() == "Pad" && presets.getCurrentProgramIndex() == -1 && presets.getCurrentName() == "hand",
               "a hand-edited category is read leniently; a user file never claims to be a host program");

        // Previous and next: the factory bank, then the user's presets, sorted, wrapping around.
        const auto users = presets.getUserPresets();
        check (users.size() == 3 && users[1].getFileNameWithoutExtension() == "Odd" && users[2].getFileNameWithoutExtension() == "Plain",
               "the user folder lists its presets sorted by name");
        presets.loadFactory (PresetManager::numFactoryPresets - 1);
        presets.step (+1);
        const bool toFirstUser = presets.getCurrentFile() == users[0];
        presets.step (+1);
        const bool toSecondUser = presets.getCurrentFile() == users[1];
        presets.loadFile (users[2], error);
        presets.step (+1);
        const bool wrapsToFactory = presets.getCurrentProgramIndex() == 0;
        presets.step (-1);
        const bool backToLastUser = presets.getCurrentFile() == users[2];
        check (toFirstUser && toSecondUser && wrapsToFactory && backToLastUser,
               "next goes from the last factory preset to the user's, and wraps; previous goes back");

        users[1].replaceWithText ("<Parameters><Pattern");   // damaged
        presets.loadFile (users[0], error);
        presets.step (+1);
        check (presets.getCurrentFile() == users[2], "next skips a damaged user preset");

        // Broken files: refused, nothing changes.
        presets.loadFactory (7);
        const auto good = sessionText (p);
        bool refused = true;
        const auto junk = dir.getChildFile ("junk.neonpreset");
        for (const char* text : { "", "not xml", "<Other/>", "<Parameters stateVersion=\"4\"" })
        {
            junk.replaceWithText (text);
            const bool r = ! presets.loadFile (junk, error) && error.isNotEmpty();
            if (! r) std::printf ("DEBUG not refused: %s\n", text);
            refused = refused && r;
        }
        refused = refused && ! presets.loadFile (dir.getChildFile ("missing.neonpreset"), error);
        std::printf ("DEBUG %d %d %d\n", (int) refused, (int) (sessionText (p) == good), presets.getCurrentProgramIndex());
        check (refused && sessionText (p) == good && presets.getCurrentProgramIndex() == 7,
               "a missing, empty, non-XML or foreign file is refused, and the current sound stays");
    }

    //==========================================================================
    void testPrograms()
    {
        std::printf ("Host programs\n");
        NeonSynthProcessor p;
        p.setRateAndBufferSizeDetails (testRate, blockSize);
        p.prepareToPlay (testRate, blockSize);
        HostListener host;
        p.addListener (&host);

        bool namesRight = p.getNumPrograms() == PresetManager::numFactoryPresets;
        for (int i = 0; i < p.getNumPrograms(); ++i)
            namesRight = namesRight && p.getProgramName (i) == FactoryPresets::all()[(size_t) i].name;
        check (namesRight, "16 programs, named after the factory presets");
        check (p.getCurrentProgram() == 0 && p.getPresetManager().getCurrentName().isEmpty(), "a new instance has no preset (program 0)");

        pumpMessages();
        host.programChanges = host.stateChanges = 0;
        p.setCurrentProgram (9);
        const bool loaded = p.getCurrentProgram() == 9 && std::abs (plainValue (p, "cutoff") - 700.0f) < 0.5f;
        const bool notYet = host.programChanges == 0;
        pumpMessages();
        check (loaded, "setCurrentProgram loads that factory preset");
        check (notYet && host.programChanges == 1 && host.stateChanges >= 1,
               "the host hears of it afterwards, on the message thread (program and state changed)");
        p.setCurrentProgram (-1);
        p.setCurrentProgram (99);
        check (p.getCurrentProgram() == 9, "an out-of-range program number is ignored");

        // The session remembers the program, so a restored project reports the same one.
        setParameter (p, "cutoff", 1234.0f);
        const auto session = save (p);
        NeonSynthProcessor restored;
        restored.setStateInformation (session.getData(), (int) session.getSize());
        check (restored.getCurrentProgram() == 9 && restored.getPresetManager().getCurrentName() == "Rounded Bass"
                   && std::abs (plainValue (restored, "cutoff") - 1234.0f) < 0.5f,
               "a restored session reports the program it came from, with its own edits");

        // A session from before v1.11.0 has no preset.
        restored.setCurrentProgram (3);
        juce::MemoryBlock old;
        if (auto xml = juce::XmlDocument::parse (juce::File (NEONSYNTH_TESTS_DIR).getChildFile ("fixtures/state/v1.10.0.xml")))
            juce::AudioProcessor::copyXmlToBinary (*xml, old);
        restored.setStateInformation (old.getData(), (int) old.getSize());
        check (old.getSize() > 0 && restored.getCurrentProgram() == 0 && restored.getPresetManager().getCurrentName().isEmpty()
                   && restored.getPresetManager().getCurrentProgramIndex() == -1,
               "a v1.10.0 session loads with no preset");

        // Hostile values in a session's preset attributes.
        auto hostile = juce::AudioProcessor::getXmlFromBinary (session.getData(), (int) session.getSize());
        hostile->setAttribute ("program", 4000);
        hostile->setAttribute ("category", juce::String::repeatedString ("x", 5000));
        hostile->setAttribute ("preset", juce::String::repeatedString ("y", 100000));
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (*hostile, block);
        restored.setStateInformation (block.getData(), (int) block.getSize());
        check (restored.getPresetManager().getCurrentProgramIndex() == -1 && restored.getPresetManager().getCurrentCategory().isEmpty()
                   && restored.getPresetManager().getCurrentName().length() <= 128,
               "a session's preset attributes are checked: program in range, a known category, a short name");
        p.removeListener (&host);
    }

    //==========================================================================
    void testPresetBar (const juce::File& dir)
    {
        std::printf ("Preset bar\n");
        NeonSynthProcessor p;
        auto& presets = p.getPresetManager();
        presets.setUserFolder (dir.getChildFile ("bar"));
        juce::String error;
        presets.saveFile (presets.userFileFor ("Mine"), "Lead", error);
        presets.loadFactory (0);

        PresetBar bar (presets);
        bar.setBounds (0, 0, 308, 32);
        const auto* prev = bar.findChildWithID ("presetPrev");
        const auto* name = bar.findChildWithID ("presetName");
        const auto* next = bar.findChildWithID ("presetNext");
        const auto* saveButton = bar.findChildWithID ("presetSave");
        check (prev != nullptr && name != nullptr && next != nullptr && saveButton != nullptr,
               "the bar has previous, name, next and SAVE (by component ID)");
        bool inside = true, apart = true;
        for (const auto* c : { prev, name, next, saveButton })
            inside = inside && c != nullptr && bar.getLocalBounds().contains (c->getBounds()) && c->getWidth() >= 30;
        for (const auto* a : { prev, name, next, saveButton })
            for (const auto* b : { prev, name, next, saveButton })
                apart = apart && (a == b || a == nullptr || b == nullptr || ! a->getBounds().intersects (b->getBounds()));
        check (inside && apart, "they fit in the bar's 308 x 32 without overlapping");
        check (bar.getShownName() == "Glass Bell" && bar.getShownCategory() == "Keys", "the bar shows the preset and its category");

        if (auto* button = dynamic_cast<juce::Button*> (bar.findChildWithID ("presetNext")))
            button->onClick();
        check (presets.getCurrentProgramIndex() == 1 && bar.getShownName() == "Comb Chime", "> loads the next preset and shows it");
        if (auto* button = dynamic_cast<juce::Button*> (bar.findChildWithID ("presetPrev")))
        {
            button->onClick();
            button->onClick();
        }
        check (presets.getCurrentName() == "Mine" && bar.getShownCategory() == "Lead", "< from the first factory preset wraps to the user's");

        auto menu = bar.createMenu();
        int submenus = 0, userItems = 0, factoryItems = 0;
        for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
        {
            const auto& item = it.getItem();
            submenus += item.subMenu != nullptr ? 1 : 0;
            factoryItems += item.itemID >= 1 && item.itemID <= PresetManager::numFactoryPresets ? 1 : 0;
            userItems += item.itemID >= 1000 && item.itemID < 3000 ? 1 : 0;
        }
        check (submenus == PresetManager::categories().size() && factoryItems == PresetManager::numFactoryPresets && userItems == 1,
               "the menu has the factory bank in its seven categories and the user's preset");
        bar.menuItemChosen (PresetManager::numFactoryPresets);   // the last factory preset
        check (presets.getCurrentProgramIndex() == PresetManager::numFactoryPresets - 1, "choosing a factory preset in the menu loads it");
        bar.menuItemChosen (1000);
        check (presets.getCurrentName() == "Mine", "choosing a user preset loads it");

        // A session restored by the host reaches the bar through the manager's change message.
        presets.loadFactory (4);
        const auto state = save (p);
        presets.loadFactory (5);
        p.setStateInformation (state.getData(), (int) state.getSize());
        pumpMessages();
        check (bar.getShownName() == "Folded Growl", "the bar follows a session restored by the host");

        juce::Image image (juce::Image::ARGB, 308, 32, true);
        {
            juce::Graphics g (image);
            bar.paintEntireComponent (g, true);
        }
        bool drawn = false;
        for (int y = 0; y < 32 && ! drawn; ++y)
            for (int x = 0; x < 308 && ! drawn; ++x)
                drawn = image.getPixelAt (x, y).getAlpha() > 0;
        check (drawn, "the bar paints");
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // the change messages and the host notification use the message thread
    const auto dir = juce::File::createTempFile ("neonsynth-presets");
    dir.createDirectory();

    testFactoryData();
    testFactoryPresetsLoadAndRender();
    testSaveAndLoad (dir);
    testPrograms();
    testPresetBar (dir);

    dir.deleteRecursively();
    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
