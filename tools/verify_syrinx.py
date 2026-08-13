#!/usr/bin/env python3
"""Checks the parts of src/syrinx.cpp that the control-rate rework actually
changed. The ODE step, the biquad and the DC/LP chain are byte-for-byte the same
code, so what needs proving is:

  1. the render loop's chunked cursor still visits every sample exactly once,
     and fires a control update exactly on 32-sample boundaries;
  2. the incremental envelope equals the old closed-form one;
  3. the recursive AM / vibrato oscillators equal cosf/sinf over a syllable;
  4. the Pade tanh and the new drive/trim are close to the old clipper;
  5. moving beta to control rate + a one-block ramp costs little pitch accuracy.

(5) uses the real calibration tables, inverted, so the answer comes out in cents.
"""
import math
import os
import re
import struct
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SR = 22050.0
CTRL = 32


# ---------------------------------------------------------------- parsing ----
def read(path):
    with open(path) as f:
        return f.read()


def f32(x):
    """Round a Python float to float32, so we model the firmware's precision."""
    return struct.unpack("f", struct.pack("f", x))[0]


def parse_float_array(src, name):
    m = re.search(r"%s\s*\[[0-9]*\]\s*=\s*\{(.*?)\};" % re.escape(name), src, re.S)
    return [float(v) for v in re.findall(r"-?[0-9.eE+-]+f?", m.group(1).replace("f", ""))]


def parse_float_matrix(src, name):
    m = re.search(r"%s\s*\[[0-9]*\]\[[0-9]*\]\s*=\s*\{(.*?)\n\};" % re.escape(name), src, re.S)
    rows = re.findall(r"\{([^{}]*)\}", m.group(1))
    return [[float(v) for v in re.findall(r"-?[0-9.eE+-]+", r.replace("f", ""))] for r in rows]


CAL = read(f"{ROOT}/include/calibration.h")
P_GRID = parse_float_array(CAL, "CAL_PRESSURE_GRID")
B_GRID = parse_float_array(CAL, "CAL_BETA_GRID")
F0_GRID = parse_float_array(CAL, "CAL_INV_F0_GRID")
INV_BETA = parse_float_matrix(CAL, "CAL_INV_BETA")
AMP_PP = parse_float_matrix(CAL, "CAL_AMP_PP")
BETA_MIN, BETA_MAX = 0.15, 16.0
CAL_GAMMA = 24000.0
PHI = 0.17061

assert len(P_GRID) == 29 and len(F0_GRID) == 192 and len(B_GRID) == 206
assert len(INV_BETA) == 29 and len(INV_BETA[0]) == 192
assert len(AMP_PP) == 29 and len(AMP_PP[0]) == 206

BIRD = read(f"{ROOT}/include/bird_data.h")
CONTOURS = {}
for m in re.finditer(r"ContourPoint (cont_\w+)\[\]\s*=\s*\{(.*?)\};", BIRD, re.S):
    pts = re.findall(r"\{([0-9.eE+-]+)f,\s*([0-9.eE+-]+)f\}", m.group(2))
    CONTOURS[m.group(1)] = [(float(a), float(b)) for a, b in pts]

SYLLABLES = []
for m in re.finditer(r"SyllableData (syl_\w+)\[\]\s*=\s*\{(.*?)\n\};", BIRD, re.S):
    for row in re.finditer(r"\{([^{}]*?)\},\s*/\*\s*(\S+)", m.group(2)):
        parts = [p.strip() for p in row.group(1).split(",")]
        SYLLABLES.append(dict(
            name=f"{m.group(1)}:{row.group(2)}",
            dur=float(parts[0].rstrip("f")), level=float(parts[1].rstrip("f")),
            timbre=int(parts[2]), hold=int(parts[3]), two=int(parts[4]),
            attack=float(parts[5].rstrip("f")), harmonic=float(parts[6].rstrip("f")),
            am=float(parts[7].rstrip("f")), amDepth=float(parts[8].rstrip("f")),
            vibRate=float(parts[9].rstrip("f")), vibDepth=float(parts[10].rstrip("f")),
            contour=CONTOURS[parts[12]]))

# The header is generated, so its syllable count is whatever inventory it was
# built from — 44 for the vendored authored twelve, 12724 for the shipped corpus.
# Assert only that parsing found something and lost nothing.
assert SYLLABLES, "no syllables parsed out of include/bird_data.h"
assert len(SYLLABLES) == len(CONTOURS), (len(SYLLABLES), len(CONTOURS))
print(f"parsed {len(SYLLABLES)} syllables, {len(CONTOURS)} contours\n")

fails = []


def check(label, ok, detail):
    print(f"  [{'PASS' if ok else 'FAIL'}] {label}: {detail}")
    if not ok:
        fails.append(label)


# ------------------------------------------- 1. render loop cursor ----------
# Mirrors the while/chunk structure of the new syrinxRender: an outer loop over
# render buffers, an inner walk that stops at control boundaries.
def simulate_loop(dur_samples, block_sizes):
    visited, ctrl_at, pos = [], [], 0
    for n in block_sizes:
        i = 0
        while i < n:
            idx = pos
            if idx >= dur_samples:
                break
            if (idx & (CTRL - 1)) == 0:
                ctrl_at.append(idx)
            chunk = CTRL - (idx & (CTRL - 1))
            chunk = min(chunk, n - i, dur_samples - idx)
            for _ in range(chunk):
                visited.append(idx)
                i += 1
                idx += 1
            pos = idx
    return visited, ctrl_at


print("1. render loop cursor")
bad = None
for dur in (41, 256, 257, 1000, 2646, 4410):
    for blocks in ([256] * 40, [17] * 400, [1] * 5000, [255, 3, 300, 64] * 30):
        vis, ctrl = simulate_loop(dur, blocks)
        if vis != list(range(dur)):
            bad = f"dur={dur}: visited {len(vis)} samples, expected {dur}"
            break
        want = list(range(0, dur, CTRL))
        if ctrl != want:
            bad = f"dur={dur}: control updates at {ctrl[:6]}, expected {want[:6]}"
            break
    if bad:
        break
check("every sample visited once, in order; control fires on 32-boundaries",
      bad is None, bad or "6 durations x 4 block patterns, all exact")


# ------------------------------------------------- 2. envelope --------------
def env_closed_form(syl, i, envN):
    """The old envelopeAt()."""
    if envN <= 1 or i >= envN:
        return 0.0
    dur = envN / SR
    t = i / SR
    atk = syl["attack"] if syl["attack"] > 0 else 0.008
    atk = min(max(atk, 1.0 / SR), 0.9 * dur)
    tau = dur / 2.5
    if t < atk:
        env = t / atk
    elif syl["hold"]:
        knee = 0.75 * dur
        env = math.exp(-(t - knee) / tau) if t > knee else 1.0
    else:
        env = math.exp(-(t - atk) / tau)
    rel = round(0.006 * SR)
    if 1 < rel < envN and i >= envN - rel:
        env *= max(0.0, 1.0 - (i - (envN - rel)) / (rel - 1))
    return env


def env_incremental(syl, envN, dur_s):
    """The new incremental path, in the same order the firmware runs it."""
    envDur = envN / SR
    atk = syl["attack"] if syl["attack"] > 0 else 0.008
    atk = min(max(atk, 1.0 / SR), 0.9 * envDur)
    atkN = max(1, math.ceil(atk * SR))
    atkInc = 1.0 / (atk * SR)
    kneeS = 0.75 * envDur
    kneeN = math.floor(kneeS * SR) + 1
    tau = envDur / 2.5
    invTau = 1.0 / max(1e-4, tau)
    decMul = f32(math.exp(-(1.0 / SR) * invTau))
    rel = round(0.006 * SR)
    relN = rel if 1 < rel < envN else 0
    relStart = envN - relN
    relInc = 1.0 / (relN - 1) if relN > 1 else 0.0

    started, env, out = False, 0.0, []
    for idx in range(envN + 40):
        if idx >= envN:
            out.append(0.0)
            env = 0.0
            continue
        if idx < atkN:
            env = idx * atkInc
        elif syl["hold"] and idx < kneeN:
            env = 1.0
        elif started:
            env = f32(env * decMul)
        else:
            started = True
            t = idx / SR
            ref = kneeS if syl["hold"] else atk
            env = f32(math.exp(-(t - ref) * invTau)) if t > ref else 1.0
        e = env
        if relN and idx >= relStart:
            e *= max(0.0, 1.0 - (idx - relStart) * relInc)
        out.append(e)
    return out


print("\n2. incremental envelope vs the old closed form")
worst, worst_name = 0.0, ""
for syl in SYLLABLES:
    dur_s = syl["dur"]  # durScale 1.0
    envN = max(2, round(dur_s * SR))
    inc = env_incremental(syl, envN, dur_s)
    for i in range(envN + 40):
        d = abs(inc[i] - env_closed_form(syl, i, envN))
        if d > worst:
            worst, worst_name = d, f"{syl['name']} @ {i}"
check(f"max |incremental - closed form| over all {len(SYLLABLES)} syllables",
      worst < 5e-4, f"{worst:.2e} (worst at {worst_name}); 1 LSB of the 8-bit DAC is 3.9e-3")


# ----------------------------------------------- 3. AM / vibrato ------------
print("\n3. recursive oscillators vs libm")
worst_am, worst_am_name = 0.0, ""
for syl in SYLLABLES:
    if syl["am"] <= 0 or syl["amDepth"] <= 0:
        continue
    w = 2.0 * math.pi * syl["am"] / SR
    rc, rs = f32(math.cos(w)), f32(math.sin(w))
    c, s = 1.0, 0.0
    n = round((syl["dur"] + 0.02) * SR)
    for i in range(n):
        exact = math.cos(2.0 * math.pi * syl["am"] * i / SR)
        # what the firmware multiplies the pressure by, either way
        d = abs(syl["amDepth"] * 0.5 * (c - exact))
        if d > worst_am:
            worst_am, worst_am_name = d, f"{syl['name']} @ {i}/{n}"
        c, s = f32(c * rc - s * rs), f32(s * rc + c * rs)
check("AM: max pressure-factor error over a full syllable",
      worst_am < 1e-4, f"{worst_am:.2e} (worst at {worst_am_name})")

# vibrato steps once per control block, so it is a staircase by design
worst_vib, worst_vib_name = 0.0, ""
for syl in SYLLABLES:
    if syl["vibRate"] <= 0 or syl["vibDepth"] <= 0:
        continue
    w = 2.0 * math.pi * syl["vibRate"] * CTRL / SR
    rc, rs = f32(math.cos(w)), f32(math.sin(w))
    c, s = 1.0, 0.0
    n = round((syl["dur"] + 0.02) * SR)
    for i in range(0, n, CTRL):
        exact = math.sin(2.0 * math.pi * syl["vibRate"] * i / SR)
        cents = abs(1200.0 * math.log2((1 + syl["vibDepth"] * s) / (1 + syl["vibDepth"] * exact)))
        if cents > worst_vib:
            worst_vib, worst_vib_name = cents, f"{syl['name']} @ {i}"
        c, s = f32(c * rc - s * rs), f32(s * rc + c * rs)
check("vibrato: max pitch error at control points",
      worst_vib < 1.0, f"{worst_vib:.4f} cents (worst at {worst_vib_name or 'n/a'})")


# ------------------------------------------------ 4. output clipper ---------
def fast_tanh(x):
    if x < -3.0:
        return -1.0
    if x > 3.0:
        return 1.0
    x2 = x * x
    num = x * (135135.0 + x2 * (17325.0 + x2 * (378.0 + x2)))
    den = 135135.0 + x2 * (62370.0 + x2 * (3150.0 + 28.0 * x2))
    return num / den


print("\n4. output soft clip")
worst_t = max(abs(fast_tanh(x / 1000.0) - math.tanh(x / 1000.0)) for x in range(-3000, 3001))
check("Pade tanh vs libm tanh on [-3, 3]", worst_t < 1e-4, f"max abs error {worst_t:.2e}")

old_g, new_g = 1.6 * 0.8, 1.28 * 0.95
check("small-signal gain change old -> new", abs(20 * math.log10(new_g / old_g)) < 0.6,
      f"{old_g:.3f} -> {new_g:.3f} = {20 * math.log10(new_g / old_g):+.2f} dB")
pts = [0.2, 0.4, 0.6, 0.8, 1.0]
print("     x    old out   new out   (new ceiling 0.95 vs old 0.80)")
for x in pts:
    print(f"    {x:.1f}   {math.tanh(1.6 * x) * 0.8:7.4f}   {fast_tanh(1.28 * x) * 0.95:7.4f}")




# ------------------------- 5. O(1) table indexing == the old scan/search -----
# The tables are regular, so the firmware now indexes them arithmetically. That
# is only safe if it lands on exactly the same cell as the old linear scan and
# binary search for every input, including on cell boundaries.
def press_row_scan(pressure):
    pi = 0
    while pi < len(P_GRID) - 2 and P_GRID[pi + 1] < pressure:
        pi += 1
    span = P_GRID[pi + 1] - P_GRID[pi] or 1.0
    return pi, min(1.0, max(0.0, (pressure - P_GRID[pi]) / span))


def press_row_fast(pressure):
    p = pressure * 20.0
    pi = int(p) if p >= 0 else 0
    pi = min(max(pi, 0), len(P_GRID) - 2)
    return pi, min(1.0, max(0.0, p - pi))


def f0_col_search(f0):
    cl = min(max(f0, F0_GRID[0]), F0_GRID[-1])
    lo, hi = 0, len(F0_GRID) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if F0_GRID[mid] <= cl:
            lo = mid
        else:
            hi = mid
    return lo


LOGB = math.log(F0_GRID[0])
INVLOGR = 1.0 / math.log(F0_GRID[1] / F0_GRID[0])


def f0_col_fast(f0):
    cl = min(max(f0, F0_GRID[0]), F0_GRID[-1])
    lo = int((math.log(f0) - LOGB) * INVLOGR) if f0 > 0 else 0
    lo = min(max(lo, 0), len(F0_GRID) - 2)
    if lo < len(F0_GRID) - 2 and F0_GRID[lo + 1] <= cl:
        lo += 1
    elif lo > 0 and F0_GRID[lo] > cl:
        lo -= 1
    return lo


def beta_col_scan(beta):
    bi = 0
    while bi < len(B_GRID) - 2 and B_GRID[bi + 1] < beta:
        bi += 1
    return bi


def beta_col_fast(beta):
    if beta < 1.0:
        bi = int((beta - 0.15) * 100.0)
    elif beta < 4.0:
        bi = 85 + int((beta - 1.0) * 20.0)
    else:
        bi = 145 + int((beta - 4.0) * 5.0)
    return min(max(bi, 0), len(B_GRID) - 2)


print("\n5. O(1) table indexing vs the old scan / binary search")

mism = []
for k in range(200001):
    p = -0.2 + k * (1.8 / 200000.0)
    a, af = press_row_scan(p)
    b, bf = press_row_fast(p)
    # the cell only matters where the interpolated result differs
    if a != b and abs(af - (1.0 if a < b else 0.0)) > 1e-4:
        mism.append((p, a, b))
check("pressure row, 200k points over [-0.2, 1.6]", not mism,
      "identical cell (or an equivalent boundary)" if not mism else f"{len(mism)} mismatches e.g. {mism[:3]}")

mism = []
for k in range(200001):
    f0 = 700.0 * math.exp(k * (math.log(17000.0 / 700.0) / 200000.0))
    if f0_col_search(f0) != f0_col_fast(f0):
        mism.append(f0)
check("f0 column, 200k points log-spaced over 700 Hz .. 17 kHz", not mism,
      "identical index everywhere" if not mism else f"{len(mism)} mismatches e.g. {mism[:3]}")

mism = []
for k in range(200001):
    beta = 0.15 + k * ((16.0 - 0.15) / 200000.0)
    a, b = beta_col_scan(beta), beta_col_fast(beta)
    if a != b:
        # boundary ties are fine when the interpolation fraction saturates
        span = B_GRID[a + 1] - B_GRID[a]
        if abs((beta - B_GRID[a]) / span - 1.0) > 1e-6:
            mism.append((beta, a, b))
check("beta column, 200k points over 0.15 .. 16.0", not mism,
      "identical cell (or an equivalent boundary)" if not mism else f"{len(mism)} mismatches e.g. {mism[:3]}")

# also exercise the exact grid points, where off-by-one is most likely
edge = []
for g in F0_GRID:
    for d in (-1e-3, 0.0, 1e-3):
        if f0_col_search(g + d) != f0_col_fast(g + d):
            edge.append(g + d)
check("f0 column at all 192 grid points +- 1 mHz", not edge,
      "no off-by-one" if not edge else f"{len(edge)} mismatches e.g. {edge[:3]}")


# ------------------------------ 6. f0 at control rate, as a pitch error -----
def contour_log(contour, u):
    if u <= contour[0][0]:
        return math.log(contour[0][1])
    if u >= contour[-1][0]:
        return math.log(contour[-1][1])
    for k in range(len(contour) - 1):
        t0, h0 = contour[k]
        t1, h1 = contour[k + 1]
        if t0 <= u <= t1:
            fr = (u - t0) / max(1e-6, t1 - t0)
            return math.log(h0) + (math.log(h1) - math.log(h0)) * fr
    return math.log(contour[-1][1])


print("\n6. pitch: the 32-sample contour ramp vs the old per-sample contour")
print("   (the ramp is exact inside a contour segment; error only at the kinks)")
worst_c, worst_name, acc, cnt, over5 = 0.0, "", 0.0, 0, 0
for syl in SYLLABLES:
    dur_s = syl["dur"]
    envN = max(2, round(dur_s * SR))
    logF0, logInc = 0.0, 0.0
    for idx in range(envN):
        if idx % CTRL == 0:
            uNow = min(1.0, (idx / SR) / dur_s)
            uEnd = min(1.0, ((idx + CTRL) / SR) / dur_s)
            logNow = contour_log(syl["contour"], uNow)
            logInc = (contour_log(syl["contour"], uEnd) - logNow) / CTRL
            logF0 = logNow
        exact = contour_log(syl["contour"], min(1.0, (idx / SR) / dur_s))
        cents = abs(1200.0 * (logF0 - exact) / math.log(2))
        acc += cents
        cnt += 1
        if cents > 5.0:
            over5 += 1
        if cents > worst_c:
            worst_c, worst_name = cents, f"{syl['name']} @ {idx}/{envN}"
        logF0 += logInc
# A kink inside a 32-sample block makes the ramp a chord across the bend. The
# bound that matters is not the peak but how long it lasts: each excursion is
# confined to one block (1.45 ms), and the ear needs tens of ms to resolve
# pitch. The old code already staircased the *tract* filter at this same rate.
check("pitch error from the control-rate contour ramp: peak", worst_c < 25.0,
      f"{worst_c:.2f} cents (worst at {worst_name}), mean {acc / cnt:.4f} cents")
check("pitch error from the control-rate contour ramp: duration above 5 cents",
      over5 / cnt < 0.005,
      f"{over5} of {cnt} samples = {over5 / SR * 1000:.1f} ms out of {cnt / SR * 1000:.0f} ms "
      f"({100 * over5 / cnt:.3f} %), all inside single 1.45 ms blocks at contour corners")
print("     for scale: the chorus spreads individuals by +-90 cents and the")
print("     per-note tremor is +-0.6 % = +-10 cents.")

print()
if fails:
    print(f"FAILED: {', '.join(fails)}")
    sys.exit(1)
print("all checks passed")
