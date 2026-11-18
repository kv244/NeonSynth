# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
# Licensed under the GNU Affero General Public License v3; see LICENSE.
"""Cuts the earlier-release session fixtures from the current one.

    build/.../SessionStateTests --write-fixture tests/fixtures/state/v1.10.0.xml
    python tests/fixtures/state/make_fixtures.py

Each older fixture is the current session reduced to exactly what that release's
getStateInformation() wrote: only the parameters that release had (their IDs are read
from the release's own source with `git show <tag>:<file>`), and only the children and
stateVersion it wrote:

    v1.2.0   <Parameters> with 9 parameters; no stateVersion, no Pattern (before the sequencer)
    v1.4.0   + the 7 sequencer parameters, stateVersion="2", <Pattern>
    v1.5.0   + Source, Sample and Granular parameters, stateVersion="3", <Sample ... flac="...">

The parameter values are the plain values the APVTS writes, so they mean the same in
every release. Run from the repository root with the release tags fetched.
"""
import re
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
RELEASES = {
    "v1.2.0": ("BasicSynthProcessor.cpp", None, False, False),
    "v1.4.0": ("NeonSynthProcessor.cpp", "2", True, False),
    "v1.5.0": ("NeonSynthProcessor.cpp", "3", True, True),
}


def parameter_ids(tag, source):
    text = subprocess.run(["git", "show", f"{tag}:{source}"], check=True, capture_output=True, text=True).stdout
    return set(re.findall(r'ParameterID\s*\{\s*"([A-Za-z]+)"', text))


def main():
    current = ET.parse(HERE / "v1.10.0.xml")
    for tag, (source, version, pattern, sample) in RELEASES.items():
        root = ET.fromstring(ET.tostring(current.getroot()))
        ids = parameter_ids(tag, source)
        for name in list(root.attrib):
            if name != "stateVersion":
                del root.attrib[name]          # view properties came later
        if version is None:
            root.attrib.pop("stateVersion", None)
        else:
            root.set("stateVersion", version)
        for child in list(root):
            keep = (child.tag == "PARAM" and child.get("id") in ids) \
                or (child.tag == "Pattern" and pattern) or (child.tag == "Sample" and sample)
            if not keep:
                root.remove(child)
        missing = ids - {c.get("id") for c in root if c.tag == "PARAM"}
        assert not missing, f"{tag}: the current session lacks {missing}"
        ET.ElementTree(root).write(HERE / f"{tag}.xml", encoding="UTF-8", xml_declaration=True)
        print(f"{tag}: {len(ids)} parameters, stateVersion={version}, pattern={pattern}, sample={sample}")


if __name__ == "__main__":
    main()
