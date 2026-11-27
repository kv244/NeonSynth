// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <optional>
#include "dsp/Wavetable.h"
#include "state/StateFormat.h"

/**
    CUSTOM's harmonics and the drawn cycle (v1.12.0): what the session saves, the HARMONICS view
    and the SHAPE strip edit, and the wavetables the voices play.

    - The spectrum (32 levels and phases) and the drawing (256 points, or none) are read and
      written under a lock, by the message thread and by whichever thread the host saves or
      loads a session on. Never the audio thread.
    - Each new spectrum is built into wavetables and handed to the audio thread through a
      WavetableBank (lock-free); the audio thread calls acquireForAudio() once per block.
    - getVersion() counts changes, so views repaint when it moves.

    Saved as (written only when they differ from a new session's, so a session that never used
    CUSTOM is saved exactly as before):
        <Harmonics levels="32 numbers" phases="32 numbers"/>
        <Drawing points="256 numbers"/>
*/
class FourierState
{
public:
    static constexpr int drawingPoints = 256;
    using Drawing = std::array<float, drawingPoints>;

    FourierState() : spectrum (HarmonicSpectrum::defaultSpectrum()), bank (spectrum) {}

    HarmonicSpectrum getSpectrum() const
    {
        const juce::ScopedLock sl (lock);
        return spectrum;
    }

    /** Sanitises, stores and hands the new tables to the voices. Returns false if nothing changed. */
    bool setSpectrum (HarmonicSpectrum newSpectrum)
    {
        newSpectrum.sanitise();
        const juce::ScopedLock sl (lock);
        if (newSpectrum == spectrum)
            return false;
        spectrum = newSpectrum;
        bank.publish (spectrum);
        version.fetch_add (1, std::memory_order_release);
        return true;
    }

    std::optional<Drawing> getDrawing() const
    {
        const juce::ScopedLock sl (lock);
        return drawing;
    }

    /** Stores the drawn cycle (values -1..1; others clamped, non-finite ones 0), or clears it. */
    void setDrawing (std::optional<Drawing> newDrawing)
    {
        if (newDrawing.has_value())
            for (auto& v : *newDrawing)
                v = std::isfinite (v) ? juce::jlimit (-1.0f, 1.0f, v) : 0.0f;
        const juce::ScopedLock sl (lock);
        drawing = newDrawing;
        version.fetch_add (1, std::memory_order_release);
    }

    /** Counts changes to the spectrum or the drawing. Any thread. */
    int getVersion() const noexcept   { return version.load (std::memory_order_acquire); }

    /** The newest finished tables. Audio thread only, once per block. */
    const WavetableSet& acquireForAudio() noexcept   { return bank.acquire(); }

    //==============================================================================
    /** Appends <Harmonics> and <Drawing> to a session's tree where they differ from a new session's. */
    void writeTo (juce::ValueTree& parent) const
    {
        const juce::ScopedLock sl (lock);
        if (spectrum != HarmonicSpectrum::defaultSpectrum())
            parent.appendChild (harmonicsTree (spectrum), nullptr);
        if (drawing.has_value())
        {
            juce::ValueTree d (StateFormat::drawingId);
            d.setProperty (StateFormat::drawingPointsId, numbersToString (drawing->data(), drawingPoints), nullptr);
            parent.appendChild (d, nullptr);
        }
    }

    /**
        Reads them back from a session's tree. A missing or damaged element (a wrong count,
        text that is not numbers) gives a new session's value, so a session from before v1.12
        loads with the SAW set and no drawing.
    */
    void readFrom (const juce::ValueTree& parent)
    {
        HarmonicSpectrum s = HarmonicSpectrum::defaultSpectrum();
        const auto h = parent.getChildWithName (StateFormat::harmonicsId);
        HarmonicSpectrum parsed;
        if (h.isValid()
            && stringToNumbers (h.getProperty (StateFormat::harmonicLevelsId).toString(), parsed.levels.data(), HarmonicSpectrum::numHarmonics)
            && stringToNumbers (h.getProperty (StateFormat::harmonicPhasesId).toString(), parsed.phases.data(), HarmonicSpectrum::numHarmonics))
            s = parsed;

        std::optional<Drawing> d;
        const auto dt = parent.getChildWithName (StateFormat::drawingId);
        Drawing points {};
        if (dt.isValid() && stringToNumbers (dt.getProperty (StateFormat::drawingPointsId).toString(), points.data(), drawingPoints))
            d = points;

        setSpectrum (s);
        setDrawing (d);
    }

    /** A spectrum as the <Harmonics> element sessions save (also used for factory presets). */
    static juce::ValueTree harmonicsTree (const HarmonicSpectrum& s)
    {
        juce::ValueTree h (StateFormat::harmonicsId);
        h.setProperty (StateFormat::harmonicLevelsId, numbersToString (s.levels.data(), HarmonicSpectrum::numHarmonics), nullptr);
        h.setProperty (StateFormat::harmonicPhasesId, numbersToString (s.phases.data(), HarmonicSpectrum::numHarmonics), nullptr);
        return h;
    }

private:
    static juce::String numbersToString (const float* values, int count)
    {
        juce::StringArray parts;
        for (int i = 0; i < count; ++i)
            parts.add (juce::String ((double) values[i], 8, true));   // 9 significant digits: reads back as the same float
        return parts.joinIntoString (" ");
    }

    /** Exactly `count` numbers, separated by spaces; false (and `out` untouched) otherwise. */
    static bool stringToNumbers (const juce::String& text, float* out, int count)
    {
        juce::StringArray parts;
        parts.addTokens (text, " ", "");
        parts.removeEmptyStrings();
        if (parts.size() != count)
            return false;
        std::array<float, drawingPoints> values {};
        for (int i = 0; i < count; ++i)
        {
            const auto& p = parts[i];
            if (! p.containsOnly ("0123456789+-.eE"))
                return false;
            values[(size_t) i] = p.getFloatValue();
        }
        std::copy (values.begin(), values.begin() + count, out);
        return true;
    }

    juce::CriticalSection lock;
    HarmonicSpectrum spectrum;
    std::optional<Drawing> drawing;
    WavetableBank bank;
    std::atomic<int> version { 0 };
};
