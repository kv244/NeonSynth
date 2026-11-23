// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>

/*
    The saved state's names and version, in one place (v1.11.0): the processor writes and
    reads sessions with them, and the preset manager builds the factory presets with them, so
    a factory preset can never drift from what the processor reads.

    Layout:
        <Parameters stateVersion="4" ...view properties, preset, category, program...>
            <PARAM id="..." value="..."/> ...                     (the APVTS)
            <Pattern><Step on="1" note="48" velocity="110" [tie="1"] [slide="1"]/> x16</Pattern>
            <Chop pattern="0101010101010101"/>                    (v1.6.0+, step 1 first, "1" = chopped)
            <Sample default="1"/> or <Sample name path hash [flac]/>   (v1.5.0+)
        </Parameters>
*/
namespace StateFormat
{
    /**
        Saved-state format version, written as an attribute of the state tree.
          (none) v1.0 - v1.3: parameters, the editor's view and, from v1.3, the pattern
          2      v1.4: same content; the attribute exists so later format changes
                 can tell old sessions apart and migrate them
          3      v1.5: adds the Sample child (the sample, embedded or by path);
                 older states load with the built-in pad
          4      v1.6+: adds the Chop child (CHOP's 16-step pattern); older
                 states load with the default pattern (every second step).
                 v1.11.0 adds optional attributes only (tie, slide, preset, category,
                 program), whose absence means what older releases did, so it stays 4.
    */
    inline const juce::Identifier stateVersionId { "stateVersion" };
    constexpr int currentStateVersion = 4;

    inline const juce::Identifier paramId    { "PARAM" };   // the APVTS's own names
    inline const juce::Identifier paramIdId  { "id" };
    inline const juce::Identifier valueId    { "value" };

    inline const juce::Identifier patternId  { "Pattern" };
    inline const juce::Identifier stepId     { "Step" };
    inline const juce::Identifier onId       { "on" };
    inline const juce::Identifier noteId     { "note" };
    inline const juce::Identifier velocityId { "velocity" };
    inline const juce::Identifier tieId      { "tie" };     // v1.11.0; written only when set, absent = off
    inline const juce::Identifier slideId    { "slide" };   // v1.11.0; written only when set, absent = off

    inline const juce::Identifier chopId        { "Chop" };
    inline const juce::Identifier chopPatternId { "pattern" };

    inline const juce::Identifier sampleId        { "Sample" };
    inline const juce::Identifier sampleDefaultId { "default" };
    inline const juce::Identifier sampleNameId    { "name" };
    inline const juce::Identifier samplePathId    { "path" };
    inline const juce::Identifier sampleHashId    { "hash" };
    inline const juce::Identifier sampleFlacId    { "flac" };
}
