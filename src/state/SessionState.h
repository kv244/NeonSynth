// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include "sequencer/StepSequencer.h"
#include "state/SampleSlot.h"
#include "state/SampleLoader.h"
#include "state/StateFormat.h"
#include "state/FourierState.h"

/**
    The saved session (v1.11.1: moved out of NeonSynthProcessor): writing and reading the
    state the host stores (the layout is in StateFormat.h), and the sample, which a session
    carries in its Sample child and which can also be loaded from a file or reset to the
    built-in pad at any time.

    The processor owns one and its getStateInformation() and setStateInformation() call save()
    and load(). The parameters stay in the processor's APVTS, the pattern in its sequencer and
    CHOP's pattern in its atomic: this reads and writes them there. It starts with the built-in
    pad.

    Threads: save() and load() run wherever the host calls the state functions (the message
    thread, or a host's own thread), the sample functions on any thread except the audio
    thread. The audio thread only touches getSampleSlot().beginBlock().
*/
class SessionState
{
public:
    /** What the editor shows about the sample. */
    struct SampleStatus
    {
        juce::String name;       ///< file name, or "Default pad"
        juce::String problem;    ///< why the last load failed, "sample missing: <name>" or "sample changed"; empty when fine
        bool loading = false;    ///< a background load is in progress
        double seconds = 0.0;
        double sampleRate = 0.0; ///< the file's own sample rate (playback converts to the host's)
        juce::String path;       ///< full path of the file; empty for the built-in pad
    };

    /** The processor's parameters, pattern and CHOP pattern, which a session saves and restores;
        the processor is also told when a background load changes the sample. All four must
        outlive this. */
    SessionState (juce::AudioProcessor& processorToNotify, juce::AudioProcessorValueTreeState& parameterState,
                  StepSequencer& patternSequencer, std::atomic<juce::uint16>& chopPatternBits,
                  FourierState& fourierState);
    ~SessionState();

    /** The processor's getStateInformation(). */
    void save (juce::MemoryBlock& destData);

    /** The processor's setStateInformation(). A session is untrusted input: anything that
        cannot be read is ignored and leaves the current state as it was. */
    void load (const void* data, int sizeInBytes);

    /** Counts the sessions and presets loaded (v1.11.0): the pattern row clears its undo
        history when it changes, so UNDO never writes an old sound's pattern onto a new one. */
    int getLoadCount() const noexcept                { return loads.load (std::memory_order_acquire); }

    /** Fills the pattern with the default bassline (used until a session supplies one). */
    static void loadDefaultPattern (StepSequencer& target) noexcept;

    //==========================================================================
    // Sample (played by the Sample and Granular sources, v1.5.0). One sample
    // per instance; a built-in pad until a file is loaded. See SampleData.h
    // for how a file is prepared and SampleSlot.h for the lock-free hand-off.

    /** The current sample, for the audio thread (beginBlock) and the processor's sample functions. */
    SampleSlot& getSampleSlot() noexcept                 { return sampleSlot; }
    const SampleSlot& getSampleSlot() const noexcept     { return sampleSlot; }

    /**
        Message thread. Starts loading a WAV, AIFF or FLAC file in the
        background; the sample switches once it's ready. A newer request
        supersedes an older one that hasn't finished.
    */
    void loadSampleFileAsync (const juce::File& file);

    /** Any thread except the audio thread. Loads now; on failure the current sample stays and `error` says why. */
    bool loadSampleFile (const juce::File& file, juce::String& error);

    /** Any thread except the audio thread. Goes back to the built-in pad. */
    void useDefaultSample();

    /** Message thread (or any thread except the audio thread). */
    SampleStatus getSampleStatus() const;

    /** How many times save() has had to base64-encode an embedded sample (for tests). */
    int getNumEmbeddedSampleEncodes() const noexcept   { return numEmbeddedSampleEncodes.load(); }

private:
    juce::AudioProcessorValueTreeState& apvts;
    StepSequencer& sequencer;
    std::atomic<juce::uint16>& chopPattern;
    FourierState& fourier;   // v1.12.0: CUSTOM's harmonics and the drawn cycle

    std::atomic<int> loads { 0 };   // see getLoadCount()

    // Sample. sampleSlot must outlive sampleLoader (whose jobs publish into it), so it is declared first.
    SampleSlot sampleSlot;
    void publishSample (std::unique_ptr<SampleData> sample, const SampleData::Origin* missing = nullptr, bool changedSinceSaved = false);
    void restoreSample (const juce::ValueTree& sampleTree);   // load()

    mutable juce::CriticalSection sampleInfoLock;   // guards the fields below; never taken by the audio thread
    SampleData::Origin missingSample;          // a session's sample that couldn't be found (kept so saving doesn't lose it)
    juce::String missingSampleHash;
    juce::String sampleProblem;

    // The current sample's embedded FLAC as base64 text, exactly as it goes into the saved state
    // (up to about 13 MB). Kept from the session it was loaded from, or from the first save, so
    // later saves don't encode it again. It belongs to the sample with this hash and sample rate.
    struct EmbeddedSampleText
    {
        juce::String text, hash;
        double sampleRate = 0.0;

        bool isFor (const SampleData& sample) const noexcept
        {
            // An identity check, not arithmetic: both rates come from the same file header.
            return text.isNotEmpty() && hash == sample.getHash() && juce::exactlyEqual (sampleRate, sample.getSampleRate());
        }
    };
    EmbeddedSampleText embeddedSampleText;
    std::atomic<int> numEmbeddedSampleEncodes { 0 };

    // Every request to change the sample (a load, PAD, a session) takes the next generation,
    // and only the newest may publish. backgroundLoadGeneration is the generation of the
    // background load still running, or 0: the status shows "loading..." only while that load
    // is also the newest request, so one that was overtaken can't leave the status stuck.
    std::atomic<int> sampleLoadGeneration { 0 };
    std::atomic<int> backgroundLoadGeneration { 0 };
    /** Tells the host the session changed (a sample loaded in the background), on the message
        thread: the loader's thread posts it here instead of calling updateHostDisplay itself. */
    struct HostNotifier : juce::AsyncUpdater
    {
        explicit HostNotifier (juce::AudioProcessor& p) : processor (p) {}
        ~HostNotifier() override { cancelPendingUpdate(); }
        void handleAsyncUpdate() override
        {
            processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
        }
        juce::AudioProcessor& processor;
    };
    HostNotifier hostNotifier;   // before sampleLoader, so it outlives the jobs that trigger it

    juce::ThreadPool  sampleLoader { juce::ThreadPoolOptions{}.withThreadName ("NeonSynth sample loader").withNumberOfThreads (1) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionState)
};
