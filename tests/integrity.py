# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
# Licensed under the GNU Affero General Public License v3; see LICENSE.

"""Consistency checks the compilers cannot make (the integrity job, v1.11.0).

    python tests/integrity.py            # every check; exits 1 if any finds a problem
    python tests/integrity.py --list     # also print what each check looked at
    python tests/integrity.py --cppcheck # cppcheck instead (see run_cppcheck)

1. Version: the same x.y.z in CMakeLists.txt (project and plugin), installer.nsi, the manual's
   "Applies to" line, the cover (docs/images/cover.json, written by docs/figures/cover.py) and
   the newest CHANGELOG row.
2. Parameters: every parameter in tests/parameter-layout.txt is named in the user manual and on
   the help page (HelpOverlay.h's text), by the label it has in the window. PARAMETER_TERMS
   below says which words count for each; a new parameter needs a line there.
3. Links: every local path the manual, README, DEVELOPMENT.md and the companion books
   (docs/theory/*.md) link to exists, and so does every repository path they quote in
   backticks (`src/dsp/Oscillator.h`). Links into this repository on GitHub are checked too
   when they point at master, main or the version being released (they will be these files).
4. Constants: each tuning constant that more than one place depends on is defined once.
   CONSTANTS lists them; add one whenever a review finds a duplicate.

With --cppcheck it runs cppcheck --enable=unusedFunction,style over src/ and tests/ instead
(cppcheck 2.13 or newer on the PATH) and fails on what is left after the filters in
run_cppcheck(): JUCE is not given to cppcheck, so it cannot see that JUCE calls the virtual
functions NeonSynth overrides.

Python 3.9+, standard library only.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANUAL = ROOT / "docs" / "NeonSynth-User-Manual.md"
HELP = ROOT / "src" / "gui" / "HelpOverlay.h"
LAYOUT = ROOT / "tests" / "parameter-layout.txt"
REPO_URL = "https://github.com/kv244/NeonSynth/"

# ---------------------------------------------------------------------------------------------
# 2. How each parameter is named in the window, and so in the manual and on the help page.
# Each entry is a list of regular expressions; any one found counts. An all-capitals word is a
# control's label and must match exactly; (?i) makes a phrase case-insensitive.
PARAMETER_TERMS = {
    "waveform":      [r"\bWaveform\b", r"\bWAVEFORM\b"],
    "cutoff":        [r"\bCUTOFF\b"],
    "filterEnabled": [r"\bON\b"],
    "attack":        [r"(?i)\battack\b"],
    "decay":         [r"(?i)\bdecay\b"],
    "sustain":       [r"(?i)\bsustain\b"],
    "release":       [r"(?i)\brelease\b"],
    "gain":          [r"\bGAIN\b"],
    "width":         [r"\bWIDTH\b"],
    "seqOn":         [r"\bRUN\b"],
    "seqSync":       [r"\bSYNC\b"],
    "seqTempo":      [r"\bTEMPO\b"],
    "seqRate":       [r"\bRate\b", r"\bRATE\b"],
    "seqSwing":      [r"\bSWING\b"],
    "seqGate":       [r"\bGATE\b"],
    "seqMode":       [r"\bTranspose\b"],
    "source":        [r"\bSOURCE\b"],
    "smpRoot":       [r"\bROOT\b"],
    "smpStart":      [r"(?i)\bstart\b"],
    "smpLoop":       [r"\bOne-shot\b"],
    "smpLoopStart":  [r"(?i)\bloop\b"],
    "smpLoopEnd":    [r"(?i)\bloop\b"],
    "smpXfade":      [r"\bXFADE\b"],
    "grPos":         [r"\bPOS\b"],
    "grScan":        [r"\bSCAN\b"],
    "grSize":        [r"\bSIZE\b"],
    "grDensity":     [r"\bDENS\b"],
    "grSpray":       [r"\bSPRAY\b"],
    "grJitter":      [r"\bJITTER\b"],
    "chopOvertone":  [r"\bOVERTONE\b"],
    "chopComb":      [r"\bCOMB\b"],
    "noiseLevel":    [r"\bNOISE\b"],
    "foldDrive":     [r"\bDRIVE\b"],
    "foldMix":       [r"\bFOLD\b"],
    "filterMode":    [r"(?i)\bhigh-pass\b"],
    "filterRes":     [r"\bRES\b"],
    "filterDrive":   [r"\bDRIVE\b"],
    "filterEnvAmt":  [r"\bENV\b"],
    "filterAttack":  [r"(?i)\bfilter envelope\b", r"\bFILTER the cutoff"],
    "filterDecay":   [r"(?i)\bfilter envelope\b", r"\bFILTER the cutoff"],
    "filterSustain": [r"(?i)\bfilter envelope\b", r"\bFILTER the cutoff"],
    "filterRelease": [r"(?i)\bfilter envelope\b", r"\bFILTER the cutoff"],
    "filterVel":     [r"\bVEL\b"],
    "lfoShape":      [r"\bLFO\b"],
    "lfoRate":       [r"\bRATE\b"],
    "lfoSync":       [r"\bSYNC\b"],
    "lfoDivision":   [r"(?i)\bnote length\b"],
    "lfoPitch":      [r"\bPITCH\b"],
    "lfoCutoff":     [r"\bCUTOFF\b"],
    "lfoAmp":        [r"\bAMP\b"],
    "bendRange":     [r"\bBEND\b"],
    "vibrato":       [r"\bVIBRATO\b"],
    "voiceCount":    [r"\bVOICES\b"],
    # v1.11.0
    "hold":          [r"\bHOLD\b"],
    "glide":         [r"\bGLIDE\b"],
    "legato":        [r"\bLEGATO\b"],
    "seqLength":     [r"\bLENGTH\b"],
    "seqKey":        [r"\bKEY\b"],
    "seqScale":      [r"\bSCALE\b"],
}

# ---------------------------------------------------------------------------------------------
# 4. Tuning constants that must be defined once. "name": a C++ name that may be defined (const
# or constexpr, at any scope) only once in src/. "literal": a number written out that may appear
# only once in code (comments are ignored) across the given files, because it is one setting
# used in several places.
CONSTANTS = [
    {"name": "softClipKnee"},
    {"name": "softClipCeiling"},
    {"name": "voiceGain"},
    {"name": "numVoices"},
    {"name": "jumpToleranceSeconds"},
    {"name": "keyboardChannel"},
    {"name": "transposeReferenceNote"},
    {"name": "stepsPerPattern"},
    {"name": "maxEmbeddedBytes"},
    {"name": "scopeFifoSize"},
    {"name": "baseWidth"},
    {"name": "baseHeight"},
    {"name": "slideController"},
    {"what": "the voice's 5 ms hard-stop fade (the envelope's release and the tail the host is told)",
     "literal": r"\b0\.005f?\b",
     "files": ["src/plugin/NeonSynthVoice.h", "src/plugin/NeonSynthProcessor.h", "src/plugin/NeonSynthProcessor.cpp"]},
    {"what": "the voice's 2 ms crossfade between two read positions of the sample (a retrigger, a move to another copy)",
     "literal": r"\b0\.002 \* sampleRate\b",
     "files": ["src/plugin/NeonSynthVoice.h"]},
]

problems = []
verbose = False


def problem(check, text):
    problems.append(f"[{check}] {text}")


def note(text):
    if verbose:
        print(f"    {text}")


def read(path):
    return path.read_text(encoding="utf-8")


def strip_comments(code):
    """C++ without its comments, line numbers kept (string literals are not parsed; good enough here)."""
    code = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), code, flags=re.S)
    return re.sub(r"//[^\n]*", "", code)


# ---------------------------------------------------------------------------------------------
def check_version():
    cmake = read(ROOT / "CMakeLists.txt")
    found = {}
    m = re.search(r"project\(NeonSynth VERSION\s+(\d+\.\d+\.\d+)", cmake)
    found["CMakeLists.txt project()"] = m.group(1) if m else None
    m = re.search(r"juce_add_plugin\(NeonSynth.*?\bVERSION\s+(\d+\.\d+\.\d+)", cmake, flags=re.S)
    found["CMakeLists.txt juce_add_plugin()"] = m.group(1) if m else None

    nsi = read(ROOT / "installer.nsi")
    m = re.search(r'!define PRODUCT_VERSION "(\d+\.\d+\.\d+)"', nsi)
    found["installer.nsi PRODUCT_VERSION"] = m.group(1) if m else None
    m = re.search(r"/DPRODUCT_VERSION=(\d+\.\d+\.\d+)", nsi)
    found["installer.nsi build comment"] = m.group(1) if m else None

    m = re.search(r"\*\*Applies to:\*\*\s*NeonSynth v(\d+\.\d+\.\d+)", read(MANUAL))
    found["manual, Applies to"] = m.group(1) if m else None

    cover = ROOT / "docs" / "images" / "cover.json"
    found["cover (docs/images/cover.json)"] = json.loads(read(cover)).get("version") if cover.exists() else None

    m = re.search(r"^\|\s*\[v(\d+\.\d+\.\d+)\]", read(ROOT / "CHANGELOG.md"), flags=re.M)
    found["CHANGELOG newest row"] = m.group(1) if m else None

    expected = found["CMakeLists.txt project()"]
    for where, version in found.items():
        note(f"{where}: {version}")
        if version is None:
            problem("version", f"no version found in {where}")
        elif version != expected:
            problem("version", f"{where} says {version}, CMakeLists.txt says {expected}")
    return expected


# ---------------------------------------------------------------------------------------------
def help_page_text():
    """The help page's text: the string literals in HelpOverlay.h's buildColumns()."""
    source = read(HELP)
    start = source.find("void buildColumns()")
    body = source[start:] if start >= 0 else source
    return "\n".join(m.group(1) for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', body))


def check_parameters():
    ids = [line.split()[1] for line in read(LAYOUT).splitlines() if line.strip()]
    texts = {"the manual": read(MANUAL), "the help page": help_page_text()}
    for pid in ids:
        terms = PARAMETER_TERMS.get(pid)
        if terms is None:
            problem("parameters", f"{pid}: no entry in PARAMETER_TERMS (tests/integrity.py): say how the window labels it")
            continue
        for where, text in texts.items():
            if any(re.search(t, text) for t in terms):
                note(f"{pid}: in {where}")
            else:
                problem("parameters", f"{pid} is not described in {where} (looked for {' or '.join(terms)})")
    for pid in sorted(set(PARAMETER_TERMS) - set(ids)):
        problem("parameters", f"PARAMETER_TERMS has {pid}, which is not in tests/parameter-layout.txt")


# ---------------------------------------------------------------------------------------------
LINK = re.compile(r"!?\[[^\]]*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
REFERENCE = re.compile(r"^\s*\[[^\]]+\]:\s*(\S+)", re.M)
HTML_SRC = re.compile(r"""<(?:img|a)\b[^>]*?\b(?:src|href)\s*=\s*["']([^"']+)["']""", re.I)
QUOTED_PATH = re.compile(r"`((?:src|tests|docs|fonts|\.github)/[A-Za-z0-9_./-]+)`")


def check_links(version):
    documents = [MANUAL, ROOT / "README.md", ROOT / "docs" / "DEVELOPMENT.md"]
    documents += sorted((ROOT / "docs" / "theory").glob("*.md"))
    local_refs = {"master", "main", f"v{version}"}

    for doc in documents:
        if not doc.exists():
            problem("links", f"{doc.relative_to(ROOT)} is missing")
            continue
        text = read(doc)
        # Fenced code blocks show commands and output, not links.
        text = re.sub(r"```.*?```", "", text, flags=re.S)
        name = doc.relative_to(ROOT).as_posix()
        targets = [m.group(1) for m in LINK.finditer(text)]
        targets += [m.group(1) for m in REFERENCE.finditer(text)]
        targets += [m.group(1) for m in HTML_SRC.finditer(text)]
        checked = 0

        for target in targets:
            path = None
            if target.startswith(REPO_URL):
                m = re.match(re.escape(REPO_URL) + r"(?:blob|tree|raw)/([^/]+)/([^#?]+)", target)
                if m and m.group(1) in local_refs:
                    path = ROOT / m.group(2)
            elif re.match(r"^[a-z][a-z0-9+.-]*:", target, flags=re.I) or target.startswith("#"):
                continue   # another site, mail, or a heading in the same document
            else:
                local = target.split("#", 1)[0].split("?", 1)[0]
                if local:
                    path = (doc.parent / local)
            if path is None:
                continue
            checked += 1
            if not path.exists():
                problem("links", f"{name} links to {target}, which does not exist")

        for m in QUOTED_PATH.finditer(text):
            quoted = m.group(1).rstrip(".,;:")
            if any(c in quoted for c in "*<>{}"):
                continue
            checked += 1
            if not (ROOT / quoted).exists():
                problem("links", f"{name} quotes `{quoted}`, which does not exist")
        note(f"{name}: {checked} paths")


# ---------------------------------------------------------------------------------------------
def check_constants():
    sources = sorted(p for p in (ROOT / "src").rglob("*") if p.suffix in (".h", ".cpp"))
    code = {p: strip_comments(read(p)) for p in sources}

    for entry in CONSTANTS:
        if "name" in entry:
            definition = re.compile(r"\b(?:constexpr|const)\b[^;(){}]*?\b" + re.escape(entry["name"]) + r"\s*(?:=|\{)")
            places = [f"{p.relative_to(ROOT).as_posix()}" for p, c in code.items() for _ in definition.finditer(c)]
            note(f"{entry['name']}: defined in {', '.join(places) or 'nowhere'}")
            if len(places) == 0:
                problem("constants", f"{entry['name']} is not defined in src/ any more: update CONSTANTS in tests/integrity.py")
            elif len(places) > 1:
                problem("constants", f"{entry['name']} is defined {len(places)} times: {', '.join(places)}")
        else:
            pattern = re.compile(entry["literal"])
            places = []
            for f in entry["files"]:
                path = ROOT / f
                if not path.exists():
                    problem("constants", f"{f} (for {entry['what']}) does not exist: update CONSTANTS")
                    continue
                for line_no, line in enumerate(strip_comments(read(path)).splitlines(), 1):
                    places += [f"{f}:{line_no}" for _ in pattern.finditer(line)]
            note(f"{entry['what']}: {len(places)} in code")
            if len(places) > 1:
                problem("constants", f"{entry['what']} is written out {len(places)} times: {'; '.join(places)}. "
                                     "Define it once and use the name")


# ---------------------------------------------------------------------------------------------
# cppcheck. Findings it reports by design that are not defects here, each with the reason.
CPPCHECK_IGNORED_IDS = {
    "useStlAlgorithm": "advice on style: a plain loop is as clear in DSP and test code",
    "missingIncludeSystem": "JUCE and the standard library are not given to cppcheck",
}
CPPCHECK_SUPPRESSIONS = [
    ("containerOutOfBounds", "src/gui/SpectrumView.h", "curve has numPoints + 1 entries; cppcheck takes it for numPoints"),
    ("badBitmaskCheck", "src/plugin/MidiRouter.cpp", "keyboardChannel - 1 is 0 on purpose: the channel bits are replaced"),
    ("knownConditionTrueFalse", "tests/EditorTests.cpp", "the page cache's count changes inside paint calls cppcheck does not follow"),
    ("knownConditionTrueFalse", "tests/SampleDataTests.cpp", "where no hash was recorded for the platform, the pad is compared with itself"),
    ("knownConditionTrueFalse", "tests/PresetTests.cpp", "the host's counters change inside pumpMessages(), on the message thread, which cppcheck does not follow"),
    ("knownArgument", "tests/PresetTests.cpp", "the same: the counter is read again after pumpMessages()"),
    ("unusedFunction", "src/state/PresetManager.h", "compareElements is the comparator juce::Array::sort calls by name"),
    ("suspiciousFloatingPointCast", "tests/FilterTests.cpp", "the float round trip is the point: the filter holds its coefficient as a float"),
]
JUCE_ENTRY_POINTS = {"createPluginFilter", "createEditor"}   # called by JUCE's plugin wrappers


def run_cppcheck():
    exe = shutil.which("cppcheck")
    if exe is None:
        print("cppcheck is not installed (apt install cppcheck)")
        return 2
    command = [exe, "--enable=unusedFunction,style", "--std=c++17", "--language=c++", "-I", "src",
               "--inline-suppr", "--quiet", "--template={file}|{line}|{severity}|{id}|{message}"]
    command += [f"--suppress={check}:{file}" for check, file, _ in CPPCHECK_SUPPRESSIONS]
    command += ["src", "tests"]
    print(" ".join(command))
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)

    sources = {p.relative_to(ROOT).as_posix(): read(p).splitlines()
               for folder in ("src", "tests") for p in (ROOT / folder).rglob("*") if p.suffix in (".h", ".cpp")}
    all_code = "\n".join("\n".join(lines) for lines in sources.values())

    def overrides(name):
        return re.search(r"\b" + re.escape(name) + r"\s*\([^;{]*\)[^;{]*\boverride\b", all_code) is not None

    findings = []
    for line in (result.stdout + result.stderr).splitlines():
        parts = line.split("|", 4)
        if len(parts) != 5:
            if line.strip():
                findings.append(line)
            continue
        file, line_no, severity, check, message = parts
        if check in CPPCHECK_IGNORED_IDS:
            continue
        if check == "unusedFunction":
            m = re.search(r"'([^']+)'", message)
            if m and (m.group(1) in JUCE_ENTRY_POINTS or overrides(m.group(1))):
                continue   # JUCE calls it
        findings.append(f"{file}:{line_no}: {severity} ({check}): {message}")

    for f in findings:
        print(f"  FAIL  {f}")
    print(f"cppcheck: {len(findings)} finding(s)" if findings else "cppcheck: all passed")
    return 1 if findings or result.returncode not in (0, 1) else 0


def main():
    global verbose
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--list", action="store_true", help="print what each check looked at")
    parser.add_argument("--cppcheck", action="store_true", help="run cppcheck (filtered) instead of the checks")
    args = parser.parse_args()
    verbose = args.list
    if args.cppcheck:
        return run_cppcheck()

    checks = [("version", check_version), ("parameters", check_parameters), ("links", None), ("constants", check_constants)]
    version = None
    for name, fn in checks:
        print(f"{name}")
        before = len(problems)
        if name == "version":
            version = fn()
        elif name == "links":
            check_links(version)
        else:
            fn()
        for p in problems[before:]:
            print(f"  FAIL  {p}")
        if len(problems) == before:
            print("  PASS")

    print(f"integrity: {len(problems)} problem(s)" if problems else "integrity: all passed")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
