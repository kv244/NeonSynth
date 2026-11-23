// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include <array>

/*
    The displays' view of the output: a rolling copy of the newest samples from the scope
    FIFO, and how much longer the display windows are at high sample rates. Message thread.
*/

//==============================================================================
/** The largest displayRateFactor() (192 kHz): the views size their buffers for it. */
constexpr int maxDisplayRateFactor = 4;

/**
    How much longer the displays' windows are at this sample rate than at 48 kHz:
    1 up to 48 kHz, 2 at 88.2 and 96 kHz, 4 at 176.4 and 192 kHz. Scaling by it
    keeps the scope showing the same time span, and the spectrum the same
    frequency resolution, at every sample rate.
*/
inline int displayRateFactor (double sampleRate) noexcept
{
    // Between the standard rates, so 88.2 and 96 kHz double the windows and 176.4 and 192 kHz quadruple them.
    constexpr double doubleAbove = 60000.0, quadrupleAbove = 120000.0;
    return sampleRate > quadrupleAbove ? maxDisplayRateFactor : sampleRate > doubleAbove ? 2 : 1;
}

/** Rolling copy of the most recent output samples, filled from the scope FIFO. */
class SignalHistory
{
public:
    static constexpr int size = 1 << 15;   // power of two; holds the largest FFT (2^14, at 192 kHz) twice over

    void push (const float* samples, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            data[(size_t) writePos] = samples[i];
            writePos = (writePos + 1) & (size - 1);
        }
    }

    /** Copies the newest n samples (n <= size) into dest, oldest first. */
    void copyLatest (float* dest, int n) const noexcept
    {
        const int start = (writePos - n) & (size - 1);
        for (int i = 0; i < n; ++i)
            dest[i] = data[(size_t) ((start + i) & (size - 1))];
    }

private:
    std::array<float, size> data {};
    int writePos = 0;
};
