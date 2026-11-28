// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "state/PresetManager.h"

const juce::StringArray& PresetManager::categories()
{
    static const juce::StringArray names { "Bass", "Lead", "Pad", "Keys", "Sequence", "Percussion", "FX" };
    return names;
}

juce::String PresetManager::validCategory (const juce::String& text)
{
    const int index = categories().indexOf (text.trim(), true);
    return index >= 0 ? categories()[index] : juce::String();
}

juce::File PresetManager::defaultUserFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("NeonSynth").getChildFile ("Presets");
}

PresetManager::PresetManager (juce::AudioProcessor& p, juce::AudioProcessorValueTreeState& state)
    : processor (p), apvts (state)
{
    jassert (FactoryPresets::all().size() == (size_t) numFactoryPresets);
    apvts.state.addListener (this);
}

PresetManager::~PresetManager()
{
    apvts.state.removeListener (this);
    cancelPendingUpdate();
}

juce::String PresetManager::getFactoryName (int index)
{
    return juce::isPositiveAndBelow (index, numFactoryPresets) ? juce::String (FactoryPresets::all()[(size_t) index].name) : juce::String();
}

juce::String PresetManager::getFactoryCategory (int index)
{
    return juce::isPositiveAndBelow (index, numFactoryPresets) ? juce::String (FactoryPresets::all()[(size_t) index].category) : juce::String();
}

juce::String PresetManager::getFactoryDescription (int index)
{
    return juce::isPositiveAndBelow (index, numFactoryPresets) ? juce::String (FactoryPresets::all()[(size_t) index].description) : juce::String();
}

std::unique_ptr<juce::XmlElement> PresetManager::createFactoryXml (int index) const
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
    if (preset.chopDepths != nullptr)   // v1.13.0: depths, with the nearest on/off pattern beside them
    {
        const auto depths = parseChopDepths (preset.chopDepths);
        jassert (depths.has_value());   // PresetTests checks every factory depth list parses
        if (depths.has_value())
        {
            auto* chop = xml->createNewChildElement (StateFormat::chopId);
            juce::String pattern;
            for (int k = 0; k < Chop::stepsPerPattern; ++k)
                pattern << (ChopSettings::depthOf (*depths, k) >= 8 ? "1" : "0");
            chop->setAttribute (StateFormat::chopPatternId, pattern);
            chop->setAttribute (StateFormat::chopDepthsId, juce::String (preset.chopDepths).trim());
        }
    }
    else if (preset.chop != nullptr)
        xml->createNewChildElement (StateFormat::chopId)->setAttribute (StateFormat::chopPatternId, preset.chop);
    if (preset.harmonics != nullptr)   // v1.12.0: CUSTOM's harmonics, as a session saves them
    {
        const auto spectrum = parseHarmonics (preset.harmonics);
        jassert (spectrum.has_value());   // PresetTests checks every factory harmonics string parses
        if (spectrum.has_value())
            xml->addChildElement (FourierState::harmonicsTree (*spectrum).createXml().release());
    }
    xml->createNewChildElement (StateFormat::sampleId)->setAttribute (StateFormat::sampleDefaultId, 1);
    return xml;
}

PresetManager::PatternStep PresetManager::parseStep (juce::String token)
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

std::optional<HarmonicSpectrum> PresetManager::parseHarmonics (const char* text)
{
    HarmonicSpectrum s;   // all levels and phases 0
    std::array<bool, HarmonicSpectrum::numHarmonics> seen {};
    int count = 0;
    for (const auto& token : juce::StringArray::fromTokens (text, " ", ""))
    {
        if (token.isEmpty())
            continue;
        const auto number = token.upToFirstOccurrenceOf (":", false, false);
        auto rest = token.fromFirstOccurrenceOf (":", false, false);
        const auto levelText = rest.upToFirstOccurrenceOf ("@", false, false);
        const auto phaseText = rest.containsChar ('@') ? rest.fromFirstOccurrenceOf ("@", false, false) : juce::String ("0");
        if (! token.containsChar (':') || number.isEmpty() || ! number.containsOnly ("0123456789")
            || levelText.isEmpty() || ! levelText.containsOnly ("0123456789.")
            || phaseText.isEmpty() || ! phaseText.containsOnly ("0123456789."))
            return std::nullopt;
        const int k = number.getIntValue();
        const float level = levelText.getFloatValue(), phase = phaseText.getFloatValue();
        if (k < 1 || k > HarmonicSpectrum::numHarmonics || seen[(size_t) (k - 1)]
            || level < 0.0f || level > 1.0f || phase < 0.0f || phase >= juce::MathConstants<float>::twoPi)
            return std::nullopt;
        seen[(size_t) (k - 1)] = true;
        s.levels[(size_t) (k - 1)] = level;
        s.phases[(size_t) (k - 1)] = phase;
        ++count;
    }
    if (count == 0)
        return std::nullopt;
    return s;
}

std::optional<ChopSettings::Depths> PresetManager::parseChopDepths (const char* text)
{
    juce::StringArray parts;
    parts.addTokens (text, " ", "");
    parts.removeEmptyStrings();
    if (parts.size() != Chop::stepsPerPattern)
        return std::nullopt;
    ChopSettings::Depths depths = 0;
    for (int k = 0; k < Chop::stepsPerPattern; ++k)
    {
        if (! parts[k].containsOnly ("0123456789") || parts[k].length() > 2 || parts[k].getIntValue() > ChopSettings::maxDepth)
            return std::nullopt;
        depths = ChopSettings::withDepth (depths, k, parts[k].getIntValue());
    }
    return depths;
}

std::vector<PresetManager::PatternStep> PresetManager::parsePattern (const char* text)
{
    std::vector<PatternStep> steps;
    for (const auto& token : juce::StringArray::fromTokens (text, " ", ""))
        if (token.isNotEmpty())
            steps.push_back (parseStep (token));
    return steps;
}

void PresetManager::requestFactory (int index)
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

bool PresetManager::loadFactory (int index)
{
    const auto xml = createFactoryXml (index);
    if (xml == nullptr)
        return false;
    const auto text = xml->toString (juce::XmlElement::TextFormat().singleLine());
    return loadText (text.toRawUTF8(), text.getNumBytesAsUTF8(), getFactoryName (index), index, {});
}

bool PresetManager::loadFile (const juce::File& file, juce::String& error)
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

bool PresetManager::saveFile (const juce::File& file, const juce::String& category, juce::String& error)
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

juce::File PresetManager::userFileFor (const juce::String& name) const
{
    auto safe = juce::File::createLegalFileName (name.trim()).substring (0, maxFileNameLength).trim();
    if (safe.isEmpty())
        safe = "Untitled";
    return getUserFolder().getChildFile (safe + fileExtension);
}

juce::Array<juce::File> PresetManager::getUserPresets() const
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

bool PresetManager::step (int delta)
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

int PresetManager::xmlBinaryMagic()
{
    static const int magic = []
    {
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (juce::XmlElement ("x"), block);
        return juce::MemoryInputStream (block, false).readInt();
    }();
    return magic;
}

bool PresetManager::loadText (const void* text, size_t numBytes, const juce::String& name, int program, const juce::File& file)
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

bool PresetManager::endsLikeAPreset (const juce::MemoryBlock& data) const
{
    const auto* text = static_cast<const char*> (data.getData());
    auto end = data.getSize();
    while (end > 0 && (text[end - 1] == 0 || juce::CharacterFunctions::isWhitespace (text[end - 1])))
        --end;
    const auto closing = "</" + apvts.state.getType().toString() + ">";
    const auto tail = juce::String::fromUTF8 (text + end - juce::jmin (end, (size_t) 64), (int) juce::jmin (end, (size_t) 64));
    return tail.endsWith (closing);
}

bool PresetManager::isViewProperty (const juce::Identifier& id)
{
    static const juce::Identifier stateVersionId { "stateVersion" };
    return id != categoryId && id != presetId && id != programId && id != stateVersionId;
}

void PresetManager::setCurrent (const juce::String& name, const juce::String& category, int program, const juce::File& file)
{
    {
        const juce::ScopedLock sl (lock);
        currentName = name.substring (0, maxNameLength);
        currentCategory = validCategory (category);
        currentProgram = program;
        currentFile = file;
    }
    sendChangeMessage();
}

void PresetManager::writeMetaToState()
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

void PresetManager::valueTreeRedirected (juce::ValueTree& tree)
{
    stateReplaced = true;

    // Untrusted: the name is shortened, the category must be one of ours, the program a factory index.
    const auto program = (int) tree.getProperty (programId, -1);
    setCurrent (tree.getProperty (presetId).toString(), tree.getProperty (categoryId).toString(),
                juce::isPositiveAndBelow (program, numFactoryPresets) ? program : -1, {});
}

void PresetManager::handleAsyncUpdate()
{
    if (const int program = pendingProgram.exchange (-1, std::memory_order_acquire); program >= 0)
        loadFactory (program);   // a program change from another thread (requestFactory()); it notifies again
    processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withProgramChanged (true).withNonParameterStateChanged (true));
}
