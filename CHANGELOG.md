# Changelog

Every NeonSynth release, newest first. Each release on GitHub carries the Windows installer
(`NeonSynth-x.y.z-Windows-Setup.exe`) and, from v1.6.0, the user manual for that version as a PDF;
follow the version link to get them.

| Version | Date | What changed |
|---|---|---|
| [v1.10.1](https://github.com/kv244/Synth/releases/tag/v1.10.1) | 18 Nov 2026 | Hardening: a damaged or hostile session can no longer crash or hang the plugin (a bad embedded-sample size, parameter values that are not numbers, XML nested hundreds of thousands deep, a relative sample path); the host hears of a background sample load on the message thread; new tests for broken sessions, sessions saved by v1.2 to v1.10, random sequencer and envelope settings, and an ASan/UBSan CI job; SECURITY, CONTRIBUTING and this changelog. No change to controls or sound |
| [v1.10.0](https://github.com/kv244/Synth/releases/tag/v1.10.0) | 18 Nov 2026 | The retro CRT look: phosphor text and traces on a dark green tube inside a bezel, scope and spectrum afterglow, block meters, arc-ring knobs, the sequencer's steps showing their notes, embedded VT323 and IBM Plex Mono fonts, and a SCANLINES switch saved with the session. The layout and the sound are unchanged: old sessions sound bit-identical |
| [v1.9.0](https://github.com/kv244/Synth/releases/tag/v1.9.0) | 17 Nov 2026 | Movement: one LFO (five shapes, free or synced) on pitch, cutoff and level; pitch bend and a mod-wheel vibrato from MIDI, for keyboard and sequencer notes alike; click the voice lights to set how many voices play (1 = mono); a vertical GAIN fader beside the meters and a glow around every panel. Old sessions sound bit-identical; projects with recorded bend or mod-wheel data now respond to it |
| [v1.8.0](https://github.com/kv244/Synth/releases/tag/v1.8.0) | 15 Nov 2026 | The filter: low-pass, high-pass, band-pass and notch; resonance up to self-oscillation that follows the envelope; drive; a filter envelope (AMP / FILTER tabs) and velocity to cutoff; the spectrum draws the real response. Old sessions sound bit-identical |
| [v1.7.1](https://github.com/kv244/Synth/releases/tag/v1.7.1) | 13 Nov 2026 | Review fixes: sample loading and replacement, Granular bracket, CI runs once per commit, code statistics report; no change to controls or sound |
| [v1.7.0](https://github.com/kv244/Synth/releases/tag/v1.7.0) | 12 Nov 2026 | A new 1280 × 860 window that scales from 75% to 150%, four draggable points on the envelope graph, Esc closes the help from any control; the parameter-layout, editor and old-session comparison tests; the demo piece. No change to the sound |
| [v1.6.0](https://github.com/kv244/Synth/releases/tag/v1.6.0) | 11 Nov 2026 | Waveform sculpting: CHOP, Noise and Fold, with the SHAPE strip; cleaner band-limited oscillators; the illustrated manual and its PDF |
| [v1.5.0](https://github.com/kv244/Synth/releases/tag/v1.5.0) | 5 Nov 2026 | Sample, Granular and Random sources; the sample is saved inside the session |
| [v1.4.0](https://github.com/kv244/Synth/releases/tag/v1.4.0) | 5 Nov 2026 | Hardening: sample-accurate notes, the output soft clipper, a new envelope, no stuck notes. Renamed from BasicSynth to NeonSynth (old projects still open); licence changed from MIT to AGPLv3 |
| [v1.3.0](https://github.com/kv244/Synth/releases/tag/v1.3.0) | 4 Nov 2026 | The 16-step sequencer |
| [v1.2.0](https://github.com/kv244/Synth/releases/tag/v1.2.0) | 3 Nov 2026 | Visualisers, stereo width, a resizable window |
| [v1.1.0](https://github.com/kv244/Synth/releases/tag/v1.1.0) | 3 Nov 2026 | Real-time safety, JUCE 9, a rewritten voice |
| [v1.0.1](https://github.com/kv244/Synth/releases/tag/v1.0.1) | 21 Jun 2026 | Licence file, CI and README badges |
| [v1.0.0](https://github.com/kv244/Synth/releases/tag/v1.0.0) | 2 Jun 2026 | First release: four waveforms, a low-pass filter, 8 voices |

Releases up to and including v1.3.0 were published under the MIT license; from v1.4.0 NeonSynth is
licensed under the AGPLv3 (see [LICENSE](LICENSE)). Before v1.4.0 the plugin was called BasicSynth;
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#upgrading-from-basicsynth) explains the rename.
