// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "gui/SampleView.h"

SampleView::SampleView (NeonSynthProcessor& p, juce::AudioProcessorValueTreeState& state)
    : processor (p), apvts (state)
{
    startValue     = apvts.getRawParameterValue ("smpStart");
    loopStartValue = apvts.getRawParameterValue ("smpLoopStart");
    loopEndValue   = apvts.getRawParameterValue ("smpLoopEnd");
    loopModeValue  = apvts.getRawParameterValue ("smpLoop");
    sourceValue    = apvts.getRawParameterValue ("source");
    grainPosValue  = apvts.getRawParameterValue ("grPos");
    grainSprayValue = apvts.getRawParameterValue ("grSpray");
    setTooltip ("Sample: drag the cyan line (start) or the amber brackets (loop); click to set the start. "
                "Granular: drag to move the cloud (magenta line). Drop a WAV, AIFF or FLAC file anywhere on the synth to load it.");
}

void SampleView::refresh()
{
    bool changed = false;

    // A new sample: rebuild the min/max overview (from the 1/4-rate copy: plenty for a few hundred pixels).
    processor.withCurrentSample ([&] (const SampleData* sample)
    {
        const auto hash = sample != nullptr ? sample->getHash() : juce::String();
        if (hash == shownHash && sample != nullptr && (bool) sample->getOrigin().isDefault == shownDefault)
            return;

        shownHash = hash;
        shownDefault = sample != nullptr && sample->getOrigin().isDefault;
        peaks.assign (numBuckets, { 0.0f, 0.0f });

        if (sample != nullptr)
        {
            const int level = SampleData::numLevels - 1;
            const int n = sample->getLength (level);
            const float* data = sample->getData (level);
            for (int b = 0; b < numBuckets; ++b)
            {
                const int from = (int) ((juce::int64) b * n / numBuckets);
                const int to   = juce::jmax (from + 1, (int) ((juce::int64) (b + 1) * n / numBuckets));
                float lo = 0.0f, hi = 0.0f;
                for (int i = from; i < juce::jmin (to, n); ++i)
                {
                    lo = juce::jmin (lo, data[i]);
                    hi = juce::jmax (hi, data[i]);
                }
                peaks[(size_t) b] = { lo, hi };
            }
        }
        changed = true;
        waveform.invalidate();
    });

    const Markers m { startValue->load(), loopStartValue->load(), loopEndValue->load(),
                      (int) loopModeValue->load(), (NeonSynthVoice::Source) (int) sourceValue->load(),
                      grainPosValue->load(), grainSprayValue->load() };
    if (! (m == markers)) { markers = m; changed = true; }

    std::array<float, NeonSynthProcessor::numVoices> heads {};
    const auto& views = processor.getVoiceViews();
    for (size_t i = 0; i < heads.size(); ++i)
        heads[i] = views[i].samplePosition.load (std::memory_order_relaxed);
    if (heads != playheads) { playheads = heads; changed = true; }

    std::array<float, numShownGrains> grainHeads {};
    for (size_t v = 0; v < views.size(); ++v)
        for (size_t g = 0; g < views[v].grainPositions.size(); ++g)
            grainHeads[v * views[v].grainPositions.size() + g] = views[v].grainPositions[g].load (std::memory_order_relaxed);
    if (grainHeads != grains) { grains = grainHeads; changed = true; }

    if (changed)
        repaint();
}

void SampleView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const bool active = markers.source == NeonSynthVoice::Source::sample
                     || markers.source == NeonSynthVoice::Source::granular;   // dimmed while the oscillator or Random plays
    const float alpha = active ? 1.0f : 0.35f;
    const auto inner = bounds.reduced (4.0f, 3.0f);
    const float mid = inner.getCentreY(), halfHeight = inner.getHeight() * 0.5f;

    // The inset and the waveform change only with the sample (or the dimming): drawn once, then
    // copied, while the markers and playheads on top move.
    if (std::exchange (waveformActive, active) != active)
        waveform.invalidate();
    waveform.paint (g, getLocalBounds(), [&] (juce::Graphics& wg)
    {
        neon::drawPanel (wg, bounds, neon::cyan);
        wg.setColour (neon::cyan.withAlpha (0.14f * alpha));
        wg.fillRect (juce::Rectangle<float> (inner.getX(), std::round (mid), inner.getWidth(), 1.0f));   // the zero line

        // One min/max line per bucket: a faint wide pass (the phosphor's halo), then the trace
        // (The buckets sit closer than a unit apart, so the core stays mostly hue: near-white
        // there would merge into one pale block.)
        for (const auto& [width, colour] : { std::pair { 2.0f, neon::cyan.withAlpha (0.10f * alpha) },
                                             std::pair { 1.0f, neon::cyan.interpolatedWith (juce::Colours::white, 0.25f).withAlpha (0.6f * alpha) } })
        {
            wg.setColour (colour);
            for (int b = 0; b < numBuckets; ++b)
            {
                const float x = inner.getX() + inner.getWidth() * ((float) b + 0.5f) / (float) numBuckets;
                const auto [lo, hi] = peaks[(size_t) b];
                wg.fillRect (juce::Rectangle<float> (x - width * 0.5f, mid - hi * halfHeight, width, (hi - lo) * halfHeight + 1.0f));
            }
        }
    });

    const bool granular = markers.source == NeonSynthVoice::Source::granular;

    // Granular: the band Spray can reach around Position
    if (granular)
    {
        const float half = 0.5f * markers.spray;
        const float x0 = xFor (markers.grainPos - half), x1 = xFor (markers.grainPos + half);
        g.setColour (neon::magenta.withAlpha (0.14f));
        g.fillRect (juce::Rectangle<float> (x0, inner.getY(), juce::jmax (1.0f, x1 - x0), inner.getHeight()));
    }

    // Loop region
    if (! granular && markers.loopMode != SampleReader::oneShot)
    {
        const float x0 = xFor (markers.loopStart), x1 = xFor (juce::jmax (markers.loopStart, markers.loopEnd));
        g.setColour (neon::amber.withAlpha (0.12f * alpha));
        g.fillRect (juce::Rectangle<float> (x0, inner.getY(), x1 - x0, inner.getHeight()));
        g.setColour (neon::amber.withAlpha (0.25f * alpha));   // the brackets' glow
        drawBracket (g, x0, inner, true, 2.0f);
        drawBracket (g, x1, inner, false, 2.0f);
        g.setColour (neon::phosphorOf (neon::amber).withAlpha (0.9f * alpha));
        drawBracket (g, x0, inner, true, 0.0f);
        drawBracket (g, x1, inner, false, 0.0f);
    }

    if (granular)
    {
        // Position, and a short tick per sounding grain
        neon::drawGlowLine (g, xFor (markers.grainPos), inner.getY(), inner.getBottom(), neon::magenta);
        g.setColour (neon::crt::phosphor.withAlpha (0.9f));
        for (const float grain : grains)
            if (grain >= 0.0f)
                g.fillRect (juce::Rectangle<float> (xFor (grain) - 0.5f, mid - halfHeight * 0.45f, 1.5f, halfHeight * 0.9f));
        return;
    }

    // Start marker
    neon::drawGlowLine (g, xFor (markers.start), inner.getY(), inner.getBottom(), neon::cyan.withAlpha (alpha));

    // Playheads
    for (const float head : playheads)
        if (head >= 0.0f)
            neon::drawGlowLine (g, std::floor (xFor (head)) + 0.5f, inner.getY(), inner.getBottom(),
                                neon::crt::phosphor.withAlpha (0.85f * alpha), 1.0f);
}

void SampleView::mouseDown (const juce::MouseEvent& e)
{
    const float x = (float) e.x;
    const float grab = 8.0f;
    const bool granular = markers.source == NeonSynthVoice::Source::granular;
    const bool loopShown = ! granular && markers.loopMode != SampleReader::oneShot;   // as paint() draws the brackets

    dragging = granular ? "grPos" : "smpStart";   // Granular: anywhere moves the cloud
    if (loopShown && std::abs (x - xFor (markers.loopStart)) < grab)
        dragging = "smpLoopStart";
    else if (loopShown && std::abs (x - xFor (markers.loopEnd)) < grab)
        dragging = "smpLoopEnd";

    if (auto* param = apvts.getParameter (dragging))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (fractionAt (x));
    }
}

void SampleView::mouseDrag (const juce::MouseEvent& e)
{
    if (auto* param = apvts.getParameter (dragging))
        param->setValueNotifyingHost (fractionAt ((float) e.x));
}

void SampleView::mouseUp (const juce::MouseEvent&)
{
    if (auto* param = apvts.getParameter (dragging))
        param->endChangeGesture();
    dragging = {};
}

float SampleView::xFor (float fraction) const noexcept
{
    const auto inner = getLocalBounds().toFloat().reduced (4.0f, 3.0f);
    return inner.getX() + inner.getWidth() * juce::jlimit (0.0f, 1.0f, fraction);
}

float SampleView::fractionAt (float x) const noexcept
{
    const auto inner = getLocalBounds().toFloat().reduced (4.0f, 3.0f);
    return juce::jlimit (0.0f, 1.0f, (x - inner.getX()) / juce::jmax (1.0f, inner.getWidth()));
}

void SampleView::drawBracket (juce::Graphics& g, float x, juce::Rectangle<float> r, bool opening, float grow)
{
    const float tick = opening ? 5.0f : -5.0f;
    g.fillRect (juce::Rectangle<float> (x - 1.0f, r.getY(), 2.0f, r.getHeight()).expanded (grow, 0.0f));
    g.fillRect (juce::Rectangle<float> (juce::jmin (x, x + tick), r.getY(), std::abs (tick), 2.0f).expanded (grow, grow * 0.5f));
    g.fillRect (juce::Rectangle<float> (juce::jmin (x, x + tick), r.getBottom() - 2.0f, std::abs (tick), 2.0f).expanded (grow, grow * 0.5f));
}
