# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
"""Builds the manual's annotated screenshots from real application captures.

    python docs/figures/compose_ui.py [--captures docs/images/captures] [--out docs/images]

The captures are untouched screenshots of the running Standalone (see README.md
for how they were taken). Nothing is drawn into them except numbered callouts
and thin outlines, which are vector shapes laid over the picture; the keys that
explain the numbers sit below the screenshot, never on it.

Writes (SVG + 300 dpi PNG):
    fig01-interface            the whole window, fourteen numbered regions
    fig03-shape-strip          the SOURCE panel with the SHAPE strip
    fig06-sample-granular-ui   the Sample and Granular control strips
    fig08-sequencer            the step grid with a gesture key
"""
import argparse
import pathlib
import textwrap

import numpy as np
from PIL import Image

import figstyle as fs
plt = fs.plt


def put_image(fig, img, top_mm, width_mm=160.0, margin_px=0, bottom_px=0, left_px=0):
    """Places img at 160 mm wide (with optional white margins above, below and to the left); returns its axes."""
    h_mm = width_mm * (img.shape[0] + margin_px + bottom_px) / (img.shape[1] + left_px)
    H = fig.get_figheight() / fs.MM
    ax = fig.add_axes([0, 1 - (top_mm + h_mm) / H, width_mm / fs.WIDTH_MM, h_mm / H])
    ax.imshow(img, interpolation="none", aspect="auto")
    ax.set_xlim(-left_px, img.shape[1])
    ax.set_ylim(img.shape[0] + bottom_px, -margin_px)
    ax.axis("off")
    return ax, h_mm


def outline(ax, box, color="white"):
    x0, y0, x1, y1 = box
    ax.plot([x0, x1, x1, x0, x0], [y0, y0, y1, y1, y0], color="black", lw=2.0, solid_joinstyle="miter")
    ax.plot([x0, x1, x1, x0, x0], [y0, y0, y1, y1, y0], color=color, lw=0.9, ls=(0, (4, 2)),
            solid_joinstyle="miter")


def marker(ax, x, y, label):
    ax.scatter([x], [y], s=210, color="white", edgecolor="black",
               linewidth=1.2, zorder=5)
    ax.text(x, y, str(label), ha="center", va="center", fontsize=9 if len(str(label)) == 1 else 8,
            fontweight="bold", color="black", zorder=6)


def key_block(fig, items, top_mm, cols=2, wrap=40, line_mm=3.9, gap_mm=1.6):
    """Draws a numbered key below the picture, filled column by column."""
    H = fig.get_figheight() / fs.MM
    per = -(-len(items) // cols)
    col_w = 160 / cols
    for c in range(cols):
        y = top_mm
        for n, text in items[c * per:(c + 1) * per]:
            lines = textwrap.wrap(text, wrap) or [""]
            fig.text((c * col_w + 1.0) / 160, 1 - y / H, str(n), ha="left", va="top", fontsize=9,
                     fontweight="bold")
            fig.text((c * col_w + 7.0) / 160, 1 - y / H, "\n".join(lines), ha="left", va="top", fontsize=9,
                     linespacing=1.2)
            y += len(lines) * line_mm + gap_mm
    return y


def key_height(items, cols=2, wrap=40, line_mm=3.9, gap_mm=1.6):
    per = -(-len(items) // cols)
    return max(sum(len(textwrap.wrap(t, wrap)) * line_mm + gap_mm for _, t in items[c * per:(c + 1) * per])
               for c in range(cols))


# The captures are the window at 150%: 1920 x 1290 pixels for the 1280 x 860 design grid.
# Boxes below are in design pixels, as in NeonSynthEditor::layout(), and scaled here.
SCALE = 1.5


def px(box):
    return tuple(v * SCALE for v in box)


# ---------------------------------------------------------------- figure 1
FIG1_REGIONS = [                     # number, box (design px), marker (design px): on a corner or edge
    (1, (28, 114, 404, 154), (6, 134)),        # with nothing under it, never on a label
    (2, (28, 186, 404, 610), (6, 400)),        # from v1.12.0 with the CUSTOM / NAIVE / DRAW row above the strip
    (3, (440, 88, 940, 336), (946, 88)),
    (4, (440, 358, 748, 612), (754, 358)),      # takes in the AMP / FILTER tabs (v1.8.0)
    (5, (782, 362, 940, 626), (946, 362)),      # the whole filter panel, with the LFO band under CUTOFF
    (6, (976, 90, 1252, 344), (1256, 90)),      # OUTPUT, the top of the right-hand column since v1.9.0
    (7, (976, 368, 1252, 622), (1256, 368)),    # MODULATION, the bottom of it (v1.9.0)
    (8, (758, 26, 1172, 58), (766, 42)),        # the "16 VOICES" readout and the lights; the marker clears
                                                # the SCANLINES button (v1.10.0), which ends at x 748
    (9, (1206, 22, 1256, 62), (1191, 42)),
    (10, (28, 648, 1252, 838), (6, 760)),
    (11, (248, 26, 556, 58), (330, 70)),       # the preset bar (v1.11.0), marker just under the header line
    (12, (564, 26, 636, 58), (600, 70)),       # HOLD, marker just under the header line
    (13, (1088, 540, 1250, 630), (1256, 585)),  # GLIDE and LEGATO, MODULATION's second row
    (14, (150, 688, 754, 716), (6, 702)),      # the pattern row, marker in the white strip
]
FIG1_LEFT = 30                       # white strip left of the capture, capture px: the left markers sit in it
FIG1_KEY = [
    (1, "Source and waveform. The left menu picks Oscillator, Sample, Granular or Random; the right menu picks the oscillator waveform."),
    (2, "The strip and its knob row: the SHAPE strip for Oscillator and Random (Figure 3), with CUSTOM, NAIVE and DRAW above it (v1.12.0); the sample for Sample and Granular (Figure 6)."),
    (3, "Display: SCOPE or SPECTRUM view of the mix, or the HARMONICS view (Figure 11)."),
    (4, "Envelope. AMP or FILTER: drag the four points, or turn ATTACK, DECAY, SUSTAIN and RELEASE."),
    (5, "Filter. ON, CUTOFF, the mode, and RES, DRIVE, ENV and VEL. The green band under CUTOFF is the LFO's sweep."),
    (6, "Output: WIDTH, the GAIN fader and the left and right level meters."),
    (7, "Modulation: the LFO (shape, SYNC, RATE, and its PITCH, CUTOFF and AMP depths), BEND and VIBRATO, with bars for the pitch bend and mod wheel (Figure 9)."),
    (8, "Voice lights: one for each of the 16 voices, lit while it sounds. Click one to set how many voices can play (1 = MONO)."),
    (9, "Quick help. Click it, or anywhere on the help page, or press Esc to close."),
    (10, "Step sequencer: RUN, SYNC, rate, keyboard mode, TEMPO, SWING and GATE above; the 16 steps below (Figure 8)."),
    (11, "Preset bar: the current preset and its category, < and > to step through the presets, the name for the menu, SAVE."),
    (12, "HOLD: keys keep sounding after you let go, until HOLD is switched off."),
    (13, "GLIDE, the time to slide to a new note, and LEGATO (with one voice: overlapping keys glide without restarting)."),
    (14, "The pattern row: LENGTH, KEY and SCALE, then TOOLS, ENTER and UNDO."),
]


def fig01(cap, out):
    img = np.asarray(Image.open(cap / "ui-oscillator-running.png").convert("RGB"))
    key_mm = key_height(FIG1_KEY)
    h_img = 160 * img.shape[0] / (img.shape[1] + FIG1_LEFT)
    fig = fs.figure(h_img + key_mm + 7)
    ax, h_mm = put_image(fig, img, 0, left_px=FIG1_LEFT)
    for n, box, _ in FIG1_REGIONS:
        outline(ax, px(box))
    for n, _, (mx, my) in FIG1_REGIONS:
        marker(ax, mx * SCALE, my * SCALE, n)
    key_block(fig, FIG1_KEY, h_mm + 3.5)
    return fs.save(fig, out, "fig01-interface")


# ---------------------------------------------------------------- figure 3
SOURCE_PANEL = (16, 82, 416, 630)    # the SOURCE panel, design px


def fig03(cap, out):
    img = np.asarray(Image.open(cap / "ui-shape.png").convert("RGB"))
    x0, y0, x1, y1 = px(SOURCE_PANEL)
    crop = img[int(y0):int(y1), int(x0):int(x1)]
    width = 72.0                     # mm: a quarter of a page tall
    h = width * crop.shape[0] / crop.shape[1]
    fig = fs.figure(h)
    ax = fig.add_axes([(160 - width) / 2 / 160, 0, width / 160, 1])
    ax.imshow(crop, interpolation="none", aspect="auto")
    ax.axis("off")
    return fs.save(fig, out, "fig03-shape-strip")


# ------------------------------------------------------------ figures 11 and 13 (v1.12.0)
DISPLAY_PANEL = (428, 82, 952, 344)  # the display panel (SCOPE / SPECTRUM / HARMONICS), design px
STRIP_FOOT = 516                     # just below the SHAPE strip (218 + 290), design px


def fig11(cap, out):
    """The HARMONICS view: CUSTOM on, the SAW set, HARMONICS 9."""
    img = np.asarray(Image.open(cap / "ui-harmonics.png").convert("RGB"))
    x0, y0, x1, y1 = px(DISPLAY_PANEL)
    crop = img[int(y0):int(y1), int(x0):int(x1)]
    width = 120.0
    fig = fs.figure(width * crop.shape[0] / crop.shape[1])
    ax = fig.add_axes([(160 - width) / 2 / 160, 0, width / 160, 1])
    ax.imshow(crop, interpolation="none", aspect="auto")
    ax.axis("off")
    return fs.save(fig, out, "fig11-harmonics")


def fig13(cap, out):
    """DRAW: the SOURCE panel down to the strip's foot, beside the HARMONICS view: a drawn cycle
    (faint) and the wave its first 32 harmonics play, with those harmonics as bars."""
    img = np.asarray(Image.open(cap / "ui-draw.png").convert("RGB"))
    sx0, sy0, sx1, _ = px(SOURCE_PANEL)
    left = img[int(sy0):int(STRIP_FOOT * SCALE), int(sx0):int(sx1)]
    dx0, dy0, dx1, dy1 = px(DISPLAY_PANEL)
    right = img[int(dy0):int(dy1), int(dx0):int(dx1)]
    gap = 4.0                                            # mm between the two panels
    scale_mm = (160.0 - gap) / (left.shape[1] + right.shape[1])   # one scale for both: mm per capture px
    h = left.shape[0] * scale_mm
    fig = fs.figure(h)
    wl, wr = left.shape[1] * scale_mm, right.shape[1] * scale_mm
    ax = fig.add_axes([0, 0, wl / 160, 1])
    ax.imshow(left, interpolation="none", aspect="auto")
    ax.axis("off")
    hr = right.shape[0] * scale_mm
    ax = fig.add_axes([(wl + gap) / 160, 1 - hr / h, wr / 160, hr / h])
    ax.imshow(right, interpolation="none", aspect="auto")
    ax.axis("off")
    return fs.save(fig, out, "fig13-draw")


# ------------------------------------------------------------- figure 6
KNOB_TOP, KNOB_BOTTOM = 512, 606     # the knob row, design px
BELOW = 40                           # white strip under each crop for the knob numbers, crop px


def knob_regions(first, centres, half):
    return [(first + i, (c - half, KNOB_TOP, c + half, KNOB_BOTTOM), (c, None)) for i, c in enumerate(centres)]


SAMPLE_REGIONS = [
    (1, (28, 156, 404, 192), (14, 174)),        # LOAD, PAD, loop menu
    (2, (28, 214, 404, 512), (14, 360)),        # waveform view
] + knob_regions(3, [68, 142, 216, 289, 362], 34)
SAMPLE_KEY = [
    (1, "LOAD and PAD fill the sample; the menu beside them chooses the loop mode (One-shot, Loop, Loop until release)."),
    (2, "The sample as a waveform. The cyan line is START; the amber brackets mark L.START and L.END."),
    (3, "ROOT: the key that plays the sample at its original speed."),
    (4, "START: where each note begins reading."),
    (5, "L.START: where the loop begins."),
    (6, "L.END: where the loop ends."),
    (7, "XFADE: the crossfade at the loop end."),
]
GRAN_REGIONS = [
    (1, (28, 156, 404, 192), (14, 174)),
    (2, (28, 214, 404, 512), (14, 360)),
] + knob_regions(3, [58, 111, 163, 216, 268, 321, 373], 25)
GRAN_KEY = [
    (1, "LOAD and PAD fill the sample; the loop menu is dimmed in Granular."),
    (2, "The sample as a waveform. The magenta line is POS; the shaded band is the reach of SPRAY."),
    (3, "ROOT: the sample's pitch reference."),
    (4, "POS: where grains are taken from."),
    (5, "SCAN: how fast POS moves through the file; frozen = 0."),
    (6, "SIZE: grain length."),
    (7, "DENS: grains started each second."),
    (8, "SPRAY: how widely grains scatter around POS."),
    (9, "JITTER: random pitch variation per grain."),
]
PANEL_MM = 77.0                      # each panel's width on the page


STRIP_CROP = (0, 82, 416, 630)       # the SOURCE panel plus the window margin to its left, for markers


def panel(fig, img, left_mm, top_mm, regions):
    x0, y0, x1, y1 = px(STRIP_CROP)
    crop = img[int(y0):int(y1), int(x0):int(x1)]
    H = fig.get_figheight() / fs.MM
    h_mm = PANEL_MM * (crop.shape[0] + BELOW) / crop.shape[1]
    ax = fig.add_axes([left_mm / 160, 1 - (top_mm + h_mm) / H, PANEL_MM / 160, h_mm / H])
    ax.imshow(crop, interpolation="none", aspect="auto")
    ax.set_xlim(0, crop.shape[1])
    ax.set_ylim(crop.shape[0] + BELOW, 0)
    ax.axis("off")
    for n, box, _ in regions:
        b = px(box)
        outline(ax, (b[0] - x0, b[1] - y0, b[2] - x0, b[3] - y0))
    for n, _, (mx, my) in regions:
        X = mx * SCALE - x0
        Y = crop.shape[0] + BELOW * 0.5 if my is None else my * SCALE - y0
        marker(ax, X, Y, n)
    return h_mm


def fig06a(cap, out):
    sample = np.asarray(Image.open(cap / "ui-sample.png").convert("RGB"))
    gran = np.asarray(Image.open(cap / "ui-granular.png").convert("RGB"))
    x0, y0, x1, y1 = px(STRIP_CROP)
    panel_mm = PANEL_MM * ((y1 - y0) + BELOW) / (x1 - x0)
    keys = dict(cols=1, wrap=38)
    key_mm = max(key_height(SAMPLE_KEY, **keys), key_height(GRAN_KEY, **keys))
    title_mm = 7
    total = title_mm + panel_mm + 3 + key_mm + 2
    fig = fs.figure(total)
    right = 160 - PANEL_MM
    for left, title, img, regions, key in ((0, "a  Source: Sample", sample, SAMPLE_REGIONS, SAMPLE_KEY),
                                           (right, "b  Source: Granular", gran, GRAN_REGIONS, GRAN_KEY)):
        fig.text(left / 160, 1 - 4.5 / total, title, fontsize=10, fontweight="bold", va="center")
        h = panel(fig, img, left, title_mm, regions)
        key_column(fig, key, left, title_mm + h + 3)
    return fs.save(fig, out, "fig06-sample-granular-ui")


def key_column(fig, items, left_mm, top_mm, wrap=38, line_mm=3.9, gap_mm=1.6):
    """A one-column key starting at left_mm."""
    H = fig.get_figheight() / fs.MM
    y = top_mm
    for n, text in items:
        lines = textwrap.wrap(text, wrap) or [""]
        fig.text((left_mm + 1.0) / 160, 1 - y / H, str(n), ha="left", va="top", fontsize=9, fontweight="bold")
        fig.text((left_mm + 6.0) / 160, 1 - y / H, "\n".join(lines), ha="left", va="top", fontsize=9, linespacing=1.2)
        y += len(lines) * line_mm + gap_mm
    return y


# ---------------------------------------------------------------- figure 8
SEQ_PANEL = (16, 642, 1264, 846)     # design px
SEQ_MARGIN = 95                      # white strip above the capture, for the callouts (capture px)
STEP_X, STEP_PITCH = 32, 76          # first step's left edge and the step spacing, design px


def step_x(i, frac=0.5):
    return STEP_X + i * STEP_PITCH + frac * 74


SEQ_MARKS = [                        # number, target (x, y) in design px
    (1, (step_x(0), 720)),           # enabled step
    (2, (step_x(1), 720)),           # disabled step
    (3, (step_x(6, 0.86), 763)),     # note name, beside the text (VT323 since v1.10.0, wider)
    (4, (step_x(4, 0.12), 812)),     # velocity bar, its first block; the line runs beside the note name
    (5, (step_x(8), 726)),           # playhead (the inverted step since v1.10.0), its top edge
    (6, (step_x(2, 0.27), 827)),     # a TIE that is set (v1.11.0), and the line into the gap
    (7, (step_x(7, 0.72), 827)),     # a SLD that is set
    (8, (step_x(13), 760)),          # a step beyond LENGTH, faded
    (9, (430, 702)),                 # the pattern row (at SCALE)
]
SEQ_KEY = [
    (1, "Enabled step: an amber outline and a glowing note name. Every fourth step has a mark along its top edge, which marks the beat."),
    (2, "Disabled step: dark, with a dim note name. Its note and velocity are kept."),
    (3, "Note name: the pitch the step plays."),
    (4, "Velocity bar: a row of blocks; the more are lit, the higher the step's velocity."),
    (5, "Playhead: the step now playing is drawn inverted, an amber block with dark text."),
    (6, "TIE set: the note is held into the next step, which does not play its own; a line joins the two cells."),
    (7, "SLD set: the note glides into the next step's note, at GLIDE time."),
    (8, "A step beyond LENGTH (here 12 steps): drawn faded, kept, not played."),
    (9, "The pattern row: LENGTH, KEY, SCALE, TOOLS, ENTER and UNDO."),
]
SEQ_GESTURES = [
    ("Click", "turn the step on or off"),
    ("Drag up or down", "change the note: one semitone (or scale note) for every 6 pixels"),
    ("Drag the velocity bar", "change the velocity (the accent), up or down"),
    ("Shift-drag or right-drag", "change the velocity anywhere on the step"),
    ("Mouse wheel", "note up or down by one semitone (or scale note)"),
    ("Shift + mouse wheel", "velocity up or down by 4"),
    ("Click TIE or SLD", "tie the step to the next, or slide into it"),
]


def fig08(cap, out):
    full = np.asarray(Image.open(cap / "ui-sequencer.png").convert("RGB"))
    x0, y0, x1, y1 = (int(v) for v in px(SEQ_PANEL))
    crop = full[y0:y1, x0:x1]
    h_img = 160 * (crop.shape[0] + SEQ_MARGIN) / crop.shape[1]
    key_mm = key_height(SEQ_KEY, cols=1, wrap=80)
    H = h_img + 3 + key_mm + 4 + len(SEQ_GESTURES) * 4.4 + 8
    fig = fs.figure(H)
    ax, h = put_image(fig, crop, 0, margin_px=SEQ_MARGIN)
    for n, (tx, ty) in SEQ_MARKS:
        tX, tY = tx * SCALE - x0, ty * SCALE - y0
        X, Y = tX, -SEQ_MARGIN * 0.5
        ax.plot([X, tX], [Y, tY], color="white", lw=3.0, zorder=2)
        ax.plot([X, tX], [Y, tY], color="black", lw=1.0, zorder=3)
        ax.scatter([tX], [tY], s=18, color="black", edgecolor="white", linewidth=1.0, zorder=4)
        marker(ax, X, Y, n)
    key_block(fig, SEQ_KEY, h + 3, cols=1, wrap=80)
    y = h + 3 + key_mm + 4
    fig.text(0.0, 1 - y / H, "Mouse and wheel gestures on a step", fontsize=10, fontweight="bold", va="top")
    y += 6
    for g, t in SEQ_GESTURES:
        fig.text(1.0 / 160, 1 - y / H, g, fontsize=9, fontweight="bold", va="top")
        fig.text(52.0 / 160, 1 - y / H, t, fontsize=9, va="top")
        y += 4.4
    return fs.save(fig, out, "fig08-sequencer")


def main():
    here = pathlib.Path(__file__).resolve().parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--captures", type=pathlib.Path, default=here.parent / "images" / "captures")
    ap.add_argument("--out", type=pathlib.Path, default=here.parent / "images")
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    for fn in (fig01, fig03, fig06a, fig08, fig11, fig13):
        print(fn.__name__, [p.name for p in fn(a.captures, a.out)])


if __name__ == "__main__":
    main()
