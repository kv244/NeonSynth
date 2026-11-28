// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "gui/HelpOverlay.h"

HelpOverlay::HelpOverlay (int designWidth, int designHeight)
    : baseWidth (designWidth), baseHeight (designHeight)
{
    buildColumns();
    setWantsKeyboardFocus (true);
}

void HelpOverlay::paint (juce::Graphics& g)
{
    // Static text over the editor: drawn once into an image and blitted after that,
    // because the overlay repaints whenever a meter or the scope underneath it does.
    page.paint (g, getLocalBounds(), [this] (juce::Graphics& pg) { paintPage (pg); });
}

bool HelpOverlay::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        close();
        return true;
    }
    return false;
}

void HelpOverlay::show()
{
    setVisible (true);
    toFront (true);
    grabKeyboardFocus();
    escapeWasDown = isEscapeHeldInFront();   // a key already down when it opened doesn't count
    startTimerHz (escapePollHz);
}

void HelpOverlay::visibilityChanged()
{
    if (! isVisible())
        stopTimer();
}

void HelpOverlay::pollEscapeKey()
{
    const bool down = isEscapeHeldInFront();
    const bool pressed = down && ! escapeWasDown;
    escapeWasDown = down;
    if (pressed)
        dismiss();
}

bool HelpOverlay::dismiss()
{
    if (! isVisible())
        return false;
    close();
    return true;
}

void HelpOverlay::paintPage (juce::Graphics& g) const
{
    g.fillAll (neon::crt::screenEdge.withAlpha (0.95f));

    // Frame: a 1 px phosphor line, square corners, a faint halo outside and inside.
    const juce::Rectangle<float> frame { 20.0f, 16.0f, (float) baseWidth - 40.0f, (float) baseHeight - 32.0f };
    const auto hue = neon::crt::phosphorGlow;
    g.setColour (hue.withAlpha (0.05f));
    g.drawRect (frame.expanded (3.0f), 3.0f);
    g.setColour (hue.withAlpha (0.10f));
    g.drawRect (frame.expanded (1.0f), 2.0f);
    g.setColour (hue.withAlpha (0.06f));
    g.drawRect (frame.reduced (1.0f), 3.0f);
    g.setColour (hue.withAlpha (0.65f));
    g.drawRect (frame, 1.0f);

    neon::crt::drawGlowText (g, "> NEON SYNTH - QUICK HELP", { 40.0f, 24.0f, 600.0f, 36.0f },
                             neon::crt::phosphor, neon::crt::displayFont (32.0f), juce::Justification::left);

    g.setColour (neon::crt::phosphorDim);
    g.setFont (neon::crt::labelFont (13.0f));
    g.drawText ("Click anywhere or press Esc to close", baseWidth - 440, 32, 400, 22,
                juce::Justification::right);

    // A dotted rule under the header, like a terminal's separator line.
    g.setColour (neon::crt::phosphorDim.withAlpha (0.45f));
    for (float x = 40.0f; x < (float) baseWidth - 40.0f; x += 6.0f)
        g.fillRect (x, 64.0f, 3.0f, 1.0f);

    for (size_t i = 0; i < columns.size(); ++i)
    {
        const juce::Rectangle<float> area { columnX[i], 76.0f, columnWidth, (float) baseHeight - 100.0f };

        // The headings' halo: the same layout with only the headings drawn, faint,
        // a pixel off in each direction, under the text itself.
        juce::TextLayout halo;
        halo.createLayout (glows[i], columnWidth);
        for (const auto& [dx, dy] : { std::pair { -1.0f, 0.0f }, std::pair { 1.0f, 0.0f }, std::pair { 0.0f, -1.0f }, std::pair { 0.0f, 1.0f } })
            halo.draw (g, area.translated (dx, dy));

        juce::TextLayout layout;
        layout.createLayout (columns[i], columnWidth);
        layout.draw (g, area);
    }
}

void HelpOverlay::close()
{
    setVisible (false);
    if (onClose != nullptr)
        onClose();
}

void HelpOverlay::addSection (size_t column, const Section& section, juce::Colour colour)
{
    const auto heading = "> " + juce::String (section.first) + "\n";
    const auto headingFont = neon::crt::displayFont (22.0f);
    const auto bodyFont    = neon::crt::labelFont (13.0f);
    const auto gapFont     = neon::crt::labelFont (6.0f);

    // Headings in the panel hue, lifted towards white so they read as lit phosphor.
    columns[column].append (heading, headingFont, colour.interpolatedWith (juce::Colours::white, 0.35f));
    glows[column].append (heading, headingFont, colour.withAlpha (0.28f));

    for (const auto* line : section.second)
    {
        columns[column].append (juce::String (line) + "\n", bodyFont, neon::crt::phosphor.withAlpha (0.92f));
        glows[column].append (juce::String (line) + "\n", bodyFont, juce::Colours::transparentBlack);
    }

    columns[column].append ("\n", gapFont, colour);   // gap before the next heading
    glows[column].append ("\n", gapFont, juce::Colours::transparentBlack);
}

void HelpOverlay::buildColumns()
{
    for (auto* set : { &columns, &glows })
        for (auto& c : *set)
        {
            c.setWordWrap (juce::AttributedString::byWord);
            c.setLineSpacing (1.5f);
        }

    // Left column: the step sequencer.
    addSection (0, { "STEP SEQUENCER", {
        "RUN starts and stops the pattern.",
        "SYNC follows your DAW's tempo and transport: press play in the DAW to hear it. In the Standalone, or with SYNC off, TEMPO sets the speed.",
        "Rate is the step length: 1/16 = four steps per beat, T = triplets.",
        "SWING delays every second step. GATE sets how long each note lasts." } }, neon::amber);

    addSection (0, { "EDITING STEPS", {
        "Click: switch a step on or off (its note is kept).",
        "Drag up / down: change the note (one semitone per 6 px, or one scale note with SCALE on). The wheel nudges it.",
        "Drag the velocity bar at the bottom (or shift-drag anywhere): velocity, the step's accent. Shift + wheel nudges it.",
        "TIE holds the note into the next step. SLD slides into the next step's note at GLIDE time." } }, neon::amber);

    addSection (0, { "PATTERN ROW", {
        "LENGTH: the pattern repeats after 1 to 16 steps. KEY + SCALE: choosing them fills the steps with the scale, rising from octave 3 (UNDO undoes it).",
        "ENTER: switch it on, click a step, play keys: each fills a step and moves on.",
        "TOOLS: shift, octave up / down, reverse, randomise, clear. UNDO (or Ctrl+Z) steps back." } }, neon::amber);

    addSection (0, { "MODES", {
        "Layer: the pattern plays along with your keyboard.",
        "Replace: the pattern only; keyboard notes are ignored.",
        "Transpose: the pattern plays while you hold a key, shifted by that key's distance from C4 (hold C5 for an octave up)." } }, neon::amber);

    addSection (0, { "SHAPE (Oscillator and Random)", {
        "The strip shows one cycle of the wave after CHOP and Fold. Click a step to chop it; drag to draw each step's depth (top = fully chopped); double-click resets.",
        "OVERTONE: how far the shaded steps are cut (100% silences them, 200% turns them upside down). COMB repeats the pattern within each cycle.",
        "NOISE fades the wave into white noise. FOLD folds it back on itself for brighter harmonics; DRIVE sets how hard." } }, neon::cyan);

    addSection (0, { "FOURIER (above the strip; v1.12)", {
        "CUSTOM plays a wave built from 32 harmonics, set as bars in the HARMONICS view; HARMONICS sets how many play. BUILD adds them one by one.",
        "NAIVE switches band-limiting off: harmonics above half the sample rate fold back down (red on the SPECTRUM). DRAW: draw one cycle in the strip." } }, neon::cyan);

    addSection (0, { "MOVEMENT (MODULATION panel)", {
        "LFO: a slow wave (Sine, Triangle, Square, Saw, or Random: a new level each cycle). RATE sets its speed in Hz; with SYNC it is a note length that follows the tempo.",
        "PITCH, CUTOFF and AMP set how far it moves the pitch (semitones), the filter (octaves; the green band under CUTOFF) and the level (tremolo). At 0 it does nothing.",
        "Pitch bend and mod wheel work from any MIDI keyboard or controller. BEND: the range in semitones each way. VIBRATO: the wheel's vibrato depth at the top, in cents.",
        "GLIDE: how long a note takes to slide from the last one. LEGATO (at MONO): a key played over a held one glides there without a new attack.",
        "HOLD (in the header) keeps your keys sounding after you let go, until you switch it off." } }, neon::green);

    // Right column: the synth and getting sound out.
    addSection (1, { "SOUND", {
        "SOURCE: the oscillator (its Waveform), Random (16 drifting steps), the sample, or Granular: grains of it.",
        "ENVELOPE: attack, decay, sustain and release of every note. AMP shows the loudness envelope, FILTER the cutoff's. Drag the points on the shape or turn the knobs; double-click a point to reset it.",
        "FILTER: CUTOFF and the mode (low-pass, high-pass, band-pass or notch); ON switches it in. RES boosts the cutoff; near 100% the filter rings on its own. Its DRIVE soft-clips the sound before the filter. ENV: how many octaves the filter envelope moves the cutoff (up or down); VEL: how far harder notes open it.",
        "GAIN: the output level fader beside the L / R meter (double-click for 0 dB). WIDTH: how far notes are spread left and right by pitch; 0% is mono.",
        "VOICES lights the voices in use, each with the note it plays. Click a light to set how many voices can play: 1 = mono. SCOPE / SPECTRUM switch the display.",
        "SCANLINES (in the header) turns the CRT scanlines on or off; the choice is saved with the session." } }, neon::cyan);

    addSection (1, { "SAMPLE", {
        "LOAD a WAV, AIFF or FLAC file, or drop one anywhere on the synth. PAD goes back to the built-in pad.",
        "ROOT is the key that plays the file at its own pitch. Drag the cyan line to set the start and the amber brackets to set the loop.",
        "One-shot plays to the end; Loop loops while the note sounds; Loop until release loops while the key is held. XFADE smooths the loop point." } }, neon::cyan);

    addSection (1, { "GRANULAR", {
        "POS places the cloud in the sample (or drag on the waveform); SCAN moves it, 0 freezes it. SIZE and DENS set grain length and grains per second.",
        "SPRAY scatters the grains around POS; JITTER detunes each one. ROOT works as for Sample." } }, neon::magenta);

    addSection (1, { "PLAYING NOTES", {
        "MIDI keyboard in the Standalone: Options > Audio/MIDI Settings, then tick it under MIDI inputs.",
        "No keyboard? Press RUN, or use a virtual keyboard such as VMPK with loopMIDI." } }, neon::cyan);

    addSection (1, { "NO SOUND?", {
        "Laptop speakers play very little below about 200 Hz. For low notes pick Sawtooth or Square, raise the notes, or use headphones.",
        "Check the output device and its active channels in Options > Audio/MIDI Settings." } }, neon::cyan);

    addSection (1, { "PRESETS AND SAVING", {
        "The preset bar: < and > step through the presets; click the name for the menu; SAVE keeps your sound in Documents/NeonSynth/Presets.",
        "The pattern, the sample (up to 10 MB, otherwise by its path) and all settings are saved with your DAW project (and by the Standalone between runs)." } }, neon::cyan);
}
