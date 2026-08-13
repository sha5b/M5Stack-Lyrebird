#!/usr/bin/env python3
"""Lay out the corpus as a point cloud, for the flasher page and for the board.

Reads include/bird_data.h — the generated header the firmware sings from, so no
picture of the corpus can drift from the corpus — and writes the same cloud twice:

  web/src/lib/corpus.ts   base64, for the page's backdrop ($lib/galaxy)
  include/galaxy_data.h   PROGMEM, for the device's band (src/galaxy.cpp)

Every syllable, not a sample: 12724 marks is 56 KB either way, which against the
1.8 MB firmware image and the 3 MB app partition is nothing, and it is the
difference between "the corpus" and "some of the corpus". A sample also loses the
thing that makes the picture worth drawing — a species reads as an island, and
islands with holes in them read as noise.

**The layout is done here, once.** It used to be done in TypeScript, which meant
the board would have needed its own copy of the same maths in C++, and "kept in
step by hand" across two languages is a promise that does not survive a year. It
also could not be kept: the mixing hash reads well in JS and is not reproducible
there, because `h * 0x2545f491` overflows the 53 bits a double can hold exactly.
So both consumers now get positions rather than features, and neither one can
disagree with the other about where a syllable is.

Where a mark goes:

  island   Every species gets one, on a Fibonacci sphere in corpus order — so
           neighbouring islands mean "next in the table", not "similar". Islands
           are pulled in or out per species, filling from 0.3R, because a hollow
           shell projects to a ring and a filled one projects to a disc with a
           brighter limb, which is the only thing that gives the cloud an edge.
  inside   The syllable's own three numbers. Pitch is up, as it is on the
           device's log frequency axis; duration and contour sweep are the two
           horizontal axes, rotated by a per-island angle so two species with
           similar syllables are not two identically-oriented smudges.

This is not a learned embedding. The parent project's galaxy is a real projection
of fitted vectors; this arranges four quantised features. Within an island the
arrangement *is* the data; between islands it is only the table's order.

Re-run after tools/generate_assets.py:

  python3 tools/generate_galaxy.py
"""
import base64
import json
import math
import os
import re
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "bird_data.h")
OUT_TS = os.path.join(ROOT, "web", "src", "lib", "corpus.ts")
OUT_H = os.path.join(ROOT, "include", "galaxy_data.h")

# The axis the device plots on (see include/ui.h), so a mark's height in the
# cloud means what a trace's height means on the spectrogram.
F_LO, F_HI = 250.0, 10000.0
DUR_LO, DUR_HI = 0.02, 2.0
SWEEP_MAX = 1.5     # contour range / mean; above this is a whistle-to-buzz leap

GOLDEN_ANGLE = 2.399963229728653

# A buzz is a fainter mark than a whistle: the timbre classes carry how much of
# the sound is actually pitched.
TIMBRE_WEIGHT = (1.0, 0.82, 0.66)


def hash01(n, salt):
    """Deterministic [0, 1). Exact 32-bit integer maths — islands must sit in the
    same place on the board, on the page, and across rebuilds."""
    M = 0xFFFFFFFF
    h = (n * 0x9E3779B1 + salt * 0x85EBCA6B) & M
    h ^= h >> 15
    h = (h * 0x2545F491) & M
    h ^= h >> 13
    return h / 4294967296.0


def norm_log(value, lo, hi):
    """value -> 0..1, log-spaced and clamped."""
    if value <= lo:
        return 0.0
    if value >= hi:
        return 1.0
    return math.log(value / lo) / math.log(hi / lo)


def parse(header):
    """-> [[syllable, ...], ...] in SPECIES[] order."""
    text = open(header).read()

    contours = {}
    for m in re.finditer(r"ContourPoint (cont_\w+)\[\]\s*=\s*\{(.*?)\};", text, re.S):
        pts = re.findall(r"\{([0-9.eE+-]+)f,\s*([0-9.eE+-]+)f\}", m.group(2))
        contours[m.group(1)] = [float(hz) for _, hz in pts]

    banks = {}
    for m in re.finditer(r"SyllableData (syl_\w+)\[\]\s*=\s*\{(.*?)\n\};", text, re.S):
        rows = []
        for row in re.finditer(r"\{([^{}]*?)\},\s*/\*", m.group(2)):
            parts = [p.strip().rstrip("f") for p in row.group(1).split(",")]
            rows.append({
                "dur": float(parts[0]),
                "timbre": int(parts[2]),
                "hz": contours[parts[12].strip()],
            })
        banks[m.group(1)] = rows

    table = re.search(r"SpeciesData SPECIES\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    species = []
    for row in re.finditer(r'\{"(?:[^"\\]|\\.)*",\s*\d+,\s*(syl_\w+),', table.group(1)):
        species.append(banks[row.group(1)])
    return species


def layout(species):
    """-> ([(x, y, z, shade), ...], [island_start, ...])

    Points come out in species order, so a species is a contiguous run and both
    consumers can find one from two integers instead of a list per species.
    """
    n_species = len(species)
    points = []
    starts = [0]

    for isl, bank in enumerate(species):
        yc = 1 - (2 * isl + 1) / n_species
        rc = math.sqrt(max(0.0, 1 - yc * yc))
        theta = GOLDEN_ANGLE * isl
        shell = 0.3 + 0.7 * hash01(isl, 1)
        cx = math.cos(theta) * rc * shell
        cz = math.sin(theta) * rc * shell
        cy = yc * shell

        spin = hash01(isl, 2) * math.pi * 2
        spread = 0.07 + 0.05 * hash01(isl, 3)
        cs, sn = math.cos(spin), math.sin(spin)

        for syl in bank:
            hz = syl["hz"]
            mean = sum(hz) / len(hz)
            sweep = (max(hz) - min(hz)) / mean if mean > 0 else 0.0

            u = (norm_log(syl["dur"], DUR_LO, DUR_HI) - 0.5) * 2
            v = (min(sweep, SWEEP_MAX) / SWEEP_MAX - 0.5) * 2
            up = (norm_log(mean, F_LO, F_HI) - 0.5) * 2

            points.append((
                cx + (u * cs - v * sn) * spread,
                cy + up * spread * 1.4,
                cz + (u * sn + v * cs) * spread,
                TIMBRE_WEIGHT[syl["timbre"]],
            ))
        starts.append(len(points))

    return points, starts


def quantise(points):
    """Positions to int8, with the scale that maps them back. int8 over a unit
    cloud is 1/127, which is well under a pixel in either renderer."""
    extent = max(max(abs(p[0]), abs(p[1]), abs(p[2])) for p in points)
    k = 127.0 / extent
    out = []
    for x, y, z, shade in points:
        out.append((
            max(-127, min(127, int(round(x * k)))),
            max(-127, min(127, int(round(y * k)))),
            max(-127, min(127, int(round(z * k)))),
            max(0, min(255, int(round(shade * 255)))),
        ))
    return out, extent / 127.0


def write_ts(pts, starts, scale, n_species):
    blob = b"".join(struct.pack("<bbbB", *p) for p in pts)
    idx = b"".join(struct.pack("<H", s) for s in starts)

    def b64_lines(data):
        b64 = base64.b64encode(data).decode("ascii")
        lines = [b64[i:i + 100] for i in range(0, len(b64), 100)]
        # json.dumps rather than repr: these go into TypeScript, not Python.
        return " +\n".join("\t%s" % json.dumps(line) for line in lines)

    with open(OUT_TS, "w") as f:
        f.write(f"""// Generated by tools/generate_galaxy.py — do not edit.
// Source: include/bird_data.h (Lyrebird inventory, CC BY-NC-SA 4.0).
//
// The corpus laid out as a point cloud: every one of the {len(pts)} syllables the
// firmware sings, across {n_species} species. Positions, not features — the layout
// happens once, in the generator, so this page and include/galaxy_data.h cannot
// disagree about where a syllable is. Four bytes a mark, plus one index entry a
// species. Unpacked by $lib/galaxy; see the generator for how a mark is placed.

export const SPECIES = {n_species};
export const SYLLABLES = {len(pts)};

/** int8 units back to the layout's own scale, where the cloud is about 1 across. */
export const SCALE = {scale!r};

const PACKED =
{b64_lines(blob)};

const INDEX =
{b64_lines(idx)};

export interface Cloud {{
	count: number;
	/** Layout units, roughly -1..1. */
	x: Float32Array;
	y: Float32Array;
	z: Float32Array;
	/** 0..1: how much of this syllable is a pitched tone. A buzz is fainter. */
	shade: Float32Array;
	/**
	 * Marks of species `s` are `start[s] .. start[s + 1]`. They are contiguous
	 * because the generator emits them in species order, which is what lets an
	 * island light up without a list per species.
	 */
	start: Uint16Array;
}}

/**
 * Unpack, once. One pass into flat typed arrays rather than {len(pts)} objects: the
 * renderer walks these every frame, and an array of small objects is the one
 * shape that would make that cost anything.
 */
export function cloud(): Cloud {{
	const bytes = atob(PACKED);
	const count = bytes.length / 4;
	const out: Cloud = {{
		count,
		x: new Float32Array(count),
		y: new Float32Array(count),
		z: new Float32Array(count),
		shade: new Float32Array(count),
		start: new Uint16Array(SPECIES + 1)
	}};
	for (let i = 0, o = 0; i < count; i++, o += 4) {{
		// atob gives unsigned bytes; these three are signed.
		out.x[i] = ((bytes.charCodeAt(o) << 24) >> 24) * SCALE;
		out.y[i] = ((bytes.charCodeAt(o + 1) << 24) >> 24) * SCALE;
		out.z[i] = ((bytes.charCodeAt(o + 2) << 24) >> 24) * SCALE;
		out.shade[i] = bytes.charCodeAt(o + 3) / 255;
	}}
	const index = atob(INDEX);
	for (let s = 0; s <= SPECIES; s++) {{
		out.start[s] = index.charCodeAt(s * 2) | (index.charCodeAt(s * 2 + 1) << 8);
	}}
	return out;
}}
""")
    return len(blob) + len(idx)


def write_header(pts, starts, scale, n_species):
    lines = [
        "// Generated by tools/generate_galaxy.py — do not edit.",
        "// Source: include/bird_data.h (Lyrebird inventory, CC BY-NC-SA 4.0).",
        "//",
        "// The corpus as a point cloud, for the band on the screen (src/galaxy.cpp).",
        "// The same layout the flasher page draws from web/src/lib/corpus.ts, computed",
        "// once in the generator so the two cannot disagree.",
        "//",
        "// int8 axes: 1/127 of the cloud's own extent, which is well under a pixel on a",
        "// 310 px band. PROGMEM, so this costs flash and not RAM.",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "struct GalaxyPoint {",
        "    int8_t x, y, z;",
        "    uint8_t shade;  // 0..255: how much of this syllable is a pitched tone",
        "};",
        "",
        "#define GALAXY_COUNT %d" % len(pts),
        "",
        "// int8 units back to the layout's scale, where the cloud is about 1 across.",
        "static const float GALAXY_SCALE = %sf;" % repr(scale),
        "",
        "static const GalaxyPoint GALAXY[GALAXY_COUNT] = {",
    ]
    for i in range(0, len(pts), 6):
        row = ", ".join("{%d, %d, %d, %d}" % p for p in pts[i:i + 6])
        lines.append("    %s," % row)
    lines += [
        "};",
        "",
        "// Marks of species s are GALAXY[GALAXY_ISLAND[s] .. GALAXY_ISLAND[s + 1]).",
        "static const uint16_t GALAXY_ISLAND[%d] = {" % len(starts),
    ]
    for i in range(0, len(starts), 16):
        lines.append("    %s," % ", ".join(str(s) for s in starts[i:i + 16]))
    lines += ["};", ""]

    text = "\n".join(lines)
    with open(OUT_H, "w") as f:
        f.write(text)
    return len(pts) * 4 + len(starts) * 2, len(text)


def main():
    species = parse(HEADER)
    points, starts = layout(species)
    pts, scale = quantise(points)

    packed_ts = write_ts(pts, starts, scale, len(species))
    packed_h, source_h = write_header(pts, starts, scale, len(species))

    print("%d species, %d syllables" % (len(species), len(pts)))
    print("  corpus.ts:      %.1f KB packed" % (packed_ts / 1024))
    print("  galaxy_data.h:  %.1f KB of flash, %.1f KB of source"
          % (packed_h / 1024, source_h / 1024))


if __name__ == "__main__":
    main()
