// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "plugin/NeonSynthProcessor.h"
#include "dsp/Sculpt.h"
#include "dsp/CycleAnalysis.h"
#include "gui/NeonStyle.h"

/**
    One cycle of the Oscillator or Random wave as it leaves the Fold stage
    (v1.6.0), shown in the sample strip's place while either source plays.

    - The wave is drawn with CHOP and Fold applied; Noise is left out, since a
      single drawn cycle of noise would mislead more than it shows.
    - CHOP's 16 x Comb steps are marked along the cycle, the chopped ones shaded
      (faintly while Overtone is 0, when chopping changes nothing).
    - Click a step to toggle it; drag across steps to set them all the same way.
      Steps repeat with Comb, so a click toggles every copy of that step.
    - With Random, the drawing follows the 16 levels of the lowest sounding
      Random voice, which drift as Random re-rolls them; with none sounding it
      shows a fixed example set.

    It draws the ideal wave (no band-limiting): this is a picture of the shape,
    not of the exact samples. refresh() is called by the editor's 30 Hz timer
    and repaints only when something shown has changed. Message thread only.

    v1.12.0, CUSTOM and DRAW:
    - With CUSTOM on (and the Oscillator source) the wave is CUSTOM's: its spectrum's first
      HARMONICS harmonics, scaled as the voices' tables are, then CHOP and Fold as before.
    - If a cycle has been drawn, the drawn line is shown faintly behind that wave, so the
      rounding and ripple of 32 harmonics at a sharp corner (Gibbs) can be seen.
    - With DRAW on as well, the pointer draws the cycle instead of toggling CHOP's steps: a
      drag sets the 256 drawing points under it (filling those a fast drag skipped), and on
      release the drawing is analysed into 32 harmonics and handed to the processor once.
      A double-click clears the drawing back to a sine. With DRAW or CUSTOM off, or with
      Random, clicks toggle CHOP's steps as before.
*/
class ShapeView : public juce::Component,
                  public juce::SettableTooltipClient
{
public:
    ShapeView (NeonSynthProcessor& p, juce::AudioProcessorValueTreeState& state);

    /**
        v1.12.0, DRAW (the editor's switch, saved with the editor's view): with DRAW on and
        CUSTOM on, the pointer draws one cycle instead of toggling CHOP's steps.
    */
    void setDrawMode (bool on);

    bool isDrawMode() const noexcept   { return drawMode; }

    /** True while the pointer draws the cycle: DRAW and CUSTOM on, with the Oscillator source (v1.12.0). */
    bool isDrawing() const noexcept;

    /** Re-reads the settings and, for Random, the sounding levels. Message thread. */
    void refresh();

    void paint (juce::Graphics& g) override;

    void mouseDown (const juce::MouseEvent& e) override;

    void mouseDrag (const juce::MouseEvent& e) override;

    /** v1.12.0: a drawing is analysed and handed over once, when the drag ends. */
    void mouseUp (const juce::MouseEvent&) override;

    /** v1.12.0: while drawing, a double-click clears the drawing back to a sine. */
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    static_assert (CycleAnalysis::numPoints == FourierState::drawingPoints, "a drawing is one analysed cycle");

    static constexpr const char* chopTooltip = "One cycle of the wave after CHOP and Fold. Click a step to chop it (or not); "
                                               "drag across steps to set several. OVERTONE sets how far chopped steps are cut.";
    static constexpr const char* drawTooltip = "Draw one cycle; it plays as its first 32 harmonics. "
                                               "Drag across the strip to draw; double-click for a sine.";
    static constexpr const char* drawCaption = "Draw one cycle; it plays as its first 32 harmonics";
    static constexpr const char* needsCustomCaption = "DRAW: switch CUSTOM on to draw a cycle";

    /** Points of CUSTOM's wave kept for drawing it; the strip is narrower than this, so the line stays smooth. */
    static constexpr int customWavePoints = 1024;

    /** A drawing point (0..255) and value (-1..1) under a position, clamped to the strip. */
    struct DrawPoint { int index = 0; float value = 0.0f; };

    DrawPoint pointAt (juce::Point<float> position) const noexcept;

    /** The cycle now playing (before CHOP and Fold) at the drawing's points, to start a drawing from. */
    CycleAnalysis::Cycle playingCycle() const noexcept;

    bool drawMode = false;   // v1.12.0
    struct Shown
    {
        bool random = false;
        int waveform = 0, comb = 1;
        float overtone = 0.0f, drive = 3.0f, foldMix = 0.0f;
        juce::uint16 pattern = 0;
        std::array<float, RandomStepOscillator::numSteps> levels {};
        bool custom = false, drawing = false;      // v1.12.0
        int harmonics = HarmonicSpectrum::numHarmonics;
        int fourierVersion = -1;                   // -1: not read yet

        bool operator== (const Shown& o) const noexcept
        {
            return random == o.random && waveform == o.waveform && comb == o.comb && overtone == o.overtone
                && drive == o.drive && foldMix == o.foldMix && pattern == o.pattern && levels == o.levels
                && custom == o.custom && drawing == o.drawing && harmonics == o.harmonics && fourierVersion == o.fourierVersion;
        }
    };

    /** The ideal wave at a phase, with CHOP's gain and Fold applied (no band-limiting, no noise). */
    float valueAt (float phase) const noexcept;

    /** The pattern step (0..15) under x, or -1 outside the drawing. */
    int stepAt (float x) const noexcept;

    NeonSynthProcessor& processor;
    std::atomic<float>* sourceValue = nullptr;
    std::atomic<float>* waveformValue = nullptr;
    std::atomic<float>* overtoneValue = nullptr;
    std::atomic<float>* combValue = nullptr;
    std::atomic<float>* driveValue = nullptr;
    std::atomic<float>* foldMixValue = nullptr;
    std::atomic<float>* customValue = nullptr;      // v1.12.0
    std::atomic<float>* harmonicsValue = nullptr;

    std::array<float, RandomStepOscillator::numSteps> exampleLevels {};
    Shown shown;
    bool paintChopped = true;
    int  lastStep = -1;

    // v1.12.0: CUSTOM's spectrum and drawing as last read (when getFourierVersion() moved), the
    // wave they play as, and the drawing in progress.
    HarmonicSpectrum spectrum = HarmonicSpectrum::defaultSpectrum();
    std::optional<FourierState::Drawing> drawing;
    float scale = HarmonicSpectrum::seriesGain;
    std::array<float, customWavePoints> customWave {};
    CycleAnalysis::Cycle pending {};
    bool dragging = false;
    DrawPoint lastPoint;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShapeView)
};
