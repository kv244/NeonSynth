# Session fixtures

Sessions as earlier releases saved them, for the "Sessions saved by earlier releases" checks
in `tests/SessionStateTests.cpp`: each must still load (its parameters, pattern, sample and
CHOP pattern, with every parameter added since at its neutral default) and play, and saved
again it must render the same.

| File | Saved as by | What it holds |
|---|---|---|
| `v1.2.0.xml` | v1.2.0 (BasicSynth) | 9 parameters; no stateVersion, no pattern |
| `v1.4.0.xml` | v1.4.0 | 16 parameters, stateVersion 2, the sequencer pattern |
| `v1.5.0.xml` | v1.5.0 | 29 parameters, stateVersion 3, the pattern and an embedded sample (FLAC) |
| `v1.10.0.xml` | v1.10.0 | every parameter, stateVersion 4, pattern, CHOP pattern and embedded sample |
| `v1.11.0.xml` | v1.11.0 | as v1.10.0, plus GLIDE, LEGATO, LENGTH, KEY and SCALE set and a tie and a slide in the pattern |

`v1.11.0.xml` and `v1.10.0.xml` were written by `SessionStateTests --write-fixture` (each by its own release) (a quarter-second test
sample, Sawtooth, the filter on at 900 Hz and a few other settings away from their defaults;
the sample's name and path were then made neutral by hand). The older three are cut from `v1.10.0.xml`
by `make_fixtures.py`, which keeps exactly what each release wrote, reading that release's
parameter IDs from its own source at the tag. The files are the XML inside the binary
block a host stores; the test wraps them with `copyXmlToBinary`, as JUCE does.

Do not regenerate a fixture to make a failing check pass: a failure here means a session a
user saved could open differently.
