# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
# Licensed under the GNU Affero General Public License v3; see LICENSE.
"""Draws the repository's social preview (docs/images/social-preview.png, 1280 x 640).

GitHub shows it when the repository is linked on social sites and chat apps. It is not set
from the repository: upload it under Settings -> General -> Social preview after a change.
It carries no version number, so it does not go stale with each release; redraw it when the
look changes (it uses the Oscillator capture, docs/images/captures/ui-oscillator-running.png).

    python docs/figures/social_preview.py
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]
W, H = 1280, 640
CASING, BEZEL, RIM = (42, 39, 35), (53, 48, 42), (15, 13, 11)
GLASS_CENTRE, GLASS_EDGE = (7, 23, 15), (1, 4, 2)
PHOSPHOR, PHOSPHOR_DIM, GLOW = (200, 255, 230), (111, 174, 149), (61, 255, 176)
AMBER = (255, 193, 94)


def font(name, size):
    return ImageFont.truetype(str(ROOT / "fonts" / name), size)


def glass(size):
    """The tube: brightest in the middle, falling to almost black at the corners."""
    w, h = size
    grad = Image.radial_gradient("L").resize((w, h))      # 0 in the centre, 255 at the edge
    return Image.composite(Image.new("RGB", size, GLASS_EDGE), Image.new("RGB", size, GLASS_CENTRE), grad)


def glow_text(img, xy, text, fnt, colour, halo):
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).text(xy, text, font=fnt, fill=halo + (200,))
    img.alpha_composite(layer.filter(ImageFilter.GaussianBlur(7)))
    ImageDraw.Draw(img).text(xy, text, font=fnt, fill=colour)


def main():
    img = Image.new("RGBA", (W, H), CASING + (255,))
    d = ImageDraw.Draw(img)

    # Bezel and glass, the whole card.
    d.rounded_rectangle((14, 14, W - 15, H - 15), 34, fill=RIM)
    d.rounded_rectangle((20, 20, W - 21, H - 21), 30, fill=BEZEL)
    screen = (40, 40, W - 41, H - 41)
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle(screen, 22, fill=255)
    tube = Image.new("RGBA", (W, H))
    tube.paste(glass((screen[2] - screen[0], screen[3] - screen[1])), screen[:2])
    img.paste(tube, (0, 0), mask)

    # The window itself, on the right, with a faint phosphor halo around it.
    shot = Image.open(ROOT / "docs/images/captures/ui-oscillator-running.png").convert("RGBA")
    sw = 720
    shot = shot.resize((sw, round(shot.height * sw / shot.width)), Image.LANCZOS)
    sx, sy = W - 40 - 34 - sw, (H - shot.height) // 2
    halo = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(halo).rectangle((sx - 4, sy - 4, sx + sw + 3, sy + shot.height + 3), fill=GLOW + (90,))
    img.alpha_composite(halo.filter(ImageFilter.GaussianBlur(14)))
    img.alpha_composite(shot, (sx, sy))

    # The words, on the left.
    x = 78
    glow_text(img, (x, 132), "NEON_SYNTH", font("VT323-Regular.ttf", 96), PHOSPHOR, GLOW)
    d = ImageDraw.Draw(img)
    plex, plexBold = font("IBMPlexMono-Regular.ttf", 21), font("IBMPlexMono-SemiBold.ttf", 21)
    d.text((x, 246), "> a learning synthesizer", font=plexBold, fill=PHOSPHOR)
    d.text((x, 276), "  for musicians and coders", font=plexBold, fill=PHOSPHOR)
    for i, line in enumerate(["Four sources, a filter, an LFO", "and a 16-step sequencer:",
                              "every control shows what", "it does to the sound."]):
        d.text((x, 330 + 28 * i), line, font=plex, fill=PHOSPHOR_DIM)
    d.text((x, 498), "VST3 · STANDALONE · AGPLv3", font=font("VT323-Regular.ttf", 30), fill=AMBER)
    d.text((x, 532), "github.com/kv244/Synth", font=plex, fill=PHOSPHOR_DIM)

    # Scanlines over the glass only.
    lines = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ld = ImageDraw.Draw(lines)
    for y in range(screen[1], screen[3], 3):
        ld.line((screen[0], y + 2, screen[2], y + 2), fill=(0, 0, 0, 34))
    clipped = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    clipped.paste(lines, (0, 0), mask)
    img.alpha_composite(clipped)

    out = ROOT / "docs/images/social-preview.png"
    img.convert("RGB").save(out, optimize=True)
    print(out, out.stat().st_size // 1024, "KB")


if __name__ == "__main__":
    main()
