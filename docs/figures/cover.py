# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
"""Draws the manual's cover: docs/images/cover.svg and cover.png (A4 at 300 dpi).

    python docs/figures/cover.py --date "10 November 2026" [--out docs/images]

The cover carries the version (read from CMakeLists.txt) and the release date
(--date; without it, the date already recorded for this version is kept). Both
are written to cover.json, and docs/build_manual.py --check fails when that
version is not the one in CMakeLists.txt, so REDRAW THE COVER ON EVERY VERSION
BUMP, with the new release date.

The picture is the instrument's idea in one image, shown the way the v1.10
editor looks: a CRT terminal. A warm grey casing and bezel frame a dark green
tube; on its glass, a stack of single-cycle waves morphs from sine through
triangle and square to sawtooth, with the 16-step row underneath, all in
phosphor colours with faint scanlines. The titles are set in VT323 and the
small lines in IBM Plex Mono, the editor's two fonts (both OFL, in fonts/).
Everything is drawn by this script (no stock art); the glow is layered strokes
and the glass a raster gradient, so the SVG has no filters.
"""
import argparse
import json
import pathlib
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib import font_manager  # noqa: E402
from matplotlib.collections import LineCollection  # noqa: E402
from matplotlib.colors import to_rgb  # noqa: E402
from matplotlib.patches import FancyBboxPatch, Rectangle  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
FONTS = HERE.parent.parent / "fonts"

W_MM, H_MM = 210.0, 297.0
# The editor's palette (CrtTheme.h and Visualisers.h)
CYAN, MAGENTA, AMBER, GREEN = "#19e6ff", "#ff4fd8", "#ffb319", "#5cff6a"
PHOSPHOR, PHOSPHOR_DIM, PHOSPHOR_GLOW = "#c8ffe6", "#6fae95", "#3dffb0"
SCREEN_CENTRE, SCREEN_EDGE = "#07170f", "#010402"
BEZEL, BEZEL_SHADOW, CASING = "#35302a", "#0f0d0b", "#2a2723"
SCANLINE_STRENGTH = 0.12          # as the editor's overlay
SCANLINE_PITCH_MM = 0.75

plt.rcParams.update({"font.family": "DejaVu Sans", "svg.fonttype": "path", "svg.hashsalt": "neonsynth-cover"})


def font(name):
    """The bundled font file, so the cover looks the same on every machine."""
    path = FONTS / name
    font_manager.fontManager.addfont(str(path))
    return font_manager.FontProperties(fname=str(path))


VT323 = font("VT323-Regular.ttf")
PLEX = font("IBMPlexMono-Regular.ttf")
PLEX_SEMI = font("IBMPlexMono-SemiBold.ttf")


def glow(ax, x, y, color, lw=1.1, alpha=1.0, zorder=3, clip=None):
    for width, a in ((9.0, 0.035), (5.5, 0.07), (3.0, 0.16), (1.0, 1.0)):
        line, = ax.plot(x, y, color=color, lw=lw * width, alpha=a * alpha, solid_capstyle="round",
                        solid_joinstyle="round", zorder=zorder)
        if clip is not None:
            line.set_clip_path(clip)


def glow_text(ax, x, y, text, color, size, fp, halo=PHOSPHOR_GLOW, zorder=50, **kw):
    """Phosphor text: a soft halo of the text drawn wider and fainter behind it (no blur filter)."""
    import matplotlib.patheffects as pe
    t = ax.text(x, y, text, color=color, fontsize=size, fontproperties=fp, zorder=zorder, **kw)
    w = min(size, 30) * 0.5                  # a big title would get a blocky halo from wider strokes
    t.set_path_effects([pe.Stroke(linewidth=w * 0.55, foreground=halo, alpha=0.06),
                        pe.Stroke(linewidth=w * 0.28, foreground=halo, alpha=0.12),
                        pe.Stroke(linewidth=w * 0.10, foreground=halo, alpha=0.25),
                        pe.Normal()])
    return t


def wave(kind, ph):
    """One of the four shapes at phase ph (cycles), range -1..1."""
    t = ph % 1.0
    if kind == 0:
        return np.sin(2 * np.pi * t)
    if kind == 1:
        return 1 - 4 * np.abs(((t + 0.25) % 1.0) - 0.5)
    if kind == 2:
        return np.where(t < 0.5, 1.0, -1.0)
    return 2 * ((t + 0.5) % 1.0) - 1


def mix(c0, c1, u):
    a, b = np.array(to_rgb(c0)), np.array(to_rgb(c1))
    return tuple(a + (b - a) * u)


def spaced(text):
    return " ".join(text)          # wide, technical letter-spacing


def draw(out_dir, version, date):
    fig = plt.figure(figsize=(W_MM / 25.4, H_MM / 25.4))
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_xlim(0, W_MM)
    ax.set_ylim(0, H_MM)
    ax.axis("off")
    cx = W_MM / 2

    # the casing fills the page; the bezel sits on it with a dark rim, the glass inside that
    ax.add_patch(Rectangle((0, 0), W_MM, H_MM, facecolor=CASING, edgecolor="none", zorder=0))
    bz = (9.0, 24.0, W_MM - 9.0, H_MM - 9.0)                         # x0, y0, x1, y1 of the bezel
    ax.add_patch(FancyBboxPatch((bz[0], bz[1]), bz[2] - bz[0], bz[3] - bz[1],
                                boxstyle="round,pad=0,rounding_size=9", facecolor=BEZEL,
                                edgecolor=BEZEL_SHADOW, linewidth=3.0, zorder=1))
    # a lighter top edge and a darker bottom edge give the bezel some depth
    ax.plot([bz[0] + 9, bz[2] - 9], [bz[3] - 1.2, bz[3] - 1.2], color="#4a443c", lw=1.2, zorder=1.5)
    ax.plot([bz[0] + 9, bz[2] - 9], [bz[1] + 1.2, bz[1] + 1.2], color="#221f1b", lw=1.6, zorder=1.5)

    gl = (bz[0] + 11.0, bz[1] + 11.0, bz[2] - 11.0, bz[3] - 11.0)    # the glass
    gw, gh = gl[2] - gl[0], gl[3] - gl[1]
    glass = FancyBboxPatch((gl[0], gl[1]), gw, gh, boxstyle="round,pad=0,rounding_size=7",
                           facecolor="none", edgecolor=BEZEL_SHADOW, linewidth=2.2, zorder=30)
    ax.add_patch(glass)
    clip = FancyBboxPatch((gl[0], gl[1]), gw, gh, boxstyle="round,pad=0,rounding_size=7",
                          facecolor="none", edgecolor="none", transform=ax.transData)

    # the tube: a radial gradient, brightest in the middle, almost black at the corners
    ny, nx = 280, 200
    yy, xx = np.mgrid[0:ny, 0:nx]
    d = np.hypot((xx / (nx - 1) - 0.5) / 0.5, (yy / (ny - 1) - 0.5) / 0.5) / np.sqrt(2)
    d = np.clip(d, 0, 1) ** 1.4
    c0, c1 = np.array(to_rgb(SCREEN_CENTRE)), np.array(to_rgb(SCREEN_EDGE))
    tube = c0 + (c1 - c0) * d[:, :, None]
    im = ax.imshow(tube, extent=(gl[0], gl[2], gl[1], gl[3]), origin="lower", aspect="auto",
                   interpolation="bicubic", zorder=2)
    im.set_clip_path(clip)

    # a faint graticule, as behind the scope
    for gx in np.arange(gl[0] + 12, gl[2] - 6, 12.0):
        ln, = ax.plot([gx, gx], [gl[1], gl[3]], color=PHOSPHOR_GLOW, lw=0.35, alpha=0.05, zorder=3)
        ln.set_clip_path(clip)
    for gy in np.arange(gl[1] + 12, gl[3] - 6, 12.0):
        ln, = ax.plot([gl[0], gl[2]], [gy, gy], color=PHOSPHOR_GLOW, lw=0.35, alpha=0.05, zorder=3)
        ln.set_clip_path(clip)

    # title block: the editor's header and a terminal prompt
    glow_text(ax, cx, 247, "NEON_SYNTH", PHOSPHOR, 76, VT323, ha="center", va="center")
    glow(ax, [gl[0] + 18, gl[2] - 18], [231.5, 231.5], PHOSPHOR_GLOW, lw=0.5, alpha=0.7, zorder=40, clip=clip)
    glow_text(ax, gl[0] + 18, 222, "> USER MANUAL", CYAN, 26, VT323, halo=CYAN, ha="left", va="center")
    glow_text(ax, gl[0] + 18, 211.5, "A learning synthesizer: hear a waveform, see it, shape it",
              PHOSPHOR_DIM, 9.0, PLEX, halo=PHOSPHOR_GLOW, ha="left", va="center")

    # the stack: sine -> triangle -> square -> sawtooth, cyan through magenta to amber
    rows, top, bottom = 28, 192.0, 84.0
    x = np.linspace(gl[0] + 10, gl[2] - 10, 1600)
    s = (x - x[0]) / (x[-1] - x[0])
    window = np.sin(np.pi * s) ** 1.6                    # lines lie flat at the edges
    ph = (s - 0.5) * 3.0                                 # three cycles across the screen
    for r in range(rows):
        u = r / (rows - 1) * 3.0                         # 0..3 through the four shapes
        k = min(int(u), 2)
        f = u - k
        f = f * f * (3 - 2 * f)                          # ease between shapes
        y = (1 - f) * wave(k, ph) + f * wave(k + 1, ph)
        base = top + (bottom - top) * r / (rows - 1)
        color = mix(CYAN, MAGENTA, u / 1.5) if u < 1.5 else mix(MAGENTA, AMBER, (u - 1.5) / 1.5)
        strong = (r % 9 == 0)                            # the four pure shapes stand out
        glow(ax, x, base + y * window * 5.8, color, lw=1.2 if strong else 0.55,
             alpha=1.0 if strong else 0.62, zorder=5 + r)   # inside the glass: no clip, so
                                                                                  # the SVG keeps simplified paths

    for r, name in ((0, "SINE"), (9, "TRIANGLE"), (18, "SQUARE"), (27, "SAWTOOTH")):
        base = top + (bottom - top) * r / (rows - 1)
        glow_text(ax, gl[2] - 6, base + 1.5, name, PHOSPHOR_DIM, 13, VT323, ha="right", va="bottom",
                  zorder=45)

    # the 16-step row, as the sequencer draws it: every other step on, the playing step inverted
    n, gap = 16, 2.0
    size = (gw - 20 - (n - 1) * gap) / n
    x0, ys = gl[0] + 10, 52.0
    notes = ["C3", "C3", "C4", "C3", "D#3", "A3", "G3", "A#3", "C3", "C3", "C4", "C3", "F3", "F3", "G3", "D#3"]
    for i in range(n):
        on, playing = i % 2 == 0, i == 8
        xi = x0 + i * (size + gap)
        face = AMBER if playing else (mix("#000000", AMBER, 0.16) if on else "none")
        ax.add_patch(Rectangle((xi, ys), size, size * 1.3, linewidth=0.8 if on else 0.5,
                               edgecolor=AMBER, facecolor=face, alpha=1.0 if on else 0.35, zorder=45))
        ink = SCREEN_EDGE if playing else AMBER
        ax.text(xi + size / 2, ys + size * 0.72, notes[i], color=ink, fontsize=10.5, fontproperties=VT323,
                ha="center", va="center", alpha=1.0 if on else 0.45, zorder=46)
    glow_text(ax, gl[0] + 10, ys + size * 1.3 + 3.5, "> SEQUENCER", AMBER, 13, VT323, halo=AMBER,
              ha="left", va="bottom", zorder=46)

    # author, version and date: terminal lines at the foot of the screen, with a cursor
    glow_text(ax, gl[0] + 10, 43.0, "RAZVAN JULIAN PETRESCU / AUDIODSP", PHOSPHOR, 8.5, PLEX_SEMI,
              ha="left", va="center")
    line = f"VERSION {version}  ·  {date.upper()}"
    t = glow_text(ax, gl[0] + 10, 38.0, line, CYAN, 8.5, PLEX_SEMI, halo=CYAN, ha="left", va="center")
    fig.canvas.draw()
    bb = t.get_window_extent().transformed(ax.transData.inverted())
    ax.add_patch(Rectangle((bb.x1 + 1.2, 36.4), 1.9, 3.2, facecolor=PHOSPHOR, edgecolor="none", zorder=50))

    # scanlines and the glass: faint dark lines across the tube, a soft sheen at the top left
    lines = [[(gl[0], y), (gl[2], y)] for y in np.arange(gl[1], gl[3], SCANLINE_PITCH_MM)]
    sl = LineCollection(lines, colors=[(0, 0, 0, SCANLINE_STRENGTH * 2.2)], linewidths=0.9, zorder=60)
    sl.set_clip_path(clip)
    ax.add_collection(sl)
    sy, sx = np.mgrid[0:100, 0:70]
    r = np.hypot((sx / 69 - 0.18) / 0.55, (sy / 99 - 0.86) / 0.40)
    sheen = np.zeros((100, 70, 4))
    sheen[:, :, :3] = to_rgb("#b4ffdc")
    sheen[:, :, 3] = 0.07 * np.clip(1 - r, 0, 1) ** 2
    im = ax.imshow(sheen, extent=(gl[0], gl[2], gl[1], gl[3]), origin="lower", aspect="auto",
                   interpolation="bicubic", zorder=61)
    im.set_clip_path(clip)

    # the casing below the bezel: a name plate and a power light
    ax.text(bz[0] + 6, 12.5, "NEONSYNTH", color="#8a8276", fontsize=11, fontproperties=VT323,
            ha="left", va="center", zorder=5)
    lx = bz[2] - 8
    for rad, a in ((2.6, 0.08), (1.8, 0.18), (1.1, 1.0)):
        ax.add_patch(plt.Circle((lx, 12.5), rad, color=GREEN, alpha=a, zorder=5, linewidth=0))
    for i in range(6):                                  # vent slots
        ax.add_patch(FancyBboxPatch((bz[2] - 62 + i * 7.5, 10.8), 4.5, 3.4,
                                    boxstyle="round,pad=0,rounding_size=1.2", facecolor=BEZEL_SHADOW,
                                    edgecolor="none", alpha=0.7, zorder=5))

    out_dir.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_dir / "cover.svg", metadata={"Date": None, "Creator": None})
    fig.savefig(out_dir / "cover.png", dpi=300, metadata={"Software": None})
    plt.close(fig)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=pathlib.Path, default=HERE.parent / "images")
    ap.add_argument("--date", help='release date as printed, e.g. "10 November 2026"')
    a = ap.parse_args()
    cmake = (HERE.parent.parent / "CMakeLists.txt").read_text(encoding="utf-8")
    version = re.search(r"project\(NeonSynth VERSION\s+(\d+\.\d+\.\d+)", cmake).group(1)
    info = a.out / "cover.json"
    date = a.date
    if not date and info.exists():
        old = json.loads(info.read_text(encoding="utf-8"))
        date = old["release_date"] if old.get("version") == version else None
    if not date:
        sys.exit(f"cover: give the release date of v{version} with --date")
    draw(a.out, version, date)
    info.write_text(json.dumps({"version": version, "release_date": date}, indent=2) + "\n", encoding="utf-8")
    print(f"wrote cover.svg, cover.png and cover.json (v{version}, {date})")
