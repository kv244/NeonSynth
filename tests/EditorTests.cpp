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
#include "plugin/NeonSynthProcessor.h"
#include "gui/ScopeView.h"
#include "gui/LevelMeter.h"
#include "gui/AdsrView.h"
#include "gui/NeonSynthEditor.h"
#include "gui/HarmonicsView.h"
#include "gui/SpectrumView.h"
#include <array>
#include <cmath>
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
    bool showing (juce::Component& c, const juce::Component& editor)
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
        int changesOnMessageThread = 0, changesElsewhere = 0;
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override
        {
            ++(juce::MessageManager::getInstance()->isThisTheMessageThread() ? changesOnMessageThread : changesElsewhere);
        }
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

    /** WCAG 2 contrast ratio between two opaque colours (1 to 21). */
    double contrast (juce::Colour a, juce::Colour b)
    {
        auto luminance = [] (juce::Colour c)
        {
            auto channel = [] (juce::uint8 v)
            {
                const double x = v / 255.0;
                return x <= 0.04045 ? x / 12.92 : std::pow ((x + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * channel (c.getRed()) + 0.7152 * channel (c.getGreen()) + 0.0722 * channel (c.getBlue());
        };
        const double la = luminance (a), lb = luminance (b);
        return (juce::jmax (la, lb) + 0.05) / (juce::jmin (la, lb) + 0.05);
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

//==============================================================================
// v1.12.0: the HARMONICS view and the SPECTRUM's aliasing marks (agent B).
namespace { namespace harmonicsTests
{
    bool near (float a, float b, float tolerance = 1.0e-4f)   { return std::abs (a - b) <= tolerance; }

    bool sameSpectrum (const HarmonicSpectrum& a, const HarmonicSpectrum& b)
    {
        for (size_t i = 0; i < (size_t) HarmonicSpectrum::numHarmonics; ++i)
            if (! near (a.levels[i], b.levels[i]) || ! near (a.phases[i], b.phases[i]))
                return false;
        return true;
    }

    /** Holds one note (or lets it go), rendering a few blocks so the voices report it. */
    void playNote (NeonSynthProcessor& p, int note, bool on)
    {
        juce::AudioBuffer<float> buffer (2, 512);
        for (int block = 0; block < 8; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent (on ? juce::MidiMessage::noteOn (1, note, (juce::uint8) 100) : juce::MidiMessage::noteOff (1, note), 0);
            buffer.clear();
            p.processBlock (buffer, midi);
        }
    }

    void run (juce::AudioProcessorEditor& ed, NeonSynthProcessor& processor, GestureLog& gestures, Mouse& mouse)
    {
        std::printf ("\nHARMONICS view (v1.12.0): bars, presets, ZERO PHASE, BUILD\n");
        ed.setSize (baseWidth, baseHeight);   // the checks below are at 100%
        auto* viewButton = dynamic_cast<juce::Button*> (find (ed, "harmonicsButton"));
        auto* scopeButton = dynamic_cast<juce::Button*> (find (ed, "scopeButton"));
        auto* view = dynamic_cast<HarmonicsView*> (find (ed, "harmonicsView"));
        if (viewButton == nullptr || scopeButton == nullptr || view == nullptr)
        {
            check (false, "the HARMONICS button and view found");
            return;
        }
        const char* ids[] = { "harmonicsSine", "harmonicsSaw", "harmonicsSquare", "harmonicsTriangle", "zeroPhase", "build",
                              "harmonics", "harmonicsReadout" };
        scopeButton->triggerClick();
        pump();
        check (allShowing (ed, ids, std::size (ids), false), "its controls are hidden while SCOPE is shown");
        viewButton->triggerClick();
        pump();
        check (allShowing (ed, ids, std::size (ids), true), "view 2 shows the presets, ZERO PHASE, BUILD, HARMONICS and the readout");

        juce::StringArray noTip, outside, overlaps;
        for (const auto* id : ids)
            if (auto* c = find (ed, id))
            {
                if (auto* tip = dynamic_cast<juce::SettableTooltipClient*> (c); tip != nullptr && tip->getTooltip().isEmpty() && juce::String (id) != "harmonicsReadout")
                    noTip.add (id);
                if (! view->getLocalBounds().contains (view->getLocalArea (c, c->getLocalBounds())))
                    outside.add (id);
                for (const auto* other : ids)
                    if (auto* o = find (ed, other); o != nullptr && o != c && juce::String (id) < juce::String (other)
                                                    && boundsIn (ed, *c).intersects (boundsIn (ed, *o)))
                        overlaps.add (juce::String (id) + "/" + other);
            }
        check (noTip.isEmpty(), "every control has a tooltip" + (noTip.isEmpty() ? juce::String() : " (none: " + noTip.joinIntoString (", ") + ")"));
        check (outside.isEmpty() && overlaps.isEmpty(), "they lie inside the view and do not overlap"
                                                            + (outside.isEmpty() && overlaps.isEmpty() ? juce::String() : " (" + outside.joinIntoString (", ") + overlaps.joinIntoString (", ") + ")"));

        juce::StringArray small;
        std::function<void (juce::Component&)> scan = [&] (juce::Component& c)
        {
            if (auto* label = dynamic_cast<juce::Label*> (&c); label != nullptr && label->getFont().getHeight() < 12.0f)
                small.add (juce::String (label->getFont().getHeight(), 1) + " px");
            for (auto* child : c.getChildren())
                scan (*child);
        };
        scan (*view);
        check (small.isEmpty(), "its labels are at least 12 px at 100%");

        const auto click = [&] (const char* id) { if (auto* b = dynamic_cast<juce::Button*> (find (ed, id))) b->triggerClick(); pump(); };

        // SQUARE (over a drawn cycle, which it drops), then the bars.
        processor.setCycleDrawing (FourierState::Drawing {}, HarmonicSpectrum::sine());
        click ("harmonicsSquare");
        check (sameSpectrum (processor.getHarmonicSpectrum(), HarmonicSpectrum::square()), "SQUARE sets the square series (levels and phases)");
        check (! processor.getCycleDrawing().has_value(), "and drops the drawn cycle, which no longer matches what plays");
        click ("harmonicsTriangle");
        check (sameSpectrum (processor.getHarmonicSpectrum(), HarmonicSpectrum::triangle()), "TRIANGLE sets the triangle series");

        const auto bar5 = view->barBounds (5);
        mouse.down (*view, { bar5.getCentreX(), bar5.getBottom() - 0.8f * bar5.getHeight() });
        mouse.up (*view, { bar5.getCentreX(), bar5.getBottom() - 0.8f * bar5.getHeight() }, false);
        check (near (processor.getHarmonicSpectrum().levels[4], 0.8f, 0.02f), "clicking bar 5 at 80% height sets harmonic 5 to 80% ("
                                                                                + juce::String (processor.getHarmonicSpectrum().levels[4], 3) + ")");
        check (near (processor.getHarmonicSpectrum().phases[4], HarmonicSpectrum::triangle().phases[4]), "and keeps its phase");

        // A quick stroke across bars 10..16 at half height: every bar on the way is set, none skipped.
        const auto b10 = view->barBounds (10), b16 = view->barBounds (16);
        const float half = b10.getBottom() - 0.5f * b10.getHeight();
        mouse.down (*view, { b10.getCentreX(), half });
        mouse.drag (*view, { b16.getCentreX(), half });
        mouse.up (*view, { b16.getCentreX(), half }, true);
        bool allHalf = true;
        for (int k = 10; k <= 16; ++k)
            allHalf = allHalf && near (processor.getHarmonicSpectrum().levels[(size_t) (k - 1)], 0.5f, 0.02f);
        check (allHalf, "a drag across bars 10 to 16 sets every one of them");

        view->mouseDoubleClick (mouse.event (*view, { bar5.getCentreX(), half }, {}, false));
        check (processor.getHarmonicSpectrum().levels[4] == 0.0f, "a double-click sets a bar to 0");

        const auto before = processor.getHarmonicSpectrum();
        click ("zeroPhase");
        const auto after = processor.getHarmonicSpectrum();
        bool zero = true, kept = true;
        for (size_t i = 0; i < (size_t) HarmonicSpectrum::numHarmonics; ++i)
        {
            zero = zero && after.phases[i] == 0.0f;
            kept = kept && after.levels[i] == before.levels[i];
        }
        check (zero && kept, "ZERO PHASE sets every phase to 0 and keeps the levels");

        // BUILD: HARMONICS steps up over time, as one gesture; a second click stops it.
        click ("harmonicsSaw");
        auto* readout = dynamic_cast<juce::Label*> (find (ed, "harmonicsReadout"));
        set (processor, "harmonics", 32.0f);
        check (readout != nullptr && readout->getText() == "Gibbs: 9% overshoot at the jump", "SAW with all 32: the readout tells of Gibbs's 9% overshoot");
        gestures.clear();
        click ("build");
        check (view->isBuilding() && juce::roundToInt (valueOf (processor, "harmonics")) == 1, "BUILD starts at 1 harmonic");
        pump (2700);   // about 3 a second
        const int midway = juce::roundToInt (valueOf (processor, "harmonics"));
        check (midway >= 4 && midway < 32, "and adds them over time (" + juce::String (midway) + " after 2.7 s)");
        check (readout != nullptr && readout->getText() == juce::String (midway) + " of 32", "the readout counts them (" + (readout != nullptr ? readout->getText() : juce::String()) + ")");
        saveSnapshot (ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f), "100-harmonics-build");
        click ("build");
        // Compare with the count at the click, not with `midway`: under a slow build
        // (ThreadSanitizer) the snapshot above can take longer than one BUILD step.
        const int stopped = juce::roundToInt (valueOf (processor, "harmonics"));
        pump (500);
        check (! view->isBuilding() && stopped >= midway && juce::roundToInt (valueOf (processor, "harmonics")) == stopped,
               "a second click stops it where it is (" + juce::String (stopped) + ")");
        check (gestures.oneGesture (indexOf (processor, "harmonics")), "BUILD is one host gesture, begun and ended");
        set (processor, "harmonics", 32.0f);

        //======================================================================
        std::printf ("\nSPECTRUM (v1.12.0): red marks where NAIVE's harmonics fold back\n");
        check (SpectrumView::foldedFrequency (25000.0, 48000.0) == 23000.0 && SpectrumView::foldedFrequency (50000.0, 48000.0) == 2000.0
                   && SpectrumView::foldedFrequency (10000.0, 48000.0) == 10000.0,
               "folding at 48 kHz: 25 kHz lands at 23 kHz, 50 kHz at 2 kHz, 10 kHz stays");

        SpectrumView::AliasSource saw;
        saw.shown = true;
        saw.waveform = PolyBlepOscillator::sawtooth;
        saw.fundamentalHz = 5000.0f;
        const auto sawMarks = SpectrumView::aliasMarks (saw, 48000.0);
        check (sawMarks.size() == (size_t) SpectrumView::maxAliasMarks && sawMarks[0].harmonic == 5 && sawMarks[0].hz == 23000.0
                   && near (sawMarks[0].relativeLevel, 0.2f) && sawMarks[1].harmonic == 6 && sawMarks[1].hz == 18000.0,
               "saw at 5 kHz: harmonic 5 (25 kHz) lands at 23 kHz at 1/5 the level, 6 at 18 kHz; 48 marks");
        auto square = saw;
        square.waveform = PolyBlepOscillator::square;
        const auto squareMarks = SpectrumView::aliasMarks (square, 48000.0);
        bool odd = squareMarks.size() == (size_t) SpectrumView::maxAliasMarks;
        for (const auto& m : squareMarks)
            odd = odd && (m.harmonic % 2) == 1;
        check (odd && squareMarks[0].harmonic == 5 && squareMarks[1].harmonic == 7, "square: odd harmonics only");
        auto triangle = square;
        triangle.waveform = PolyBlepOscillator::triangle;
        const auto triangleMarks = SpectrumView::aliasMarks (triangle, 48000.0);
        check (! triangleMarks.empty() && near (triangleMarks[0].relativeLevel, 1.0f / 25.0f), "triangle: 1/k squared");
        auto sine = saw;
        sine.waveform = PolyBlepOscillator::sine;
        auto hidden = saw;
        hidden.shown = false;
        check (SpectrumView::aliasMarks (sine, 48000.0).empty() && SpectrumView::aliasMarks (hidden, 48000.0).empty(),
               "a sine has none, and nothing is marked unless shown");
        auto custom = saw;
        custom.custom = true;
        custom.harmonics = 8;
        custom.levels.fill (0.5f);
        custom.levels[5] = 0.0f;   // harmonic 6 silent
        const auto customMarks = SpectrumView::aliasMarks (custom, 48000.0);
        check (customMarks.size() == 3 && customMarks[0].harmonic == 5 && customMarks[1].harmonic == 7 && customMarks[2].harmonic == 8
                   && near (customMarks[0].relativeLevel, 1.0f),
               "CUSTOM: harmonics up to HARMONICS with a level, at their level");

        // In the editor: NAIVE on, a high saw note.
        auto* spectrumButton = dynamic_cast<juce::Button*> (find (ed, "spectrumButton"));
        auto* spectrum = dynamic_cast<SpectrumView*> (find (ed, "spectrumView"));
        if (spectrumButton == nullptr || spectrum == nullptr)
        {
            check (false, "the SPECTRUM button and view found");
            return;
        }
        set (processor, "source", 0.0f);
        set (processor, "waveform", (float) PolyBlepOscillator::sawtooth);
        set (processor, "naive", 1.0f);
        spectrumButton->triggerClick();
        {
            // Earlier checks leave notes sounding: stop them and let them finish their release first,
            // since the marks follow the lowest sounding voice.
            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer allOff, none;
            allOff.addEvent (juce::MidiMessage::allNotesOff (1), 0);
            processor.processBlock (buffer, allOff);
            const auto anyActive = [&processor]
            {
                for (const auto& v : processor.getVoiceViews())
                    if (v.active.load())
                        return true;
                return false;
            };
            for (int block = 0; block < 4000 && anyActive(); ++block)
                processor.processBlock (buffer, none);
        }
        playNote (processor, 96, true);
        pump (70);
        playNote (processor, 96, true);   // still held: more blocks, so the analyser has the note
        pump (70);
        check (! spectrum->getAliasMarks().empty() && spectrum->getAliasMarks()[0].harmonic == 12,
               "NAIVE on and C7 (2093 Hz) playing: marks from harmonic 12 up (first: " + juce::String (spectrum->getAliasMarks().empty() ? 0 : spectrum->getAliasMarks()[0].harmonic) + ")");
        saveSnapshot (ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f), "100-spectrum-aliasing");
        set (processor, "naive", 0.0f);
        pump (70);
        check (spectrum->getAliasMarks().empty(), "NAIVE off: no marks");
        playNote (processor, 96, false);
        set (processor, "waveform", 0.0f);
        scopeButton->triggerClick();
        pump();
    }
} }

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
                                "loadButton", "padButton", "helpButton", "helpOverlay", "meter", "lfoView", "bendBar", "wheelBar",
                                "scanlinesButton", "scanlines" })
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
                    if (child->getComponentID().isNotEmpty() && child->getComponentID() != "helpOverlay"
                        && child->getComponentID() != "scanlines" && showing (*child, ed))
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
            if (auto* views = dynamic_cast<juce::Button*> (find (ed, ranged->getParameterID() == "harmonics" ? "harmonicsButton" : "scopeButton")))
                views->setToggleState (true, juce::sendNotificationSync);   // v1.12.0: HARMONICS lives in its own view

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

        // v1.11.0: the pattern row and the grid's new gestures.
        const float w = (float) grid->getWidth() / (float) StepSequencer::numSteps;
        const float h = (float) grid->getHeight();
        const auto at = [w] (int step, float x, float y) { return juce::Point<float> (w * (float) step + x * w, y); };

        const int vel9 = processor.getSequencerStep (9).velocity;
        mouse.down (*grid, at (9, 0.5f, h - 20.0f));
        mouse.drag (*grid, at (9, 0.5f, h - 32.0f));
        mouse.up (*grid, at (9, 0.5f, h - 32.0f), true);
        check (processor.getSequencerStep (9).velocity == juce::jlimit (1, 127, vel9 + 12), "dragging the velocity bar up 12 px raises the velocity by 12 (no shift)");

        const auto step10 = processor.getSequencerStep (10);
        mouse.click (*grid, at (10, 0.3f, h - 5.0f));
        check (processor.getSequencerStep (10).tie && processor.getSequencerStep (10).on == step10.on, "TIE marker toggles the tie only");
        mouse.click (*grid, at (10, 0.7f, h - 5.0f));
        check (processor.getSequencerStep (10).slide, "SLD marker toggles the slide");

        auto* undo = dynamic_cast<juce::Button*> (find (ed, "seqUndo"));
        if (undo != nullptr) undo->triggerClick();
        pump();
        check (processor.getSequencerStep (10).tie && ! processor.getSequencerStep (10).slide, "UNDO takes the slide back");
        ed.keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0));
        check (! processor.getSequencerStep (10).tie, "Ctrl+Z takes the tie back");

        set (processor, "seqLength", 4.0f);
        pump (100);
        check (dynamic_cast<StepGrid*> (grid)->getLength() == 4, "the grid follows LENGTH");
        set (processor, "seqLength", 16.0f);

        set (processor, "seqScale", 1.0f);   // C major
        processor.setSequencerStep (11, { true, 60, 100 });
        pump (100);
        mouse.down (*grid, at (11, 0.5f, h * 0.5f));
        mouse.drag (*grid, at (11, 0.5f, h * 0.5f - 12.0f));
        mouse.up (*grid, at (11, 0.5f, h * 0.5f - 12.0f), true);
        check (processor.getSequencerStep (11).note == 64, "with C major locked, dragging up 12 px moves two scale notes (C to E)");

        if (auto* tools = find (ed, "seqTools"))
            if (auto* row = dynamic_cast<PatternRow*> (tools->getParentComponent()))
            {
                const auto before = processor.getSequencerStep (0);
                row->applyTool (PatternRow::Tool::shiftRight);
                check (processor.getSequencerStep (1) == before, "TOOLS shift right moves step 1 to step 2");
                row->undo();
                check (processor.getSequencerStep (0) == before, "and undo moves it back");

                // v1.13.0: choosing a scale snaps the steps; a host or session changing it does not.
                set (processor, "seqScale", 0.0f);
                processor.setSequencerStep (2, { true, 61, 100 });   // C#4: not in C major
                set (processor, "seqScale", 1.0f);                   // as a host would: no pick in the menu
                pump (100);
                check (processor.getSequencerStep (2).note == 61, "a scale set by the host or a session leaves the steps alone");
                row->snapPatternToScale();                           // what the user's pick in the menu does
                check (processor.getSequencerStep (2).note == 60 || processor.getSequencerStep (2).note == 62,
                       "choosing a scale moves a step outside it to the nearest note in it");
                row->undo();
                check (processor.getSequencerStep (2).note == 61, "and UNDO puts it back");   // C major stays on for ENTER below
            }

        auto* enter = dynamic_cast<juce::Button*> (find (ed, "seqEnter"));
        if (enter != nullptr)
        {
            enter->triggerClick();
            pump();
            const auto step2 = processor.getSequencerStep (2);
            mouse.click (*grid, at (2, 0.5f, h * 0.5f));
            check (processor.getSequencerStep (2).on == step2.on, "while ENTER is armed a click picks the step instead of toggling it");
            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 61, (juce::uint8) 77), 0);   // C#, snapped to C
            midi.addEvent (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 99), 10);
            processor.processBlock (buffer, midi);
            pump (150);
            check (processor.getSequencerStep (2).note == 60 && processor.getSequencerStep (2).velocity == 77 && processor.getSequencerStep (2).on
                   && processor.getSequencerStep (3).note == 67, "ENTER writes each played note (snapped) and moves on");
            enter->triggerClick();
            pump();
            check (! enter->getToggleState(), "ENTER off");
        }
        set (processor, "seqScale", 0.0f);
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

        // v1.13.0: a drag draws depths, each step taking the pointer's height (top = 15, bottom = 0).
        const auto inner = shape->getLocalBounds().toFloat().reduced (4.0f, 3.0f);
        const auto atHeight = [inner] (int step, float height)   // height 0..1 from the bottom
        {
            return juce::Point<float> (inner.getX() + inner.getWidth() * ((float) step + 0.5f) / 16.0f,
                                       inner.getBottom() - height * inner.getHeight());
        };
        const auto depth = [&processor] (int step) { return ChopSettings::depthOf (processor.getChopDepths(), step); };
        mouse.down (*shape, atHeight (8, 0.0f));
        mouse.drag (*shape, atHeight (8, 0.0f));   // no movement: still a click
        check (depth (8) == 0 || depth (8) == ChopSettings::maxDepth, "a press without moving only toggles the step");
        const float heights[] = { 0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f };
        for (int s = 8; s <= 11; ++s)
            mouse.drag (*shape, atHeight (s, heights[s - 8]));
        mouse.up (*shape, atHeight (11, 1.0f), true);
        check (depth (8) == 0 && depth (9) == 5 && depth (10) == 10 && depth (11) == 15,
               "a drag from step 9 to 12, rising from the bottom to the top, sets depths 0, 5, 10 and 15");

        mouse.down (*shape, atHeight (0, 1.0f));
        mouse.drag (*shape, atHeight (0, 1.0f) + juce::Point<float> (0.0f, 8.0f));
        mouse.drag (*shape, atHeight (4, 0.5f));   // a fast drag: steps 2 to 4 are filled in too
        mouse.up (*shape, atHeight (4, 0.5f), true);
        check (depth (1) == 8 && depth (2) == 8 && depth (3) == 8 && depth (4) == 8, "a fast drag sets the steps it skipped");

        shape->mouseDoubleClick (mouse.event (*shape, atHeight (5, 0.5f), {}, false));
        check (processor.getChopDepths() == ChopSettings::defaultDepths, "a double-click goes back to every second step");
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
        gestures.changesOnMessageThread = gestures.changesElsewhere = 0;
        processor.loadSampleFileAsync (wav);
        processor.loadSampleFileAsync (wav);
        check (processor.getSampleStatus().loading, "while a file loads in the background the status says so");
        for (int i = 0; i < 100 && processor.getSampleStatus().loading; ++i)
            pump (50);
        check (! processor.getSampleStatus().loading && processor.getSampleStatus().name == wav.getFileName(),
               "two loads in a row end with the file loaded and the status clear");
        pump (100);
        check (gestures.changesOnMessageThread >= 1 && gestures.changesElsewhere == 0,
               "the host hears of a background load on the message thread, never the loader's ("
                   + juce::String (gestures.changesOnMessageThread) + " / " + juce::String (gestures.changesElsewhere) + ")");

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

            // v1.11.0: a host that keeps the key from every component. The overlay reads the
            // key's state instead; here a stand-in for the system's key state.
            if (auto* overlay = dynamic_cast<HelpOverlay*> (help))
            {
                bool escapeDown = true;   // held as the page opens: that press doesn't count
                overlay->isEscapeHeldInFront = [&escapeDown] { return escapeDown; };
                helpButton->triggerClick();
                pump();
                overlay->pollEscapeKey();
                const bool stayedOpen = showing (*help, ed);
                escapeDown = false;
                overlay->pollEscapeKey();
                escapeDown = true;   // a fresh press, which no component was sent
                overlay->pollEscapeKey();
                pump();
                check (stayedOpen && ! showing (*help, ed) && ! helpButton->getToggleState(),
                       "Esc the host keeps to itself still closes the help page (a fresh press only) and releases ?");

                // Through the timer, as in a host: open, press, wait a few ticks.
                escapeDown = false;
                helpButton->triggerClick();
                pump();
                escapeDown = true;
                for (int i = 0; i < 20 && showing (*help, ed); ++i)
                    juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
                check (! showing (*help, ed) && ! helpButton->getToggleState(), "the key's state is read on a timer while the page is open");
                overlay->isEscapeHeldInFront = [] { return false; };
            }
            else check (false, "the help page is a HelpOverlay");
        }
        else check (false, "view buttons, views and help page found");
    }

    //==========================================================================
    std::printf ("\nSCANLINES (v1.10.0)\n");
    {
        auto* overlay = find (ed, "scanlines");
        auto* button = dynamic_cast<juce::Button*> (find (ed, "scanlinesButton"));
        auto* knob = find (ed, "attack");
        if (overlay != nullptr && button != nullptr && knob != nullptr)
        {
            check (showing (*overlay, ed) && button->getToggleState(), "the scanlines are on by default");
            auto* content = overlay->getParentComponent();
            const auto centre = content->getLocalPoint (knob, knob->getLocalBounds().getCentre());
            check (content->getComponentAt (centre) == knob, "a click on a knob reaches the knob through the scanlines");
            button->triggerClick();
            pump();
            check (! showing (*overlay, ed) && ! button->getToggleState()
                       && ! (bool) processor.getAPVTS().state.getProperty ("scanlines", true),
                   "SCANLINES turns them off, and the choice is kept with the session");
            button->triggerClick();
            pump();
            check (showing (*overlay, ed), "and on again");
        }
        else check (false, "scanlines overlay and SCANLINES button found");
    }

    //==========================================================================
    std::printf ("\nThe CRT look (v1.10.0): legible, and drawn once\n");
    {
        // Text on the tube glass: the phosphor colours and the four hues (as the frame and the
        // controls use them) against the brightest part of the glass, at least WCAG AA (4.5:1).
        juce::StringArray weak;
        const auto glass = neon::crt::screenCentre;
        const std::pair<const char*, juce::Colour> inks[] = {
            { "phosphor", neon::crt::phosphor }, { "phosphorDim", neon::crt::phosphorDim },
            { "cyan", neon::cyan }, { "magenta", neon::magenta }, { "amber", neon::amber }, { "green", neon::green } };
        for (const auto& [name, ink] : inks)
            if (contrast (ink, glass) < 4.5)
                weak.add (juce::String (name) + " " + juce::String (contrast (ink, glass), 1) + ":1");
        check (weak.isEmpty(), "text colours reach 4.5:1 against the tube glass" + (weak.isEmpty() ? juce::String() : " (too faint: " + weak.joinIntoString (", ") + ")"));

        auto* crtEditor = dynamic_cast<NeonSynthEditor*> (&ed);
        auto* scope = dynamic_cast<OscilloscopeView*> (find (ed, "scopeView"));
        auto* meter = dynamic_cast<LevelMeter*> (find (ed, "meter"));
        auto* help = dynamic_cast<HelpOverlay*> (find (ed, "helpOverlay"));
        auto* helpButton = dynamic_cast<juce::Button*> (find (ed, "helpButton"));
        if (crtEditor != nullptr && scope != nullptr && meter != nullptr && help != nullptr && helpButton != nullptr)
        {
            // Two frames of a playing synth, some timer ticks apart: the backdrop, the scope's
            // graticule and the meter's scale are drawn for the first and only copied for the second.
            auto counts = [&] { return std::array<int, 3> { crtEditor->getBackdrop().getRedrawCount(),
                                                            scope->getBackdrop().getRedrawCount(),
                                                            meter->getBackdrop().getRedrawCount() }; };
            play (processor, true);
            pump (70);
            (void) ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            const auto first = counts();
            play (processor, true);
            pump (140);
            (void) ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            const auto second = counts();
            play (processor, false);
            check (first[0] >= 1 && first[1] >= 1 && first[2] >= 1 && first == second,
                   "the backdrop, the scope's graticule and the meter's scale are drawn once, then reused");

            ed.setSize (juce::roundToInt (baseWidth * 1.5f), juce::roundToInt (baseHeight * 1.5f));
            (void) ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            check (counts()[0] == second[0] + 1, "a new window size draws the backdrop again, once, at the new size");
            ed.setSize (baseWidth, baseHeight);
            (void) ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);

            helpButton->triggerClick();
            pump();
            (void) ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            const int pageDraws = help->getPageCache().getRedrawCount();
            play (processor, true);
            pump (140);
            (void) ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            play (processor, false);
            check (pageDraws >= 1 && help->getPageCache().getRedrawCount() == pageDraws,
                   "the help page is drawn once while the views under it keep moving");
            helpButton->triggerClick();
            pump();
        }
        else check (false, "editor, scope, meter and help page found");

        // States the size sweep below does not show, saved for a look (NEONSYNTH_SNAPSHOT_DIR).
        set (processor, "seqOn", 1.0f);
        for (int i = 0; i < 6; ++i)
            play (processor, true);   // about 0.5 s at 48 kHz: the playhead moves a few steps
        pump (70);
        saveSnapshot (ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f), "100-sequencer-running");
        play (processor, false);
        set (processor, "seqOn", 0.0f);

        set (processor, "filterEnabled", 0.0f);
        saveSnapshot (ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f), "100-filter-off");
        set (processor, "filterEnabled", 1.0f);

        // A pop-up menu, drawn the way the look-and-feel draws one (opening a real menu
        // window needs a window manager, which the test's virtual display does not have).
        if (auto* combo = dynamic_cast<juce::ComboBox*> (find (ed, "filterMode")))
        {
            auto& lf = combo->getLookAndFeel();
            const int rowHeight = 24, width = 220, rows = combo->getNumItems();
            juce::Image menu (juce::Image::ARGB, width, rows * rowHeight + 8, true);
            {
                juce::Graphics g (menu);
                lf.drawPopupMenuBackground (g, width, menu.getHeight());
                for (int i = 0; i < rows; ++i)
                    lf.drawPopupMenuItem (g, { 0, 4 + i * rowHeight, width, rowHeight }, false, true, i == 1,
                                          combo->getItemId (i) == combo->getSelectedId(), false,
                                          combo->getItemText (i), {}, nullptr, nullptr);
            }
            saveSnapshot (menu, "100-popup-menu");
            check (rows > 0 && menu.getPixelAt (width / 2, rowHeight + 4 + rowHeight / 2) != menu.getPixelAt (width / 2, 2),
                   "a pop-up menu draws, with its highlighted row standing out");
        }
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
    harmonicsTests::run (ed, processor, gestures, mouse);   // v1.12.0: the HARMONICS view and the aliasing marks

    //==========================================================================
    std::printf ("\nThe envelope tab and SCANLINES are kept with the plugin's state\n");
    {
        showEnvTab (ed, 1);
        if (auto* scanlinesButton = dynamic_cast<juce::Button*> (find (ed, "scanlinesButton")))
            scanlinesButton->triggerClick();   // off
        pump();   // a click arrives as a message
        processor.editorBeingDeleted (editor.get());   // what a host's wrapper does before deleting the editor
        editor.reset();
        editor.reset (processor.createEditorAndMakeActive());
        pump();
        auto* button = editor != nullptr ? dynamic_cast<juce::Button*> (find (*editor, "filterEnvButton")) : nullptr;
        check (editor != nullptr && button != nullptr && button->getToggleState()
                   && allShowing (*editor, filterEnvControls, std::size (filterEnvControls), true)
                   && allShowing (*editor, ampEnvControls, std::size (ampEnvControls), false),
               "a reopened editor shows the FILTER tab it was closed on");
        auto* overlay = editor != nullptr ? find (*editor, "scanlines") : nullptr;
        check (overlay != nullptr && ! overlay->isVisible(), "and with the scanlines off, as it was closed");
        if (editor != nullptr)
            showEnvTab (*editor, 0);
    }

    //==========================================================================
    std::printf ("\nDRAW one cycle (v1.12.0)\n");
    if (auto* drawEditor = editor.get())
    {
        drawEditor->setSize (baseWidth, baseHeight);
        set (processor, "source", 0.0f);
        set (processor, "chopComb", 1.0f);
        set (processor, "custom", 1.0f);
        set (processor, "harmonics", 32.0f);
        processor.setHarmonicSpectrum (HarmonicSpectrum::saw());
        processor.setCycleDrawing (std::nullopt, HarmonicSpectrum::saw());
        pump();

        auto* shape = dynamic_cast<ShapeView*> (find (*drawEditor, "shapeView"));
        auto* drawButton = dynamic_cast<juce::Button*> (find (*drawEditor, "drawButton"));
        if (shape != nullptr && drawButton != nullptr && showing (*shape, *drawEditor))
        {
            const auto stripArea = boundsIn (*drawEditor, *shape).expanded (8);
            saveSnapshot (drawEditor->createComponentSnapshot (stripArea), "draw-1-custom-saw");

            // Where drawing point i (0..255) at value v (-1..1) is, as ShapeView maps them.
            const auto at = [shape] (int i, float v)
            {
                const auto inner = shape->getLocalBounds().toFloat().reduced (4.0f, 3.0f);
                const float halfHeight = inner.getHeight() * 0.5f / 1.2f;
                return juce::Point<float> (inner.getX() + inner.getWidth() * (float) i / 256.0f, inner.getCentreY() - v * halfHeight);
            };
            const auto stepAt = [shape] (int step)
            {
                const auto inner = shape->getLocalBounds().toFloat().reduced (4.0f, 3.0f);
                return juce::Point<float> (inner.getX() + inner.getWidth() * ((float) step + 0.5f) / 16.0f, inner.getCentreY());
            };
            const auto bit = [&processor] (int step) { return ((processor.getChopPattern() >> step) & 1u) != 0; };

            if (! drawButton->getToggleState())
                drawButton->triggerClick();
            pump();
            check (shape->isDrawMode() && shape->isDrawing(), "DRAW on with CUSTOM on and the Oscillator: the strip draws");

            // A square-ish cycle in a few fast moves: high for the first half, low for the second.
            const auto pattern = processor.getChopPattern();
            const int versionBefore = processor.getFourierVersion();
            mouse.down (*shape, at (0, 1.1f));
            for (int i : { 40, 127 })
                mouse.drag (*shape, at (i, 1.1f));
            for (int i : { 128, 200, 255 })
                mouse.drag (*shape, at (i, -1.1f));
            check (processor.getFourierVersion() == versionBefore, "nothing is handed over during the drag");
            mouse.up (*shape, at (255, -1.1f), true);
            pump();
            saveSnapshot (drawEditor->createComponentSnapshot (stripArea), "draw-2-square-drawn");
            saveSnapshot (drawEditor->createComponentSnapshot (drawEditor->getLocalBounds()), "draw-3-editor");

            const auto drawn = processor.getCycleDrawing();
            bool square = drawn.has_value();
            for (int i = 0; square && i < 256; ++i)
                square = (*drawn)[(size_t) i] == (i < 128 ? 1.0f : -1.0f);
            check (processor.getFourierVersion() != versionBefore && drawn.has_value(), "releasing stores the drawing");
            check (square, "the line follows the pointer over all 256 points: a fast drag fills the points it skipped");
            const auto spectrum = processor.getHarmonicSpectrum();
            check (spectrum != HarmonicSpectrum::saw() && std::abs (spectrum.levels[0] - 1.0f) < 0.01f
                       && std::abs (spectrum.levels[2] - 1.0f / 3.0f) < 0.01f && spectrum.levels[1] < 0.01f,
                   "and changes the spectrum: the drawn square's (odd harmonics 1/k)");
            check (processor.getChopPattern() == pattern, "drawing leaves CHOP's steps alone");

            // A double-click clears the drawing back to a sine (as a host sends it: a click, then the double-click).
            mouse.click (*shape, at (64, 0.0f));
            shape->mouseDoubleClick (mouse.event (*shape, at (64, 0.0f), {}, false));
            pump();
            check (! processor.getCycleDrawing().has_value() && processor.getHarmonicSpectrum() == HarmonicSpectrum::sine(),
                   "a double-click clears the drawing to a sine");

            // CUSTOM off: DRAW on or not, a click toggles a CHOP step.
            set (processor, "custom", 0.0f);
            bool before = bit (5);
            mouse.click (*shape, stepAt (5));
            check (! shape->isDrawing() && bit (5) != before, "with CUSTOM off, a click still toggles a CHOP step");
            mouse.click (*shape, stepAt (5));
            set (processor, "custom", 1.0f);

            drawButton->triggerClick();   // DRAW off
            pump();
            before = bit (5);
            const int versionNow = processor.getFourierVersion();
            mouse.click (*shape, stepAt (5));
            check (! shape->isDrawMode() && bit (5) != before && processor.getFourierVersion() == versionNow,
                   "with DRAW off, a click toggles a CHOP step and draws nothing");
            mouse.click (*shape, stepAt (5));
        }
        else check (false, "shape view and DRAW button found and showing");

        set (processor, "custom", 0.0f);
        processor.setCycleDrawing (std::nullopt, HarmonicSpectrum::defaultSpectrum());
        pump();
    }

    processor.removeListener (&gestures);
    editor.reset();
    processor.releaseResources();

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
