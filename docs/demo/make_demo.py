# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
# Licensed under the GNU Affero General Public License v3; see LICENSE.
"""
Renders the demo piece, docs/demo/neonsynth-demo.wav, from the NeonSynth VST3.
The WAV is not kept in git (it is in .gitignore). One copy, the latest, is the asset
of the 'demo' release, which the README links to; after a re-render replace it with
    gh release upload demo docs/demo/neonsynth-demo.wav --clobber

Every sound is NeonSynth: six instances of the plugin, one per part, each
rendered offline with pedalboard and summed. Nothing else touches the audio
except a gain per part, a fade at the end and peak normalisation.

    python docs/demo/make_demo.py path/to/NeonSynth.vst3 [out.wav]

The parts (16 bars at 100 BPM, Am - F - C - G; v1.11):
  pad     Granular: a slow-scanning grain cloud of the built-in pad, wide; the LFO breathes on the cutoff
  bass    Random source (16 drifting steps) through CHOP and the wavefolder, filter wobbling
  acid    the step sequencer as a 303: one voice, ties, slides gliding at GLIDE time, accents
          (step velocity) opening a resonant, driven filter through the filter envelope;
          Transpose mode with HOLD: one key, pressed once and let go, latches the riff
  hats    Oscillator faded fully into NOISE, through the high-pass filter
  arp     Sawtooth, CHOP overtone and comb sweeping, folded, through a resonant band-pass
  lead    Sample: the built-in pad looped, MONO with LEGATO and GLIDE, the mod wheel's vibrato
          rising through long notes, and a pitch bend down at the end

Parameters that move during the piece are set every 25 ms between process calls;
the pattern, ties and slides are written into the plugin's saved state.
The Random source and the grain cloud are random, so two renders differ slightly.
"""
import math
import re
import struct
import sys
from pathlib import Path

import mido
import numpy as np
import pedalboard
from pedalboard.io import AudioFile

SR = 48000
BPM = 100.0
BEAT = 60.0 / BPM
BAR = 4 * BEAT
BARS = 16
TAIL = 4.0
LENGTH = BARS * BAR + TAIL
STEP = 0.025   # seconds between automation updates

# One chord per bar: root (MIDI, around C3), and the pad voicing.
CHORDS = [
    (45, [57, 60, 64, 71]),   # Am(add9)
    (41, [53, 57, 60, 64]),   # Fmaj7
    (48, [55, 60, 64, 67]),   # C
    (43, [55, 59, 62, 69]),   # G(add9)
]


def chord(bar):
    return CHORDS[bar % len(CHORDS)]


def plugin_path(vst):
    path = Path(vst).resolve()
    inner = path / "Contents" / "x86_64-win" / path.name
    return str(inner if sys.platform == "win32" and inner.is_file() else path)


# The plugin's saved state, to write the sequencer pattern (as test_midi.py does): pedalboard wraps
# the VST3 state as <VST3PluginState><IComponent>base64</IComponent>, JUCE's VST3 wrapper appends
# its own data after the plugin's block, and both blocks are copyXmlToBinary ("VC2!", size, XML).
JUCE_XML_MAGIC = 0x21324356
JUCE_BASE64 = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"


def xml_from_binary(data):
    magic, length = struct.unpack("<II", data[:8])
    assert magic == JUCE_XML_MAGIC, "not a JUCE binary XML block"
    return data[8:8 + length].rstrip(b"\0").decode("utf-8"), data[8 + length:]


def xml_to_binary(xml):
    text = xml.encode("utf-8") + b"\0"
    return struct.pack("<II", JUCE_XML_MAGIC, len(text)) + text


def base64_decode(text):
    size, chars = text.split(".", 1)
    return sum(JUCE_BASE64.index(c) << (6 * i) for i, c in enumerate(chars)).to_bytes(int(size), "little")


def base64_encode(data):
    n = int.from_bytes(data, "little")
    return f"{len(data)}." + "".join(JUCE_BASE64[(n >> (6 * i)) & 63] for i in range((len(data) * 8 + 5) // 6))


def set_pattern(plugin, steps):
    """steps: 16 of (note, velocity, 'tie' / 'slide' / '') or None for a rest."""
    outer, _ = xml_from_binary(plugin.raw_state)
    component = re.search(r"<IComponent>([^<]*)</IComponent>", outer).group(1)
    inner, private = xml_from_binary(base64_decode(component))
    cells = []
    for step in steps:
        note, velocity, mark = step if step else (45, 100, "")
        extra = f' {mark}="1"' if mark else ""
        cells.append(f'<Step on="{0 if step is None else 1}" note="{note}" velocity="{velocity}"{extra}/>')
    pattern = "<Pattern>" + "".join(cells) + "</Pattern>"
    inner, count = re.subn(r"<Pattern>.*?</Pattern>|<Pattern/>", pattern, inner, flags=re.S)
    assert count == 1, "no pattern in the saved state"
    plugin.raw_state = xml_to_binary(outer.replace(component, base64_encode(xml_to_binary(inner) + private)))


def notes(events):
    """(note, on_seconds, off_seconds, velocity) -> timed MIDI messages."""
    out = []
    for note, on, off, velocity in events:
        out.append((on, mido.Message("note_on", note=note, velocity=velocity)))
        out.append((off, mido.Message("note_off", note=note)))
    return out


def render(vst, params, messages, automation=None, pattern=None):
    """messages: (seconds, mido.Message). automation: seconds -> {parameter: value}."""
    plugin = pedalboard.load_plugin(vst)
    if pattern is not None:
        set_pattern(plugin, pattern)
    for key, value in params.items():
        setattr(plugin, key, value)

    # Work in whole samples: a message that lands a rounding error before the end of a chunk
    # would be dropped, so each one is given to the chunk its sample falls in.
    chunk_samples = int(round(STEP * SR))
    chunks = int(round(LENGTH / STEP))
    queued = [[] for _ in range(chunks)]
    for when, message in messages:
        sample = int(round(when * SR))
        index = min(chunks - 1, sample // chunk_samples)
        queued[index].append(message.copy(time=(sample - index * chunk_samples) / SR))

    out = []
    for index in range(chunks):
        if automation is not None:
            for key, value in automation(index * STEP).items():
                setattr(plugin, key, value)
        out.append(plugin.process(sorted(queued[index], key=lambda m: m.time), STEP, SR,
                                  buffer_size=256, reset=index == 0))
    return np.concatenate(out, axis=1)


def sweep(t, start, end, low, high):
    """Exponential glide from low to high between two times, held outside them."""
    x = min(1.0, max(0.0, (t - start) / (end - start)))
    return low * (high / low) ** x


def pad_part(vst):
    events = []
    for bar in range(BARS):
        for i, note in enumerate(chord(bar)[1]):
            events.append((note, bar * BAR + 0.03 * i, (bar + 1) * BAR - 0.05, 70 + 6 * i))
    return render(vst, dict(source="Granular", grain_position=0.25, grain_scan=0.15, grain_size=180.0,
                            grain_density=45.0, grain_spray=0.5, grain_pitch_jitter=14.0,
                            attack=1.2, decay=1.0, sustain=0.8, release=2.8, stereo_width=1.0, cutoff=500.0,
                            filter_resonance=25.0,
                            lfo_shape="Sine", lfo_rate=1.0 / (2 * BAR), lfo_cutoff=0.8),   # one breath per 2 bars
                  notes(events),
                  lambda t: dict(cutoff=sweep(t, 0.0, 8 * BAR, 500.0, 4000.0),
                                 grain_position=0.25 + 0.2 * math.sin(2 * math.pi * t / (8 * BAR))))


def bass_part(vst):
    rhythm = [(0.0, 0.9, 112), (2.0, 0.9, 104), (3.5, 0.4, 76)]   # beats: start, length, velocity
    events = []
    for bar in range(2, BARS):
        root = chord(bar)[0] - 12
        for start, length, velocity in rhythm:
            note = root + (12 if start == 3.5 else 0)
            events.append((note, bar * BAR + start * BEAT, bar * BAR + (start + length) * BEAT - 0.02, velocity))
    return render(vst, dict(source="Random", chop_overtone=70.0, chop_comb=1, fold_drive=4.0, fold_mix=35.0,
                            attack=0.004, decay=0.25, sustain=0.55, release=0.12, stereo_width=0.0, cutoff=260.0),
                  notes(events),
                  lambda t: dict(cutoff=260.0 * 2.0 ** (1.0 * (0.5 + 0.5 * math.sin(2 * math.pi * t / (2 * BEAT)))),
                                 fold_mix=sweep(t, 8 * BAR, 14 * BAR, 35.0, 75.0)))


# The acid line in A minor, written for the key C4 (Transpose plays it unshifted); it stays in A minor
# over all four chords. 'slide' glides into the next step at GLIDE time, 'tie' holds into it,
# velocity is the accent.
ACID = [(45, 127, ""), (45, 68, ""), (57, 100, "slide"), (55, 84, ""),
        (45, 118, ""), None, (48, 92, "slide"), (52, 78, ""),
        (45, 124, ""), (57, 74, "tie"), (57, 74, ""), (55, 104, "slide"),
        (48, 86, ""), (45, 120, ""), (43, 84, "slide"), (45, 98, "")]


def acid_part(vst):
    # HOLD latches the key in Transpose mode: C4 is pressed for a moment at bar 5 and the riff plays
    # on until HOLD is switched off at the end of bar 16.
    events = [(60, 4 * BAR, 4 * BAR + 0.2, 96)]
    return render(vst, dict(waveform="Sawtooth", voices=1, glide=70.0, hold=True,
                            sequencer_sync=False, sequencer_mode="Transpose", sequencer_tempo=BPM,
                            sequencer_rate="1/16", sequencer_gate=0.55, sequencer_on=True,
                            filter_mode="Low-pass", cutoff=220.0, filter_resonance=72.0, filter_drive=45.0,
                            filter_env_amount=3.2, filter_attack=0.001, filter_decay=0.16, filter_sustain=0.0,
                            filter_release=0.08, filter_velocity=1.6,
                            attack=0.002, decay=0.25, sustain=0.65, release=0.05, stereo_width=0.0),
                  notes(events),
                  lambda t: dict(cutoff=sweep(t, 4 * BAR, 12 * BAR, 220.0, 700.0),
                                 filter_resonance=72.0 + 14.0 * min(1.0, max(0.0, (t - 8 * BAR) / (4 * BAR))),
                                 hold=t < BARS * BAR),
                  pattern=ACID)


def hats_part(vst):
    events = []
    for bar in range(4, BARS):
        for eighth in range(8):
            on = bar * BAR + eighth * BEAT / 2
            if eighth % 2 == 1:                       # offbeat hat
                events.append((96, on, on + 0.03, 92))
            elif eighth in (2, 6):                    # backbeat: a longer, lower burst
                events.append((60, on, on + 0.11, 118))
            if bar >= 8 and eighth % 2 == 1:          # sixteenth ghost notes from bar 9
                events.append((100, on + BEAT / 4, on + BEAT / 4 + 0.02, 48))
    return render(vst, dict(waveform="Square", noise=100.0, fold_drive=7.0, fold_mix=55.0,
                            filter_mode="High-pass", cutoff=5000.0, filter_resonance=30.0,
                            attack=0.001, decay=0.05, sustain=0.0, release=0.06, stereo_width=0.7),
                  notes(events),
                  lambda t: dict(cutoff=sweep(t, 4 * BAR, 14 * BAR, 5000.0, 8000.0)))


def arp_part(vst):
    events = []
    for bar in range(8, BARS):
        tones = sorted(chord(bar)[1]) + [n + 12 for n in sorted(chord(bar)[1])]
        order = [0, 2, 4, 1, 5, 3, 6, 2, 7, 4, 6, 3, 5, 1, 4, 2]
        for i in range(16):
            on = bar * BAR + i * BEAT / 4
            events.append((tones[order[i]] + 12, on, on + BEAT / 4 * 0.7, 118 if i % 4 == 0 else 62 + 9 * (i % 3)))
    return render(vst, dict(waveform="Sawtooth", chop_overtone=0.0, chop_comb=2, fold_drive=5.0, fold_mix=45.0,
                            filter_mode="Band-pass", filter_resonance=55.0, filter_velocity=0.8,
                            attack=0.002, decay=0.14, sustain=0.15, release=0.2, stereo_width=0.9, cutoff=1500.0),
                  notes(events),
                  lambda t: dict(chop_overtone=100.0 + 95.0 * math.sin(2 * math.pi * (t - 8 * BAR) / (4 * BAR)),
                                 chop_comb=2 if t < 12 * BAR else 5,
                                 cutoff=sweep(t, 8 * BAR, 15 * BAR, 1200.0, 6000.0)))


def lead_part(vst):
    # note, start beat, beats; each note overlaps the next a little, so LEGATO glides between them.
    melody = [(76, 0.0, 3.0), (79, 3.0, 1.0), (77, 4.0, 2.5), (72, 6.5, 1.5),
              (76, 8.0, 2.0), (79, 10.0, 1.0), (84, 11.0, 1.0), (83, 12.0, 4.0),
              (81, 16.0, 3.0), (84, 19.0, 1.0), (83, 20.0, 2.0), (79, 22.0, 2.0),
              (77, 24.0, 2.0), (76, 26.0, 2.0), (74, 28.0, 1.0), (76, 29.0, 3.0)]
    start = 8 * BAR
    events = [(note, start + beat * BEAT, start + (beat + beats) * BEAT + 0.04, 100) for note, beat, beats in melody]
    messages = notes(events)
    end = start + 32 * BEAT
    t = start
    while t < end:                                    # the mod wheel rises through each long note
        beat = (t - start) / BEAT
        note_start = max(b for _, b, _ in melody if b <= beat + 1e-9)
        wheel = int(min(127, max(0, (beat - note_start - 0.5) * 60)))
        messages.append((t, mido.Message("control_change", control=1, value=wheel)))
        t += 0.05
    bend_from = end - 2 * BEAT                        # the last note bends down a whole tone
    for i in range(41):
        x = i / 40
        messages.append((bend_from + x * 1.5 * BEAT, mido.Message("pitchwheel", pitch=int(-8191 * x * x))))
    messages.append((end + 3.5, mido.Message("pitchwheel", pitch=0)))
    return render(vst, dict(source="Sample", sample_loop="Loop", sample_root=72, sample_start=0.05,
                            sample_loop_start=0.2, sample_loop_end=0.7, sample_crossfade=40.0,
                            voices=1, legato=True, glide=140.0, vibrato_depth=45.0, pitch_bend_range=2,
                            attack=0.15, decay=0.6, sustain=0.75, release=1.8, stereo_width=0.5, cutoff=3500.0),
                  messages,
                  lambda t: dict(cutoff=sweep(t, 8 * BAR, 16 * BAR, 2200.0, 6500.0)))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    vst = plugin_path(sys.argv[1])
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).with_name("neonsynth-demo.wav")

    parts = [("pad", pad_part, 0.7), ("bass", bass_part, 0.8), ("acid", acid_part, 0.6),
             ("hats", hats_part, 0.4), ("arp", arp_part, 0.45), ("lead", lead_part, 0.7)]
    mix = np.zeros((2, int(round(LENGTH / STEP)) * int(round(STEP * SR))), dtype=np.float64)
    for name, part, gain in parts:
        audio = part(vst)
        peak = float(np.max(np.abs(audio)))
        print(f"  {name:5s} peak {peak:.3f}, rms {float(np.sqrt(np.mean(audio ** 2))):.3f}")
        if not np.isfinite(audio).all() or peak == 0.0:
            print(f"  {name}: silent or not finite")
            return 1
        mix += gain * audio[:, :mix.shape[1]]

    fade = int(2.5 * SR)                                        # fade the tail out
    mix[:, -fade:] *= np.linspace(1.0, 0.0, fade) ** 2
    mix *= 10 ** (-1.0 / 20) / np.max(np.abs(mix))              # peak at -1 dBFS

    with AudioFile(str(out), "w", SR, 2, bit_depth=16) as f:
        f.write(mix.astype(np.float32))
    print(f"wrote {out} ({mix.shape[1] / SR:.1f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
