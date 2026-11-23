// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

// The editor's code (v1.11.1): construction, layout, painting, the timer and the mouse and file
// handling, compiled once here instead of in every file that includes NeonSynthEditor.h. The
// header keeps the class, its members and the one-line helpers.

#include "plugin/NeonSynthProcessor.h"
#include "gui/NeonSynthEditor.h"

namespace
{
    // How a control that does nothing at the moment is drawn: dimmed, still readable.
    constexpr float unusedAlpha   = 0.45f;   // a source's controls while another source plays
    constexpr float disabledAlpha = 0.4f;    // the filter's controls while the filter is off

    // The window opens at this scale on a screen too small for it at 100%.
    constexpr float smallScreenScale = 0.75f;
}

NeonSynthEditor::NeonSynthEditor (NeonSynthProcessor& p)
    : AudioProcessorEditor (&p), synthProcessor (p), apvts (p.getAPVTS()), sampleView (p, p.getAPVTS()), shapeView (p, p.getAPVTS()), stepGrid (p)
{
    addAndMakeVisible (content);
    content.setLookAndFeel (&crtLook);   // every control inherits the CRT look (v1.10.0)
    content.setOpaque (true);            // the backdrop covers it, so nothing behind it is ever drawn
    content.onPaint = [this] (juce::Graphics& g)
    {
        // The backdrop, panels and labels change only with the envelope tab and the source,
        // so they are drawn once into an image and copied from it; the LFO band moves.
        backdrop.paint (g, content.getLocalBounds(), [this] (juce::Graphics& ig) { paintContent (ig); });
        paintCutoffBand (g);
    };

    attackValue  = apvts.getRawParameterValue ("attack");
    decayValue   = apvts.getRawParameterValue ("decay");
    sustainValue = apvts.getRawParameterValue ("sustain");
    releaseValue = apvts.getRawParameterValue ("release");
    cutoffValue  = apvts.getRawParameterValue ("cutoff");
    filterValue  = apvts.getRawParameterValue ("filterEnabled");
    gainValue    = apvts.getRawParameterValue ("gain");
    widthValue   = apvts.getRawParameterValue ("width");
    seqTempoValue = apvts.getRawParameterValue ("seqTempo");
    seqSwingValue = apvts.getRawParameterValue ("seqSwing");
    seqGateValue  = apvts.getRawParameterValue ("seqGate");
    smpRootValue     = apvts.getRawParameterValue ("smpRoot");
    smpStartValue    = apvts.getRawParameterValue ("smpStart");
    smpLoopStartValue = apvts.getRawParameterValue ("smpLoopStart");
    smpLoopEndValue  = apvts.getRawParameterValue ("smpLoopEnd");
    smpXfadeValue    = apvts.getRawParameterValue ("smpXfade");
    grPosValue       = apvts.getRawParameterValue ("grPos");
    grScanValue      = apvts.getRawParameterValue ("grScan");
    grSizeValue      = apvts.getRawParameterValue ("grSize");
    grDensityValue   = apvts.getRawParameterValue ("grDensity");
    grSprayValue     = apvts.getRawParameterValue ("grSpray");
    grJitterValue    = apvts.getRawParameterValue ("grJitter");
    chopOvertoneValue = apvts.getRawParameterValue ("chopOvertone");
    chopCombValue     = apvts.getRawParameterValue ("chopComb");
    noiseValue        = apvts.getRawParameterValue ("noiseLevel");
    foldDriveValue    = apvts.getRawParameterValue ("foldDrive");
    foldMixValue      = apvts.getRawParameterValue ("foldMix");
    filterResValue    = apvts.getRawParameterValue ("filterRes");
    filterModeValue   = apvts.getRawParameterValue ("filterMode");
    filterDriveValue  = apvts.getRawParameterValue ("filterDrive");
    filterEnvAmtValue  = apvts.getRawParameterValue ("filterEnvAmt");
    filterVelValue     = apvts.getRawParameterValue ("filterVel");
    filterAttackValue  = apvts.getRawParameterValue ("filterAttack");
    filterDecayValue   = apvts.getRawParameterValue ("filterDecay");
    filterSustainValue = apvts.getRawParameterValue ("filterSustain");
    filterReleaseValue = apvts.getRawParameterValue ("filterRelease");
    voiceCountValue = apvts.getRawParameterValue ("voiceCount");   // v1.9.0; null in a build without it
    lfoShapeValue  = apvts.getRawParameterValue ("lfoShape");
    lfoRateValue   = apvts.getRawParameterValue ("lfoRate");
    lfoPitchValue  = apvts.getRawParameterValue ("lfoPitch");
    lfoCutoffValue = apvts.getRawParameterValue ("lfoCutoff");
    lfoAmpValue    = apvts.getRawParameterValue ("lfoAmp");
    bendRangeValue = apvts.getRawParameterValue ("bendRange");
    vibratoValue   = apvts.getRawParameterValue ("vibrato");
    glideValue     = apvts.getRawParameterValue ("glide");   // v1.11.0

    // ── Oscillator ───────────────────────────────────────────────────────
    content.addAndMakeVisible (waveformCombo);
    waveformCombo.addItem ("Sine",     1);
    waveformCombo.addItem ("Square",   2);
    waveformCombo.addItem ("Triangle", 3);
    waveformCombo.addItem ("Sawtooth", 4);
    waveformCombo.setColour (juce::ComboBox::outlineColourId, neon::cyan);
    waveformCombo.setColour (juce::ComboBox::textColourId,    neon::cyan);
    waveformCombo.setColour (juce::ComboBox::arrowColourId,   neon::cyan);
    waveformAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
        (apvts, "waveform", waveformCombo);

    content.addAndMakeVisible (sourceCombo);
    sourceCombo.addItemList ({ "Oscillator", "Sample", "Granular", "Random" }, 1);   // same order as the parameter
    sourceCombo.setTooltip ("What the voices play: the oscillator (Waveform), the loaded sample, grains of it, or Random: a pitched wave of 16 random steps that keeps evolving");
    for (auto id : { juce::ComboBox::outlineColourId, juce::ComboBox::textColourId, juce::ComboBox::arrowColourId })
        sourceCombo.setColour (id, neon::cyan);
    sourceAttachment = std::make_unique<ComboBoxAttachment> (apvts, "source", sourceCombo);

    // ── Sample ───────────────────────────────────────────────────────────
    content.addAndMakeVisible (sampleView);
    content.addChildComponent (shapeView);   // in the sample view's place while Oscillator or Random plays
    setUpReadout (sampleNameLabel, neon::cyan, juce::Justification::centredLeft);
    sampleNameLabel.setInterceptsMouseClicks (true, false);   // so its tooltip (the full path) shows
    setUpReadout (sampleStatusLabel, neon::red, juce::Justification::centredRight);

    for (auto* button : { &loadButton, &padButton })
    {
        button->setColour (juce::TextButton::buttonColourId,  juce::Colours::transparentBlack);
        button->setColour (juce::TextButton::textColourOffId, neon::cyan);
        button->setColour (juce::ComboBox::outlineColourId,   neon::cyan.withAlpha (0.6f));
        content.addAndMakeVisible (button);
    }
    loadButton.setTooltip ("Load a WAV, AIFF or FLAC file (or drop one on the synth)");
    padButton.setTooltip ("Go back to the built-in pad");
    loadButton.onClick = [this] { chooseSampleFile(); };
    padButton.onClick  = [this] { synthProcessor.useDefaultSample(); };

    content.addAndMakeVisible (smpLoopCombo);
    smpLoopCombo.addItemList ({ "One-shot", "Loop", "Loop until release" }, 1);   // same order as the parameter
    smpLoopCombo.setTooltip ("One-shot: play to the end.  Loop: loop while the note sounds.  "
                             "Loop until release: loop while the key is held, then play on to the end.");
    for (auto id : { juce::ComboBox::outlineColourId, juce::ComboBox::textColourId, juce::ComboBox::arrowColourId })
        smpLoopCombo.setColour (id, neon::cyan);
    smpLoopAttachment = std::make_unique<ComboBoxAttachment> (apvts, "smpLoop", smpLoopCombo);

    const std::pair<juce::Slider*, const char*> sampleKnobs[] = {
        { &smpRootKnob, "smpRoot" }, { &smpStartKnob, "smpStart" }, { &smpLoopStartKnob, "smpLoopStart" },
        { &smpLoopEndKnob, "smpLoopEnd" }, { &smpXfadeKnob, "smpXfade" } };
    for (size_t i = 0; i < std::size (sampleKnobs); ++i)
    {
        auto* knob = sampleKnobs[i].first;
        knob->setSliderStyle (juce::Slider::Rotary);
        knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob->setColour (juce::Slider::rotarySliderFillColourId,    neon::cyan);
        knob->setColour (juce::Slider::rotarySliderOutlineColourId, neon::cyan.withAlpha (0.3f));
        knob->setColour (juce::Slider::thumbColourId,               neon::cyan);
        content.addAndMakeVisible (knob);
        sampleKnobAttachments[i] = std::make_unique<SliderAttachment> (apvts, sampleKnobs[i].second, *knob);
    }
    smpRootKnob.setTooltip ("Root: the key at which the sample plays at its own pitch");
    smpXfadeKnob.setTooltip ("Loop crossfade length");

    const std::pair<juce::Slider*, const char*> grainKnobs[] = {
        { &grPosKnob, "grPos" }, { &grScanKnob, "grScan" }, { &grSizeKnob, "grSize" },
        { &grDensityKnob, "grDensity" }, { &grSprayKnob, "grSpray" }, { &grJitterKnob, "grJitter" } };
    for (size_t i = 0; i < std::size (grainKnobs); ++i)
    {
        auto* knob = grainKnobs[i].first;
        knob->setSliderStyle (juce::Slider::Rotary);
        knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob->setColour (juce::Slider::rotarySliderFillColourId,    neon::magenta);
        knob->setColour (juce::Slider::rotarySliderOutlineColourId, neon::magenta.withAlpha (0.3f));
        knob->setColour (juce::Slider::thumbColourId,               neon::magenta);
        content.addChildComponent (knob);
        grainKnobAttachments[i] = std::make_unique<SliderAttachment> (apvts, grainKnobs[i].second, *knob);
    }
    grPosKnob.setTooltip ("Position: where in the sample the cloud sits (or drag the magenta line on the waveform)");
    grScanKnob.setTooltip ("Scan: how fast the cloud moves through the sample, in either direction; 0 freezes it");
    grSizeKnob.setTooltip ("Size: length of each grain");
    grDensityKnob.setTooltip ("Density: grains started per second");
    grSprayKnob.setTooltip ("Spray: how far each grain may start from the cloud's position");
    grJitterKnob.setTooltip ("Pitch jitter: random detune of each grain, in cents");

    // Waveform sculpting (v1.6.0): the knob row while Oscillator or Random plays.
    const std::pair<juce::Slider*, const char*> shapeKnobs[] = {
        { &chopOvertoneKnob, "chopOvertone" }, { &chopCombKnob, "chopComb" }, { &noiseKnob, "noiseLevel" },
        { &foldDriveKnob, "foldDrive" }, { &foldMixKnob, "foldMix" } };
    for (size_t i = 0; i < std::size (shapeKnobs); ++i)
    {
        auto* knob = shapeKnobs[i].first;
        knob->setSliderStyle (juce::Slider::Rotary);
        knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob->setColour (juce::Slider::rotarySliderFillColourId,    neon::cyan);
        knob->setColour (juce::Slider::rotarySliderOutlineColourId, neon::cyan.withAlpha (0.3f));
        knob->setColour (juce::Slider::thumbColourId,               neon::cyan);
        content.addChildComponent (knob);
        shapeKnobAttachments[i] = std::make_unique<SliderAttachment> (apvts, shapeKnobs[i].second, *knob);
    }
    chopOvertoneKnob.setTooltip ("Overtone: how far CHOP cuts the shaded steps. 0% leaves them, 100% silences them, 200% turns them upside down");
    chopCombKnob.setTooltip ("Comb: repeats the 16-step pattern this many times per cycle, for metallic tones");
    noiseKnob.setTooltip ("Noise: crossfades the wave towards white noise, keeping the level steady");
    foldDriveKnob.setTooltip ("Drive: how hard the wavefolder folds the wave back on itself");
    foldMixKnob.setTooltip ("Fold: how much of the folded wave to hear. 0% switches the folder off");
    for (auto* label : { &chopOvertoneReadout, &chopCombReadout, &noiseReadout, &foldDriveReadout, &foldMixReadout })
        setUpReadout (*label, neon::cyan, juce::Justification::centred);

    for (auto* label : { &smpRootReadout, &smpStartReadout, &smpLoopStartReadout, &smpLoopEndReadout, &smpXfadeReadout,
                         &grPosReadout, &grScanReadout, &grSizeReadout, &grDensityReadout, &grSprayReadout, &grJitterReadout })
        setUpReadout (*label, neon::cyan, juce::Justification::centred);
    for (auto* label : { &grPosReadout, &grScanReadout, &grSizeReadout, &grDensityReadout, &grSprayReadout, &grJitterReadout })
        label->setColour (juce::Label::textColourId, neon::magenta);

    // Waveform only applies to the oscillator, the loop mode only to Sample, and the strip
    // follows the source: Sample's knobs, Granular's, or (Oscillator and Random) the shape view
    // with the sculpting knobs.
    sourceListener = std::make_unique<juce::ParameterAttachment> (
        *apvts.getParameter ("source"),
        [this] (float value)
        {
            using Source = NeonSynthVoice::Source;
            const auto source = (Source) juce::roundToInt (value);
            const auto mode = source == Source::sample ? StripMode::sample : source == Source::granular ? StripMode::granular : StripMode::shape;
            waveformCombo.setAlpha (source == Source::oscillator ? 1.0f : unusedAlpha);
            smpLoopCombo.setAlpha (mode == StripMode::sample ? 1.0f : unusedAlpha);
            sampleNameLabel.setAlpha (mode == StripMode::shape ? unusedAlpha : 1.0f);
            sampleView.setVisible (mode != StripMode::shape);
            shapeView.setVisible (mode == StripMode::shape);
            if (std::exchange (stripMode, mode) != stripMode && getWidth() > 0)
            {
                layoutKnobRow();
                backdrop.invalidate();   // the strip's caption and the knob names change
                content.repaint();
            }
        });
    sourceListener->sendInitialUpdate();

    // ── Envelope ─────────────────────────────────────────────────────────
    content.addAndMakeVisible (adsrView);
    adsrView.attach (apvts.getParameter ("attack"), apvts.getParameter ("decay"),
                     apvts.getParameter ("sustain"), apvts.getParameter ("release"));

    for (auto* knob : { &attackKnob, &decayKnob, &sustainKnob, &releaseKnob })
    {
        knob->setSliderStyle (juce::Slider::Rotary);
        knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob->setColour (juce::Slider::rotarySliderFillColourId,    neon::amber);
        knob->setColour (juce::Slider::rotarySliderOutlineColourId, neon::amber.withAlpha (0.3f));
        knob->setColour (juce::Slider::thumbColourId,               neon::amber);
        content.addAndMakeVisible (knob);
    }
    attackAttachment  = std::make_unique<SliderAttachment> (apvts, "attack",  attackKnob);
    decayAttachment   = std::make_unique<SliderAttachment> (apvts, "decay",   decayKnob);
    sustainAttachment = std::make_unique<SliderAttachment> (apvts, "sustain", sustainKnob);
    releaseAttachment = std::make_unique<SliderAttachment> (apvts, "release", releaseKnob);

    for (auto* label : { &attackReadout, &decayReadout, &sustainReadout, &releaseReadout })
        setUpReadout (*label, neon::amber, juce::Justification::centred);

    // The filter envelope (v1.8.0): a second view and set of knobs in the same places, in
    // magenta, shown instead of the amp envelope's by the FILTER tab. Two fixed sets rather
    // than one set re-bound on a tab change: each control stays attached to one parameter, so
    // host automation and undo gestures never land on the wrong one.
    content.addChildComponent (filterAdsrView);
    filterAdsrView.setAccent (neon::magenta);
    filterAdsrView.attach (apvts.getParameter ("filterAttack"), apvts.getParameter ("filterDecay"),
                           apvts.getParameter ("filterSustain"), apvts.getParameter ("filterRelease"));

    const std::pair<juce::Slider*, const char*> filterEnvKnobs[] = {
        { &filterAttackKnob, "filterAttack" }, { &filterDecayKnob, "filterDecay" },
        { &filterSustainKnob, "filterSustain" }, { &filterReleaseKnob, "filterRelease" } };
    for (size_t i = 0; i < std::size (filterEnvKnobs); ++i)
    {
        auto* knob = filterEnvKnobs[i].first;
        knob->setSliderStyle (juce::Slider::Rotary);
        knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob->setColour (juce::Slider::rotarySliderFillColourId,    neon::magenta);
        knob->setColour (juce::Slider::rotarySliderOutlineColourId, neon::magenta.withAlpha (0.3f));
        knob->setColour (juce::Slider::thumbColourId,               neon::magenta);
        content.addChildComponent (knob);
        filterEnvKnobAttachments[i] = std::make_unique<SliderAttachment> (apvts, filterEnvKnobs[i].second, *knob);
    }
    filterAttackKnob.setTooltip ("Filter attack: how long the filter envelope takes to rise to its peak");
    filterDecayKnob.setTooltip ("Filter decay: how long it then takes to fall to the sustain level");
    filterSustainKnob.setTooltip ("Filter sustain: where the filter envelope rests while the key is held");
    filterReleaseKnob.setTooltip ("Filter release: how long it takes to fall back after the key is let go");
    for (auto* label : { &filterAttackReadout, &filterDecayReadout, &filterSustainReadout, &filterReleaseReadout })
        setUpReadout (*label, neon::magenta, juce::Justification::centred);

    // AMP / FILTER: which envelope the panel shows (both always run).
    for (auto [button, colour] : { std::pair { &ampEnvButton, neon::amber }, std::pair { &filterEnvButton, neon::magenta } })
    {
        button->setClickingTogglesState (true);
        button->setRadioGroupId (2);
        button->setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
        button->setColour (juce::TextButton::buttonOnColourId, colour.withAlpha (0.25f));
        button->setColour (juce::TextButton::textColourOffId,  colour.withAlpha (0.55f));
        button->setColour (juce::TextButton::textColourOnId,   colour);
        button->setColour (juce::ComboBox::outlineColourId,    colour.withAlpha (0.5f));
        content.addAndMakeVisible (button);
    }
    ampEnvButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    filterEnvButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    ampEnvButton.setTooltip ("Show the amp envelope: how the loudness of each note rises and falls");
    filterEnvButton.setTooltip ("Show the filter envelope: how the cutoff moves during each note (how far: ENV in FILTER)");
    ampEnvButton.onClick    = [this] { if (ampEnvButton.getToggleState())    setEnvTab (0); };
    filterEnvButton.onClick = [this] { if (filterEnvButton.getToggleState()) setEnvTab (1); };

    // ── Filter ───────────────────────────────────────────────────────────
    setUpLinearSlider (cutoffSlider, neon::magenta);
    cutoffAttachment = std::make_unique<SliderAttachment> (apvts, "cutoff", cutoffSlider);
    setUpReadout (cutoffReadout, neon::magenta, juce::Justification::centredRight);

    content.addAndMakeVisible (filterToggle);
    filterToggle.setButtonText ("ON");
    filterToggle.setColour (juce::ToggleButton::textColourId,         neon::magenta);
    filterToggle.setColour (juce::ToggleButton::tickColourId,         neon::magenta);
    filterToggle.setColour (juce::ToggleButton::tickDisabledColourId, neon::magenta.withAlpha (0.3f));
    filterToggleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>
        (apvts, "filterEnabled", filterToggle);

    // Follows clicks and host automation alike; callbacks arrive on the message thread.
    filterEnabledListener = std::make_unique<juce::ParameterAttachment> (
        *apvts.getParameter ("filterEnabled"),
        [this] (float value) { updateFilterState (value >= 0.5f); });
    // Mode, resonance and drive (v1.8.0)
    content.addAndMakeVisible (filterModeCombo);
    filterModeCombo.addItemList ({ "Low-pass", "High-pass", "Band-pass", "Notch" }, 1);   // same order as the parameter
    filterModeCombo.setTooltip ("Filter mode: which part of the spectrum the filter keeps. Low-pass keeps the lows, "
                                "high-pass the highs, band-pass a band around the cutoff, notch everything but it");
    for (auto id : { juce::ComboBox::outlineColourId, juce::ComboBox::textColourId, juce::ComboBox::arrowColourId })
        filterModeCombo.setColour (id, neon::magenta);
    filterModeAttachment = std::make_unique<ComboBoxAttachment> (apvts, "filterMode", filterModeCombo);

    for (auto* knob : { &filterResKnob, &filterDriveKnob, &filterEnvAmtKnob, &filterVelKnob })
    {
        knob->setSliderStyle (juce::Slider::Rotary);
        knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob->setColour (juce::Slider::rotarySliderFillColourId,    neon::magenta);
        knob->setColour (juce::Slider::rotarySliderOutlineColourId, neon::magenta.withAlpha (0.3f));
        knob->setColour (juce::Slider::thumbColourId,               neon::magenta);
        content.addAndMakeVisible (knob);
    }
    filterResAttachment   = std::make_unique<SliderAttachment> (apvts, "filterRes",   filterResKnob);
    filterDriveAttachment = std::make_unique<SliderAttachment> (apvts, "filterDrive", filterDriveKnob);
    filterEnvAmtAttachment = std::make_unique<SliderAttachment> (apvts, "filterEnvAmt", filterEnvAmtKnob);
    filterVelAttachment    = std::make_unique<SliderAttachment> (apvts, "filterVel",    filterVelKnob);
    filterResKnob.setTooltip ("Resonance: boosts the frequencies at the cutoff. From about 98% the filter rings on its own, "
                              "a sine at the cutoff that follows the envelope");
    filterDriveKnob.setTooltip ("Drive: soft-clips the sound before the filter, so its new harmonics meet the resonance. "
                                "Loud peaks stay where they are; quieter parts come up");
    filterEnvAmtKnob.setTooltip ("Envelope amount: how far the filter envelope moves the cutoff, in octaves. "
                                 "Below 0 it moves it down");
    filterVelKnob.setTooltip ("Velocity: how far harder notes raise the cutoff, in octaves at full velocity");
    for (auto* label : { &filterResReadout, &filterDriveReadout, &filterEnvAmtReadout, &filterVelReadout })
        setUpReadout (*label, neon::magenta, juce::Justification::centred);

    filterEnabledListener->sendInitialUpdate();

    // ── Voices ───────────────────────────────────────────────────────────
    content.addAndMakeVisible (voiceLeds);
    // Click light N for N voices (v1.9.0): one gesture, so a host records it as one edit.
    voiceLeds.onLightClicked = [this] (int count)
    {
        if (auto* param = apvts.getParameter ("voiceCount"))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 ((float) count));
            param->endChangeGesture();
        }
    };
    setUpReadout (voiceCountReadout, neon::cyan, juce::Justification::centredRight);

    // ── Output: display, meter, gain, width ──────────────────────────────
    content.addChildComponent (oscilloscope);
    content.addChildComponent (spectrum);
    content.addAndMakeVisible (meter);

    for (auto* button : { &scopeButton, &spectrumButton })
    {
        button->setClickingTogglesState (true);
        button->setRadioGroupId (1);
        button->setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
        button->setColour (juce::TextButton::buttonOnColourId, neon::cyan.withAlpha (0.25f));
        button->setColour (juce::TextButton::textColourOffId,  neon::cyan.withAlpha (0.55f));
        button->setColour (juce::TextButton::textColourOnId,   neon::cyan);
        button->setColour (juce::ComboBox::outlineColourId,    neon::cyan.withAlpha (0.5f));
        content.addAndMakeVisible (button);
    }
    scopeButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    spectrumButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    scopeButton.onClick    = [this] { if (scopeButton.getToggleState())    setView (0); };
    spectrumButton.onClick = [this] { if (spectrumButton.getToggleState()) setView (1); };

    // GAIN: a vertical fader beside the meter, like a mixer's channel strip. The attachment
    // makes a double-click return it to 0 dB; the wheel nudges it.
    setUpLinearSlider (gainSlider, neon::cyan);
    gainSlider.setSliderStyle (juce::Slider::LinearVertical);
    gainSlider.setTooltip ("Output level, in dB. Double-click for 0 dB");
    gainAttachment = std::make_unique<SliderAttachment> (apvts, "gain", gainSlider);
    setUpReadout (gainReadout, neon::cyan, juce::Justification::centred);

    setUpLinearSlider (widthSlider, neon::cyan);
    widthSlider.setTooltip ("Stereo width: how far notes are spread left and right by pitch. 0% is mono");
    widthAttachment = std::make_unique<SliderAttachment> (apvts, "width", widthSlider);
    setUpReadout (widthReadout, neon::cyan, juce::Justification::centredRight);

    // ── Modulation (v1.9.0) ──────────────────────────────────────────────
    // Its own hue, green, for everything that moves the sound over time.
    content.addAndMakeVisible (lfoView);
    content.addAndMakeVisible (bendBar);
    content.addAndMakeVisible (wheelBar);
    bendBar.setTooltip ("Where the pitch bend is now (from any MIDI keyboard or controller)");
    wheelBar.setTooltip ("Where the mod wheel is now (from any MIDI keyboard or controller)");

    for (auto* combo : { &lfoShapeCombo, &lfoDivisionCombo })
    {
        for (auto id : { juce::ComboBox::outlineColourId, juce::ComboBox::textColourId, juce::ComboBox::arrowColourId })
            combo->setColour (id, neon::green);
        content.addAndMakeVisible (combo);
    }
    lfoShapeCombo.addItemList ({ "Sine", "Triangle", "Square", "Saw", "Random" }, 1);   // same order as the parameter
    lfoDivisionCombo.addItemList ({ "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/8T", "1/16T" }, 1);
    lfoShapeCombo.setTooltip ("LFO shape. Random holds a new random level each cycle");
    lfoDivisionCombo.setTooltip ("LFO cycle as a note length, at the host's tempo (or TEMPO in the Standalone). "
                                 "With the host playing it lines up with the bar");
    lfoShapeAttachment    = std::make_unique<ComboBoxAttachment> (apvts, "lfoShape",    lfoShapeCombo);
    lfoDivisionAttachment = std::make_unique<ComboBoxAttachment> (apvts, "lfoDivision", lfoDivisionCombo);

    // SYNC: a latching button like the sequencer's.
    lfoSyncButton.setClickingTogglesState (true);
    lfoSyncButton.setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
    lfoSyncButton.setColour (juce::TextButton::buttonOnColourId, neon::green.withAlpha (0.30f));
    lfoSyncButton.setColour (juce::TextButton::textColourOffId,  neon::green.withAlpha (0.55f));
    lfoSyncButton.setColour (juce::TextButton::textColourOnId,   neon::green);
    lfoSyncButton.setColour (juce::ComboBox::outlineColourId,    neon::green.withAlpha (0.6f));
    lfoSyncButton.setTooltip ("Set the LFO's rate as a note length that follows the tempo, instead of in Hz");
    content.addAndMakeVisible (lfoSyncButton);
    lfoSyncAttachment = std::make_unique<ButtonAttachment> (apvts, "lfoSync", lfoSyncButton);

    setUpLinearSlider (lfoRateSlider, neon::green);
    lfoRateSlider.setTooltip ("LFO rate in cycles per second");
    lfoRateAttachment = std::make_unique<SliderAttachment> (apvts, "lfoRate", lfoRateSlider);
    setUpReadout (lfoRateReadout, neon::green, juce::Justification::centredRight);

    const std::pair<juce::Slider*, const char*> modKnobs[] = {
        { &lfoPitchKnob, "lfoPitch" }, { &lfoCutoffKnob, "lfoCutoff" }, { &lfoAmpKnob, "lfoAmp" },
        { &bendRangeKnob, "bendRange" }, { &vibratoKnob, "vibrato" } };
    for (size_t i = 0; i < std::size (modKnobs); ++i)
    {
        auto* knob = modKnobs[i].first;
        knob->setSliderStyle (juce::Slider::Rotary);
        knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob->setColour (juce::Slider::rotarySliderFillColourId,    neon::green);
        knob->setColour (juce::Slider::rotarySliderOutlineColourId, neon::green.withAlpha (0.3f));
        knob->setColour (juce::Slider::thumbColourId,               neon::green);
        content.addAndMakeVisible (knob);
        modKnobAttachments[i] = std::make_unique<SliderAttachment> (apvts, modKnobs[i].second, *knob);
    }
    lfoPitchKnob.setTooltip ("LFO to pitch: how far the LFO bends every voice, in semitones (vibrato, sirens)");
    lfoCutoffKnob.setTooltip ("LFO to cutoff: how far the LFO sweeps the filter, in octaves either way (wah, wobble). "
                              "The green band under CUTOFF shows the sweep");
    lfoAmpKnob.setTooltip ("LFO to amp: tremolo depth. The LFO only ever turns the level down");
    bendRangeKnob.setTooltip ("Pitch bend range: how far the pitch bend wheel bends, in semitones each way");
    vibratoKnob.setTooltip ("Vibrato: how deep the mod wheel's vibrato is at the top of its travel, in cents");
    for (auto* label : { &lfoPitchReadout, &lfoCutoffReadout, &lfoAmpReadout, &bendRangeReadout, &vibratoReadout })
        setUpReadout (*label, neon::green, juce::Justification::centred);

    // v1.11.0: GLIDE and LEGATO, beside the other pitch controls.
    glideKnob.setSliderStyle (juce::Slider::Rotary);
    glideKnob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    glideKnob.setColour (juce::Slider::rotarySliderFillColourId,    neon::green);
    glideKnob.setColour (juce::Slider::rotarySliderOutlineColourId, neon::green.withAlpha (0.3f));
    glideKnob.setColour (juce::Slider::thumbColourId,               neon::green);
    glideKnob.setTooltip ("Glide: how long each voice takes to slide to a new note (0 = jump straight there)");
    content.addAndMakeVisible (glideKnob);
    glideAttachment = std::make_unique<SliderAttachment> (apvts, "glide", glideKnob);
    setUpReadout (glideReadout, neon::green, juce::Justification::centred);

    legatoButton.setClickingTogglesState (true);
    legatoButton.setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
    legatoButton.setColour (juce::TextButton::buttonOnColourId, neon::green.withAlpha (0.30f));
    legatoButton.setColour (juce::TextButton::textColourOffId,  neon::green.withAlpha (0.55f));
    legatoButton.setColour (juce::TextButton::textColourOnId,   neon::green);
    legatoButton.setColour (juce::ComboBox::outlineColourId,    neon::green.withAlpha (0.6f));
    legatoButton.setTooltip ("Legato (with the voice limit at MONO): a key pressed while another is held glides to "
                             "the new note without restarting the envelopes; letting it go returns to the key still held. With more voices it does nothing");
    content.addAndMakeVisible (legatoButton);
    legatoAttachment = std::make_unique<ButtonAttachment> (apvts, "legato", legatoButton);

    // SYNC swaps the rate slider (Hz) for the note length, in the same place.
    lfoSyncListener = std::make_unique<juce::ParameterAttachment> (
        *apvts.getParameter ("lfoSync"),
        [this] (float value)
        {
            const bool sync = value >= 0.5f;
            lfoRateSlider.setVisible (! sync);
            lfoDivisionCombo.setVisible (sync);
        });
    lfoSyncListener->sendInitialUpdate();

    // ── Step sequencer ───────────────────────────────────────────────────
    // RUN and SYNC are latching buttons (lit = on) rather than tick boxes,
    // so they read as transport controls. ButtonAttachment works with any
    // juce::Button whose clicks toggle its state.
    for (auto* toggle : { &seqOnButton, &seqSyncButton })
    {
        toggle->setClickingTogglesState (true);
        toggle->setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
        toggle->setColour (juce::TextButton::buttonOnColourId, neon::amber.withAlpha (0.30f));
        toggle->setColour (juce::TextButton::textColourOffId,  neon::amber.withAlpha (0.55f));
        toggle->setColour (juce::TextButton::textColourOnId,   neon::amber);
        toggle->setColour (juce::ComboBox::outlineColourId,    neon::amber.withAlpha (0.6f));
        content.addAndMakeVisible (toggle);
    }
    seqOnButton.setTooltip ("Run the step sequencer");
    seqSyncButton.setTooltip ("Follow the host's tempo and transport when it has one; otherwise use TEMPO");
    seqOnAttachment   = std::make_unique<ButtonAttachment> (apvts, "seqOn",   seqOnButton);
    seqSyncAttachment = std::make_unique<ButtonAttachment> (apvts, "seqSync", seqSyncButton);

    setUpLinearSlider (seqTempoSlider, neon::amber);
    seqTempoAttachment = std::make_unique<SliderAttachment> (apvts, "seqTempo", seqTempoSlider);
    setUpReadout (seqTempoReadout, neon::amber, juce::Justification::centredRight);

    for (auto* combo : { &seqRateCombo, &seqModeCombo })
    {
        combo->setColour (juce::ComboBox::outlineColourId, neon::amber);
        combo->setColour (juce::ComboBox::textColourId,    neon::amber);
        combo->setColour (juce::ComboBox::arrowColourId,   neon::amber);
        content.addAndMakeVisible (combo);
    }
    seqRateCombo.addItemList ({ "1/8", "1/16", "1/32", "1/8T", "1/16T" }, 1);   // same order as the parameter
    seqModeCombo.addItemList ({ "Layer", "Replace", "Transpose" }, 1);
    seqRateCombo.setTooltip ("Step length");
    seqModeCombo.setTooltip ("Layer: sequencer + keyboard.  Replace: keyboard ignored.  "
                             "Transpose: the pattern plays while a key is held, shifted by (key - C4).");
    seqRateAttachment = std::make_unique<ComboBoxAttachment> (apvts, "seqRate", seqRateCombo);
    seqModeAttachment = std::make_unique<ComboBoxAttachment> (apvts, "seqMode", seqModeCombo);

    // v1.11.0: the pattern row (LENGTH, KEY, SCALE, TOOLS, ENTER, UNDO), one component; see PatternRow.h.
    content.addAndMakeVisible (patternRow);

    content.addAndMakeVisible (stepGrid);

    setUpLinearSlider (seqSwingSlider, neon::amber);
    seqSwingAttachment = std::make_unique<SliderAttachment> (apvts, "seqSwing", seqSwingSlider);
    setUpReadout (seqSwingReadout, neon::amber, juce::Justification::centredRight);

    setUpLinearSlider (seqGateSlider, neon::amber);
    seqGateAttachment = std::make_unique<SliderAttachment> (apvts, "seqGate", seqGateSlider);
    setUpReadout (seqGateReadout, neon::amber, juce::Justification::centredRight);

    // Help: "?" toggles the overlay; the overlay un-toggles the button when it closes itself.
    helpButton.setClickingTogglesState (true);
    helpButton.setTooltip ("Quick help");
    helpButton.setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
    helpButton.setColour (juce::TextButton::buttonOnColourId, neon::cyan.withAlpha (0.25f));
    helpButton.setColour (juce::TextButton::textColourOffId,  neon::cyan);
    helpButton.setColour (juce::TextButton::textColourOnId,   neon::cyan);
    helpButton.setColour (juce::ComboBox::outlineColourId,    neon::cyan.withAlpha (0.6f));
    helpButton.onClick = [this]
    {
        if (helpButton.getToggleState()) helpOverlay.show();
        else                             helpOverlay.setVisible (false);
    };
    helpOverlay.onClose = [this] { helpButton.setToggleState (false, juce::dontSendNotification); };
    content.addAndMakeVisible (helpButton);

    // SCANLINES (v1.10.0): a view setting saved with the session, like SCOPE / SPECTRUM.
    scanlinesButton.setClickingTogglesState (true);
    scanlinesButton.setTooltip ("The CRT's scanlines on or off");
    scanlinesButton.setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
    scanlinesButton.setColour (juce::TextButton::buttonOnColourId, neon::cyan.withAlpha (0.25f));
    scanlinesButton.setColour (juce::TextButton::textColourOffId,  neon::cyan.withAlpha (0.55f));
    scanlinesButton.setColour (juce::TextButton::textColourOnId,   neon::cyan);
    scanlinesButton.setColour (juce::ComboBox::outlineColourId,    neon::cyan.withAlpha (0.6f));
    scanlinesButton.onClick = [this] { setScanlines (scanlinesButton.getToggleState()); };
    content.addAndMakeVisible (scanlinesButton);

    // v1.11.0: the preset bar (PresetBar.h) and HOLD.
    content.addAndMakeVisible (presetBar);
    holdButton.setClickingTogglesState (true);
    holdButton.setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
    holdButton.setColour (juce::TextButton::buttonOnColourId, neon::cyan.withAlpha (0.25f));
    holdButton.setColour (juce::TextButton::textColourOffId,  neon::cyan.withAlpha (0.55f));
    holdButton.setColour (juce::TextButton::textColourOnId,   neon::cyan);
    holdButton.setColour (juce::ComboBox::outlineColourId,    neon::cyan.withAlpha (0.6f));
    holdButton.setTooltip ("Hold: keys keep sounding after you let go, until HOLD is switched off");
    content.addAndMakeVisible (holdButton);
    holdAttachment = std::make_unique<ButtonAttachment> (apvts, "hold", holdButton);

    content.addChildComponent (helpOverlay);   // above every control
    content.addChildComponent (scanlines);     // added last, over everything; clicks pass through it

    // Component IDs: a control attached to a parameter carries that parameter's ID, and the
    // views and buttons carry the names below. EditorTests finds everything by these IDs
    // alone, so any layout that keeps them passes the same tests.
    const std::pair<juce::Component*, const char*> ids[] = {
        { &waveformCombo, "waveform" }, { &sourceCombo, "source" }, { &smpLoopCombo, "smpLoop" },
        { &smpRootKnob, "smpRoot" }, { &smpStartKnob, "smpStart" }, { &smpLoopStartKnob, "smpLoopStart" },
        { &smpLoopEndKnob, "smpLoopEnd" }, { &smpXfadeKnob, "smpXfade" },
        { &grPosKnob, "grPos" }, { &grScanKnob, "grScan" }, { &grSizeKnob, "grSize" },
        { &grDensityKnob, "grDensity" }, { &grSprayKnob, "grSpray" }, { &grJitterKnob, "grJitter" },
        { &chopOvertoneKnob, "chopOvertone" }, { &chopCombKnob, "chopComb" }, { &noiseKnob, "noiseLevel" },
        { &foldDriveKnob, "foldDrive" }, { &foldMixKnob, "foldMix" },
        { &attackKnob, "attack" }, { &decayKnob, "decay" }, { &sustainKnob, "sustain" }, { &releaseKnob, "release" },
        { &cutoffSlider, "cutoff" }, { &filterToggle, "filterEnabled" },
        { &filterModeCombo, "filterMode" }, { &filterResKnob, "filterRes" }, { &filterDriveKnob, "filterDrive" },
        { &filterEnvAmtKnob, "filterEnvAmt" }, { &filterVelKnob, "filterVel" },
        { &filterAttackKnob, "filterAttack" }, { &filterDecayKnob, "filterDecay" },
        { &filterSustainKnob, "filterSustain" }, { &filterReleaseKnob, "filterRelease" },
        { &gainSlider, "gain" }, { &widthSlider, "width" }, { &meter, "meter" },
        { &lfoShapeCombo, "lfoShape" }, { &lfoSyncButton, "lfoSync" }, { &lfoRateSlider, "lfoRate" },
        { &lfoDivisionCombo, "lfoDivision" }, { &lfoPitchKnob, "lfoPitch" }, { &lfoCutoffKnob, "lfoCutoff" },
        { &lfoAmpKnob, "lfoAmp" }, { &bendRangeKnob, "bendRange" }, { &vibratoKnob, "vibrato" },
        { &lfoView, "lfoView" }, { &bendBar, "bendBar" }, { &wheelBar, "wheelBar" },
        { &seqOnButton, "seqOn" }, { &seqSyncButton, "seqSync" }, { &seqTempoSlider, "seqTempo" },
        { &seqRateCombo, "seqRate" }, { &seqModeCombo, "seqMode" }, { &seqSwingSlider, "seqSwing" },
        { &seqGateSlider, "seqGate" },
        { &sampleView, "sampleView" }, { &shapeView, "shapeView" }, { &stepGrid, "stepGrid" },
        { &adsrView, "adsrView" }, { &filterAdsrView, "filterAdsrView" },
        { &ampEnvButton, "ampEnvButton" }, { &filterEnvButton, "filterEnvButton" }, { &oscilloscope, "scopeView" }, { &spectrum, "spectrumView" },
        { &scopeButton, "scopeButton" }, { &spectrumButton, "spectrumButton" },
        { &loadButton, "loadButton" }, { &padButton, "padButton" },
        { &voiceLeds, "voiceCount" }, { &helpButton, "helpButton" }, { &helpOverlay, "helpOverlay" },
        { &scanlinesButton, "scanlinesButton" }, { &scanlines, "scanlines" },
        { &presetBar, "presetBar" }, { &holdButton, "hold" }, { &glideKnob, "glide" }, { &legatoButton, "legato" } };
    // (v1.11.0: the pattern row gives its six controls their IDs, seqLength ... seqUndo, itself.)
    for (const auto& [component, id] : ids)
        component->setComponentID (id);

    // The chosen view and envelope tab are GUI state, saved with the plugin's state but not automatable.
    setView ((int) apvts.state.getProperty (viewPropertyId, 0));
    setEnvTab ((int) apvts.state.getProperty (envTabPropertyId, 0));
    setScanlines ((bool) apvts.state.getProperty (neon::crt::scanlinesPropertyId, true));

    layout();

    // 75% to 150%, fixed aspect ratio; opens at the largest of 75% and 100% that fits the screen.
    setResizable (true, true);
    setResizeLimits (baseWidth * 3 / 4, baseHeight * 3 / 4, baseWidth * 3 / 2, baseHeight * 3 / 2);
    getConstrainer()->setFixedAspectRatio ((double) baseWidth / (double) baseHeight);
    const float openScale = openingScale();
    setSize (juce::roundToInt ((float) baseWidth * openScale), juce::roundToInt ((float) baseHeight * openScale));

    // While the editor was closed the scope FIFO filled up with old audio;
    // throw it away so the first frame shows what is playing now.
    while (synthProcessor.drainScope (scratchLeft.data(), scratchRight.data(), (int) scratchLeft.size()) > 0) {}

    updateReadouts();
    startTimerHz (timerHz);
}

NeonSynthEditor::~NeonSynthEditor()
{
    stopTimer();   // the Timer base outlives our members; make sure no tick can arrive mid-teardown
    content.setLookAndFeel (nullptr);   // before crtLook goes, while every control still exists
}

void NeonSynthEditor::resized()
{
    // Everything is laid out in design units inside "content"; the window just scales it.
    content.setBounds (0, 0, baseWidth, baseHeight);
    content.setTransform (juce::AffineTransform::scale (scale()));
}

bool NeonSynthEditor::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && helpOverlay.dismiss())
        return true;
    if (patternRow.handleUndoKey (key)) return true;   // v1.11.0: Ctrl+Z, pattern undo
    return AudioProcessorEditor::keyPressed (key);
}

void NeonSynthEditor::paint (juce::Graphics& g)
{
    g.fillAll (neon::crt::casing);   // the strip around the content when the window is a pixel off the aspect ratio: the casing
}

float NeonSynthEditor::openingScale()
{
    constexpr int titleBar = 48;
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const auto area = display->userBounds;   // logical pixels, bars excluded
        if (area.getWidth() < baseWidth || area.getHeight() < baseHeight + titleBar)
            return smallScreenScale;
    }
    return 1.0f;
}

std::vector<NeonSynthEditor::KnobSlot> NeonSynthEditor::knobRowFor (StripMode mode)
{
    if (mode == StripMode::shape)
        return { { &chopOvertoneKnob, &chopOvertoneReadout, "OVERTONE" }, { &chopCombKnob, &chopCombReadout, "COMB" },
                 { &noiseKnob, &noiseReadout, "NOISE" }, { &foldDriveKnob, &foldDriveReadout, "DRIVE" },
                 { &foldMixKnob, &foldMixReadout, "FOLD" } };

    if (mode == StripMode::granular)
        return { { &smpRootKnob, &smpRootReadout, "ROOT" }, { &grPosKnob, &grPosReadout, "POS" },
                 { &grScanKnob, &grScanReadout, "SCAN" }, { &grSizeKnob, &grSizeReadout, "SIZE" },
                 { &grDensityKnob, &grDensityReadout, "DENS" }, { &grSprayKnob, &grSprayReadout, "SPRAY" },
                 { &grJitterKnob, &grJitterReadout, "JITTER" } };

    return { { &smpRootKnob, &smpRootReadout, "ROOT" }, { &smpStartKnob, &smpStartReadout, "START" },
             { &smpLoopStartKnob, &smpLoopStartReadout, "L.START" }, { &smpLoopEndKnob, &smpLoopEndReadout, "L.END" },
             { &smpXfadeKnob, &smpXfadeReadout, "XFADE" } };
}

std::vector<NeonSynthEditor::KnobSlot> NeonSynthEditor::allKnobSlots()
{
    std::vector<KnobSlot> all;
    for (const auto mode : { StripMode::sample, StripMode::granular, StripMode::shape })
    {
        const auto row = knobRowFor (mode);
        all.insert (all.end(), row.begin(), row.end());
    }
    return all;
}

juce::Range<int> NeonSynthEditor::rowKnobSpan (int i, int count) noexcept
{
    const auto edge = [count] (int k) { return 32 + juce::roundToInt (368.0f * (float) k / (float) count); };
    return { edge (i), edge (i + 1) };
}

void NeonSynthEditor::layoutKnobRow()
{
    for (const auto& slot : allKnobSlots())
    {
        slot.knob->setVisible (false);
        slot.readout->setVisible (false);
    }
    const auto row = knobRow();
    for (size_t i = 0; i < row.size(); ++i)
    {
        const auto span = rowKnobSpan ((int) i, (int) row.size());
        row[i].knob->setBounds (span.getStart(), 516, span.getLength(), 52);
        row[i].readout->setBounds (span.getStart(), 588, span.getLength(), 14);
        row[i].knob->setVisible (true);
        row[i].readout->setVisible (true);
    }
}

void NeonSynthEditor::layout()
{
    // Header
    voiceLeds.setBounds (880, 33, 288, 18);
    voiceCountReadout.setBounds (764, 33, 110, 18);
    helpButton.setBounds (1212, 26, 40, 32);
    presetBar.setBounds (248, 26, 308, 32);          // v1.11.0
    holdButton.setBounds (564, 26, 72, 32);          // v1.11.0
    scanlinesButton.setBounds (644, 26, 112, 32);

    // SOURCE panel (16, 82, 400 x 548)
    sourceCombo.setBounds (32, 118, 176, 32);
    waveformCombo.setBounds (216, 118, 184, 32);
    loadButton.setBounds (32, 160, 64, 28);
    padButton.setBounds (102, 160, 56, 28);
    smpLoopCombo.setBounds (166, 160, 234, 28);
    sampleNameLabel.setBounds (32, 194, 258, 18);
    sampleStatusLabel.setBounds (294, 194, 106, 18);
    sampleView.setBounds (32, 218, 368, 290);
    shapeView.setBounds (32, 218, 368, 290);
    layoutKnobRow();

    // Middle: view (428, 82, 524 x 262), then envelope (428, 356, 330 x 274) and filter (770, 356, 182 x 274)
    scopeButton.setBounds (444, 92, 92, 28);
    spectrumButton.setBounds (536, 92, 110, 28);
    oscilloscope.setBounds (444, 128, 492, 204);
    spectrum.setBounds (444, 128, 492, 204);

    // Envelope: AMP / FILTER tabs in the header, then the view and knobs (each tab's in the same places).
    ampEnvButton.setBounds (630, 363, 52, 22);
    filterEnvButton.setBounds (682, 363, 60, 22);
    adsrView.setBounds (444, 392, 298, 116);
    filterAdsrView.setBounds (444, 392, 298, 116);
    juce::Slider* knobs[] = { &attackKnob, &decayKnob, &sustainKnob, &releaseKnob };
    juce::Label*  reads[] = { &attackReadout, &decayReadout, &sustainReadout, &releaseReadout };
    juce::Slider* filterKnobs[] = { &filterAttackKnob, &filterDecayKnob, &filterSustainKnob, &filterReleaseKnob };
    juce::Label*  filterReads[] = { &filterAttackReadout, &filterDecayReadout, &filterSustainReadout, &filterReleaseReadout };
    for (int i = 0; i < 4; ++i)
    {
        knobs[i]->setBounds (444 + 75 * i, 516, 70, 52);
        reads[i]->setBounds (444 + 75 * i, 588, 70, 14);
        filterKnobs[i]->setBounds (knobs[i]->getBounds());
        filterReads[i]->setBounds (reads[i]->getBounds());
    }

    // Filter: ON, cutoff, mode, then two rows of knobs (RES DRIVE, ENV VEL) with their labels and readouts.
    filterToggle.setBounds (872, 362, 64, 26);
    cutoffReadout.setBounds (856, 394, 80, 16);
    cutoffSlider.setBounds (786, 412, 150, 26);
    filterModeCombo.setBounds (786, 444, 150, 24);
    filterResKnob.setBounds (786, filterKnobRow1, 70, 42);
    filterDriveKnob.setBounds (866, filterKnobRow1, 70, 42);
    filterResReadout.setBounds (786, filterKnobRow1 + 56, 70, 14);
    filterDriveReadout.setBounds (866, filterKnobRow1 + 56, 70, 14);
    filterEnvAmtKnob.setBounds (786, filterKnobRow2, 70, 42);
    filterVelKnob.setBounds (866, filterKnobRow2, 70, 42);
    filterEnvAmtReadout.setBounds (786, filterKnobRow2 + 56, 70, 14);
    filterVelReadout.setBounds (866, filterKnobRow2 + 56, 70, 14);

    // OUTPUT panel (the top of the right-hand column): WIDTH across the top, then a channel
    // strip: the GAIN fader (dB marks to its left, readout under it) beside the L / R meter.
    widthSlider.setBounds (1030, 126, 150, 28);
    widthReadout.setBounds (1184, 132, 64, 16);
    gainSlider.setBounds (1014, 178, 36, 146);
    gainReadout.setBounds (976, 326, 82, 14);
    meter.setBounds (1072, 164, 176, outputPanel.getBottom() - 8 - 164);

    // MODULATION panel (the bottom of the right-hand column): a label column (980), a control
    // column (1022 to 1172) and a readout column (1176 to 1248); then the LFO picture beside
    // the three depth knobs, then the two wheels.
    const int m = modPanel.getY();
    lfoShapeCombo.setBounds (1022, m + 36, 150, 24);
    lfoSyncButton.setBounds (1176, m + 36, 72, 24);
    lfoRateSlider.setBounds (1022, m + 64, 150, 26);
    lfoDivisionCombo.setBounds (1022, m + 65, 150, 24);
    lfoRateReadout.setBounds (1176, m + 69, 72, 16);
    lfoView.setBounds (980, m + modKnobRow1, 84, 70);
    juce::Slider* depthKnobs[] = { &lfoPitchKnob, &lfoCutoffKnob, &lfoAmpKnob };
    juce::Label*  depthReads[] = { &lfoPitchReadout, &lfoCutoffReadout, &lfoAmpReadout };
    for (int i = 0; i < 3; ++i)
    {
        depthKnobs[i]->setBounds (1068 + 60 * i, m + modKnobRow1, 60, 42);
        depthReads[i]->setBounds (1068 + 60 * i, m + modKnobRow1 + 56, 60, 14);
    }
    // Row 2 (v1.11.0): the three pitch knobs, then LEGATO above the two controller bars.
    bendRangeKnob.setBounds (980, m + modKnobRow2, 56, 42);
    bendRangeReadout.setBounds (980, m + modKnobRow2 + 56, 56, 14);
    vibratoKnob.setBounds (1036, m + modKnobRow2, 56, 42);
    vibratoReadout.setBounds (1036, m + modKnobRow2 + 56, 56, 14);
    glideKnob.setBounds (1092, m + modKnobRow2, 56, 42);
    glideReadout.setBounds (1092, m + modKnobRow2 + 56, 56, 14);
    legatoButton.setBounds (1156, m + modKnobRow2, 92, 22);
    bendBar.setBounds (1156, m + modKnobRow2 + 30, 92, 10);
    wheelBar.setBounds (1156, m + modKnobRow2 + 56, 92, 10);

    // SEQUENCER panel (16, 642, 1248 x 204)
    seqOnButton.setBounds (150, 652, 72, 28);
    seqSyncButton.setBounds (228, 652, 72, 28);
    seqRateCombo.setBounds (308, 652, 80, 28);
    seqModeCombo.setBounds (396, 652, 124, 28);
    seqTempoSlider.setBounds (602, 652, 116, 28);
    seqTempoReadout.setBounds (722, 656, 92, 20);
    seqSwingSlider.setBounds (884, 652, 100, 28);
    seqSwingReadout.setBounds (988, 656, 48, 20);
    seqGateSlider.setBounds (1100, 652, 92, 28);
    seqGateReadout.setBounds (1196, 656, 52, 20);
    // The pattern row (v1.11.0), then the grid, 30 px shorter than before.
    patternRow.setBounds (150, 690, 604, 24);   // LENGTH 150, KEY 262, SCALE 326, TOOLS 502, ENTER 594, UNDO 682
    stepGrid.setBounds (32, 722, 1216, 112);

    helpOverlay.setBounds (0, 0, baseWidth, baseHeight);
    scanlines.setBounds (0, 0, baseWidth, baseHeight);
    backdrop.invalidate();   // the fader's dB marks follow the fader

    // Every readout is 12 px or more at 100%.
    for (auto* label : { &attackReadout, &decayReadout, &sustainReadout, &releaseReadout,
                         &filterAttackReadout, &filterDecayReadout, &filterSustainReadout, &filterReleaseReadout,
                         &cutoffReadout, &gainReadout, &widthReadout, &voiceCountReadout, &filterResReadout, &filterDriveReadout,
                         &lfoRateReadout, &lfoPitchReadout, &lfoCutoffReadout, &lfoAmpReadout, &bendRangeReadout, &vibratoReadout, &glideReadout,
                         &filterEnvAmtReadout, &filterVelReadout,
                         &seqTempoReadout, &seqSwingReadout, &seqGateReadout, &sampleNameLabel, &sampleStatusLabel,
                         &smpRootReadout, &smpStartReadout, &smpLoopStartReadout, &smpLoopEndReadout, &smpXfadeReadout,
                         &grPosReadout, &grScanReadout, &grSizeReadout, &grDensityReadout, &grSprayReadout, &grJitterReadout,
                         &chopOvertoneReadout, &chopCombReadout, &noiseReadout, &foldDriveReadout, &foldMixReadout })
        label->setFont (neon::font (12.0f));
    voiceCountReadout.setFont (neon::font (12.0f, true));   // it names the lights, as the panel labels do
}

NeonSynthEditor::CutoffBand NeonSynthEditor::cutoffBandNow() const
{
    CutoffBand band;
    const float octaves = lfoCutoffValue->load();
    if (octaves == 0.0f || cutoffSlider.getWidth() <= 0)
        return band;

    const auto range = cutoffSlider.getRange();
    const auto xFor = [this, range] (double hz)
    {
        return (float) cutoffSlider.getX() + cutoffSlider.getPositionOfValue (range.clipValue (hz));
    };
    const double cutoff = cutoffValue->load(), swing = std::exp2 (std::abs ((double) octaves));
    band.shown = true;
    band.low   = xFor (cutoff / swing);
    band.high  = xFor (cutoff * swing);
    band.now   = xFor (cutoff * std::exp2 ((double) (octaves * synthProcessor.getModulation().getLfoValue())));
    return band;
}

void NeonSynthEditor::paintCutoffBand (juce::Graphics& g) const
{
    if (! cutoffBand.shown)
        return;
    const float alpha = filterValue->load() >= 0.5f ? 1.0f : 0.4f;   // dimmed with the filter, like the slider
    const auto area = cutoffBandArea.toFloat();
    const float left = juce::jlimit (area.getX(), area.getRight(), cutoffBand.low);
    const float right = juce::jlimit (area.getX(), area.getRight(), cutoffBand.high);
    g.setColour (neon::green.withAlpha (0.55f * alpha));
    g.fillRoundedRectangle (left, area.getY() + 1.5f, juce::jmax (2.0f, right - left), 3.0f, 1.5f);
    g.setColour (neon::green.withAlpha (alpha));
    g.fillRect (juce::jlimit (area.getX(), area.getRight() - 2.0f, cutoffBand.now - 1.0f), area.getY(), 2.0f, area.getHeight());
}

void NeonSynthEditor::paintContent (juce::Graphics& g)
{
    namespace frame = neon::crt::frame;
    const auto window = juce::Rectangle<int> (0, 0, baseWidth, baseHeight).toFloat();
    frame::drawBackdrop (g, window);

    {
        // The panels' outer glow stops at the glass, as light inside a tube would.
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (frame::glassShape (window));
        struct Panel { juce::Rectangle<int> area; juce::Colour colour; int header; };
        for (const auto& p : { Panel { { 16, 82, 400, 548 },   neon::cyan,    30 },   // source
                               Panel { { 428, 82, 524, 262 },  neon::cyan,    42 },   // view
                               Panel { { 428, 356, 330, 274 }, envTab == 1 ? neon::magenta : neon::amber, 30 },   // envelope: the tab's colour
                               Panel { { 770, 356, 182, 274 }, neon::magenta, 30 },   // filter
                               Panel { outputPanel,            neon::cyan,    30 },   // output
                               Panel { modPanel,               neon::green,   30 },   // modulation (v1.9.0)
                               Panel { { 16, 642, 1248, 204 }, neon::amber,   46 } }) // sequencer: its control row
            frame::drawSection (g, p.area.toFloat(), p.colour, (float) p.header);
        frame::drawHeaderRule (g, { 16.0f, 14.0f, 1248.0f, 56.0f });   // the header band (16, 14, 1248 x 56) has no box
    }

    // Header
    frame::drawName (g, { 30.0f, 20.0f, 300.0f, 44.0f });

    const auto title = [&g] (const char* text, juce::Colour colour, int x, int y, int w = 150)
    {
        frame::drawTitle (g, text, juce::Rectangle<int> (x, y, w, 18).toFloat(), colour);
    };
    const auto label = [&g] (const char* text, juce::Colour colour, int x, int y, int w, int h, juce::Justification j = juce::Justification::left)
    {
        frame::drawLabel (g, text, juce::Rectangle<int> (x, y, w, h).toFloat(), colour, j);
    };

    // SOURCE: the title, what the strip below shows, and the names of the knobs under it
    title ("SOURCE", neon::cyan, 32, 92, 118);
    frame::drawHint (g, stripMode == StripMode::shape ? "SHAPE - one cycle" : stripMode == StripMode::sample ? "SAMPLE - drag the markers" : "GRAINS - drag POS",
                     { 152.0f, 92.0f, 248.0f, 18.0f }, juce::Justification::centredRight);
    const auto row = knobRow();
    for (size_t i = 0; i < row.size(); ++i)
    {
        const auto span = rowKnobSpan ((int) i, (int) row.size());
        label (row[i].name, row[i].knob == &smpRootKnob || stripMode != StripMode::granular ? neon::cyan : neon::magenta,
               span.getStart(), 572, span.getLength(), 14, juce::Justification::centred);
    }

    // Envelope: the knob names take the colour of the envelope shown
    title ("ENVELOPE", envTab == 1 ? neon::magenta : neon::amber, 444, 366, 180);   // in the tab's colour, like its panel
    const char* names[] = { "ATTACK", "DECAY", "SUSTAIN", "RELEASE" };
    for (int i = 0; i < 4; ++i)
        label (names[i], envTab == 1 ? neon::magenta : neon::amber, 444 + 75 * i, 572, 70, 14, juce::Justification::centred);

    // Filter
    title ("FILTER", neon::magenta, 786, 366, 82);
    label ("CUTOFF", neon::magenta, 786, 394, 66, 16);
    label ("RES",   neon::magenta, 786, filterKnobRow1 + 42, 70, 14, juce::Justification::centred);
    label ("DRIVE", neon::magenta, 866, filterKnobRow1 + 42, 70, 14, juce::Justification::centred);
    label ("ENV",   neon::magenta, 786, filterKnobRow2 + 42, 70, 14, juce::Justification::centred);
    label ("VEL",   neon::magenta, 866, filterKnobRow2 + 42, 70, 14, juce::Justification::centred);

    // Output
    title ("OUTPUT", neon::cyan, 980, 92, 268);
    label ("WIDTH", neon::cyan, 980, 132, 48, 16);
    label ("GAIN",  neon::cyan, 976, 162, 82, 14, juce::Justification::centred);

    // The fader's groove and its dB marks, at the fader's own positions for those values.
    frame::drawFaderGroove (g, gainSlider.getBounds().toFloat().expanded (4.0f, 2.0f));
    for (const float db : { 6.0f, 0.0f, -12.0f, -24.0f, -48.0f })
    {
        const float y = (float) gainSlider.getY() + gainSlider.getPositionOfValue (db);
        // +6 is a tick only: its number would crowd the 0 just below it
        const auto numberArea = db > 0.0f ? juce::Rectangle<float>() : juce::Rectangle<float> (972.0f, std::round (y) - 7.0f, 28.0f, 14.0f);
        frame::drawDbMark (g, y, 1010.0f, juce::approximatelyEqual (db, 0.0f), numberArea, juce::String (juce::roundToInt (db)));
    }

    // Modulation
    const int m = modPanel.getY();
    title ("MODULATION", neon::green, 980, m + 10, 268);
    label ("LFO",  neon::green, 980, m + 39, 40, 18);
    label ("RATE", neon::green, 980, m + 69, 40, 16);
    const char* depthNames[] = { "PITCH", "CUTOFF", "AMP" };
    for (int i = 0; i < 3; ++i)
        label (depthNames[i], neon::green, 1068 + 60 * i, m + modKnobRow1 + 42, 60, 14, juce::Justification::centred);
    label ("BEND",    neon::green, 980,  m + modKnobRow2 + 42, 56, 14, juce::Justification::centred);
    label ("VIBRATO", neon::green, 1036, m + modKnobRow2 + 42, 56, 14, juce::Justification::centred);
    label ("GLIDE",   neon::green, 1092, m + modKnobRow2 + 42, 56, 14, juce::Justification::centred);
    frame::drawHint (g, "pitch bend", { 1156.0f, (float) (m + modKnobRow2 + 40), 92.0f, 14.0f }, juce::Justification::centred);
    frame::drawHint (g, "mod wheel",  { 1156.0f, (float) (m + modKnobRow2 + 66), 92.0f, 14.0f }, juce::Justification::centred);

    // Sequencer
    title ("SEQUENCER", neon::amber, 32, 657, 114);
    label ("TEMPO", neon::amber, 544, 656, 56, 20);
    label ("SWING", neon::amber, 830, 656, 52, 20);
    label ("GATE",  neon::amber, 1056, 656, 42, 20);
}

juce::String NeonSynthEditor::fitSampleName (const juce::String& name, const juce::String& details) const
{
    const auto font = sampleNameLabel.getFont();
    const auto width = [&font] (const juce::String& text)
    {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (font, text, 0.0f, 0.0f);
        return glyphs.getBoundingBox (0, -1, true).getWidth();
    };

    const float available = (float) sampleNameLabel.getWidth() - 4.0f;
    if (width (name + details) <= available)
        return name + details;

    auto shortened = name;
    while (shortened.length() > 1 && width (shortened + "..." + details) > available)
        shortened = shortened.dropLastCharacters (1);
    return shortened.trimEnd() + "..." + details;
}

juce::String NeonSynthEditor::formatSampleRate (double rate)
{
    if (rate <= 0.0)
        return {};
    const auto hz = juce::roundToInt (rate);
    const int decimals = hz % 1000 == 0 ? 0 : (hz % 100 == 0 ? 1 : 2);
    return juce::String (hz / 1000.0, decimals) + " kHz";
}

bool NeonSynthEditor::isSampleFile (const juce::String& path)
{
    return juce::File (path).hasFileExtension ("wav;aif;aiff;flac");
}

void NeonSynthEditor::chooseSampleFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Load a sample", juce::File(), "*.wav;*.aif;*.aiff;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (const auto file = fc.getResult(); file.existsAsFile())
                                  synthProcessor.loadSampleFileAsync (file);
                          });
}

bool NeonSynthEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && isSampleFile (files[0]);
}

void NeonSynthEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (files.size() == 1 && isSampleFile (files[0]))
        synthProcessor.loadSampleFileAsync (juce::File (files[0]));
}

void NeonSynthEditor::setUpLinearSlider (juce::Slider& slider, juce::Colour colour)
{
    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setColour (juce::Slider::thumbColourId,      colour);
    slider.setColour (juce::Slider::trackColourId,      colour.withAlpha (0.3f));
    slider.setColour (juce::Slider::backgroundColourId, juce::Colours::black);
    content.addAndMakeVisible (slider);
}

void NeonSynthEditor::setUpReadout (juce::Label& label, juce::Colour colour, juce::Justification justification)
{
    label.setColour (juce::Label::textColourId, colour);
    label.setJustificationType (justification);
    label.setInterceptsMouseClicks (false, false);
    label.setBorderSize ({});
    content.addAndMakeVisible (label);
}

void NeonSynthEditor::updateFilterState (bool on)
{
    for (juce::Component* c : { static_cast<juce::Component*> (&cutoffSlider), static_cast<juce::Component*> (&filterModeCombo),
                                static_cast<juce::Component*> (&filterResKnob), static_cast<juce::Component*> (&filterDriveKnob),
                                static_cast<juce::Component*> (&filterEnvAmtKnob), static_cast<juce::Component*> (&filterVelKnob) })
    {
        c->setEnabled (on);
        c->setAlpha (on ? 1.0f : disabledAlpha);
    }
    for (auto* label : { &cutoffReadout, &filterResReadout, &filterDriveReadout, &filterEnvAmtReadout, &filterVelReadout })
        label->setAlpha (on ? 1.0f : disabledAlpha);
}

void NeonSynthEditor::setScanlines (bool on)
{
    scanlines.setVisible (on);
    scanlinesButton.setToggleState (on, juce::dontSendNotification);
    apvts.state.setProperty (neon::crt::scanlinesPropertyId, on, nullptr);
}

void NeonSynthEditor::setView (int view)
{
    view = juce::jlimit (0, 1, view);
    oscilloscope.setVisible (view == 0);
    spectrum.setVisible (view == 1);
    scopeButton.setToggleState (view == 0, juce::dontSendNotification);
    spectrumButton.setToggleState (view == 1, juce::dontSendNotification);
    apvts.state.setProperty (viewPropertyId, view, nullptr);
}

void NeonSynthEditor::setEnvTab (int tab)
{
    tab = juce::jlimit (0, 1, tab);
    const bool filter = tab == 1;
    for (juce::Component* c : { static_cast<juce::Component*> (&adsrView), static_cast<juce::Component*> (&attackKnob), static_cast<juce::Component*> (&decayKnob),
                                static_cast<juce::Component*> (&sustainKnob), static_cast<juce::Component*> (&releaseKnob),
                                static_cast<juce::Component*> (&attackReadout), static_cast<juce::Component*> (&decayReadout),
                                static_cast<juce::Component*> (&sustainReadout), static_cast<juce::Component*> (&releaseReadout) })
        c->setVisible (! filter);
    for (juce::Component* c : { static_cast<juce::Component*> (&filterAdsrView), static_cast<juce::Component*> (&filterAttackKnob), static_cast<juce::Component*> (&filterDecayKnob),
                                static_cast<juce::Component*> (&filterSustainKnob), static_cast<juce::Component*> (&filterReleaseKnob),
                                static_cast<juce::Component*> (&filterAttackReadout), static_cast<juce::Component*> (&filterDecayReadout),
                                static_cast<juce::Component*> (&filterSustainReadout), static_cast<juce::Component*> (&filterReleaseReadout) })
        c->setVisible (filter);
    ampEnvButton.setToggleState (! filter, juce::dontSendNotification);
    filterEnvButton.setToggleState (filter, juce::dontSendNotification);
    apvts.state.setProperty (envTabPropertyId, tab, nullptr);
    if (std::exchange (envTab, tab) != tab)
    {
        backdrop.invalidate();
        content.repaint (juce::Rectangle<int> (428, 356, 330, 274).expanded (4));   // the knob names and the panel's glow change colour
    }
}

int NeonSynthEditor::voiceCount() const
{
    return voiceCountValue != nullptr ? juce::roundToInt (voiceCountValue->load()) : NeonSynthProcessor::numVoices;
}

void NeonSynthEditor::updateReadouts()
{
    const auto set = [] (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);   // repaints only if the text changed
    };

    set (attackReadout,  neon::formatTime (attackValue->load()));
    set (decayReadout,   neon::formatTime (decayValue->load()));
    set (sustainReadout, juce::String (juce::roundToInt (sustainValue->load() * 100.0f)) + "%");
    set (releaseReadout, neon::formatTime (releaseValue->load()));
    set (cutoffReadout,  neon::formatHz (cutoffValue->load()));
    set (filterResReadout,   juce::String (juce::roundToInt (filterResValue->load())) + "%");
    set (filterDriveReadout, juce::String (juce::roundToInt (filterDriveValue->load())) + "%");
    // Rounded first, so a value a hair either side of 0 reads "0.00 oct" and not "-0.00 oct".
    const float envOctaves = (float) juce::roundToInt (filterEnvAmtValue->load() * 100.0f) / 100.0f;
    set (filterEnvAmtReadout, (envOctaves > 0.0f ? "+" : "") + juce::String (envOctaves, 2) + " oct");
    set (filterVelReadout,    juce::String (filterVelValue->load(), 2) + " oct");
    set (filterAttackReadout,  neon::formatTime (filterAttackValue->load()));
    set (filterDecayReadout,   neon::formatTime (filterDecayValue->load()));
    set (filterSustainReadout, juce::String (juce::roundToInt (filterSustainValue->load() * 100.0f)) + "%");
    set (filterReleaseReadout, neon::formatTime (filterReleaseValue->load()));

    const float gainDb = gainValue->load();
    set (gainReadout,  (gainDb > 0.0f ? "+" : "") + juce::String (gainDb, 1) + " dB");
    set (widthReadout, juce::String (juce::roundToInt (widthValue->load() * 100.0f)) + "%");

    // Shows the tempo actually in use: the host's while following it, otherwise the TEMPO slider.
    set (seqTempoReadout, synthProcessor.isSequencerFollowingHost()
                              ? "HOST " + juce::String (synthProcessor.getSequencerTempo(), 1)
                              : juce::String (seqTempoValue->load(), 1) + " BPM");
    set (seqSwingReadout, juce::String (juce::roundToInt (seqSwingValue->load() * 100.0f)) + "%");
    set (seqGateReadout,  juce::String (juce::roundToInt (seqGateValue->load() * 100.0f)) + "%");

    set (voiceCountReadout, voiceCount() == 1 ? juce::String ("MONO") : juce::String (voiceCount()) + " VOICES");

    // Modulation. Signed values are rounded first, so a hair either side of 0 reads "0.00", not "-0.00".
    const auto signedText = [] (float v, const char* unit)
    {
        const float rounded = (float) juce::roundToInt (v * 100.0f) / 100.0f;
        return (rounded > 0.0f ? "+" : "") + juce::String (rounded, 2) + unit;
    };
    const float lfoHz = lfoRateValue->load();
    set (lfoRateReadout,   lfoDivisionCombo.isVisible() ? juce::String ("to tempo")
                                                       : juce::String (lfoHz, lfoHz < 10.0f ? 2 : 1) + " Hz");
    set (lfoPitchReadout,  signedText (lfoPitchValue->load(), " st"));
    set (lfoCutoffReadout, signedText (lfoCutoffValue->load(), " oct"));
    set (lfoAmpReadout,    juce::String (juce::roundToInt (lfoAmpValue->load())) + "%");
    set (bendRangeReadout, juce::String (juce::CharPointer_UTF8 ("\xc2\xb1")) + juce::String (juce::roundToInt (bendRangeValue->load())) + " st");
    set (vibratoReadout,   juce::String (juce::roundToInt (vibratoValue->load())) + " ct");
    const int glideMs = juce::roundToInt (glideValue->load());
    set (glideReadout,     glideMs == 0 ? juce::String ("off") : glideMs < 1000 ? juce::String (glideMs) + " ms"
                                                                                : juce::String ((float) glideMs / 1000.0f, 2) + " s");

    const auto percent = [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; };
    set (smpRootReadout,      juce::MidiMessage::getMidiNoteName (juce::roundToInt (smpRootValue->load()), true, true, 4));
    set (smpStartReadout,     percent (smpStartValue->load()));
    set (smpLoopStartReadout, percent (smpLoopStartValue->load()));
    set (smpLoopEndReadout,   percent (smpLoopEndValue->load()));
    set (smpXfadeReadout,     juce::String (juce::roundToInt (smpXfadeValue->load())) + " ms");

    const float scan = grScanValue->load();
    set (grPosReadout,     percent (grPosValue->load()));
    set (grScanReadout,    scan == 0.0f ? juce::String ("frozen") : (scan > 0.0f ? "+" : "") + juce::String (scan, 2) + "x");
    set (grSizeReadout,    juce::String (juce::roundToInt (grSizeValue->load())) + " ms");
    set (grDensityReadout, juce::String (juce::roundToInt (grDensityValue->load())) + "/s");
    set (grSprayReadout,   percent (grSprayValue->load()));
    set (grJitterReadout,  juce::String (juce::roundToInt (grJitterValue->load())) + " ct");

    set (chopOvertoneReadout, juce::String (juce::roundToInt (chopOvertoneValue->load())) + "%");
    set (chopCombReadout,     "x" + juce::String (juce::roundToInt (chopCombValue->load())));
    set (noiseReadout,        juce::String (juce::roundToInt (noiseValue->load())) + "%");
    set (foldDriveReadout,    juce::String (foldDriveValue->load(), 1));
    set (foldMixReadout,      juce::String (juce::roundToInt (foldMixValue->load())) + "%");

    const auto status = synthProcessor.getSampleStatus();
    // Re-fitted only when the text or the label's size changes (it measures glyphs).
    const auto details = "  " + juce::String (status.seconds, 1) + " s  " + formatSampleRate (status.sampleRate);
    if (status.name + details != fittedFor || sampleNameLabel.getWidth() != fittedWidth)
    {
        fittedFor   = status.name + details;
        fittedWidth = sampleNameLabel.getWidth();
        set (sampleNameLabel, fitSampleName (status.name, details));
    }
    sampleNameLabel.setTooltip (status.path.isNotEmpty() ? status.path : status.name);
    set (sampleStatusLabel, status.loading ? juce::String ("loading...") : status.problem);
}

void NeonSynthEditor::timerCallback()
{
    const int n = synthProcessor.drainScope (scratchLeft.data(), scratchRight.data(), (int) scratchLeft.size());

    // Scope and spectrum show the mid signal (L + R) / 2.
    for (int i = 0; i < n; ++i)
        scratchMid[(size_t) i] = 0.5f * (scratchLeft[(size_t) i] + scratchRight[(size_t) i]);
    history.push (scratchMid.data(), n);

    meter.update (scratchLeft.data(), scratchRight.data(), n, timerHz);

    const auto& voices = synthProcessor.getVoiceViews();
    float lowestHz = 0.0f;
    for (const auto& v : voices)
    {
        const float hz = v.frequencyHz.load (std::memory_order_relaxed);
        if (v.active.load (std::memory_order_relaxed) && hz > 0.0f && (lowestHz == 0.0f || hz < lowestHz))
            lowestHz = hz;
    }

    const double sampleRate = synthProcessor.getSampleRate();
    if (oscilloscope.isVisible())
        oscilloscope.update (history, sampleRate, lowestHz);
    else
        spectrum.update (history, sampleRate, cutoffValue->load(), filterValue->load() >= 0.5f,
                         juce::roundToInt (filterModeValue->load()), filterResValue->load() * 0.01f);

    adsrView.update (attackValue->load(), decayValue->load(), sustainValue->load(), releaseValue->load(), &voices);
    filterAdsrView.update (filterAttackValue->load(), filterDecayValue->load(), filterSustainValue->load(),
                           filterReleaseValue->load(), nullptr);   // no voice dots: the voices report the amp envelope only
    voiceLeds.update (voices);
    voiceLeds.setLimit (voiceCount());

    const auto& modulation = synthProcessor.getModulation();
    const bool lfoActive = lfoPitchValue->load() != 0.0f || lfoCutoffValue->load() != 0.0f || lfoAmpValue->load() != 0.0f;
    lfoView.update ((Lfo::Shape) juce::jlimit (0, 4, juce::roundToInt (lfoShapeValue->load())),
                    modulation.getLfoPhase(), modulation.getLfoValue(), lfoActive);
    bendBar.update (modulation.getBendPosition());
    wheelBar.update (modulation.getWheelPosition());
    if (const auto band = cutoffBandNow(); band != cutoffBand)
    {
        cutoffBand = band;
        content.repaint (cutoffBandArea);
    }
    stepGrid.refresh();   // pattern + playhead
    sampleView.refresh(); // sample, markers, playheads
    shapeView.refresh();  // the sculpted cycle, CHOP's steps
    updateReadouts();
    synthProcessor.freeReplacedSamples();   // a sample replaced while notes played it, once they have ended
    patternRow.forgetHistoryIfStateLoaded();
}

// The processor's editor is made here (v1.11.1), so NeonSynthProcessor.cpp needs no GUI header.
juce::AudioProcessorEditor* NeonSynthProcessor::createEditor()
{
    return new NeonSynthEditor (*this);
}
