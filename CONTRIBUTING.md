# Contributing to NeonSynth

Thank you for helping. This file says how to build and test a change and what a pull request must
keep. For security problems, read [SECURITY.md](SECURITY.md) first and do not open a public issue.

## Build and test

[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) has the commands; this section only says what to run.

| Step | Where it is described |
|---|---|
| Build the VST3 and the Standalone | [Building](docs/DEVELOPMENT.md#building) |
| Build and run the unit tests (`-DNEONSYNTH_BUILD_TESTS=ON`, then `ctest`) | [Unit tests and benchmark](docs/DEVELOPMENT.md#unit-tests-and-benchmark) |
| Run the Python suite against the VST3 (`python test_midi.py`) | [Python Testing](docs/DEVELOPMENT.md#python-testing) |
| Check that old sessions render the same (`tests/compare_versions.py`) | [Old sessions still sound the same](docs/DEVELOPMENT.md#old-sessions-still-sound-the-same) |
| Lint the Python files (`ruff check .`) | `pyproject.toml` |

Configure with `-DNEONSYNTH_WARNINGS_AS_ERRORS=ON` before you push: CI does, so a warning fails the
pull request. `EditorTests` opens the editor offscreen; on Linux run it under `xvfb-run`.

### The benchmark

`RenderBenchmark` (built with the tests) times `processBlock` without a host, from idle up to 16
granular voices. Run it before and after a change to the audio path and put both results in the pull
request. Compare runs on the same machine only: the numbers in
[Performance](docs/DEVELOPMENT.md#performance) are from the development machine, and CI's runners
are only roughly comparable.

## Code style

- C++20, in the JUCE style the code already uses: a space before the parentheses of a call, braces on
  their own line, 4 spaces, no tabs. Match the file you are in.
- Modules grouped by job under `src/`: `src/dsp` (signal processing), `src/sequencer`, `src/state`,
  `src/gui` and `src/plugin`. Code that runs for every sample (the oscillators, CHOP, the filter, the
  grain loop, the voice's render loop) stays header-only, so the compiler sees it inline; setup,
  per-note work and the GUI go in `.cpp` files. Those are `plugin/NeonSynthProcessor.cpp`,
  `plugin/Parameters.cpp`, `plugin/MidiRouter.cpp`, `plugin/MidiLogger.cpp`,
  `plugin/NeonSynthVoice.cpp`, `state/SessionState.cpp` and `gui/NeonSynthEditor.cpp` (v1.11.1); a
  new one goes in the plugin's `target_sources` and, if the tests need it, in `neonsynth_engine`'s
  sources in `CMakeLists.txt`. Includes name the folder (`#include "dsp/Oscillator.h"`). A new module
  goes in the folder of its job and in the [Project Structure](docs/DEVELOPMENT.md#project-structure)
  table.
- Comments explain why, not what: the reason for a constant, a choice or an ordering. Say in a
  function's comment which thread may call it, as the existing headers do ("Audio thread", "Message
  thread", "Any thread").
- No warnings under GCC, Clang or MSVC: no unused variables or parameters, no shadowed names, float
  comparisons through `juce::approximatelyEqual` or `juce::exactlyEqual` where they are meant.
- New source files carry the same three-line licence header as the others.

## Real-time rules

The audio thread runs `processBlock`, the voices and the sequencer. On it:

- **No allocation or freeing.** Buffers are sized in the constructor or `prepareToPlay`.
  `RealtimeSafetyTests` replaces `operator new` and `delete` and fails if `processBlock` allocates or
  frees, across every source, the sequencer, the LFO and a sample being replaced on another thread.
- **No locks.** Data crosses threads through atomics and lock-free FIFOs; see
  [Threads](docs/DEVELOPMENT.md#threads) for each hand-off.
- **No exceptions, no I/O, no waiting** on another thread. Formatting and file writes (the MIDI log,
  sample loading) happen on background threads.

The tests only catch allocation; locks and blocking calls are caught in review, so say in the pull
request which thread new code runs on.

## Rules every pull request keeps

| Rule | How it is checked |
|---|---|
| Old sessions render bit-identically. A session saved with an earlier release must sound the same, sample for sample. | `tests/compare_versions.py` renders 16 sessions through the v1.6.0 release and the new build in CI (`test.yml`) and fails on any difference |
| New parameters are appended at the end of the layout, with a neutral default (the value that leaves the sound as it was). Hosts store sessions by parameter ID and normalised value, so nothing may be renamed, moved or re-ranged. | `ParameterLayoutTests` compares every parameter with `tests/parameter-layout.txt`; after appending one, update the reference with `ParameterLayoutTests --write` and commit it |
| Every feature is visible in the GUI: every parameter has a control, each source shows its own controls with nothing overlapping, and every view paints at 75%, 100% and 150%. | `EditorTests` |
| The user manual describes every change, and its cover and "Applies to" name the release. | `docs/build_manual.py --check` in CI (`manual.yml`, and again at release); redraw the cover with `python docs/figures/cover.py --date "<release date>"` |
| The manual's "What's new in this version" section opens with an entry for the release (`### vX.Y.Z`): what a player notices, new, changed or fixed, with links to the chapters; the CHANGELOG keeps the full history. | `tests/integrity.py` (the integrity job) fails until the newest entry names the CMake version |
| The plugin passes pluginval at strictness 10. | `test.yml` on every pull request, and `release.yml` before a release |

A release also bumps the version in `CMakeLists.txt` (both places) and `installer.nsi`, and adds a row
to [CHANGELOG.md](CHANGELOG.md). The steps are at the top of `.github/workflows/release.yml`.

## How AI assistance is used

Much of NeonSynth's code and documentation is written with Claude (Anthropic), as the README's
licence section says. It goes through the same process as any other contribution:

1. Every change comes as a pull request and is reviewed by the maintainer before it is merged.
2. CI must be green: the build on GCC (Ubuntu) and MSVC (Windows) with warnings as errors, all unit
   tests including `RealtimeSafetyTests`, `ParameterLayoutTests` and `EditorTests`, the Python suite,
   the bit-identical comparison with the reference release, pluginval at strictness 10, and the
   manual check.
3. Before a release the maintainer installs the build, plays it and listens to it.

Contributions written with any AI tool are welcome on the same terms. Say so in the pull request, and
make sure you understand and can explain every line you submit.

## Licence

By contributing you agree that your contribution is licensed under the
[GNU Affero General Public License v3](LICENSE) (AGPL-3.0-only), like the rest of NeonSynth. The project
was relicensed from MIT to AGPLv3 at v1.4.0; releases up to and including v1.3.0 were MIT.
