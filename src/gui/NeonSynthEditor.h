// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "plugin/NeonSynthProcessor.h"
#include "gui/NeonStyle.h"
#include "gui/SignalHistory.h"
#include "gui/ScopeView.h"
#include "gui/SpectrumView.h"
#include "gui/LevelMeter.h"
#include "gui/AdsrView.h"
#include "gui/VoiceLights.h"
#include "gui/CrtLookAndFeel.h"
#include "gui/CrtFrame.h"
#include "gui/SequencerView.h"
#include "gui/PatternRow.h"
#include "gui/HelpOverlay.h"
#include "gui/SampleView.h"
#include "gui/ShapeView.h"
#include "gui/LfoView.h"
#include "gui/PresetBar.h"   // v1.11.0

/**
    Neon Synth editor.

    Laid out once on a fixed 1280 x 860 design grid. Every control lives in one
    child component ("content") of that size, and the editor scales that component
    to the window with a single transform (fixed aspect ratio, 75% to 150%), so the
    layout needs no scale arithmetic and a font size means pixels at 100%. The
    window opens at 100% if that fits the screen, otherwise at 75%.

    Header: name, voice lights (click one to set how many voices can play), help.
    SOURCE panel: source and waveform, the sample controls (load, built-in pad, loop
    mode), the strip (sample waveform with draggable markers, or the sculpted
    cycle) and the knob row under it. A file can also be dropped anywhere.
    Middle: scope / spectrum, envelope (shape + knobs; AMP / FILTER tabs) and filter.
    OUTPUT panel: stereo width, then the gain fader beside the stereo meter.
    MODULATION panel (v1.9.0, green): the LFO (shape, SYNC, rate or note length, its
    picture, and the PITCH / CUTOFF / AMP depths), then the pitch bend range and the
    mod wheel's vibrato, each beside a bar showing where that controller is now.
    SEQUENCER panel: run / sync / rate / mode / tempo / swing / gate, then the 16 steps.

    The "?" button in the top-right corner opens HelpOverlay, a quick-reference
    page drawn over the whole editor.

    A 30 Hz timer drains the processor's lock-free scope FIFO and reads the
    per-voice atomics; it is the only path from audio to GUI. Each view repaints
    only when what it shows has changed, so an idle editor costs almost nothing.

    The code is in NeonSynthEditor.cpp (v1.11.1), and so is NeonSynthProcessor::createEditor(),
    so the processor needs no GUI header.
*/
class NeonSynthEditor : public juce::AudioProcessorEditor,
                         public juce::FileDragAndDropTarget,
                         private juce::Timer
{
public:
    /** The cached backdrop (panels, titles, labels); EditorTests checks it is reused. */
    const neon::crt::CachedPainter& getBackdrop() const noexcept { return backdrop; }

    explicit NeonSynthEditor (NeonSynthProcessor& p);
    ~NeonSynthEditor() override;

    void resized() override;

    /** Escape closes the help overlay from any control. A key goes to the focused component
        first and then up through its parents to here, so this catches it even when the host
        left focus on a knob or menu rather than on the overlay. */
    bool keyPressed (const juce::KeyPress& key) override;

    void paint (juce::Graphics& g) override;

private:
    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    static constexpr int baseWidth  = 1280;
    static constexpr int baseHeight = 860;
    static constexpr int timerHz    = 30;
    static inline const juce::Identifier viewPropertyId { "view" };
    static inline const juce::Identifier envTabPropertyId { "envelopeTab" };   // 0 = AMP, 1 = FILTER

    float scale() const noexcept { return (float) getWidth() / (float) baseWidth; }

    /** 100% if the window (and a title bar's worth) fits the main screen's usable area, otherwise 75%. */
    static float openingScale();

    // The knob row of the sample strip: Sample's five knobs, Granular's seven (sharing ROOT),
    // or the five sculpting knobs while Oscillator or Random plays.
    struct KnobSlot { juce::Slider* knob; juce::Label* readout; const char* name; };
    enum class StripMode { sample, granular, shape };

    std::vector<KnobSlot> knobRow() { return knobRowFor (stripMode); }
    std::vector<KnobSlot> knobRowFor (StripMode mode);
    std::vector<KnobSlot> allKnobSlots();

    /** The knob row spans the SOURCE panel's inner width (x = 32 to 400), shared out evenly. */
    static juce::Range<int> rowKnobSpan (int i, int count) noexcept;

    void layoutKnobRow();

    /** Puts every control in its place on the 1280 x 860 grid (the knob row is placed by layoutKnobRow()). */
    void layout();

    // Top of the filter panel's two knob rows; each knob's label sits 42 px below its top, the readout 56 px.
    static constexpr int filterKnobRow1 = 474, filterKnobRow2 = 550;

    // The right-hand column (964, 82, 300 x 548), split in two: OUTPUT on top, MODULATION below.
    static inline const juce::Rectangle<int> outputPanel { 964, 82, 300, 266 };
    static inline const juce::Rectangle<int> modPanel    { 964, 360, 300, 270 };
    // The MODULATION panel's two knob rows, from its top; labels and readouts as in the filter panel.
    static constexpr int modKnobRow1 = 100, modKnobRow2 = 184;

    // The cutoff slider's LFO band: a thin strip between the slider and the mode box.
    static inline const juce::Rectangle<int> cutoffBandArea { 780, 438, 162, 6 };

    /** Where the band and its tick are drawn, in content x; empty while LFO CUTOFF is 0. */
    struct CutoffBand
    {
        float low = 0.0f, high = 0.0f, now = 0.0f;
        bool  shown = false;
        bool operator!= (const CutoffBand& o) const noexcept
        {
            return shown != o.shown || low != o.low || high != o.high || now != o.now;
        }
    };

    /** How far the LFO swings the cutoff: CUTOFF's octaves either way around the cutoff, and
        where the LFO has it now. The voices add the filter envelope and velocity on top, so
        this is the LFO's share alone. */
    CutoffBand cutoffBandNow() const;

    void paintCutoffBand (juce::Graphics& g) const;

    /** The CRT frame, the panels and the static labels, drawn on the design grid (under the
        controls) and cached in `backdrop`; how each piece looks lives in CrtFrame.h. */
    void paintContent (juce::Graphics& g);

    /** name + details, with the name shortened ("...") so the details always fit the label. */
    juce::String fitSampleName (const juce::String& name, const juce::String& details) const;

    /** 48000 -> "48 kHz", 44100 -> "44.1 kHz", 22050 -> "22.05 kHz". */
    static juce::String formatSampleRate (double rate);

    //==========================================================================
    // Sample files

    static bool isSampleFile (const juce::String& path);
    void chooseSampleFile();

public:
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    void setUpLinearSlider (juce::Slider& slider, juce::Colour colour);
    void setUpReadout (juce::Label& label, juce::Colour colour, juce::Justification justification);
    void updateFilterState (bool on);

    /** SCANLINES: the overlay on or off, saved with the session (not a parameter). */
    void setScanlines (bool on);
    void setView (int view);

    /** 0: the amp envelope's view and knobs, 1: the filter envelope's, in the same places. */
    void setEnvTab (int tab);

    /** The Voices parameter: how many voices may play (16 if this build has no such parameter). */
    int voiceCount() const;

    void updateReadouts();
    void timerCallback() override;

    /** Everything the editor shows lives here, at the design size; resized() scales it to the window. */
    struct Content : juce::Component
    {
        std::function<void (juce::Graphics&)> onPaint;
        void paint (juce::Graphics& g) override { if (onPaint) onPaint (g); }
    };

    NeonSynthProcessor&           synthProcessor;
    juce::AudioProcessorValueTreeState& apvts;
    CrtLookAndFeel crtLook;   // declared before content, so it outlives everything that uses it
    Content content;   // declared before every control, so it outlives them
    neon::crt::CachedPainter backdrop;   // paintContent(), drawn once per change

    std::atomic<float>* attackValue = nullptr;
    std::atomic<float>* decayValue = nullptr;
    std::atomic<float>* sustainValue = nullptr;
    std::atomic<float>* releaseValue = nullptr;
    std::atomic<float>* cutoffValue = nullptr;
    std::atomic<float>* filterValue = nullptr;
    std::atomic<float>* gainValue = nullptr;
    std::atomic<float>* widthValue = nullptr;
    std::atomic<float>* seqTempoValue = nullptr;
    std::atomic<float>* seqSwingValue = nullptr;
    std::atomic<float>* seqGateValue = nullptr;
    std::atomic<float>* smpRootValue = nullptr;
    std::atomic<float>* smpStartValue = nullptr;
    std::atomic<float>* smpLoopStartValue = nullptr;
    std::atomic<float>* smpLoopEndValue = nullptr;
    std::atomic<float>* smpXfadeValue = nullptr;
    std::atomic<float>* grPosValue = nullptr;
    std::atomic<float>* grScanValue = nullptr;
    std::atomic<float>* grSizeValue = nullptr;
    std::atomic<float>* grDensityValue = nullptr;
    std::atomic<float>* grSprayValue = nullptr;
    std::atomic<float>* grJitterValue = nullptr;
    std::atomic<float>* chopOvertoneValue = nullptr;
    std::atomic<float>* chopCombValue = nullptr;
    std::atomic<float>* noiseValue = nullptr;
    std::atomic<float>* foldDriveValue = nullptr;
    std::atomic<float>* foldMixValue = nullptr;
    std::atomic<float>* filterResValue = nullptr;
    std::atomic<float>* filterModeValue = nullptr;
    std::atomic<float>* filterDriveValue = nullptr;
    std::atomic<float>* filterEnvAmtValue = nullptr;
    std::atomic<float>* filterVelValue = nullptr;
    std::atomic<float>* filterAttackValue = nullptr;
    std::atomic<float>* filterDecayValue = nullptr;
    std::atomic<float>* filterSustainValue = nullptr;
    std::atomic<float>* filterReleaseValue = nullptr;
    std::atomic<float>* voiceCountValue = nullptr;
    std::atomic<float>* lfoShapeValue = nullptr;
    std::atomic<float>* lfoRateValue = nullptr;
    std::atomic<float>* lfoPitchValue = nullptr;
    std::atomic<float>* lfoCutoffValue = nullptr;
    std::atomic<float>* lfoAmpValue = nullptr;
    std::atomic<float>* bendRangeValue = nullptr;
    std::atomic<float>* vibratoValue = nullptr;
    std::atomic<float>* glideValue = nullptr;

    // Visualisers and their data
    SignalHistory    history;
    OscilloscopeView oscilloscope;
    SpectrumView     spectrum;
    LevelMeter       meter;
    AdsrView         adsrView, filterAdsrView;
    VoiceLeds        voiceLeds;
    std::array<float, NeonSynthProcessor::scopeFifoSize> scratchLeft {}, scratchRight {}, scratchMid {};

    juce::TextButton scopeButton { "SCOPE" }, spectrumButton { "SPECTRUM" };
    juce::TooltipWindow tooltipWindow { this };

    // Controls
    juce::ComboBox waveformCombo;
    juce::Slider   attackKnob, decayKnob, sustainKnob, releaseKnob;
    juce::Slider   cutoffSlider, gainSlider, widthSlider;
    juce::ToggleButton filterToggle;
    juce::Label voiceCountReadout;   // "16 VOICES" or "MONO", beside the lights
    juce::Label attackReadout, decayReadout, sustainReadout, releaseReadout,
                cutoffReadout, gainReadout, widthReadout;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> waveformAttachment;
    std::unique_ptr<SliderAttachment> attackAttachment, decayAttachment, sustainAttachment, releaseAttachment,
                                      cutoffAttachment, gainAttachment, widthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> filterToggleAttachment;
    juce::ComboBox filterModeCombo;
    juce::Slider   filterResKnob, filterDriveKnob;
    juce::Label    filterResReadout, filterDriveReadout;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> filterModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   filterResAttachment, filterDriveAttachment;
    juce::Slider   filterEnvAmtKnob, filterVelKnob;
    juce::Label    filterEnvAmtReadout, filterVelReadout;
    std::unique_ptr<SliderAttachment> filterEnvAmtAttachment, filterVelAttachment;

    // Filter envelope (the ENVELOPE panel's FILTER tab)
    juce::TextButton ampEnvButton { "AMP" }, filterEnvButton { "FILTER" };
    juce::Slider     filterAttackKnob, filterDecayKnob, filterSustainKnob, filterReleaseKnob;
    juce::Label      filterAttackReadout, filterDecayReadout, filterSustainReadout, filterReleaseReadout;
    std::array<std::unique_ptr<SliderAttachment>, 4> filterEnvKnobAttachments;
    int envTab = 0;   // which envelope the panel shows (see setEnvTab)

    // Source and sample
    juce::ComboBox sourceCombo, smpLoopCombo;
    std::unique_ptr<ComboBoxAttachment> sourceAttachment, smpLoopAttachment;
    SampleView       sampleView;
    juce::Label      sampleNameLabel, sampleStatusLabel;
    juce::TextButton loadButton { "LOAD" }, padButton { "PAD" };
    juce::Slider     smpRootKnob, smpStartKnob, smpLoopStartKnob, smpLoopEndKnob, smpXfadeKnob;
    juce::Label      smpRootReadout, smpStartReadout, smpLoopStartReadout, smpLoopEndReadout, smpXfadeReadout;
    std::array<std::unique_ptr<SliderAttachment>, 5> sampleKnobAttachments;
    juce::Slider     grPosKnob, grScanKnob, grSizeKnob, grDensityKnob, grSprayKnob, grJitterKnob;
    juce::Label      grPosReadout, grScanReadout, grSizeReadout, grDensityReadout, grSprayReadout, grJitterReadout;
    std::array<std::unique_ptr<SliderAttachment>, 6> grainKnobAttachments;
    ShapeView        shapeView;
    juce::Slider     chopOvertoneKnob, chopCombKnob, noiseKnob, foldDriveKnob, foldMixKnob;
    juce::Label      chopOvertoneReadout, chopCombReadout, noiseReadout, foldDriveReadout, foldMixReadout;
    std::array<std::unique_ptr<SliderAttachment>, 5> shapeKnobAttachments;
    StripMode stripMode = StripMode::shape;   // what the sample strip shows (follows Source)
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String fittedFor;      // the sample label's last unshortened text, and the width it was fitted to
    int fittedWidth = -1;

    // Step sequencer
    StepGrid           stepGrid;
    juce::TextButton seqOnButton { "RUN" }, seqSyncButton { "SYNC" };
    juce::Slider       seqTempoSlider, seqSwingSlider, seqGateSlider;
    juce::ComboBox     seqRateCombo, seqModeCombo;
    juce::Label        seqTempoReadout, seqSwingReadout, seqGateReadout;
    std::unique_ptr<ButtonAttachment>   seqOnAttachment, seqSyncAttachment;
    std::unique_ptr<SliderAttachment>   seqTempoAttachment, seqSwingAttachment, seqGateAttachment;
    std::unique_ptr<ComboBoxAttachment> seqRateAttachment, seqModeAttachment;
    // v1.11.0: the pattern row (after stepGrid, which it hooks into)
    PatternRow         patternRow { synthProcessor, stepGrid };

    // Modulation (v1.9.0)
    LfoView          lfoView;
    ControllerBar    bendBar { true }, wheelBar { false };
    juce::ComboBox   lfoShapeCombo, lfoDivisionCombo;
    juce::TextButton lfoSyncButton { "SYNC" };
    juce::Slider     lfoRateSlider, lfoPitchKnob, lfoCutoffKnob, lfoAmpKnob, bendRangeKnob, vibratoKnob;
    juce::Label      lfoRateReadout, lfoPitchReadout, lfoCutoffReadout, lfoAmpReadout, bendRangeReadout, vibratoReadout;
    std::unique_ptr<ComboBoxAttachment> lfoShapeAttachment, lfoDivisionAttachment;
    std::unique_ptr<ButtonAttachment>   lfoSyncAttachment;
    std::unique_ptr<SliderAttachment>   lfoRateAttachment;
    std::array<std::unique_ptr<SliderAttachment>, 5> modKnobAttachments;
    // v1.11.0: glide and legato
    juce::Slider     glideKnob;
    juce::Label      glideReadout;
    juce::TextButton legatoButton { "LEGATO" };
    std::unique_ptr<SliderAttachment> glideAttachment;
    std::unique_ptr<ButtonAttachment> legatoAttachment;
    // v1.11.0: the header's preset bar (PresetBar.h) and HOLD
    PresetBar        presetBar { synthProcessor.getPresetManager() };
    juce::TextButton holdButton { "HOLD" };
    std::unique_ptr<ButtonAttachment> holdAttachment;
    CutoffBand cutoffBand;   // as last drawn under the cutoff slider

    // Help
    juce::TextButton helpButton { "?" };
    HelpOverlay      helpOverlay { baseWidth, baseHeight };
    juce::TextButton scanlinesButton { "SCANLINES" };
    neon::crt::ScanlineOverlay scanlines;

    std::unique_ptr<juce::ParameterAttachment> sourceListener;
    std::unique_ptr<juce::ParameterAttachment> lfoSyncListener;
    std::unique_ptr<juce::ParameterAttachment> filterEnabledListener;   // last: destroyed first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeonSynthEditor)
};
