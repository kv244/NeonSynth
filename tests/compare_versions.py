# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
# Licensed under the GNU Affero General Public License v3; see LICENSE.
"""
Checks that sessions saved with an earlier release still sound the same.

Renders a fixed set of sessions through two builds of the VST3, the earlier
release and the current one, and requires the audio to be identical sample for
sample. Every parameter added since a release starts at a neutral default, so a
session that never touches the new controls must render bit-identically: this
is the check behind the roadmap's "defaults neutral" rule.

A session that sets a parameter the earlier release lacks (a new feature) has
nothing to compare against there, so it is rendered twice by the current build,
each time in a fresh process, and the two renders must match: the new feature
must be repeatable. Each build renders in its own process, so the two plugins
(which share a plugin ID) never meet.

Since v1.13.0 the factory bank is compared too: every preset, chosen through the
host's program list, plays CHORD (sequenced presets play their pattern on a held
note) in both builds, matched by name. A preset the earlier release lacks is
compared with a second render of the new build. A preset whose sound is meant to
change goes in PRESETS_CHANGED with the reason, for that release only.

    python tests/compare_versions.py OLD.vst3 NEW.vst3

Exits non-zero, naming the sessions that differ, if any does.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

SR = 48000


def notes(*events):
    """events: (note, on_seconds, off_seconds, velocity)"""
    import mido
    out = []
    for n, on, off, vel in events:
        out += [mido.Message("note_on", note=n, velocity=vel, time=on),
                mido.Message("note_off", note=n, time=off)]
    return out


CHORD = [(48, 0.0, 1.6, 100), (60, 0.1, 1.5, 90), (64, 0.2, 1.4, 70), (67, 0.3, 1.3, 50)]
LINE = [(36 + 7 * i, 0.25 * i, 0.25 * i + 0.2, 40 + 10 * i) for i in range(8)]

# name: (parameters, notes, seconds). Parameter names as pedalboard spells them.
SESSIONS = {
    "defaults-chord": ({}, CHORD, 2.2),
    "defaults-line": ({}, LINE, 2.5),
    "square-no-filter": (dict(waveform="Square", filter_enabled=False), CHORD, 2.2),
    "triangle-dark": (dict(waveform="Triangle", cutoff=300.0), LINE, 2.5),
    "saw-envelope": (dict(waveform="Sawtooth", attack=0.2, decay=0.4, sustain=0.3, release=0.8,
                          cutoff=5000.0, stereo_width=1.0), CHORD, 2.6),
    "saw-cutoff-low-gain": (dict(waveform="Sawtooth", cutoff=150.0, output_gain_db=-12.0, stereo_width=0.0), CHORD, 2.2),
    "loud-into-clipper": (dict(waveform="Sawtooth", output_gain_db=6.0, filter_enabled=False), CHORD, 2.2),
    "chop-noise-fold": (dict(waveform="Sawtooth", chop_overtone=150.0, chop_comb=3, noise=20.0,
                             fold_drive=6.0, fold_mix=70.0), CHORD, 2.2),
    "random-source": (dict(source="Random", chop_overtone=60.0, fold_mix=30.0), LINE, 2.5),
    "sample-loop": (dict(source="Sample", sample_loop="Loop", sample_start=0.1, sample_loop_start=0.3,
                         sample_loop_end=0.6, sample_crossfade=30.0), CHORD, 2.2),
    "sample-one-shot": (dict(source="Sample", sample_loop="One-shot", sample_root=48), LINE, 2.5),
    "sample-until-release": (dict(source="Sample", sample_loop="Loop until release"), CHORD, 2.6),
    "granular": (dict(source="Granular", grain_position=0.3, grain_scan=0.5, grain_size=120.0,
                      grain_density=60.0, grain_spray=0.4, grain_pitch_jitter=20.0), CHORD, 2.4),
    "granular-frozen": (dict(source="Granular", grain_scan=0.0, grain_size=40.0, grain_density=150.0), LINE, 2.5),
    # The sequencer in Transpose mode: it plays only while a key is held, so nothing sounds before the
    # render starts. (Layer mode with Sync off is not repeatable under pedalboard, which runs the
    # plugin while it applies parameters, so the free-running clock has already started: two
    # renders of the same build differ. StepSequencerTests cover its timing instead.)
    "sequencer-transpose": (dict(sequencer_sync=False, sequencer_mode="Transpose", sequencer_rate="1/8T",
                                 sequencer_on=True), [(64, 0.0, 2.0, 100)], 2.5),
    "sequencer-swing-gate": (dict(sequencer_sync=False, sequencer_mode="Transpose", sequencer_tempo=140.0,
                                  sequencer_rate="1/16", sequencer_swing=0.3, sequencer_gate=0.8, sequencer_on=True),
                             [(55, 0.0, 1.0, 100), (60, 1.2, 2.2, 100)], 2.6),
    # The filter's modes, resonance and drive (v1.8.0). Earlier builds lack these parameters, so
    # these sessions compare the new build with itself (see the module docstring).
    "filter-low-pass-res50": (dict(waveform="Sawtooth", cutoff=800.0, filter_mode="Low-pass",
                                   filter_resonance=50.0), LINE, 2.5),
    "filter-high-pass-res50": (dict(waveform="Sawtooth", cutoff=800.0, filter_mode="High-pass",
                                    filter_resonance=50.0), LINE, 2.5),
    "filter-band-pass-res50": (dict(waveform="Sawtooth", cutoff=800.0, filter_mode="Band-pass",
                                    filter_resonance=50.0), LINE, 2.5),
    "filter-notch-res50": (dict(waveform="Sawtooth", cutoff=800.0, filter_mode="Notch",
                                filter_resonance=50.0), LINE, 2.5),
    "filter-self-oscillation-chord": (dict(waveform="Sawtooth", cutoff=1200.0, filter_mode="Low-pass",
                                           filter_resonance=100.0), CHORD, 2.2),
    "filter-drive-saw": (dict(waveform="Sawtooth", cutoff=1500.0, filter_resonance=70.0,
                              filter_drive=100.0), CHORD, 2.2),
    # The filter envelope and velocity (v1.8.0), new-build only as above. LINE's velocities rise.
    "filter-env-pluck": (dict(waveform="Sawtooth", cutoff=200.0, filter_env_amount=4.0,
                              filter_resonance=40.0), LINE, 2.5),
    "filter-env-negative": (dict(cutoff=4000.0, filter_env_amount=-3.0), CHORD, 2.2),
    "filter-velocity": (dict(cutoff=300.0, filter_velocity=3.0), LINE, 2.5),
}

# Factory presets whose sound a release changes on purpose, with the reason; emptied after each
# release. They are rendered and must be repeatable, but are not compared with the old build.
PRESETS_CHANGED: dict = {}
HELD = [(48, 0.0, 3.0, 100)]   # a held note for sequenced presets (Transpose plays while it is held)


def plugin_path(vst: str) -> str:
    """An absolute path pedalboard can load. On Windows that is the DLL inside the bundle
    (Contents/x86_64-win/NeonSynth.vst3), as test_midi.py uses; elsewhere the bundle itself."""
    path = Path(vst).resolve()
    inner = path / "Contents" / "x86_64-win" / path.name
    return str(inner if sys.platform == "win32" and inner.is_file() else path)


def render(vst: str, out: str) -> None:
    import pedalboard
    probe = pedalboard.load_plugin(vst)
    known = set(probe.parameters.keys())
    results, skipped = {}, {}
    for name, (params, events, seconds) in SESSIONS.items():
        plugin = pedalboard.load_plugin(vst)
        missing = [k for k in params if k not in known]
        if missing:
            skipped[name] = missing
            continue
        for key, value in params.items():
            setattr(plugin, key, value)
        results[name] = plugin.process(notes(*events), seconds, SR, buffer_size=512, reset=True)
    program = probe.parameters.get("program")
    names = list(program.valid_values) if program is not None else []
    for name in names:
        plugin = pedalboard.load_plugin(vst)
        plugin.program = name
        sequenced = bool(getattr(plugin, "sequencer_on", False))
        events, seconds = (HELD, 3.4) if sequenced else (CHORD, 3.4)
        results["preset: " + name] = plugin.process(notes(*events), seconds, SR, buffer_size=512, reset=True)
    np.savez(out, **results)
    if skipped:
        print(f"{vst}: sessions skipped, parameters missing: {skipped}", file=sys.stderr)


def main() -> int:
    if len(sys.argv) == 4 and sys.argv[1] == "render":
        render(sys.argv[2], sys.argv[3])
        return 0
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    old_vst, new_vst = (plugin_path(v) for v in sys.argv[1:3])
    with tempfile.TemporaryDirectory() as tmp:
        def render_in_process(vst: str, i: int) -> dict:
            out = str(Path(tmp) / f"render{i}.npz")
            subprocess.run([sys.executable, __file__, "render", vst, out], check=True)
            with np.load(out) as data:   # closed again, or Windows can't delete the folder
                return {key: data[key] for key in data.files}

        old = render_in_process(old_vst, 0)
        new = render_in_process(new_vst, 1)
        # Sessions only the new build can render are checked against a second render of the new build.
        new_only = [name for name in SESSIONS if name in new and name not in old]
        again = render_in_process(new_vst, 2) if new_only else {}

        failed = []
        presets = [k for k in new if k.startswith("preset: ")]
        for name in presets:
            if name not in old or name[len("preset: "):] in PRESETS_CHANGED:
                new_only.append(name)
        if any(name in new_only and name not in again for name in presets):
            again = render_in_process(new_vst, 2)
        for name in list(SESSIONS) + presets:
            if name in new_only:
                a, b, against = again.get(name), new[name], "a second render of the new build"
                if name[len("preset: "):] in PRESETS_CHANGED:
                    against += f"; changed on purpose: {PRESETS_CHANGED[name[len('preset: '):]]}"
            elif name in old and name in new:
                a, b, against = old[name], new[name], "the old build"
            else:
                print(f"  SKIP  {name} (a parameter it sets, or the preset, is missing from the new build)")
                continue
            if a is None:
                failed.append(name)
                print(f"  FAIL  {name}: missing from the second render of the new build")
            elif not np.isfinite(b).all():
                failed.append(name)
                print(f"  FAIL  {name}: the new build's output is not finite")
            elif a.shape == b.shape and np.array_equal(a, b):
                peak = float(np.max(np.abs(b)))
                print(f"  PASS  {name}: identical to {against} ({b.shape[1]} samples, peak {peak:.3f})")
            else:
                failed.append(name)
                if a.shape != b.shape:
                    print(f"  FAIL  {name}: shapes differ, {a.shape} vs {b.shape}")
                else:
                    diff = np.abs(a - b)
                    first = int(np.argmax(diff.max(axis=0) > 0))
                    print(f"  FAIL  {name}: max difference {diff.max():.3g}, first at sample {first} "
                          f"({first / SR:.4f} s), {int((diff > 0).sum())} samples differ")

    print(f"\n{'ALL IDENTICAL' if not failed else 'DIFFERENT: ' + ', '.join(failed)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
