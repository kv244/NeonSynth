// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include <utility>
#include <vector>

/**
    The factory bank (v1.11.0): 16 presets compiled into the plugin, which are also the host's
    program list (NeonSynthProcessor::getProgramName and friends).

    Each preset is written as data, in each parameter's own units (Hz, seconds, %, a choice's
    index), so a value can be changed by ear without any tool: PresetManager turns an entry into
    a complete session (every parameter, the pattern, CHOP, the built-in pad) and loads it through
    setStateInformation like any other preset. A parameter not listed takes its default, so each
    preset sounds the same whatever was loaded before it.

    Sources: the factory preset ideas of the v1.10 roadmap and the user manual's sound-design
    recipes (rounded bass, slow pad, frozen and moving granular, random pluck, chopped lead, wind).
    Sample-based presets use the built-in pad, so the bank needs no files. GLIDE, LEGATO and SLIDE
    appear in two presets (Rounded Bass, Chopped Lead); every other v1.11 parameter stays at its
    default.

    Patterns are 16 space-separated steps: a note name with C4 = MIDI 60 ("D#3"), then
    ":velocity", a leading "-" for a step that is off (its note is kept, as in the grid), and a
    trailing "~" for a tie or "/" for a slide into the next step. nullptr = the default pattern.
*/
namespace FactoryPresets
{
    struct Preset
    {
        const char* name;
        const char* category;       ///< one of PresetManager's categories
        const char* description;    ///< one line: what it is and how to play it
        std::vector<std::pair<const char*, float>> values;   ///< parameter ID, value in the parameter's units
        const char* pattern = nullptr;   ///< nullptr: the default bassline
        const char* chop    = nullptr;   ///< CHOP's 16 steps, "1" = chopped; nullptr: every second step
    };

    inline const std::vector<Preset>& all()
    {
        // Choice indices: waveform 0 Sine, 1 Square, 2 Triangle, 3 Sawtooth; source 0 Oscillator,
        // 1 Sample, 2 Granular, 3 Random; filterMode 0 Low-pass, 1 High-pass, 2 Band-pass, 3 Notch;
        // seqRate 0 1/8, 1 1/16; lfoShape 0 Sine, 1 Triangle, 2 Square, 3 Saw, 4 Random;
        // smpLoop 1 Loop; seqKey 9 A; seqScale 10 Minor pentatonic.
        static const std::vector<Preset> presets
        {
            // ── Keys ──────────────────────────────────────────────────────────
            { "Glass Bell", "Keys", "A folded sine that rings and mellows as it fades; play from C6 up",
              { { "waveform", 0.0f }, { "foldMix", 100.0f }, { "foldDrive", 5.0f },
                { "attack", 0.001f }, { "decay", 2.0f }, { "sustain", 0.0f }, { "release", 1.5f },
                { "cutoff", 3000.0f }, { "filterEnvAmt", 1.6f }, { "filterAttack", 0.001f },
                { "filterDecay", 1.8f }, { "filterSustain", 0.0f }, { "filterRelease", 1.5f },
                { "gain", -2.0f } } },

            { "Comb Chime", "Keys", "A combed, lightly folded triangle: a metallic chime with a soft ring",
              { { "waveform", 2.0f }, { "chopOvertone", 100.0f }, { "chopComb", 3.0f },
                { "foldMix", 40.0f }, { "foldDrive", 2.0f },
                { "attack", 0.002f }, { "decay", 1.4f }, { "sustain", 0.0f }, { "release", 0.8f },
                { "cutoff", 7000.0f } } },

            { "Marimba Chop", "Keys", "A chopped square with a short decay: wooden, hollow mallet notes",
              { { "waveform", 1.0f }, { "chopOvertone", 100.0f }, { "chopComb", 4.0f },
                { "attack", 0.001f }, { "decay", 0.45f }, { "sustain", 0.0f }, { "release", 0.3f },
                { "cutoff", 3500.0f } } },

            // ── Bass ──────────────────────────────────────────────────────────
            { "Warm Sub", "Bass", "A sine with a touch of fold, centred: a round sub bass for C1 to C3",
              { { "waveform", 0.0f }, { "foldMix", 25.0f }, { "foldDrive", 2.0f },
                { "attack", 0.005f }, { "decay", 0.3f }, { "sustain", 0.8f }, { "release", 0.2f },
                { "cutoff", 900.0f }, { "width", 0.0f } } },

            { "Folded Growl", "Bass", "A hard-folded sawtooth under a low-pass: a gritty, growling bass",
              { { "waveform", 3.0f }, { "foldMix", 100.0f }, { "foldDrive", 8.0f },
                { "attack", 0.003f }, { "decay", 0.4f }, { "sustain", 0.7f }, { "release", 0.15f },
                { "cutoff", 1500.0f }, { "width", 0.1f }, { "gain", -2.0f } } },

            // ── Lead ──────────────────────────────────────────────────────────
            { "Chopped Lead", "Lead", "A chopped, folded sawtooth, mono: overlapping keys glide (legato)",
              { { "waveform", 3.0f }, { "chopOvertone", 100.0f }, { "chopComb", 2.0f },
                { "noiseLevel", 5.0f }, { "foldDrive", 3.0f }, { "foldMix", 50.0f },
                { "attack", 0.005f }, { "decay", 0.3f }, { "sustain", 0.6f }, { "release", 0.2f },
                { "cutoff", 6000.0f }, { "width", 0.3f },
                { "voiceCount", 1.0f }, { "legato", 1.0f }, { "glide", 80.0f } } },

            // ── Pad ───────────────────────────────────────────────────────────
            { "Sampled Pad", "Pad", "The built-in pad looped through its middle half; hold chords",
              { { "source", 1.0f }, { "smpRoot", 60.0f }, { "smpLoop", 1.0f }, { "smpStart", 0.0f },
                { "smpLoopStart", 0.25f }, { "smpLoopEnd", 0.75f }, { "smpXfade", 20.0f },
                { "attack", 0.8f }, { "decay", 0.6f }, { "sustain", 0.75f }, { "release", 2.0f },
                { "cutoff", 3000.0f }, { "width", 0.7f } } },

            { "Shimmer Cloud", "Pad", "Dense, short grains of the pad, slowly scanned; play octaves or fifths",
              { { "source", 2.0f }, { "grPos", 0.4f }, { "grScan", 0.1f }, { "grSize", 40.0f },
                { "grDensity", 90.0f }, { "grSpray", 0.35f }, { "grJitter", 20.0f },
                { "attack", 0.8f }, { "sustain", 0.7f }, { "release", 2.5f }, { "cutoff", 9000.0f } } },

            { "Frozen Cloud", "Pad", "Long grains held at one point of the pad; move POS to explore",
              { { "source", 2.0f }, { "grPos", 0.5f }, { "grScan", 0.0f }, { "grSize", 150.0f },
                { "grDensity", 35.0f }, { "grSpray", 0.1f }, { "grJitter", 5.0f },
                { "attack", 0.5f }, { "decay", 0.3f }, { "sustain", 0.8f }, { "release", 2.0f },
                { "cutoff", 4000.0f }, { "gain", 4.0f } } },

            // ── Sequence ──────────────────────────────────────────────────────
            { "Rounded Bass", "Sequence", "The default bassline on a filtered sawtooth, with two slides",
              { { "waveform", 3.0f }, { "attack", 0.005f }, { "decay", 0.18f }, { "sustain", 0.35f },
                { "release", 0.1f }, { "cutoff", 700.0f }, { "width", 0.0f },
                { "seqOn", 1.0f }, { "seqRate", 1.0f }, { "seqGate", 0.4f }, { "glide", 60.0f } },
              "C3:112 -C3:90 C4:80 C3:100 D#3:112 -D#3:90 G3:90 A#3:80/ "
              "C3:112 -C3:90 C4:80 C3:100/ F3:112 -F3:90 G3:90 D#3:80" },

            { "Random Pluck", "Sequence", "Random-source plucks in 1/8 with velocity accents; each note differs",
              { { "source", 3.0f }, { "attack", 0.003f }, { "decay", 0.25f }, { "sustain", 0.0f },
                { "release", 0.12f }, { "cutoff", 2500.0f },
                { "seqOn", 1.0f }, { "seqRate", 0.0f }, { "seqGate", 0.35f } },
              "C4:120 -C4:80 G3:70 C4:90 D#4:110 -D#4:80 C4:70 G3:90 "
              "A#3:120 -A#3:80 C4:70 D#4:90 F4:110 D#4:70 C4:90 G3:70" },

            { "Broken Radio", "Sequence", "A noisy, folded Random wave in A minor pentatonic, 12 steps against the bar",
              { { "source", 3.0f }, { "noiseLevel", 15.0f }, { "foldMix", 30.0f }, { "foldDrive", 4.0f },
                { "attack", 0.002f }, { "decay", 0.15f }, { "sustain", 0.2f }, { "release", 0.1f },
                { "cutoff", 3000.0f },
                { "seqOn", 1.0f }, { "seqRate", 1.0f }, { "seqGate", 0.4f },
                { "seqLength", 12.0f }, { "seqKey", 9.0f }, { "seqScale", 10.0f } },
              "A3:110 -A3:80 C4:90 E4:100 D4:80 -D4:80 G3:100 A3:90 "
              "C4:110 -C4:80 E3:90 G3:80 A4:100 G4:80 E4:90 D4:70" },

            // ── Percussion ────────────────────────────────────────────────────
            { "Closed Hat Loop", "Percussion", "High-passed noise ticks in 1/16 with accents on the beat",
              { { "noiseLevel", 100.0f }, { "attack", 0.001f }, { "decay", 0.05f }, { "sustain", 0.0f },
                { "release", 0.05f }, { "filterMode", 1.0f }, { "cutoff", 7000.0f }, { "width", 0.0f },
                { "seqOn", 1.0f }, { "seqRate", 1.0f }, { "seqGate", 0.3f } },
              "C4:120 C4:60 C4:90 C4:60 C4:120 C4:60 C4:90 C4:75 "
              "C4:120 C4:60 C4:90 C4:60 C4:120 C4:70 C4:100 C4:80" },

            // ── FX ────────────────────────────────────────────────────────────
            { "Wind Sweep", "FX", "Band-passed noise that rises into a whistle and gusts; hold low notes",
              { { "noiseLevel", 100.0f }, { "filterMode", 2.0f }, { "cutoff", 600.0f }, { "filterRes", 55.0f },
                { "attack", 2.5f }, { "sustain", 1.0f }, { "release", 3.0f },
                { "filterEnvAmt", 2.0f }, { "filterAttack", 3.0f }, { "filterDecay", 4.0f },
                { "filterSustain", 0.3f }, { "filterRelease", 3.0f }, { "filterVel", 1.0f },
                { "lfoShape", 4.0f }, { "lfoRate", 0.2f }, { "lfoCutoff", 1.0f }, { "width", 0.8f } } },

            { "Gale", "FX", "Wind Sweep louder and wilder: a howling, gritty storm; add a fifth above",
              { { "noiseLevel", 100.0f }, { "filterMode", 2.0f }, { "cutoff", 900.0f }, { "filterRes", 80.0f },
                { "filterDrive", 30.0f },
                { "attack", 2.5f }, { "sustain", 1.0f }, { "release", 3.0f },
                { "filterEnvAmt", 3.0f }, { "filterAttack", 3.0f }, { "filterDecay", 2.0f },
                { "filterSustain", 0.3f }, { "filterRelease", 3.0f }, { "filterVel", 1.0f },
                { "lfoShape", 1.0f }, { "lfoRate", 0.35f }, { "lfoCutoff", 1.5f }, { "lfoAmp", 30.0f },
                { "width", 0.8f } } },

            { "Drifting Cloud", "FX", "Grains scanning forward through the pad: a moving atmosphere",
              { { "source", 2.0f }, { "grPos", 0.2f }, { "grScan", 0.2f }, { "grSize", 100.0f },
                { "grDensity", 45.0f }, { "grSpray", 0.25f }, { "grJitter", 12.0f },
                { "attack", 0.3f }, { "decay", 0.5f }, { "sustain", 0.7f }, { "release", 1.5f },
                { "width", 0.6f } } },
        };
        return presets;
    }
}
