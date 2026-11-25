#!/usr/bin/env python3
"""Stereo/expression edition of A Machine Remembers.
SPDX-License-Identifier: AGPL-3.0-only

Place beside the latest render_machine_remembers.py (with its setup/base64 fixes).
Run this file INSTEAD of the original, with the same arguments:
 python render_machine_remembers_stereo.py --vst "C:/Program Files/Common Files/VST3/NeonSynth.vst3" --out machine_remembers_stereo
 python render_machine_remembers_stereo.py --out stereo_score
Optional: --base-renderer /path/to/render_machine_remembers.py

Preserves the base file unchanged. Requires numpy/mido/pedalboard only for audio.
Adds native LFO tremolo, CC1 vibrato, moving stereo balance, and causal stereo
cross-delays. Kick/sub remain centered and undelayed. This is a companion,
not a standalone replacement for the original renderer.
"""
import argparse
import ast
import copy
import json
import math
import sys
from pathlib import Path

# tremolo depths are percentages; rates are Hz; all modulation is global per part.
EXPRESSION = {
    '01_signal':          dict(depth=5., rate=.11, vibrato=12., wheel=35),
    '02_memory_cloud':    dict(depth=22., rate=.065, vibrato=9., wheel=30),
    '03_clockwork':       dict(depth=12., rate=.8333333333, vibrato=0., wheel=0),
    '04_foundation':      dict(depth=0., rate=.1, vibrato=0., wheel=0),
    '05_engine':          dict(depth=8., rate=1.6666666667, vibrato=0., wheel=0),
    '06_sparks':          dict(depth=15., rate=.4166666667, vibrato=10., wheel=45),
    '07_declaration':     dict(depth=9., rate=.2, vibrato=35., wheel=None),
    '08_impact':          dict(depth=0., rate=.1, vibrato=0., wheel=0),
    '09_backbeat':        dict(depth=0., rate=.1, vibrato=0., wheel=0),
    '10_particles':       dict(depth=8., rate=1.6666666667, vibrato=0., wheel=0),
    '11_disintegration':  dict(depth=30., rate=.045, vibrato=18., wheel=65),
    '12_recollections':   dict(depth=16., rate=.09, vibrato=22., wheel=55),
}
# pan excursion, cycle length in beats, phase in radians, delay wet gain.
SPATIAL = {
    '01_signal': ( .10, 96., .0, .04),
    '02_memory_cloud': (.38, 128., .0, .10),
    '03_clockwork': (.20, 16., .3, .045),
    '04_foundation': (0., 64., 0., 0.),
    '05_engine': (.08, 32., .4, .015),
    '06_sparks': (.43, 24., 1.2, .09),
    '07_declaration': (.14, 64., .2, .055),
    '08_impact': (0., 64., 0., 0.),
    '09_backbeat': (.06, 32., .0, .025),
    '10_particles': (.25, 12., .8, .045),
    '11_disintegration': (.45, 160., 1.6, .12),
    '12_recollections': (.36, 80., 2.2, .10),
}


def enhance_score(original, spb):
    tracks = copy.deepcopy(original())
    for item in tracks:
        settings = EXPRESSION[item['name']]
        item['params'].update(lfo_shape='Sine', lfo_sync=False,
                              lfo_rate=settings['rate'], lfo_amp=settings['depth'],
                              vibrato_depth=settings['vibrato'])
        wheel_max = settings['wheel']
        if wheel_max is not None:
            item['controllers'] = [e for e in item['controllers'] if e[0] != 1]
            if wheel_max:
                first = min(n[1] for n in item['notes'])
                last = max(n[2] for n in item['notes'])
                beat = first
                while beat <= last:
                    # Slow, smooth controller movement; actual MIDI retained in exported score.
                    value = round(wheel_max * (.5 - .5 * math.cos(2 * math.pi * (beat - first) / 32)))
                    item['controllers'].append([1, value, round(beat, 6)])
                    beat += .5
                item['controllers'].append([1, 0, round(last + .1, 6)])
            else:
                item['controllers'].append([1, 0, 0.])
        # The original lead's per-note wheel swells and pitch bends are preserved.
        item['controllers'].sort(key=lambda event: event[2])
    return tracks


class StereoProcessor:
    def __init__(self, spb):
        self.spb = spb
        self.history = {}

    def __call__(self, audio, gains, position, sample_rate, item):
        import numpy as np
        name = item['name']
        if name not in SPATIAL:
            return audio * gains
        excursion, period, phase, wet = SPATIAL[name]
        count = audio.shape[1]
        beat = (position + np.arange(count, dtype=np.float64)) / sample_rate / self.spb
        pan = excursion * np.sin(2 * np.pi * beat / period + phase)
        # Multiplicative stereo balance: no downmix, preserving the original stereo image.
        balance = np.stack((np.sqrt(1 - pan), np.sqrt(1 + pan)))
        dry = audio.astype(np.float64, copy=False)
        if wet:
            # Pure delays rather than chorus: no pitch shifting or feedback.
            left_delay = max(1, round(.011 * sample_rate))
            right_delay = max(1, round(.019 * sample_rate))
            length = max(left_delay, right_delay)
            state = self.history.get(name)
            if state is None:
                previous = np.zeros((2, length), dtype=np.float64)
            else:
                if state[0] != position or state[1] != sample_rate:
                    raise RuntimeError('Non-contiguous stereo-processing blocks for ' + name)
                previous = state[2]
            extended = np.concatenate((previous, dry), axis=1)
            echoes = np.stack((extended[1, length-left_delay:length-left_delay+count],
                               extended[0, length-right_delay:length-right_delay+count]))
            self.history[name] = (position + count, sample_rate, extended[:, -length:].copy())
            dry = (dry + wet * echoes) / (1 + wet)
        return dry * balance * gains


class MixerPatch(ast.NodeTransformer):
    """Replace only the existing audio-times-gains mix expression."""
    def __init__(self):
        self.matches = 0

    def visit_AugAssign(self, node):
        self.generic_visit(node)
        value = node.value
        if (isinstance(node.target, ast.Subscript)
                and isinstance(node.target.value, ast.Name)
                and node.target.value.id == 'mix'
                and isinstance(node.op, ast.Add)
                and isinstance(value, ast.BinOp) and isinstance(value.op, ast.Mult)
                and isinstance(value.left, ast.Name) and value.left.id == 'audio'
                and isinstance(value.right, ast.Name) and value.right.id == 'gains'):
            node.value = ast.copy_location(ast.Call(
                func=ast.Name(id='_stereo_process', ctx=ast.Load()),
                args=[ast.Name(id=name, ctx=ast.Load()) for name in
                      ['audio', 'gains', 'position', 'sample_rate', 'item']],
                keywords=[]), value)
            self.matches += 1
        return node


def load_base(path):
    text = path.read_text(encoding='utf-8-sig')
    tree = ast.parse(text, filename=str(path))
    functions = {n.name for n in tree.body if isinstance(n, ast.FunctionDef)}
    required = {'main', 'score', 'automation', 'b64encode', 'b64decode'}
    if not required.issubset(functions):
        raise ValueError('This is not the expected A Machine Remembers base renderer.')
    patch = MixerPatch()
    tree = patch.visit(tree)
    if patch.matches != 1:
        raise ValueError(f'Expected one mix expression; found {patch.matches}. Base file left unchanged.')
    ast.fix_missing_locations(tree)
    namespace = {'__name__': 'machine_remembers_base', '__file__': str(path)}
    exec(compile(tree, str(path), 'exec'), namespace)
    namespace['_stereo_process'] = StereoProcessor(namespace['SPB'])
    old_score = namespace['score']
    namespace['score'] = lambda: enhance_score(old_score, namespace['SPB'])
    return namespace


def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--base-renderer', type=Path,
                        default=Path(__file__).resolve().with_name('render_machine_remembers.py'))
    args, remaining = parser.parse_known_args()
    path = args.base_renderer.expanduser().resolve()
    if path == Path(__file__).resolve():
        raise ValueError('--base-renderer must point to the ORIGINAL renderer, not this companion.')
    if not path.is_file():
        raise FileNotFoundError('Keep the latest render_machine_remembers.py beside this companion, '
                                'or specify --base-renderer. Missing: ' + str(path))
    namespace = load_base(path)
    # Original main handles plugin preflight, sample loading, render bars and output.
    sys.argv = [str(path)] + remaining
    namespace['main']()
    if '--help' not in remaining and '-h' not in remaining:
        out = Path('machine_remembers')
        for i, arg in enumerate(remaining):
            if arg == '--out' and i + 1 < len(remaining):
                out = Path(remaining[i + 1])
            elif arg.startswith('--out='):
                out = Path(arg.split('=', 1)[1])
        if out.is_dir():
            (out / 'stereo_settings.json').write_text(json.dumps(dict(
                edition='stereo-expression', expression=EXPRESSION, spatial=SPATIAL,
                note='Moving pans and delays are audio-only, not represented in MIDI. '
                     'Native tremolo parameters are in score.json; CC1 is in MIDI.'), indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
