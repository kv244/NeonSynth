# Manual figures

The figures in `docs/images/` for [the user manual](../NeonSynth-User-Manual.md), the scripts that make them, and how to remake them. Edition 1.7 of the manual, for NeonSynth v1.9.0. All four captures were retaken with v1.9.0, because every panel now has a glow outline and a header strip and the right-hand column is split into OUTPUT and the new MODULATION panel: Figure 1 now has ten regions, and `ui-oscillator-running` also has LFO CUTOFF +1.50 oct at RATE 2 Hz, so the LFO band under CUTOFF and the dot in the LFO view show. Its playhead is now on step 9, the step Figure 8's callout 5 points at (the v1.8.0 capture had it on step 4). The SOURCE panel and the sequencer did not move, so the boxes in Figures 3, 6 and 8 are unchanged. Figures 2, 4, 5 and 7 were rendered again with v1.9.0 and came out byte-identical; Figure 9 is new.

## What each figure is

| Figure | File | Kind | Made by |
|---|---|---|---|
| 1 Main window, ten numbered regions | `fig01-interface` | Actual application capture | `compose_ui.py` |
| 2 The four waveforms and their spectra | `fig02-waveforms` | Plot derived from rendered audio | `render_plots.py` |
| 3 The SOURCE panel with the SHAPE strip | `fig03-shape-strip` | Actual application capture | `compose_ui.py` |
| 4 Amplitude envelope | `fig04-envelope` | Plot derived from rendered audio | `render_plots.py` |
| 5 Low-pass filter, before and after | `fig05-filter` | Plot derived from rendered audio | `render_plots.py` |
| 6 The SOURCE panel for Sample and Granular | `fig06-sample-granular-ui` | Actual application capture | `compose_ui.py` |
| 7 Where Sample and Granular read | `fig07-sample-granular-schematic` | Explanatory schematic (over the pad's rendered waveform) | `render_plots.py` |
| 8 Step grid and gestures | `fig08-sequencer` | Actual application capture | `compose_ui.py` |
| 9 The five LFO shapes | `fig09-lfo-shapes` | Plot derived from rendered audio | `render_plots.py` |
| 10 The signal path | `fig10-signal-path` | Explanatory schematic, drawn from the source code (not from audio) | `signal_flow.py` |
| Cover | `cover` | Drawn artwork, with the version and release date | `cover.py --date "<release date>"` (redraw on every version bump; CI checks the version) |

Every figure is written as an SVG (text as outlines) and a PNG at 300 dpi. All are drawn 160 mm wide, the print text width, with 9 pt labels and 10 pt panel titles in DejaVu Sans. Lines are told apart by style and label as well as by colour, so they read in grayscale. The captions in the manual carry the same kind label.

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

1. Build the Standalone at the revision above. Start `Xvfb :99 -screen 0 2400x1600x24`, with a PulseAudio null sink so the audio device opens.
2. Run the Standalone and size its window to 1924 × 1324 (`xdotool windowsize`). That makes the plugin area exactly 1920 × 1290, the 1280 × 860 design at 150%, the largest size the window allows; it is drawn at that size, not enlarged.
3. Grab the screen with `ffmpeg -f x11grab -draw_mouse 0`, with the pointer parked off the window, and crop to the plugin area (x 4–1924, y 30–1320).
4. States: `ui-oscillator-running`, from a fresh instance, has Sawtooth, LFO CUTOFF +1.50 oct and LFO RATE 2.00 Hz, and RUN on (the voices, scope, meters, LFO view, cutoff band and playhead are live); it is the frame of a 25 fps grab in which the playhead is on step 9. Then, from a second fresh instance with RUN off: `ui-shape` has Sawtooth with OVERTONE 100% and FOLD 100%; `ui-sample` then sets Source to Sample with START 45%, L.START 30%, L.END 79%; `ui-granular` then sets Source to Granular. Controls were set with the mouse (a click on a knob sets it by angle) and wheel, and each state was checked in the capture before it was used: a menu opens with the current item under the pointer, so its items are not always in the same place.

Figure 1 has ten regions: 1 source and waveform menus, 2 the strip and its knob row, 3 the display, 4 the envelope with its AMP / FILTER tabs, 5 the filter, 6 OUTPUT (WIDTH, the GAIN fader and the meters), 7 MODULATION (LFO, BEND, VIBRATO), 8 the voice lights and their readout, 9 quick help, 10 the step sequencer. A white strip left of the capture holds the markers for 1, 2 and 10.

Boxes in `compose_ui.py` are in design pixels (the coordinates used by `NeonSynthEditor::layout()`) and scaled by 1.5 to the captures. If the layout changes, move the boxes there and retake the captures.

## Limits

- Small text inside the screenshots (note names, LOAD, PAD) is the instrument's own and is smaller than 9 pt at 160 mm, even though the capture is not enlarged. The callouts, keys and captions are 9 pt or larger.
- The SVG of a capture embeds the screenshot as a raster, so those files are large.
- Windows and Linux renders can differ in the last decimal places, which does not change anything visible in these plots.
