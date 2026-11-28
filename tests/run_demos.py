#!/usr/bin/env python3
# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
# Licensed under the GNU Affero General Public License v3; see LICENSE.
"""Runs every demo script in docs/demo against a NeonSynth VST3 (v1.13.0), so a release that
breaks one is caught in CI and not by the next person who runs it (as "Silent part: 24_identity"
was in v1.12). Each script renders its short form into a temporary folder; this checks that it
exits cleanly and that every WAV it wrote is finite and not silent.

    python tests/run_demos.py path/to/NeonSynth.vst3

About 30 seconds. Needs the Python test requirements (numpy, mido, pedalboard).
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

DEMO = Path(__file__).resolve().parent.parent / "docs" / "demo"
SILENT_DB = -60.0


def runs(vst: str):
    """(name, arguments, folder or file the run writes) for each demo, in its short form."""
    return [
        ("make_demo", [str(DEMO / "make_demo.py"), vst, "{out}/demo.wav"], "demo.wav"),
        ("render_machine_remembers", [str(DEMO / "render_machine_remembers.py"), "--vst", vst,
                                      "--out", "{out}/mr", "--preview-seconds", "20"], "mr"),
        ("render_machine_remembers_stereo", [str(DEMO / "render_machine_remembers_stereo.py"), "--vst", vst,
                                             "--out", "{out}/mrs", "--preview-seconds", "20"], "mrs"),
        ("render_phase_transitions", [str(DEMO / "render_phase_transitions.py"), "--vst", vst,
                                      "--preview", "--out", "{out}/pt"], "pt"),
    ]


def plugin_path(vst: str) -> str:
    """The bundle, or on Windows the DLL inside it, as test_midi.py and compare_versions.py load it."""
    path = Path(vst).resolve()
    inner = path / "Contents" / "x86_64-win" / path.name
    return str(inner if sys.platform == "win32" and inner.is_file() else path)


def check_wav(path: Path) -> str:
    """'' when the file is finite and not silent, else what is wrong with it."""
    from pedalboard.io import AudioFile
    with AudioFile(str(path)) as f:
        audio = f.read(f.frames)
    if audio.size == 0:
        return "empty"
    if not np.isfinite(audio).all():
        return "not finite"
    peak = float(np.max(np.abs(audio)))
    if peak <= 10 ** (SILENT_DB / 20):
        return f"silent (peak {20 * np.log10(peak + 1e-12):.1f} dBFS)"
    return ""


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    vst = plugin_path(sys.argv[1])
    failed = []
    with tempfile.TemporaryDirectory() as out:
        for name, arguments, wrote in runs(vst):
            command = [sys.executable] + [a.replace("{out}", out) for a in arguments]
            result = subprocess.run(command, cwd=out, capture_output=True, text=True)
            if result.returncode != 0:
                failed.append(name)
                print(f"  FAIL  {name}: exit code {result.returncode}\n{result.stdout[-2000:]}{result.stderr[-2000:]}")
                continue
            target = Path(out) / wrote
            wavs = [target] if target.suffix == ".wav" else sorted(target.rglob("*.wav"))
            problems = [f"{w.relative_to(out)}: {p}" for w in wavs if (p := check_wav(w))]
            if not wavs:
                problems = ["wrote no WAV"]
            if problems:
                failed.append(name)
                print(f"  FAIL  {name}: " + "; ".join(problems))
            else:
                print(f"  PASS  {name}: {len(wavs)} WAV file(s), all finite and audible")
    print(f"\n{'ALL DEMOS RUN' if not failed else 'FAILED: ' + ', '.join(failed)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
