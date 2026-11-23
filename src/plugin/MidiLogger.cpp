// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "plugin/MidiLogger.h"
#include <atomic>

juce::String MidiLogger::findMidiLogPath (juce::AudioProcessor::WrapperType wrapper)
{
    auto path = juce::SystemStats::getEnvironmentVariable ("NEONSYNTH_MIDILOG", {});
    if (path.isEmpty())
        path = juce::SystemStats::getEnvironmentVariable ("BASICSYNTH_MIDILOG", {});

    if (path.isEmpty() && wrapper == juce::AudioProcessor::wrapperType_Standalone)
    {
        const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
        const auto index = args.indexOf ("--midilog");

        if (index >= 0 && index + 1 < args.size())
            path = args[index + 1].unquoted();
    }

    return path;
}

juce::File MidiLogger::midiLogFileForNewInstance (const juce::String& path)
{
    // One count for the whole process: every instance that logs takes the next number.
    static std::atomic<int> loggingInstances { 0 };
    const int instance = ++loggingInstances;

    auto file = juce::File::getCurrentWorkingDirectory().getChildFile (path);
    if (instance > 1)
        file = file.getSiblingFile (file.getFileNameWithoutExtension() + "-" + juce::String (instance)
                                    + file.getFileExtension());
    return file;
}
