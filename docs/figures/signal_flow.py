# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
# Licensed under the GNU Affero General Public License v3; see LICENSE.
"""Figure 10: the signal path (explanatory schematic, v1.12.0).

Drawn from the source, not from audio: every box names a stage in the order
NeonSynthProcessor::processBlock() and NeonSynthVoice::renderSamples() run
them. Run from the repository root:

    python3 docs/figures/signal_flow.py [out_dir]     (default docs/images)

Writes fig10-signal-path.svg and .png in the house style (figstyle.py).
"""
import sys
from pathlib import Path

import matplotlib.patches as mp

sys.path.insert(0, str(Path(__file__).resolve().parent))
import figstyle as fs  # noqa: E402

W, H = 160.0, 112.0          # mm; the axes use millimetres as data units
SMALL, BODY = 6.0, 7.0       # pt


def box(ax, x, y, w, h, title, lines=(), edge=fs.INK, fill="white", bold=True, dashed=False):
    ax.add_patch(mp.FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0,rounding_size=1.2",
                                   linewidth=0.8, edgecolor=edge, facecolor=fill,
                                   linestyle=(0, (3, 2)) if dashed else "-"))
    n = len(lines)
    top = y + h / 2 + (n * 2.6) / 2
    ax.text(x + w / 2, top, title, ha="center", va="center", fontsize=BODY,
            fontweight="bold" if bold else "normal", color=fs.INK)
    for i, line in enumerate(lines):
        ax.text(x + w / 2, top - 2.9 * (i + 1), line, ha="center", va="center",
                fontsize=SMALL, color=fs.INK)
    return (x, y, w, h)


def arrow(ax, pts, color=fs.INK, dashed=False, lw=0.8):
    """A polyline through pts with an arrowhead at the end."""
    xs, ys = zip(*pts)
    ls = (0, (2.5, 1.5)) if dashed else "-"
    if len(pts) > 2:
        ax.plot(xs[:-1], ys[:-1], color=color, lw=lw, linestyle=ls, solid_capstyle="butt")
    ax.annotate("", xy=pts[-1], xytext=pts[-2],
                arrowprops=dict(arrowstyle="-|>,head_length=0.55,head_width=0.3", mutation_scale=8, color=color,
                                lw=lw, linestyle="-", shrinkA=0, shrinkB=0))
    if dashed and len(pts) == 2:
        ax.plot(xs, ys, color=color, lw=lw, linestyle=ls)


def label(ax, x, y, text, color=fs.GREY, ha="left", size=SMALL, style="normal", weight="normal"):
    ax.text(x, y, text, ha=ha, va="center", fontsize=size, color=color, style=style, fontweight=weight)


def draw():
    fig = fs.figure(H)
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_xlim(0, W)
    ax.set_ylim(0, H)
    ax.axis("off")
    MOD = fs.ACCENT

    # --- Top: notes in, and the shared modulation ---------------------------------------------
    box(ax, 2, 99, 26, 8, "Keyboard MIDI")
    box(ax, 2, 88, 26, 8, "Step sequencer")
    box(ax, 33, 92, 27, 10, "MIDI routing", ["keyboard to ch. 1,", "sequencer on ch. 16"])
    box(ax, 64, 92, 30, 10, "Voice allocation", ["up to 16 voices", "(VOICES limit, 1 = mono)"])
    arrow(ax, [(28, 103), (30.5, 103), (30.5, 98.5), (33, 98.5)])
    arrow(ax, [(28, 92), (30.5, 92), (30.5, 95.5), (33, 95.5)])
    arrow(ax, [(60, 97), (64, 97)])

    # v1.12.0: CUSTOM's 32 wavetables, built off the audio thread from the HARMONICS view or a
    # drawn cycle, handed to every voice's oscillator once per block (lock-free triple buffer).
    box(ax, 34, 83.5, 28, 7, "CUSTOM tables", ["32 harmonics, lock-free"])
    arrow(ax, [(37, 83.5), (37, 71.5), (34, 71.5)], color=fs.GREY)

    box(ax, 101, 88, 57, 19, "Modulation, once per block", ["LFO: 5 shapes, free or synced", "pitch bend, mod-wheel vibrato (MIDI)",
                                                            "→ pitch ratio, LFO cutoff, tremolo"],
        edge=MOD)

    # --- Middle: one voice ---------------------------------------------------------------------
    ax.add_patch(mp.FancyBboxPatch((2, 30), 156, 52, boxstyle="round,pad=0,rounding_size=2",
                                   linewidth=0.8, edgecolor=fs.GREY, facecolor="none",
                                   linestyle=(0, (4, 2))))
    label(ax, 4, 79.3, "Each voice (one note)", color=fs.INK, weight="bold", size=BODY)
    arrow(ax, [(79, 92), (79, 82)])

    # sources
    box(ax, 6, 64, 28, 9, "Oscillator", ["BLEP or CUSTOM tables;", "NAIVE: no BLEP"])
    box(ax, 6, 53, 28, 9, "Random", ["16 drifting steps"])
    box(ax, 6, 42, 28, 9, "Sample", ["3 rate-reduced copies"])
    box(ax, 6, 31.5, 28, 9, "Granular", ["up to 32 grains"])
    label(ax, 40, 77.8, "pitch × pitch ratio (all four sources)", color=MOD)
    arrow(ax, [(110, 88), (110, 76), (20, 76), (20, 73)], color=MOD, dashed=True)

    # sculpting: Oscillator and Random only
    box(ax, 44, 60, 15, 8, "CHOP", ["steps"])
    box(ax, 63, 60, 17, 8, "Noise", ["equal power"])
    box(ax, 84, 60, 19, 8, "Fold", ["2× oversampled"])
    arrow(ax, [(34, 68.5), (39, 68.5), (39, 64), (44, 64)])
    ax.plot([34, 39, 39], [57.5, 57.5, 64], color=fs.INK, lw=0.8)   # joins the oscillator's line
    arrow(ax, [(59, 64), (63, 64)])
    arrow(ax, [(80, 64), (84, 64)])
    label(ax, 44, 70.5, "Oscillator and Random only", style="italic")

    # the per-voice chain
    box(ax, 44, 40, 24, 11, "Velocity ×", ["amp envelope"])
    box(ax, 72, 40, 18, 11, "Drive", ["ADAA"])
    box(ax, 94, 36, 35, 19, "Filter (SVF)", ["LP / HP / BP / Notch", "resonance limiter follows", "the amp envelope",
                                              "cutoff + env + vel + LFO"], fill="white")
    box(ax, 132, 40, 24, 11, "Pitch-based", ["pan (WIDTH)"])
    arrow(ax, [(93.5, 60), (93.5, 56), (56, 56), (56, 51)])
    arrow(ax, [(34, 46.5), (44, 46.5)])
    arrow(ax, [(34, 36), (38, 36), (38, 43), (44, 43)])
    arrow(ax, [(68, 45.5), (72, 45.5)])
    arrow(ax, [(90, 45.5), (94, 45.5)])
    arrow(ax, [(129, 45.5), (132, 45.5)])
    # Filter ON crossfades the dry signal with drive + filter together, so they form one stage.
    ax.add_patch(mp.FancyBboxPatch((70, 33.5), 61, 23.5, boxstyle="round,pad=0,rounding_size=1.5",
                                   linewidth=0.7, edgecolor=fs.GREY, facecolor="none", linestyle=(0, (1.5, 1.5))))
    label(ax, 100.5, 31.8, "filter ON / bypass: drive and filter together", ha="center")
    arrow(ax, [(140, 88), (140, 58), (122, 58), (122, 55)], color=MOD, dashed=True)
    label(ax, 141, 62, "LFO cutoff", color=MOD)

    # --- Bottom: the mix ---------------------------------------------------------------------
    box(ax, 4, 10, 22, 11, "Voice sum", ["all voices"])
    box(ax, 31, 10, 24, 11, "Tremolo", ["LFO AMP: dips only"], edge=MOD)
    box(ax, 60, 10, 22, 11, "Output gain", ["GAIN fader"])
    box(ax, 88, 10, 30, 11, "Soft clipper", ["−6 dBFS knee,", "−1 dBFS ceiling"])
    box(ax, 128, 14, 28, 9, "Audio output", ["mono or stereo"], fill=fs.LIGHT)
    box(ax, 128, 2, 29, 9, "Displays", ["scope, spectrum, meters"])
    arrow(ax, [(144, 40), (144, 28), (15, 28), (15, 21)])
    arrow(ax, [(26, 15.5), (31, 15.5)])
    arrow(ax, [(55, 15.5), (60, 15.5)])
    arrow(ax, [(82, 15.5), (88, 15.5)])
    arrow(ax, [(118, 18.5), (128, 18.5)])
    arrow(ax, [(118, 12.5), (123, 12.5), (123, 6.5), (128, 6.5)])
    label(ax, 121.5, 4.0, "lock-free FIFO", ha="right")
    arrow(ax, [(158, 95), (159.3, 95), (159.3, 24.5), (43, 24.5), (43, 21)], color=MOD, dashed=True)
    label(ax, 97, 22.8, "tremolo", color=MOD)
    return fig


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "images"
    paths = fs.save(draw(), out, "fig10-signal-path")
    print("wrote", paths)


if __name__ == "__main__":
    main()
