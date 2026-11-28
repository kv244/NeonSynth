// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include <optional>
#include "state/FactoryPresets.h"
#include "state/FourierState.h"
#include "dsp/Chop.h"   // ChopSettings::Depths (v1.13.0)
#include "state/StateFormat.h"

/**
    Presets (v1.11.0).

    A .neonpreset file is the session's own XML, exactly what getStateInformation() writes (every
    parameter, the pattern, CHOP, the sample by reference or embedded), with two attributes on the
    root: "category" (Bass, Lead, Pad, Keys, Sequence, Percussion, FX; absent = none) and "preset"
    (its name). Loading one goes through setStateInformation(), so a preset file gets every check
    a session gets: it is untrusted input, a file someone may have sent.

    The bank is the 24 factory presets (FactoryPresets.h, compiled in; 16 until v1.12.0) followed by the user's
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
    static constexpr int numFactoryPresets = 26;   // 16 in v1.11; v1.12.0 appended 8, v1.13.0 more

    /** Largest preset file read: a session with the largest embedded sample is about 14 MB. */
    static constexpr juce::int64 maxFileBytes = juce::int64 { 32 } * 1024 * 1024;

    /** A preset's name is cut to this many characters; its file name (made from it) to fewer. */
    static constexpr int maxNameLength = 128;
    static constexpr int maxFileNameLength = 64;

    static inline const juce::Identifier categoryId { "category" };
    static inline const juce::Identifier presetId   { "preset" };
    static inline const juce::Identifier programId  { "program" };

    /** The categories, in the order the menu shows them. "" (none) is not in the list. */
    static const juce::StringArray& categories();

    /** One of categories(), or "" for anything else (an untrusted file's attribute). */
    static juce::String validCategory (const juce::String& text);

    /** Documents/NeonSynth/Presets: where SAVE writes and the menu looks. */
    static juce::File defaultUserFolder();

    PresetManager (juce::AudioProcessor& p, juce::AudioProcessorValueTreeState& state);

    ~PresetManager() override;

    /** Tests point this elsewhere, so they never touch the real Documents folder. */
    void setUserFolder (const juce::File& folder)   { const juce::ScopedLock sl (lock); userFolder = folder; }
    juce::File getUserFolder() const                { const juce::ScopedLock sl (lock); return userFolder; }

    //==========================================================================
    // Factory bank

    static juce::String getFactoryName (int index);

    static juce::String getFactoryCategory (int index);

    static juce::String getFactoryDescription (int index);

    /**
        The factory preset as a complete .neonpreset document: every parameter (the listed ones
        at their values, the rest at their defaults), the pattern (or none, which loads the
        default), CHOP and the built-in pad.
    */
    std::unique_ptr<juce::XmlElement> createFactoryXml (int index) const;

    /** One step as the factory patterns write it (see FactoryPresets.h). */
    struct PatternStep
    {
        bool on = false;
        int note = 60, velocity = 100;
        bool tie = false, slide = false;
        bool valid = false;
    };

    /** Parses one step: "-D#3:90~". C4 = 60. */
    static PatternStep parseStep (juce::String token);

    /**
        A factory preset's harmonics (v1.12.0): "k:level" or "k:level@phase", separated by spaces,
        k 1..32, level 0..1, phase in radians 0..2 pi. Unlisted harmonics are 0. Empty if any
        token is malformed or out of range, or a harmonic is listed twice.
    */
    static std::optional<HarmonicSpectrum> parseHarmonics (const char* text);

    static std::vector<PatternStep> parsePattern (const char* text);

    /** A factory preset's CHOP depths (v1.13.0): 16 numbers 0..15 separated by spaces; empty otherwise. */
    static std::optional<ChopSettings::Depths> parseChopDepths (const char* text);

    //==========================================================================
    // Loading and saving

    /**
        The host's program change. Hosts are meant to call it on the message thread, but a
        wrapper or host may not; loading builds XML and replaces the state, which must never run
        on the audio thread, so from any other thread the load is passed to the message thread.
    */
    void requestFactory (int index);

    /** Message thread. False if the index is out of range. */
    bool loadFactory (int index);

    /** Loads a .neonpreset file. On failure the current sound stays and `error` says why. */
    bool loadFile (const juce::File& file, juce::String& error);

    /**
        Saves the current session as a .neonpreset file, named after the file, with a category
        ("" = none). The file is then the current preset. False, with `error`, if it can't be written.
    */
    bool saveFile (const juce::File& file, const juce::String& category, juce::String& error);

    /** The file SAVE writes for a name: in the user folder, the name made safe for a file name. */
    juce::File userFileFor (const juce::String& name) const;

    /** The user folder's presets, sorted by name (case-insensitive). */
    juce::Array<juce::File> getUserPresets() const;

    /** Factory presets first, then the user's; delta +1 = next, -1 = previous. Wraps around. */
    bool step (int delta);

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
    static int xmlBinaryMagic();

    /** The state's text, wrapped the way copyXmlToBinary() wraps XML, through setStateInformation(). */
    bool loadText (const void* text, size_t numBytes, const juce::String& name, int program, const juce::File& file);

    /**
        A file cut short (a full disk, a copy that stopped) would often still parse: JUCE's XML
        parser accepts an element that never closes, and the state would then load with its
        parameters missing, which keep whatever values they had. A complete preset ends with the
        root's closing tag (a preset always has children, so never with a self-closing root), so
        anything else is refused before it is parsed at all. Not "/>": every PARAM line ends that
        way, so a file cut after any of them would pass.
    */
    bool endsLikeAPreset (const juce::MemoryBlock& data) const;

    static bool isViewProperty (const juce::Identifier& id);

    void setCurrent (const juce::String& name, const juce::String& category, int program, const juce::File& file);

    /** The current preset into the state (so the session remembers it). Message thread. */
    void writeMetaToState();

    /** apvts.replaceState(): a preset or a session was loaded (on setStateInformation's thread). */
    void valueTreeRedirected (juce::ValueTree& tree) override;

    /** Tells the host, on the message thread, that the program and the state changed. */
    void handleAsyncUpdate() override;

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
