// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include "state/SampleData.h"
#include "dsp/SampleReader.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// SIMD paths for the grain loop. x86-64 always has SSE2; AVX2 is compiled in too
// but only used when the CPU reports it (checked once, at run time), so one binary
// runs everywhere. Other CPUs (ARM) use the scalar loop.
#if defined (__SSE2__) || defined (_M_X64) || defined (_M_AMD64)
 #include <immintrin.h>
 #define NEONSYNTH_GRAINS_SSE2 1
 #define NEONSYNTH_GRAINS_AVX2 1
 #if defined (__GNUC__) || defined (__clang__)
  #define NEONSYNTH_TARGET_AVX2 __attribute__ ((target ("avx2")))   // GCC/Clang: allow AVX2 in this function only
 #else
  #define NEONSYNTH_TARGET_AVX2                                     // MSVC allows AVX2 intrinsics anywhere
 #endif
#else
 #define NEONSYNTH_GRAINS_SSE2 0
 #define NEONSYNTH_GRAINS_AVX2 0
#endif

#ifndef NEONSYNTH_GRAINS_AVX2_GATHER
 #define NEONSYNTH_GRAINS_AVX2_GATHER 1   // automatic AVX2 reads with vgatherdps (1) or with scalar loads (0)
#endif

/**
    @file GrainCloud.h

    The granular source for one voice: short windowed grains read from
    the sample near a moving position, summed into a cloud.

    Scheduling. A countdown starts a grain every 1 / Density seconds,
    jittered by +-30% so dense clouds don't buzz at the density rate.

    Each grain, when it starts, takes
      - a start point: the voice's scan head +- Spray x half the file
        (Spray 1 reaches across the whole file), kept inside the sample;
      - a length: Size (5 - 500 ms of output);
      - a pitch: the note's ratio to the root, as in SampleReader, +- Pitch
        jitter (up to 100 cents);
      - a window: (4x(1-x))^2, a Hann-shaped bell (it differs from
        sin^2(pi x) by at most 0.065, near x = 0.22 and 0.78; by 0.0625 at
        x = 0.25), smooth with zero slope at both ends, computed in four multiplies.
    Later knob changes affect the next grains, never one that is playing, so
    turning a knob can't click.

    The scan head starts at Position and moves at Scan x real time (-2x to
    +2x of the file's own speed; 0 freezes it), wrapping at the file's ends.
    Position itself stays live: moving it moves the whole cloud.

    Level. Grains overlap by about Density x Size; the sum is scaled by
    1 / sqrt(max(1, overlap)) so thin and dense clouds sound about equally
    loud. The output soft clipper still catches peaks.

    Pitch modulation (v1.9.0). setPitchRatio() multiplies the pitch (bend,
    vibrato, LFO). A grain starting then takes the note's ratio times it, on
    the copy that suits that pitch; grains already sounding follow it too,
    their step scaled by the ratio's change since they started, once per
    render() call (the voice renders in chunks of up to 32 samples while the
    pitch moves). The step is a whole number, worked out before the grain
    loop, so the scalar and SIMD loops stay bit-identical; and at ratio 1
    every step is exactly what it was before v1.9.0.

    A grain bent up so far that it would read past the end of its copy ends
    there (v1.11.1): it stops before the first sample whose read position
    reaches the end. Until v1.11.0 its step shrank to fit the rest of each render() call
    instead (frozen at 0 once it got there), which made the result depend on
    the block size. A grain that stays inside its copy is unchanged: without
    pitch modulation none ever reaches the end (each is sized to fit).

    Voices. Up to 32 grains at once; an onset is skipped while all 32 are
    busy (stealing one would click). Notes pitched well above the root read
    the sample's band-limited copies, as in SampleReader.

    Determinism. Randomness comes from a xorshift generator seeded per note
    (from the note, the voice and a counter the processor resets with the
    voices), so the same MIDI renders the same audio.

    Holds a reference on the sample from begin() to end(). Audio thread only:
    no allocation, no locks.

    Non-finite inputs (v1.11.1). begin(), setPitchRatio() and setSettings()
    ignore a NaN or infinite value (begin() refuses a non-finite sample rate;
    a non-finite pitch ratio or setting keeps the value it replaces). Only a
    bug could send one, but a NaN in a grain's step or window would silence
    the voice for good. Finite values pass through untouched.
*/
class GrainCloud
{
public:
    static constexpr int maxGrains = 32;

    // The ranges the grain settings are held to (the parameters' ranges): Density in grains per
    // second, Size in seconds. Size is clamped in float for the level and in double (the same
    // decimal values, not the floats widened) where it sets a grain's length; both kept as they were.
    static constexpr float  minDensity = 1.0f;
    static constexpr float  maxDensity = 200.0f;
    static constexpr float  minSizeSeconds = 0.005f;
    static constexpr float  maxSizeSeconds = 0.5f;
    static constexpr double minSizeSecondsDouble = 0.005;
    static constexpr double maxSizeSecondsDouble = 0.5;

    /** Per-block settings from the Grain parameters. */
    struct Settings
    {
        float position = 0.5f;       ///< 0..1 of the sample
        float scan = 0.0f;           ///< -2..2 x the file's own speed
        float sizeSeconds = 0.08f;   ///< 0.005..0.5
        float density = 30.0f;       ///< grains per second, 1..200
        float spray = 0.1f;          ///< 0..1
        float jitterCents = 0.0f;    ///< 0..100
    };

    GrainCloud() = default;

    GrainCloud (const GrainCloud&) = delete;
    GrainCloud& operator= (const GrainCloud&) = delete;
    ~GrainCloud() = default;   // never touches the sample: at shutdown it may already be gone

    /**
        Starts a note. Grains already sounding from the previous note on this
        voice play out (they are windowed, so a retrigger never clicks), unless
        the sample itself has changed. Returns false if there is no sample.
    */
    bool begin (const SampleData* newSample, int midiNote, int rootNote, double hostSampleRate,
                const Settings& newSettings, juce::uint32 seed) noexcept
    {
        if (newSample == nullptr || newSample->getLength() <= 0 || ! (hostSampleRate > 0.0) || ! std::isfinite (hostSampleRate))
        {
            end();
            return false;
        }

        if (newSample != sample)
        {
            end();
            sample = newSample;
            sample->users.fetch_add (1);
        }

        hostRate = hostSampleRate;
        rng = seed != 0 ? seed : goldenRatioSeed;

        const double ratio = std::pow (2.0, (midiNote - rootNote) / 12.0) * sample->getSampleRate() / hostRate;
        level = 0;
        while (level < SampleData::numLevels - 1 && ratio / (double) (1 << level) > SampleReader::maxFullRateIncrement)
            ++level;
        noteIncrement = ratio / (double) (1 << level);
        noteRatio = ratio;

        settings = finiteOr (newSettings, settings);
        scanOffset = 0.0;
        samplesToNextGrain = 0;   // the first grain starts at once
        spawning = true;
        return true;
    }

    /** Starts no more grains; those sounding play out (the voice moved to another source). */
    void stopSpawning() noexcept   { spawning = false; }

    /** False once it has stopped spawning and its last grain has ended (it can then be end()ed). */
    bool isSounding() const noexcept   { return sample != nullptr && (spawning || numActive > 0); }

    /**
        The pitch multiplier for the next render() calls (v1.9.0): new grains
        start at it, sounding ones follow it. 1 = the note's own pitch.
    */
    void setPitchRatio (float ratio) noexcept
    {
        if (std::isfinite (ratio))   // a NaN or infinite ratio has no whole-number step
            pitchRatio = ratio;
    }

    /** Settings for the grains that start from now on (Position and Scan act immediately). */
    void setSettings (const Settings& newSettings) noexcept   { settings = finiteOr (newSettings, settings); }

    /** The settings set last: begin() takes them for a new note. */
    const Settings& getSettings() const noexcept   { return settings; }

    /** Ends the note at once: drops every grain and releases the sample. Safe when idle. */
    void end() noexcept
    {
        numActive = 0;
        spawning = false;
        if (sample != nullptr)
            sample->users.fetch_sub (1);
        sample = nullptr;
    }

    bool isActive() const noexcept           { return sample != nullptr; }
    int getNumActiveGrains() const noexcept  { return numActive; }
    juce::int64 getGrainsStarted() const noexcept   { return grainsStarted; }
    int getLevel() const noexcept            { return level; }

    /** Read position (0..1) of up to `max` sounding grains, for display; returns how many. */
    int getGrainPositions (float* dest, int max) const noexcept
    {
        if (dest == nullptr)
            return 0;

        const int n = juce::jmin (max, numActive);
        for (int i = 0; i < n; ++i)
            dest[i] = (float) ((double) grains[(size_t) i].position * (1.0 / fixedPointOne)) * grains[(size_t) i].lengthScale;
        return n;
    }

    /**
        Renders `numSamples` of the cloud into `out` (overwriting it). Grains
        start on their exact sample: the block is split at each onset, and
        between onsets every grain renders its stretch in one tight loop.
    */
    void render (float* out, int numSamples) noexcept
    {
        if (numSamples <= 0)
            return;

        std::fill (out, out + numSamples, 0.0f);
        if (sample == nullptr)
            return;

        // One path for the whole block: forceSimdPath() from another thread can't switch it
        // halfway, and the atomic is read once rather than once per grain and segment.
        const SimdPath path = activeSimdPath();

        int pos = 0;
        while (pos < numSamples)
        {
            if (spawning && samplesToNextGrain <= 0)
            {
                startGrain();
                const double interval = hostRate / (double) juce::jlimit (minDensity, maxDensity, settings.density);
                samplesToNextGrain = juce::jmax (1, (int) (interval * (minOnsetScale + onsetScaleRange * random01())));
            }

            const int segment = spawning ? juce::jmin (numSamples - pos, samplesToNextGrain) : numSamples - pos;

            for (int i = 0; i < numActive;)
            {
                if (renderGrain (grains[(size_t) i], out + pos, segment, path))
                    ++i;
                else
                    grains[(size_t) i] = grains[(size_t) --numActive];   // finished: swap in the last one
            }

            // The scan head moves through the file at Scan x its own speed.
            scanOffset += (double) settings.scan * (double) segment / (sample->getLengthSeconds() * hostRate);
            scanOffset -= std::floor (scanOffset);

            samplesToNextGrain -= segment;
            pos += segment;
        }

        const float overlap = juce::jlimit (minSizeSeconds, maxSizeSeconds, settings.sizeSeconds) * juce::jlimit (minDensity, maxDensity, settings.density);
        const float gain = 1.0f / std::sqrt (juce::jmax (1.0f, overlap));
        for (int i = 0; i < numSamples; ++i)
            out[i] *= gain;
    }

    /** One sample (for tests; the voice renders whole blocks). */
    float nextSample() noexcept
    {
        float x[8];   // render() writes only x[0]. Eight floats keep GCC's array-bounds check quiet
        render (x, 1);   // about the SSE2 (4 wide) and AVX2 (8 wide) paths, which a one-sample call never enters
        return x[0];
    }

private:
    /** 1.0 in the grains' 32.32 fixed-point positions and steps: 2^32. */
    static constexpr double fixedPointOne = 4294967296.0;

    /** Onsets are spaced by the interval times 0.7 .. 1.3 (+-30%), so dense clouds don't buzz at the density rate. */
    static constexpr double minOnsetScale = 0.7;
    static constexpr double onsetScaleRange = 0.6;

    /** A grain is at least this many samples long (its window needs a start and an end). */
    static constexpr int minGrainSamples = 2;

    /** Source samples kept clear at the end of a copy: the cubic reads up to x[2] past a position. */
    static constexpr double interpolationMargin = 3.0;

    /** The seed used for a seed of 0 (xorshift would stay at 0 for ever): 2^32 / golden ratio. */
    static constexpr juce::uint32 goldenRatioSeed = 0x9e3779b9u;

    /**
        A grain's read position is 32.32 fixed point (sample index in the top
        32 bits): stepping it is one integer add, so nothing slow sits on the
        loop-carried path, and its fraction becomes a float by bit
        manipulation rather than a conversion (see fractionOf). The window phase is age x windowStep,
        computed afresh for every sample rather than accumulated, so the
        scalar, SSE2 and AVX2 loops produce bit-identical results whatever
        order they work through the samples in (an accumulated phase would
        round differently in each).
    */
    struct Grain
    {
        const float* data = nullptr;   // the copy (level) it reads: grains outlive a retrigger to another pitch
        juce::uint64 position = 0, step = 0;
        int   age = 0;                 // samples rendered so far: the window phase is age x windowStep
        float windowStep = 0.0f;       // 1 / length
        int   remaining = 0;           // output samples left
        float lengthScale = 1.0f;      // 1 / that copy's length, for display
        float pitch = 1.0f;            // the pitch ratio it started at: step is right at that ratio
        juce::uint64 end = 0;          // its copy's length in 32.32: a raised pitch must not read past it
    };

    /**
        The fractional part of a 32.32 position as a float in [0, 1), built
        from its bits (top 23 fraction bits as the mantissa of a float in
        [1, 2), minus 1). Avoids an int-to-float conversion, which on x64
        only partly writes its register and so chains the loop's iterations.
    */
    static float fractionOf (juce::uint64 position) noexcept
    {
        const juce::uint32 bits = 0x3f800000u | ((juce::uint32) position >> 9);
        float f;
        std::memcpy (&f, &bits, sizeof (f));
        return f - 1.0f;
    }

    //==========================================================================
    // The grain loop. Three versions of the same arithmetic, picked per call:
    //
    //   AVX2   8 samples per step, when the CPU has it (most x86 PCs since 2013);
    //          its reads use the gather instruction, or scalar loads (avx2Loads)
    //   SSE2   4 samples per step (every x86-64 CPU)
    //   scalar 1 sample per step (the last few samples, and non-x86 CPUs)
    //
    // Each lane does exactly the scalar loop's operations in the same order, and
    // there is deliberately no FMA (a fused multiply-add rounds once instead of
    // twice), so all three render bit-identical audio: the same session sounds
    // the same on every machine, and the tests can compare the paths directly.
    // "No FMA" needs the compiler's help too: GCC and Clang fuse a*b+c on their
    // own when the target has FMA, so CMakeLists.txt builds with -ffp-contract=off.
    // MSVC needs /fp:precise, its default: /fp:fast may fuse and reorder, and
    // /fp:contract fuses. (MSVC before Visual Studio 2022 could also fuse under
    // /fp:precise when building for AVX2; 2022 and later don't unless asked.)
    //
    // What makes this loop expensive is not the arithmetic but the four reads
    // per sample from four different positions; the SIMD versions do the maths
    // for several samples at once and overlap those reads.

public:
    /**
        avx2 and avx2Loads differ only in how they read the sample: with the gather
        instruction or with scalar loads (the same values, so the same audio). Which
        is faster depends on the CPU (gather is slow on some AMD and older Intel
        cores); NEONSYNTH_GRAINS_AVX2_GATHER picks the one automatic uses, and the
        render benchmark's --grains mode times both (v1.11.1).
    */
    enum class SimdPath { automatic, scalar, sse2, avx2, avx2Loads };

    /** Forces one path (tests and benchmarks); automatic picks the best the CPU has. */
    static void forceSimdPath (SimdPath path) noexcept   { forcedPath().store (path); }

    /** The path render() uses now. */
    static SimdPath activeSimdPath() noexcept
    {
        const auto forced = forcedPath().load();
        if (forced != SimdPath::automatic)
            return (forced == SimdPath::avx2 || forced == SimdPath::avx2Loads) && ! cpuHasAVX2() ? SimdPath::sse2 : forced;
       #if NEONSYNTH_GRAINS_AVX2
        if (cpuHasAVX2()) return NEONSYNTH_GRAINS_AVX2_GATHER ? SimdPath::avx2 : SimdPath::avx2Loads;
       #endif
        return NEONSYNTH_GRAINS_SSE2 ? SimdPath::sse2 : SimdPath::scalar;
    }

    static bool cpuHasAVX2() noexcept
    {
        static const bool has = juce::SystemStats::hasAVX2();   // CPUID, read once
        return has;
    }

private:
    static std::atomic<SimdPath>& forcedPath() noexcept
    {
        static std::atomic<SimdPath> path { SimdPath::automatic };
        return path;
    }

    /** Catmull-Rom between x0 and x1, in Horner form. */
    static float cubic (float xm1, float x0, float x1, float x2, float t) noexcept
    {
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    /** The window, (4x(1-x))^2, at sample `age` of a grain. */
    static float windowAt (int age, float windowStep) noexcept
    {
        const float x = (float) age * windowStep;
        const float u = 4.0f * (x - x * x);
        return u * u;
    }

    /**
        Adds up to `count` samples of the grain to `out`; returns false once it
        has ended. The window is (4x(1-x))^2, a parabola-squared Hann shape
        (within about 0.065 of sin^2(pi x), smooth, zero slope at both ends): four
        multiplies, no table.
    */
    bool renderGrain (Grain& g, float* __restrict out, int count, SimdPath path) noexcept
    {
        const juce::uint64 step = stepNow (g);

        // Bent up past the end of its copy (v1.11.1): render the samples whose position is
        // still inside it, then end. Which sample that is depends only on the grain's own
        // positions, never on where the block boundaries fall.
        int n = juce::jmin (count, g.remaining);
        bool runsOut = false;
        if (n > 0 && g.position + (juce::uint64) n * step > g.end)
        {
            const juce::uint64 inside = g.position >= g.end ? juce::uint64 (0)
                                      : step == 0           ? (juce::uint64) n
                                                            : (g.end - g.position + step - 1) / step;   // positions below the end
            if (inside < (juce::uint64) n)
            {
                n = (int) inside;
                runsOut = true;
            }
        }

        int s = 0;
        switch (path)
        {
           #if NEONSYNTH_GRAINS_AVX2
            case SimdPath::avx2:       s = renderGrainAVX2<true>  (g, out, n, step); break;
            case SimdPath::avx2Loads:  s = renderGrainAVX2<false> (g, out, n, step); break;
           #endif
           #if NEONSYNTH_GRAINS_SSE2
            case SimdPath::sse2:  s = renderGrainSSE2 (g, out, n, step); break;
           #endif
            case SimdPath::automatic:
            case SimdPath::scalar:
            default:              break;
        }

        // Scalar loop: the samples the SIMD loop left over, or all of them.
        const float* __restrict const data = g.data;
        for (; s < n; ++s)
        {
            const float* d = data + (int) (g.position >> 32);
            out[s] += cubic (d[-1], d[0], d[1], d[2], fractionOf (g.position)) * windowAt (g.age, g.windowStep);
            g.position += step;
            ++g.age;
        }

        g.remaining = runsOut ? 0 : g.remaining - n;
        return g.remaining > 0;
    }

    /**
        The grain's step for this render() call. At the ratio it started at (always,
        without pitch modulation) that is its own step, untouched; otherwise the step
        scaled by the ratio's change since. A bend up makes it read further than it was
        sized for; renderGrain() ends it at the end of its copy.
    */
    juce::uint64 stepNow (const Grain& g) const noexcept
    {
        juce::uint64 step = g.step;
        if (pitchRatio != g.pitch)
            step = (juce::uint64) ((double) g.step * ((double) pitchRatio / (double) g.pitch));
        return step;
    }

   #if NEONSYNTH_GRAINS_SSE2
    /** 4 samples per step; returns how many samples it rendered (a multiple of 4). */
    static int renderGrainSSE2 (Grain& g, float* __restrict out, int n, juce::uint64 step) noexcept
    {
        const float* __restrict const data = g.data;   // the output never overlaps the sample
        juce::uint64 position = g.position;

        const __m128 half = _mm_set1_ps (0.5f), oneHalf = _mm_set1_ps (1.5f), two = _mm_set1_ps (2.0f),
                     twoHalf = _mm_set1_ps (2.5f), four = _mm_set1_ps (4.0f), one = _mm_set1_ps (1.0f);
        const __m128 windowStep = _mm_set1_ps (g.windowStep);
        __m128i ages = _mm_setr_epi32 (g.age, g.age + 1, g.age + 2, g.age + 3);
        const __m128i ageStep = _mm_set1_epi32 (4);

        int s = 0;
        for (; s + 4 <= n; s += 4)
        {
            const juce::uint64 p0 = position, p1 = p0 + step, p2 = p1 + step, p3 = p2 + step;
            const float* d0 = data + (int) (p0 >> 32);
            const float* d1 = data + (int) (p1 >> 32);
            const float* d2 = data + (int) (p2 >> 32);
            const float* d3 = data + (int) (p3 >> 32);

            // SSE2 has no gather: the four reads per sample are scalar loads packed into lanes.
            const __m128 xm1 = _mm_setr_ps (d0[-1], d1[-1], d2[-1], d3[-1]);
            const __m128 x0  = _mm_setr_ps (d0[0],  d1[0],  d2[0],  d3[0]);
            const __m128 x1  = _mm_setr_ps (d0[1],  d1[1],  d2[1],  d3[1]);
            const __m128 x2  = _mm_setr_ps (d0[2],  d1[2],  d2[2],  d3[2]);

            // Fractions from the bits (see fractionOf), four at once.
            const __m128i bits = _mm_or_si128 (_mm_set1_epi32 (0x3f800000),
                                               _mm_setr_epi32 ((int) ((juce::uint32) p0 >> 9), (int) ((juce::uint32) p1 >> 9),
                                                               (int) ((juce::uint32) p2 >> 9), (int) ((juce::uint32) p3 >> 9)));
            const __m128 t = _mm_sub_ps (_mm_castsi128_ps (bits), one);

            const __m128 c1 = _mm_mul_ps (half, _mm_sub_ps (x1, xm1));
            const __m128 c2 = _mm_sub_ps (_mm_add_ps (_mm_sub_ps (xm1, _mm_mul_ps (twoHalf, x0)), _mm_mul_ps (two, x1)), _mm_mul_ps (half, x2));
            const __m128 c3 = _mm_add_ps (_mm_mul_ps (half, _mm_sub_ps (x2, xm1)), _mm_mul_ps (oneHalf, _mm_sub_ps (x0, x1)));
            const __m128 v  = _mm_add_ps (_mm_mul_ps (_mm_add_ps (_mm_mul_ps (_mm_add_ps (_mm_mul_ps (c3, t), c2), t), c1), t), x0);

            const __m128 x = _mm_mul_ps (_mm_cvtepi32_ps (ages), windowStep);
            const __m128 u = _mm_mul_ps (four, _mm_sub_ps (x, _mm_mul_ps (x, x)));
            _mm_storeu_ps (out + s, _mm_add_ps (_mm_loadu_ps (out + s), _mm_mul_ps (v, _mm_mul_ps (u, u))));

            position = p3 + step;
            ages = _mm_add_epi32 (ages, ageStep);
        }

        g.position = position;
        g.age += s;
        return s;
    }
   #endif

   #if NEONSYNTH_GRAINS_AVX2
    /**
        8 samples per step; returns how many it rendered (a multiple of 8).
        With `gather`, the reads use the AVX2 gather instruction (vgatherdps): one
        instruction fetches the 8 lanes' values from 8 addresses, instead of 8
        scalar loads and 7 inserts. Without it, those scalar loads (see SimdPath).
    */
    template <bool gather>
    NEONSYNTH_TARGET_AVX2 static int renderGrainAVX2 (Grain& g, float* __restrict out, int n, juce::uint64 step) noexcept
    {
        const float* __restrict const data = g.data;
        juce::uint64 position = g.position;

        const __m256 half = _mm256_set1_ps (0.5f), oneHalf = _mm256_set1_ps (1.5f), two = _mm256_set1_ps (2.0f),
                     twoHalf = _mm256_set1_ps (2.5f), four = _mm256_set1_ps (4.0f), one = _mm256_set1_ps (1.0f);
        const __m256 windowStep = _mm256_set1_ps (g.windowStep);
        __m256i ages = _mm256_add_epi32 (_mm256_set1_epi32 (g.age), _mm256_setr_epi32 (0, 1, 2, 3, 4, 5, 6, 7));
        const __m256i ageStep = _mm256_set1_epi32 (8);
        const __m256i exponent = _mm256_set1_epi32 (0x3f800000);

        int s = 0;
        for (; s + 8 <= n; s += 8)
        {
            // Sample indices and fraction bits of the 8 positions (32.32 fixed point), packed
            // straight into registers. (Writing them to a small array and loading that as a
            // vector stalls: the CPU can't forward eight 4-byte stores into one 32-byte load.)
            const juce::uint64 p0 = position, p1 = p0 + step, p2 = p1 + step, p3 = p2 + step,
                               p4 = p3 + step, p5 = p4 + step, p6 = p5 + step, p7 = p6 + step;
            const __m256i idx = _mm256_setr_epi32 ((int) (p0 >> 32), (int) (p1 >> 32), (int) (p2 >> 32), (int) (p3 >> 32),
                                                   (int) (p4 >> 32), (int) (p5 >> 32), (int) (p6 >> 32), (int) (p7 >> 32));
            const __m256i frac = _mm256_setr_epi32 ((int) ((juce::uint32) p0 >> 9), (int) ((juce::uint32) p1 >> 9),
                                                    (int) ((juce::uint32) p2 >> 9), (int) ((juce::uint32) p3 >> 9),
                                                    (int) ((juce::uint32) p4 >> 9), (int) ((juce::uint32) p5 >> 9),
                                                    (int) ((juce::uint32) p6 >> 9), (int) ((juce::uint32) p7 >> 9));
            __m256 xm1, x0, x1, x2;
            if constexpr (gather)
            {
                xm1 = _mm256_i32gather_ps (data - 1, idx, 4);
                x0  = _mm256_i32gather_ps (data,     idx, 4);
                x1  = _mm256_i32gather_ps (data + 1, idx, 4);
                x2  = _mm256_i32gather_ps (data + 2, idx, 4);
            }
            else
            {
                juce::ignoreUnused (idx);
                const float* d0 = data + (int) (p0 >> 32); const float* d1 = data + (int) (p1 >> 32);
                const float* d2 = data + (int) (p2 >> 32); const float* d3 = data + (int) (p3 >> 32);
                const float* d4 = data + (int) (p4 >> 32); const float* d5 = data + (int) (p5 >> 32);
                const float* d6 = data + (int) (p6 >> 32); const float* d7 = data + (int) (p7 >> 32);
                xm1 = _mm256_setr_ps (d0[-1], d1[-1], d2[-1], d3[-1], d4[-1], d5[-1], d6[-1], d7[-1]);
                x0  = _mm256_setr_ps (d0[0],  d1[0],  d2[0],  d3[0],  d4[0],  d5[0],  d6[0],  d7[0]);
                x1  = _mm256_setr_ps (d0[1],  d1[1],  d2[1],  d3[1],  d4[1],  d5[1],  d6[1],  d7[1]);
                x2  = _mm256_setr_ps (d0[2],  d1[2],  d2[2],  d3[2],  d4[2],  d5[2],  d6[2],  d7[2]);
            }
            const juce::uint64 p = p7 + step;

            const __m256i bits = _mm256_or_si256 (exponent, frac);
            const __m256 t = _mm256_sub_ps (_mm256_castsi256_ps (bits), one);

            const __m256 c1 = _mm256_mul_ps (half, _mm256_sub_ps (x1, xm1));
            const __m256 c2 = _mm256_sub_ps (_mm256_add_ps (_mm256_sub_ps (xm1, _mm256_mul_ps (twoHalf, x0)), _mm256_mul_ps (two, x1)), _mm256_mul_ps (half, x2));
            const __m256 c3 = _mm256_add_ps (_mm256_mul_ps (half, _mm256_sub_ps (x2, xm1)), _mm256_mul_ps (oneHalf, _mm256_sub_ps (x0, x1)));
            const __m256 v  = _mm256_add_ps (_mm256_mul_ps (_mm256_add_ps (_mm256_mul_ps (_mm256_add_ps (_mm256_mul_ps (c3, t), c2), t), c1), t), x0);

            const __m256 x = _mm256_mul_ps (_mm256_cvtepi32_ps (ages), windowStep);
            const __m256 u = _mm256_mul_ps (four, _mm256_sub_ps (x, _mm256_mul_ps (x, x)));
            _mm256_storeu_ps (out + s, _mm256_add_ps (_mm256_loadu_ps (out + s), _mm256_mul_ps (v, _mm256_mul_ps (u, u))));

            position = p;
            ages = _mm256_add_epi32 (ages, ageStep);
        }

        g.position = position;
        g.age += s;

        // Clear the upper halves of the 256-bit registers before returning to code
        // compiled for SSE: on some Intel CPUs mixing the two without this costs a
        // slow state transition on every SSE instruction that follows.
        _mm256_zeroupper();
        return s;
    }
   #endif

    void startGrain() noexcept
    {
        if (numActive >= maxGrains)
            return;   // all busy: skip this onset rather than cut a grain off

        // The copy and rate for this grain: the note's, or under pitch modulation the
        // ones that suit the note's ratio times the current pitch ratio.
        int grainLevel = level;
        double grainIncrement = noteIncrement;
        if (pitchRatio != 1.0f)
        {
            const double ratio = noteRatio * (double) pitchRatio;
            grainLevel = 0;
            while (grainLevel < SampleData::numLevels - 1 && ratio / (double) (1 << grainLevel) > SampleReader::maxFullRateIncrement)
                ++grainLevel;
            grainIncrement = ratio / (double) (1 << grainLevel);
        }

        const int length = sample->getLength (grainLevel);
        const int sizeSamples = juce::jmax (minGrainSamples, (int) (juce::jlimit (minSizeSecondsDouble, maxSizeSecondsDouble, (double) settings.sizeSeconds) * hostRate));

        double increment = grainIncrement * std::pow (2.0, (double) (settings.jitterCents * (2.0f * random01() - 1.0f)) / 1200.0);
        const double span = (double) sizeSamples * increment;   // source samples the grain will read
        if (span >= (double) length - interpolationMargin)
            increment = juce::jmax (0.0, ((double) length - interpolationMargin) / (double) sizeSamples);   // longer than the file: slow it to fit

        // Centre of the cloud, then the spray around it; the grain must fit inside the file.
        double centre = (double) juce::jlimit (0.0f, 1.0f, settings.position) + scanOffset;
        centre -= std::floor (centre);
        const double spread = 0.5 * (double) juce::jlimit (0.0f, 1.0f, settings.spray) * (2.0 * random01() - 1.0);
        const double lastStart = juce::jmax (0.0, (double) length - (double) sizeSamples * increment - interpolationMargin);
        const double start = juce::jlimit (0.0, lastStart, (centre + spread) * (double) length);

        Grain g;
        g.data        = sample->getData (grainLevel);
        g.position    = (juce::uint64) (start * fixedPointOne);
        g.step        = (juce::uint64) (increment * fixedPointOne);
        g.age         = 0;
        g.windowStep  = 1.0f / (float) sizeSamples;
        g.remaining   = sizeSamples;
        g.lengthScale = 1.0f / (float) juce::jmax (1, length);
        g.pitch       = pitchRatio;
        g.end         = (juce::uint64) juce::jmax (0, length) << 32;

        grains[(size_t) numActive++] = g;
        ++grainsStarted;
    }

    /** `s`, with each setting that is not a finite number taken from `previous` instead. */
    static Settings finiteOr (const Settings& s, const Settings& previous) noexcept
    {
        const auto pick = [] (float x, float old) noexcept { return std::isfinite (x) ? x : old; };
        Settings r;
        r.position    = pick (s.position,    previous.position);
        r.scan        = pick (s.scan,        previous.scan);
        r.sizeSeconds = pick (s.sizeSeconds, previous.sizeSeconds);
        r.density     = pick (s.density,     previous.density);
        r.spray       = pick (s.spray,       previous.spray);
        r.jitterCents = pick (s.jitterCents, previous.jitterCents);
        return r;
    }

    /** xorshift32 in [0, 1). */
    float random01() noexcept
    {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (float) (rng >> 8) * (1.0f / 16777216.0f);
    }

    std::array<Grain, maxGrains> grains {};
    int numActive = 0;

    const SampleData* sample = nullptr;
    Settings settings;
    double hostRate = 48000.0, noteIncrement = 1.0, scanOffset = 0.0;
    double noteRatio = 1.0;      // the note's rate on the full-rate copy (noteIncrement is on `level`)
    float  pitchRatio = 1.0f;    // from setPitchRatio()
    int level = 0, samplesToNextGrain = 0;
    bool spawning = false;
    juce::uint32 rng = 1;
    juce::int64 grainsStarted = 0;
};
