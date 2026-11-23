// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include <array>
#include "plugin/NeonSynthVoice.h"
#include "plugin/VoiceAllocator.h"
#include "plugin/MidiRouter.h"
#include "plugin/MidiLogger.h"
#include "plugin/Parameters.h"
#include "sequencer/StepSequencer.h"
#include "state/SessionState.h"
#include "state/PresetManager.h"

/*
    The plugin's processor. v1.11.1 split it by job; it keeps processBlock, the voice views, the
    scope FIFO and the glue between the parts:
      plugin/Parameters       the parameter layout and the typed pointers into the APVTS
      plugin/MidiRouter       the MIDI the synth plays: keyboard, sequencer modes, HOLD, ENTER, the clocks
      plugin/VoiceAllocator   LimitedSynthesiser, the Voices limit and voice stealing
      state/SessionState      saving and restoring the session, and the sample it carries
*/

class NeonSynthProcessor : public juce::AudioProcessor
{
public:
    NeonSynthProcessor();
    ~NeonSynthProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    using AudioProcessor::processBlock;   // keep the double-precision overload visible
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    void releaseResources() override;
    void reset() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    juce::AudioProcessorEditor* createEditor() override;   // in gui/NeonSynthEditor.cpp: the processor does not include the editor
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "NeonSynth"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    /** Release time plus the 5 ms hard-stop fade (the envelope's release is linear and exact). */
    double getTailLengthSeconds() const override { return (double) parameters.release->get() + NeonSynthVoice::hardStopFadeSeconds; }

    // v1.11.0, region C (presets): the 16 factory presets are the host's program list. The current
    // program is the factory preset loaded last, remembered with the session (PresetManager.h); 0
    // when there is none, as JUCE needs an index.
    int getNumPrograms() override                         { return PresetManager::numFactoryPresets; }
    int getCurrentProgram() override                      { return juce::jmax (0, presetManager.getCurrentProgramIndex()); }
    void setCurrentProgram (int index) override           { presetManager.requestFactory (index); }
    const juce::String getProgramName (int index) override { return PresetManager::getFactoryName (index); }
    void changeProgramName (int, const juce::String&) override {}   // the factory names are fixed

    /** Counts the sessions and presets loaded (v1.11.0): the pattern row clears its undo
        history when it changes, so UNDO never writes an old sound's pattern onto a new one. */
    int getStateLoadCount() const noexcept                { return session.getLoadCount(); }

    /** Presets: the factory bank, the user folder, loading and saving (message thread). */
    PresetManager& getPresetManager() noexcept            { return presetManager; }

    void getStateInformation (juce::MemoryBlock& destData) override        { session.save (destData); }
    void setStateInformation (const void* data, int sizeInBytes) override  { session.load (data, sizeInBytes); }

    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }

    static constexpr int numVoices = 16;

    //==========================================================================
    // Output soft clipper (applied after the output gain). Below the knee the
    // signal passes through untouched; above it, it bends smoothly towards the
    // ceiling and can never exceed it. Stateless, so no latency and no pumping.
    //
    // The bend is a tanh-like rational curve, u (27 + u^2) / (27 + 9 u^2), which
    // has slope 1 at the knee, rises monotonically and meets the ceiling with
    // zero slope at u = 3 (an input of about +4.5 dBFS), staying there above.
    // It costs a few multiplies and one divide: about 4 ns per clipped sample
    // with MSVC, against about 16 ns for std::tanh.
    static constexpr float softClipKnee    = 0.50118723f;   // -6 dBFS
    static constexpr float softClipCeiling = 0.89125094f;   // -1 dBFS

    static float softClip (float x) noexcept
    {
        const float magnitude = std::abs (x);
        if (magnitude <= softClipKnee)
            return x;

        constexpr float range = softClipCeiling - softClipKnee;
        const float u    = juce::jmin ((magnitude - softClipKnee) / range, 3.0f);
        const float bend = u * (27.0f + u * u) / (27.0f + 9.0f * u * u);   // 0..1
        return x < 0.0f ? -(softClipKnee + range * bend) : softClipKnee + range * bend;
    }

    //==========================================================================
    // Per-voice state for the visualisers. Written once per block on the audio
    // thread (relaxed atomic stores), read by the editor's timer. Fields may be
    // from neighbouring blocks, which is fine for display.
    struct VoiceView
    {
        std::atomic<bool>  active { false };
        std::atomic<float> level { 0.0f };
        std::atomic<float> frequencyHz { 0.0f };
        std::atomic<float> secondsSinceOn { 0.0f };
        std::atomic<float> secondsSinceOff { -1.0f };
        std::atomic<float> samplePosition { -1.0f };   // 0..1 while playing the sample, else -1
        std::array<std::atomic<float>, NeonSynthVoice::Telemetry::maxShownGrains> grainPositions;   // 0..1 per sounding grain, else -1
        std::atomic<bool>  playingRandom { false };
        std::array<std::atomic<float>, RandomStepOscillator::numSteps> randomLevels;   // valid while playingRandom

        VoiceView()
        {
            for (auto& g : grainPositions) g.store (-1.0f);
            for (auto& l : randomLevels)   l.store (0.0f);
        }
    };

    const std::array<VoiceView, numVoices>& getVoiceViews() const noexcept { return voiceViews; }

    //==========================================================================
    // Step sequencer (see StepSequencer.h for the engine and its timing model)

    /** How the keyboard and the sequencer share the synth (the Sequencer Mode parameter). */
    using SequencerMode = MidiRouter::SequencerMode;

    /** Reads a step of the pattern. Any thread. */
    StepSequencer::Step getSequencerStep (int index) const noexcept   { return sequencer.getStep (index); }

    /**
        Edits a step of the pattern. Message thread: besides the lock-free
        store, it tells the host the plugin state changed so the project is
        marked as modified.
    */
    void setSequencerStep (int index, const StepSequencer::Step& step);

    /** v1.11.0: writes every step that differs from `pattern`, then tells the host once (the
        pattern tools, undo and ENTER). Message thread. */
    void setSequencerPattern (const StepSequencer::Pattern& pattern);

    /**
        v1.11.0, ENTER: while armed, every keyboard note-on is also queued for the editor, which
        writes it into the step being entered. Message thread arms and reads; the audio thread
        only pushes (lock-free, dropped when the queue is full).
    */
    void setNoteEntryArmed (bool armed) noexcept     { router.setNoteEntryArmed (armed); }
    bool popEnteredNote (int& note, int& velocity) noexcept   { return router.popEnteredNote (note, velocity); }

    /** Step that most recently started, or -1 while the sequencer is stopped. Any thread. */
    int getSequencerPlayhead() const noexcept   { return sequencer.getCurrentStep(); }

    /** The modulation's state for the MODULATION panel (v1.9.0): LFO phase and value, bend and wheel positions. Any thread. */
    const Modulation& getModulation() const noexcept   { return modulation; }

    /** Tempo the sequencer followed in the last block (host or internal), for display. */
    float getSequencerTempo() const noexcept    { return router.getSequencerTempo(); }

    /** True when the last block was clocked by the host transport rather than the internal tempo. */
    bool isSequencerFollowingHost() const noexcept   { return router.isSequencerFollowingHost(); }

    //==========================================================================
    // Sample (played by the Sample and Granular sources, v1.5.0). One sample
    // per instance; a built-in pad until a file is loaded. See SampleData.h
    // for how a file is prepared and SampleSlot.h for the lock-free hand-off.

    /** What the editor shows about the sample. */
    using SampleStatus = SessionState::SampleStatus;

    /**
        Message thread. Starts loading a WAV, AIFF or FLAC file in the
        background; the sample switches once it's ready. A newer request
        supersedes an older one that hasn't finished.
    */
    void loadSampleFileAsync (const juce::File& file)   { session.loadSampleFileAsync (file); }

    /** Any thread except the audio thread. Loads now; on failure the current sample stays and `error` says why. */
    bool loadSampleFile (const juce::File& file, juce::String& error)   { return session.loadSampleFile (file, error); }

    /** Any thread except the audio thread. Goes back to the built-in pad. */
    void useDefaultSample()                       { session.useDefaultSample(); }

    /** Message thread (or any thread except the audio thread). */
    SampleStatus getSampleStatus() const          { return session.getSampleStatus(); }

    /**
        Message thread. Frees samples that were replaced while notes still played them, once
        those notes have ended. A load and stopping the audio do this too; the editor's timer
        calls it so that memory comes back without waiting for either.
    */
    void freeReplacedSamples()                    { session.getSampleSlot().collectGarbage(); }

    /** Replaced samples still waiting to be freed (for tests). */
    int getNumRetiredSamples() const              { return session.getSampleSlot().getNumRetired(); }

    /** Calls `fn (const SampleData*)` with the current sample, safely against a concurrent load. Not for the audio thread. */
    template <typename Fn>
    auto withCurrentSample (Fn&& fn) const   { return session.getSampleSlot().withCurrent (std::forward<Fn> (fn)); }

    /** How many times getStateInformation() has had to base64-encode an embedded sample (for tests). */
    int getNumEmbeddedSampleEncodes() const noexcept   { return session.getNumEmbeddedSampleEncodes(); }

    //==========================================================================
    // CHOP's pattern (v1.6.0): 16 steps, saved with the session, not host parameters.

    /** Any thread except the audio thread. Sounding voices pick it up at their next cycle. */
    void setChopStep (int step, bool chopped) noexcept;

    /** Bit k set = step k (0..15) chopped. */
    juce::uint16 getChopPattern() const noexcept   { return chopPattern.load(); }

    //==========================================================================
    // Scope data: audio thread writes, editor timer (message thread) reads.
    static constexpr int scopeFifoSize = 1 << 15;   // ~0.7 s at 48 kHz

    /** Message thread only. Copies up to maxFrames stereo frames; returns how many. */
    int drainScope (float* destLeft, float* destRight, int maxFrames) noexcept;

private:
    void pushScope (const float* left, const float* right, int numFrames) noexcept;

    // Stereo frames, interleaved L/R: index i of the FIFO lives at 2i and 2i + 1.
    juce::AbstractFifo scopeFifo { scopeFifoSize };
    std::array<float, 2 * (std::size_t) scopeFifoSize> scopeRing {};

    MidiLogger  midiLogger;
    juce::int64 samplesProcessed = 0;

    LimitedSynthesiser synth;
    std::vector<NeonSynthVoice*> voices;    // owned by synth; cached to avoid dynamic_cast per block
    std::array<VoiceView, numVoices> voiceViews;

    void publishVoiceViews() noexcept;

    StepSequencer sequencer;
    MidiRouter    router { sequencer, synth };

    /** The sequencer's and HOLD's parameters for the router, read once per block. Audio thread. */
    MidiRouter::Controls routerControls() const noexcept;

    /** CHOP's pattern, bit k = step k chopped. Written by the editor, read once per block; lock-free. */
    std::atomic<juce::uint16> chopPattern { ChopSettings().pattern };

    juce::AudioProcessorValueTreeState apvts;
    Parameters    parameters { apvts };            // after apvts, whose parameters it looks up
    PresetManager presetManager { *this, apvts };   // v1.11.0, region C; after apvts, whose state it listens to

    /** The LFO, pitch bend and mod wheel (v1.9.0), shared by all voices. */
    Modulation modulation;

    /** The musical time the LFO follows this block (host song position, or free-running). Audio thread. */
    Lfo::Transport lfoTransport() const;

    juce::SmoothedValue<float> outputGain { 1.0f };
    static constexpr double outputGainGlideSeconds = 0.02;   // GAIN glides over 20 ms, so automating it doesn't zipper

    // The session and its sample; after apvts, the sequencer and chopPattern, which it saves and restores.
    SessionState session { *this, apvts, sequencer, chopPattern };
    const SampleData* blockSample = nullptr;   // audio thread: the sample for the current block

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeonSynthProcessor)
};
