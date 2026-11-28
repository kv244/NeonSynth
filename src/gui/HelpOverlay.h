// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "gui/NeonStyle.h"

/**
    Quick-reference help for Neon Synth, shown over the whole editor when the
    "?" button is pressed.

    The text is laid out on the editor's 1280 x 860 design grid. The overlay is a
    child of the scaled content, so it stays legible at any window size without
    re-flowing. It is plain static text, built once and drawn once into a cached
    image (v1.10.0: a CRT terminal page, VT323 headings over Plex Mono text).

    Closing: click anywhere on the overlay or press Escape. The editor's "?"
    button also toggles it. The overlay takes all mouse clicks while it is
    visible, so nothing underneath is changed by accident.

    Escape has three ways in, because hosts treat a plugin window's keys differently:
      1. The overlay takes keyboard focus when it opens, so Escape comes straight here.
      2. If the host leaves focus on a control instead, the key goes up through the
         control's parents to the editor's keyPressed(), which calls dismiss().
      3. Some hosts (DAWs with their own shortcuts, or windows that never pass keys on)
         keep the key before any component sees it (v1.11.0). While the overlay is open, a
         timer also reads the Escape key's state from the system, 40 times a second, and
         closes on a fresh press while the host's process is in front. It runs only while
         the help is showing, and only notices a key pressed after the help opened.

    To edit the help, change the entries in buildColumns() (in HelpOverlay.cpp) - each one is a
    heading followed by its lines.
*/
class HelpOverlay : public juce::Component,
                    private juce::Timer
{
public:
    /** Called when the overlay closes itself (click or Escape), so the owner can update its button. */
    std::function<void()> onClose;

    /** Whether Escape is held down right now while this process is in front: the system's key
        state, which a host cannot keep from us (way 3 above). Tests replace it. */
    std::function<bool()> isEscapeHeldInFront = []
    {
        return juce::Process::isForegroundProcess() && juce::KeyPress::isKeyCurrentlyDown (juce::KeyPress::escapeKey);
    };

    HelpOverlay (int designWidth, int designHeight);

    void paint (juce::Graphics& g) override;

    /** The page's cache (EditorTests can check it is drawn once and reused). */
    const neon::crt::CachedPainter& getPageCache() const noexcept { return page; }

    void mouseDown (const juce::MouseEvent&) override  { close(); }

    bool keyPressed (const juce::KeyPress& key) override;

    /** Shows the overlay and takes keyboard focus so Escape works straight away. */
    void show();

    /** The overlay was hidden by any route (Escape, a click, the "?" button): no more polling. */
    void visibilityChanged() override;

    /** One look at the Escape key (the timer's work; tests call it directly). */
    void pollEscapeKey();

    /** Closes the overlay if it is open; returns true if it was. */
    bool dismiss();

private:
    void timerCallback() override   { pollEscapeKey(); }

    /** The CRT terminal page: the tube darkened almost to black, a thin glowing
        phosphor frame, VT323 headings and Plex Mono body text. The text is bright
        and the backing nearly opaque so it stays easy to read under the scanlines. */
    void paintPage (juce::Graphics& g) const;

    void close();

    using Section = std::pair<const char*, std::initializer_list<const char*>>;

    /** Appends a heading and its lines to one column, and the same text to its halo
        string with only the heading visible (so both lay out identically). */
    void addSection (size_t column, const Section& section, juce::Colour colour);

    void buildColumns();

    // 40 Hz: a quick tap holds a key down for 50 ms or more, so one is never missed.
    static constexpr int escapePollHz = 40;
    bool escapeWasDown = false;

    const int baseWidth, baseHeight;
    static constexpr float columnWidth = 560.0f;
    static constexpr float columnX[2] = { 40.0f, 680.0f };
    std::array<juce::AttributedString, 2> columns;
    std::array<juce::AttributedString, 2> glows;   // the headings' halo, same layout as columns
    neon::crt::CachedPainter page;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HelpOverlay)
};
