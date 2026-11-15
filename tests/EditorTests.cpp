// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Editor tests: the editor is opened offscreen (never put on the desktop) and
    driven with synthetic mouse and wheel events, then checked against what the
    manual says each gesture does:

      - every parameter has a control, and each source shows the controls it should;
      - knobs and sliders move their parameter, inside one begin/end change gesture
        (so hosts record a drag as one automation edit);
      - combo boxes and buttons write their parameter, and follow it when it changes;
      - step grid: click toggles a step, drag changes its note, wheel nudges it;
      - CHOP strip: click toggles a step, a drag paints the same state across steps;
      - sample view: click sets START, dragging a loop bracket moves it, Granular moves POS
        (also where a loop bracket is hidden under it);
      - a replaced sample is freed once its notes end, and an overtaken load clears "loading...";
      - envelope view: dragging A, D or R sideways changes that time, dragging S up or down
        sets the level, each in one gesture; a double-click resets a point; a click elsewhere
        does nothing;
      - envelope tabs: AMP and FILTER show their own view and knobs (and the layout checks run
        on both), the filter envelope's view and knobs move only their own parameters, and the
        chosen tab survives closing and reopening the editor;
      - SCOPE / SPECTRUM and the help page switch what is shown;
      - the VOICES lights: clicking light N sets Voices to N (one gesture), with its readout;
      - the right-hand column: OUTPUT on top and MODULATION below, each control inside
        its own panel; LFO SYNC swaps the rate in Hz for the note length; the readouts;
        the green LFO band under CUTOFF appears only while LFO CUTOFF is not 0; GAIN is a
        vertical fader beside the meter that a double-click returns to 0 dB;
      - every view paints at 75%, 100% and 150% for every source, with and without
        the help page, while notes play.

    Controls are found by component ID only (a parameter's control carries the
    parameter's ID), so the tests don't depend on the layout and carry over to a
    rebuilt editor that keeps the IDs.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target EditorTests
    Run the resulting EditorTests executable; it exits non-zero on failure.
    On Linux it needs an X display (in CI: xvfb-run).
*/
#include "NeonSynthProcessor.h"
#include "Visualisers.h"
#include <cstdio>
#include <cstdlib>
#include <map>

namespace
{
    int failures = 0;

    void check (bool condition, const juce::String& what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what.toRawUTF8());
        if (! condition)
            ++failures;
    }

    /** Lets timers (the editor's 30 Hz refresh) and async messages run. */
    void pump (int milliseconds = 60)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (milliseconds);
    }

    juce::Component* find (juce::Component& root, const juce::String& id)
    {
        if (root.getComponentID() == id)
            return &root;
        for (auto* child : root.getChildren())
            if (auto* found = find (*child, id))
                return found;
        return nullptr;
    }

    /** True if the component and all its parents up to the editor are visible. */
    bool showing (juce::Component& c, juce::Component& editor)
    {
        for (auto* p = &c; p != nullptr && p != &editor; p = p->getParentComponent())
            if (! p->isVisible())
                return false;
        return c.getWidth() > 0 && c.getHeight() > 0;
    }

    /** Counts begin/end change gestures per parameter index. */
    struct GestureLog : juce::AudioProcessorListener
    {
        std::map<int, int> begins, ends;
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
        void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int index) override { ++begins[index]; }
        void audioProcessorParameterChangeGestureEnd   (juce::AudioProcessor*, int index) override { ++ends[index]; }
        void clear() { begins.clear(); ends.clear(); }
        bool oneGesture (int index) const
        {
            const auto b = begins.find (index), e = ends.find (index);
            return b != begins.end() && e != ends.end() && b->second == e->second && b->second >= 1;
        }
    };

    //==========================================================================
    // Synthetic mouse events, delivered straight to the component (no hit-testing).
    struct Mouse
    {
        juce::int64 time = 1000;   // every event a new time: Slider ignores a wheel event with a repeated time
        juce::Point<float> downAt;

        juce::MouseEvent event (juce::Component& c, juce::Point<float> at, juce::ModifierKeys mods, bool dragged)
        {
            time += 20;
            return { juce::Desktop::getInstance().getMainMouseSource(), at, mods,
                     juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f,
                     &c, &c, juce::Time (time), downAt, juce::Time (time), 1, dragged };
        }

        void down (juce::Component& c, juce::Point<float> at, juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier)
        {
            downAt = at;
            c.mouseDown (event (c, at, mods, false));
        }
        void drag (juce::Component& c, juce::Point<float> at, juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier)
        {
            c.mouseDrag (event (c, at, mods, true));
        }
        void up (juce::Component& c, juce::Point<float> at, bool dragged)
        {
            c.mouseUp (event (c, at, {}, dragged));
        }
        void click (juce::Component& c, juce::Point<float> at)
        {
            down (c, at);
            up (c, at, false);
        }
        void wheel (juce::Component& c, juce::Point<float> at, float deltaY)
        {
            juce::MouseWheelDetails w {};
            w.deltaY = deltaY;
            c.mouseWheelMove (event (c, at, {}, false), w);
        }
    };

    //==========================================================================
    juce::RangedAudioParameter* parameter (NeonSynthProcessor& p, const juce::String& id)
    {
        return p.getAPVTS().getParameter (id);
    }

    float valueOf (NeonSynthProcessor& p, const juce::String& id)
    {
        return p.getAPVTS().getRawParameterValue (id)->load();
    }

    void set (NeonSynthProcessor& p, const juce::String& id, float value)
    {
        auto* param = parameter (p, id);
        param->setValueNotifyingHost (param->convertTo0to1 (value));
        pump();
    }

    int indexOf (NeonSynthProcessor& p, const juce::String& id)
    {
        return parameter (p, id)->getParameterIndex();
    }

    /** Renders a few blocks with a chord, so voices, meters and views have something to show. */
    void play (NeonSynthProcessor& p, bool notesOn)
    {
        juce::AudioBuffer<float> buffer (2, 512);
        for (int block = 0; block < 8; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                for (int note : { 48, 60, 64, 67 })
                    midi.addEvent (notesOn ? juce::MidiMessage::noteOn (1, note, (juce::uint8) 100)
                                           : juce::MidiMessage::noteOff (1, note), 0);
            buffer.clear();
            p.processBlock (buffer, midi);
        }
    }

    /** A mono sine in a temporary WAV file; long enough that loading it takes a moment. */
    juce::File writeTestWav (double seconds)
    {
        const auto file = juce::File::createTempFile (".wav");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions{}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (24));

        juce::AudioBuffer<float> b (1, (int) (seconds * 44100.0));
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 330.0 * i / 44100.0));
        writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        return file;
    }

    constexpr int baseWidth = 1280, baseHeight = 860;   // the editor's design size

    constexpr const char* sourceNames[] = { "Oscillator", "Sample", "Granular", "Random" };

    // What the strip under SOURCE shows for each source (1 = shown).
    struct SourceControls { const char* id; bool shown[4]; };
    constexpr SourceControls perSource[] = {
        { "sampleView",   { false, true,  true,  false } },
        { "shapeView",    { true,  false, false, true  } },
        { "smpRoot",      { false, true,  true,  false } },
        { "smpStart",     { false, true,  false, false } },
        { "smpLoopStart", { false, true,  false, false } },
        { "smpLoopEnd",   { false, true,  false, false } },
        { "smpXfade",     { false, true,  false, false } },
        { "grPos",        { false, false, true,  false } },
        { "grScan",       { false, false, true,  false } },
        { "grSize",       { false, false, true,  false } },
        { "grDensity",    { false, false, true,  false } },
        { "grSpray",      { false, false, true,  false } },
        { "grJitter",     { false, false, true,  false } },
        { "chopOvertone", { true,  false, false, true  } },
        { "chopComb",     { true,  false, false, true  } },
        { "noiseLevel",   { true,  false, false, true  } },
        { "foldDrive",    { true,  false, false, true  } },
        { "foldMix",      { true,  false, false, true  } },
    };

    // The ENVELOPE panel's tabs: what each shows (tab 0 = AMP, 1 = FILTER).
    constexpr const char* ampEnvControls[]    = { "adsrView", "attack", "decay", "sustain", "release" };
    constexpr const char* filterEnvControls[] = { "filterAdsrView", "filterAttack", "filterDecay", "filterSustain", "filterRelease" };

    bool isFilterEnvControl (const juce::String& id)
    {
        for (const auto* c : filterEnvControls)
            if (id == c)
                return true;
        return false;
    }

    /** Clicks AMP (0) or FILTER (1) in the ENVELOPE panel. */
    void showEnvTab (juce::Component& ed, int tab)
    {
        if (auto* button = dynamic_cast<juce::Button*> (find (ed, tab == 0 ? "ampEnvButton" : "filterEnvButton")))
            if (! button->getToggleState())
                button->triggerClick();
        pump();
    }

    /** True if every listed control is showing (shown) or none is (! shown). */
    bool allShowing (juce::Component& ed, const char* const* ids, size_t count, bool shown)
    {
        for (size_t i = 0; i < count; ++i)
            if (auto* c = find (ed, ids[i]); c == nullptr || showing (*c, ed) != shown)
                return false;
        return true;
    }

    // The right-hand column (964, 82, 300 x 548) on the design grid, split into OUTPUT and MODULATION (v1.9.0).
    const juce::Rectangle<int> rightColumn (964, 82, 300, 548);
    const juce::Rectangle<int> outputPanel (964, 82, 300, 266);
    const juce::Rectangle<int> modPanel    (964, 360, 300, 270);
    constexpr const char* outputControls[] = { "gain", "width", "meter" };
    constexpr const char* modControls[] = { "lfoShape", "lfoSync", "lfoRate", "lfoDivision", "lfoPitch", "lfoCutoff", "lfoAmp",
                                            "bendRange", "vibrato", "lfoView", "bendBar", "wheelBar" };

    /** A component's bounds in the editor's (design) coordinates; the editor must be at 100%. */
    juce::Rectangle<int> boundsIn (juce::Component& ed, juce::Component& c)
    {
        return ed.getLocalArea (&c, c.getLocalBounds());
    }

    /** True if some label in the editor shows exactly this text. */
    bool labelShows (juce::Component& root, const juce::String& text)
    {
        if (auto* label = dynamic_cast<juce::Label*> (&root); label != nullptr && label->isVisible() && label->getText() == text)
            return true;
        for (auto* child : root.getChildren())
            if (labelShows (*child, text))
                return true;
        return false;
    }

    /** Saves an image into NEONSYNTH_SNAPSHOT_DIR, if that is set. */
    void saveSnapshot (const juce::Image& image, const juce::String& name)
    {
        // JUCE's call instead of std::getenv, which MSVC deprecates.
        if (const auto dir = juce::SystemStats::getEnvironmentVariable ("NEONSYNTH_SNAPSHOT_DIR", {}); dir.isNotEmpty() && image.isValid())
            if (juce::File (dir).createDirectory().wasOk())
            {
                // FileOutputStream appends to an existing file: start each image afresh.
                const auto file = juce::File (dir).getChildFile (name + ".png");
                file.deleteFile();
                if (juce::FileOutputStream out (file); out.openedOk())
                    juce::PNGImageFormat().writeImageToStream (image, out);
            }
    }

    /** Pixels in an area that are clearly the modulation green (and not cyan, magenta or grey). */
    int greenPixels (const juce::Image& image)
    {
        int count = 0;
        for (int y = 0; y < image.getHeight(); ++y)
            for (int x = 0; x < image.getWidth(); ++x)
            {
                const auto c = image.getPixelAt (x, y);
                count += c.getGreen() > 90 && c.getGreen() > c.getRed() + 30 && c.getGreen() > c.getBlue() + 30 ? 1 : 0;
            }
        return count;
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    NeonSynthProcessor processor;
    processor.setRateAndBufferSizeDetails (48000.0, 512);
    processor.prepareToPlay (48000.0, 512);

    GestureLog gestures;
    processor.addListener (&gestures);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditorAndMakeActive());
    if (editor == nullptr)
    {
        std::printf ("FAIL  the processor made no editor\n");
        return 1;
    }
    auto& ed = *editor;
    const int openedWidth = ed.getWidth(), openedHeight = ed.getHeight();
    ed.setSize (baseWidth, baseHeight);   // the checks below run at 100%, whatever size the editor opened at
    pump (200);   // the built-in pad loads on a background thread
    Mouse mouse;

    //==========================================================================
    std::printf ("Every parameter has a control\n");
    {
        juce::StringArray missing;
        for (auto* param : processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param); ranged != nullptr && find (ed, ranged->getParameterID()) == nullptr)
                missing.add (ranged->getParameterID());
        check (missing.isEmpty(), "a component with each parameter's ID" + (missing.isEmpty() ? juce::String() : " (missing: " + missing.joinIntoString (", ") + ")"));

        for (const auto* id : { "stepGrid", "sampleView", "shapeView", "adsrView", "filterAdsrView", "scopeView", "spectrumView",
                                "scopeButton", "spectrumButton", "ampEnvButton", "filterEnvButton",
                                "loadButton", "padButton", "helpButton", "helpOverlay", "meter", "lfoView", "bendBar", "wheelBar" })
            check (find (ed, id) != nullptr, juce::String ("a component with ID ") + id);
    }

    //==========================================================================
    std::printf ("\nEach source shows its own controls, and nothing that is shown overlaps\n");
    for (int source = 0; source < 4; ++source)
    {
        set (processor, "source", (float) source);
        juce::StringArray wrong;
        for (const auto& c : perSource)
            if (auto* comp = find (ed, c.id); comp == nullptr || showing (*comp, ed) != c.shown[source])
                wrong.add (c.id);
        check (wrong.isEmpty(), juce::String (sourceNames[source]) + ": the right controls shown and hidden"
                                    + (wrong.isEmpty() ? juce::String() : " (wrong: " + wrong.joinIntoString (", ") + ")"));

        // Both envelope tabs: the filter envelope's knobs sit where the amp envelope's do.
        for (int tab = 0; tab < 2; ++tab)
        {
            showEnvTab (ed, tab);
            const auto where = juce::String (sourceNames[source]) + (tab == 0 ? ", AMP tab" : ", FILTER tab");

            // Overlaps among the shown, named controls (the help page covers everything on purpose).
            juce::Array<juce::Component*> shown;
            std::function<void (juce::Component&)> collect = [&] (juce::Component& c)
            {
                for (auto* child : c.getChildren())
                {
                    if (child->getComponentID().isNotEmpty() && child->getComponentID() != "helpOverlay" && showing (*child, ed))
                        shown.add (child);
                    collect (*child);
                }
            };
            collect (ed);
            juce::StringArray overlaps;
            for (int i = 0; i < shown.size(); ++i)
                for (int j = i + 1; j < shown.size(); ++j)
                    if (ed.getLocalArea (shown[i], shown[i]->getLocalBounds()).intersects (ed.getLocalArea (shown[j], shown[j]->getLocalBounds())))
                        overlaps.add (shown[i]->getComponentID() + "/" + shown[j]->getComponentID());
            check (overlaps.isEmpty(), where + ": no two shown controls overlap"
                                           + (overlaps.isEmpty() ? juce::String() : " (" + overlaps.joinIntoString (", ") + ")"));

            juce::StringArray outside;
            for (auto* c : shown)
                if (! ed.getLocalBounds().contains (ed.getLocalArea (c, c->getLocalBounds())))
                    outside.add (c->getComponentID());
            check (outside.isEmpty(), where + ": every shown control lies inside the window"
                                          + (outside.isEmpty() ? juce::String() : " (" + outside.joinIntoString (", ") + ")"));
        }
        showEnvTab (ed, 0);
    }

    //==========================================================================
    std::printf ("\nThe window: opening size, limits, and legible labels\n");
    {
        const bool opensAt75  = openedWidth == baseWidth * 3 / 4 && openedHeight == baseHeight * 3 / 4;
        const bool opensAt100 = openedWidth == baseWidth && openedHeight == baseHeight;
        check (opensAt75 || opensAt100, "it opens at 75% or 100% of 1280 x 860 (opened at " + juce::String (openedWidth) + " x " + juce::String (openedHeight) + ")");

        const auto* constrainer = ed.getConstrainer();
        check (constrainer != nullptr && ed.isResizable()
                   && constrainer->getMinimumWidth() == baseWidth * 3 / 4 && constrainer->getMaximumWidth() == baseWidth * 3 / 2
                   && constrainer->getMinimumHeight() == baseHeight * 3 / 4 && constrainer->getMaximumHeight() == baseHeight * 3 / 2,
               "it resizes from 75% to 150%");

        // At 100% a font height is a pixel height: every label is 12 px or more.
        juce::StringArray small;
        std::function<void (juce::Component&)> scan = [&] (juce::Component& c)
        {
            if (auto* label = dynamic_cast<juce::Label*> (&c); label != nullptr && label->getFont().getHeight() < 12.0f)
                small.add (juce::String (label->getFont().getHeight(), 1) + " px at " + label->getBounds().toString());
            for (auto* child : c.getChildren())
                scan (*child);
        };
        scan (ed);
        check (small.isEmpty(), "every label is at least 12 px at 100%" + (small.isEmpty() ? juce::String() : " (small: " + small.joinIntoString ("; ") + ")"));
    }

    //==========================================================================
    std::printf ("\nKnobs and sliders: the wheel moves the parameter in one gesture\n");
    {
        for (auto* param : processor.getParameters())
        {
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param);
            auto* slider = dynamic_cast<juce::Slider*> (find (ed, ranged->getParameterID()));
            if (slider == nullptr)
                continue;

            // Show the slider: pick a source that shows it, and the envelope tab it is on.
            for (const auto& c : perSource)
                if (ranged->getParameterID() == c.id)
                    for (int source = 0; source < 4; ++source)
                        if (c.shown[source]) { set (processor, "source", (float) source); break; }
            showEnvTab (ed, isFilterEnvControl (ranged->getParameterID()) ? 1 : 0);

            const auto id = ranged->getParameterID();
            check (showing (*slider, ed), id + ": shown");
            const float before = valueOf (processor, id);
            const auto centre = slider->getLocalBounds().getCentre().toFloat();
            gestures.clear();
            mouse.wheel (*slider, centre, 0.25f);
            if (valueOf (processor, id) == before)   // already at its maximum
                mouse.wheel (*slider, centre, -0.25f);
            const float after = valueOf (processor, id);
            check (after != before && gestures.oneGesture (indexOf (processor, id)),
                   id + ": " + juce::String (before, 3) + " -> " + juce::String (after, 3) + ", inside begin/end gestures");
            set (processor, id, before);
        }
        showEnvTab (ed, 0);
    }

    //==========================================================================
    std::printf ("\nCombo boxes and buttons write their parameter and follow it\n");
    set (processor, "source", 0.0f);
    for (const auto* id : { "waveform", "source", "smpLoop", "seqRate", "seqMode", "lfoShape", "lfoDivision" })
    {
        auto* combo = dynamic_cast<juce::ComboBox*> (find (ed, id));
        if (combo == nullptr) { check (false, juce::String (id) + " is a combo box"); continue; }

        const float before = valueOf (processor, id);
        const int last = combo->getNumItems() - 1;
        gestures.clear();
        combo->setSelectedItemIndex (last, juce::sendNotificationSync);
        pump();
        check (juce::roundToInt (valueOf (processor, id)) == last && gestures.oneGesture (indexOf (processor, id)),
               juce::String (id) + ": choosing \"" + combo->getItemText (last) + "\" sets the parameter, in one gesture");

        set (processor, id, 0.0f);
        check (combo->getSelectedItemIndex() == 0, juce::String (id) + ": the box follows the parameter");
        set (processor, id, before);
    }
    for (const auto* id : { "filterEnabled", "seqOn", "seqSync", "lfoSync" })
    {
        auto* button = dynamic_cast<juce::Button*> (find (ed, id));
        if (button == nullptr) { check (false, juce::String (id) + " is a button"); continue; }

        const bool before = valueOf (processor, id) >= 0.5f;
        gestures.clear();
        button->triggerClick();
        pump();
        check ((valueOf (processor, id) >= 0.5f) != before && gestures.oneGesture (indexOf (processor, id)),
               juce::String (id) + ": a click flips it, in one gesture");

        set (processor, id, before ? 1.0f : 0.0f);
        check (button->getToggleState() == before, juce::String (id) + ": the button follows the parameter");
    }

    //==========================================================================
    std::printf ("\nVoice lights: a click sets how many voices can play\n");
    if (auto* leds = find (ed, "voiceCount"); leds != nullptr && parameter (processor, "voiceCount") != nullptr)
    {
        const auto light = [leds] (int n) { return juce::Point<float> ((float) leds->getWidth() * ((float) n - 0.5f) / 16.0f, (float) leds->getHeight() * 0.5f); };
        gestures.clear();
        mouse.click (*leds, light (4));
        pump();
        check (juce::roundToInt (valueOf (processor, "voiceCount")) == 4 && gestures.oneGesture (indexOf (processor, "voiceCount")),
               "clicking light 4 sets Voices to 4, in one gesture");
        check (labelShows (ed, "4 VOICES"), "the readout says 4 VOICES");
        mouse.click (*leds, light (1));
        pump();
        check (juce::roundToInt (valueOf (processor, "voiceCount")) == 1 && labelShows (ed, "MONO"), "light 1 sets Voices to 1, and the readout says MONO");
        mouse.click (*leds, light (16));
        pump();
        check (juce::roundToInt (valueOf (processor, "voiceCount")) == 16 && labelShows (ed, "16 VOICES"), "light 16 sets Voices back to 16");
    }
    else check (false, "the voice lights (ID voiceCount) and the voiceCount parameter found");

    //==========================================================================
    std::printf ("\nThe right-hand column: OUTPUT and MODULATION\n");
    {
        check (rightColumn.contains (outputPanel) && rightColumn.contains (modPanel) && ! outputPanel.intersects (modPanel),
               "OUTPUT and MODULATION split the right-hand column without overlapping");

        const auto inside = [&ed] (const char* const* ids, size_t count, juce::Rectangle<int> panel)
        {
            juce::StringArray wrong;
            for (size_t i = 0; i < count; ++i)
                if (auto* c = find (ed, ids[i]); c == nullptr || ! panel.reduced (8).contains (boundsIn (ed, *c)))
                    wrong.add (ids[i]);
            return wrong;
        };
        const auto outWrong = inside (outputControls, std::size (outputControls), outputPanel);
        check (outWrong.isEmpty(), "GAIN, WIDTH and the meter lie inside OUTPUT" + (outWrong.isEmpty() ? juce::String() : " (not: " + outWrong.joinIntoString (", ") + ")"));
        const auto modWrong = inside (modControls, std::size (modControls), modPanel);
        check (modWrong.isEmpty(), "every MODULATION control lies inside its panel" + (modWrong.isEmpty() ? juce::String() : " (not: " + modWrong.joinIntoString (", ") + ")"));
        if (auto* meter = find (ed, "meter"))
            check (meter->getHeight() >= 140, "the meter is still a usable height (" + juce::String (meter->getHeight()) + " px)");

        // GAIN is a vertical fader beside the meter, like a mixer channel strip.
        auto* fader = dynamic_cast<juce::Slider*> (find (ed, "gain"));
        auto* meter = find (ed, "meter");
        if (fader != nullptr && meter != nullptr)
        {
            const auto f = boundsIn (ed, *fader), mt = boundsIn (ed, *meter);
            check (fader->getSliderStyle() == juce::Slider::LinearVertical && f.getHeight() >= 120,
                   "GAIN is a vertical fader (" + juce::String (f.getHeight()) + " px of travel)");
            check (f.getRight() <= mt.getX() && f.getY() < mt.getBottom() && mt.getY() < f.getBottom(),
                   "the fader stands beside the meter, to its left");
            auto* gainParam = parameter (processor, "gain");
            check (fader->isDoubleClickReturnEnabled()
                       && std::abs (fader->getDoubleClickReturnValue() - gainParam->convertFrom0to1 (gainParam->getDefaultValue())) < 1.0e-4,
                   "a double-click returns the fader to its default (0 dB)");
            check (labelShows (ed, "0.0 dB"), "its readout shows 0.0 dB");
        }
        else check (false, "the gain fader and the meter found");

        // Both SYNC states: the rate in Hz or the note length, never both, and nothing overlaps.
        auto* rate = find (ed, "lfoRate");
        auto* division = find (ed, "lfoDivision");
        for (const bool sync : { false, true })
        {
            set (processor, "lfoSync", sync ? 1.0f : 0.0f);
            const auto where = juce::String (sync ? "SYNC on" : "SYNC off");
            check (rate != nullptr && division != nullptr && showing (*rate, ed) == ! sync && showing (*division, ed) == sync,
                   where + (sync ? ": the note length is shown instead of the rate" : ": the rate in Hz is shown, not the note length"));

            juce::StringArray overlaps;
            for (size_t i = 0; i < std::size (modControls); ++i)
                for (size_t j = i + 1; j < std::size (modControls); ++j)
                {
                    auto* a = find (ed, modControls[i]);
                    auto* b = find (ed, modControls[j]);
                    if (a != nullptr && b != nullptr && showing (*a, ed) && showing (*b, ed) && boundsIn (ed, *a).intersects (boundsIn (ed, *b)))
                        overlaps.add (juce::String (modControls[i]) + "/" + modControls[j]);
                }
            check (overlaps.isEmpty(), where + ": no two MODULATION controls overlap" + (overlaps.isEmpty() ? juce::String() : " (" + overlaps.joinIntoString (", ") + ")"));

            if (sync)
            {
                set (processor, "lfoPitch", 2.0f);
                set (processor, "lfoCutoff", 1.5f);
                set (processor, "lfoAmp", 40.0f);
                set (processor, "lfoShape", 1.0f);
                pump (100);
                saveSnapshot (ed.createComponentSnapshot (modPanel, true, 1.0f), "modulation-panel-sync");
            }
        }
        set (processor, "lfoSync", 0.0f);
        pump (100);

        // Readouts, in the panel's units (depths still set from the SYNC-on pass above).
        check (labelShows (ed, "+2.00 st"), "PITCH reads +2.00 st");
        check (labelShows (ed, "+1.50 oct"), "CUTOFF reads +1.50 oct");
        check (labelShows (ed, "40%"), "AMP reads 40%");
        check (labelShows (ed, juce::String (juce::CharPointer_UTF8 ("\xc2\xb1")) + "2 st"), "BEND reads +/-2 st (the default)");
        check (labelShows (ed, "30 ct"), "VIBRATO reads 30 ct (the default)");
        check (labelShows (ed, "1.00 Hz"), "RATE reads 1.00 Hz (the default) with SYNC off");

        const auto modImage = ed.createComponentSnapshot (modPanel, true, 1.0f);
        saveSnapshot (modImage, "modulation-panel");
        check (greenPixels (modImage) > 500, "the MODULATION panel is drawn in its green");

        // The green band under CUTOFF: only while LFO CUTOFF is not 0.
        const juce::Rectangle<int> filterPanel (770, 356, 182, 274);
        const juce::Rectangle<int> band (780, 438, 162, 6);
        auto withBand = ed.createComponentSnapshot (band, true, 1.0f);
        saveSnapshot (ed.createComponentSnapshot (filterPanel, true, 1.0f), "filter-panel-lfo");
        set (processor, "lfoCutoff", 0.0f);
        pump (100);
        const auto withoutBand = ed.createComponentSnapshot (band, true, 1.0f);
        check (greenPixels (withBand) > 20 && greenPixels (withoutBand) == 0,
               "the LFO band under CUTOFF shows while LFO CUTOFF is 1.5 oct (" + juce::String (greenPixels (withBand))
                   + " green pixels) and not at 0 (" + juce::String (greenPixels (withoutBand)) + ")");

        for (const auto* id : { "lfoPitch", "lfoAmp", "lfoShape" })
            set (processor, id, parameter (processor, id)->convertFrom0to1 (parameter (processor, id)->getDefaultValue()));
    }

    //==========================================================================
    std::printf ("\nStep grid\n");
    if (auto* grid = find (ed, "stepGrid"))
    {
        const auto cell = [grid] (int step)
        {
            const float w = (float) grid->getWidth() / (float) StepSequencer::numSteps;
            return juce::Point<float> (w * ((float) step + 0.5f), (float) grid->getHeight() * 0.5f);
        };
        const auto step3 = processor.getSequencerStep (3);
        mouse.click (*grid, cell (3));
        check (processor.getSequencerStep (3).on != step3.on, "click toggles step 4");

        const auto step5 = processor.getSequencerStep (5);
        mouse.down (*grid, cell (5));
        mouse.drag (*grid, cell (5).translated (0.0f, -30.0f));
        mouse.up (*grid, cell (5).translated (0.0f, -30.0f), true);
        check (processor.getSequencerStep (5).note == step5.note + 5, "dragging up 30 px raises step 6 by 5 semitones");
        check (processor.getSequencerStep (5).on == step5.on, "a drag does not toggle the step");

        const int noteAfter = processor.getSequencerStep (5).note;
        mouse.wheel (*grid, cell (5), 1.0f);
        check (processor.getSequencerStep (5).note == noteAfter + 1, "the wheel raises the note by a semitone");

        const int velocity = processor.getSequencerStep (7).velocity;
        mouse.down (*grid, cell (7), juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
        mouse.drag (*grid, cell (7).translated (0.0f, 10.0f), juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
        mouse.up (*grid, cell (7).translated (0.0f, 10.0f), true);
        check (processor.getSequencerStep (7).velocity == juce::jlimit (1, 127, velocity - 10), "shift-dragging down 10 px lowers the velocity by 10");
    }
    else check (false, "step grid found");

    //==========================================================================
    std::printf ("\nCHOP strip (Oscillator)\n");
    set (processor, "source", 0.0f);
    set (processor, "chopComb", 1.0f);
    if (auto* shape = find (ed, "shapeView"))
    {
        const auto at = [shape] (int step)
        {
            const auto inner = shape->getLocalBounds().toFloat().reduced (4.0f, 3.0f);
            return juce::Point<float> (inner.getX() + inner.getWidth() * ((float) step + 0.5f) / 16.0f, inner.getCentreY());
        };
        const auto bit = [&processor] (int step) { return ((processor.getChopPattern() >> step) & 1u) != 0; };

        const bool before = bit (5);
        mouse.click (*shape, at (5));
        check (bit (5) != before, "click toggles step 6");
        mouse.click (*shape, at (5));
        check (bit (5) == before, "a second click toggles it back");

        const bool painted = ! bit (8);
        mouse.down (*shape, at (8));
        for (int s = 9; s <= 11; ++s)
            mouse.drag (*shape, at (s));
        mouse.up (*shape, at (11), true);
        check (bit (8) == painted && bit (9) == painted && bit (10) == painted && bit (11) == painted,
               "a drag from step 9 to 12 gives all four the clicked step's new state");
    }
    else check (false, "shape view found");

    //==========================================================================
    std::printf ("\nSample view\n");
    set (processor, "source", 1.0f);
    set (processor, "smpLoop", 1.0f);   // Loop: the brackets are shown
    if (auto* view = find (ed, "sampleView"))
    {
        const auto inner = view->getLocalBounds().toFloat().reduced (4.0f, 3.0f);
        const auto at = [inner] (float fraction) { return juce::Point<float> (inner.getX() + inner.getWidth() * fraction, inner.getCentreY()); };

        set (processor, "smpLoopStart", 0.2f);
        set (processor, "smpLoopEnd", 1.0f);
        gestures.clear();
        mouse.click (*view, at (0.5f));
        check (std::abs (valueOf (processor, "smpStart") - 0.5f) < 0.01f && gestures.oneGesture (indexOf (processor, "smpStart")),
               "click away from the brackets sets START there, in one gesture");

        gestures.clear();
        mouse.down (*view, at (1.0f));
        mouse.drag (*view, at (0.9f));
        mouse.drag (*view, at (0.8f));
        mouse.up (*view, at (0.8f), true);
        check (std::abs (valueOf (processor, "smpLoopEnd") - 0.8f) < 0.01f && gestures.oneGesture (indexOf (processor, "smpLoopEnd")),
               "dragging the L.END bracket from 100% to 80% moves it, in one gesture");

        mouse.down (*view, at (0.2f));
        mouse.drag (*view, at (0.3f));
        mouse.up (*view, at (0.3f), true);
        check (std::abs (valueOf (processor, "smpLoopStart") - 0.3f) < 0.01f, "dragging the L.START bracket moves it");

        set (processor, "source", 2.0f);
        gestures.clear();
        mouse.down (*view, at (0.1f));
        mouse.drag (*view, at (0.7f));
        mouse.up (*view, at (0.7f), true);
        check (std::abs (valueOf (processor, "grPos") - 0.7f) < 0.01f && gestures.oneGesture (indexOf (processor, "grPos")),
               "Granular: a drag moves POS, in one gesture");

        // L.END is at 80% and not drawn in Granular: a drag that starts on it is still a POS drag.
        mouse.down (*view, at (0.8f));
        mouse.drag (*view, at (0.6f));
        mouse.up (*view, at (0.6f), true);
        check (std::abs (valueOf (processor, "grPos") - 0.6f) < 0.01f && std::abs (valueOf (processor, "smpLoopEnd") - 0.8f) < 0.01f,
               "Granular: a drag that starts on the hidden L.END bracket moves POS and leaves L.END alone");
    }
    else check (false, "sample view found");

    //==========================================================================
    std::printf ("\nSample loading and replaced samples\n");
    {
        const auto wav = writeTestWav (20.0);
        set (processor, "seqOn", 0.0f);
        set (processor, "source", 1.0f);

        // A sample replaced while notes still play it is kept until they end, then freed
        // by the editor's timer (nothing else runs once the notes have stopped).
        const float releaseBefore = valueOf (processor, "release");
        set (processor, "release", 0.05f);   // earlier sections left it wherever their gestures ended
        juce::String error;
        check (processor.loadSampleFile (wav, error), "the test file loads");
        play (processor, true);
        processor.useDefaultSample();
        check (processor.getNumRetiredSamples() == 1, "a sample replaced while notes play it is kept for them");
        for (int i = 0; i < 8; ++i)
            play (processor, false);   // released: the voices fade out and let go of it
        pump (150);
        check (processor.getNumRetiredSamples() == 0, "once those notes have ended, the replaced sample is freed");
        set (processor, "release", releaseBefore);

        // A background load overtaken by PAD must not leave the status on "loading...".
        processor.loadSampleFileAsync (wav);
        processor.useDefaultSample();
        for (int i = 0; i < 100 && processor.getSampleStatus().loading; ++i)
            pump (50);
        check (! processor.getSampleStatus().loading, "a load overtaken by PAD does not leave the status on loading...");
        check (processor.getSampleStatus().name == "Default pad", "and the built-in pad is what plays");

        // The same for two loads in a row: the status clears when the second one is in.
        processor.loadSampleFileAsync (wav);
        processor.loadSampleFileAsync (wav);
        check (processor.getSampleStatus().loading, "while a file loads in the background the status says so");
        for (int i = 0; i < 100 && processor.getSampleStatus().loading; ++i)
            pump (50);
        check (! processor.getSampleStatus().loading && processor.getSampleStatus().name == wav.getFileName(),
               "two loads in a row end with the file loaded and the status clear");

        processor.useDefaultSample();
        wav.deleteFile();
    }

    //==========================================================================
    std::printf ("\nEnvelope view: the four points\n");
    if (auto* adsr = dynamic_cast<AdsrView*> (find (ed, "adsrView")))
    {
        struct Point { int handle; const char* id; };
        for (const auto& point : { Point { AdsrView::attackHandle, "attack" }, Point { AdsrView::decayHandle, "decay" },
                                   Point { AdsrView::releaseHandle, "release" } })
        {
            const float before = valueOf (processor, point.id);
            auto at = adsr->handlePosition (point.handle);
            gestures.clear();
            mouse.down (*adsr, at);
            mouse.drag (*adsr, at.translated (40.0f, 0.0f));
            mouse.drag (*adsr, at.translated (80.0f, 0.0f));
            mouse.up (*adsr, at.translated (80.0f, 0.0f), true);
            pump();
            const float longer = valueOf (processor, point.id);
            check (longer > before && gestures.oneGesture (indexOf (processor, point.id)),
                   juce::String (point.id) + ": dragging its point right lengthens it (" + juce::String (before, 3) + " -> " + juce::String (longer, 3) + " s), in one gesture");

            at = adsr->handlePosition (point.handle);
            mouse.down (*adsr, at);
            mouse.drag (*adsr, at.translated (-50.0f, 0.0f));
            mouse.up (*adsr, at.translated (-50.0f, 0.0f), true);
            pump();
            check (valueOf (processor, point.id) < longer, juce::String (point.id) + ": dragging it left shortens it");

            at = adsr->handlePosition (point.handle);
            gestures.clear();
            adsr->mouseDoubleClick (mouse.event (*adsr, at, juce::ModifierKeys::leftButtonModifier, false));
            pump();
            auto* param = parameter (processor, point.id);
            check (std::abs (param->getValue() - param->getDefaultValue()) < 1.0e-6f && gestures.oneGesture (indexOf (processor, point.id)),
                   juce::String (point.id) + ": a double-click puts it back to its default, in one gesture");
            set (processor, point.id, before);
        }

        // Sustain follows the pointer's height: a quarter of the way down the plot is 75%.
        {
            const float before = valueOf (processor, "sustain");
            const auto at = adsr->handlePosition (AdsrView::sustainHandle);
            const float top = adsr->handlePosition (AdsrView::attackHandle).y, bottom = adsr->handlePosition (AdsrView::releaseHandle).y;
            const juce::Point<float> target (at.x, top + 0.25f * (bottom - top));
            gestures.clear();
            mouse.down (*adsr, at);
            mouse.drag (*adsr, target);
            mouse.up (*adsr, target, true);
            pump();
            check (std::abs (valueOf (processor, "sustain") - 0.75f) < 0.011f && gestures.oneGesture (indexOf (processor, "sustain")),
                   "sustain: dragging its point to a quarter of the way down sets " + juce::String (valueOf (processor, "sustain"), 2) + ", in one gesture");
            set (processor, "sustain", before);
        }

        // A press away from every point does nothing.
        {
            const float a = valueOf (processor, "attack"), d = valueOf (processor, "decay"), s = valueOf (processor, "sustain"), r = valueOf (processor, "release");
            const auto empty = adsr->handlePosition (AdsrView::releaseHandle).translated (-30.0f, -60.0f);
            gestures.clear();
            mouse.down (*adsr, empty);
            mouse.drag (*adsr, empty.translated (40.0f, 20.0f));
            mouse.up (*adsr, empty.translated (40.0f, 20.0f), true);
            pump();
            check (valueOf (processor, "attack") == a && valueOf (processor, "decay") == d && valueOf (processor, "sustain") == s
                       && valueOf (processor, "release") == r && gestures.begins.empty(),
                   "a drag that starts away from the points changes nothing");
        }
    }
    else check (false, "envelope view found");

    //==========================================================================
    std::printf ("\nEnvelope tabs: AMP and FILTER\n");
    {
        check (allShowing (ed, ampEnvControls, std::size (ampEnvControls), true)
                   && allShowing (ed, filterEnvControls, std::size (filterEnvControls), false),
               "AMP (the default) shows the amp envelope's view and knobs, and not the filter envelope's");
        showEnvTab (ed, 1);
        check (allShowing (ed, filterEnvControls, std::size (filterEnvControls), true)
                   && allShowing (ed, ampEnvControls, std::size (ampEnvControls), false),
               "FILTER shows the filter envelope's view and knobs and hides the amp envelope's");
        showEnvTab (ed, 0);
        check (allShowing (ed, ampEnvControls, std::size (ampEnvControls), true)
                   && allShowing (ed, filterEnvControls, std::size (filterEnvControls), false),
               "AMP shows the amp envelope again");

        // Each filter-envelope knob and point moves its own parameter and nothing else, in one gesture.
        showEnvTab (ed, 1);
        const auto values = [&processor]
        {
            std::vector<float> v;
            for (auto* param : processor.getParameters())
                v.push_back (param->getValue());
            return v;
        };
        const auto onlyChanged = [&] (const std::vector<float>& before, int index)
        {
            const auto after = values();
            for (size_t i = 0; i < after.size(); ++i)
                if ((after[i] != before[i]) != ((int) i == index))
                    return false;
            return gestures.oneGesture (index) && gestures.begins.size() == 1;
        };
        for (const auto* id : { "filterAttack", "filterDecay", "filterSustain", "filterRelease" })
        {
            auto* knob = dynamic_cast<juce::Slider*> (find (ed, id));
            if (knob == nullptr) { check (false, juce::String (id) + " is a knob"); continue; }
            const float start = valueOf (processor, id);

            // A rotary knob follows the pointer's angle round its centre: from 12 o'clock to 3 o'clock.
            const auto centre = knob->getLocalBounds().getCentre().toFloat();
            auto before = values();
            gestures.clear();
            mouse.down (*knob, centre.translated (0.0f, -18.0f));
            mouse.drag (*knob, centre.translated (12.0f, -12.0f));
            mouse.drag (*knob, centre.translated (18.0f, 0.0f));
            mouse.up (*knob, centre.translated (18.0f, 0.0f), true);
            pump();
            check (onlyChanged (before, indexOf (processor, id)),
                   juce::String (id) + ": dragging the knob moves it (" + juce::String (start, 3) + " -> "
                       + juce::String (valueOf (processor, id), 3) + ") and nothing else, in one gesture");

            before = values();
            gestures.clear();
            mouse.wheel (*knob, centre, -0.25f);
            pump();
            check (onlyChanged (before, indexOf (processor, id)), juce::String (id) + ": the wheel moves it and nothing else, in one gesture");
            set (processor, id, start);
        }

        if (auto* view = dynamic_cast<AdsrView*> (find (ed, "filterAdsrView")))
        {
            const float start = valueOf (processor, "filterDecay");
            const auto at = view->handlePosition (AdsrView::decayHandle);
            const auto before = values();
            gestures.clear();
            mouse.down (*view, at);
            mouse.drag (*view, at.translated (40.0f, 0.0f));
            mouse.up (*view, at.translated (40.0f, 0.0f), true);
            pump();
            check (valueOf (processor, "filterDecay") > start && onlyChanged (before, indexOf (processor, "filterDecay")),
                   "the filter envelope's D point lengthens Filter Decay and nothing else, in one gesture");

            gestures.clear();
            view->mouseDoubleClick (mouse.event (*view, view->handlePosition (AdsrView::decayHandle), juce::ModifierKeys::leftButtonModifier, false));
            pump();
            auto* param = parameter (processor, "filterDecay");
            check (std::abs (param->getValue() - param->getDefaultValue()) < 1.0e-6f && gestures.oneGesture (indexOf (processor, "filterDecay")),
                   "a double-click on it puts Filter Decay back to its default, in one gesture");
            set (processor, "filterDecay", start);
        }
        else check (false, "filter envelope view found");
        showEnvTab (ed, 0);
    }

    //==========================================================================
    std::printf ("\nView buttons and help page\n");
    {
        auto* scope = find (ed, "scopeView");
        auto* spectrum = find (ed, "spectrumView");
        auto* scopeButton = dynamic_cast<juce::Button*> (find (ed, "scopeButton"));
        auto* spectrumButton = dynamic_cast<juce::Button*> (find (ed, "spectrumButton"));
        auto* help = find (ed, "helpOverlay");
        auto* helpButton = dynamic_cast<juce::Button*> (find (ed, "helpButton"));

        if (scope && spectrum && scopeButton && spectrumButton && help && helpButton)
        {
            spectrumButton->triggerClick();
            pump();
            check (showing (*spectrum, ed) && ! showing (*scope, ed), "SPECTRUM shows the spectrum instead of the scope");
            scopeButton->triggerClick();
            pump();
            check (showing (*scope, ed) && ! showing (*spectrum, ed), "SCOPE shows the scope again");

            helpButton->triggerClick();
            pump();
            check (showing (*help, ed), "? opens the help page");
            helpButton->triggerClick();
            pump();
            check (! showing (*help, ed), "? again closes it");

            // Escape from a knob: the host may leave focus there. Deliver the key the way a
            // window does, to the component and then up through its parents until one takes it.
            auto sendKey = [] (juce::Component* from, const juce::KeyPress& key)
            {
                for (auto* c = from; c != nullptr; c = c->getParentComponent())
                    if (c->keyPressed (key))
                        return true;
                return false;
            };
            const juce::KeyPress escape (juce::KeyPress::escapeKey);
            auto* knob = find (ed, "attack");
            helpButton->triggerClick();
            pump();
            const bool taken = knob != nullptr && sendKey (knob, escape);
            pump();
            check (taken && ! showing (*help, ed) && ! helpButton->getToggleState(),
                   "Esc on a knob closes the help page and releases ?");
            check (knob != nullptr && ! sendKey (knob, escape), "Esc with the help page closed is left for the host");
        }
        else check (false, "view buttons, views and help page found");
    }

    //==========================================================================
    std::printf ("\nEvery view paints, at every size, for every source\n");
    {
        auto* spectrumButton = dynamic_cast<juce::Button*> (find (ed, "spectrumButton"));
        auto* scopeButton = dynamic_cast<juce::Button*> (find (ed, "scopeButton"));
        auto* helpButton = dynamic_cast<juce::Button*> (find (ed, "helpButton"));
        int painted = 0, blank = 0;
        juce::StringArray problems;

        for (const float scale : { 0.75f, 1.0f, 1.5f })
        {
            ed.setSize (juce::roundToInt (baseWidth * scale), juce::roundToInt (baseHeight * scale));
            for (int source = 0; source < 4; ++source)
            {
                set (processor, "source", (float) source);
                play (processor, true);
                for (int view = 0; view < 2; ++view)
                {
                    (view == 0 ? scopeButton : spectrumButton)->triggerClick();
                    for (const bool withHelp : { false, true })
                    {
                        if (helpButton->getToggleState() != withHelp)
                            helpButton->triggerClick();
                        pump (70);   // two timer ticks: views refresh from the voices

                        // The FILTER envelope tab too, once per size and source (with the scope, without help).
                        const int tabs = view == 0 && ! withHelp ? 2 : 1;
                        for (int tab = 0; tab < tabs; ++tab)
                        {
                            if (tab == 1)
                                showEnvTab (ed, 1);

                            const auto image = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
                            // Look at them: set NEONSYNTH_SNAPSHOT_DIR to a folder. (JUCE's call instead of std::getenv,
                            // which MSVC deprecates.)
                            if (const auto dir = juce::SystemStats::getEnvironmentVariable ("NEONSYNTH_SNAPSHOT_DIR", {}); dir.isNotEmpty() && image.isValid())
                                if (juce::File (dir).createDirectory().wasOk())
                                {
                                    // FileOutputStream appends to an existing file: start each image afresh.
                                    const auto file = juce::File (dir).getChildFile (juce::String (juce::roundToInt (scale * 100)) + "-" + sourceNames[source]
                                                                                     + (view == 0 ? "-scope" : "-spectrum") + (withHelp ? "-help" : "")
                                                                                     + (tab == 1 ? "-filterenv" : "") + ".png");
                                    file.deleteFile();
                                    if (juce::FileOutputStream out (file); out.openedOk())
                                        juce::PNGImageFormat().writeImageToStream (image, out);
                                }
                            ++painted;
                            if (! image.isValid() || image.getWidth() != ed.getWidth() || image.getHeight() != ed.getHeight())
                                problems.add (juce::String (scale) + "x " + sourceNames[source]);

                            // A drawn editor is far from one flat colour: sample a grid of pixels.
                            juce::Colour first = image.getPixelAt (0, 0);
                            bool varied = false;
                            for (int y = 0; y < image.getHeight() && ! varied; y += 17)
                                for (int x = 0; x < image.getWidth() && ! varied; x += 13)
                                    varied = image.getPixelAt (x, y) != first;
                            if (! varied)
                                ++blank;

                            if (tab == 1)
                                showEnvTab (ed, 0);
                        }
                    }
                }
                play (processor, false);
            }
        }
        if (helpButton->getToggleState())
            helpButton->triggerClick();

        check (problems.isEmpty(), juce::String (painted) + " snapshots (3 sizes x 4 sources x 2 views x help off/on, plus the FILTER envelope tab) painted at the window's size"
                                       + (problems.isEmpty() ? juce::String() : " (failed: " + problems.joinIntoString (", ") + ")"));
        check (blank == 0, "none of them is a blank image");
    }

    //==========================================================================
    std::printf ("\nThe envelope tab is kept with the plugin's state\n");
    {
        showEnvTab (ed, 1);
        processor.editorBeingDeleted (editor.get());   // what a host's wrapper does before deleting the editor
        editor.reset();
        editor.reset (processor.createEditorAndMakeActive());
        pump();
        auto* button = editor != nullptr ? dynamic_cast<juce::Button*> (find (*editor, "filterEnvButton")) : nullptr;
        check (editor != nullptr && button != nullptr && button->getToggleState()
                   && allShowing (*editor, filterEnvControls, std::size (filterEnvControls), true)
                   && allShowing (*editor, ampEnvControls, std::size (ampEnvControls), false),
               "a reopened editor shows the FILTER tab it was closed on");
        if (editor != nullptr)
            showEnvTab (*editor, 0);
    }

    processor.removeListener (&gestures);
    editor.reset();
    processor.releaseResources();

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
