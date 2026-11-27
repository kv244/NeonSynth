#!/usr/bin/env python3
"""A Machine Remembers: NeonSynth offline renderer and matching MIDI score.
SPDX-License-Identifier: AGPL-3.0-only
JUCE state-wrapper approach follows kv244/NeonSynth test_midi.py and
 docs/demo/make_demo.py (upstream copyright Razvan Julian Petrescu).

Updated: streaming JUCE base64, setup diagnostics, render progress bars,
contiguous-array 24-bit WAV fix. No tqdm required.

Score/MIDI only (standard library):
  python render_machine_remembers.py --out score_only
Audio (numpy, mido, pedalboard, compatible NeonSynth v1.11 VST3):
  python render_machine_remembers.py --vst "C:/Program Files/Common Files/VST3/NeonSynth.vst3" --out machine_remembers
Preview:
  python render_machine_remembers.py --vst /path/to/NeonSynth.vst3 --out preview --preview-seconds 30

First-draft composition; a full plugin render and listening review are required.
"""
import argparse
import bisect
import functools
import json
import math
import re
import struct
import sys
import time
import wave
from pathlib import Path
from xml.sax.saxutils import quoteattr

BPM = 100
SPB = 60.0 / BPM
BARS = 216
DURATION = BARS * 4 * SPB + 12
SECTIONS = [(0, 'The first signal'), (36, 'The mechanism wakes'),
            (72, 'The engine'), (120, 'Memory failure'), (168, 'Recognition')]
MOTIF = [69, 72, 71, 64]
CHORDS = [[45, 52, 57, 60, 64], [41, 48, 53, 57, 60],
          [48, 55, 60, 64, 67], [43, 50, 55, 59, 62]]


def score():
    tracks = []

    def track(name, params, gain, pan=0, sample=None):
        item = dict(name=name, params=dict(sequencer_on=False, **params),
                    gain=gain, pan=pan, sample=sample, notes=[], controllers=[], bends=[])
        tracks.append(item)
        return item

    def note(target, pitch, beat, duration, velocity=90):
        target['notes'].append([int(pitch), round(beat, 6),
                                round(beat + duration, 6), int(velocity)])

    opening = track('01_signal', dict(waveform='Sine', attack=.025, decay=.8,
                    sustain=.65, release=2., cutoff=6000., stereo_width=.1), .52)
    for bar in list(range(0, 32, 4)) + list(range(168, 212, 4)):
        for i, pitch in enumerate(MOTIF):
            note(opening, pitch, bar * 4 + i * 3, 2.5, 70 + i * 5)
    note(opening, 69, 848, 15, 65)

    cloud = track('02_memory_cloud', dict(source='Granular', sample_root=69,
                  grain_size=220., grain_density=40., grain_spray=.5,
                  grain_pitch_jitter=8., grain_scan=.07, attack=3., decay=1.,
                  sustain=.65, release=6., stereo_width=1., cutoff=1500.), .27, 0, 'seed')
    for bar in range(8, 212, 4):
        for pitch in CHORDS[(bar // 4) % 4][1:]:
            note(cloud, pitch + 12, bar * 4, 15, 65)

    pulse = track('03_clockwork', dict(waveform='Square', chop_overtone=120.,
                  chop_comb=3, fold_drive=3., fold_mix=20., attack=.002, decay=.12,
                  sustain=.12, release=.08, cutoff=1400., filter_resonance=35.,
                  stereo_width=.4), .23, -.3)
    order = [1, 2, 3, 2, 4, 2, 3, 1]
    for bar in range(36, 120):
        tones = CHORDS[(bar // 4) % 4]
        for j in range(8):
            if (j + bar) % 7 != 0:
                note(pulse, tones[order[j]] + 12, bar * 4 + j * .5, .22,
                     105 if j % 4 == 0 else 60 + 6 * (j % 3))

    sub = track('04_foundation', dict(waveform='Sine', attack=.008, decay=.3,
                sustain=.75, release=.12, filter_enabled=False, stereo_width=0.), .52)
    growl = track('05_engine', dict(source='Random', chop_overtone=130., chop_comb=2,
                  fold_drive=6., fold_mix=65., attack=.004, decay=.2, sustain=.5,
                  release=.09, cutoff=750., filter_resonance=40., stereo_width=.2), .3)
    for bar in range(52, 120):
        root = CHORDS[(bar // 4) % 4][0] - 12
        rhythm = ([(0, .8, 0), (1.5, .35, 0), (2, .7, 0), (3, .35, 7), (3.5, .3, 12)]
                  if bar >= 72 else [(0, 1.6, 0), (2.5, .9, 0)])
        for beat, duration, interval in rhythm:
            note(sub, root + interval, bar * 4 + beat, duration, 108)
            if bar >= 72:
                note(growl, root + 12 + interval, bar * 4 + beat, duration,
                     110 if beat == 0 else 92)

    arp = track('06_sparks', dict(waveform='Sawtooth', chop_overtone=95., chop_comb=4,
                fold_drive=4., fold_mix=40., filter_mode='Band-pass', filter_resonance=45.,
                cutoff=2800., attack=.002, decay=.15, sustain=.08, release=.25,
                stereo_width=.8), .19, .35)
    for bar in range(80, 120):
        tones = CHORDS[(bar // 4) % 4][1:]
        for j in range(16):
            if (bar + j) % 5:
                note(arp, tones[(j * 3 + bar) % 4] + 24, bar * 4 + j * .25, .16,
                     95 if j % 4 == 0 else 58 + (j % 4) * 7)

    lead = track('07_declaration', dict(waveform='Sawtooth', voices=1, legato=True,
                 glide=90., filter_resonance=35., filter_drive=20., cutoff=2400.,
                 attack=.04, decay=.4, sustain=.65, release=.6, vibrato_depth=30.,
                 pitch_bend_range=2, stereo_width=.1), .34)
    for bar in range(88, 120, 4):
        for i, pitch in enumerate(MOTIF):
            beat = bar * 4 + i * 3
            note(lead, pitch + (12 if bar >= 104 and i == 1 else 0), beat, 3.04, 106)
            for j in range(7):
                lead['controllers'].append([1, min(110, j * 18), beat + j * .4])
    for j in range(25):
        lead['bends'].append([-round(6500 * (j / 24) ** 2), 476 + j * .12])
    lead['bends'].append([0, 480])

    kick = track('08_impact', dict(waveform='Sine', voices=1, glide=45., fold_drive=2.,
                 fold_mix=10., attack=.001, decay=.3, sustain=0., release=.1,
                 filter_enabled=False, stereo_width=0.), .75)
    snare = track('09_backbeat', dict(waveform='Triangle', noise=65., cutoff=6500.,
                  attack=.001, decay=.14, sustain=0., release=.08, stereo_width=.2), .35, -.1)
    hats = track('10_particles', dict(waveform='Square', noise=100., filter_mode='High-pass',
                 cutoff=6500., attack=.001, decay=.035, sustain=0., release=.025,
                 stereo_width=.5), .18, .3)
    for bar in range(64, 120):
        for j in range(4):
            if bar % 8 == 7 and j == 3:
                continue
            beat = bar * 4 + j
            gap = 1 / (48000 * SPB)
            note(kick, 84, beat, gap, 110)
            note(kick, 33, beat + gap, .35, 120 if j == 0 else 105)
        for j in [1, 3]:
            note(snare, 50, bar * 4 + j, .15, 106)
        for j in range(8 if bar < 96 else 16):
            note(hats, 96, bar * 4 + j * (.5 if bar < 96 else .25), .045,
                 80 if j % 2 else 48)
        if bar % 8 == 7:
            for j in range(6):
                note(snare, 50, bar * 4 + 2.5 + j * .25, .12, 50 + j * 10)

    ruins = track('11_disintegration', dict(source='Granular', sample_root=69,
                  grain_size=300., grain_density=55., grain_spray=.7,
                  grain_pitch_jitter=20., grain_scan=.02, attack=2., decay=2.,
                  sustain=.7, release=8., cutoff=2200., stereo_width=1.), .27, -.2, 'phrase')
    for bar in range(120, 184, 4):
        for k, pitch in enumerate(CHORDS[(bar // 4) % 4][1:]):
            note(ruins, pitch + 12, bar * 4 + k * .35, 13.5, 65 + k * 3)

    fragments = track('12_recollections', dict(source='Sample', sample_root=69,
                      sample_loop='One-shot', attack=.15, decay=.5, sustain=.75,
                      release=1.5, cutoff=3200., stereo_width=.6), .3, .25, 'phrase')
    for bar in range(124, 168, 3):
        note(fragments, [57, 64, 69, 72][(bar // 3) % 4], bar * 4, 3 + bar % 5, 65 + bar % 20)
    for bar in range(172, 208, 8):
        note(fragments, 69, bar * 4, 12, 54)
    for item in tracks:
        item['notes'].sort(key=lambda event: (event[1], event[0]))
    return tracks


def vlq(value):
    result = [value & 127]
    value >>= 7
    while value:
        result.insert(0, (value & 127) | 128)
        value >>= 7
    return bytes(result)


def midi_bytes(tracks):
    ppq = 15360

    def meta(kind, data):
        return bytes([255, kind]) + vlq(len(data)) + data

    def chunk(events):
        events.sort(key=lambda event: (event[0], event[1]))
        buffer = bytearray()
        last = 0
        for tick, priority, data in events:
            buffer.extend(vlq(tick - last) + data)
            last = tick
        end = max(last, round(DURATION / SPB * ppq))
        buffer.extend(vlq(end - last) + b'\xff\x2f\x00')
        return b'MTrk' + struct.pack('>I', len(buffer)) + buffer

    conductor = [(0, 0, meta(3, b'A Machine Remembers')),
                 (0, 1, meta(81, (600000).to_bytes(3, 'big'))),
                 (0, 2, meta(88, bytes([4, 2, 24, 8])))]
    for bar, name in SECTIONS:
        conductor.append((bar * 4 * ppq, 3, meta(6, name.encode())))
    chunks = [chunk(conductor)]
    channels = [0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12]
    for item, channel in zip(tracks, channels):
        events = [(0, 0, meta(3, item['name'].encode()))]
        for pitch, on, off, velocity in item['notes']:
            events.extend([(round(on * ppq), 2, bytes([0x90 | channel, pitch, velocity])),
                           (round(off * ppq), 1, bytes([0x80 | channel, pitch, 0]))])
        for controller, value, beat in item['controllers']:
            events.append((round(beat * ppq), 0, bytes([0xB0 | channel, controller, value])))
        for value, beat in item['bends']:
            encoded = value + 8192
            events.append((round(beat * ppq), 0,
                           bytes([0xE0 | channel, encoded & 127, (encoded >> 7) & 127])))
        chunks.append(chunk(events))
    return b'MThd' + struct.pack('>IHHH', 6, 1, len(chunks), ppq) + b''.join(chunks)


ALPH = '.ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+'


def xml_unwrap(data):
    if len(data) < 8:
        raise ValueError('Truncated JUCE state wrapper')
    magic, length = struct.unpack('<II', data[:8])
    if magic != 0x21324356 or length > len(data) - 8:
        raise ValueError('Unsupported JUCE state wrapper')
    return data[8:8 + length].rstrip(b'\0').decode(), data[8 + length:]


def xml_wrap(text):
    data = text.encode() + b'\0'
    return struct.pack('<II', 0x21324356, len(data)) + data


def b64encode(data):
    """Streaming JUCE custom base64; never builds an audio-sized integer."""
    chars = []
    accumulator = 0
    bits = 0
    for byte in data:
        accumulator |= byte << bits
        bits += 8
        while bits >= 6:
            chars.append(ALPH[accumulator & 63])
            accumulator >>= 6
            bits -= 6
    if bits:
        chars.append(ALPH[accumulator & 63])
    return f'{len(data)}.' + ''.join(chars)


def b64decode(text):
    """Streaming decoder for JUCE custom base64 (not standard base64)."""
    size_text, chars = text.split('.', 1)
    expected_size = int(size_text)
    if expected_size < 0:
        raise ValueError('Invalid encoded sample size')
    lookup = {char: index for index, char in enumerate(ALPH)}
    data = bytearray()
    accumulator = 0
    bits = 0
    for char in chars:
        if char not in lookup:
            raise ValueError(f'Invalid JUCE base64 character: {char!r}')
        accumulator |= lookup[char] << bits
        bits += 6
        while bits >= 8:
            data.append(accumulator & 255)
            accumulator >>= 8
            bits -= 8
    if len(data) != expected_size:
        raise ValueError(f'Decoded size mismatch: expected {expected_size}, got {len(data)}')
    return bytes(data)


def setup_message(text):
    print('  ' + text, flush=True)


def load_sample(plugin, path):
    setup_message('Reading plugin state...')
    outer, tail = xml_unwrap(plugin.raw_state)
    match = re.search(r'<IComponent>([^<]*)</IComponent>', outer)
    if not match:
        raise ValueError('IComponent missing')
    setup_message('Decoding component state...')
    inner, private = xml_unwrap(b64decode(match[1]))
    element = '<Sample name=' + quoteattr(path.name) + ' path=' + quoteattr(str(path.resolve())) + '/>'
    inner, count = re.subn(r'<Sample\b[^>]*/>', lambda _: element, inner, count=1)
    if count != 1:
        raise ValueError('Sample state element missing; incompatible plugin')
    setup_message('Encoding updated component state...')
    component = b64encode(xml_wrap(inner) + private)
    setup_message('Applying sample state to plugin...')
    plugin.raw_state = xml_wrap(outer[:match.start(1)] + component + outer[match.end(1):]) + tail
    setup_message('Reading sample state back for verification...')
    saved_outer, _ = xml_unwrap(plugin.raw_state)
    saved_match = re.search(r'<IComponent>([^<]*)</IComponent>', saved_outer)
    if not saved_match:
        raise ValueError('IComponent missing after loading sample')
    setup_message('Decoding verification state...')
    check, _ = xml_unwrap(b64decode(saved_match[1]))
    sample_element = re.search(r'<Sample\b[^>]*/>', check)
    if (not sample_element or not re.search(r'\bhash="[0-9a-f]{16}"', sample_element[0])
            or 'flac=' not in sample_element[0]):
        raise ValueError('Sample did not load and embed successfully')
    setup_message('Sample loaded and verified.')


def automation(item, beat):
    bar = beat / 4
    name = item['name']
    if name == '02_memory_cloud':
        return dict(grain_position=.45 + .3 * math.sin(beat * math.pi / 128),
                    cutoff=800 + 1600 * (.5 + .5 * math.sin(beat * math.pi / 192)))
    if name == '03_clockwork':
        return dict(cutoff=1000 + 1800 * min(1, max(0, (bar - 36) / 70)),
                    chop_overtone=100 + 75 * math.sin(beat * math.pi / 32))
    if name == '05_engine':
        return dict(cutoff=400 + 1400 * (.5 + .5 * math.sin(beat * math.pi)) ** 2)
    if name == '06_sparks':
        return dict(cutoff=1800 + 2600 * (.5 + .5 * math.sin(beat * math.pi / 64)))
    if name == '11_disintegration':
        return dict(grain_position=.5 + .4 * math.sin(beat * math.pi / 96),
                    grain_size=80 + 340 * (.5 + .5 * math.sin(beat * math.pi / 128)),
                    cutoff=900 + 1800 * (.5 + .5 * math.cos(beat * math.pi / 160)))
    return {}


class Progress:
    """Dependency-free progress display; updates at most four times/sec."""
    def __init__(self, label, total):
        self.label = label
        self.total = max(1, total)
        self.started = time.monotonic()
        self.last = self.started
        self.done = 0
        self.draw()

    @staticmethod
    def clock(seconds):
        seconds = int(max(0, seconds))
        return f'{seconds // 60:02d}:{seconds % 60:02d}'

    def draw(self, final=False):
        fraction = min(1., max(0., self.done / self.total))
        elapsed = max(.001, time.monotonic() - self.started)
        eta = self.clock(elapsed * (1 - fraction) / fraction) if fraction else '--:--'
        filled = int(26 * fraction)
        bar = '#' * filled + '-' * (26 - filled)
        text = (f'{self.label:22s} [{bar}] {fraction * 100:6.2f}% '
                f'elapsed {self.clock(elapsed)} ETA {eta}')
        sys.stderr.write('\r' + text + ('\n' if final else ''))
        sys.stderr.flush()

    def update(self, done):
        self.done = min(self.total, max(0, done))
        now = time.monotonic()
        if now - self.last >= .25 and self.done < self.total:
            self.draw()
            self.last = now

    def finish(self):
        self.draw(final=True)


def _render_progress(function):
    @functools.wraps(function)
    def wrapped(plugin, item, start, end):
        progress = Progress(item.get('name', 'render'), end - start)
        try:
            for position, audio in function(plugin, item, start, end):
                yield position, audio
                progress.update(position + audio.shape[1] - start)
        except BaseException:
            progress.finish()
            sys.stderr.write(f"{item.get('name', 'render')}: interrupted or failed.\n")
            raise
        else:
            progress.finish()
    return wrapped


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vst', help='NeonSynth VST3 bundle or inner binary; omit for score/MIDI only')
    parser.add_argument('--out', type=Path, default=Path('machine_remembers'))
    parser.add_argument('--rate', type=int, default=48000, choices=[44100, 48000])
    parser.add_argument('--preview-seconds', type=float, default=None,
                        help='Render first N seconds; MIDI remains full length')
    args = parser.parse_args()
    if args.preview_seconds is not None and (not math.isfinite(args.preview_seconds) or args.preview_seconds <= 0):
        parser.error('--preview-seconds must be a positive finite number')
    args.out.mkdir(parents=True, exist_ok=True)
    tracks = score()
    (args.out / 'A_Machine_Remembers.mid').write_bytes(midi_bytes(tracks))
    (args.out / 'score.json').write_text(json.dumps(dict(title='A Machine Remembers',
        bpm=BPM, duration_seconds=DURATION, sections=SECTIONS, tracks=tracks), indent=2), encoding='utf-8')
    if not args.vst:
        print('Wrote full score and MIDI; audio not requested.')
        return

    import numpy as np
    import mido
    import pedalboard

    vst = Path(args.vst).expanduser().resolve()
    inner = vst / 'Contents' / 'x86_64-win' / vst.name
    if sys.platform == 'win32' and inner.is_file():
        vst = inner
    if not vst.exists():
        raise FileNotFoundError(vst)

    def configure(params, sample=None):
        setup_message('Loading plugin instance...')
        plugin = pedalboard.load_plugin(str(vst))
        missing = set(params) - set(plugin.parameters)
        if missing:
            raise ValueError(f'Incompatible build: missing {sorted(missing)}; '
                             f'available {sorted(plugin.parameters)}')
        if sample is not None:
            setup_message(f'Loading sample into plugin state: {sample.name}...')
            load_sample(plugin, sample)
        setup_message('Assigning patch parameters...')
        for key, value in params.items():
            setattr(plugin, key, value)
        setup_message('Setup complete.')
        return plugin

    print('Checking plugin parameters...', flush=True)
    for item in tracks:
        print('Checking ' + item['name'], flush=True)
        probe = configure(item['params'])
        for key, value in automation(item, 0).items():
            setattr(probe, key, value)
        del probe
    sample_rate = args.rate

    def messages(item):
        events = []
        for pitch, on, off, velocity in item['notes']:
            events.extend([(round(on * SPB * sample_rate), 2,
                            mido.Message('note_on', note=pitch, velocity=velocity)),
                           (round(off * SPB * sample_rate), 1,
                            mido.Message('note_off', note=pitch))])
        for controller, value, beat in item['controllers']:
            events.append((round(beat * SPB * sample_rate), 0,
                           mido.Message('control_change', control=controller, value=value)))
        for value, beat in item['bends']:
            events.append((round(beat * SPB * sample_rate), 0,
                           mido.Message('pitchwheel', pitch=value)))
        return sorted(events, key=lambda event: (event[0], event[1]))

    @_render_progress
    def blocks(plugin, item, start, end):
        events = messages(item)
        times = [event[0] for event in events]
        index = bisect.bisect_left(times, start)
        chunk_size = round(.025 * sample_rate)
        position = start
        first = True
        while position < end:
            count = min(chunk_size, end - position)
            batch = []
            while index < len(events) and events[index][0] < position + count:
                batch.append(events[index][2].copy(time=(events[index][0] - position) / sample_rate))
                index += 1
            for key, value in automation(item, position / sample_rate / SPB).items():
                setattr(plugin, key, value)
            audio = plugin.process(batch, count / sample_rate, sample_rate,
                                   buffer_size=256, reset=first)
            if audio.shape != (2, count) or not np.isfinite(audio).all():
                raise ValueError('Invalid audio block')
            yield position, audio
            first = False
            position += count

    def write24(path, blocks_iter, total_frames=None):
        """Write stereo blocks as 24-bit PCM with optional export progress."""
        progress = Progress('Final WAV export', total_frames) if total_frames is not None else None
        written = 0
        try:
            with wave.open(str(path), 'wb') as writer:
                writer.setnchannels(2)
                writer.setsampwidth(3)
                writer.setframerate(sample_rate)
                for audio in blocks_iter:
                    samples = np.rint(np.clip(audio.T, -1., 1. - 1. / 8388608)
                                      * 8388608).astype('<i4')
                    samples = np.ascontiguousarray(samples)
                    packed = samples.view('u1').reshape(-1, 4)[:, :3].tobytes()
                    writer.writeframesraw(packed)
                    written += audio.shape[1]
                    if progress:
                        progress.update(written)
        finally:
            if progress:
                progress.finish()

    assets = args.out / 'assets'
    assets.mkdir(exist_ok=True)
    seed_params = dict(sequencer_on=False, waveform='Sawtooth', cutoff=2500., attack=.1,
                       decay=.6, sustain=.65, release=1.5, chop_overtone=65.,
                       fold_drive=3., fold_mix=15.)
    seed = dict(name='seed', notes=[[69, 0, 12, 95]], controllers=[], bends=[])
    phrase = dict(name='phrase', notes=[[pitch, i * 3, i * 3 + 2.7, 90]
                  for i, pitch in enumerate(MOTIF)], controllers=[], bends=[])
    for name, item in [('seed', seed), ('phrase', phrase)]:
        print(f'Rendering source asset: {name}', flush=True)
        plugin = configure(seed_params)
        write24(assets / (name + '.wav'),
                (audio for _, audio in blocks(plugin, item, 0, sample_rate * 12)))
        del plugin

    length = DURATION if args.preview_seconds is None else min(DURATION, args.preview_seconds)
    total = round(length * sample_rate)
    if total < 1:
        raise ValueError('Requested duration is shorter than one sample')
    scratch = args.out / 'mix.float64.tmp'
    mix = np.memmap(scratch, dtype='float64', mode='w+', shape=(2, total))
    mix[:] = 0
    stats = []
    for item in tracks:
        start = max(0, round(item['notes'][0][1] * SPB * sample_rate) - sample_rate // 2)
        if start >= total:
            continue
        end = min(total, round((max(event[2] for event in item['notes']) * SPB + 12) * sample_rate))
        sample = assets / (item['sample'] + '.wav') if item['sample'] else None
        print(f"Rendering part: {item['name']}", flush=True)
        plugin = configure(item['params'], sample)
        setup_message('Starting audio render...')
        peak = 0.
        energy = 0.
        count = 0
        gains = np.array([min(1., 1 - item['pan']), min(1., 1 + item['pan'])])[:, None] * item['gain']
        for position, audio in blocks(plugin, item, start, end):
            peak = max(peak, float(np.max(np.abs(audio))))
            energy += float(np.sum(audio.astype('float64') ** 2))
            count += audio.size
            mix[:, position:position + audio.shape[1]] += audio * gains
        if peak <= 1e-9:
            raise ValueError('Unexpected silent part: ' + item['name'])
        stats.append(dict(name=item['name'], peak=peak, rms=math.sqrt(energy / max(count, 1))))
        print(item['name'], stats[-1], flush=True)
        del plugin

    block_size = sample_rate * 5
    peak = 0.
    progress = Progress('Fade / peak scan', total)
    try:
        for position in range(0, total, block_size):
            count = min(block_size, total - position)
            seconds = (position + np.arange(count)) / sample_rate
            fade = np.minimum(1, np.maximum(0, (length - seconds) / 5)) ** 2
            mix[:, position:position + count] *= fade
            peak = max(peak, float(np.max(np.abs(mix[:, position:position + count]))))
            progress.update(position + count)
    finally:
        progress.finish()
    if peak <= 1e-9:
        raise ValueError('Silent mix')
    normalization_gain = 10 ** (-1 / 20) / peak
    output_wav = args.out / 'A_Machine_Remembers.wav'
    write24(output_wav, (mix[:, position:position + block_size] * normalization_gain
                        for position in range(0, total, block_size)), total_frames=total)
    mix.flush()
    del mix
    scratch.unlink()
    report = dict(duration_seconds=length, sample_rate=sample_rate, parts=stats,
                  normalization_gain=normalization_gain, target_sample_peak_dbfs=-1,
                  plugin=str(vst), auditioned=False,
                  renderer_revision='streaming-base64-setup-diagnostics')
    (args.out / 'render_report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'Rendered: {output_wav}')
    print('Listening review still required.')


if __name__ == '__main__':
    main()
