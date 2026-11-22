// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Unit tests for the LFO (Lfo.h, v1.9.0): each shape's value at known phases,
    rate accuracy from 0.02 to 20 Hz, output range, the synced phase from a song
    position and after a loop, free-running continuity across block sizes,
    Random repeating after reset() and changing only at wraps, the 2 ms slew and
    the 10 ms shape crossfade, at 44.1, 48 and 96 kHz; bars that follow the host's
    time signature (v1.11.0), with 4/4 unchanged bit for bit.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target LfoTests
    Run the resulting LfoTests executable; it exits non-zero on failure.
*/
#include "dsp/Lfo.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

namespace
{
    int failures = 0;

    void check (bool condition, const char* what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what);
        if (! condition)
            ++failures;
    }

    using Shape = Lfo::Shape;
    using Division = Lfo::Division;
    const Shape allShapes[] = { Shape::sine, Shape::triangle, Shape::square, Shape::saw, Shape::random };
    const double sampleRates[] = { 44100.0, 48000.0, 96000.0 };

    Lfo::Settings freeRunning (Shape shape, float hz)
    {
        Lfo::Settings s;
        s.shape = shape;
        s.rateHz = hz;
        return s;
    }

    Lfo makeLfo (double rate, const Lfo::Settings& s)
    {
        Lfo lfo;
        lfo.setSettings (s);   // before prepare(), so reset() starts on this shape with no crossfade
        lfo.prepare (rate);
        return lfo;
    }

    std::vector<float> renderBlocks (Lfo& lfo, const std::vector<int>& blocks, const Lfo::Transport& transport = {})
    {
        int total = 0;
        for (auto b : blocks) total += b;
        std::vector<float> out ((size_t) total);
        int pos = 0;
        for (auto b : blocks)
        {
            lfo.render (out.data() + pos, b, transport);
            pos += b;
        }
        return out;
    }

    std::vector<float> renderLong (Lfo& lfo, long long numSamples)
    {
        std::vector<float> out ((size_t) numSamples);
        const int block = 512;
        for (long long pos = 0; pos < numSamples; pos += block)
            lfo.render (out.data() + pos, (int) std::min<long long> (block, numSamples - pos), {});
        return out;
    }

    bool sameBits (const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
    }

    float maxStepIn (const std::vector<float>& x, size_t from = 1, size_t to = (size_t) -1)
    {
        float m = 0.0f;
        for (size_t i = std::max<size_t> (from, 1); i < std::min (to, x.size()); ++i)
            m = std::max (m, std::abs (x[i] - x[i - 1]));
        return m;
    }

    bool near (double a, double b, double tolerance)   { return std::abs (a - b) <= tolerance; }

    void testShapes()
    {
        std::printf ("Shapes at known phases\n");
        const float h = 0.3f;
        const auto v = [h] (Shape s, double p) { return Lfo::shapeValue (s, p, h); };
        const float e = 1.0e-6f;

        check (near (v (Shape::sine, 0.0), 0.0, e) && near (v (Shape::sine, 0.25), 1.0, e)
               && near (v (Shape::sine, 0.5), 0.0, e) && near (v (Shape::sine, 0.75), -1.0, e)
               && near (v (Shape::sine, 0.125), std::sqrt (0.5), e), "sine = sin (2 pi p)");
        check (v (Shape::triangle, 0.0) == 0.0f && v (Shape::triangle, 0.125) == 0.5f && v (Shape::triangle, 0.25) == 1.0f
               && v (Shape::triangle, 0.5) == 0.0f && v (Shape::triangle, 0.75) == -1.0f && v (Shape::triangle, 0.875) == -0.5f,
               "triangle: 0, +1 at 0.25, 0 at 0.5, -1 at 0.75 (the sine's zero crossings)");
        check (v (Shape::square, 0.0) == 1.0f && v (Shape::square, 0.49) == 1.0f
               && v (Shape::square, 0.5) == -1.0f && v (Shape::square, 0.99) == -1.0f, "square: +1 then -1");
        check (v (Shape::saw, 0.0) == -1.0f && v (Shape::saw, 0.5) == 0.0f && v (Shape::saw, 0.75) == 0.5f
               && near (v (Shape::saw, 0.999), 0.998, e), "saw: rising ramp -1 .. +1");
        check (v (Shape::random, 0.1) == h && v (Shape::random, 0.9) == h, "random: the held value");

        // The rendered output follows the formula: at 1 Hz nothing is slewed, so sample n is the
        // shape at phase n / fs exactly (Square apart from its edges, Random apart from its wraps).
        bool exact = true;
        for (auto shape : { Shape::sine, Shape::triangle, Shape::saw })
        {
            auto lfo = makeLfo (48000.0, freeRunning (shape, 1.0f));
            const auto out = renderLong (lfo, 48000);
            double phase = 0.0;
            for (size_t n = 0; n < out.size(); ++n)
            {
                exact = exact && out[n] == Lfo::shapeValue (shape, phase, 0.0f);
                phase += 1.0 / 48000.0;
            }
        }
        check (exact, "sine, triangle and saw render bit-exactly as their formulas within a cycle");
    }

    void testRateAndRange()
    {
        std::printf ("Rate accuracy and range\n");
        for (auto fs : sampleRates)
        {
            for (auto hz : { 0.02f, 1.0f, 20.0f })
            {
                // Long enough for several cycles, ending half a cycle past a whole number.
                const int cycles = hz < 0.1f ? 5 : hz < 5.0f ? 50 : 200;
                const double seconds = (cycles + 0.5) / (double) hz;
                const auto numSamples = (long long) std::llround (seconds * fs);

                auto lfo = makeLfo (fs, freeRunning (Shape::sine, hz));
                const auto out = renderLong (lfo, numSamples);

                int upward = 0;
                for (size_t i = 1; i < out.size(); ++i)
                    if (out[i - 1] < 0.0f && out[i] >= 0.0f)
                        ++upward;

                const double expectedPhase = std::fmod ((double) numSamples * (double) hz / fs, 1.0);
                char what[160];
                std::snprintf (what, sizeof (what), "%.2f Hz at %.0f Hz: %d cycles (got %d), phase %.6f (expected %.6f)",
                               (double) hz, fs, cycles, upward, lfo.getPhase(), expectedPhase);
                check (upward == cycles && near (lfo.getPhase(), expectedPhase, 1.0e-6), what);
            }

            bool inRange = true;
            for (auto shape : allShapes)
            {
                for (auto hz : { 0.02f, 3.7f, 20.0f })
                {
                    auto lfo = makeLfo (fs, freeRunning (shape, hz));
                    for (auto x : renderLong (lfo, (long long) fs * 2))
                        inRange = inRange && std::isfinite (x) && x >= -1.0f && x <= 1.0f;
                }
            }
            char what[80];
            std::snprintf (what, sizeof (what), "every shape stays within -1 .. +1 at %.0f Hz", fs);
            check (inRange, what);
        }

        // Out-of-range rates are clamped.
        auto slow = makeLfo (48000.0, freeRunning (Shape::saw, 0.0f));
        renderLong (slow, 48000);
        auto fast = makeLfo (48000.0, freeRunning (Shape::saw, 1000.0f));
        renderLong (fast, 4800);
        const double fastPhase = fast.getPhase();
        check (near (slow.getPhase(), 0.02, 1.0e-9) && std::min (fastPhase, 1.0 - fastPhase) < 1.0e-9,
               "rate is clamped to 0.02 .. 20 Hz");
    }

    void testSync()
    {
        std::printf ("Tempo sync\n");
        Lfo::Settings s;
        s.shape = Shape::saw;
        s.sync = true;
        s.division = Division::quarter;

        auto lfo = makeLfo (48000.0, s);
        float x = 0.0f;
        lfo.render (&x, 1, { true, 3.5, 120.0 });
        const double inc = 120.0 / 60.0 / 48000.0;
        check (x == 0.0f && near (lfo.getPhase(), 0.5 + inc, 1.0e-12), "ppq 3.5 with 1/4: phase 0.5 (saw = 0)");

        // Play on in blocks following the song position, then loop back to the start.
        std::vector<float> out (512);
        double ppq = 3.5 + 1.0 / 24000.0;
        bool following = true;
        for (int b = 0; b < 40; ++b)
        {
            lfo.render (out.data(), 512, { true, ppq, 120.0 });
            ppq += 512.0 * 2.0 / 48000.0;
            following = following && near (lfo.getPhase(), std::fmod (ppq, 1.0), 1.0e-9);
        }
        check (following, "follows the song position across blocks");

        // Loop jump: 1 bar division, song jumps from beat 7.9 back to 4.5 -> phase 0.125.
        s.division = Division::bar1;
        s.shape = Shape::sine;
        auto looped = makeLfo (48000.0, s);
        looped.render (out.data(), 512, { true, 7.9 - 512.0 * 2.0 / 48000.0, 120.0 });
        looped.render (out.data(), 512, { true, 4.5, 120.0 });
        const double barInc = 120.0 / 60.0 / 4.0 / 48000.0;
        check (near (looped.getPhase(), 0.125 + 512.0 * barInc, 1.0e-12), "after a loop jump the phase comes from the new ppq");
        // The jump itself is slewed over 2 ms (96 samples); after that the sine is exact again.
        check (out[200] == Lfo::shapeValue (Shape::sine, 0.125 + 200.0 * barInc, 0.0f)
               && maxStepIn (out) <= 2.0f / 96.0f + 1.0e-6f, "the jump is slewed, then lands exactly on the sine");

        // Synced but the host is stopped: free-runs at the tempo, ignoring ppq.
        s.division = Division::eighth;
        auto stopped = makeLfo (48000.0, s);
        stopped.render (out.data(), 512, { false, 3.3, 90.0 });
        stopped.render (out.data(), 512, { false, 9.9, 90.0 });
        check (near (stopped.getPhase(), 1024.0 * 90.0 / 60.0 / 0.5 / 48000.0, 1.0e-12),
               "synced with the host stopped: free-runs at bpm / 60 / beats");

        // Triplets: 1/8T at 120 bpm is 6 Hz.
        s.division = Division::eighthTriplet;
        auto triplet = makeLfo (48000.0, s);
        renderLong (triplet, 4000);
        check (near (triplet.getPhase(), 4000.0 * 6.0 / 48000.0, 1.0e-12), "1/8T at 120 bpm runs at 6 Hz");
    }

    void testContinuity()
    {
        std::printf ("Free-running continuity across blocks\n");
        bool same = true;
        for (auto fs : sampleRates)
        {
            for (auto shape : allShapes)
            {
                auto a = makeLfo (fs, freeRunning (shape, 20.0f));
                auto b = makeLfo (fs, freeRunning (shape, 20.0f));
                const auto whole = renderBlocks (a, { 1000 });
                const auto split = renderBlocks (b, { 1, 31, 512, 456 });
                same = same && sameBits (whole, split) && a.getPhase() == b.getPhase();
            }
        }
        check (same, "1000 samples as one block == as 1 + 31 + 512 + 456 (every shape, every rate)");

        // reset() repeats a render exactly, including Random and a crossfade in progress.
        auto lfo = makeLfo (48000.0, freeRunning (Shape::random, 13.0f));
        const auto run = [&lfo]
        {
            lfo.reset();
            lfo.setSettings (freeRunning (Shape::random, 13.0f));
            auto out = renderLong (lfo, 30000);
            lfo.setSettings (freeRunning (Shape::square, 13.0f));
            auto more = renderLong (lfo, 3000);
            out.insert (out.end(), more.begin(), more.end());
            return out;
        };
        const auto first = run();
        const auto second = run();
        check (sameBits (first, second), "two renders from reset() are bit-identical (Random, slew, crossfade)");
    }

    void testRandom()
    {
        std::printf ("Random (sample and hold)\n");
        const double fs = 48000.0;
        const float hz = 5.0f;   // a wrap every 9600 samples
        auto lfo = makeLfo (fs, freeRunning (Shape::random, hz));
        const auto out = renderLong (lfo, 96000);

        const int period = 9600, slew = 100;   // 2 ms is 96 samples; allow for the wrap landing a sample late
        bool holds = true;
        std::vector<float> levels;
        for (int c = 0; c < 10; ++c)
        {
            const int start = c * period;
            const float level = out[(size_t) start + (size_t) slew];
            levels.push_back (level);
            for (int i = start + slew; i < start + period; ++i)
                holds = holds && out[(size_t) i] == level;
        }
        check (holds, "between wraps (after the 2 ms slew) the value is constant");

        bool changes = true;
        for (size_t c = 1; c < levels.size(); ++c)
            changes = changes && levels[c] != levels[c - 1];
        check (changes, "a new value at every wrap");

        float lo = 1.0f, hi = -1.0f;
        auto many = makeLfo (fs, freeRunning (Shape::random, 20.0f));
        const auto long_ = renderLong (many, (long long) fs * 50);   // 1000 cycles
        for (size_t i = 2400; i < long_.size(); i += 2400)
        {
            lo = std::min (lo, long_[i - 1]);
            hi = std::max (hi, long_[i - 1]);
        }
        check (lo < -0.9f && hi > 0.9f, "values spread over -1 .. +1");
        check (maxStepIn (out) <= 2.0f / 96.0f + 1.0e-6f && out[0] == levels[0], "steps are slewed (at most 2 per 2 ms)");
    }

    void testSlew()
    {
        std::printf ("Slew\n");
        for (auto fs : sampleRates)
        {
            const float limit = (float) (2.0 / (0.002 * fs)) * 1.0001f;
            auto square = makeLfo (fs, freeRunning (Shape::square, 20.0f));
            const auto sq = renderLong (square, (long long) fs);

            // The first falling edge (at phase 0.5) leaves +1 and takes 2 ms to reach -1.
            size_t edge = 0;
            while (sq[edge] == 1.0f)
                ++edge;
            const auto twoMs = (size_t) std::lround (0.002 * fs);
            const auto halfCycle = 0.5 * fs / 20.0;
            char what[120];
            std::snprintf (what, sizeof (what), "square at %.0f Hz: steps at most 2 per 2 ms, -1 reached in 2 ms", fs);
            check (maxStepIn (sq) <= limit && std::abs ((double) edge - halfCycle) <= 1.0 && sq[edge] > 0.9f
                   && sq[edge + twoMs - 2] > -1.0f && sq[edge + twoMs + 1] == -1.0f, what);

            auto saw = makeLfo (fs, freeRunning (Shape::saw, 20.0f));
            const auto sw = renderLong (saw, (long long) fs);
            std::snprintf (what, sizeof (what), "saw wrap at %.0f Hz is slewed too", fs);
            check (maxStepIn (sw) <= limit, what);
        }

        // Sine and triangle are untouched by the slew even at 20 Hz.
        bool exact = true;
        for (auto shape : { Shape::sine, Shape::triangle })
        {
            auto lfo = makeLfo (44100.0, freeRunning (shape, 20.0f));
            const auto out = renderLong (lfo, 2205);   // one cycle
            double phase = 0.0;
            for (auto x : out)
            {
                exact = exact && x == Lfo::shapeValue (shape, phase, 0.0f);
                phase += 20.0 / 44100.0;
            }
        }
        check (exact, "sine and triangle at 20 Hz are not slewed");
    }

    /** A playing host's transport in a time signature (v1.11.0). */
    Lfo::Transport hostAt (double ppq, double bpm, int numerator, int denominator, double barStart = -1.0)
    {
        Lfo::Transport t;
        t.hostPlaying = true;
        t.ppq = ppq;
        t.bpm = bpm;
        t.beatsPerBar = 4.0 * numerator / denominator;
        t.hasBarStart = barStart >= 0.0;
        t.barStartPpq = barStart >= 0.0 ? barStart : 0.0;
        return t;
    }

    void testTimeSignature()
    {
        std::printf ("Bars follow the time signature (v1.11.0)\n");

        check (Lfo::divisionBeats (Division::bars4) == 16.0 && Lfo::divisionBeats (Division::bars2) == 8.0
               && Lfo::divisionBeats (Division::bar1) == 4.0 && Lfo::divisionBeats (Division::bar1, 4.0) == 4.0,
               "4/4 (and no host): 4, 2 and 1 bars are 16, 8 and 4 beats, as before");
        check (Lfo::divisionBeats (Division::bar1, 3.0) == 3.0 && Lfo::divisionBeats (Division::bars4, 3.0) == 12.0
               && Lfo::divisionBeats (Division::bar1, 3.5) == 3.5 && Lfo::divisionBeats (Division::bars2, 3.5) == 7.0
               && Lfo::divisionBeats (Division::bar1, 2.5) == 2.5,
               "3/4, 7/8 and 5/8: a bar is 3, 3.5 and 2.5 beats");

        bool noteLengths = true;
        for (auto d : { Division::half, Division::quarter, Division::eighth, Division::sixteenth,
                        Division::eighthTriplet, Division::sixteenthTriplet })
            for (auto beatsPerBar : { 3.0, 3.5, 1.5, 6.0 })
                noteLengths = noteLengths && Lfo::divisionBeats (d, beatsPerBar) == Lfo::divisionBeats (d);
        check (noteLengths, "note lengths (1/2 .. 1/16T) stay in beats in every signature");

        Lfo::Settings s;
        s.shape = Shape::saw;
        s.sync = true;
        s.division = Division::bar1;
        float x = 0.0f;
        const double inc3 = 120.0 / 60.0 / 3.0 / 48000.0;

        // Song position 4.5 in 3/4: halfway through the second bar (4/4 would say 0.125).
        auto waltz = makeLfo (48000.0, s);
        waltz.render (&x, 1, hostAt (4.5, 120.0, 3, 4, 3.0));
        check (x == 0.0f && near (waltz.getPhase(), 0.5 + inc3, 1.0e-12), "3/4, ppq 4.5, 1 bar: phase 0.5 (saw = 0)");

        // 7/8, 2 bars = 7 beats: ppq 10.5 is halfway through the cycle.
        s.division = Division::bars2;
        auto sevenEight = makeLfo (48000.0, s);
        sevenEight.render (&x, 1, hostAt (10.5, 120.0, 7, 8, 7.0));
        check (near (sevenEight.getPhase(), 0.5 + 120.0 / 60.0 / 7.0 / 48000.0, 1.0e-12), "7/8, ppq 10.5, 2 bars: phase 0.5");

        // Free-running in 3/4 (host stopped, or synced with no song position): bpm / 60 / 3 beats.
        s.division = Division::bar1;
        auto stopped = makeLfo (48000.0, s);
        Lfo::Transport idle;
        idle.bpm = 90.0;
        idle.beatsPerBar = 3.0;
        std::vector<float> out (4800);
        stopped.render (out.data(), 4800, idle);
        check (near (stopped.getPhase(), 4800.0 * 90.0 / 60.0 / 3.0 / 48000.0, 1.0e-12), "3/4 with the host stopped: one cycle per 3 beats");

        // The signature changed earlier: one bar of 3/4, then 4/4 from beat 3. Bars start at
        // 3, 7, 11 ...; a 1-bar cycle must start on them (ppq / 4 would put beat 3 at 0.75).
        auto changed = makeLfo (48000.0, s);
        changed.render (&x, 1, hostAt (3.0, 120.0, 4, 4, 3.0));
        const double inc4 = 120.0 / 60.0 / 4.0 / 48000.0;
        const double atBarLine = changed.getPhase();
        changed.render (&x, 1, hostAt (9.0, 120.0, 4, 4, 7.0));
        check (near (atBarLine, inc4, 1.0e-12) && near (changed.getPhase(), 0.5 + inc4, 1.0e-12),
               "after a 3/4 bar, 4/4 bars count from the host's bar line (beat 3: phase 0, beat 9: 0.5)");

        // Counted from the bar line, 4-bar cycles keep their place along a stretch of one
        // signature: the bars are counted as if 4/4 had held from the start (bar b + 1 here).
        s.division = Division::bars4;
        auto fourBars = makeLfo (48000.0, s);
        bool steady = true;
        for (int bar = 0; bar < 12; ++bar)
        {
            const double barStart = 3.0 + 4.0 * bar;
            fourBars.render (&x, 1, hostAt (barStart + 2.0, 120.0, 4, 4, barStart));
            steady = steady && near (fourBars.getPhase() - inc4 / 4.0, std::fmod ((bar + 1.5) / 4.0, 1.0), 1.0e-9);
        }
        check (steady, "4 bars after a signature change: a quarter cycle per bar, steady across 12 bars");

        // 4/4 bit for bit: the new transport fields (bar line on the grid, or none) change nothing.
        for (auto division : { Division::bars4, Division::bars2, Division::bar1, Division::quarter, Division::sixteenthTriplet })
        {
            s.division = division;
            s.shape = Shape::sine;
            auto before = makeLfo (44100.0, s);
            auto after  = makeLfo (44100.0, s);
            std::vector<float> a, b;
            double ppq = 17.25;
            for (int block = 0; block < 50; ++block)
            {
                Lfo::Transport old;
                old.hostPlaying = true;
                old.ppq = ppq;
                old.bpm = 128.0;
                const auto withBar = hostAt (ppq, 128.0, 4, 4, std::floor (ppq / 4.0) * 4.0);
                std::vector<float> x1 (441), x2 (441);
                before.render (x1.data(), 441, old);
                after.render (x2.data(), 441, withBar);
                a.insert (a.end(), x1.begin(), x1.end());
                b.insert (b.end(), x2.begin(), x2.end());
                ppq += 441.0 * 128.0 / 60.0 / 44100.0;
            }
            char what[120];
            std::snprintf (what, sizeof (what), "4/4 with the host's bar lines renders as without them (division %d)", (int) division);
            check (sameBits (a, b), what);
        }

        // A signature that makes no sense (a host bug) is taken as 4/4.
        s.division = Division::bar1;
        s.shape = Shape::saw;
        bool fallback = true;
        for (auto beatsPerBar : { 0.0, -3.0, 1000.0, std::nan ("") })
        {
            auto odd = makeLfo (48000.0, s);
            auto t = hostAt (6.0, 120.0, 4, 4);
            t.beatsPerBar = beatsPerBar;
            odd.render (&x, 1, t);
            fallback = fallback && near (odd.getPhase(), 0.5 + inc4, 1.0e-12);
        }
        check (fallback, "a bar of 0, negative, 1000 or NaN beats is taken as 4/4");
    }

    void testCrossfade()
    {
        std::printf ("Shape crossfade\n");
        for (auto fs : sampleRates)
        {
            const auto fade = (size_t) std::lround (0.01 * fs);
            const float bound = (float) (2.0 / (0.002 * fs)) + 2.0f / (float) fade + 0.003f;

            // Sine to square just after phase 0.5: the sine is near 0, the square at -1.
            auto lfo = makeLfo (fs, freeRunning (Shape::sine, 1.0f));
            std::vector<float> out;
            const auto add = [&out] (const std::vector<float>& x) { out.insert (out.end(), x.begin(), x.end()); };
            add (renderLong (lfo, (long long) (0.51 * fs)));
            const auto change = out.size();
            lfo.setSettings (freeRunning (Shape::square, 1.0f));
            add (renderLong (lfo, (long long) (0.05 * fs)));

            char what[160];
            std::snprintf (what, sizeof (what), "sine -> square at %.0f Hz: no step bigger than %.4f, then pure square", fs, (double) bound);
            check (maxStepIn (out) <= bound && out[change + fade] == -1.0f && out.back() == -1.0f
                   && out[change + fade / 2] > -0.6f && out[change + fade / 2] < -0.4f, what);

            // Turning the knob through every shape, one per 64-sample block, never steps.
            auto knob = makeLfo (fs, freeRunning (Shape::sine, 7.0f));
            std::vector<float> turned = renderLong (knob, 1000);
            for (int r = 0; r < 4; ++r)
            {
                for (auto shape : { Shape::square, Shape::saw, Shape::random, Shape::triangle, Shape::sine, Shape::saw, Shape::square })
                {
                    knob.setSettings (freeRunning (shape, 7.0f));
                    const auto block = renderBlocks (knob, { 64 });
                    turned.insert (turned.end(), block.begin(), block.end());
                }
            }
            std::snprintf (what, sizeof (what), "rapid shape changes at %.0f Hz: no step bigger than %.4f", fs, (double) bound);
            check (maxStepIn (turned) <= bound, what);
        }
    }
}

int main()
{
    testShapes();
    testRateAndRange();
    testSync();
    testContinuity();
    testRandom();
    testSlew();
    testCrossfade();
    testTimeSignature();

    if (failures == 0)
        std::printf ("LfoTests: all passed\n");
    else
        std::printf ("LfoTests: %d FAILED\n", failures);

    return failures == 0 ? 0 : 1;
}
