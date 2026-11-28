// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include <utility>
#include <vector>

/**
    The factory bank (v1.11.0): 16 presets compiled into the plugin, 24 from v1.12.0, which are also the host's
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
        const char* harmonics = nullptr; ///< v1.12.0: CUSTOM's harmonics, "k:level" or "k:level@phase"
                                         ///< (k 1..32, level 0..1, phase in radians), unlisted ones 0;
                                         ///< nullptr: the SAW set a new session starts with
        const char* chopDepths = nullptr; ///< v1.13.0: CHOP's 16 depths, "0".."15" separated by spaces;
                                          ///< replaces `chop` when given
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
                { "gain", -9.0f } } },

            { "Comb Chime", "Keys", "A combed, lightly folded triangle: a metallic chime with a soft ring",
              { { "waveform", 2.0f }, { "chopOvertone", 100.0f }, { "chopComb", 3.0f },
                { "foldMix", 40.0f }, { "foldDrive", 2.0f },
                { "attack", 0.002f }, { "decay", 1.4f }, { "sustain", 0.0f }, { "release", 0.8f },
                { "cutoff", 7000.0f }, { "gain", -5.0f } } },

            { "Marimba Chop", "Keys", "A chopped square with a short decay: wooden, hollow mallet notes",
              { { "waveform", 1.0f }, { "chopOvertone", 100.0f }, { "chopComb", 4.0f },
                { "attack", 0.001f }, { "decay", 0.45f }, { "sustain", 0.0f }, { "release", 0.3f },
                { "cutoff", 3500.0f }, { "gain", -7.0f } } },

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
                { "cutoff", 3000.0f }, { "width", 0.7f }, { "gain", -4.0f } } },

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
                { "lfoShape", 4.0f }, { "lfoRate", 0.2f }, { "lfoCutoff", 1.0f }, { "width", 0.8f },
                { "gain", -3.0f } } },

            { "Gale", "FX", "Wind Sweep louder and wilder: a howling, gritty storm; add a fifth above",
              { { "noiseLevel", 100.0f }, { "filterMode", 2.0f }, { "cutoff", 900.0f }, { "filterRes", 80.0f },
                { "filterDrive", 30.0f },
                { "attack", 2.5f }, { "sustain", 1.0f }, { "release", 3.0f },
                { "filterEnvAmt", 3.0f }, { "filterAttack", 3.0f }, { "filterDecay", 2.0f },
                { "filterSustain", 0.3f }, { "filterRelease", 3.0f }, { "filterVel", 1.0f },
                { "lfoShape", 1.0f }, { "lfoRate", 0.35f }, { "lfoCutoff", 1.5f }, { "lfoAmp", 30.0f },
                { "width", 0.8f }, { "gain", -9.0f } } },

            { "Drifting Cloud", "FX", "Grains scanning forward through the pad: a moving atmosphere",
              { { "source", 2.0f }, { "grPos", 0.2f }, { "grScan", 0.2f }, { "grSize", 100.0f },
                { "grDensity", 45.0f }, { "grSpray", 0.25f }, { "grJitter", 12.0f },
                { "attack", 0.3f }, { "decay", 0.5f }, { "sustain", 0.7f }, { "release", 1.5f },
                { "width", 0.6f } } },

            // ── v1.12.0 "Fourier": CUSTOM harmonics and NAIVE ─────────────────
            // Appended, so the host's programs 1-16 keep their numbers.
            { "Drawbar Organ", "Keys", "CUSTOM: harmonics 1, 2, 3, 4, 6 and 8, like an organ's drawbars; add the mod wheel for vibrato",
              { { "custom", 1.0f }, { "attack", 0.002f }, { "decay", 0.1f }, { "sustain", 1.0f }, { "release", 0.08f },
                { "cutoff", 12000.0f }, { "width", 0.4f }, { "gain", -16.0f } },
              nullptr, nullptr, "1:1 2:0.8 3:0.6 4:0.5 6:0.3 8:0.25" },

            { "Square Ladder", "Keys", "CUSTOM: harmonics 1, 4, 9, 16 and 25 only, a bell that is still in tune; play from C4 up",
              { { "custom", 1.0f }, { "attack", 0.001f }, { "decay", 2.5f }, { "sustain", 0.0f }, { "release", 1.5f },
                { "cutoff", 14000.0f }, { "gain", -12.0f } },
              nullptr, nullptr, "1:1 4:0.5 9:0.35 16:0.25 25:0.18" },

            { "Octave Stack", "Bass", "CUSTOM: harmonics 1, 2, 4 and 8, octaves with no fifths; mono",
              { { "custom", 1.0f }, { "attack", 0.004f }, { "decay", 0.35f }, { "sustain", 0.6f }, { "release", 0.15f },
                { "cutoff", 2500.0f }, { "width", 0.0f }, { "voiceCount", 1.0f }, { "gain", -4.0f } },
              nullptr, nullptr, "1:1 2:0.6 4:0.35 8:0.2" },

            { "Hollow Reed", "Lead", "CUSTOM: odd harmonics only, the hollow tone of a clarinet; slow vibrato",
              { { "custom", 1.0f }, { "attack", 0.03f }, { "decay", 0.2f }, { "sustain", 0.85f }, { "release", 0.25f },
                { "cutoff", 4000.0f }, { "lfoRate", 5.0f }, { "lfoPitch", 0.12f }, { "width", 0.2f }, { "gain", -2.5f } },
              nullptr, nullptr, "1:1 3:0.45 5:0.25 7:0.15 9:0.1 11:0.07" },

            { "Phase Matters", "Lead", "A sawtooth's levels with scattered phases, folded: press ZERO PHASE and the tone changes, though without FOLD it would not",
              { { "custom", 1.0f }, { "harmonics", 16.0f }, { "foldMix", 80.0f }, { "foldDrive", 3.0f },
                { "attack", 0.005f }, { "decay", 0.3f }, { "sustain", 0.7f }, { "release", 0.2f },
                { "cutoff", 7000.0f }, { "width", 0.3f }, { "gain", -4.0f } },
              nullptr, nullptr,
              "1:0.5@0.4 2:0.25@2.9 3:0.1667@5.1 4:0.125@1.3 5:0.1@4.4 6:0.0833@0.9 7:0.0714@3.6 8:0.0625@5.8 "
              "9:0.0556@2.2 10:0.05@4.9 11:0.0455@0.2 12:0.0417@3.1 13:0.0385@5.5 14:0.0357@1.7 15:0.0333@4.0 16:0.03125@6.1" },

            { "Aliased Arcade", "Lead", "NAIVE: a square with no band-limiting, so high notes alias into chiptune grit; play from C6 up",
              { { "waveform", 1.0f }, { "naive", 1.0f }, { "filterEnabled", 0.0f },
                { "attack", 0.001f }, { "decay", 0.2f }, { "sustain", 0.5f }, { "release", 0.1f },
                { "width", 0.2f }, { "gain", -8.0f } } },

            { "Vowel Ah", "Pad", "CUSTOM: a hump at harmonics 6 to 9 over a soft slope, like a sung vowel; best around C3",
              { { "custom", 1.0f }, { "attack", 0.6f }, { "decay", 0.5f }, { "sustain", 0.8f }, { "release", 2.0f },
                { "cutoff", 6000.0f }, { "width", 0.7f }, { "gain", -9.5f } },
              nullptr, nullptr, "1:0.6 2:0.35 3:0.25 4:0.2 5:0.3 6:0.5 7:0.6 8:0.5 9:0.35 10:0.2 11:0.1 12:0.08" },

            { "Fold-back Siren", "FX", "NAIVE: a slowly rising and falling sawtooth whose aliases move the other way; hold a note from C6 up",
              { { "waveform", 3.0f }, { "naive", 1.0f }, { "filterEnabled", 0.0f },
                { "attack", 0.5f }, { "sustain", 1.0f }, { "release", 1.5f },
                { "lfoShape", 1.0f }, { "lfoRate", 0.1f }, { "lfoPitch", 12.0f }, { "width", 0.5f }, { "gain", -1.0f } } },

            // ── v1.13.0 ───────────────────────────────────────────────────────
            // A tubular bell's strike note is heard from partials near 2:3:4 above a weak hum,
            // with fainter high partials, a bright strike that mellows and a long ring. CUSTOM
            // builds that from whole harmonics; a slow, shallow tremolo gives the beating of a
            // real tube. (The ring modulator later in v1.13 can add true inharmonicity.)
            { "Tubular Bells", "Keys", "CUSTOM: strike partials 2, 3 and 4 over a weak hum, a bright strike and a long ring; play from C4 to C6",
              { { "custom", 1.0f }, { "harmonics", 17.0f },
                { "attack", 0.001f }, { "decay", 5.0f }, { "sustain", 0.0f }, { "release", 4.0f },
                { "cutoff", 2500.0f }, { "filterEnvAmt", 2.0f }, { "filterAttack", 0.001f }, { "filterDecay", 1.5f }, { "filterSustain", 0.0f },
                { "filterRelease", 2.0f }, { "lfoRate", 3.5f }, { "lfoAmp", 8.0f }, { "width", 0.5f }, { "gain", -8.0f } },
              nullptr, nullptr, "1:0.15 2:0.9 3:0.8 4:0.7 5:0.2 6:0.35 8:0.2 9:0.25 11:0.12 13:0.15 17:0.08" },

            { "Acid Line", "Sequence", "A 303-style line: resonant sawtooth, snappy filter envelope, accents open the filter, SLIDE steps glide",
              { { "waveform", 3.0f }, { "attack", 0.003f }, { "decay", 0.3f }, { "sustain", 0.6f }, { "release", 0.04f },
                { "cutoff", 350.0f }, { "filterRes", 82.0f }, { "filterDrive", 30.0f },
                { "filterEnvAmt", 3.2f }, { "filterAttack", 0.001f }, { "filterDecay", 0.22f }, { "filterSustain", 0.0f },
                { "filterRelease", 0.08f }, { "filterVel", 1.5f }, { "width", 0.0f }, { "gain", -8.0f },
                { "seqOn", 1.0f }, { "seqRate", 1.0f }, { "seqGate", 0.5f }, { "glide", 60.0f } },
              "A2:120 A2:70 A3:100/ A2:70 -A2:70 C3:120 A2:70 G3:110/ "
              "A2:70 D#3:100 A2:70 -A2:70 C3:120/ D3:80 A2:70 G2:110" },

            { "Orchestra Hit", "Keys", "CUSTOM: a bright brass-and-strings stab with a noisy bite, after the 1980s sampled orchestra hit; play short chords",
              { { "custom", 1.0f }, { "harmonics", 24.0f }, { "noiseLevel", 8.0f },
                { "attack", 0.002f }, { "decay", 0.45f }, { "sustain", 0.0f }, { "release", 0.3f },
                { "cutoff", 1600.0f }, { "filterEnvAmt", 2.6f }, { "filterAttack", 0.001f }, { "filterDecay", 0.3f },
                { "filterSustain", 0.0f }, { "filterRelease", 0.3f }, { "filterVel", 1.0f }, { "filterDrive", 25.0f },
                { "width", 0.7f }, { "gain", -12.0f } },
              nullptr, nullptr,
              "1:1 2:0.8 3:0.65 4:0.55 5:0.45 6:0.38 7:0.32 8:0.27 9:0.23 10:0.2 11:0.17 12:0.15 14:0.12 16:0.1 18:0.08 20:0.06 22:0.05 24:0.04" },

            { "Swelling Brass Pad", "Pad", "A sawtooth that swells open over two seconds with a slow vibrato, after the brassy pads of early-1980s film scores; hold chords",
              { { "waveform", 3.0f }, { "noiseLevel", 3.0f },
                { "attack", 1.2f }, { "decay", 1.5f }, { "sustain", 0.85f }, { "release", 2.5f },
                { "cutoff", 600.0f }, { "filterRes", 15.0f }, { "filterEnvAmt", 2.2f }, { "filterAttack", 1.8f },
                { "filterDecay", 2.5f }, { "filterSustain", 0.6f }, { "filterRelease", 2.5f }, { "filterVel", 0.6f },
                { "lfoRate", 5.0f }, { "lfoPitch", 0.12f }, { "width", 0.8f }, { "gain", -12.0f } } },
        };
        return presets;
    }
}
