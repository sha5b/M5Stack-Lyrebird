#!/usr/bin/env python3
"""Draw the Open Graph card, web/static/og-card.png, 1200 x 630.

The card is the only picture of this project most people will ever see: it is what a
link to it becomes in a chat window, a search result or a post. So it has to say the
same things the page says, and the figures on it are **read out of the generated
headers** rather than typed here — a card claiming a different corpus size than the
firmware sings is the exact drift this repo keeps having to clean up.

Layout follows the parent project's card (Lyrebird/app/static/brand/og-card.png): the
mark on faintly ruled paper, the name, a mono strapline, one sentence, a row of figures over
a signal-coloured rule, and the credit. Tokens are web/src/app.css's.

**The type is a substitute and that is visible.** The house pairing is IBM Plex
Sans/Mono with a bookish serif for the wordmark; none of those are on this machine, so
this draws Noto Serif, Liberation Sans and Liberation Mono. If you regenerate the card
somewhere with Plex installed, point FONTS below at it — the layout does not change.

Needs Pillow. Run from anywhere:

  python3 tools/make_og_card.py
"""
import os
import re

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

W, H = 1200, 630
PAPER = (250, 251, 252)
INK = (16, 22, 31)
INK_DIM = (82, 96, 111)
INK_FAINT = (141, 153, 168)
RULE = (216, 222, 231)
SIGNAL = (91, 63, 214)

# Ruled paper, as on the parent's card. Nothing to do with the board's picture.
GRID_STEP = 60

FONTS = {
    "serif": "/usr/share/fonts/google-noto-vf/NotoSerif[wght].ttf",
    "sans": "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
    "mono": "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf",
}


def font(kind, size, weight=None):
    f = ImageFont.truetype(FONTS[kind], size)
    if weight is not None:
        try:
            f.set_variation_by_axes([weight])
        except (OSError, AttributeError):
            pass  # a static font: the regular instance is what we get
    return f


def figures():
    """The corpus, from the headers the firmware is built from."""
    birds = open(os.path.join(ROOT, "include", "bird_data.h")).read()
    galaxy = open(os.path.join(ROOT, "include", "galaxy_data.h")).read()
    species = int(re.search(r"#define SPECIES_COUNT (\d+)", birds).group(1))
    syllables = int(re.search(r"#define GALAXY_COUNT (\d+)", galaxy).group(1))
    songs = len(re.findall(r"\{\d+, seq_", birds))
    return species, syllables, songs


def main():
    species, syllables, songs = figures()

    img = Image.new("RGB", (W, H), PAPER)
    d = ImageDraw.Draw(img)

    for x in range(0, W + 1, GRID_STEP):
        d.line([(x, 0), (x, H)], fill=RULE, width=1)
    for y in range(0, H + 1, GRID_STEP):
        d.line([(0, y), (W, y)], fill=RULE, width=1)
    # The card is mostly paper: hold the ruling back to a whisper.
    img = Image.blend(Image.new("RGB", (W, H), PAPER), img, 0.45)
    d = ImageDraw.Draw(img)

    mark = Image.open(os.path.join(ROOT, "web", "static", "icon-512.png")).convert("RGBA")
    mark = mark.resize((104, 104), Image.LANCZOS)
    img.paste(mark, (96, 128), mark)

    d.text((228, 122), "Lyrebird", font=font("serif", 84, 500), fill=INK)
    d.text((232, 226), "m5stack fire  ·  cores3  ·  no samples, no sd card",
           font=font("mono", 22), fill=INK_DIM)

    d.text((96, 320), "A pocket dawn chorus. Every note is integrated on the device",
           font=font("sans", 34), fill=INK)
    d.text((96, 366), "from a physical model of the avian syrinx.",
           font=font("sans", 34), fill=INK)

    row = [f"{species} species", f"{syllables} syllables", f"{songs} songs",
           "flashed from the browser"]
    x = 96
    fm = font("mono", 21)
    for i, cell in enumerate(row):
        d.text((x, 470), cell, font=fm, fill=INK_DIM)
        x += int(d.textlength(cell, font=fm)) + (64 if i < len(row) - 1 else 0)
    d.line([(96, 508), (x, 508)], fill=SIGNAL, width=2)

    d.text((96, 536), "Shahab Nedaei  ·  variable.gallery", font=font("sans", 22),
           fill=INK_FAINT)

    out = os.path.join(ROOT, "web", "static", "og-card.png")
    img.save(out, optimize=True)
    print(f"{out}  {W}x{H}  {species} species, {syllables} syllables, {songs} songs")


if __name__ == "__main__":
    main()
