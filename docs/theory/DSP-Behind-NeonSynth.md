<!-- NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu. SPDX-License-Identifier: AGPL-3.0-only.
     Source of "The DSP Behind NeonSynth". Build the PDF with: python docs/theory/build_book.py
     Display equations are ```equation blocks: plain text, where x^{...} and x_{...} (or x^2, f_c)
     are printed as superscripts and subscripts in the PDF. Diagrams and pseudocode are ```text blocks. -->

<div class="titleblock" markdown="1">

![NeonSynth: architecture, signal flow, verification](images/banner.svg)

# The DSP Behind NeonSynth

*A companion mini textbook for musicians and programmers*

Third edition, revised • 22 November 2026 • Target instrument: NeonSynth v1.10.1

</div>

## Scope and verification

This book connects NeonSynth's controls, sounds, mathematics, implementation, and tests. It covers the four sources, waveform sculpting, envelopes, modulation by the LFO, pitch bend and the mod wheel, resonant multi-mode filtering with drive, stereo placement, output protection, sequencing, visualizers, and the JUCE development workflow.

Equations and pseudocode are explanatory reformulations, not copied source listings. Listening settings are experiments rather than factory presets. Product comparisons establish documented feature families, not identical proprietary algorithms.

Primary project material: [repository](https://github.com/kv244/NeonSynth/tree/v1.10.1), [release manual source](https://github.com/kv244/NeonSynth/blob/v1.10.1/docs/NeonSynth-User-Manual.md), [developer notes](https://github.com/kv244/NeonSynth/blob/v1.10.1/docs/DEVELOPMENT.md), and [figure-generation notes](https://github.com/kv244/NeonSynth/blob/v1.10.1/docs/figures/README.md).

## How to use the book

Musicians can begin with Chapters 1–2, then follow the listening laboratories. Programmers should read the shared voice architecture first, use the source index to locate each implementation, and return to the mathematics appendix when needed.

For every topic, distinguish three things: the ideal mathematical model, the practical algorithm, and the behavior measured from a particular build. A correct model does not prove a correct implementation; an attractive display does not replace a measurement.

## Glossary

- **ADAA (Antiderivative Anti-Aliasing):** A technique that reduces aliasing in memoryless nonlinearities by evaluating the continuous-time integral (antiderivative) of the waveshaping curve across each sample interval rather than sampling the curve instantaneously.
- **Aliasing:** Frequency folding that occurs when continuous or discrete signals contain components above the Nyquist frequency (F<sub>s</sub> / 2), causing those components to appear as lower, unintended frequencies.
- **BLAMP (Band-Limited Ramp):** A polynomial residual added near waveform slope discontinuities (such as triangle corners) to attenuate high-frequency harmonics before sampling.
- **BLEP (Band-Limited Step):** A polynomial residual added near instantaneous amplitude jumps (such as sawtooth resets or square transitions) to attenuate aliasing.
- **Homogeneity (Degree-1):** The scaling property f(αx) = αf(x) for α > 0. In NeonSynth's resonant filter limiter, scaling both the audio signal and the limiter ceiling L together scales the output proportionally, allowing the filter to track the preceding amplitude envelope seamlessly.
- **LFO (Low-Frequency Oscillator):** A periodic signal too slow to be heard as a pitch (here 0.02 to 20 Hz), used as a control signal that moves another parameter such as pitch, cutoff, or level.
- **Notch (Band-Reject):** A filter response that attenuates a narrow frequency band around the cutoff while passing lower and higher frequencies; formed in a state-variable filter by summing the low-pass and high-pass outputs (LP + HP).
- **Sample-and-Hold:** Taking a value at one instant and holding it until the next; NeonSynth's Random LFO shape holds a new random value for each LFO cycle.
- **Self-Oscillation:** Sustained sinusoidal oscillation produced by a resonant filter when feedback damping (k = 1/Q) reaches or drops below zero, stabilized at a bounded amplitude by a nonlinear limiter.
- **TPT / ZDF (Topology-Preserving Transform / Zero-Delay Feedback):** A discretization method using trapezoidal integration that preserves the continuous-time analog state-variable topology and solves implicit feedback loops algebraically without inserting unit sample delays.
- **Tremolo and Vibrato:** Periodic modulation of level (tremolo) and of pitch (vibrato). In NeonSynth the LFO's AMP depth makes tremolo; the mod wheel and the LFO's PITCH depth make vibrato.
- **Van der Pol Damping:** Amplitude-dependent nonlinear damping (k<sub>eff</sub>) that increases resistance as oscillation amplitude grows, smoothly bounding self-oscillation into a stable limit cycle without hard clipping.

## Notation Table

| Symbol | Meaning | Units / Domain |
|---|---|---|
| F<sub>s</sub> | Audio sample rate | Hz (samples/second) |
| f, f<sub>c</sub>, f<sub>0</sub> | Frequency, filter cutoff frequency, fundamental frequency | Hz |
| ω, ω<sub>c</sub> | Normalized or analog angular frequency | rad/sample or rad/s |
| x[n], y[n] | Discrete-time input and output at sample index n | Dimensionless amplitude |
| Q | Filter quality factor (resonance sharpness) | Dimensionless (Q > 0) |
| k | Filter damping factor (k = 1/Q) | Dimensionless |
| L | Filter limiter ceiling (max(velocityLevel × ampEnv, 10<sup>−6</sup>)) | Dimensionless amplitude |
| D | Filter pre-drive linear gain (10<sup>drive_dB/20</sup>) | Linear gain (1 to 10) |
| s<sub>1</sub>, s<sub>2</sub> | State-variable filter integrator states | Dimensionless state |
| m[n] | LFO output at sample n | −1 to +1 |
| r[n] | Pitch ratio at sample n (2<sup>semitones/12</sup>) | Dimensionless (r > 0) |
| ULP | Unit in the last place (floating-point precision spacing) | IEEE-754 float32 / float64 |

## Contents

<div class="contents" markdown="1">

**Part I — Architecture and DSP**

- [1. Architecture and complexity](#1-architecture-and-complexity)
- [2. Oscillators, harmonics, and edge correction](#2-oscillators-harmonics-and-edge-correction)
- [3. The evolving Random source](#3-the-evolving-random-source)
- [4. CHOP and periodic amplitude modulation](#4-chop-and-periodic-amplitude-modulation)
- [5. Noise and equal-power mixing](#5-noise-and-equal-power-mixing)
- [6. Wavefolding and oversampling](#6-wavefolding-and-oversampling)
- [7. Sample playback and resampling](#7-sample-playback-and-resampling)
- [8. Granular synthesis](#8-granular-synthesis)
- [9. Envelopes, velocity, and continuity](#9-envelopes-velocity-and-continuity)
- [10. Modulation: the LFO, pitch bend and the mod wheel](#10-modulation-the-lfo-pitch-bend-and-the-mod-wheel)
- [11. Multi-mode resonant filtering and drive](#11-multi-mode-resonant-filtering-and-drive)
- [12. Stereo and output protection](#12-stereo-and-output-protection)
- [13. Sequencing](#13-sequencing)
- [14. Displays and measurement](#14-displays-and-measurement)
- [15. State, real-time engineering, and verification](#15-state-real-time-engineering-and-verification)

**Part II — Listening laboratories**

- [16. Experimental method](#16-experimental-method)
- [17. Reading dependencies map](#17-reading-dependencies-map)
- [18. Sixteen guided laboratories](#18-sixteen-guided-laboratories)
- [19. Final sound-design exercise](#19-final-sound-design-exercise)

**Part III — Supporting material**

- [Appendix A. Mathematics for DSP](#appendix-a-mathematics-for-dsp)
- [Appendix B. Troubleshooting by signal path](#appendix-b-troubleshooting-by-signal-path)
- [Appendix C. Versioned feature-to-source index](#appendix-c-versioned-feature-to-source-index)
- [Appendix D. Model → Algorithm → Verified Behavior](#appendix-d-model--algorithm--verified-behavior)

**Part IV — JUCE and development**

- [20. Framework features and development flow](#20-framework-features-and-development-flow)

</div>

# Part I — Architecture and DSP

## 1. Architecture and complexity

### 1.1 From notes to samples

Digital signal processing, or DSP, uses numerical operations to generate, analyze, or transform sampled signals. A sample is an amplitude value; sample rate F<sub>s</sub> is the number of values representing one second.

A voice is an independently evolving sound instance with its own pitch, envelope, playback position, and filter state. Polyphony means several voices evolve simultaneously. NeonSynth has sixteen voices, each selecting Oscillator, Sample, Granular, or Random at note-on. The Voices parameter (1 to 16, default 16) lets new notes use only the first N of them.

```text
  Keyboard MIDI ----+
                    +--> MIDI routing --> voice allocation (first N of 16: Voices)
  Step sequencer ---+                           |
                                                v
  Shared, once per block:  pitch bend + mod-wheel vibrato + LFO
    --> pitch ratio r[n] (every source), LFO cutoff (every filter), tremolo g[n] (sum)
  One voice:
    Oscillator --> CHOP --> Noise --> Fold --+
    Random ------> CHOP --> Noise --> Fold --+
    Sample player --------------------------+--> velocity × amp ADSR
    Granular cloud -------------------------+           |
    (each source reads at pitch × r[n])               drive
                                                        |
                                              filter (LP/HP/BP/Notch)
                                         [cutoff + filter env + vel + LFO]
                                                        |
                                                  pitch-based pan
                                                        |
  All voices ---------------------------------------> sum
                                                        |
                                   tremolo × g[n] --> output gain --> soft clipper
                                                                           |
                                            display-data transfer <-- output
```

One source per voice is not monophonic operation. A granular voice can itself contain many grains. CHOP, Noise, and Fold apply only to Oscillator and Random; Sample and Granular bypass them. In NeonSynth v1.9.0, the voice signal path routes the amplitude-enveloped signal into the pre-filter ADAA drive stage and multi-mode resonant state-variable filter (modulated by cutoff, filter envelope, velocity, and the LFO) before pitch-dependent stereo panning. The instrument has no live audio-input processing path: recordings enter through loading.

A shared modulation stage (Chapter 10) runs once per block, before any voice renders. It reads pitch bend and the mod wheel from the block's incoming MIDI, renders the LFO, and hands every voice the same per-sample buffers: a pitch ratio that all four sources apply, an LFO cutoff that joins the filter's octave sum, and a tremolo gain that multiplies the sum of the voices. The manual draws the same path as [Figure 10](https://github.com/kv244/NeonSynth/blob/v1.10.1/docs/NeonSynth-User-Manual.md#signal-path).

Voice allocation assigns each note-on to a voice. JUCE's `Synthesiser` takes a free voice, or steals a sounding one when all are busy. With Voices at 16, NeonSynth leaves that choice to JUCE, so sessions saved before v1.9.0 allocate and steal exactly as before. Below 16, NeonSynth's own `findFreeVoice` looks only at the first N voices: a free one if there is one, otherwise the voice already playing the same note, otherwise the oldest voice whose key is up (in its release), otherwise the oldest of all. Voices above the limit keep their notes until those end, so lowering the limit never cuts a note off. With one voice the instrument is monophonic with retrigger: the new note takes over the sounding voice, whose envelope attacks from its current level and whose phase continues (Chapter 9), so there is no click, but there is no glide either.

### 1.2 Order is part of the sound

A linear operation obeys superposition: processing a sum equals summing separately processed inputs, and scaling the input scales the output proportionally. Nonlinear processing violates those relationships and can create new frequencies.

For nonlinear folder F and envelope E:

```equation
E[n] × F(x[n]) ≠ F(E[n] × x[n])
```

NeonSynth folds before its amplitude envelope. A fading envelope therefore scales the folded signal without itself reducing folder drive. Downstream of the amplitude envelope, the drive and resonant filter stages use homogeneity with respect to the envelope level L so that drive saturation and filter self-oscillation track the envelope contour cleanly while preserving exact signal ordering. Noise follows CHOP, so muted waveform segments can still contain noise; Fold processes their combination.

### 1.3 Execution contexts

An audio callback produces a block of samples before a deadline. File access, waiting, allocation, and destruction can be unpredictable and should not become prerequisites for the next block.

| Context | Work |
|---|---|
| Audio callback | Events, modulation, voices, filtering, mixing |
| GUI/message thread | Controls, drawing, spectrum analysis |
| Background workers | Sample preparation and log writing |

A FIFO transfers items in first-in, first-out order. Atomic fields support indivisible accesses under the language's concurrency rules. NeonSynth uses these mechanisms for display transfer, including the MODULATION panel's LFO phase and value and the bend and wheel positions (relaxed atomics written once per block), while prepared samples are published separately and retired only when safe. Individual atomic accesses do not by themselves guarantee an entire multi-field snapshot is coherent; the displayed telemetry tolerates neighboring-block values.

### 1.4 Complexity

Big-O notation describes growth, not exact CPU time. With B samples, V active voices, G grains per voice, L stored samples, K filter taps, and N FFT points:

| Subsystem | Approximate work | Main challenge |
|---|---|---|
| Oscillator, envelope, filter, pan | O(BV) | State, continuity, and coefficient decimation |
| CHOP and edge correction | O(BV) | Boundary correctness |
| Modulation buffers (LFO, bend, wheel) | O(B), shared by all voices | Sample accuracy and bit-identity at rest |
| Sample playback | O(BV) | Reconstruction and looping |
| Granular playback | O(BVG) | Scheduling and memory access |
| Sample preparation | O(LK) | Filtering and safe publication |
| FFT analysis | O(N log N) | Analysis and normalization |

The short sine-folder and state-variable filter equations require substantial surrounding code for oversampling, anti-aliasing, and state continuity. Conversely, a large control panel can have little mathematical complexity. Benchmarking must accompany complexity analysis.

**Repository reading:** `NeonSynthVoice.h`, `NeonSynthProcessor.h/.cpp`, `Modulation.h`, `SampleSlot.h`, `CytomicSvf.h` at tag `v1.10.1`.

## 2. Oscillators, harmonics, and edge correction

### 2.1 Phase accumulation

Frequency is repetition rate; phase is position within a cycle. A phase accumulator stores that position and advances it each sample, the same basic principle used by numerically controlled oscillators.

```equation
phase[n + 1] = fractionalPart(phase[n] + frequency / sampleRate)
```

Different functions turn phase into amplitude:

```text
sine:     sin(2π phase)
saw:      2 phase − 1
square:   +1 for phase < 0.5, otherwise −1
triangle: 4 phase − 1 below 0.5, otherwise 3 − 4 phase
```

NeonSynth retains phase across notes and caps the phase increment at 0.49 cycles per sample (just below Nyquist). Calling `setWaveform()` only stores a pending waveform shape; it is applied unconditionally when the phase wraps past 1.0 (at the start of the next cycle). Across this waveform switch, the treble-compensation filter retains its state history. Boundary latching prevents mid-cycle discontinuities, though it is an update policy rather than a guarantee that every waveform pair shares identical boundary amplitude.

Since v1.9.0 the increment can also be multiplied, sample by sample, by the shared pitch ratio r[n] of Chapter 10:

```equation
Δphase[n] = min(r[n] × frequency / sampleRate, 0.49)
```

The cap applies to the modulated increment, so a bend far up stays below Nyquist. The note's own increment is kept, so when the modulation ends the oscillator is back on it exactly. The Random source (Chapter 3) follows the same rule.

### 2.2 Harmonics and timbre

The fundamental is a periodic waveform's base repetition frequency. Harmonics are integer multiples of it. A spectrum describes frequency content; timbre also includes how that content and articulation evolve over time.

An ideal sine contains one frequency. Square and triangle contain odd harmonics, with the triangle's upper components declining more rapidly. A saw contains odd and even harmonics. The [Roland oscillator explanation](https://static.roland.com/manuals/s-1_manual_v102/eng/87294902.html) gives related waveform descriptions.

### 2.3 Nyquist and aliasing

Nyquist frequency is half the sample rate. Frequencies separated by integer multiples of the sample rate can produce identical sampled sequences. Aliasing is this ambiguity appearing in the result, often as unintended tones.

Anti-aliasing reduces unwanted frequency folding. Band-limiting restricts frequency content. A naive saw has an amplitude discontinuity; a triangle has a slope discontinuity. Both require unlimited ideal bandwidth, although their high harmonics decline differently.

### 2.4 BLEP and BLAMP

BLEP means band-limited step: a smoothed substitute for an instantaneous amplitude jump. A residual is the correction added to the naive waveform rather than a complete replacement waveform. BLAMP means band-limited ramp and corrects a change of slope. It is appropriate for triangle corners rather than saw resets.

```equation
corrected = naive + jumpHeight × stepResidual + slopeChangePerSample × cornerResidual
```

PolyBLEP approximates corrections with short polynomials. NeonSynth's `PolyBlepOscillator` name is historical: the implementation (`Blep4`) uses four-point residuals derived from a cubic B-spline kernel spanning 4 samples (±2 samples around the discontinuity). Specifically:

- **BLEP residual:** The integral of the cubic B-spline kernel from fractional distance d to 2, yielding 0.5 at the edge (d = 0) and 0 at d = 2.
- **BLAMP residual:** The second integral of the cubic B-spline kernel, yielding 7/30 at the corner (d = 0) and 1/120 at d = 1.

These residuals are exact while the phase step per sample is ≤ 0.25 (ensuring adjacent 4-sample correction windows do not collide). Above 0.25 up to the 0.49 phase-increment cap, some aliasing returns, though the output remains strictly bounded. A step of 0.25 is 12 kHz at 48 kHz; bending a high note upward can carry it across that point.

```text
value = naiveWaveform(phase)
for each nearby amplitude edge:
    value += edgeJump × stepResidual(edgeDistance)
for each nearby corner:
    value += slopeChange × cornerResidual(cornerDistance)
advancePhase()
```

The implementation challenge is overlapping corrections and keeping edge assumptions consistent while settings change. This is practical anti-aliasing, not ideal brick-wall filtering.

### 2.5 Treble compensation

An FIR, or finite impulse response, filter sums a finite number of weighted input samples. The positions are taps; their weights are coefficients.

NeonSynth partly restores treble softened by the 4-point B-spline correction:

```equation
y[n] = (1 + 2a) x[n − 1] − a (x[n] + x[n − 2]),   a = 0.27
```

The sine bypasses compensation. Restoring desired harmonics can also amplify residual aliases.

### 2.6 Relatives and other domains

[amsynth](https://amsynth.github.io/) and [Pigments](https://www.arturia.com/products/software-instruments/pigments/overview) provide related software waveform families; Roland S-1 and [Waldorf Iridium](https://waldorfmusic.com/iridium-en/) provide hardware counterparts. Matching waveform names do not establish matching anti-aliasing implementations.

Outside music, direct digital synthesis uses phase accumulation for communications and test equipment; see [Analog Devices' DDS tutorial](https://www.analog.com/media/en/training-seminars/tutorials/MT-085.pdf).

**Repository reading:** `Oscillator.h`, especially `Blep4` and `PolyBlepOscillator` at tag `v1.10.1`.

## 3. The evolving Random source

Sample-and-hold keeps a value until the next update. NeonSynth places sixteen held random levels around a mostly repeating cycle. Repetition supports pitch; gradual replacement changes timbre.

```text
step = floor(16 × phase)
value = levels[step] + adjacentEdgeCorrections()
if replacementIsDue:
    levels[(step + 8) modulo 16] = nextRandomValue()
advancePhase()
```

Updates occur away from the playhead so recently used edge assumptions do not change. Sixteen steps per cycle become very short at high notes, where the local corrections lose effectiveness. Pitch modulation multiplies its phase step as it does the oscillator's (§2.1).

A pseudorandom generator is deterministic despite producing an irregular-looking sequence. A seed chooses its starting state. Xorshift uses integer shifts and exclusive-OR operations; NeonSynth uses seeded generators for controlled repeatability.

Random's SHAPE follows the lowest sounding Random voice, or a fixed example when none sounds. The display is not proof that every voice has the same current shape.

Pigments' Turing/random modulators and MicroFreak's random LFO shapes are conceptual relatives, not exact audio-rate equivalents. The exact evolving-wave algorithm has no established non-musical deployment in the sources reviewed; reproducible random stimulus generation is the broader connection, not an identity claim.

**Repository reading:** `RandomStepOscillator` in `Oscillator.h` at tag `v1.10.1`.

## 4. CHOP and periodic amplitude modulation

Amplitude modulation multiplies a signal by a changing gain. CHOP places a mask inside each oscillator cycle.

For Comb C, Overtone o from 0 to 2, and binary pattern p:

```text
k = floor(16 C phase) modulo 16
mask = 1 − o p[k]
output = input × mask
```

Selected segments are unchanged at 0%, muted at 100%, and inverted at 200%. Comb repeats the pattern; it is not a delay-line comb filter.

Convolution adds shifted and scaled copies of another sequence. Multiplication in time corresponds to convolution in frequency, explaining why a repeating mask redistributes harmonics instead of only lowering level.

At a boundary:

```equation
outputJump ≈ waveformValueAtEdge × maskGainChange
```

NeonSynth scales its BLEP correction accordingly. Let L = 1 / (Δt · 16 · C) be the segment length in samples (where Δt = f / F<sub>s</sub> is the phase increment per sample). To prevent ultrasonic aliasing when segments become narrower than the 4-point correction kernel, CHOP scales its effective modulation depth by:

```equation
depth = clamp((L − 2) / 2, 0, 1)
```

The effect is at full depth for L ≥ 4 samples, fades linearly between L = 4 and L = 2, and reaches zero for L ≤ 2 samples. Pattern and Comb updates wait for a cycle boundary; Overtone glides over 10 ms.

[Roland S-1 OSC CHOP](https://static.roland.com/manuals/s-1_manual_v102/eng/87294783.html) is the direct hardware inspiration, although its high-frequency policy differs. NeonSynth itself is the software implementation established here; no second exact counterpart is asserted. Timed amplitude/polarity modulation has communications relatives, but those do not implement the musical control scheme merely because the algebra resembles it.

**Repository reading:** `Chop`, `ChopSettings`, and oscillator edge-value helpers in `Oscillator.h` at tag `v1.10.1`.

## 5. Noise and equal-power mixing

White noise has constant power spectral density; a digital generator approximates this within its bandwidth. Unlike a periodic oscillator, it does not concentrate energy into stable harmonic lines.

NeonSynth mixes waveform x and noise η using:

```equation
y = cos(π a / 2) x + sin(π a / 2) η
```

An equal-power crossfade uses gains whose squares sum to one. For uncorrelated zero-mean inputs of equal variance, expected output power stays constant. Correlation describes how strongly signals vary together; variance measures their spread around the mean.

These assumptions matter. Different waveforms have different RMS levels and perceived loudness, so equal-power does not guarantee equally loud results for every source.

Noise follows CHOP. Muting a source segment therefore does not mute noise added afterward. Fold processes the resulting combination.

Pigments, amsynth, S-1, and Quantum offer noise-related generation. Outside synthesis, noise is used as an interference model or test stimulus, including [radar pulse-compression experiments](https://www.mathworks.com/help/signal/ug/radar-pulse-compression.html).

**Repository reading:** `Sculpt.h` at tag `v1.10.1`.

## 6. Wavefolding and oversampling

### 6.1 The nonlinear function

Waveshaping maps input amplitude through a function. Wavefolding bends large excursions back instead of flattening them; see the [Stanford wavefolder explanation](https://ccrma.stanford.edu/~jatin/ComplexNonlinearities/Wavefolder.html).

NeonSynth uses:

```equation
F(x) = sin((π / 2) · drive · x),    y = (1 − mix) x + mix F(x)
```

Drive 1 is not transparent; Mix zero is the neutral setting. The sine power series contains cubic and higher powers, which generate harmonics. A symmetric sine and odd-symmetric transfer function ideally produce odd harmonics, but newly generated components can exceed Nyquist.

### 6.2 Oversampling

Oversampling processes at a higher rate than the output, leaving more room for generated frequencies before conversion back. Interpolation creates the high-rate signal; low-pass filtering before decimation removes content that would alias on return.

```text
2× interpolation → folder → low-pass/decimation
                 → DC blocker → aligned dry/wet mix
```

A half-band FIR is useful for factor-two conversion and can contain many zero coefficients. NeonSynth uses this efficiency. Two-times oversampling reduces aliasing; it does not guarantee removal at arbitrary Drive.

### 6.3 Timing and DC

Latency is processing delay. Mixing delayed and undelayed paths can create frequency-dependent interference, so alignment matters.

A fractional delay approximates a non-integer sample shift. NeonSynth uses an integer delay plus a first-order all-pass. An all-pass preserves magnitude but changes phase; its group delay describes the frequency-dependent timing effect. This is approximate alignment, not a perfect broadband half-sample shift.

DC offset is a nonzero average. A DC blocker removes it with a low-cutoff high-pass:

```equation
y[n] = x[n] − x[n − 1] + r y[n − 1]
```

### 6.4 Complexity and comparisons

The sine equation is simple; the filters, history buffers, bypass transitions, alignment, and smoothing create most of the code complexity. Pigments and [MicroFreak](https://dl.arturia.net/products/microfreak/manual/microfreak_Manual_5_0_1_EN.pdf) offer related folding, not necessarily the same transfer curve.

Outside synthesis, [oversampling/interpolation](https://www.analog.com/media/en/training-seminars/tutorials/MT-017.pdf) has broad conversion-system uses. No common non-musical use of this exact musical folder chain is established here.

**Repository reading:** `Sculpt.h` at tag `v1.10.1`.

## 7. Sample playback and resampling

### 7.1 Pitch as read rate

Resampling reconstructs a signal at new time positions. A sample player changes pitch by changing its progress through a recording:

```equation
rate = 2^{(note − root) / 12} × fileSampleRate / hostSampleRate,    positionNext = position + rate
```

A semitone has ratio 2<sup>1/12</sup>; an octave doubles rate. ROOT identifies the key for original-rate playback. It does not analyze pitch. Ordinary rate-based playback changes duration and pitch together.

Pitch modulation (Chapter 10) multiplies the read rate per sample, positionNext = position + rate × r[n], which bends duration together with pitch, as a tape speed change would.

### 7.2 Interpolation

Interpolation estimates between stored values. Catmull–Rom uses four neighbors to construct a cubic that passes through the central samples; it is smooth and efficient, but can overshoot and is not ideal reconstruction.

```equation
y(t) = c_0 + c_1 t + c_2 t^2 + c_3 t^3
```

Horner form evaluates it with nested multiplication:

```text
value = ((c3 × fraction + c2) × fraction + c1) × fraction + c0
```

Padding makes nearby reads safe at buffer boundaries. Interpolation alone is not sufficient anti-aliasing for arbitrary upward transposition.

### 7.3 Prefiltered levels

Decimation reduces sample rate after filtering frequencies that would alias. NeonSynth stores original-, half-, and quarter-rate sample versions, selecting one at note-on so that the read increment stays at or below 2<sup>3/12</sup> ≈ 1.19 (three semitones above unity). Since v1.9.0, pitch modulation can move a sounding note to another version, with a crossfade and one semitone of hysteresis (§10.6).

The sinc function underlies ideal band-limited reconstruction. A windowed-sinc FIR truncates and tapers that infinite ideal response. NeonSynth prepares its reduced versions with a 95-tap Blackman-windowed filter. A Blackman window is a cosine-sum taper that trades a wider central response for lower sidelobes.

Only three levels exist. Extreme upward transposition can still exceed the preferred read increment, so this is a bounded compromise, not an unlimited resampler.

### 7.4 Preparation and persistence

Peak normalization scales a whole signal to a target maximum. Quantization rounds amplitudes to discrete levels; bit depth determines how many.

Import averages channels to mono, retains at most 60 seconds, normalizes to a −1 dBFS peak, and rounds to a 24-bit grid. It does not overwrite the file. Opposing stereo channels may cancel. Later stereo spread comes from voice pan, not preserved sample imaging.

### 7.5 Loops

| Mode | Behavior |
|---|---|
| One-shot | Forward playback without looping |
| Loop | Repeats while the voice sounds, including Release |
| Loop until release | Exits looping after release and continues forward |

All use ADSR. A short Release may hide the file's end. Crossfades smooth joins but correlated material may swell or cancel. The wrap skips the already-crossfaded head region, affecting the effective repeat interval.

ROOT, Start, loop markers, and crossfade are captured at note-on. Retrigger after editing them.

Pigments and [FluidSynth](https://github.com/FluidSynth/fluidsynth) provide software sampling; Iridium and Quantum provide hardware sampling. Resampling is also a general communications and multirate-processing technique.

**Repository reading:** `SampleData.h`, `SampleLoader.h`, `SampleReader.h`, `SampleSlot.h` at tag `v1.10.1`.

## 8. Granular synthesis

A grain is a short excerpt multiplied by an amplitude window. Granular synthesis overlaps grains so source selection can evolve separately from playback inside each fragment.

```equation
grain_j[n] = gain_j × w((n − onset_j) / duration_j) × sample(start_j + rate_j (n − onset_j))
output[n] = ∑_j grain_j[n]
```

A window is a finite weighting shape that softens boundaries. Hann is a raised cosine. NeonSynth instead uses the Hann-like polynomial:

```equation
w(u) = [4u(1 − u)]^2,    0 ≤ u ≤ 1
```

It has zero value and slope at the endpoints and is cheap to evaluate, but is not exactly Hann.

| Control | Meaning |
|---|---|
| Position | Base source selection |
| Scan | Movement of that selection |
| Size | Output duration per grain |
| Density | Starts per second |
| Spray | Random selection displacement |
| Pitch Jitter | Random rate variation |

A cent is one hundredth of a semitone: a change of δ cents multiplies rate by 2<sup>δ/1200</sup>. Frozen Scan does not freeze individual grain audio. Negative Scan moves selection backward, not playback within each grain.

Expected overlap is approximately density times duration. The engine scales by 1 / √(max(1, overlap)), assuming roughly uncorrelated grains; correlated grains behave differently. A 32-grain cap bounds resources, and full pools skip starts rather than truncate existing grains. Scheduling varies intervals by ±30% around nominal.

Fixed-point arithmetic places integer and fractional portions in one integer representation. NeonSynth uses 32.32 positions for inexpensive stepping. SIMD performs one operation on several values at once; scalar, SSE2, and AVX2 loops cover different CPUs and block tails. Fused multiply-add rounds a·b+c once instead of twice, so contraction is disabled to help align alternate paths.

Pitch modulation (Chapter 10) reaches the cloud in two ways. A grain that starts takes the note's rate times the current pitch ratio, read from the version that suits that rate. Grains already sounding follow too: their 32.32 step is scaled by the ratio's change since they started, once per render chunk of at most 32 samples, so every SIMD lane still adds the same integer (§10.6).

Pigments, Iridium, and Quantum offer granular relatives. Outside synthesis, windowing also supports radar sidelobe suppression, but windowed measurement is not granular synthesis.

**Repository reading:** `GrainCloud.h` and `tests/GranularTests.cpp` at tag `v1.10.1`.

## 9. Envelopes, velocity, and continuity

ADSR generates Attack, Decay, Sustain, and Release trajectories. Attack, Decay, and Release are times; Sustain is a level.

NeonSynth uses linear segments:

```equation
attackStep = 1 / (attackSeconds × F_s),    decayStep = (1 − sustain) / (decaySeconds × F_s),
releaseStep = levelAtNoteOff / (releaseSeconds × F_s)
```

Release begins from the actual level at note-off. Retriggering attacks from the current level rather than resetting to zero; with a fixed attack slope this means the remaining rise can be shorter than a fresh attack.

Smoothing spreads a control change over time. Slew limiting bounds its rate, reducing clicks and zipper noise caused by discrete changes. Sustain slews; ordinary refreshes preserve an ongoing release slope. Editing Release recalculates a remaining fade from the current level.

Since v1.8.0, NeonSynth uses two independent `VoiceEnvelope` instances per voice:

1. **Amplitude envelope:** Multiplies source amplitude with velocity level (`(source * velocityLevel) * ampEnvelope`) and governs voice lifetime.
2. **Filter envelope:** Gated simultaneously with the amplitude envelope to modulate filter cutoff frequency, with defaults of 10 ms Attack, 300 ms Decay, 0.5 Sustain, and 300 ms Release. The filter envelope never ends the voice.

Voice stealing reassigns an already sounding voice, potentially changing phase, source, level, pan, and state. NeonSynth combines continuity strategies rather than relying on one universal fade. These reduce artifacts but should not be treated as a proof that every arbitrary transition is perfect.

Envelopes appear in amsynth, Pigments, S-1, and MicroFreak. Outside music, the relatives are state machines, ramps, and controlled trajectories. An envelope detector is different: it measures an existing signal instead of generating a control shape.

**Repository reading:** `VoiceEnvelope.h`, `NeonSynthVoice.h` at tag `v1.10.1`.

## 10. Modulation: the LFO, pitch bend and the mod wheel

### 10.1 A control signal, not a sound

Modulation means varying one parameter with another signal over time. A low-frequency oscillator, or LFO, is a periodic signal too slow to be heard as a pitch; it is used as a control signal, scaled into the units of whatever it moves. NeonSynth has one LFO for the whole instrument. Every voice reads the same values, so the notes of a chord move together, and a note that starts later joins the wave wherever it is.

The LFO is a phase accumulator (§2.1) running at 0.02 to 20 Hz. Its phase p, from 0 to 1, becomes a bipolar value m from −1 to +1:

```text
sine:     sin(2π p)
triangle: 4p below 1/4, 2 − 4p below 3/4, otherwise 4p − 4
square:   +1 for p < 1/2, otherwise −1
saw:      2p − 1 (rising)
random:   a value drawn uniformly from −1..+1 at each new cycle, held until the next
```

The LFO's triangle starts at 0 and rises, like its sine; the audio triangle of §2.1 starts at −1. Random is sample-and-hold: one value is taken per cycle and held. Its source is a pseudorandom generator with a fixed seed, reseeded by `reset()`, so an offline render from a reset repeats exactly.

With SYNC on, a note length of b beats replaces the rate: 4 bars, 2 bars, 1 bar, 1/2, 1/4, 1/8, 1/16, 1/8T, and 1/16T are 16, 8, 4, 2, 1, 1/2, 1/4, 1/3, and 1/6 beats. While a host transport plays, the phase comes from the song position at the first sample of each block:

```equation
rate = (BPM / 60) / b,    p = fractionalPart(ppq / b)
```

so the LFO lands on the beat after a jump or a loop. With the transport stopped, and in the Standalone, it free-runs at the host's tempo, or else at the sequencer's TEMPO. A bar is counted as four quarter notes whatever the host's time signature.

### 10.2 Pitch is exponential

Equal musical intervals are equal frequency ratios. NeonSynth therefore adds every pitch modulation in semitones and converts the sum once:

```equation
semitones[n] = bend[n] × range + vibrato[n] + m[n] × pitchDepth[n]
r[n] = 2^{semitones[n] / 12}
```

Twelve semitones up doubles the frequency; twelve down halves it. Equal deviations up and down are symmetrical in pitch, not in hertz. The LFO's PITCH depth runs from −12 to +12 semitones; a negative depth turns the shape upside down.

Pitch bend arrives as a 14-bit value v from 0 to 16383, centred at 8192:

```equation
bend = (v − 8192) / 8191 for v ≥ 8192,    (v − 8192) / 8192 below
```

so both ends reach exactly ±1 and the centre is exactly 0. The bend range runs from 0 to 24 semitones each way (default 2).

Vibrato is periodic pitch modulation. The mod wheel w, from 0 to 1 (controller 1 alone as a 7-bit value, or with controller 33 as its fine part, 14 bits), scales a fixed 5.5 Hz sine:

```equation
vibrato[n] = w[n] × (cents / 100) × sin(2π × 5.5 Hz × n / F_s)
```

with cents the VIBRATO depth at the wheel's top (0 to 100, default 30). The vibrato's phase runs freely whether the wheel is up or not, and separately from the LFO, so both can be used together.

Because an oscillator's phase accumulates its frequency, pitch modulation is frequency modulation. A vibrato of ±30 cents at 5.5 Hz on A4 swings the frequency by about ±7.7 Hz, a modulation index β = Δf / f<sub>m</sub> ≈ 1.4, which spreads energy into sidebands spaced 5.5 Hz apart: finer than the Spectrum display's bins (Chapter 14).

### 10.3 Cutoff in octaves

Cutoff modulation is exponential too. The LFO's CUTOFF depth d, from −5 to +5 octaves, joins the octave sum of §11.5:

```equation
f_c = clamp(f_knob × 2^{envAmount · envLevel + velAmount · velocity + d · m[n]}, 20 Hz, 0.45 F_s)
```

A depth of +2 octaves swings an 800 Hz cutoff between 200 Hz and 3.2 kHz. The filter must be on to hear it.

### 10.4 Tremolo: amplitude modulation that only dips

Tremolo is periodic amplitude modulation. With AMP depth A from 0 to 1:

```equation
g[n] = 1 − A × (1 − m[n]) / 2
```

At the LFO's top (m = +1) the gain is 1; at its bottom (m = −1) it is 1 − A. The gain never exceeds unity, so turning AMP up cannot raise peaks into the soft clipper (§12.3). The price is level: for a sine LFO the mean gain is 1 − A / 2, so full depth averages about −6 dB.

Writing g = (1 − A/2) + (A/2) m shows the structure of amplitude modulation: a carrier plus sidebands. Multiplication in time is convolution in frequency (Chapter 4), so a sine LFO at f<sub>L</sub> adds components at f ± f<sub>L</sub>, each at A/4 of the input's amplitude. Square and Random shapes add more sidebands, softened by the 2 ms slew described below.

The tremolo multiplies the sum of all voices once, after the synth renders and before the output gain. Because the same gain applies to every voice, this equals applying it to each voice, for one multiplication per channel and sample instead of one per voice.

### 10.5 Algorithm: one modulation stage per block

`Modulation` runs on the audio thread once per block, before the synth renders. It first scans the block's incoming MIDI for pitch bend, controllers 1 and 33 (the mod wheel), and controller 121 (Reset All Controllers, which centres the bend and drops the wheel to zero), on any channel, keeping each with its sample position (up to 256 per block; further messages merge into the last entry, which keeps the latest values). Then it renders the block's buffers:

```text
once per block, before the voices:
    events   = bend / CC 1 / CC 33 / CC 121 from the incoming MIDI, with positions
    m[0..B)  = LFO.render(B, transport)
    for n in 0..B-1:
        bend and wheel targets  = the latest event at or before n
        r[n]       = 2^((bend·range + wheel·depth·sin(2π·5.5·t) + m[n]·pitchDepth) / 12)
        octaves[n] = m[n] · cutoffDepth
        g[n]       = 1 − ampDepth · (1 − m[n]) / 2
    a buffer at its neutral value for the whole block is handed on as null
every voice:      source(pitch × r[n]) → ... → filter(cutoff + octaves[n]) → pan
after the voices: sum × g[n] → output gain → soft clipper
```

The buffers are computed once per block for all voices, but their values are per sample, and a bend or wheel message takes effect on its own sample. The transcendental work, one sine for the vibrato and one `exp2` for the ratio, is paid once per sample for the whole instrument, not once per voice; each voice multiplies by the shared value. Buffers are allocated in `prepare()` for max(block size, 8192) samples; a longer block holds the last value beyond that while the LFO, the vibrato, and the glides still advance.

Smoothing times differ by purpose. The bend glides linearly over 5 ms, so a controller that sends coarse steps does not sound stepped. The wheel, the vibrato depth, and the three LFO depths glide over 20 ms, so turning them does not zipper. After `prepare()` or `reset()` the depths snap to their settings, so a session that loads with an LFO depth set does not fade it in, and a render from a reset repeats.

The LFO itself is smoothed by a rate limiter rather than a filter:

```equation
|m[n] − m[n − 1]| ≤ 2 / (0.002 × F_s)
```

A full jump from −1 to +1 takes exactly 2 ms. A limiter leaves every slower signal untouched, bit for bit: the fastest sine (20 Hz) moves at most 2π × 20 ≈ 126 per second, the triangle 80, and the saw's ramp 40, against a limit of 1000 per second. In practice it acts only on the Square's edges, Random's steps, the Saw's wrap, and the jump when a synced host loops or relocates. A shape change crossfades linearly over 10 ms, the old shape running on through its own limiter; another change during a fade starts a new fade from wherever the output is.

Null buffers keep default sessions bit-identical to v1.8.0. A buffer whose sources rest at their neutral values for the whole block (no events, bend centred and not gliding, wheel or vibrato depth at 0, PITCH or CUTOFF depth at 0) is not computed at all, and one that is computed but neutral everywhere is also handed on as a null pointer. With a null pitch buffer every source runs its v1.8.0 code path, operation for operation. With a null cutoff buffer nothing is added to the octave sum, and an octave sum of exactly 0 returns the knob value without the `log2`/`exp2` round trip. With AMP at 0 and not fading, the tremolo is not applied.

A non-null cutoff buffer makes the voice render in its gliding version (§11.5): the filter coefficients are recomputed every 16 samples from the LFO value of that sample. At 48 kHz that is 3000 updates per second, 150 per cycle of the fastest LFO.

### 10.6 Pitch in each source

The pitch ratio is applied where each source decides how fast to move through its material:

- **Oscillator and Random:** the phase step becomes min(Δφ<sub>note</sub> × r[n], 0.49) (§2.1). Nothing is stored, so the next unmodulated sample is back at the note's own step.
- **Sample:** the read increment becomes increment × r[n] (§7.1). The version (§7.3) can then be wrong for the new rate. When the rate rises past the current version's limit, the voice moves the note to the next smaller version; it moves back to a larger one only when the rate falls to 2<sup>2/12</sup> relative to that version, one semitone lower. That hysteresis stops vibrato hovering near a boundary from switching back and forth. Without pitch modulation the note returns to the version it started on.
- **Granular:** new grains start at the note's rate times r, on the version that suits that rate. While the ratio moves, the cloud renders in chunks of at most 32 samples (0.67 ms at 48 kHz), each at the ratio of its first sample, and a grain that started at ratio r<sub>0</sub> steps by:

    ```equation
    step' = floor(step_0 × r / r_0)        (32.32 fixed point, worked out in double once per chunk)
    ```

A move between sample versions copies the note into the voice's second reader (the one used for retrigger crossfades), then rescales the moving reader's position, loop points, crossfade, and increment by a power of two: sample n of a version sits at sample 2n of the version above, so both readers play the same point in the sound. The old version then fades out and the new one fades in over 2 ms. The crossfade is linear on purpose: the two signals are the same sound, fully correlated, so linear gains keep the amplitude constant, whereas the equal-power law of Chapter 5, meant for uncorrelated signals, would swell by 3 dB at its midpoint. A note that has not sounded yet moves without a fade; a move waits while a retrigger crossfade is still running. Switching rather than reading on keeps a bend up from aliasing and a bend down from losing its top end.

For the grains, the scaled step is one integer computed before the grain loop, so the scalar, SSE2, and AVX2 loops all add the same value and stay bit-identical (§8). At r = r<sub>0</sub> the step is untouched, so at ratio 1 every grain steps exactly as in v1.8.0. If a bend up would carry a grain past the end of its version, the step shrinks to fit.

```text
renderCloud(block):
    if no pitch modulation:  cloud.setPitchRatio(1); cloud.render(whole block)
    else for each chunk of ≤ 32 samples:
        cloud.setPitchRatio(r[first sample of chunk])
        cloud.render(chunk)       // new grains: copy and rate for noteRate × r
                                  // sounding grains: step = step0 × r / r0, one integer
```

### 10.7 Verified behavior

- **`tests/LfoTests.cpp`:** each shape matches its formula bit for bit within a cycle; rates are accurate from 0.02 to 20 Hz; a song position of 3.5 beats with 1/4 gives phase 0.5; the phase follows the song position across blocks and after a loop jump; 1/8T at 120 BPM runs at 6 Hz; 1000 samples rendered as one block equal the same samples rendered as 1 + 31 + 512 + 456; two renders from `reset()` are bit-identical; Random is constant between wraps and new at every wrap; steps are slewed to at most 2 per 2 ms, while sine and triangle at 20 Hz are not touched; all at 44.1, 48, and 96 kHz.
- **`tests/ModulationTests.cpp`:** a full bend gives exactly ±2 and ±24 semitones at those ranges, range 0 leaves the buffer null, the upper half maps over 8191 steps and the lower over 8192, and a bend on channel 16 counts; an event at sample 100 leaves the earlier samples untouched; the bend is halfway at 2.5 ms and arrives after 5 ms; controller 1 alone is 7-bit and with controller 33 is 14-bit; controller 121 restores the null buffer; the wheel at its top swings ±30 cents at 5.5 Hz (11 cycles in 2 s); the tremolo never exceeds 1, reaches 0 at AMP 100% and 0.5 at AMP 50%, and fades in over 20 ms; blocks longer than the buffers are handled.
- **`tests/PitchTests.cpp`:** at ratios of 0, +12, and −7 semitones, Oscillator, Random, and Sample sound within 3 cents of the note times the ratio, and Granular peaks within 25 cents; an all-1 pitch buffer renders exactly as no buffer does; a 0 to +24 semitone sweep on a sample crosses the version boundaries at +3 and +15 semitones with no step larger than an ideal sweep's (within 1%), a level that holds above 97% of its peak, and an end pitch within 3 cents of two octaves up; the version switch has its hysteresis.
- **`tests/MidiModulationTests.cpp`:** through the whole processor, a full bend raises A4 by 2 semitones; in Layer mode a keyboard note and the sequencer's notes (channel 16) are bent together; the wheel at its top swings about 30 cents; LFO CUTOFF of 3 octaves at 2 Hz makes the brightness rise and fall.
- **`tests/RealtimeSafetyTests.cpp`:** no allocation or free in `processBlock` with the LFO on all three depths in every shape, synced and free, the Voices limit changing, and bend and wheel moving mid-block. **`tests/compare_versions.py`:** sessions that never touch the new controls render bit-identically to the v1.8.0 build.

`RenderBenchmark` (48 kHz, 512-sample blocks, 16 notes held at full velocity, share of one CPU core):

| Scenario | CPU |
|---|---|
| 16 voices, no modulation | 1.35% |
| lfo cutoff 16: LFO CUTOFF +2 octaves at 4 Hz, every voice gliding | 1.73% |
| vibrato 16: mod wheel at its top, pitch moving every sample | 1.28% |
| bend granular: a slow bend on the granular source, 200 grains/s of 250 ms | 6.05% |

The LFO on the cutoff costs about 0.4% of a core for 16 voices, the price of the 16-sample coefficient updates. The vibrato's cost is within the measurement's noise: a multiplication per voice and sample. The granular figure is dominated by the grains themselves (up to 32 per voice), and belongs beside the granular worst case rather than beside plain voices.

Pigments, MicroFreak, and the S-1 offer LFO, vibrato, and tempo-synced relatives, not necessarily these shapes or smoothing rules. Outside music, vibrato is frequency modulation and tremolo is amplitude modulation, as in radio; sample-and-hold is the first stage of every analog-to-digital converter; and taking the LFO's phase from the song position is the timestamped scheduling of Chapter 13.

**Repository reading:** `Lfo.h`, `Modulation.h`, `NeonSynthVoice.h` (`setModulation`, `sourceSample`, `settleSampleCopy`, `renderCloud`, `modulatedCutoffHz`), `SampleReader.h` (`levelFor`, `moveToLevel`), `GrainCloud.h` (`setPitchRatio`, `stepNow`), `tests/LfoTests.cpp`, `tests/ModulationTests.cpp`, `tests/PitchTests.cpp`, `tests/MidiModulationTests.cpp` at tag `v1.10.1`.

## 11. Multi-mode resonant filtering and drive

### 11.1 Topology-preserving state-variable filter (TPT / ZDF)

A second-order filter provides 12 dB/octave asymptotic attenuation slopes and shapes the frequency spectrum around a characteristic cutoff frequency f<sub>c</sub>. In continuous time, the analog state-variable prototype expresses low-pass, band-pass, and high-pass responses simultaneously from two coupled integrator stages:

```equation
H_LP(s) = ω_c^2 / (s^2 + (ω_c/Q)s + ω_c^2),    H_BP(s) = (ω_c/Q)s / (s^2 + (ω_c/Q)s + ω_c^2),
H_HP(s) = s^2 / (s^2 + (ω_c/Q)s + ω_c^2)
```

To discretize this analog network without inserting artificial unit delays in the feedback path, NeonSynth (since v1.8.0) implements a two-pole trapezoidal-integrator state-variable filter (`CytomicSvf.h`), one per voice. Using pre-warped integrator gain g = tan(π f<sub>c</sub> / F<sub>s</sub>) computed in double precision, damping R<sub>2</sub> = 1/Q, and loop normalization h = 1 / (1 + R<sub>2</sub> g + g<sup>2</sup>), each sample updates the two integrator states s<sub>1</sub>, s<sub>2</sub> algebraically:

```text
g = (float) tan(pi * fc / fs)   // computed in double
R2 = 1 / Q
h = 1 / (1 + R2 * g + g * g)

yHP = h * (x - s1 * (g + R2) - s2)
yBP = g * yHP + s1
s1  = g * yHP + yBP
yLP = g * yBP + s2
s2  = g * yBP + yLP
```

**Floating-point associativity and bit-identity:** At resonance 0% (Q = 1/√2, Butterworth) and drive 0%, `CytomicSvf` reproduces JUCE's `StateVariableTPTFilter` bit for bit. Andrew Simper's literal Cytomic equations describe the exact same transfer function, but arrange additions and multiplications in a different order. Because IEEE-754 floating-point arithmetic is not associative ((a + b) + c ≠ a + (b + c)), using the literal Cytomic operation order would alter the least significant bits of every legacy v1.7.1 preset. Copying JUCE's exact arithmetic sequence guarantees bit-identical backward compatibility when resonance and drive are at their neutral defaults.

### 11.2 Filter modes and resonance mapping

Because y<sub>HP</sub>, y<sub>BP</sub>, and y<sub>LP</sub> are computed simultaneously from the same two states s<sub>1</sub>, s<sub>2</sub>, NeonSynth exposes four selectable filter modes:

- **Low-Pass (LP):** y<sub>LP</sub>
- **High-Pass (HP):** y<sub>HP</sub>
- **Band-Pass (BP):** y<sub>BP</sub>
- **Notch (Band-Reject):** y<sub>LP</sub> + y<sub>HP</sub>

When switching modes on an active voice, NeonSynth crossfades between the old and new mode outputs over 10 ms to prevent switching clicks.

The Resonance control maps percentages (0% to 100%) to the damping factor k = 1/Q:

| Resonance Control | Damping k = 1/Q | Quality Factor Q | Behavior |
|---|---|---|---|
| 0% | 1.4142 | 0.71 | Butterworth (maximally flat passband) |
| 25% | 0.8398 | 1.19 | Gentle resonant emphasis |
| 50% | 0.5000 | 2.00 | Classic resonant peak |
| 75% | 0.2568 | 3.89 | Pronounced vocal/acid resonance |
| 90% | 0.1000 | 10.0 | High-Q narrow peak |
| 95% | 0.0393 | 25.5 | Near-threshold ringing |
| 98% | 0.0000 | ∞ | Self-oscillation threshold |
| 100% | −0.0275 | −36.4 | Active negative damping (driven self-oscillation) |

### 11.3 Van der Pol self-oscillation limiter and envelope homogeneity

When k ≤ 0 (Resonance ≥ 98%), linear poles cross onto or outside the unit circle, causing unbounded exponential growth unless stabilized by a nonlinear restoring mechanism. Rather than hard-clipping an internal state, `CytomicSvf` implements smooth, amplitude-dependent van der Pol damping:

```equation
A^2 = (s_1^2 + s_2^2) / 4,    q = c A^2 / L^2,    k_eff = k + B q / (B + q)
```

with constants c = 0.1 and B = 2.

- **Phase-ripple-free amplitude sensing:** Near the resonant cutoff frequency f<sub>c</sub>, the two integrator states s<sub>1</sub> and s<sub>2</sub> have equal amplitude and sit 90° (a quarter cycle) apart in phase. By the Pythagorean identity sin<sup>2</sup>θ + cos<sup>2</sup>θ = 1, their squared sum (s<sub>1</sub><sup>2</sup> + s<sub>2</sub><sup>2</sup>)/4 measures the instantaneous squared envelope A<sup>2</sup> without rippling at twice the carrier frequency, preventing second-harmonic distortion in the damping loop.
- **Measured self-oscillation stability:** At 48 kHz, self-oscillation settles cleanly at a peak amplitude of approximately 1.05 L. Across frequency, the oscillation level remains within 1 dB from 100 Hz to 5 kHz, measuring −3.6 dB at 10 kHz and −5.6 dB at 15 kHz. The resulting ringing pitch tracks within 0.02% of the target cutoff frequency f<sub>c</sub>.
- **Envelope following through degree-1 homogeneity:** The limiter ceiling is tied dynamically to the voice's amplitude envelope:

    ```equation
    L = max(velocityLevel × ampEnvelope, 10^{−6})
    ```

    Because the effective damping k<sub>eff</sub> depends solely on the dimensionless ratio A/L, the filter equation is homogeneous of degree 1: scaling both the input signal x and the ceiling L by the same factor scales the output y identically. Consequently, the filter behaves acoustically as if the amplitude envelope were placed *after* the filter—self-oscillation swells during Attack, decays smoothly during Release, and terminates in true silence—while the physical signal path remains `envelope → drive → filter`, preserving bit-identity with v1.7.1 patches.

> **What Was Tried: Design Dead Ends in Resonant Limiting**
>
> Before arriving at quadrature-state van der Pol damping with pre-filter ADAA drive, three alternative limiter topologies were prototyped and rejected based on quantitative measurements:
>
> 1. **Hard/soft clipping the band-pass state (s<sub>1</sub>):** Created severe odd-harmonic aliasing measured at −15 dB on a sawtooth wave, and caused the self-oscillation output level to climb by +13 dB as cutoff increased toward Nyquist.
> 2. **Amplitude sensing from the previous band-pass sample alone (y<sub>BP</sub>[n−1]<sup>2</sup>):** Became numerically unstable and blew up near f<sub>c</sub> = F<sub>s</sub> / 4, where a sinusoid sampled at four points per cycle passes near zero on every alternate sample, momentarily fooling the limiter into thinking oscillation amplitude had collapsed.
> 3. **Placing nonlinear drive inside the feedback loop:** Generated dense inharmonic intermodulation products at −25 dB to −40 dB that 2× oversampling failed to eliminate—because the inharmonic components were created by nonlinear feedback dynamics inside the resonant loop itself rather than simple frequency folding.

### 11.4 Pre-filter ADAA drive

Following dead-end measurement 3, nonlinear drive was placed *ahead* of the resonant filter loop rather than inside it. For a Drive setting of 0 to 20 dB (D = 10<sup>drive_dB/20</sup>) and ceiling L:

```equation
y = L · sat(D x / 4L) / sat(D / 4),    sat(u) = u / √(1 + u^2)
```

Normalizing by sat(D/4) ensures that a signal already sitting at the ceiling L maintains its exact peak amplitude, while quieter signals are boosted by up to +8.6 dB at full drive (20 dB). Because the denominator scales x by L, the drive stage shares the exact same degree-1 homogeneity as the filter limiter.

To suppress aliasing without oversampling the entire filter loop, NeonSynth evaluates sat(u) using **first-order Antiderivative Anti-Aliasing (ADAA)**. The continuous antiderivative of sat(u) is:

```equation
F_1(u) = ∫ u / √(1 + u^2) du = √(1 + u^2)
```

Across a sample transition from u<sub>n−1</sub> to u<sub>n</sub>, the average continuous output is (F<sub>1</sub>(u<sub>n</sub>) − F<sub>1</sub>(u<sub>n−1</sub>)) / (u<sub>n</sub> − u<sub>n−1</sub>). Two numerical details are critical:

- **Double-precision evaluation and midpoint fallback:** When u is small, √(1 + u<sup>2</sup>) is close to 1.0, so subtracting two nearly equal square roots in 32-bit float suffers catastrophic cancellation. `CytomicSvf` computes F<sub>1</sub>(u) in 64-bit `double`, and falls back directly to sat((u<sub>n</sub> + u<sub>n−1</sub>)/2) whenever |Δu| < 10<sup>−5</sup>.
- **Half-sample latency crossfade:** First-order ADAA introduces an inherent half-sample (0.5 T<sub>s</sub>) group delay. To prevent phase cancellation or comb filtering when dialing drive in from zero, the first 5% of the Drive knob crossfades smoothly from the undelayed dry signal into the ADAA output.

**Measured anti-aliasing performance:** At 100% drive on a PolyBLEP sawtooth, ADAA adds virtually zero measurable aliasing beyond the oscillator's own baseline (−32.4 dB vs. −32.7 dB), outperforming naive memoryless clipping by 11 to 17 dB.

### 11.5 Cutoff modulation, smoothing, and computational cost

Cutoff frequency is modulated exponentially in octaves by the filter envelope, note velocity, and (since v1.9.0) the LFO, clamped safely below Nyquist:

```equation
f_c = clamp(f_knob × 2^{envAmount · envLevel + velAmount · velocity + lfoOctaves[n]}, 20 Hz, 0.45 F_s)
```

When the octave sum is exactly zero (both `envAmount` and `velAmount` at zero and no LFO cutoff this block), f<sub>knob</sub> is returned untouched without the `log2`/`exp2` round trip, preserving bit-identity. The LFO term lfoOctaves[n] = d · m[n] is read from the shared buffer at the sample being rendered (§10.3); when no LFO cutoff is set the buffer is null and nothing is added. The cutoff knob is smoothed multiplicatively over 20 ms (f[n] = f<sub>0</sub> (f<sub>1</sub> / f<sub>0</sub>)<sup>n/N</sup>), while ENV amount, VEL amount, Resonance, and Drive are smoothed linearly over 20 ms. The filter runs continuously while bypassed so its integrator states remain warm, and resets when the voice's amplitude envelope finishes.

To minimize trigonometric (`tan`) overhead during sweeps, filter coefficients (g, R<sub>2</sub>, h) update once every **16 samples** while any parameter or envelope is moving, and are skipped completely when at rest. A non-null LFO cutoff buffer counts as moving, so the voice renders in its gliding version for that block and each update reads the LFO value of its own sample. The 16-sample counter persists across audio buffer boundaries so rendered audio is strictly independent of the host's buffer size.

- **Sweep benchmark (v1.8.0):** Rendering 16 voices sweeping cutoff over 60 seconds of audio takes 887 ms with 16-sample coefficient decimation, compared to 1498 ms when updating every sample.
- **Load benchmark (v1.8.0):** Running 16 active voices at Resonance 90% and Drive 50% consumes 2.6% of a single CPU core, compared to 1.1% at default settings.
- **LFO benchmark (v1.9.0):** 16 voices with LFO CUTOFF at +2 octaves and 4 Hz consume 1.73% of a core, against 1.35% for the same 16 voices unmodulated (§10.7).

### 11.6 Spectrum display normalization

The Spectrum visualizer draws the filter frequency response with the same coefficient formulas as the voices' filter (`setCoefficients`, `magnitudeAt`), computed from the knob settings: cutoff, mode, and resonance. It therefore moves the instant a knob does and shows the resonant peak, but it does not include the filter envelope, velocity, or LFO modulation, nor the smoothing the audio filter applies. When high resonance creates a gain peak exceeding the display's 0 dB top edge, the plotted curve is shifted downward by the peak's height (up to 30 dB) so the resonant crown remains visible inside the viewport. While LFO CUTOFF is not 0, a green band under the CUTOFF slider shows how far the LFO swings the cutoff.

amsynth, Pigments, S-1, and MicroFreak provide multi-mode resonant filter relatives, not necessarily identical TPT/van der Pol topologies. Outside synthesis, state-variable and zero-delay feedback filters appear in control systems and digital audio equalizers.

**Repository reading:** `CytomicSvf.h` (header comment and core step), `NeonSynthVoice.h` (`modulatedCutoffHz`, `filterStep`, `advanceFilterControls`, `filterEnvelopeMovesCutoff`), `tests/FilterTests.cpp`, `tests/compare_versions.py` at tag `v1.10.1`.

## 12. Stereo and output protection

### 12.1 Pitch-dependent pan

A pan law maps position to channel gains. Constant-power laws keep their squared sum constant.

```equation
p = width × clamp((note − 60) / 24, −1, 1),    angle = π (p + 1) / 4
leftGain = √2 cos(angle),    rightGain = √2 sin(angle)
```

The normalization gives unity per channel at center; hard-pan channel gain is greater than centered per-channel gain. MIDI 60 stays centered. Width distributes notes, not delayed copies. The pan follows the note number, so pitch bend and vibrato do not move a voice in the stereo field.

Pigments' keyboard/pan modulation supports related patches, not necessarily this exact preset mapping. The mathematical connection outside music is normalized vector weighting; no non-musical use of the keyboard-specific mapping is claimed.

### 12.2 Gain and decibels

Decibels express ratios logarithmically:

```equation
dB = 20 log_10(amplitude / reference),    amplitudeGain = 10^{dB/20}
```

dBFS references digital full scale. Headroom is the margin below a selected maximum. Summed voices can exceed that maximum even when each is individually safe.

Since v1.9.0 the summed voices pass through the tremolo gain g[n] (§10.4) before the output gain G (the GAIN fader, −48 to +6 dB, smoothed over 20 ms) and the soft clipper:

```equation
y[n] = softClip(G[n] × g[n] × ∑_voices v[n])
```

Because g[n] ≤ 1, the tremolo can only lower the level reaching the clipper.

### 12.3 Soft clipping

Soft clipping bends large amplitudes toward a ceiling. The knee is where the curve begins departing from lower-level behavior.

NeonSynth passes magnitudes below the knee a = 0.50118723 (−6 dBFS) unchanged and approaches the ceiling b = 0.89125094 (−1 dBFS). With normalized excursion u = min((|x| − a) / (b − a), 3):

```equation
q(u) = u (27 + u^2) / (27 + 9u^2),    y = sign(x) [a + (b − a) q(u)]
```

Because u is capped at 3 (corresponding to an input level of roughly +4.5 dBFS), where q(3) = 1 and q'(3) = 0, the transfer curve reaches zero slope smoothly and stays completely flat above +4.5 dBFS. On MSVC, evaluating this rational polynomial takes approximately 4 ns per clipped sample, compared to roughly 16 ns for `std::tanh`.

There is no lookahead or time-varying detector, so it avoids compressor-style pumping but introduces harmonic distortion. It is not oversampled here.

A sample peak is the largest stored value. True peak concerns reconstructed values between samples; the ceiling is not a true-peak guarantee or hearing-safety guarantee. Other synths offer nonlinear shaping, but no exact proprietary match to this protective curve is asserted. Bounded nonlinear maps have broad mathematical uses; this precise curve's non-musical deployment is not established.

**Repository reading:** voice panning in `NeonSynthVoice.h`, `applyAmplitude` in `Modulation.h`, and `softClip` in `NeonSynthProcessor.h` at tag `v1.10.1`.

## 13. Sequencing

Sample-accurate timing places an event at a specified sample rather than a block boundary. It does not require the waveform to become immediately nonzero there.

```equation
samplesPerBeat = 60 F_s / BPM,    stepBeat[k] = k · stepLength + (odd k ? 0.5 · swing · stepLength : 0)
offset = ceil((stepBeat − blockStartBeat) × samplesPerBeat)
```

Swing delays alternating events. Gate sets note duration relative to nominal step length, not each alternating swung interval.

The sequencer emits MIDI, not audio. Voice rendering splits at event positions. Keyboard messages use channel 1; sequencer notes use channel 16, avoiding same-pitch note-off interference. Layer, Replace, and Transpose determine keyboard interaction.

Pitch bend and the mod wheel are not routed by channel. The modulation stage reads them from the block's incoming MIDI, on any channel, before the synth renders, and its pitch ratio reaches every sounding voice. A bend therefore reaches the sequencer's notes on channel 16 just as it reaches keyboard notes; holding a key in Transpose mode and bending bends the whole pattern. The voices ignore JUCE's per-channel `pitchWheelMoved()` and `controllerMoved()` calls.

Small transport discrepancies are tolerated while true jumps release pending notes. A tempo or beat position that is not a finite number, whether from a damaged session or a host, counts as stopped (since v1.10.1): the step loop advances by a step length worked out from the tempo, and would never finish on NaN. Accuracy is limited by transport information supplied for the block; it is not perfect knowledge of arbitrary within-block tempo changes. The LFO's SYNC follows the same transport (§10.1).

Pigments, S-1, and MicroFreak offer sequencer relatives. Outside synthesis, the shared abstraction is timestamped scheduling against a reference clock.

**Repository reading:** `StepSequencer.h`, `NeonSynthProcessor.cpp` MIDI routing, `Modulation::handleMidi` in `Modulation.h` at tag `v1.10.1`.

## 14. Displays and measurement

| Display | Represents |
|---|---|
| SHAPE | Ideal cycle after CHOP/Fold, omitting noise |
| Scope | Recent rendered output |
| Spectrum | Finite analysis of rendered output |
| Filter overlay | Calculated response from the knob settings, without envelope, velocity, or LFO modulation (auto-scaled up to 30 dB for high resonance) |
| LFO view | One cycle of the LFO shape with a dot at the current phase; dimmed while all three depths are 0 |
| Bend and wheel bars | The controllers' smoothed positions, read once per block |
| Voice lights | Sounding voices; clicking a light sets the Voices limit |

Since v1.10.0 the scope and spectrum keep a short afterglow, like a phosphor screen: each new frame is drawn bright while the previous few fade out over about 150 ms. A trace on screen is therefore a brief history, not one instant; a sound that changes quickly (a filter sweep, the LFO) shows as a fan of fading traces, and the brightest one is the newest. Read values from the brightest trace, or wait until it settles. The level meter is drawn in blocks, fully lit up to the RMS level and half lit up to the peak, with the held peak still an exact line; the blocks change colour at −12 and −2 dBFS.

A rising zero crossing moves from negative to positive. Scope triggering near such a crossing stabilizes a periodic trace. The scope uses note frequency and min/max reduction so narrow peaks are not simply discarded when samples outnumber pixels. Since v1.9.0 the frequency each voice reports includes its pitch ratio at the end of the last block (for display only), so the trigger and SHAPE's choice of the lowest Random voice follow a bend.

The Fourier transform represents a signal as frequency components including phase. The DFT analyzes a finite sampled block; an FFT computes that DFT efficiently. Frequency bins are its discrete analysis positions:

```equation
binSpacing = F_s / N
```

At 48 kHz and N = 4096, spacing is about 11.72 Hz. Resolution also depends on duration, window, and component levels. A 5.5 Hz vibrato's sidebands (§10.2) are closer together than that, so a vibrating note shows as a widened peak rather than separate lines.

Spectral leakage spreads energy because a finite record does not fit the implied periodic continuation. A window lowers sidelobes, the smaller surrounding responses, while broadening the main lobe. Hann is used by the Spectrum display; the manual's waveform figure instead uses Blackman–Harris, distinct from Blackman.

Window-gain normalization compensates for attenuation of suitable sinusoidal measurements. Noise density and tonal amplitude require different interpretations. A plotted target filter curve can briefly lead the actual smoothed audio filter.

RMS is the square root of mean squared amplitude; peak is the maximum magnitude:

```equation
RMS = √((1 / N) ∑_{n=0..N−1} x[n]^2),    peak = max(|x[n]|)
```

Neither fully predicts loudness. NeonSynth's clip light indicates proximity to the soft-clip ceiling, not necessarily digital overs.

FFT/windowing has major non-musical applications, including radar. Label illustrations as measured audio, actual application capture, or explanatory schematic; those are different forms of evidence.

**Repository reading:** `Visualisers.h` (including `Afterglow`), `ShapeView.h`, `SampleView.h`, `LfoView.h` at tag `v1.10.1`.

## 15. State, real-time engineering, and verification

| Setting | Application policy |
|---|---|
| Source | Next note-on |
| Sample Root, Start, loop, crossfade | Captured at note-on |
| CHOP pattern and Comb | Cycle boundary |
| Overtone | Smoothed |
| Cutoff | Multiplicatively smoothed (20 ms); coefficients updated every 16 samples while moving |
| Filter Mode | Crossfaded over 10 ms |
| Resonance, Drive, ENV/VEL amounts | Linearly smoothed (20 ms) |
| Fold engagement | Crossfaded |
| LFO shape | Crossfaded over 10 ms; jumps slewed over 2 ms |
| LFO PITCH, CUTOFF, AMP depths and VIBRATO | Linearly smoothed (20 ms) |
| Pitch bend / mod wheel | From the message's sample; glides of 5 ms / 20 ms |
| Voices | Next note-on; voices above the limit finish their notes |

Saved state and automation are not identical. Patterns and sample information are saved, but individual CHOP and sequencer steps are not ordinary host parameters. CHOP edits lack undo in this version.

Prepared sample values can be embedded as 24-bit FLAC, preserving the prepared representation rather than the original import. Stable parameter IDs and choice meaning preserve host automation. Neutral new settings help protect old sounds.

Version 1.9.0 appends ten parameters after the existing ones: LFO Shape, Rate, Sync, Division, Pitch, Cutoff, and Amp, Pitch Bend Range, Vibrato Depth, and Voices. `ParameterLayoutTests` checks that parameters are only ever appended. Every new one starts at a neutral value (all depths 0, Voices 16), so a session saved before v1.9.0 loads with the new controls doing nothing. The saved-state format version stays at 4: the new controls are ordinary parameters. The positions of the bend and the wheel are performance state, not saved; `reset()` and controller 121 centre them.

Safe sample replacement separates preparation, publication, reader acquisition, retirement, and later destruction. Avoiding allocation is not enough if an old object is freed in the callback.

Verification should check timing, spectrum, transitions, state round-trips, live-data ownership, no rendering allocation/free, alternate-path agreement, version-to-version regression, and host gestures. The repository includes tests across these categories.

**A session is untrusted input (v1.10.1).** A saved session, a project file or a preset someone sends can be damaged or deliberately malformed, and the plugin reads it inside the host's process, where a crash takes the DAW down with it. `tests/StateFuzzTests.cpp` feeds `setStateInformation` about 1,600 seeded variations (truncated or garbled XML, corrupted embedded samples, values that are not numbers, absurd nesting, huge patterns, long paths) and requires finite audio afterwards. It found four real failures in v1.10.0, each a lesson:

- **Trusting a length prefix.** JUCE's encoding of the embedded sample starts with its decoded size, and decoding allocates that size before checking it, so "-1" threw `bad_alloc` out of the host callback and a large number allocated gigabytes. The prefix is now checked against the 10 MB cap and against what the text can actually hold.
- **NaN is a value too.** A parameter stored as "nan" passed straight through to the DSP: a NaN tempo made the sequencer's step loop endless, a NaN grain position crashed the grain reader, and NaN in a voice's state stayed there for good. Values that are not finite numbers now fall back to the parameter's default.
- **Recursion depth is a resource.** The XML parser recurses for each nested element; 300,000 levels overflowed the stack. Sessions nested deeper than 64 levels are refused (NeonSynth's own nest three deep).
- **Relative paths** are not tried when a sample is restored.

Sessions saved by v1.2, v1.4, v1.5 and v1.10 are kept as fixtures (`tests/fixtures/state`) and must load, keep every newer parameter at its neutral default and play; the whole test suite also runs under AddressSanitizer and UndefinedBehaviorSanitizer in CI.

**Resolved v1.7.1 smoother defect (B6) and floating-point bit-identity:**

- **Smoother step fix:** In NeonSynth v1.7.1, the gliding render path advanced the level and filter-mix smoothers once before its sample loop as well as on every sample inside the loop, causing a glide to finish one sample early per render call. This defect is resolved in v1.8.0.
- **Multiplication order and ULP regression testing:** During v1.8.0 development, refactoring amplitude scaling from `(source * level) * envelope` to `source * (level * envelope)` shifted legacy patch outputs by 1–2 ULP (units in the last place) due to IEEE-754 floating-point rounding non-associativity. The regression harness (`tests/compare_versions.py`) caught the discrepancy immediately, leading to the strict operation-order preservation rules used across both the voice gain path and `CytomicSvf.h`. Cross-platform exactness should still be limited to what tests and environments establish, not inferred blindly across all compilers and instruction sets.
- **Null modulation buffers (v1.9.0):** The modulation stage hands the voices null buffers whenever nothing modulates (§10.5), and every source keeps its unmodulated code path for that case. `tests/compare_versions.py` renders the default sessions through the v1.8.0 and v1.9.0 builds and requires identical samples; `PitchTests` additionally checks that an all-1 pitch buffer renders exactly as no buffer.

**Repository reading:** `NeonSynthVoice.h`, `NeonSynthProcessor.cpp`, `Modulation.h`, `tests/FilterTests.cpp`, `tests/RealtimeSafetyTests.cpp`, `tests/ParameterLayoutTests.cpp`, `tests/StateFuzzTests.cpp`, `tests/SessionStateTests.cpp`, `tests/compare_versions.py` at tag `v1.10.1`.

# Part II — Listening laboratories

## 16. Experimental method

Start with a fresh instance or restore all relevant controls. Source switching alone is not a full reset.

| Control | Reference |
|---|---|
| Source / waveform | Oscillator / Sine |
| Overtone / Noise / Fold | 0% |
| Comb | 1 |
| Filter | Off (or LP, Cutoff max, Res 0%, Drive 0%) |
| LFO PITCH / CUTOFF / AMP | 0 |
| Mod wheel / pitch bend | At rest (bottom / centred) |
| Voices | 16 |
| Width | 0% |
| Gain | 0 dB |
| Attack / Decay | About 5 ms / 100 ms |
| Sustain / Release | 100% / about 150 ms |
| Sequencer | Off |

Use low hardware volume and repeatable notes/velocity. A MIDI clip removes performance variation. Listen first, describe second, inspect displays third. Change one variable at a time.

```text
Changed:
Kept fixed:
Prediction:
Heard:
Seen:
Explanation:
Uncertainty:
```

An honest inability to hear a difference is a valid observation.

## 17. Reading dependencies map

Use this map to pair each listening laboratory with its mathematical concepts, chapter sections, and source files before running experiments.

| Lab | Title | Prerequisite Chapters | Key Code / Test Files (v1.10.1) |
|---|---|---|---|
| **Lab 1** | Recognize the waveforms | Ch. 1, Ch. 2 (§2.1–2.5) | `Oscillator.h`, `tests/OscillatorTests.cpp` |
| **Lab 2** | Hear what the filter removes and emphasizes | Ch. 11 (§11.1–11.6), Ch. 14 | `CytomicSvf.h`, `NeonSynthVoice.h`, `tests/FilterTests.cpp` |
| **Lab 3** | Separate articulation from tone | Ch. 9, Ch. 11 (§11.3, §11.5) | `VoiceEnvelope.h`, `tests/VoiceEnvelopeTests.cpp` |
| **Lab 4** | CHOP: cut, invert, repeat | Ch. 2, Ch. 4 | `Oscillator.h` (`Chop`), `tests/ChopTests.cpp` |
| **Lab 5** | Fold a sine | Ch. 1 (§1.2), Ch. 6 | `Sculpt.h`, `tests/SculptTests.cpp` |
| **Lab 6** | Random versus Noise | Ch. 3, Ch. 5 | `Oscillator.h` (`RandomStepOscillator`), `Sculpt.h` |
| **Lab 7** | ROOT, pitch, and duration | Ch. 7 (§7.1–7.4) | `SampleReader.h`, `SampleData.h` |
| **Lab 8** | Loop joins | Ch. 7 (§7.5) | `SampleReader.h`, `tests/SampleReaderTests.cpp` |
| **Lab 9** | Granular freeze and scan | Ch. 8 | `GrainCloud.h`, `tests/GranularTests.cpp` |
| **Lab 10** | From fragments to texture | Ch. 8, Ch. 14 | `GrainCloud.h`, `SampleView.h` |
| **Lab 11** | Stereo between notes | Ch. 12 (§12.1) | `NeonSynthVoice.h` |
| **Lab 12** | Gate, swing, and release | Ch. 9, Ch. 13 | `StepSequencer.h`, `NeonSynthProcessor.cpp` |
| **Lab 13** | Headroom and clipping | Ch. 12 (§12.2–12.3) | `NeonSynthProcessor.h` (`softClip`) |
| **Lab 14** | What listening cannot prove | Ch. 2, Ch. 6, Ch. 11 (§11.4), Ch. 14, App. A | `tests/OscillatorTests.cpp`, `tests/FilterTests.cpp` |
| **Lab 15** | Three kinds of movement | Ch. 10 (§10.1–10.5), Ch. 11 (§11.5), Ch. 13 | `Lfo.h`, `Modulation.h`, `tests/LfoTests.cpp` |
| **Lab 16** | Bend, wheel, and the sample's versions | Ch. 7 (§7.3), Ch. 8, Ch. 10 (§10.2, §10.6), Ch. 14 | `Modulation.h`, `SampleReader.h`, `GrainCloud.h`, `tests/PitchTests.cpp` |

## 18. Sixteen guided laboratories

### Lab 1 — Recognize the waveforms

**Question:** Can you identify waveform character without depending on level?

**Procedure:** Hold A4 at consistent velocity. Compare Sine, Triangle, Square, and Sawtooth with neutral processing. Repeat on a lower note, then inspect Scope and Spectrum.

**Prediction:** Sine should concentrate energy at the fundamental; square and triangle should favor odd harmonics; saw should include both families.

**Interpretation:** Shape and harmonic distribution are two views of the same periodic signal. Different RMS levels mean equal peaks are not equal loudness.

**Code connection:** Waveform branches and 4-point B-spline residuals in `Oscillator.h`.

**Trap:** Do not equate louder with better. The manual's corresponding figure uses common scales without per-row normalization.

### Lab 2 — Hear what the filter removes and emphasizes

**Question:** Why does a cutoff sweep affect saw and sine differently, and how do mode, resonance, and drive alter the response?

**Procedure:** Hold a saw, enable the filter in LP mode, and sweep Cutoff. Compare HP, BP, and Notch modes, noting the smooth 10 ms mode crossfade. Next, raise Resonance to 75%–90% and sweep Cutoff across the saw's harmonics; then push Resonance to 98%–100% with Oscillator level down or Filter Envelope active to hear van der Pol self-oscillation track cutoff and follow the amplitude release. Finally, test Drive at 0%, 50%, and 100% on a quiet vs. loud note.

**Observation:** Saw brightness changes as upper harmonics fall, whereas a sine changes mainly in level unless Resonance pushes the filter into self-oscillation. Raising Drive lifts quieter passages by up to +8.6 dB while keeping full-scale peaks bounded at L.

**Interpretation:** A linear filter cannot remove upper harmonics that the source lacks, but a self-oscillating TPT filter adds its own sine tone at f<sub>c</sub> stabilized by quadrature van der Pol damping. Compare the rendered spectrum with the auto-scaled filter overlay without confusing them.

**Code connection:** `CytomicSvf.h` and filter/drive modulation in `NeonSynthVoice.h`.

**Trap:** Quieter and darker are not the same observation, and self-oscillation is not a source oscillator.

### Lab 3 — Separate articulation from tone

**Question:** How much character can amplitude shaping supply?

**Procedure:** Compare the same source using sustained (5 ms Attack, 100% Sustain, 150 ms Release), plucked (5 ms Attack, 300 ms Decay, 0% Sustain), and swelling (800 ms Attack and Release) settings. Use matching note and velocity. Release the swell before Attack finishes.

**Observation:** The early release should start from the current level. The manual illustrates this with measured audio, not only a drawing.

**Code connection:** `VoiceEnvelope.h` note-off and release-rate storage.

**Trap:** Sustain is a level. A one-shot ending is not proof of envelope failure.

### Lab 4 — CHOP: cut, invert, repeat

**Procedure:** Use a low saw, filter off, Noise/Fold zero, default alternating mask. Sweep Overtone through 0%, 100%, and 200%. At 100%, compare Comb 1, 2, 4, and 8; edit the pattern and repeat.

**Observation:** Listen separately for level, harmonic balance, and perceived pitch. The strongest component need not remain the fundamental. Raise pitch with high Comb and observe intentional effect reduction as segment length L shrinks from 4 samples toward 2 samples.

**Code connection:** `Chop::apply`, edge values, and depth reduction in `Oscillator.h`.

**Trap:** Default-mask observations do not generalize to every mask; Comb is not a delay filter.

### Lab 5 — Fold a sine

**Procedure:** Use Sine, neutral CHOP/Noise, filter off, Fold 100%. Sweep Drive from 1 upward, then compare Mix at 0%, 50%, and 100%. Repeat with Triangle and Sawtooth.

**Observation:** Listen for harmonic redistribution rather than a monotonic increase in level. Inspect the odd-harmonic structure on the sine. With a long Release, hear how the envelope scales the already-folded signal.

**Code connection:** Separate the nonlinear function from 2× oversampling, alignment, and DC removal in `Sculpt.h`.

**Trap:** Roughness alone does not identify aliasing.

### Lab 6 — Random versus Noise

**Procedure:** Hold Random with neutral sculpting for several seconds. Try to hum its pitch while noting color changes. Inspect SHAPE, then raise Noise to 100% and return it to zero.

**Interpretation:** Repetition can preserve pitch even when the shape was randomly chosen. SHAPE omits noise, so use rendered displays to inspect its addition.

**Code connection:** Compare `RandomStepOscillator` (`Oscillator.h`) and `Sculpt` noise (`Sculpt.h`).

**Trap:** At full Noise, filter and envelope still create recognizable character without a stable oscillator pitch.

### Lab 7 — ROOT, pitch, and duration

**Procedure:** Load a short recording, choose One-shot, fast Attack/full Sustain, and hold long enough. Play ROOT and octaves above/below. Keep the key fixed, change ROOT, and retrigger after each edit.

**Prediction:** An octave above doubles traversal rate and halves nominal playback duration. ROOT changes mapping, not detected pitch.

**Code connection:** Rate calculation, prefiltered level selection, and cubic read in `SampleReader.h`.

**Trap:** Note-off/Release can end the sound before the file does.

### Lab 8 — Loop joins

**Procedure:** Choose a steady source region, enable Loop, and compare short/long XFADE. Move a boundary to less similar material. Retrigger after every edit. Compare Loop and Loop until release with a long Release.

**Observation:** Listen for clicks, swells, tonal changes, and repeated events. A crossfade can soften a join while making correlated material swell or cancel.

**Code connection:** Fade region and post-wrap destination in `SampleReader.h`.

**Trap:** Longer crossfades are not automatically better or pitch-neutral.

### Lab 9 — Granular freeze and scan

**Procedure:** Use Granular, Scan 0, Size about 80 ms, Density about 30/s, Spray/Jitter 0. Hold a note, move Position, then try positive and negative Scan. Freeze again and change pitch.

**Observation:** Selection movement and grain playback are independent processes. A frozen cloud remains active; negative Scan does not reverse grains.

**Code connection:** Distinguish scan offset from grain read position in `GrainCloud.h`.

**Trap:** Frozen selection is not frozen output.

### Lab 10 — From fragments to texture

**Procedure:** With Scan frozen, reduce Density and compare grain sizes. Increase Density; then explore Spray and Pitch Jitter separately before combining them.

**Interpretation:** Explain Size as duration, Density as onset frequency, Spray as source displacement, and jitter as rate variation. Relate expected overlap to density times duration.

**Code connection:** Pool, scheduling, polynomial window, and normalization in `GrainCloud.h`.

**Trap:** Requested overlap can exceed the 32-grain limit, causing starts to be skipped. Displayed grain markers are not a complete internal dump.

### Lab 11 — Stereo between notes

**Procedure:** Play MIDI 60 at Width zero and maximum, then notes two octaves below and above, separately and together.

**Prediction:** The middle note stays centered, while low/high notes move to opposite sides. Inspect channel meters before judging a chord.

**Code connection:** Note-pan and constant-power gains in `NeonSynthVoice.h`.

**Trap:** A wide chord does not imply stereo sample import or chorus.

### Lab 12 — Gate, swing, and release

**Procedure:** Create a simple pattern with rests. With short Release, change Gate. Hold Gate fixed and lengthen Release. Restore short Release, increase Swing, and compare keyboard modes.

```text
step begins → note-on → Attack
Gate ends → note-off → Release
```

**Interpretation:** Gate schedules the event; the envelope determines the fade. Swing changes intervals, while gate duration uses nominal step length.

**Code connection:** `StepSequencer.h` and processor MIDI routing.

**Trap:** Release overlap is not necessarily a timing error.

### Lab 13 — Headroom and clipping

**Procedure:** At conservative hardware volume, compare one note and a chord at similar per-note velocity. Watch RMS/peak, raise Gain cautiously, and restore it.

**Observation:** Once clipping is active above −6 dBFS toward the −1 dBFS ceiling, more input produces greater harmonic deformation rather than proportionally higher peaks.

**Code connection:** `softClip` after output gain in `NeonSynthProcessor.h`; the tremolo, when AMP is not 0, comes before both.

**Trap:** Sample ceilings are neither true-peak guarantees nor listening-safety guarantees.

### Lab 14 — What listening cannot prove

**Procedure:** Play a saw progressively higher with neutral processing, then repeat with a folded sine and with pre-filter Drive at 100%. Inspect components without automatically labeling every unusual one an alias.

**Interpretation:** Candidate aliases may move outside the harmonic pattern, but leakage, filters, and hearing also affect observations. Stronger evidence comes from controlled offline references, delay alignment, suitable analysis intervals, and measured unwanted energy.

**Code connection:** Oscillator, sculpting, and ADAA filter spectral tests (`tests/OscillatorTests.cpp`, `tests/FilterTests.cpp`).

**Trap:** Inaudible in one patch does not mean alias-free. An error signal boosted for audition must be labeled with its added gain.

### Lab 15 — Three kinds of movement

**Question:** How does one slow control signal make three different kinds of movement, and why does the tremolo only dip?

**Procedure:** Choose Oscillator and Sawtooth, switch the filter on, and set Cutoff to about 800 Hz with Resonance at 40%. Set the LFO to Sine at 0.5 Hz with CUTOFF at +2 octaves, and hold a chord. Raise RATE to 6 Hz, then try Square and Random. Return CUTOFF to 0 and set AMP to 60% with Triangle at 5 Hz, watching the meter. Return AMP to 0 and set PITCH to 0.3 semitones at 6 Hz. Finally turn SYNC on, choose 1/8, and run the sequencer with CUTOFF at +2 octaves.

**Prediction:** The cutoff swings between about 200 Hz and 3.2 kHz, and every note of the chord opens and closes together, because there is one LFO. Square jumps between dark and bright without a click, even with resonance, because the jump takes 2 ms. At AMP 60% the level swings between full and 0.4 (about −8 dB), and the meter's peaks never rise above their unmodulated height. PITCH 0.3 semitones is ±30 cents.

**Interpretation:** One bipolar signal m[n] is scaled into three units: octaves of cutoff, a gain that only dips, and semitones of pitch. With SYNC and the sequencer running from the same transport, the sweeps land on the steps.

**Code connection:** `Lfo::render` in `Lfo.h`; `Modulation::render` and `applyAmplitude` in `Modulation.h`; `modulatedCutoffHz` in `NeonSynthVoice.h`.

**Trap:** With the filter off, CUTOFF depth does nothing. With SYNC on and the host stopped, the LFO free-runs at the tempo: it lands on the beat only while the transport plays.

### Lab 16 — Bend, wheel, and the sample's versions

**Question:** What does a bend change in each source, and when does a sample move to another of its prepared versions?

**Procedure:** Choose Sample with the built-in pad, Loop on, and hold the ROOT key. Set BEND to 24 and move the bend wheel slowly to its top and back. Repeat with Granular. Then choose Oscillator and Sine, hold A4, and raise the mod wheel with VIBRATO at 30 cents; compare it with the wheel down and the LFO's PITCH at 0.3 semitones and RATE 5.5 Hz. Finally run the sequencer in Transpose mode, hold a key, and bend.

**Prediction:** With the file at the host's sample rate, the Sample note moves to its half-rate version just above +3 semitones and to its quarter-rate version just above +15, and moves back about a semitone lower on the way down; none of these moves should click. Granular starts new grains at the bent pitch and carries the sounding ones with it. The wheel's vibrato and the LFO's at the same depth and rate should sound alike. The sequencer's notes bend with the held key's.

**Interpretation:** Pitch modulation multiplies each source's read rate, sample by sample. The version switch is an anti-aliasing decision, not a pitch change, and its crossfade is linear because both versions carry the same sound.

**Code connection:** `Modulation::handleMidi`; `SampleReader::levelFor` and `moveToLevel`; `settleSampleCopy` and `renderCloud` in `NeonSynthVoice.h`; `GrainCloud::stepNow`.

**Trap:** The Spectrum display's 11.72 Hz bins cannot resolve a 5.5 Hz vibrato's sidebands: a widened peak is the expected picture. If a wheel moves and the bar beside its knob does not, no MIDI is arriving.

## 19. Final sound-design exercise

Choose a hollow pluck, evolving digital tone, frozen atmosphere, looped instrument, or rhythmic noise patch.

Work in order: source, pitch mapping, envelope, sculpting, filter (mode, resonance, drive, filter envelope), modulation (LFO, bend, mod wheel), stereo, sequencing, output. Save a simple reference and the completed sound.

```text
The source is ... because ...
The envelope makes it ...
The main spectral change comes from ...
The variation comes from ...
The important limitation is ...
```

Programmers should also name the responsible classes and one test that would protect the behavior. The objective is prediction and explanation, not memorizing knob values.

# Part III — Supporting material

## Appendix A. Mathematics for DSP

### A.1 Notation and complex numbers

x[n] and y[n] denote input and output at integer sample n. F<sub>s</sub> is sample rate, f is hertz, and angular frequency is ω = 2πf / F<sub>s</sub> radians per sample.

A complex number z = a + jb, with j<sup>2</sup> = −1, represents a point in a plane:

```equation
magnitude = √(a^2 + b^2),    angle = atan2(b, a)
z = r e^{jθ},    e^{jθ} = cos θ + j sin θ
```

A rotating complex vector contains cosine and sine projections. Multiplication multiplies magnitudes and adds angles, simplifying cascaded filter gain/phase calculations.

Fourier coefficients contain phase even when only magnitude is displayed. Identical magnitude spectra do not require identical waveforms. This also explains why all-pass processing can preserve level while altering alignment.

### A.2 Fourier series

A periodic waveform can be written:

```equation
x(t) = a_0 + ∑_{k≥1} [a_k cos(2πk f_0 t) + b_k sin(2πk f_0 t)]
```

For an appropriately phased unit square:

```equation
x(t) = (4 / π) [sin(ω_0 t) + sin(3ω_0 t) / 3 + sin(5ω_0 t) / 5 + ···]
```

Sharp edges require substantial high-frequency content. Ideal formulas are targets; corrected oscillators are finite-rate approximations. See [Introduction to Digital Filters](https://www.dsprelated.com/freebooks/filters/) for the broader mathematical background.

### A.3 Sampling ambiguity

```equation
e^{j2π(f + m F_s)n / F_s} = e^{j2πf n / F_s}
```

for integer m. Sampling cannot distinguish these frequencies from samples alone. At 48 kHz, a 30 kHz real sinusoid aliases to 18 kHz, with phase depending on convention.

The same problem occurs when synthesis or nonlinear processing generates out-of-band frequencies internally.

### A.4 DFT, windowing, and normalization

```equation
X[k] = ∑_{n=0..N−1} x[n] e^{−j2πkn/N},    f[k] = k F_s / N
```

For real signals, negative-frequency information is redundant. Windowing multiplies samples by w[n] before analysis. For a suitable bin-centered sinusoid away from DC/Nyquist:

```equation
amplitude ≈ 2 |X_windowed[k]| / ∑ w[n]
```

Noise density requires a different normalization and interpretation. Zero-padding samples the same finite-record spectrum more densely; it does not create new resolving information.

### A.5 Convolution and LTI systems

An impulse is zero except at one sample. The impulse response h describes a linear time-invariant system:

```equation
y[n] = ∑_m h[m] x[n − m]
```

Time invariance means delaying input only delays output. A modulated filter is not strictly LTI; fixed-response formulas describe frozen settings, not every modulation detail.

### A.6 The z-transform

The z-transform represents sequences algebraically:

```equation
X(z) = ∑_n x[n] z^{−n}
```

A sample delay becomes z<sup>−1</sup>. For a one-pole smoother with zero initial state:

```equation
y[n] = (1 − a)x[n] + a y[n − 1],    H(z) = (1 − a) / (1 − a z^{−1})
```

Evaluating a stable system at z = e<sup>jω</sup> gives frequency response. The complex result describes gain and phase. See [frequency-response analysis](https://www.dsprelated.com/freebooks/filters/Frequency_Response_Analysis.html).

### A.7 Poles, zeros, stability

Zeros are numerator roots and poles denominator roots. For a causal finite-order rational transfer function in reduced form, bounded-input/bounded-output stability requires poles strictly inside the unit circle. See [pole-zero analysis](https://www.dsprelated.com/freebooks/filters/Pole_Zero_Analysis_I.html).

For the smoother, the pole is a, so |a| < 1. Near one, decay is slow. This fixed-coefficient criterion is not a complete proof for arbitrary time-varying coefficient sequences.

For the DC blocker:

```equation
H(z) = (1 − z^{−1}) / (1 − r z^{−1})
```

A zero at z = 1 rejects DC; a nearby pole controls the low-frequency transition.

### A.8 Select the right model

| Tool | Useful for | Limitation |
|---|---|---|
| Fourier series | Periodic waves and masks | Not complete for evolving randomness |
| FFT | Recorded blocks | Window/duration matter |
| Convolution | LTI filters | Not general nonlinear processing |
| z-transform | Delay and recursion | State/convergence assumptions matter |
| Pole analysis | Fixed-filter stability | Modulation and van der Pol damping need nonlinear analysis |
| Statistical power | Noise/grain overlap | Correlation changes sums |

## Appendix B. Troubleshooting by signal path

### B.1 Restore a diagnostic patch

Use Oscillator/Sawtooth, neutral sculpting, filter off (or LP, Cutoff max, Res 0%, Drive 0%), LFO depths at 0, mod wheel down and bend centred, Voices 16, Width zero, Gain 0 dB, fast Attack, full Sustain, moderate Release. Retrigger after selecting Source. Keep hardware volume low.

### B.2 No sound

```text
Does the built-in sequencer make sound?
  YES → investigate external MIDI routing.
  NO  → use Layer, not Transpose without a held key.
        Check RUN and host transport/SYNC.
        Restore a sustained diagnostic patch.
        Inspect output displays.
          Signal shown → investigate host/device/output path.
          No signal    → investigate events or synthesis.
```

For standalone isolation, use internal clock (SYNC off), Layer, about 120 BPM, and RUN. In DAW sync, start host playback as well. Displays are clues, not infallible proof: their own update path can fail. LFO AMP at 100% with a Square shape silences every other half-cycle by design.

### B.3 Sequencer works, keyboard does not

Enable the MIDI input in standalone settings. In a DAW, route the controller to the instrument and enable required monitoring/record-arm. Test Layer mode. The editor has no built-in piano keyboard; computer keys are not automatically MIDI notes.

### B.4 Displayed output but silence

Trace instrument → track → routing → master → selected device/channels → physical playback. Compare meters at successive stages. Standalone Audio/MIDI Settings controls its own output. Host-specific menus vary; this book is not a certification for every DAW.

### B.5 Silent Sample/Granular

Restore PAD, select the source, and retrigger. Inspect loading errors, then test a simple known file. Check Start, loop region, and envelope. Opposing stereo channels can cancel after mono downmix. ROOT is mapping, not pitch analysis.

### B.6 Apparently ineffective controls

| Symptom | Check |
|---|---|
| Held note ignores Source edit | Retrigger |
| Held sample ignores markers | Retrigger |
| Drive (Sculpt) does nothing | Fold may be zero |
| Drive (Filter) does nothing | Filter may be bypassed |
| CHOP does nothing | Overtone/source/high-frequency fade (L ≤ 2) |
| Noise/Fold on Sample does nothing | Unsupported source path |
| Width on middle note does nothing | MIDI 60 stays centered |
| SHAPE ignores noise | Deliberate omission |
| Waveform selector is dimmed | Non-Oscillator source |
| LFO does nothing (view dimmed) | PITCH, CUTOFF, and AMP are all 0 |
| LFO CUTOFF does nothing | Filter may be bypassed |
| SYNC LFO not on the beat | Host transport stopped: the LFO free-runs at the tempo |
| Bend or wheel does nothing | BEND or VIBRATO at 0; a still bar beside the knob means no MIDI arrives |
| Pitch stays bent or wobbling | A bend left off-centre, the wheel up, or LFO PITCH not 0; controller 121 resets bend and wheel |
| Chord loses notes | Voices below 16 |

### B.7 Unexpected duration

Short notes: inspect note-off, Gate, Sustain/Release, sample ending, and voice stealing, which happens sooner with Voices below 16. Long notes: inspect Release, overlapping events, sustain pedal, and looping. One-shot still uses ADSR; Loop until release may fade before the remaining file is audible. A bend changes a sample's duration together with its pitch. Note that the filter envelope never ends a voice; only the amplitude envelope governs voice lifetime.

### B.8 Clicks and crackles

Loop-periodic artifacts suggest joins; repeatable control artifacts suggest smoothing/state; real-time-only failures warrant host/device/deadline investigation; replacement artifacts suggest publication/lifetime handling. A periodic pulse at the LFO rate is the tremolo or the cutoff sweep, not a fault.

Reproduce with a simple patch and identical MIDI. A stable offline reproduction focuses attention on DSP/state, but does not prove every environmental cause absent. Run the relevant repository tests instead of diagnosing by sound alone.

### B.9 Expected display differences

SHAPE can have sharp corners while rendered samples are corrected. The filter overlay can lead smoothed cutoff, does not include envelope, velocity, or LFO modulation (the green band under CUTOFF shows the LFO's swing), and scales down by up to 30 dB when resonant peaks exceed 0 dB. The LFO view's dot and the bend and wheel bars are read once per block. Noisy signals need not show harmonic lines. Short analyses may not resolve nearby tones. Peak and RMS measure different properties.

### B.10 Report enough to reproduce

```text
Version and resolved revision:
Standalone/plugin format:
OS, host, device if relevant:
Sample rate and block size:
Full patch/state and sample identity:
MIDI notes, velocities, controllers, and exact timing:
Steps from a fresh instance:
Expected and observed behavior:
Offline, real-time, or both:
```

Include the smallest render/test that reproduces the problem. Preserve an unprocessed audio example rather than adding effects that hide the artifact.

## Appendix C. Versioned feature-to-source index

### C.1 Scope

This index targets release tag `v1.9.0` ([kv244/Synth at v1.9.0](https://github.com/kv244/NeonSynth/tree/v1.10.1)). Paths are relative to the repository root; class names are more durable than line numbers.

### C.2 DSP and sources

| Feature | Source / entry point (v1.10.1) | Verification |
|---|---|---|
| Shared voice | `NeonSynthVoice.h`, `NeonSynthVoice` | Processor & voice tests |
| Periodic waves | `Oscillator.h`, `PolyBlepOscillator` | `tests/OscillatorTests.cpp` |
| BLEP/BLAMP | `Oscillator.h`, `Blep4` | Oscillator spectral tests |
| Compensation | `Blep4::TrebleCompensation` | Harmonic tests |
| Random | `RandomStepOscillator` in `Oscillator.h` | Repeatability/pitch tests |
| CHOP | `Chop`, `ChopSettings` in `Oscillator.h` | `tests/ChopTests.cpp` |
| Noise/Fold | `Sculpt.h`, `Sculpt` | `tests/SculptTests.cpp` |
| ADSR (Amp & Filter) | `VoiceEnvelope.h` | `tests/VoiceEnvelopeTests.cpp` |
| LFO | `Lfo.h`, `Lfo` | `tests/LfoTests.cpp` |
| Pitch bend, mod wheel, modulation buffers, tremolo | `Modulation.h`, `Modulation`, `ModulationBlock` | `tests/ModulationTests.cpp`, `tests/MidiModulationTests.cpp` |
| Pitch in the sources | `NeonSynthVoice.h` (`sourceSample`, `settleSampleCopy`, `renderCloud`), `Oscillator.h` (`nextSample (pitchRatio)`), `SampleReader.h` (`levelFor`, `moveToLevel`), `GrainCloud.h` (`setPitchRatio`, `stepNow`) | `tests/PitchTests.cpp` |
| Multi-mode Filter & ADAA Drive | `CytomicSvf.h`, `NeonSynthVoice.h` (`modulatedCutoffHz`, `filterStep`, `advanceFilterControls`) | `tests/FilterTests.cpp`, `tests/compare_versions.py` |
| Pitch-based Pan | `NeonSynthVoice.h` | Processor/render checks |
| Gain | `NeonSynthProcessor.cpp` | Processor checks |
| Clipper | `NeonSynthProcessor.h`, `softClip` | Headroom checks |
| Voices limit | `NeonSynthProcessor.h`, `LimitedSynthesiser::findFreeVoice` | `EditorTests` (lights), `RealtimeSafetyTests` (no allocation); stealing order by inspection |
| Preparation | `SampleData.h`, `create` | `tests/SampleDataTests.cpp` |
| Loading/PAD | `SampleLoader.h` | Loader/preparation tests |
| Publication | `SampleSlot.h` | Replacement/real-time tests |
| Playback/loops | `SampleReader.h` | `tests/SampleReaderTests.cpp` |
| Granular/SIMD | `GrainCloud.h` | `tests/GranularTests.cpp` |

Test filenames above are under `tests/`. A processor-level check does not imply a dedicated test file for every row.

### C.3 Events and interface

| Feature | Source (v1.10.1) |
|---|---|
| Sequencer | `StepSequencer.h` |
| Event routing, parameters, state | `NeonSynthProcessor.cpp` |
| Bend and wheel input | `Modulation.h` (`handleMidi`) |
| MIDI logging | `MidiLogger.h` |
| Editor, MODULATION panel, voice lights, GAIN fader | `NeonSynthEditor.h` |
| LFO view | `LfoView.h` |
| Scope/FFT/meters/envelope/filter view | `Visualisers.h` |
| Ideal-cycle/CHOP view | `ShapeView.h` |
| Sample/grain view | `SampleView.h` |
| Step grid | `SequencerView.h` |
| Help | `HelpOverlay.h` |
| Build/settings | `CMakeLists.txt` |

Integration checks include `ParameterLayoutTests`, `EditorTests`, `RealtimeSafetyTests`, `MidiModulationTests`, `RenderBenchmark`, `test_midi.py`, and `tests/compare_versions.py`.

### C.4 Maintaining the index

Record book edition, plugin version, resolved commit/tag, manual revision, review date, tests executed, tests only documented, and known discrepancies. At release, verify paths and symbols, recheck ranges and update policies, run tests, and review prose whenever behavior changes.

An automated checker can verify files/symbols and flag changed dependencies; it cannot decide whether a claim such as "latched at note-on" remains semantically correct.

## Appendix D. Model → Algorithm → Verified Behavior

This summary table contrasts the ideal continuous-time mathematical model, the discrete-time implementation in NeonSynth v1.10.1, and the verified behavior established by source inspection and automated tests.

| Subsystem | Ideal Model | Implemented Algorithm (v1.10.1) | Verified Behavior & Bounds |
|---|---|---|---|
| **Oscillators (§2.1, §2.4)** | Continuous phase accumulator φ(t) with infinite-bandwidth discontinuities | 4-point cubic B-spline `Blep4` residuals (±2 samples); integral from d to 2 for BLEP (0.5 at edge, 0 at d=2), second integral for BLAMP (7/30 at corner, 1/120 at d=1); `setWaveform()` latched at phase wrap past 1.0 | Exact alias cancellation for phase increment ≤ 0.25 cycles/sample; bounded residual aliasing up to 0.49 cap; treble compensation filter retains state across waveform switches |
| **CHOP Modulation (§4)** | Instantaneous periodic binary masking x(t)(1 − o · p[k]) | Cycle-boundary latched pattern/Comb; 10 ms smoothed Overtone; BLEP-scaled jumps with high-frequency fade depth = clamp((L − 2)/2, 0, 1) where L = 1/(Δt · 16 C) | Full depth for L ≥ 4 samples, linear fade between 4 and 2, zero for L ≤ 2 samples |
| **Wavefolding (§6)** | Memoryless odd-symmetric sine folder F(x) = sin((π/2) drive · x) | 2× oversampled half-band FIR interpolation/decimation, DC blocker, and integer + first-order all-pass dry alignment | Substantially suppresses foldover aliasing; dry/wet alignment is a first-order group-delay approximation |
| **Sample & Granular (§7–8)** | Continuous sinc reconstruction and unbounded overlapping grain summation | 3 prefiltered levels (95-tap Blackman FIR) + Catmull–Rom cubic read; 32.32 fixed-point granular engine with [4u(1 − u)]<sup>2</sup> window and SIMD (scalar/SSE2/AVX2) | Capped at 32 simultaneous grains; 1/√(max(1, overlap)) gain scaling; FMA contraction disabled for cross-ISA consistency |
| **Envelopes & Smoothers (§9, §15)** | Piecewise-linear ADSR trajectories and exact exponential parameter glides | Linear per-sample step increments; amplitude envelope (`(source * level) * env`) + independent filter envelope (10/300/0.5/300 ms defaults) | v1.7.1 gliding smoother double-step bug (B6) fixed in v1.8.0; exact multiplication order preserved within 0 ULP of v1.7.1 |
| **LFO (§10.1, §10.5)** | Periodic control signal m(t) from −1 to +1 at 0.02–20 Hz, or phase-locked to musical time | One shared phase accumulator rendered per block into a per-sample buffer; seeded sample-and-hold; rate limiter of 2 per 2 ms; 10 ms shape crossfade; synced phase = frac(ppq / beats) | Shapes bit-exact to their formulas within a cycle; rates accurate 0.02–20 Hz; independent of block size; 1/8T at 120 BPM = 6 Hz; renders from `reset()` bit-identical (44.1/48/96 kHz) |
| **Pitch modulation (§10.2, §10.6)** | Instantaneous frequency f · 2<sup>s(t)/12</sup> for bend, vibrato and LFO in semitones | One `exp2` per sample, shared; oscillator step × r capped at 0.49; sample increment × r with version switch (2 ms linear crossfade, one semitone hysteresis); grain 32.32 step scaled once per chunk of ≤ 32 samples | Each source within 3 cents of note × ratio (granular 25); all-1 buffer bit-identical; 0 → +24 st sweep click-free; ±30 cents at 5.5 Hz; bend glides 5 ms from its sample; bend reaches channel-16 notes; vibrato 16 at 1.28%, bend granular at 6.05% of a core |
| **Cutoff & amplitude modulation (§10.3–10.4, §11.5, §12.2)** | f<sub>c</sub> · 2<sup>d·m(t)</sup>; bounded AM g = 1 − A(1 − m)/2 | LFO octaves in the cutoff sum, forcing 16-sample coefficient updates; tremolo on the voice sum before output gain; null buffers when idle | g never above 1, reaching 1 − A; LFO cutoff 3 octaves at 2 Hz moves brightness; lfo cutoff 16 at 1.73% vs 1.35%; default sessions bit-identical to v1.8.0 |
| **Voice allocation (§1.1)** | N-voice polyphony, oldest-first stealing | `findFreeVoice` over the first N voices (free, same note, oldest released, oldest); JUCE's own choice at 16 | Lights set the parameter in one gesture; no allocation while the limit changes; stealing order by inspection, no dedicated test |
| **Multi-Mode Filter (§11)** | Continuous 2-pole state-variable prototype (12 dB/oct) with LP/HP/BP/Notch outputs | Trapezoidal TPT (`CytomicSvf.h`) matching JUCE operation order; 10 ms mode crossfade; 20 ms exponential cutoff smoothing; coefficients updated every 16 samples while moving | Bit-identical to JUCE `StateVariableTPTFilter` at Res 0%, Drive 0%; 16-sample counter persists across blocks (887 ms vs 1498 ms per 60 s sweep on 16 voices) |
| **Filter Self-Oscillation & Drive (§11.3–11.4)** | Bounded limit-cycle oscillation and alias-free pre-filter saturation | Van der Pol damping k<sub>eff</sub> = k + Bq/(B+q) from quadrature states A<sup>2</sup> = (s<sub>1</sub><sup>2</sup> + s<sub>2</sub><sup>2</sup>)/4 scaled to L = max(vel · ampEnv, 10<sup>−6</sup>); double-precision 1st-order ADAA sat(u) = u/√(1+u<sup>2</sup>) with 5% dry crossfade | Self-oscillates at Res ≥ 98% (k ≤ 0), peak ≈ 1.05 L (±1 dB from 100 Hz to 5 kHz, −3.6 dB at 10 kHz, −5.6 dB at 15 kHz at 48 kHz), pitch within 0.02% of f<sub>c</sub>; Drive adds +8.6 dB boost to quiet signals with −32.4 dB vs −32.7 dB alias floor (11–17 dB cleaner than naive clipping) |
| **Output Soft Clipper (§12.3)** | Smooth static saturation curve bounding summed voice peaks | Rational polynomial q(u) = u(27+u<sup>2</sup>)/(27+9u<sup>2</sup>) between knee a = 0.50118723 (−6 dBFS) and ceiling b = 0.89125094 (−1 dBFS), capped at u = 3 (+4.5 dBFS) | Zero slope above +4.5 dBFS; ≈ 4 ns/sample vs ≈ 16 ns/sample for `std::tanh` (MSVC) |

# Part IV — JUCE and development

## 20. Framework features and development flow

### 20.1 What JUCE supplies

JUCE provides host-facing infrastructure, audio utilities, and GUI facilities. It does not design the instrument's DSP.

| Facility | Role in NeonSynth (v1.10.1) |
|---|---|
| `AudioProcessor` | Lifecycle, processing, state |
| `AudioProcessorEditor` | Window |
| `Synthesiser` / voice classes | Events and polyphony; `findFreeVoice` overridden for the Voices limit |
| `AudioBuffer` / `MidiBuffer` | Audio and timestamped events; bend and wheel read from the incoming `MidiBuffer` before the synth renders |
| `AudioProcessorValueTreeState` | Parameters/state integration |
| Attachments | Control bindings |
| `SmoothedValue` | Parameter ramps, bend and wheel glides, modulation depths |
| `AudioPlayHead` | Tempo and song position for the sequencer and the LFO's SYNC |
| `FFT` | Spectrum analysis (filtering now handled by custom `CytomicSvf.h`) |
| FIFO / atomics | Display transfer |
| Formats / thread pool | Loading/preparation |
| Components / Graphics / Timer | Custom UI |

NeonSynth provides its oscillator corrections, CHOP, folder, custom TPT/van der Pol resonant filter (`CytomicSvf.h`), ADAA drive, envelopes, readers, granular engine, sequencer, and (since v1.9.0) its LFO and modulation stage.

Two v1.9.0 choices show where NeonSynth steps around JUCE's defaults. The Voices limit is a `Synthesiser` subclass whose `findFreeVoice` override searches only the first N voices, and calls JUCE's own `findFreeVoice` when N is 16, so the old allocation is kept exactly. Pitch bend and the mod wheel are not taken from JUCE's per-voice `pitchWheelMoved()` and `controllerMoved()` callbacks, which JUCE delivers per channel, to the voices playing on that channel, between the sub-blocks it renders. Instead `processBlock` hands the incoming `MidiBuffer` to `Modulation`, which reads every message with its sample position and builds the buffers shared by all voices, before `synth.renderNextBlock()` runs. One message then moves every voice, whatever channel its note came from.

### 20.2 Lifecycle

```text
construct:
    parameters, voices, defaults
prepareToPlay:
    prepare DSP, allocate storage (modulation buffers included),
    reserve events, initialize timing
processBlock:
    acquire sample, snapshot parameters, set the Voices limit,
    read bend and wheel from the incoming MIDI and render the modulation,
    build MIDI, render voices, apply tremolo and output stages,
    publish displays
releaseResources:
    mark audio inactive and permit safe cleanup
save/restore:
    parameters plus patterns and sample information
```

Preparation may allocate. Rendering should not depend on file access, GUI progress, or dynamic allocation/destruction.

### 20.3 Build and verify

The release documentation specifies C++17, CMake 3.21+, and JUCE 9.0.0 supplied separately. CMake builds VST3 and Standalone.

```text
cmake -S . -B build \
  -DJUCE_DIR="/path/to/JUCE" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEONSYNTH_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

For a feature: define behavior and neutral setting; isolate the DSP; test spectra/timing/bounds/transitions; append parameters without changing old meanings; choose update policies; bind controls; verify host gestures; check restoration and compatibility; then run real-time tests, benchmarks, offline renders, and host validation.

### 20.4 The learning loop

Predict a result, listen under controlled conditions, inspect the appropriate display, explain the relationship, locate the responsible state and algorithm, and verify with repeatable tests. The manual supplies experiments; the source and tests connect them to mathematics and engineering decisions. This loop, rather than any individual synthesis trick, is the most transferable lesson of the instrument.
