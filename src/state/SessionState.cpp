// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "state/SessionState.h"
#include "dsp/Chop.h"   // CHOP's pattern: Chop::stepsPerPattern and the default (ChopSettings)
#include <cmath>

namespace
{
    // The destructor waits this long (ms) for a sample load that is running to finish.
    constexpr int loaderStopTimeoutMs = 10000;

    // An embedded sample's base64 text: the decoded size before the dot has at most this many
    // digits (999 999 999 bytes, far above the cap), and the text may run this many characters
    // past 4/3 of the cap (the size prefix and the dot).
    constexpr int maxSizeDigits = 9;
    constexpr size_t sizePrefixAllowance = 64;

    using namespace StateFormat;   // the state's names and version (src/state/StateFormat.h)

    // State tree layout for CHOP's pattern (v1.6.0+): <Chop pattern="0101010101010101"/>,
    // one character per step, step 1 first, "1" = chopped.

    juce::String chopPatternToString (juce::uint16 pattern)
    {
        juce::String text;
        for (int i = 0; i < Chop::stepsPerPattern; ++i)
            text << (((pattern >> i) & 1u) != 0 ? "1" : "0");
        return text;
    }

    juce::uint16 chopPatternFromString (const juce::String& text, juce::uint16 fallback)
    {
        if (text.length() != Chop::stepsPerPattern || ! text.containsOnly ("01"))
            return fallback;
        juce::uint16 pattern = 0;
        for (int i = 0; i < Chop::stepsPerPattern; ++i)
            if (text[i] == '1')
                pattern = (juce::uint16) (pattern | (1u << i));
        return pattern;
    }

    // State tree layout for the sample (v1.5.0+):
    //   <Sample default="1"/>                                  the built-in pad
    //   <Sample name="pad.wav" path="C:/..." hash="..." flac="<base64>"/>
    // flac is the sample itself as 24-bit FLAC, left out above 10 MB (the path is used then).
}

SessionState::SessionState (juce::AudioProcessor& processorToNotify, juce::AudioProcessorValueTreeState& parameterState,
                            StepSequencer& patternSequencer, std::atomic<juce::uint16>& chopPatternBits)
    : apvts (parameterState), sequencer (patternSequencer), chopPattern (chopPatternBits), hostNotifier (processorToNotify)
{
    publishSample (SampleLoader::makeDefaultSample());
}

SessionState::~SessionState()
{
    sampleLoader.removeAllJobs (true, loaderStopTimeoutMs);   // a running load finishes before the slot it publishes into goes away
}

void SessionState::loadDefaultPattern (StepSequencer& target) noexcept
{
    // A C-minor bassline around C3 with octave jumps; notes are kept on the
    // "off" steps too, so switching a step on gives a sensible pitch.
    const StepSequencer::Step pattern[StepSequencer::numSteps] =
    {
        { true, 48, 112 }, { false, 48, 90 }, { true, 60, 80 },  { true, 48, 100 },
        { true, 51, 112 }, { false, 51, 90 }, { true, 55, 90 },  { true, 58, 80 },
        { true, 48, 112 }, { false, 48, 90 }, { true, 60, 80 },  { true, 48, 100 },
        { true, 53, 112 }, { false, 53, 90 }, { true, 55, 90 },  { true, 51, 80 },
    };

    for (int i = 0; i < StepSequencer::numSteps; ++i)
        target.setStep (i, pattern[i]);
}

//==============================================================================
// Sample

void SessionState::publishSample (std::unique_ptr<SampleData> sample, const SampleData::Origin* missing, bool changedSinceSaved)
{
    {
        const juce::ScopedLock sl (sampleInfoLock);
        missingSample      = missing != nullptr ? *missing : SampleData::Origin{};
        missingSampleHash  = {};
        sampleProblem      = missing != nullptr ? "sample missing: " + missing->name
                           : changedSinceSaved  ? juce::String ("sample changed")
                                                : juce::String();
        embeddedSampleText = {};   // belongs to the sample being replaced (restoreSample() sets it again for an embedded one)
    }
    sampleSlot.publish (std::move (sample));
}

void SessionState::loadSampleFileAsync (const juce::File& file)
{
    const int generation = ++sampleLoadGeneration;
    backgroundLoadGeneration = generation;

    sampleLoader.addJob ([this, file, generation]
    {
        auto result = SampleLoader::loadFile (file);

        if (generation != sampleLoadGeneration.load())
            return;   // overtaken by a newer request, which owns the status now

        if (result.sample != nullptr)
            publishSample (std::move (result.sample));
        else
        {
            const juce::ScopedLock sl (sampleInfoLock);
            sampleProblem = file.getFileName() + ": " + result.error;
        }

        int expected = generation;
        backgroundLoadGeneration.compare_exchange_strong (expected, 0);   // done, unless a newer load has started

        // Hosts expect to be told about state changes on the message thread, and this is the
        // loader's thread: post it there. The AsyncUpdater is a member, so a notification still
        // pending when the processor goes is cancelled rather than delivered to a dead object.
        hostNotifier.triggerAsyncUpdate();
    });
}

bool SessionState::loadSampleFile (const juce::File& file, juce::String& error)
{
    ++sampleLoadGeneration;   // supersedes any background load still running
    auto result = SampleLoader::loadFile (file);

    if (result.sample == nullptr)
    {
        error = result.error;
        return false;
    }

    publishSample (std::move (result.sample));
    return true;
}

void SessionState::useDefaultSample()
{
    ++sampleLoadGeneration;
    publishSample (SampleLoader::makeDefaultSample());
}

SessionState::SampleStatus SessionState::getSampleStatus() const
{
    SampleStatus status;
    sampleSlot.withCurrent ([&] (const SampleData* sample)
    {
        if (sample != nullptr)
        {
            status.name    = sample->getOrigin().name;
            status.seconds    = sample->getLengthSeconds();
            status.sampleRate = sample->getSampleRate();
            status.path       = sample->getOrigin().path;
        }
    });

    const juce::ScopedLock sl (sampleInfoLock);
    status.problem = sampleProblem;
    const int loading = backgroundLoadGeneration.load();
    status.loading = loading != 0 && loading == sampleLoadGeneration.load();
    return status;
}

//==============================================================================
// State

void SessionState::save (juce::MemoryBlock& destData)
{
    // Parameters and GUI state come from the APVTS; the pattern is added as a
    // child tree because its 16 steps are not host parameters.
    // (apvts.state holds no Pattern, Chop or Sample child: load() leaves them out.)
    auto state = apvts.copyState();
    state.setProperty (stateVersionId, currentStateVersion, nullptr);

    juce::ValueTree pattern (patternId);
    for (int i = 0; i < StepSequencer::numSteps; ++i)
    {
        const auto s = sequencer.getStep (i);
        juce::ValueTree step (stepId);
        step.setProperty (onId,       s.on,       nullptr);
        step.setProperty (noteId,     s.note,     nullptr);
        step.setProperty (velocityId, s.velocity, nullptr);
        if (s.tie)   step.setProperty (tieId,   true, nullptr);   // so a session without ties or
        if (s.slide) step.setProperty (slideId, true, nullptr);   // slides is saved as before
        pattern.appendChild (step, nullptr);
    }
    state.appendChild (pattern, nullptr);

    juce::ValueTree chop (chopId);
    chop.setProperty (chopPatternId, chopPatternToString (chopPattern.load()), nullptr);
    state.appendChild (chop, nullptr);

    juce::ValueTree sampleTree (sampleId);
    sampleSlot.withCurrent ([&] (const SampleData* sample)
    {
        const juce::ScopedLock sl (sampleInfoLock);

        if (missingSample.path.isNotEmpty())
        {
            // The session's sample couldn't be found: keep pointing at it, so saving doesn't lose it.
            sampleTree.setProperty (sampleNameId, missingSample.name, nullptr);
            sampleTree.setProperty (samplePathId, missingSample.path, nullptr);
            if (missingSampleHash.isNotEmpty())
                sampleTree.setProperty (sampleHashId, missingSampleHash, nullptr);
        }
        else if (sample == nullptr || sample->getOrigin().isDefault)
        {
            sampleTree.setProperty (sampleDefaultId, true, nullptr);
        }
        else
        {
            sampleTree.setProperty (sampleNameId, sample->getOrigin().name, nullptr);
            sampleTree.setProperty (samplePathId, sample->getOrigin().path, nullptr);
            sampleTree.setProperty (sampleHashId, sample->getHash(), nullptr);
            if (sample->getEmbeddedFlac().getSize() > 0)
            {
                // Encoded once per sample; the tree shares the kept text (juce::String is reference-counted).
                if (! embeddedSampleText.isFor (*sample))
                {
                    embeddedSampleText = { sample->getEmbeddedFlac().toBase64Encoding(), sample->getHash(), sample->getSampleRate() };
                    ++numEmbeddedSampleEncodes;
                }
                sampleTree.setProperty (sampleFlacId, embeddedSampleText.text, nullptr);
            }
        }
    });
    state.appendChild (sampleTree, nullptr);

    if (auto xml = state.createXml())
        juce::AudioProcessor::copyXmlToBinary (*xml, destData);
}

namespace
{
    /** How deeply the elements in a state's XML nest, stopping once past `limit`. juce::XmlDocument
        parses nested elements recursively, so a hostile session nested a few hundred thousand deep
        would overflow the stack; ours nest three deep. A rough count is enough: '<' starts an
        element unless '/', '?' or '!' follows, "</" and "/>" end one; attribute values cannot
        contain '<' in well-formed XML, and anything else fails to parse later anyway. */
    int xmlNestingDepth (const void* data, int sizeInBytes, int limit) noexcept
    {
        const auto* text = static_cast<const char*> (data);
        int depth = 0, deepest = 0;
        for (int i = 0; i + 1 < sizeInBytes && deepest <= limit; ++i)
        {
            if (text[i] == '<')
            {
                const char next = text[i + 1];
                if (next == '/')
                    --depth;
                else if (next != '?' && next != '!')
                    deepest = juce::jmax (deepest, ++depth);
            }
            else if (text[i] == '/' && text[i + 1] == '>')
            {
                --depth;
            }
        }
        return deepest;
    }
}

void SessionState::load (const void* data, int sizeInBytes)
{
    // A session is untrusted input: from a host, a project file, a preset someone sent.
    constexpr int maxNesting = 64;
    if (data == nullptr || sizeInBytes <= 0 || xmlNestingDepth (data, sizeInBytes, maxNesting) > maxNesting)
        return;

    auto xml = juce::AudioProcessor::getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    const auto tree = juce::ValueTree::fromXml (*xml);

    // The pattern, CHOP and sample children are read into the sequencer, chopPattern and the
    // sample slot below, and save() writes them from there. apvts.state keeps
    // only what the APVTS and the editor use (the parameters and the editor's view properties):
    // the Sample child can carry about 13 MB of base64 FLAC that would otherwise stay in memory.
    auto parameters = tree.createCopy();   // cheap: juce::String values are shared, not copied
    for (int i = parameters.getNumChildren(); --i >= 0;)
        if (const auto type = parameters.getChild (i).getType(); type == patternId || type == chopId || type == sampleId)
            parameters.removeChild (i, nullptr);

    // A value that is not a finite number ("nan" parses as NaN and the APVTS passes it through
    // unchecked) would reach the DSP as NaN, where it can stall the sequencer, crash the grain
    // reader or poison a voice for good. It falls back to the parameter's default; infinities
    // are clamped by the parameter's range, but they are not real settings either.
    for (int i = 0; i < parameters.getNumChildren(); ++i)
    {
        auto param = parameters.getChild (i);
        if (auto* p = apvts.getParameter (param.getProperty ("id").toString()); p != nullptr && param.hasProperty ("value")
                                                                               && ! std::isfinite ((double) param.getProperty ("value")))
            param.setProperty ("value", p->convertFrom0to1 (p->getDefaultValue()), nullptr);
    }
    apvts.replaceState (parameters);

    // A state without a pattern (sessions saved before v1.3, or a preset that
    // only carries parameters) gets the default pattern, so loading it always
    // sounds the same whatever was loaded before.
    loadDefaultPattern (sequencer);

    const auto pattern = tree.getChildWithName (patternId);
    for (int i = 0; i < juce::jmin (StepSequencer::numSteps, pattern.getNumChildren()); ++i)
    {
        const auto step = pattern.getChild (i);
        const StepSequencer::Step absent;   // a missing note or velocity takes a new step's (60, 100)
        sequencer.setStep (i, { (bool) step.getProperty (onId, false),
                                (int)  step.getProperty (noteId, absent.note),
                                (int)  step.getProperty (velocityId, absent.velocity),
                                (bool) step.getProperty (tieId, false),
                                (bool) step.getProperty (slideId, false) });
    }

    // CHOP's pattern; states from before v1.6.0 have none and get the default.
    chopPattern.store (chopPatternFromString (tree.getChildWithName (chopId).getProperty (chopPatternId).toString(),
                                              ChopSettings().pattern));

    restoreSample (tree.getChildWithName (sampleId));
    loads.fetch_add (1, std::memory_order_release);   // v1.11.0: the editor's pattern undo starts afresh
}

void SessionState::restoreSample (const juce::ValueTree& sampleTree)
{
    ++sampleLoadGeneration;   // a background load started before this state arrived must not win

    // No Sample child (states before v1.5.0) or the default: the built-in pad.
    if (! sampleTree.isValid() || (bool) sampleTree.getProperty (sampleDefaultId, false))
    {
        if (! sampleSlot.withCurrent ([] (const SampleData* current) { return current != nullptr && current->getOrigin().isDefault; }))
        {
            publishSample (SampleLoader::makeDefaultSample());
        }
        else
        {
            const juce::ScopedLock sl (sampleInfoLock);   // already the pad: just forget any missing-sample note
            missingSample = {};
            missingSampleHash = {};
            sampleProblem = {};
        }
        return;
    }

    const SampleData::Origin origin { sampleTree.getProperty (sampleNameId).toString(),
                                      sampleTree.getProperty (samplePathId).toString(), false };
    const auto hash = sampleTree.getProperty (sampleHashId).toString();

    // Already loaded (a host re-sending the same state): nothing to do.
    if (sampleSlot.withCurrent ([&] (const SampleData* current)
            { return current != nullptr && hash.isNotEmpty() && current->getHash() == hash && ! current->getOrigin().isDefault; }))
    {
        const juce::ScopedLock sl (sampleInfoLock);
        missingSample = {};
        sampleProblem = {};
        return;
    }

    // 1. The embedded copy.
    // Its base64 text is at most 4/3 of SampleLoader::maxEmbeddedBytes plus a short size prefix (save() writes
    // no more); a longer one comes from a damaged or hostile session and is not decoded at all.
    // JUCE's encoding starts with the decoded size and a dot ("35242.lwTX..."), and
    // fromBase64Encoding() allocates whatever that number says before checking it against the
    // text, so a negative or huge one would throw bad_alloc or take gigabytes: it must be a plain
    // number, no larger than the cap, and no larger than the text after the dot can hold.
    auto plausibleEncoding = [] (const juce::String& encoded)
    {
        const int dot = encoded.indexOfChar ('.');
        if (dot <= 0 || dot > maxSizeDigits || ! encoded.substring (0, dot).containsOnly ("0123456789"))
            return false;
        const auto size = (size_t) encoded.substring (0, dot).getLargeIntValue();
        const auto payload = (size_t) (encoded.length() - dot - 1);
        return size > 0 && size <= SampleLoader::maxEmbeddedBytes && size <= payload * 3 / 4 + 3;
    };

    if (const auto encoded = sampleTree.getProperty (sampleFlacId).toString();
        encoded.isNotEmpty() && (size_t) encoded.length() <= SampleLoader::maxEmbeddedBytes / 3 * 4 + sizePrefixAllowance
            && plausibleEncoding (encoded))
    {
        juce::MemoryBlock flac;
        if (flac.fromBase64Encoding (encoded))
        {
            if (auto result = SampleLoader::loadEmbeddedFlac (flac, origin); result.sample != nullptr)
            {
                // Keep the session's text for saving, so it isn't encoded again.
                EmbeddedSampleText text { encoded, result.sample->getHash(), result.sample->getSampleRate() };
                publishSample (std::move (result.sample));
                const juce::ScopedLock sl (sampleInfoLock);
                embeddedSampleText = std::move (text);
                return;
            }
        }
    }

    // 2. The file, where it was saved from (only a full path: a relative one means nothing here,
    // and juce::File asserts on it).
    if (origin.path.isNotEmpty() && juce::File::isAbsolutePath (origin.path))
    {
        if (auto result = SampleLoader::loadFile (juce::File (origin.path)); result.sample != nullptr)
        {
            // A different file may be there now (edited, or replaced). Load it anyway, since the
            // user may have changed it on purpose, but say so rather than change the sound silently.
            const bool changed = hash.isNotEmpty() && result.sample->getHash() != hash;
            publishSample (std::move (result.sample), nullptr, changed);
            return;
        }
    }

    // 3. Neither: play the built-in pad, but remember the sample so saving keeps the reference.
    publishSample (SampleLoader::makeDefaultSample(), &origin);
    const juce::ScopedLock sl (sampleInfoLock);
    missingSampleHash = hash;
}
