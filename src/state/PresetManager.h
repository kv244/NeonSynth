// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "state/FactoryPresets.h"
#include "state/StateFormat.h"

/**
    Presets (v1.11.0).

    A .neonpreset file is the session's own XML, exactly what getStateInformation() writes (every
    parameter, the pattern, CHOP, the sample by reference or embedded), with two attributes on the
    root: "category" (Bass, Lead, Pad, Keys, Sequence, Percussion, FX; absent = none) and "preset"
    (its name). Loading one goes through setStateInformation(), so a preset file gets every check
    a session gets: it is untrusted input, a file someone may have sent.

    The bank is the 16 factory presets (FactoryPresets.h, compiled in) followed by the user's
    files in Documents/NeonSynth/Presets (created on the first save). Previous and next step
    through both in that order and wrap around. The factory presets are the host's program list.

    What the session remembers. Loading a preset leaves three attributes in the processor's
    state, so they are saved with the session: "preset" (the name), "category", and, for a factory
    preset, "program" (its index). A restored session therefore shows its preset again, and,
    importantly, reports the same current program: a VST3 host that restores its own copy of the
    program number after the session sees nothing to change, instead of loading that factory
    preset over the restored sound. A session from before v1.11.0 has none of them (no preset).

    A preset changes the sound, not the window: the editor's view settings (SCOPE or SPECTRUM, the
    envelope tab, SCANLINES), which live in the same state, are kept as they were when a preset
    loads.

    Threads. Every call here is for the message thread, except that the host may call the program
    functions (and setStateInformation) on a thread of its choosing; the shared fields are guarded
    by a lock the audio thread never takes. The host is told of a change asynchronously, on the
    message thread. Listeners (the preset bar) hear of every change, a session restored by the
    host included, through the ChangeBroadcaster, also on the message thread.
*/
class PresetManager : public juce::ChangeBroadcaster,
                      private juce::ValueTree::Listener,
                      private juce::AsyncUpdater
{
public:
    static constexpr const char* fileExtension = ".neonpreset";
    static constexpr int numFactoryPresets = 16;

    /** Largest preset file read: a session with the largest embedded sample is about 14 MB. */
    static constexpr juce::int64 maxFileBytes = 32 * 1024 * 1024;

    static inline const juce::Identifier categoryId { "category" };
    static inline const juce::Identifier presetId   { "preset" };
    static inline const juce::Identifier programId  { "program" };

    /** The categories, in the order the menu shows them. "" (none) is not in the list. */
    static const juce::StringArray& categories()
    {
        static const juce::StringArray names { "Bass", "Lead", "Pad", "Keys", "Sequence", "Percussion", "FX" };
        return names;
    }

    /** One of categories(), or "" for anything else (an untrusted file's attribute). */
    static juce::String validCategory (const juce::String& text)
    {
        const int index = categories().indexOf (text.trim(), true);
        return index >= 0 ? categories()[index] : juce::String();
    }

    /** Documents/NeonSynth/Presets: where SAVE writes and the menu looks. */
    static juce::File defaultUserFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("NeonSynth").getChildFile ("Presets");
    }

    PresetManager (juce::AudioProcessor& p, juce::AudioProcessorValueTreeState& state)
        : processor (p), apvts (state)
    {
        jassert (FactoryPresets::all().size() == (size_t) numFactoryPresets);
        apvts.state.addListener (this);
    }

    ~PresetManager() override
    {
        apvts.state.removeListener (this);
        cancelPendingUpdate();
    }

    /** Tests point this elsewhere, so they never touch the real Documents folder. */
    void setUserFolder (const juce::File& folder)   { const juce::ScopedLock sl (lock); userFolder = folder; }
    juce::File getUserFolder() const                { const juce::ScopedLock sl (lock); return userFolder; }

    //==========================================================================
    // Factory bank

    static juce::String getFactoryName (int index)
    {
        return juce::isPositiveAndBelow (index, numFactoryPresets) ? juce::String (FactoryPresets::all()[(size_t) index].name) : juce::String();
    }

    static juce::String getFactoryCategory (int index)
    {
        return juce::isPositiveAndBelow (index, numFactoryPresets) ? juce::String (FactoryPresets::all()[(size_t) index].category) : juce::String();
    }

    static juce::String getFactoryDescription (int index)
    {
        return juce::isPositiveAndBelow (index, numFactoryPresets) ? juce::String (FactoryPresets::all()[(size_t) index].description) : juce::String();
    }

    /**
        The factory preset as a complete .neonpreset document: every parameter (the listed ones
        at their values, the rest at their defaults), the pattern (or none, which loads the
        default), CHOP and the built-in pad.
    */
    std::unique_ptr<juce::XmlElement> createFactoryXml (int index) const
    {
        if (! juce::isPositiveAndBelow (index, numFactoryPresets))
            return {};
        const auto& preset = FactoryPresets::all()[(size_t) index];

        auto xml = std::make_unique<juce::XmlElement> (apvts.state.getType());
        xml->setAttribute (StateFormat::stateVersionId, StateFormat::currentStateVersion);
        xml->setAttribute (categoryId, preset.category);
        xml->setAttribute (presetId, preset.name);
        xml->setAttribute (programId, index);

        for (auto* parameter : processor.getParameters())
        {
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter);
            if (ranged == nullptr)
                continue;
            float value = ranged->convertFrom0to1 (ranged->getDefaultValue());
            for (const auto& [id, v] : preset.values)
                if (ranged->getParameterID() == id)
                    value = v;
            auto* param = xml->createNewChildElement (StateFormat::paramId);
            param->setAttribute (StateFormat::paramIdId, ranged->getParameterID());
            param->setAttribute (StateFormat::valueId, (double) value);
        }

        if (preset.pattern != nullptr)
        {
            auto* pattern = xml->createNewChildElement (StateFormat::patternId);
            for (const auto& parsed : parsePattern (preset.pattern))
            {
                jassert (parsed.valid);   // PresetTests checks every factory pattern parses
                auto* s = pattern->createNewChildElement (StateFormat::stepId);
                s->setAttribute (StateFormat::onId, parsed.on ? 1 : 0);
                s->setAttribute (StateFormat::noteId, parsed.note);
                s->setAttribute (StateFormat::velocityId, parsed.velocity);
                if (parsed.tie)   s->setAttribute (StateFormat::tieId, 1);
                if (parsed.slide) s->setAttribute (StateFormat::slideId, 1);
            }
        }
        if (preset.chop != nullptr)
            xml->createNewChildElement (StateFormat::chopId)->setAttribute (StateFormat::chopPatternId, preset.chop);
        xml->createNewChildElement (StateFormat::sampleId)->setAttribute (StateFormat::sampleDefaultId, 1);
        return xml;
    }

    /** One step as the factory patterns write it (see FactoryPresets.h). */
    struct PatternStep
    {
        bool on = false;
        int note = 60, velocity = 100;
        bool tie = false, slide = false;
        bool valid = false;
    };

    /** Parses one step: "-D#3:90~". C4 = 60. */
    static PatternStep parseStep (juce::String token)
    {
        PatternStep parsed;
        parsed.on = ! token.startsWithChar ('-');
        token = token.trimCharactersAtStart ("-");
        parsed.tie   = token.endsWithChar ('~');
        parsed.slide = token.endsWithChar ('/');
        token = token.trimCharactersAtEnd ("~/");

        const auto name = token.upToFirstOccurrenceOf (":", false, false);
        if (token.containsChar (':'))
            parsed.velocity = token.fromFirstOccurrenceOf (":", false, false).getIntValue();

        const int pitchClass = juce::String ("C D EF G A B").indexOfChar (name[0]);
        if (pitchClass < 0 || name.length() < 2)
            return parsed;
        const bool sharp = name[1] == '#';
        const auto octave = name.substring (sharp ? 2 : 1);
        if (octave.isEmpty() || ! octave.trimCharactersAtStart ("-").containsOnly ("0123456789"))
            return parsed;
        parsed.note = 12 * (octave.getIntValue() + 1) + pitchClass + (sharp ? 1 : 0);
        parsed.valid = juce::isPositiveAndBelow (parsed.note, 128) && parsed.velocity >= 1 && parsed.velocity <= 127;
        return parsed;
    }

    static std::vector<PatternStep> parsePattern (const char* text)
    {
        std::vector<PatternStep> steps;
        for (const auto& token : juce::StringArray::fromTokens (text, " ", ""))
            if (token.isNotEmpty())
                steps.push_back (parseStep (token));
        return steps;
    }

    //==========================================================================
    // Loading and saving

    /**
        The host's program change. Hosts are meant to call it on the message thread, but a
        wrapper or host may not; loading builds XML and replaces the state, which must never run
        on the audio thread, so from any other thread the load is passed to the message thread.
    */
    void requestFactory (int index)
    {
        if (juce::MessageManager::existsAndIsCurrentThread())
        {
            loadFactory (index);
            return;
        }
        if (juce::isPositiveAndBelow (index, numFactoryPresets))
        {
            pendingProgram.store (index, std::memory_order_release);
            triggerAsyncUpdate();
        }
    }

    /** Message thread. False if the index is out of range. */
    bool loadFactory (int index)
    {
        const auto xml = createFactoryXml (index);
        if (xml == nullptr)
            return false;
        const auto text = xml->toString (juce::XmlElement::TextFormat().singleLine());
        return loadText (text.toRawUTF8(), text.getNumBytesAsUTF8(), getFactoryName (index), index, {});
    }

    /** Loads a .neonpreset file. On failure the current sound stays and `error` says why. */
    bool loadFile (const juce::File& file, juce::String& error)
    {
        if (! file.existsAsFile())
        {
            error = "The preset file is missing";
            return false;
        }
        if (file.getSize() > maxFileBytes)
        {
            error = "The file is too large to be a NeonSynth preset";
            return false;
        }
        juce::MemoryBlock data;
        if (! file.loadFileAsData (data) || data.getSize() == 0)
        {
            error = "The preset file could not be read";
            return false;
        }
        if (! endsLikeAPreset (data) || ! loadText (data.getData(), data.getSize(), file.getFileNameWithoutExtension(), -1, file))
        {
            error = "Not a NeonSynth preset, or a damaged one";
            return false;
        }
        error = {};
        return true;
    }

    /**
        Saves the current session as a .neonpreset file, named after the file, with a category
        ("" = none). The file is then the current preset. False, with `error`, if it can't be written.
    */
    bool saveFile (const juce::File& file, const juce::String& category, juce::String& error)
    {
        juce::MemoryBlock state;
        processor.getStateInformation (state);
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        if (xml == nullptr)
        {
            error = "The session could not be written";
            return false;
        }

        const auto name = file.getFileNameWithoutExtension();
        const auto checkedCategory = validCategory (category);
        xml->removeAttribute (programId);   // a user preset is not one of the host's programs
        xml->setAttribute (presetId, name);
        if (checkedCategory.isNotEmpty()) xml->setAttribute (categoryId, checkedCategory);
        else                              xml->removeAttribute (categoryId);

        if (! file.getParentDirectory().createDirectory() || ! xml->writeTo (file))
        {
            error = "Could not write " + file.getFullPathName();
            return false;
        }

        setCurrent (name, checkedCategory, -1, file);
        writeMetaToState();
        triggerAsyncUpdate();   // the session's preset name changed
        error = {};
        return true;
    }

    /** The file SAVE writes for a name: in the user folder, the name made safe for a file name. */
    juce::File userFileFor (const juce::String& name) const
    {
        auto safe = juce::File::createLegalFileName (name.trim()).substring (0, 64).trim();
        if (safe.isEmpty())
            safe = "Untitled";
        return getUserFolder().getChildFile (safe + fileExtension);
    }

    /** The user folder's presets, sorted by name (case-insensitive). */
    juce::Array<juce::File> getUserPresets() const
    {
        auto files = getUserFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension);
        struct ByName
        {
            static int compareElements (const juce::File& a, const juce::File& b)
            {
                return a.getFileNameWithoutExtension().compareNatural (b.getFileNameWithoutExtension());
            }
        } byName;
        files.sort (byName);
        return files;
    }

    /** Factory presets first, then the user's; delta +1 = next, -1 = previous. Wraps around. */
    bool step (int delta)
    {
        const auto users = getUserPresets();
        const int total = numFactoryPresets + users.size();
        int position = getCurrentProgramIndex();
        if (position < 0)
        {
            const auto file = getCurrentFile();
            position = file != juce::File() ? users.indexOf (file) : -1;
            if (position >= 0)
                position += numFactoryPresets;
        }
        // Nothing loaded yet: next is the first preset, previous the last.
        const int target = position < 0 ? (delta > 0 ? 0 : total - 1)
                                        : ((position + delta) % total + total) % total;

        // A user file that fails to load is skipped, so one damaged file can't block the way.
        for (int tried = 0, t = target; tried < total; ++tried, t = ((t + (delta >= 0 ? 1 : -1)) % total + total) % total)
        {
            if (t < numFactoryPresets)
                return loadFactory (t);
            juce::String error;
            if (loadFile (users[t - numFactoryPresets], error))
                return true;
        }
        return false;
    }

    //==========================================================================
    // What is loaded now (any thread except the audio thread)

    juce::String getCurrentName() const       { const juce::ScopedLock sl (lock); return currentName; }
    juce::String getCurrentCategory() const   { const juce::ScopedLock sl (lock); return currentCategory; }
    juce::File   getCurrentFile() const       { const juce::ScopedLock sl (lock); return currentFile; }

    /** The factory preset loaded last, or -1 (none, or a user preset). */
    int getCurrentProgramIndex() const        { const juce::ScopedLock sl (lock); return currentProgram; }

private:
    /** The 4-byte header copyXmlToBinary() puts before XML, read from JUCE itself once rather
        than copied as a number, so it cannot drift from the JUCE version in use. */
    static int xmlBinaryMagic()
    {
        static const int magic = []
        {
            juce::MemoryBlock block;
            juce::AudioProcessor::copyXmlToBinary (juce::XmlElement ("x"), block);
            return juce::MemoryInputStream (block, false).readInt();
        }();
        return magic;
    }

    /** The state's text, wrapped the way copyXmlToBinary() wraps XML, through setStateInformation(). */
    bool loadText (const void* text, size_t numBytes, const juce::String& name, int program, const juce::File& file)
    {
        if (numBytes == 0 || numBytes > (size_t) maxFileBytes)
            return false;

        juce::MemoryBlock block;
        {
            juce::MemoryOutputStream out (block, false);
            out.writeInt (xmlBinaryMagic());   // the header copyXmlToBinary() writes
            out.writeInt ((int) numBytes);
            out.write (text, numBytes);
            out.writeByte (0);
        }

        // The view settings now, to put back afterwards.
        juce::NamedValueSet view;
        for (int i = 0; i < apvts.state.getNumProperties(); ++i)
            if (const auto id = apvts.state.getPropertyName (i); isViewProperty (id))
                view.set (id, apvts.state.getProperty (id));

        stateReplaced = false;
        processor.setStateInformation (block.getData(), (int) block.getSize());
        if (! stateReplaced)
            return false;   // refused by setStateInformation's checks: nothing changed

        for (int i = apvts.state.getNumProperties(); --i >= 0;)
            if (const auto id = apvts.state.getPropertyName (i); isViewProperty (id))
                apvts.state.removeProperty (id, nullptr);
        for (const auto& v : view)
            apvts.state.setProperty (v.name, v.value, nullptr);

        // valueTreeRedirected() has read the file's own category; the name and program come from here.
        setCurrent (name, program >= 0 ? getFactoryCategory (program) : getCurrentCategory(), program, file);
        writeMetaToState();
        triggerAsyncUpdate();
        return true;
    }

    /**
        A file cut short (a full disk, a copy that stopped) would often still parse: JUCE's XML
        parser accepts an element that never closes, and the state would then load with its
        parameters missing, which keep whatever values they had. A complete preset ends with the
        root's closing tag (a preset always has children, so never with a self-closing root), so
        anything else is refused before it is parsed at all. Not "/>": every PARAM line ends that
        way, so a file cut after any of them would pass.
    */
    bool endsLikeAPreset (const juce::MemoryBlock& data) const
    {
        const auto* text = static_cast<const char*> (data.getData());
        auto end = data.getSize();
        while (end > 0 && (text[end - 1] == 0 || juce::CharacterFunctions::isWhitespace (text[end - 1])))
            --end;
        const auto closing = "</" + apvts.state.getType().toString() + ">";
        const auto tail = juce::String::fromUTF8 (text + end - juce::jmin (end, (size_t) 64), (int) juce::jmin (end, (size_t) 64));
        return tail.endsWith (closing);
    }

    static bool isViewProperty (const juce::Identifier& id)
    {
        static const juce::Identifier stateVersionId { "stateVersion" };
        return id != categoryId && id != presetId && id != programId && id != stateVersionId;
    }

    void setCurrent (const juce::String& name, const juce::String& category, int program, const juce::File& file)
    {
        {
            const juce::ScopedLock sl (lock);
            currentName = name.substring (0, 128);
            currentCategory = validCategory (category);
            currentProgram = program;
            currentFile = file;
        }
        sendChangeMessage();
    }

    /** The current preset into the state (so the session remembers it). Message thread. */
    void writeMetaToState()
    {
        // Copied out first: the state's listeners (the APVTS) take their own lock, so ours is not
        // held while they run.
        const auto name = getCurrentName(), category = getCurrentCategory();
        const int program = getCurrentProgramIndex();
        auto& state = apvts.state;
        state.setProperty (presetId, name, nullptr);
        if (category.isNotEmpty()) state.setProperty (categoryId, category, nullptr);
        else                       state.removeProperty (categoryId, nullptr);
        if (program >= 0)          state.setProperty (programId, program, nullptr);
        else                       state.removeProperty (programId, nullptr);
    }

    /** apvts.replaceState(): a preset or a session was loaded (on setStateInformation's thread). */
    void valueTreeRedirected (juce::ValueTree& tree) override
    {
        stateReplaced = true;

        // Untrusted: the name is shortened, the category must be one of ours, the program a factory index.
        const auto program = (int) tree.getProperty (programId, -1);
        setCurrent (tree.getProperty (presetId).toString(), tree.getProperty (categoryId).toString(),
                    juce::isPositiveAndBelow (program, numFactoryPresets) ? program : -1, {});
    }

    /** Tells the host, on the message thread, that the program and the state changed. */
    void handleAsyncUpdate() override
    {
        if (const int program = pendingProgram.exchange (-1, std::memory_order_acquire); program >= 0)
            loadFactory (program);   // a program change from another thread (requestFactory()); it notifies again
        processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withProgramChanged (true).withNonParameterStateChanged (true));
    }

    juce::AudioProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;

    juce::CriticalSection lock;   // never taken by the audio thread
    juce::File userFolder { defaultUserFolder() };
    juce::String currentName, currentCategory;
    int currentProgram = -1;
    juce::File currentFile;
    std::atomic<bool> stateReplaced { false };
    std::atomic<int>  pendingProgram { -1 };   // requestFactory() from a thread other than the message thread

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};
