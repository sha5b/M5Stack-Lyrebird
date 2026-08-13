#!/usr/bin/env python3
"""Draw what src/galaxy.cpp will put on the band, on this machine.

Re-implements the arm model — the growth, the pitch bend, the twist, the
projection and the fade — against the real include/galaxy_data.h, drives it with
a stand-in chorus, and writes a PNG. The point is tuning: GROW, WIGGLE, TWIST_AMP
and FILL decide whether an arm is a gesture or a scribble, and finding that out by
reflashing a board is a slow way to work.

Same standing as tools/verify_syrinx.py — a host re-implementation of the maths,
not a build of the firmware. The chorus here is a stand-in (evenly spaced songs, a
synthetic contour), so it settles composition and says nothing about how the real
Poisson arrivals bunch up, or how the panel treats a dim colour.

No dependencies; it writes the PNG itself.

  python3 tools/preview_galaxy.py                    # a few moments, 3x
  python3 tools/preview_galaxy.py --at 12 --zoom 5 --birds 5
"""
import argparse
import colorsys
import math
import os
import re
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Kept in step with src/galaxy.cpp and src/ui.cpp by hand. If a figure here
# disagrees with the firmware, the firmware is right and this file is stale.
BAND_W, BAND_H = 310, 156   # PLOT_W, PLOT_H in src/ui.cpp
FPS = 25                    # FRAME_MS in src/main.cpp
TURN_X_S, TURN_Y_S = 197.0, 131.0
CAM_DIST_NEAR = 1.8
CAM_DIST_IDLE = 2.9
DOLLY_EASE = 0.035
CAM_F = 2.0
ZOOM = 100.0
STRETCH_X = 1.3
CAM_EASE = 0.06
FOCUS_HOLD_MS = 1500
GROW = 0.05
WIGGLE = 0.5
TWIST_AMP = 0.15
TWIST_RATE = 0.16
DOT_R_QUIET = 0.55
DOT_R_LOUD = 3.0
BEAD_R = 3.4
ROSTER_R = 1.8
ROSTER_ENV_MAX = 0.30
ROSTER_MEMORY_MS = 9000
STRIKES = 6
STRIKE_REACH = 1.0
STRIKE_DASH = 2
STRIKE_GAP = 7
STRIKE_AMP = 0.30
STRIKE_ATTACK = 4
STRIKE_LIFE = 45
HALO_RINGS = 3
HALO_STEP = 0.85
HALO_AMP = 0.22
TRAIL_LEN = 72
SONG_GAP_MS = 500



def parse_data():
    text = open(os.path.join(ROOT, "include", "galaxy_data.h")).read()
    scale = float(re.search(r"GALAXY_SCALE = ([0-9.eE+-]+)f", text).group(1))
    body = text[text.index("GALAXY[GALAXY_COUNT] = {"):text.index("// Marks of species")]
    pts = [(int(a), int(b), int(c), int(d))
           for a, b, c, d in re.findall(r"\{(-?\d+), (-?\d+), (-?\d+), (\d+)\}", body)]
    idx_body = text[text.index("GALAXY_ISLAND["):]
    islands = [int(v) for v in re.findall(r"(\d+),", idx_body[idx_body.index("{"):])]
    return pts, islands, scale


def hash01(n, salt):
    """src/galaxy.cpp's hash01(), exact 32-bit."""
    M = 0xFFFFFFFF
    h = (n * 0x9E3779B1 + salt * 0x85EBCA6B) & M
    h ^= h >> 15
    h = (h * 0x2545F491) & M
    h ^= h >> 13
    return h / 4294967296.0


def arm_frame(sp, pts, islands, gscale):
    """src/galaxy.cpp's armFrame(): anchor, tangential growth, radial, twist."""
    p = pts[islands[sp]]
    a = [p[0] * gscale, p[1] * gscale, p[2] * gscale]

    length = math.sqrt(sum(v * v for v in a))
    out = [0.0, 1.0, 0.0] if length < 1e-4 else [v / length for v in a]

    grow = [out[2], 0.0, -out[0]]
    length = math.sqrt(grow[0] ** 2 + grow[2] ** 2)
    if length < 1e-4:
        grow = [1.0, 0.0, 0.0]
    else:
        grow = [grow[0] / length, 0.0, grow[2] / length]

    twist = [out[1] * grow[2] - out[2] * grow[1],
             out[2] * grow[0] - out[0] * grow[2],
             out[0] * grow[1] - out[1] * grow[0]]

    # spin the frame about the radius by a per-species angle, as galaxy.cpp does
    th = hash01(sp, 11) * 2 * math.pi
    ct, st = math.cos(th), math.sin(th)
    grow, twist = ([grow[i] * ct + twist[i] * st for i in range(3)],
                   [-grow[i] * st + twist[i] * ct for i in range(3)])
    return a, grow, out, twist


def voice_color(tag, env, roster=24):
    """src/ui.cpp's uiVoiceColor(), all-birds branch, as 8-bit RGB."""
    pairs = max(1, roster // 2)
    hue = (tag // 2) / pairs
    val = 0.78 if (tag & 1) else 1.0
    e = min(1.0, max(0.0, env))
    val *= 0.25 + 0.75 * math.sqrt(e)
    r, g, b = colorsys.hsv_to_rgb(hue % 1.0, 0.85, val)
    return int(r * 255), int(g * 255), int(b * 255)


def simulate(pts, islands, gscale, until_frame, birds):
    """Run the arm model forward. Returns {tag: [(x, y, z, born, brk), ...]}.

    The stand-in chorus is sized off the real corpus: the median song there is six
    syllables of 67 ms, so that is what this sings, on 127 ms centres, a new song
    every 3.5 s, starts staggered. Pitch rises over the song and falls back — a
    plausible contour and, more to the point, a legible one.
    """
    trails = {}
    state = {}
    for i in range(birds):
        tag = (i * 2) % 24            # roster tags: two per species, 24 of them
        sp = (i * 2 + 1) * (len(islands) - 1) // (2 * birds + 1)  # spread over the corpus
        trails[tag] = []
        state[tag] = {"sp": sp, "songAge": 0, "lastMs": -10 ** 9,
                      "start": 0.7 * i, "period": 3.5}

    for f in range(until_frame + 1):
        now_ms = f * 1000 // FPS
        for tag, st in state.items():
            t = f / FPS - st["start"]
            if t < 0:
                continue
            phase = t % st["period"]
            idx = int(phase / 0.127)
            if idx >= 6 or (phase - idx * 0.127) > 0.067:
                continue  # in a gap between syllables, or the song is over

            fresh = (now_ms - st["lastMs"]) > SONG_GAP_MS
            if fresh:
                st["songAge"] = 0
            st["lastMs"] = now_ms

            frac = min(1.0, phase / 0.76)
            pitch = 0.25 + 0.55 * math.sin(frac * math.pi)

            a, grow, out, twist = arm_frame(st["sp"], pts, islands, gscale)
            length = st["songAge"] * GROW
            w = (pitch - 0.5) * WIGGLE
            tw = math.sin(st["songAge"] * TWIST_RATE) * TWIST_AMP

            # a plausible envelope: attack fast, decay across the syllable
            frac_syl = (phase - idx * 0.127) / 0.067
            env = min(1.0, frac_syl / 0.15) * (1.0 - 0.55 * frac_syl)
            trails[tag].append((
                a[0] + grow[0] * length + out[0] * w + twist[0] * tw,
                a[1] + grow[1] * length + out[1] * w + twist[1] * tw,
                a[2] + grow[2] * length + out[2] * w + twist[2] * tw,
                f, 1 if fresh else 0, max(0.05, env),
            ))
            if len(trails[tag]) > TRAIL_LEN:
                trails[tag].pop(0)
            st["songAge"] += 1

    return trails, {t: st["sp"] for t, st in state.items()}


def camera(trails, frame):
    """Re-run src/galaxy.cpp's focus, dolly, strikes and roster memory to `frame`."""
    cam = [0.0, 0.0, 0.0]
    dist = CAM_DIST_IDLE
    focus, focus_ms = -1, 0
    strikes = []          # (x, y, z, born, colour_tag)
    last_sang = {}
    for f in range(frame + 1):
        now_ms = f * 1000 // FPS
        live = {}
        for tag, pts in trails.items():
            for (x, y, z, born, brk, env) in pts:
                if born == f:
                    live[tag] = (env, (x, y, z))
        for tag in live:
            last_sang[tag] = f * 1000 // FPS
        # a fresh song strikes the grid, merging into a nearby live cross
        for tag, pts in trails.items():
            for (x, y, z, born, brk, env) in pts:
                if born == f and brk:
                    merged = False
                    for i, (sx, sy, sz, sb, stag) in enumerate(strikes):
                        if (f - sb) < STRIKE_LIFE and (
                                (sx - x) ** 2 + (sy - y) ** 2 + (sz - z) ** 2
                                < STRIKE_MERGE ** 2):
                            strikes[i] = (sx, sy, sz, f, stag)
                            merged = True
                            break
                    if not merged:
                        strikes.append((x, y, z, f, tag))
                        strikes[:] = [st for st in strikes
                                      if (f - st[3]) < STRIKE_LIFE][-STRIKES:]
        loudest = max(live.items(), key=lambda kv: kv[1][0])[0] if live else -1
        if focus >= 0 and focus in live:
            focus_ms = now_ms
        elif loudest >= 0 and (focus < 0 or (now_ms - focus_ms) > FOCUS_HOLD_MS):
            focus, focus_ms = loudest, now_ms
        # follow the head of the focused song
        head = None
        for (x, y, z, born, brk, env) in trails.get(focus, []):
            if born <= f:
                head = (x, y, z)
        dist += ((CAM_DIST_NEAR if live else CAM_DIST_IDLE) - dist) * DOLLY_EASE
        if head:
            for i in range(3):
                cam[i] += (head[i] - cam[i]) * CAM_EASE
    return cam, dist, strikes, last_sang


def render(trails, frame, cam, dist, strikes, last_sang, roster, species_of):
    t = frame / FPS
    cax, sax = math.cos(t * 2 * math.pi / TURN_X_S), math.sin(t * 2 * math.pi / TURN_X_S)
    cay, say = math.cos(t * 2 * math.pi / TURN_Y_S), math.sin(t * 2 * math.pi / TURN_Y_S)

    def project(x, y, z):
        dx, dy, dz = x - cam[0], y - cam[1], z - cam[2]
        x1 = dx * cay + dz * say
        z1 = -dx * say + dz * cay
        y2 = dy * cax - z1 * sax
        z2 = dy * sax + z1 * cax
        k = CAM_F / max(0.25, dist + z2)
        return (BAND_W * 0.5 + x1 * ZOOM * STRETCH_X * k,
                BAND_H * 0.5 - y2 * ZOOM * k, k)

    rgb = bytearray(BAND_W * BAND_H * 3)

    def put(x, y, c):
        if 0 <= x < BAND_W and 0 <= y < BAND_H:
            o = (y * BAND_W + x) * 3
            rgb[o:o + 3] = bytes(c)

    def spot(cx, cy, r, c):
        ri = int(r + 0.5)
        for dy in range(-ri, ri + 1):
            for dx in range(-ri, ri + 1):
                if dx * dx + dy * dy <= r * r + 0.3:
                    put(cx + dx, cy + dy, c)

    def wedge(x0, y0, r0, x1, y1, r1, c):
        """Stand-in for LovyanGFX's drawWedgeLine: dots along the segment, radius
        interpolated. The firmware's is anti-aliased, so the real thing is smoother."""
        steps = max(abs(x1 - x0), abs(y1 - y0), 1)
        for k in range(steps + 1):
            f = k / steps
            spot(int(x0 + (x1 - x0) * f), int(y0 + (y1 - y0) * f),
                 r0 + (r1 - r0) * f, c)

    def dim(c, amp):
        return tuple(int(v * amp) for v in c)

    # the strikes, under everything: three world axes, length quoted in pixels
    reach = STRIKE_REACH * math.hypot(BAND_W, BAND_H)
    for (sx, sy, sz, born, stag) in strikes:
        age = frame - born
        if age < 0 or age >= STRIKE_LIFE:
            continue
        # follow the bird: the cross hangs on wherever its song has got to
        head = None
        for (x, y, z, b, brk, env) in trails.get(stag, []):
            if b <= frame:
                head = (x, y, z)
        if head is None:
            continue
        sx, sy, sz = head
        cx, cy, _ = project(sx, sy, sz)
        rise = age / STRIKE_ATTACK if age < STRIKE_ATTACK else 1.0
        fall = 1.0 - age / STRIKE_LIFE
        amp = rise * fall * fall * STRIKE_AMP
        if amp <= 0.02:
            continue
        c = dim(voice_color(stag, 1.0), amp)
        for axis in range(3):
            q = [sx, sy, sz]
            q[axis] += 0.05
            px, py, _ = project(*q)
            dx, dy = px - cx, py - cy
            L = math.hypot(dx, dy)
            if L < 1e-3:
                continue
            dx, dy = dx / L, dy / L
            for side in (-1, 1):
                d = 0.0
                while d < reach:
                    for t2 in range(STRIKE_DASH + 1):
                        put(int(cx + dx * (d + t2) * side), int(cy + dy * (d + t2) * side), c)
                    d += STRIKE_DASH + STRIKE_GAP

    # the roster, over the strikes: one dot per species, lit by how recently it sang
    seen = set()
    for tag, (px, py, pz) in roster.items():
        sp = species_of[tag]
        if sp in seen:
            continue
        seen.add(sp)
        last = max((last_sang.get(t, 0) for t, s2 in species_of.items() if s2 == sp),
                   default=0)
        if not last:
            continue
        since = frame * 1000 // FPS - last
        if since >= ROSTER_MEMORY_MS:
            continue
        mem = (1.0 - since / ROSTER_MEMORY_MS) ** 2
        X, Y, persp = project(px, py, pz)
        if -4 <= X <= BAND_W + 4 and -4 <= Y <= BAND_H + 4:
            spot(int(X), int(Y), ROSTER_R * persp * (0.7 + 0.6 * mem),
                 voice_color(tag, ROSTER_ENV_MAX * mem))

    for tag, points in trails.items():
        prev = None
        for i, (x, y, z, born, brk, env) in enumerate(points):
            age = frame - born
            if age >= TRAIL_LEN:
                prev = None
                continue
            X, Y, persp = project(x, y, z)
            X, Y = int(X), int(Y)
            fade = 1.0 - age / TRAIL_LEN
            c = voice_color(tag, fade * fade)
            r = max(0.35, (DOT_R_QUIET + (DOT_R_LOUD - DOT_R_QUIET) * env) * fade * persp)
            if prev and not brk:
                wedge(prev[0], prev[1], prev[2], X, Y, r, c)
            else:
                spot(X, Y, r, c)
            prev = (X, Y, r)
            if i == len(points) - 1 and age < 2:
                for ring in range(HALO_RINGS, 0, -1):
                    spot(X, Y, BEAD_R * persp * (1 + HALO_STEP * ring),
                         dim(c, HALO_AMP / ring))
                spot(X, Y, BEAD_R * persp, c)
    return rgb


def png(path, rgb, w, h, zoom):
    raw = bytearray()
    for y in range(h):
        for _ in range(zoom):
            raw.append(0)  # filter: none
            row = rgb[y * w * 3:(y + 1) * w * 3]
            for x in range(w):
                raw += row[x * 3:x * 3 + 3] * zoom
    W, H = w * zoom, h * zoom

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--at", type=float, action="append",
                    help="seconds of chorus to simulate; repeatable (default 4, 9, 20)")
    ap.add_argument("--birds", type=int, default=3, help="birds singing (default 3)")
    ap.add_argument("--zoom", type=int, default=3)
    ap.add_argument("--out", default=os.path.join(ROOT, "galaxy-preview"))
    args = ap.parse_args()

    pts, islands, gscale = parse_data()
    print("%d marks, %d islands, band %dx%d" % (len(pts), len(islands) - 1, BAND_W, BAND_H))

    for secs in (args.at or [4.0, 9.0, 20.0]):
        frame = int(secs * FPS)
        trails, species_of = simulate(pts, islands, gscale, frame, args.birds)
        cam, dist, strikes, last_sang = camera(trails, frame)
        roster = {t: tuple(v * gscale for v in pts[islands[sp]][:3])
                  for t, sp in species_of.items()}
        rgb = render(trails, frame, cam, dist, strikes, last_sang, roster, species_of)
        path = "%s-%gs.png" % (args.out, secs)
        png(path, rgb, BAND_W, BAND_H, args.zoom)
        print("  %s  (%d arms)" % (path, sum(1 for v in trails.values() if v)))


if __name__ == "__main__":
    main()
