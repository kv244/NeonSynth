<!-- NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu. SPDX-License-Identifier: AGPL-3.0-only.
     Appendices E-G of "The DSP Behind NeonSynth". build_book.py inserts this file before Part IV and
     adds its Appendix headings to the contents. Same conventions as the book: display equations are
     equation blocks in plain text, with x^{...} and x_{...} for superscripts and subscripts.
     SUGGESTED EDITS TO BE REVIEWED: delete the note under each appendix heading when accepted. -->

## Appendix E. Filters as difference equations

> **Suggested edit, to be reviewed.** This appendix is proposed for the next edition. Delete this note when it is accepted.

A digital filter turns an input sequence x[n] into an output sequence y[n] by a fixed rule. The filters in this book are linear and time-invariant (LTI): doubling the input doubles the output, and delaying the input delays the output by the same amount. Their key property is that a sine going in comes out as a sine of the same frequency, changed only in amplitude and phase. A filter is therefore described completely by what it does to each frequency.

```equation
Σ_k a_k y[n − k] = Σ_k b_k x[n − k]
```

A filter is **FIR** (finite impulse response) when every feedback coefficient a<sub>k</sub> is zero except the first: each output is a weighted sum of recent inputs and nothing is fed back. It is **IIR** (infinite impulse response) when past outputs are fed back, so a single input can ring on indefinitely. For the same amount of computation an IIR filter can reach a steeper slope; many DSP processors, however, are optimized for the multiply-and-add of FIR filters.

The frequency response follows by feeding in a complex exponential e^{jωn}, where ω is the frequency in radians per sample (π is the Nyquist frequency):

```equation
H(e^{jω}) = Σ_k h[k] e^{−jωk}
```

Here h[k] is the impulse response, the output for a single unit impulse as input. |H| is the gain at ω, and the angle of H is the phase shift.

### E.1 Worked FIR example: treble compensation

The treble compensation of §2.5 is a three-tap FIR filter:

```equation
y[n] = (1 + 2a) x[n − 1] − a ( x[n] + x[n − 2] )
```

Its taps are symmetric, so it delays every frequency by exactly one sample and distorts no phase. Its response is

```equation
H(e^{jω}) = e^{−jω} [ (1 + 2a) − 2a cos ω ]
```

With a = 0.27 the gain is:

| Frequency | Gain | Gain in dB |
|---|---|---|
| 0 (DC) | 1.00 | 0.0 |
| F<sub>s</sub> / 8 | 1.16 | +1.3 |
| F<sub>s</sub> / 4 | 1.54 | +3.8 |
| 3 F<sub>s</sub> / 8 | 1.92 | +5.7 |
| F<sub>s</sub> / 2 (Nyquist) | 2.08 | +6.4 |

The filter leaves low frequencies alone and lifts the top octave by up to 6.4 dB. This is the arithmetic behind the warning in §2.5 that restoring treble can also amplify any alias that remains near Nyquist. The NAIVE switch (§2.6) bypasses this filter along with the band-limiting.

### E.2 Worked IIR example: one-pole smoother

```equation
y[n] = (1 − p) x[n] + p y[n − 1]
```

One feedback term makes this a one-pole IIR filter. It is stable for |p| < 1; a p close to 1 smooths heavily and responds slowly. The state-variable filter of Chapter 11 is a two-pole IIR filter built from two integrators whose states are s<sub>1</sub> and s<sub>2</sub>.

*Background reading: Audio Engineering Explained (Elsevier, 2010), Chapter 15, DSP Technology.*

## Appendix F. Modulation: sidebands, beats and vibrato

> **Suggested edit, to be reviewed.** This appendix is proposed for the next edition. Delete this note when it is accepted.

Multiplying two sine waves produces their sum and difference frequencies:

```equation
cos a · cos b = ½ [ cos(a − b) + cos(a + b) ]
```

### F.1 Amplitude modulation and CHOP

Multiplying a carrier of frequency f<sub>c</sub> by (1 + m cos 2π f<sub>m</sub> t) gives the carrier plus two sidebands, each with m/2 of the carrier's amplitude:

```equation
(1 + m cos 2π f_m t) cos 2π f_c t
  = cos 2π f_c t + (m/2) cos 2π (f_c − f_m) t + (m/2) cos 2π (f_c + f_m) t
```

CHOP (Chapter 4) is amplitude modulation by a periodic gating wave rather than a sine, so each harmonic of the source gains sidebands at ± k times the chop rate, for whole numbers k.

### F.2 Beats

Two sines of nearly equal frequency f<sub>1</sub> and f<sub>2</sub> add to a wave whose loudness swells and fades at the difference frequency:

```equation
f_beat = | f_2 − f_1 |
```

For example, 440 Hz and 442 Hz beat twice a second.

### F.3 Frequency modulation and vibrato

Modulating a carrier's frequency with a sine at f<sub>m</sub> gives the carrier plus sidebands at f<sub>c</sub> ± k f<sub>m</sub>, for k = 1, 2, 3 and so on. The modulation index compares the peak frequency deviation Δf with the modulating frequency, and Carson's rule estimates the bandwidth that holds most of the energy:

```equation
β = Δf / f_m
BW ≈ 2 (Δf + f_m)
```

The larger β is, the more slowly the sideband amplitudes die away.

**Worked example.** A vibrato of half a semitone around 440 Hz moves the pitch by 440 × (2^{0.5/12} − 1) ≈ 12.9 Hz. At a vibrato rate of 5 Hz, β ≈ 2.6 and BW ≈ 36 Hz.

**Proportional modulation.** A vibrato that scales the pitch ratio of the whole sound moves harmonic n by n times the deviation of the fundamental, at the same rate. Its modulation index is therefore β<sub>n</sub> = n β. In the example above β is 2.6 for the fundamental, 5.2 for the second harmonic and 7.7 for the third. Perception research lists proportional modulation among the factors that seem to help the ear fuse harmonics into one sound.

### F.4 Tremolo and vibrato

Tremolo is modulation of level (amplitude modulation); vibrato is modulation of pitch (frequency modulation). A slow modulator gives sidebands so close to the carrier that they are heard as movement; a fast one gives sidebands far enough away to be heard as added tones.

*Background reading: L. Gunther, The Physics of Music and Color (Springer, 2019), Chapter 8, Complex Waves.*

## Appendix G. MIDI messages

> **Suggested edit, to be reviewed.** This appendix is proposed for the next edition. Delete this note when it is accepted.

MIDI sends instructions, not sound. Messages travel serially at 31,250 bits per second as 8-bit bytes. A **status byte** (top bit 1) says what to do and, for performance messages, on which of 16 channels. A **data byte** (top bit 0) carries a 7-bit value from 0 to 127.

### G.1 Note-on

A note-on is three bytes:

```text
1001 0100   0100 0000   0101 1001
status      data 1      data 2
note-on     key 64      velocity 89
channel 5
```

The low four bits of the status byte hold the channel, counted from zero, so 0100 is channel 5. A note-on with velocity 0 is generally treated as a note-off.

### G.2 Pitch bend

Pitch bend uses two data bytes, a 14-bit value of 16,384 steps centred on 8,192:

```equation
value = 128 × MSB + LSB          (0 … 16383, centre 8192)
bend (semitones) = range × (value − 8192) / 8192
r = 2^{bend / 12}
```

The ratio r is the pitch ratio of Chapter 10. With a range of 24 semitones, full bend up gives r ≈ 4.0 (two octaves) and full bend down gives exactly 0.25.

### G.3 Controllers

A control-change message carries a controller number and a value from 0 to 127. Numbers 0 to 31 are coarse values and 32 to 63 are their fine partners; a pair gives 14-bit resolution.

| Number | Meaning | Values |
|---|---|---|
| 1 | Modulation wheel | 0 to 127 (fine partner: 33) |
| 64 | Sustain pedal | 63 or less off, 64 or more on |
| 68 | Legato footswitch | 63 or less normal, 64 or more legato |
| 123 | All notes off | any |

### G.4 Timing

Synchronization uses single-byte real-time messages: timing clock, start, stop and continue. The MIDI specification sends the timing clock 24 times per quarter note.

*Background reading: Audio Engineering Explained (Elsevier, 2010), Chapter 16, MIDI.*
