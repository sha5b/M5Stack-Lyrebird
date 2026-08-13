# HANDOFF — M5Stack-Lyrebird

What is on disk, as of 2026-08-13. Current state only: git carries the history, and a
handoff that narrates its own sessions ends up correcting itself further down, which
leaves a reader unable to tell which half is true.

## What this is

An embedded port of the [Lyrebird](../Lyrebird) bird-chorus app for the M5Stack Fire
(ESP32) and CoreS3 (ESP32-S3), plus a browser webflasher copied from
[CYD-Physarum](../CYD-Physarum). Everything is synthesized on the device by the
Mindlin–Laje syrinx model. No samples, no filesystem: the bird data is compiled into the
firmware as PROGMEM headers.

## Current state

- Both envs build: `pio run`, which is where these figures come from —

  | env | flash | static RAM | + heap | band |
  |---|---|---|---|---|
  | `m5stack-fire` | 1.79 MB of 3 MB | 68.6 KB | 89 KB canvas | songs and struck crosses |
  | `m5stack-cores3` | 1.78 MB of 3 MB | 65.7 KB | 89 KB canvas | songs and struck crosses |

  The corpus is `rodata` and is read where it lies, which is why 4.0 MB of generated C
  costs no RAM at all. Both boards draw the band now, so both pay the same 36.8 KB for
  it — `s_trail` alone is 37120 bytes, 32 tags x 72 points — and the band's statics are
  byte-identical across the two ELFs, so the ~3 KB between the boards is their own
  drivers and not the picture. The canvas is on top of that and is allocated at runtime,
  so it is not in the build figure; it logs the allocation and the free heap over serial
  at boot.
- `./scripts/build-firmware.sh` writes `lyrebird-fire.bin`, `lyrebird-cores3.bin`, a
  per-board `manifest-<id>.json` and one `firmware.json` listing both, into
  `web/static/firmware/`.
- Webflasher: `cd web && npm run check` and `npm run build` (static site into
  `web/build/`).
- **Hardware: both boards run this, and there is a recording of them doing it.**
  `web/static/lyrebird-boards.mp4` is 28 s of a Fire and a CoreS3 side by side on the
  all-birds slot, 1280 x 720 at 30 fps with an audio track — the two of them, on a desk,
  not a render. It is the page's hero and the README's, and it is the only artefact in
  this repo that is evidence rather than description.

  Read off its frames, which is where these figures come from:

  | | Fire | CoreS3 |
  |---|---|---|
  | header | `ALL BIRDS`, `1/2424  chorus  24 birds` | the same |
  | `dsp` | 1 % with nothing sounding | 21 % with one voice |
  | volume | 70 % | 100 % |
  | bottom strip | the hint line over three real buttons | three labelled touch zones |

  So the 2424-slot dial, the header block, the band and the touch strip are all live on
  both boards, and the CPU-headroom worry is retired twice over — the crackle line is
  around 90 %.

  **What the clip does not settle.** The sound on it has not been measured, only made:
  whether the Fire's 8-bit noise floor, the crackle work and the idle-hiss work below hold
  up is a judgement by ear, and nothing here is a measurement of the analog output. Two
  earlier observations are also still open — while the standing lattice existed it
  registered on the Fire and not on the CoreS3, which if it was not simply a stale image
  says **the CoreS3's panel does not resolve a blue of 1–4 of 31 the way the Fire's does**.
  The lattice is gone, so settling that now needs something else faint to compare.
- **The merged images in `web/static/firmware/` are built by hand and go stale silently.**
  They are not a build product of `pio run`, so a `pio run` that succeeds proves nothing
  about what the page will flash. This has already cost one confusing session: the CoreS3
  was flashed with an image predating `src/galaxy.cpp` and came up with the spectrogram,
  which looks exactly like the galaxy not working. Run `./scripts/build-firmware.sh`
  after touching firmware, and check the timestamps before believing a symptom.

  As they stand on disk they are current — merged after the last edit to `src/galaxy.cpp`
  and `src/ui.cpp`. `firmware.json` says `3178822-dirty`, which is the tree the script was
  run against: the version string names the commit the build *followed*, not the commit it
  will land in, so a `-dirty` version there is normal and not a warning.
- **`tools/verify_syrinx.py` fails on the shipped corpus.** See "Verification" below.
  This is the one thing to look at before anything else.

## The corpus — 2423 species, not twelve

`include/bird_data.h` is generated from the parent project's
`data/learned-inventory-final.json`: 2423 species, 12724 syllables, 2442 songs. The
authored twelve are the first twelve entries; the rest are pipeline-fitted. The header's
banner names its source, and `tools/generate_assets.py --inventory ...` reproduces it
byte-for-byte.

`assets/inventory.json` vendors only the authored twelve, so the default
`python3 tools/generate_assets.py` builds a *different, smaller* firmware. Both are
valid. Which one ships is an owner decision, and the consequences of the corpus are:

- The dial is `SPECIES_COUNT + 1` = 2424 positions, stepped one press at a time with no
  search. A named species is not reachable by hand any more; the all-birds slot is how
  the corpus gets heard.
- The all-birds roster had to become a *sample*. Two individuals of every species is
  4846 `Individual`s = 132 KiB of DRAM, which overflowed the data segment outright, and
  bought nothing: eight voices and six song players mean all but a handful could never
  sing. It is now 12 species drawn at random, two each, rolling over about every four
  minutes (`ALL_BIRDS_SPECIES`, `ROSTER_ROLL_CHANCE` in src/chorus.cpp).
- `Individual::species` is `uint16_t`; the corpus is far past 255.
- The DSP shortcuts were validated against the authored 44 syllables and were never
  re-checked against the corpus. They do not hold. See "Verification".

## Layout

```
platformio.ini            envs m5stack-fire (ESP32) and m5stack-cores3 (ESP32-S3),
                          both on m5stack/M5Unified@^0.2.19, -O2 over the default -Os
partitions.csv            factory-only, 3 MB app, no OTA, no data partitions
assets/                   vendored from Lyrebird: inventory.json (the authored 12) +
                          syrinx-calibration.json (beta/ampPP) — CC BY-NC-SA 4.0
tools/generate_assets.py  inventory JSON -> include/bird_data.h + include/calibration.h
tools/generate_galaxy.py  include/bird_data.h -> web/src/lib/corpus.ts +
                          include/galaxy_data.h
tools/preview_galaxy.py   draws the band to a PNG on the host
tools/verify_syrinx.py    host check of the DSP rework, no dependencies
include/syrinx.h
src/syrinx.cpp            the synth port
include/audio.h           one API, two backends
src/audio_dac.cpp         Fire: I2S0 built-in DAC mode + render task on core 1
src/audio_spk.cpp         CoreS3: 16-bit PCM into M5.Speaker
include/chorus.h
src/chorus.cpp            Poisson scheduler + individual-bird roster + song players
include/ui.h
src/ui.cpp                the chrome, and the band: arms, or the sweep by flag;
                          GAP sets the band's equal margins
include/galaxy.h
src/galaxy.cpp            the band — beaded songs, struck crosses, following camera
include/buttons.h
src/buttons.cpp           three logical buttons over physical or touch
src/main.cpp              M5 bring-up, button semantics, frame pacing
scripts/build-firmware.sh pio build + esptool merge_bin + manifests
web/                      SvelteKit + esptool-js flasher (from CYD-Physarum)
web/src/lib/galaxy.ts     the page's backdrop; corpus.ts is its generated data
web/src/lib/seo/          title, card, JSON-LD graph, og:video
web/static/               icons and og-card.png from the parent's brand set, robots.txt,
                          sitemap.xml, llms.txt, and lyrebird-boards.mp4 — the clip of
                          both boards, with its poster and caption track
docs/band-preview.png     the band from tools/preview_galaxy.py, for the README
```

## The galaxy — the page's backdrop and the board's arms

Both screens are arranged by the same layout and draw different things with it.

**The layout happens once, in `tools/generate_galaxy.py`**, which reads
`include/bird_data.h` and writes positions to `web/src/lib/corpus.ts` (base64, 54 KB
packed) and `include/galaxy_data.h` (PROGMEM, 54 KB of flash — four bytes a mark, plus a
4.8 KB per-species index inside that figure). Re-run it whenever `generate_assets.py` runs. Every species gets an island on a
Fibonacci sphere; inside it, pitch, duration and contour sweep place each syllable.

It used to lay out in TypeScript, which would have meant a second copy of the same maths
in C++. That was not only a promise no repo keeps — it was impossible: the mixing hash
reads perfectly well in JavaScript and is *not reproducible* there, because
`h * 0x2545f491` exceeds the 53 bits a double holds exactly. Positions, not features, is
what makes "the board and the page use the same layout" a fact rather than an intention.

**The page draws the whole corpus.** 12724 marks in graphite at low alpha, depth
darkening them, three islands answering at a time in `--signal`. Measured in Chromium at
1440×900 DPR 2: 60 fps, which needs the alpha bucketing in `draw()` — a `fillStyle` per
mark is 12724 colour parses a frame and was the whole budget. `prefers-reduced-motion`
draws one static frame and never schedules another; a hidden tab cancels the loop. The
narrow-viewport case is the awkward one, since a phone has no margins for a cloud and
every mark lands under a line of type: handled by sizing off the window *diagonal*
(constant marks-per-area whatever the shape) and dropping the canvas to 55 % opacity
below 48rem.

**The board draws a camera inside the corpus, following whoever is singing.** The band
is 310 × 147 px, and that constraint is the whole design. Four versions, and the first
three failed the same way in smaller and smaller forms — marks sitting there not meaning
anything:

| | what it drew | why it failed |
|---|---|---|
| 1 | 12724 marks accumulated into a brightness buffer | at that size twelve thousand marks is a texture, and the bird actually singing was four pixels lost inside it |
| 2 | the same marks, dim, as a bed under the songs | a ground made of twelve thousand dots on a 147 px band is not a ground, it is grain |
| 3 | the roster as a constellation — two dozen dots, faded by how recently their bird sang | a dot on its way out still reads as a dot doing nothing, and a dozen of them is still a scatter that never changes |
| 4 | a faint world-space lattice under the songs, to give the camera something static | another permanent thing that does not mean anything — the roster's fault one step further from the birds |
| 5 | songs only: beaded threads, struck crosses, a following camera, black between songs | — |

**So there is nothing permanent on the band at all.** Nothing is drawn for a bird until
it sings, and between songs the band is black, which is the correct picture of nothing
happening. Trails last about three seconds and songs arrive about every two, so it is
rarely empty for long. The idea the roster and the lattice were both there to serve — a
camera with nothing static in frame is a camera you cannot see moving — lost to the idea
they both broke: everything on this band is something that is happening.

The song: a chain of **stretched dots** growing out of its own bird's place, each
one a `drawWedgeLine` with its own radius at each end, so weight follows the envelope —
loud syllables swell the thread, quiet ones pinch it — with a brighter bead over the newest
dot. That is the parent's `shaders.ts` device (`uPointSize * (0.85 + vPeak * 2.4)`: a swell
says where the note *is*, more precisely than a thickening line) and its `ribbon.ts` finding
(a one-pixel thread disappears; a song needs real width).

The **camera** is `Framing.svelte` scaled to a panel, and it **frames the set rather than
picking a favourite**. Every bird that has sounded within `FRAME_HOLD_MS` is in the shot;
the target is their centroid and the distance is solved so what is drawn fills `FIT_FRAC` of
the band — in screen space, on both axes, see below — clamped to `[CAM_DIST_NEAR,
CAM_DIST_FAR]`. One rule covers both cases that
matter: one bird has a spread of zero, clamps to the nearest shot and gets the big single
gesture; two singing at once are shown *together* instead of the camera choosing one and
losing the other off-frame. It follows heads rather than islands, so threads stream away
behind, and eases at `CAM_EASE` — about 1.5 s to arrive, because a cut on this screen reads
as a glitch.

**The yaw is steered, not just driven.** A song is mostly a line through space, and a line
seen end-on is a dot — so a purely clock-driven drift spends part of every cycle collapsing
the most legible thing on the screen into the least. The yaw therefore has a target as well
as a rate: the angle that puts the loudest bird's thread across the view rather than along
it, which is one `atan2` on the thread's horizontal direction. Of the two solutions π apart
it takes the shorter turn, since the other is a 180° swing to an identical-looking shot. The
clock pushes away and the steering pulls back, so the camera hovers near broadside with a
slow wobble. Only the yaw: a thread that is vertical in world terms is already broadside on
the screen's other axis, and steering both would leave nothing moving.

It stays accurate under all of it. Every mark is a measurement: position is the species'
place in the corpus, distance from the middle is the f0 being sung on the spectrogram's own
log axis, weight is the envelope, hue is the individual — the same hue the sweep gives it.

Threads are geometry, not smears: points are kept in layout space and re-projected every
frame, so the camera can travel through a song already sung. Everything goes into an
`M5Canvas` and is pushed whole, which is what stops a moving line from flickering — 89 KB
of DRAM allocated at runtime, and if it will not allocate the band stays black and says so
over serial rather than crashing.

`tools/preview_galaxy.py` re-implements the whole model on the host — camera, focus,
easing, wedge dots — and writes a PNG. Three values were plainly wrong before it: `GROW` at
0.016 (left over from version 1) made every song a 15 px scribble; `TWIST_RATE` at 0.42
closed each thread into a loop, so the depth cue became the shape; and version 2's bed put
a lone syllable at RGB (0, 2, 34), which on a dark panel is black — the same 565 rounding
mistake the lattice went on to make, caught here first.

**It is behind the firmware in one place**: it still solves the distance as a world radius
against the band's half-height, which is what `src/galaxy.cpp` replaced with the
screen-space measurement below. So composition transfers and framing no longer does
exactly. Its docstring says so.

**There is one grid, and it exists only where something is singing.** It is GridBurst's:
each new song lays a cross of three axis-aligned dashed rules through the bird that started
it, rising over ~0.15 s and gone by ~1.8 s.

**A standing world-space lattice was tried under it, and is gone.** The argument for it was
sound on paper — the parent rejected a grid twice, but both objections (*a flat lattice over
the whole window does not move with the cloud, so it fights the depth it is supposed to
establish*, and a ground plane is *a permanent floor under a corpus that has no floor*) are
about a screen-space lattice or a floor, and a world-space cage snapped to the camera is
neither. It rotated, it parallaxed, and it gave the camera something to move against.

It went anyway, on the owner's look, and the reason is the one this band keeps teaching: a
permanent thing that does not mean anything is the same fault as the roster, one step
further from the birds. It also cost two rounds of work first — hand-picked RGB565 levels,
because scaling a dark grey-blue by depth rounded it to black on the panel; then a per-frame
line count, because a fixed cage ran out inside the frame at the far end of the dolly. Both
were real fixes to a thing that should not have been there. The code is out of
`src/galaxy.cpp` and `tools/preview_galaxy.py` entirely, along with `projectFront()`, which
existed because lattice lines pass through the camera and nothing else in the picture does.

One finding survives it and is worth keeping: **the preview puts every colour through 565
the way the hardware does.** It drew that invisible grid beautifully in 24-bit, which is how
a mark that rounds to black on the panel got shipped. If a colour would round to black on
the panel, the preview now shows black.

Three details of the struck cross are load-bearing, and all three come from GridBurst:

- **The reach is quoted against the view, not the world.** `STRIKE_REACH` is 1.0 of the
  band's *diagonal*, in pixels; the rules' directions are world axes, so they turn with the
  corpus, but their length does not change when the camera dollies. A fixed world length is
  only right at one zoom, and this camera moves. The diagonal rather than the half-height
  because a rule that stops inside the picture is a cross sitting on a dot, not a grid line
  — it has to leave the frame from anywhere in it, at any angle.
- **The colour is held at strike time.** A cross outlives its note by more than a second,
  and re-resolving the hue per frame would have it reporting on a bird that had stopped.
- **One cross per bird.** A bird that starts another song refreshes its own strike rather
  than adding a second set of rules through nearly the same place, which reads as one badly
  drawn cross. Six live strikes, which is more than the eight-voice pool can usefully start
  at once.

They are also faint on purpose: at full strength the scaffolding overlaid the song it was
supposed to be a reference for, so `STRIKE_AMP` holds it at 30 % and the dashes are sparse
(2 on, 7 off).

**The camera dollies as well as pans.** `CAM_DIST_NEAR` while anything is singing,
`CAM_DIST_IDLE` between songs, eased slower than the pan so a push-in reads as a push-in
rather than as the picture changing size.

**The fit is measured in screen space, on both axes, over the current song only.** Two
corrections live in that sentence. Solving it as a world radius against the half-*height*
meant a broadside gesture — which is exactly what the yaw steering works to produce — was
fitted to the short axis of a 310 x 147 band and used a third of the long one. And measuring
over every live trail point included the *previous* song, still sitting in the ring a long
way off, which drove the distance to nearly twice what it wanted and shrank a growing song
instead of following it; the walk now stops at the first `brk`. The cost of screen space is
that it is a feedback loop on last frame's distance rather than a closed form, which at these
easing rates is invisible.

So a single song eases the camera back as it grows — the gesture keeps filling the frame
instead of running off it — and that is the same mechanism that fits two birds together.

**Zoom is set so activity owns the screen.** `ZOOM` went 78 → 100 → 145 and
`CAM_DIST_NEAR` 2.0 → 1.5, with `GROW` doubled to 0.10. At the earlier values a song was a
small squiggle in a large dark rectangle, which is exactly what the band is for *not* being.
A median song now sweeps most of the way across; long ones run off the frame, which is fine
because the camera holds the head and the head is the part worth seeing.

Two more things: the cross **follows its bird** rather than staying where the song started —
GridBurst had to make the same change once its knots could move, since a strike that keeps
old coordinates ends up ruling lines through empty space — and the head bead has a **halo**,
three concentric dimming spots under it. With no bloom pass to spend on a panel, that is the
whole trick for making a mark read as a light.

**The band has now been seen on both boards** — `web/static/lyrebird-boards.mp4`, see
"Current state". The threads read as threads and the bead reads as a light at 310 x 147 on
a real panel, which is the thing the preview could not answer. What it still cannot answer
is the faint end: the lattice registered on the Fire and not on the CoreS3 before it was
removed, so how dark a mark that panel can hold is unmeasured.

`chorusSpeciesForTag()` connects a voice to its island, and can be stale by one bird: a
song player holds a *copy* of its bird, and `rollRoster()` can replace a roster entry
while that copy is still singing. It declines to evict a bird that is mid-song, which
closes the common case and not the racy one. The cost is one wrong arm for one song.

## The screen's layout, and why the header carries everything

Text used to alternate with picture down the screen — header bar, name, sub-line, band,
info line, volume bar, hints. That cost the band 40 px and left the name floating between
two rules with no rank in particular.

Now every piece of text is in one filled 36 px block at the top, in falling order of
importance: the bird's name at size 2, then slot / mode / roster with the two load figures
opposite, then volume as the block's own 2 px bottom edge rather than a widget. The band
gets 149 px instead of 124.

**The band is the same distance from the text above it as from the buttons below it** —
`GAP` in src/ui.cpp, 12 px on both sides. It used to be 2 above and 13 below, which reads
as the picture being badly placed rather than as a hierarchy. Symmetry costs the band 9 px,
and 12 is nearly the largest gap available: everything between the volume edge (38) and the
strip (211) is gap, band, gap, so each px of gap costs the band two, and the band still has
to end above `TOUCH_HIT_Y` (200 in include/buttons.h) — the touch target is deliberately
taller than the drawn strip, so a band running past that line would turn a tap on the
picture into a button press. The band runs 50 to 198, and the plot inside it is 310 × 147.

`drawReadouts()` repaints the two live figures every frame straight over themselves, with
an opaque background and a fixed-width format — right-aligned text that shrinks would
leave the tail of the longer string behind it.

## Key decisions (and why)

- **Audio (Fire): I2S0 in built-in DAC mode, DMA-paced at 22050 Hz** — not the
  DeDeNoise-Mashine `dacWrite` + `delayMicroseconds` loop (11025 Hz, jittery; fatal for
  a pitched synth). The speaker amp is analog-in off DAC1 = GPIO25, the I2S "left" DAC
  channel; M5Unified has this board as `use_dac = true, pin_data_out = GPIO_NUM_25`. It
  is **not** an NS4168 — that is an I2S part, and early comments here named it wrongly.
  The DAC reads the unsigned MSB of each 16-bit DMA slot, so mid-level is `0x8000` and
  `i2s_zero_dma_buffer` is a *pop* rather than silence; that is why `primeMidLevel()`
  exists. When paused: DAC disabled, GPIO25 high-Z.
- **Audio (CoreS3): `M5.Speaker`, not raw I2S.** The ESP32-S3 has no DAC, so the Fire's
  backend is inapplicable rather than merely different. M5Unified owns the AW88298's I2C
  bring-up and its AW9523 reset line, and guessing at those registers gets you a silent
  board with no way to tell why. Pacing comes from `M5.Speaker.isPlaying(ch) >= 2` — the
  two wave slots are the backpressure, the job `i2s_write` blocking does on the Fire.
  `magnification` is forced to 1; M5Unified defaults it to 4 here and our source is
  already full-scale.
- **Bootloader offset differs by chip**: 0x1000 on the ESP32, **0x0** on the ESP32-S3.
  Getting it wrong produces an image that flashes cleanly and never boots. Encoded per
  board in scripts/build-firmware.sh.
- **Synth scope: only what the inventory uses.** Ported: envelope path, timbre classes
  (pure/reed/buzz), attack/hold, AM, vibrato, harmonic band, detuned second side
  (`two`), calibration-table beta/ampPP lookups. Not ported: respiration, coupled
  voices, formants, noise, rough, fricative. float32; no parity with the browser harness
  is claimed. 8 voices (`SYRINX_MAX_VOICES`).
- **Chorus: port of individual.ts + startAmbient.** Pitch and vibrato spread from the
  golden-ratio low-discrepancy sequence, secondary traits from mulberry32 over an FNV
  hash of `speciesIdx#index` — deterministic, same birds every boot. Poisson arrivals,
  12 songs/min on a species and 34 on all-birds, 25 % conspecific duet answers
  0.4–1.3 s later, 0.6 % per-note tremor. Pan dropped (mono speaker);
  distance/reverb/limiter master chain not ported.
- **Buttons** (owner decision): A/C short = previous/next dial position, A/C hold =
  volume ∓0.02 on a 120 ms repeat, B short = chorus ↔ solo, B hold 1 s = pause/resume.
  Position 0 is all-birds and boots there; B is a no-op on it, because that slot is a
  chorus by definition.
- **Flasher: CYD-Physarum's `web/` stripped** — esptool-js over Web Serial, a single
  merged 0x0 image, no config sector and no boot report. Deploys via `web/railway.json`
  (RAILPACK, `npm run start` on sirv), same as CYD.

## Verification — and what it now says

`python3 tools/verify_syrinx.py` re-implements the changed DSP math in Python, parses
the real calibration and bird tables out of `include/`, and checks each shortcut against
the closed form it replaced. It is not a waveform diff of old against new — that needs a
host C++ compiler, which this machine does not have.

It used to `assert len(SYLLABLES) == 44`, so against the committed 2423-species header
it died on that line before running a single check. With the assertion relaxed to the
generated header's actual size, four checks fail on the corpus and all of them pass on
the authored 44:

| check | authored 44 | corpus 12724 | budget |
|---|---|---|---|
| incremental envelope vs closed form | 6.2e-05 | **0.313** | 5e-4 (DAC LSB is 3.9e-3) |
| AM pressure factor vs libm | 8.0e-05 | **2.6e-04** | 1e-4 |
| contour ramp, peak pitch error | 17 cents | **466 cents** | 30 cents |
| contour ramp, time above 5 cents | 0.105 % | **1.012 %** | 0.5 % |
| vibrato, Padé tanh, O(1) table indexing | pass | pass | |

The corpus reaches parameter ranges the authored twelve never did — much longer
syllables (the worst envelope case is 1296 control blocks in) and steeper contour
corners. The envelope one is the serious figure: 0.313 is 31 % of full scale and 80 DAC
LSBs, not a tolerance quibble. The pitch peaks are single 1.45 ms blocks at contour
kinks with a 0.2-cent mean, which is a different and milder kind of wrong.

**Not fixed here** — this was a documentation pass and these are behaviour. Decide
first whether the corpus ships at all; if it does, the envelope is the bug to chase.

## The crackle — four causes, all fixed in current code, none confirmed by ear

1. **CPU overrun, the main one.** The old inner loop called `expf` twice, `cosf` once
   and `tanhf` once per sample per voice, then walked two calibration tables: a
   192-entry binary search in `betaForF0`, and a linear scan in `ampPP` that restarted
   at index 0 every call and ran ~85 steps to reach beta ≈ 1. At eight voices that is
   >100 M table steps a second on a 240 MHz core, and a render task that misses the DMA
   deadline hands the DAC a late buffer, which is a click. Fixed by exploiting the
   tables' regularity — `CAL_INV_F0_GRID` is geometric, `CAL_PRESSURE_GRID` uniform,
   `CAL_BETA_GRID` uniform in three runs, so both lookups are O(1) — and by making the
   envelope, the AM oscillator and the pitch contour incremental. No libm call is left
   in the sample loop.
2. **`tx_desc_auto_clear = true`.** On a late refill the driver handed the DAC 0x0000,
   which in unsigned-MSB terms is not silence but the full negative rail: every underrun
   was a bang. Now false, so the DMA repeats the previous buffer.
3. **8-bit truncation.** `(int)(s * 127.0f) + 128` truncates toward zero — a
   double-width step across zero that every decay tail passes through as granular buzz.
   Now rounds, with ±0.5 LSB TPDF dither and first-order noise shaping.
4. **A race on `s_dacBuf`.** `audioSetRunning` called `primeMidLevel()` from loopTask
   while audioTask could still be inside `i2s_write` on the same buffer. There is an
   idle handshake now (`s_idleAck` / `waitAudioIdle`).

Supporting changes: DMA buffers 4 → 8 (~93 ms of slack); audio task priority 1 → 3 so an
LCD repaint cannot delay a refill, still pinned to core 1 on purpose because the task
watchdog watches IDLE0 and an overrun on core 0 would reboot the board; `-O2` over the
framework's `-Os`.

## Idle hiss on the Fire — three causes, same status

1. **The dither ran over silence.** TPDF dither and noise shaping are right where there
   is signal to decorrelate and wrong everywhere else; over digital silence they are a
   continuous hiss, and in chorus mode most of the wall clock *is* silence. A block that
   is exactly zero now gets the flat mid-level code and nothing else.
2. **Backlight PWM.** `M5Display::begin()` ran a 44.1 kHz LEDC PWM on GPIO32 to dim the
   screen — tens of milliamps switching next to GPIO25, a high-impedance analog DAC
   output feeding an amplifier. Brightness is pinned at 255, where the duty saturates and
   the pin stops switching. `BACKLIGHT` in src/main.cpp is the experiment: if the hiss
   changes between 255 and 120, it is the backlight and not the DAC.
3. **A pin conflict.** `audioSetRunning` called `pinMode(25, OUTPUT)` *and*
   `dac_output_enable()` on the same pad, pointing the GPIO matrix and the RTC analog
   path at one pin and letting call order decide. `i2s_set_dac_mode()` configures the pad
   by itself now, plus the `i2s_set_pin(I2S_NUM_0, NULL)` that was missing.

If hiss remains *during* notes it is the 8-bit floor, and the lever is
`AUDIO_SAMPLE_RATE` (documented in include/audio.h). Left at 22050 because that is the
rate the port was measured at. `dsp NN%` bottom right is the budget meter: past ~90 % the
render task is about to miss the DMA.

## The CoreS3 cannot be flashed from a running board

Diagnosed on real hardware, not reasoned about. The CoreS3 has no UART bridge, so it is
**two different USB devices** depending on what it is running:

| state | USB id | what it is |
|---|---|---|
| running its firmware | `303a:811a` "M5Stack Core S3" | the application's USB CDC |
| download mode | `303a:1001` "USB JTAG/serial debug unit" | the ROM bootloader |

Only the second can be flashed. On a board with a bridge (the Fire) the flasher pulses
DTR/RTS, the chip resets into its ROM, and the USB device is untouched because the bridge
is a separate chip. Here the reset **replaces the USB device**: the handle the browser was
granted stops existing halfway through the connect. That is exactly the "restarts
constantly and never answers" report.

Measured: with the app running, `esptool.py --before default_reset` sat at "Connecting..."
and ended in `Write timeout`, and the USB id stayed `303a:811a` throughout — the board
never entered download mode at all. After a 2–3 s hold of the side reset button (green LED
on, then release) the id changed to `303a:1001`, `chip_id` answered immediately with
`--before no_reset`, and `write_flash 0x0 lyrebird-cores3.bin` finished with "Hash of data
verified".

**And it fails only once.** Our build sets `ARDUINO_USB_MODE=1` from the board definition,
so Lyrebird uses the chip's hardware USB-Serial-JTAG rather than TinyUSB, and that
peripheral keeps its USB identity across a reset — verified by reflashing with plain
`--before default_reset`, which succeeded and left the id at `303a:1001` throughout. The
download-mode hold is a one-time step off the factory firmware, not a workflow. The page
says so.

In the code: `isEspressifAppPort()` in web/src/lib/installer.ts refuses to connect to
`0x303a` with a product id other than `0x1001` and says what to do, rather than letting
the user watch the board reboot. **Do not "fix" this with a longer reset sequence.** It is
not a timing problem.

Two related facts: our own CoreS3 firmware also appears as `303a:1001`, so the product id
distinguishes "ROM bootloader" from "factory firmware" but not from "our firmware
running". And the Fire on this desk is a v2.7 — it enumerates as `1a86:55d4` (WCH CH9102),
not the CP2104 that older units and the page's copy assume. Both ids are in the filter
list, so nothing is broken.

## The reset sequence — do not "improve" it again

Measured on the Fire (CH9102, `1a86:55d4`), boot banner read back over the same UART,
`waiting for download` versus `SPI_FAST_FLASH_BOOT`:

| sequence | download mode |
|---|---|
| `D0\|R1\|W100\|D1\|R0\|W50\|D0` — esptool-js stock | 4/4 |
| `D0\|R1\|W250\|D1\|R0\|W50\|D0` — longer EN low | 4/4 |
| `D0\|R1\|W100\|D1\|R0\|W250\|D0` — longer IO0 hold | 4/4 |
| `D0\|R1\|W200\|D1\|W40\|R0\|W450\|D0` — "ours SHORT" | **0/3** |
| `D0\|R1\|W500\|D1\|W80\|R0\|W900\|D0` — "ours LONG" | **0/3** |

The two that fail are the two this project shipped for a while. The only timing that
matters is the wait between `D1` and `R0`.

The reasoning that produced them was wrong about the circuit. This is the classic
two-transistor auto-reset, where EN and IO0 come from the *combination* of the two lines,
not one line each:

```
DTR RTS -> EN IO0
 0   1      0   1    reset asserted
 1   1      1   1    reset released, IO0 still high
 1   0      1   0    IO0 low
```

`D1` releases EN and `R0` pulls IO0 low, so a "settling wait" between them does the
opposite of what it sounds like: it hands the chip 40 ms of EN-high with IO0 still high,
which is exactly long enough to latch the strapping pin and boot the application. The
library's own sequence was right.

## No timeout around connect, either

`withTimeout` around `ESPLoader.connect()` could not cancel anything. Losing the race left
esptool-js's promise running: it worked through its seven attempts and held the serial port
open while it did. The port stayed claimed after we had reported failure and torn down, so
the next attempt — and any other program, including esptool.py in a terminal — got
`Could not open /dev/ttyACM0, the port is busy or doesn't exist`. It was unnecessary as
well: `connect()` is already bounded at seven attempts of reset plus five 100 ms sync
reads, so it settles by itself.

Related: a failed connect calls `Installer.forgetPort()`. A Web Serial port is a handle to
one USB device, and both a browned-out board and an ESP32-S3 leaving its application for
its ROM invalidate it while leaving the handle non-null — every retry then fails against
something that no longer exists.

## Power — the pause timeout, and the Fire's red button

**Paused for a minute and the board goes dark** (`PAUSE_OFF_MS` in src/main.cpp). It says
why first, because a screen that blanks on its own is indistinguishable from a flat battery
or a crash.

What "off" means is not the same on both boards, and is not this code's choice:

| board | PMIC | `M5.Power.powerOff()` does |
|---|---|---|
| CoreS3 | AXP2101 | cuts the rail. Actually off. |
| Fire | IP5306 | releases the boost keep-on and **returns** |

**There is no deep sleep here, and that is a bug fix rather than a preference.** The first
version called `powerOff()` and then `deepSleep(0, true)`. On the Fire that pair fights
itself — `powerOff()` sets boost keep-on *false*, then `deepSleep()` sets it *true* again —
and, far worse, a deep sleep ends in a **reboot**, which runs `setup()` and starts the
chorus. So a paused Fire went quiet, slept, took any wake on its wakeup pin, and came back
*singing*. Reported from the outside as "I paused it and after a while it started again;
only the second pause works", which is the same race landing the other way.

So `sleepNow()` silences the audio, blanks the screen, sets the backlight to 0 and calls
`powerOff()`. On the CoreS3 nothing after that runs. On the Fire it returns and the board
stays put — dark, paused, not rebooting. A button release brings the screen back and the
chorus is still paused, because nothing ever restarted it. The first press after waking is
swallowed on purpose: waking into a volume change because of the press that woke you is
worse than needing two presses.

Related, and worth knowing before touching that code: `Button_Class::pressedFor(ms)` is
`_press && (_lastMsec - _lastChange >= ms)` — **true on every pass** once the threshold is
crossed, not once. `bHoldHandled` is what makes the B hold one toggle, and there is now a
400 ms guard behind it as well, so pause and resume cannot land inside one gesture whatever
the button layer does. Both transitions log to serial.

**The Fire's red button is a short press twice, not a long press.** This is the answer to
"does the power button work" and it is not our code at all — M5Unified programs the IP5306
at `M5.begin()`, and in `reg01h` (SYS_CTL1) bit 7 selects the shutdown gesture:

```
0x01, 0b00011101
        ^ bit 7 = 0 -> "Short press twice"   (1 would be "Long press")
```

`reg00h` bit 0 confirms push-button shutdown is enabled at all, and `reg02h` sets a 64 s
light-load auto-shutdown. So: double-tap the red button, on battery. Holding it does
nothing, and holding it while plugged into USB does nothing twice over. Nothing in this
firmware changes any of that — but it is worth knowing before spending an evening on it.

## Buttons on a board that has none

`src/buttons.cpp` puts three logical buttons in front of both boards: physical
`M5.BtnA/B/C` where they exist, and three drawn zones along the bottom of the screen where
they do not, fed into `m5::Button_Class::setRawState()` so the debounce and hold thresholds
behave identically. `src/main.cpp` has no board case in it as a result.

M5Unified does map a strip below the CoreS3 screen onto BtnA/B/C, and an earlier version
leaned on that. Two reasons it was not enough: it depends on the touch digitizer reporting
coordinates past the bottom of the display, which was never verified here, and an
unlabelled control surface is a poor one regardless. The zones carry their hold action on a
second line, since a hold is otherwise undiscoverable. The touch target is deliberately
larger than the drawing — `TOUCH_HIT_Y` (200) sits above the 29 px visible strip, so the
area that responds is about 40 px tall.

## Adding an audio module

M5Unified already knows the M-Bus pins for the stackable Module Display and Module RCA,
behind `cfg.external_speaker.module_display` / `.module_rca` — a module is a config flag,
not a set of pins to type in. Module Audio is an ES8388 at I2C 0x10 and ships two pin
configurations, one for Basic/Core2 and one for CoreS3. If modules are added, the honest
choice is a build per board+module rather than a config sector: the pins are compile-time
in M5Unified anyway, and the website already asks which board you have.

## Building & flashing

```bash
pio run -e m5stack-fire   -t upload
pio run -e m5stack-cores3 -t upload

./scripts/build-firmware.sh          # merged images + manifests into web/static/firmware/
esptool.py --chip esp32   write_flash 0x0 web/static/firmware/lyrebird-fire.bin
esptool.py --chip esp32s3 write_flash 0x0 web/static/firmware/lyrebird-cores3.bin
```

Regenerating the bird data needs the parent repo beside this one:

```bash
python3 tools/generate_assets.py --inventory ../Lyrebird/data/learned-inventory-final.json
```

Headers are committed; the script is only re-run when the inventory changes.

## The page's identity — icons, SEO, and the one thing not settled

The address is **`https://m5lyrebird.variable.gallery`**, in `SITE_URL`
(web/src/lib/seo/index.ts) and repeated as literals in `web/static/robots.txt` and
`sitemap.xml`, which cannot import anything. It replaces the parent project's URL, which
this page used to claim as its own canonical — so the canonical link, `og:url` and every
JSON-LD `@id` said "browser instrument" while serving a firmware flasher. **The deploy does
not exist yet**; the DNS has to be pointed at Railway for any of it to resolve.

The icons are the parent's, copied from `../Lyrebird/app/static` and
`.../static/brand`: `favicon.svg`, `favicon.ico` (16/32/48), `apple-touch-icon.png` (180),
`icon-192.png`, `icon-512.png`. They replace CYD-Physarum's, which this repo was still
serving. The mark is a syrinx dividing into two unequal bronchi — the same mechanism the
firmware integrates — so it is the same mark on the board's page as on the instrument's.
Order in `app.html` matters: a browser takes the *last* icon it understands, so the SVG is
first and `alternate icon` carries the fallback.

`web/static/og-card.png` is generated by `tools/make_og_card.py`, which **reads the figures
out of `include/bird_data.h` and `include/galaxy_data.h`** rather than repeating them, so
the card cannot end up quoting a corpus the firmware does not sing. It needs Pillow. Two
things to know before regenerating it:

- The type is a substitute. The house pairing is IBM Plex with a bookish serif for the
  wordmark; none of those are installed here, so it draws Noto Serif and Liberation
  Sans/Mono. `FONTS` at the top of the script is where that is fixed.
- The parent's own `og-card.png` was **not** reused, and must not be: it carries the
  browser instrument's figures (444,213 syllables, 2,650 species), which are not this
  firmware's.

`web/static/lyrebird-boards.mp4` (10 MB) is the clip of the two boards, with
`lyrebird-boards-poster.jpg` beside it so a visitor pays for a poster rather than 10 MB
before anything appears, and `lyrebird-boards.vtt` beside that. The track exists because
the clip has sound and no speech: one cue for the whole length describing what the sound
*is* is the accessible answer, where timing cues would be a transcription of a Poisson
process. The clip is also the `og:video` and a `VideoObject` in the JSON-LD graph, so a
social card plays the boards instead of describing them.

`robots.txt` allows the AI crawlers on purpose, the same decision the parent made, and
disallows `/firmware/` and `/_app/` — 1.9 MB binaries and hashed JavaScript are not
readable content. `llms.txt` is the plain-text summary written for those crawlers, and it
leads with the three things about this project that get got wrong: no samples, no
streaming, and the screen is not a spectrogram.

## Known leftovers

- `tools/preview_galaxy.py` solves the camera distance the way the firmware used to, so
  its framing is not the board's. See "The galaxy".
- `docs/band-preview.png` in the README is a host render, not a photograph. There is still
  no photograph of either board in this repository.

## Out of scope (deliberately)

Improvise grammar walk, reverb/limiter, config sector, boot-report verification, OTA,
parity with the browser parity harness, any way to search the dial.
