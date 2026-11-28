#!/usr/bin/env python3
# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
# Licensed under the GNU Affero General Public License v3; see LICENSE.
"""Judges the unit tests' line coverage of src/ (v1.13.0). The coverage job runs it after llvm-cov.

    python tests/coverage_gate.py SUMMARY.json            check; exit 1 on a failure
    python tests/coverage_gate.py SUMMARY.json --write-baseline   record today's per-file figures

SUMMARY.json is `llvm-cov export -summary-only` over every test program. Two rules:

1. Floors: each folder of src/ (and the engine, every folder but gui, taken together) must keep
   at least the line coverage tests/coverage-floors.txt gives it. The floors sit about two points
   under the figures of 28 Nov 2026, so the job starts green and fails only on decline.
2. No silent drops: an engine file whose line coverage is more than DROP points below its figure in
   tests/coverage-baseline.json fails, even while its folder stays above the floor; that is new
   code merged without tests. Files new since the baseline are judged by the floors alone.

Raise the floors when coverage rises; record a new baseline at each release (the release checklist
says so), after looking at what changed. The table, and any failure, goes to stdout, which the job
also writes to its summary.
"""
import json
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FLOORS = ROOT / "tests" / "coverage-floors.txt"
BASELINE = ROOT / "tests" / "coverage-baseline.json"
DROP = 2.0                 # points
ENGINE = "engine"          # every folder of src/ but gui


def percent(covered, count):
    return 100.0 * covered / count if count else 100.0


def read_files(summary_path):
    """{'src/dsp/Blep4.h': (lines, covered)} for the files under src/."""
    data = json.loads(Path(summary_path).read_text())
    files = {}
    for entry in data["data"][0]["files"]:
        name = entry["filename"].replace("\\", "/")
        if "/src/" not in name:
            continue
        rel = "src/" + name.split("/src/", 1)[1]
        lines = entry["summary"]["lines"]
        files[rel] = (lines["count"], lines["covered"])
    return files


def read_floors():
    floors = {}
    for line in FLOORS.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            name, value = line.split()
            floors[name] = float(value)
    return floors


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    files = read_files(args[0])
    if not files:
        print("coverage gate: no files under src/ in the summary")
        return 1

    if "--write-baseline" in args:
        BASELINE.write_text(json.dumps({name: round(percent(c, n), 2) for name, (n, c) in sorted(files.items())},
                                       indent=1) + "\n")
        print(f"coverage gate: baseline written for {len(files)} files")
        return 0

    folders = defaultdict(lambda: [0, 0])
    for name, (count, covered) in files.items():
        folder = name.split("/")[1]
        for key in (folder, ENGINE) if folder != "gui" else (folder,):
            folders[key][0] += count
            folders[key][1] += covered

    failures = []
    floors = read_floors()
    print("| Area | Lines | Covered | Floor |")
    print("|---|---|---|---|")
    for key in sorted(folders, key=lambda k: (k == ENGINE, k)):
        count, covered = folders[key]
        value, floor = percent(covered, count), floors.get(key)
        print(f"| {key} | {count} | {value:.1f}% | {f'{floor:.0f}%' if floor is not None else '-'} |")
        if floor is not None and value < floor:
            failures.append(f"{key}: {value:.1f}% of lines covered, under its floor of {floor:.0f}%")
    for key in floors:
        if key not in folders:
            failures.append(f"{key}: has a floor but no files in the report")

    baseline = json.loads(BASELINE.read_text()) if BASELINE.exists() else {}
    for name, (count, covered) in sorted(files.items()):
        if name.split("/")[1] == "gui" or name not in baseline:
            continue
        value = percent(covered, count)
        if value < baseline[name] - DROP:
            failures.append(f"{name}: {value:.1f}% of lines covered, was {baseline[name]:.1f}% "
                            f"(more than {DROP:.0f} points down)")

    if failures:
        print("\n**Coverage gate failed:**\n")
        for failure in failures:
            print(f"- {failure}")
        return 1
    print(f"\nCoverage gate passed: every floor met, no engine file more than {DROP:.0f} points down.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
