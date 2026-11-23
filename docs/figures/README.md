# Manual figures

The figures in `docs/images/` for [the user manual](../NeonSynth-User-Manual.md), the scripts that make them, and how to remake them. Edition 2.0 of the manual, for NeonSynth v1.11.0. v1.11.0 changes the layout for the first time since v1.7: the header gains the preset bar and HOLD, the MODULATION panel GLIDE and LEGATO, and the sequencer a pattern row (LENGTH, KEY, SCALE, TOOLS, ENTER, UNDO), with TIE and SLD markers on each step. So Figure 1 (`ui-oscillator-running`) was retaken with v1.11.0 and gains regions 11–14, and Figure 8 now has its own capture, `ui-sequencer`, which shows a tie, two slides and LENGTH 12, with pointers 6–9 for them. The SOURCE panel did not change, so Figures 3 and 6 keep their v1.10.0 captures. The sound of default sessions did not change, so the rendered plots stay as they were. The cover was redrawn for v1.11.0.

How the v1.11.0 captures were set up: rather than setting controls with the mouse, the state was written into the Standalone's settings file (`~/.config/NeonSynth.settings`, `filterState`: JUCE's `copyXmlToBinary` of the session XML, in `MemoryBlock::toBase64Encoding`'s format) before starting it, so each capture shows exactly the stated settings. `ui-oscillator-running`: every parameter at its default except Sawtooth, LFO CUTOFF +1.5 oct, LFO RATE 2 Hz and RUN on; the frame with the playhead on step 9. `ui-sequencer`: the Rounded Bass factory preset's settings and pattern (slides on steps 8 and 12), plus a TIE on step 3 and LENGTH 12, RUN on; the frame with the playhead on step 9.

## What each figure is

| Figure | File | Kind | Made by |
|---|---|---|---|
| 1 Main window, fourteen numbered regions | `fig01-interface` | Actual application capture | `compose_ui.py` |
| 2 The four waveforms and their spectra | `fig02-waveforms` | Plot derived from rendered audio | `render_plots.py` |
| 3 The SOURCE panel with the SHAPE strip | `fig03-shape-strip` | Actual application capture | `compose_ui.py` |
| 4 Amplitude envelope | `fig04-envelope` | Plot derived from rendered audio | `render_plots.py` |
| 5 Low-pass filter, before and after | `fig05-filter` | Plot derived from rendered audio | `render_plots.py` |
| 6 The SOURCE panel for Sample and Granular | `fig06-sample-granular-ui` | Actual application capture | `compose_ui.py` |
| 7 Where Sample and Granular read | `fig07-sample-granular-schematic` | Explanatory schematic (over the pad's rendered waveform) | `render_plots.py` |
| 8 Step grid and gestures | `fig08-sequencer` | Actual application capture | `compose_ui.py` |
| 9 The five LFO shapes | `fig09-lfo-shapes` | Plot derived from rendered audio | `render_plots.py` |
| 10 The signal path | `fig10-signal-path` | Explanatory schematic, drawn from the source code (not from audio) | `signal_flow.py` |
| Cover | `cover` | Drawn artwork in the editor's CRT look, with the version and release date | `cover.py --date "<release date>"` (redraw on every version bump; CI checks the version) |

Every figure is written as an SVG (text as outlines) and a PNG at 300 dpi. All but the cover are drawn 160 mm wide, the print text width, with 9 pt labels and 10 pt panel titles in DejaVu Sans. Lines are told apart by style and label as well as by colour, so they read in grayscale. The captions in the manual carry the same kind label.

## Remaking them

```
pip install -r docs/figures/requirements.txt
python docs/figures/render_plots.py --vst3 path/to/NeonSynth.vst3 --revision <git revision>
python docs/figures/compose_ui.py
```

`render_plots.py` loads the plugin with Pedalboard, sends it MIDI and measures the audio. It asserts that every parameter landed (to 1%) and that both channels are identical, and writes the settings and measured values to `docs/images/render-settings.json`.

Common to all renders: 48 kHz, a fresh plugin instance each time, MIDI velocity 100, GAIN 0 dB, WIDTH 0% (so left and right are identical and the left is analysed), sequencer off, no normalisation. The rest is in the table below.

| Figure | Settings | Analysis |
|---|---|---|
| 2 | A4 (MIDI 69), ATK 5 ms, DEC 100 ms, SUS 100%, REL 150 ms, filter off, note held 1.9 s | Waveform: three cycles at 1.0 s. Spectrum: 1.0–1.5 s, Blackman-Harris window, 0 dBFS = full-scale sine. Same scales in every row. |
| 4 | Sine at A6 (MIDI 93), ATK 300 ms, DEC 300 ms, SUS 50%, REL 500 ms, filter off. Key down 0.1 s; key up 1.3 s, or 0.25 s for the early release | Peak of each 2 ms frame, scaled to the attack peak of the full note. Measured: sustain 50.1%, level at early release 50.0%. |
| 5 | Sawtooth at A3 (MIDI 57), ATK 5 ms, DEC 100 ms, SUS 100%, REL 150 ms, filter off then on at 1 kHz | 1.0–1.5 s. Measured: −3.1 dB at 1 kHz, −24.5 dB at 4 kHz (an ideal two-pole Butterworth gives −3.0 dB and −24.1 dB; the largest difference below 10 kHz is 2.7 dB). |
| 7 | Source Sample, built-in pad, ROOT C4, one-shot, played at C4 | The waveform behind panels a and c. Positions, grains and arrows are example drawings, not measurements. |
| 9 | Sine at A4 (MIDI 69), ATK 5 ms, DEC 100 ms, SUS 100%, REL 150 ms, filter off, LFO AMP 100%, RATE 2 Hz, note held 1.6 s; once per shape (Sine, Triangle, Square, Saw, Random) and once with LFO AMP 0% for the reference | Peak of each 2 ms frame, 0.3–1.3 s, scaled to the level with LFO AMP 0% (1 on the plot). The tremolo gain is 1 − AMP × (1 − LFO) / 2, so at 100% the level runs between 0 and 1. Measured: Sine 0.000–1.000, Triangle 0.003–1.000, Square 0.000–1.000, Saw 0.007–1.000, Random holds 0.238, 0.925, 0.53 in turn, stepping at 0.50 s and 1.00 s; period 0.499 s for the four regular shapes (2 Hz, within the 2 ms frames). |

The renders come from the Linux build of the VST3 at the revision above. The built-in pad is generated by code and is the same on every platform within the hash pinned in `tests/SampleDataTests.cpp`.

### The application captures

`docs/images/captures/` holds untouched screenshots of the Standalone, and `compose_ui.py` only adds numbered callouts, outlines and the keys. They were taken on Linux under a virtual display:

1. Build the Standalone at the version being documented (the v1.10.0 captures: branch `gui-crt` at `e5c3db2`, the version bump). Start `Xvfb :99 -screen 0 2400x1600x24`, with a PulseAudio null sink so the audio device opens.
2. Run the Standalone and size its window to 1924 × 1324 (`xdotool windowsize`). That makes the plugin area exactly 1920 × 1290, the 1280 × 860 design at 150%, the largest size the window allows; it is drawn at that size, not enlarged.
3. Grab the screen with `ffmpeg -f x11grab -draw_mouse 0`, with the pointer parked off the window, and crop to the plugin area (x 4–1924, y 30–1320).
4. States: `ui-oscillator-running`, from a fresh instance, has Sawtooth, LFO CUTOFF +1.50 oct and LFO RATE 2.00 Hz, and RUN on (the voices, scope, meters, LFO view, cutoff band and playhead are live); it is the frame of a 25 fps grab in which the playhead is on step 9. Then, from a second fresh instance with RUN off: `ui-shape` has Sawtooth with OVERTONE 100% and FOLD 100%; `ui-sample` then sets Source to Sample with START 45%, L.START 30%, L.END 79%; `ui-granular` then sets Source to Granular. Controls were set with the mouse (a click on a knob sets it by angle) and wheel, and each state was checked in the capture before it was used: a menu opens with the current item under the pointer, so its items are not always in the same place.

The scanlines are part of the v1.10 look and stay on in the captures (the SCANLINES button in the header turns them off; a fresh instance has them on). Each `ui-oscillator-running` frame of the 25 fps grab shows the playhead on one step for about four frames; pick one from the middle of the run on step 9.

Figure 1 has fourteen regions: 1 source and waveform menus, 2 the strip and its knob row, 3 the display, 4 the envelope with its AMP / FILTER tabs, 5 the filter, 6 OUTPUT (WIDTH, the GAIN fader and the meters), 7 MODULATION (LFO, BEND, VIBRATO), 8 the voice lights and their readout, 9 quick help, 10 the step sequencer, and since v1.11.0 11 the preset bar, 12 HOLD, 13 GLIDE and LEGATO, 14 the pattern row. A white strip left of the capture holds the markers for 1, 2, 10 and 14.

Boxes in `compose_ui.py` are in design pixels (the coordinates used by `NeonSynthEditor::layout()`) and scaled by 1.5 to the captures. If the layout changes, move the boxes there and retake the captures.

## The cover

`cover.py` draws an A4 page (210 × 297 mm, no margins) in the editor's CRT look, with the colours of `CrtTheme.h` and `Visualisers.h`: the warm grey casing and bezel, with a name plate and a green power light below; the tube glass, a radial gradient from #07170f to #010402 with a faint graticule, scanlines and a soft sheen; on it the title NEON_SYNTH and "> USER MANUAL" in VT323 with a phosphor halo, the wave stack that morphs from sine to sawtooth, the 16-step row with the playing step inverted, and the author, version and date as terminal lines with a cursor. VT323 and IBM Plex Mono are loaded from `fonts/` through matplotlib's font manager (both OFL); the halo is layered strokes, so the SVG has no filters. The version comes from `CMakeLists.txt` and is written with the date to `cover.json`, which `docs/build_manual.py --check` compares with the project version. It was drawn with `--date "18 November 2026"`; redraw it with the real release date at release.

## Limits

- Small text inside the screenshots (note names, LOAD, PAD) is the instrument's own and is smaller than 9 pt at 160 mm, even though the capture is not enlarged. The callouts, keys and captions are 9 pt or larger.
- The SVG of a capture embeds the screenshot as a raster, so those files are large; the scanlines compress poorly, so the v1.10 captures and Figures 1, 6 and 8 are larger than before.
- The cover's SVG is about 1.6 MB: the VT323 glyphs, drawn as outlines four times each for the halo, make up most of it.
- Windows and Linux renders can differ in the last decimal places, which does not change anything visible in these plots.

## Social preview

`social_preview.py` draws `docs/images/social-preview.png` (1280 × 640), the card GitHub shows
when the repository is linked on social sites and in chat apps. It shows the Oscillator capture
in the CRT frame with no version number, so it only needs redrawing when the look changes.
GitHub does not read it from the repository: upload it under **Settings → General → Social
preview** after redrawing it.
