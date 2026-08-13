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

- Both envs build: `pio run`. Measured off the ELFs —

  | env | flash | static RAM | + heap | band |
  |---|---|---|---|---|
  | `m5stack-fire` | 1.79 MB of 3 MB | 68.4 KB | 96 KB canvas | roster + songs |
  | `m5stack-cores3` | 1.78 MB of 3 MB | 65.5 KB | 96 KB canvas | roster + songs |

  The corpus is `rodata`, which is why 4.0 MB of generated C costs 30 KB of RAM. The
  CoreS3's extra static RAM is the arm trails (32 tags x 72 points); its canvas is
  allocated at runtime, so it does not appear in the build figure — about 160 KB of
  320 KB in use once it is up. It logs the allocation over serial at boot.
- `./scripts/build-firmware.sh` writes `lyrebird-fire.bin`, `lyrebird-cores3.bin`, a
  per-board `manifest-<id>.json` and one `firmware.json` listing both, into
  `web/static/firmware/`.
- Webflasher: `cd web && npm run check` and `npm run build` (static site into
  `web/build/`).
- **Hardware, precisely.** A CoreS3 has been flashed and photographed running. It boots,
  draws its chrome, and reports `1/2424 chorus 24 birds, all species`, `voices 0/8`,
  `dsp 16%` and `vol 76%`, with the three touch zones drawn along the bottom. So: the
  2424-slot dial is live, the touch strip renders, and **`audioGetLoad()` reads 16 %** —
  which retires the CPU-headroom worry, since the crackle line is around 90 %.

  What that photo does *not* settle: it is the pre-galaxy image (see below), and a
  photograph says nothing about sound. **Nothing has been listened to** — whether either
  board sings, how loud it is, and whether the crackle and hiss work below actually fixed
  anything are all still open. A Fire has been connected and its reset sequence measured
  over its own UART (see "The reset sequence"), but never flashed.
- **The merged images in `web/static/firmware/` are built by hand and go stale silently.**
  They are not a build product of `pio run`, so a `pio run` that succeeds proves nothing
  about what the page will flash. This has already cost one confusing session: the CoreS3
  was flashed with an image predating `src/galaxy.cpp` and came up with the spectrogram,
  which looks exactly like the galaxy not working. Run `./scripts/build-firmware.sh`
  after touching firmware, and check the timestamps before believing a symptom.
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
tools/preview_galaxy.py   draws the CoreS3's band to a PNG on the host
tools/verify_syrinx.py    host check of the DSP rework, no dependencies
include/syrinx.h
src/syrinx.cpp            the synth port
include/audio.h           one API, two backends
src/audio_dac.cpp         Fire: I2S0 built-in DAC mode + render task on core 1
src/audio_spk.cpp         CoreS3: 16-bit PCM into M5.Speaker
include/chorus.h
src/chorus.cpp            Poisson scheduler + individual-bird roster + song players
include/ui.h
src/ui.cpp                the chrome, and the band: arms, or the sweep by flag
include/galaxy.h
src/galaxy.cpp            the band — roster, beaded songs, following camera
include/buttons.h
src/buttons.cpp           three logical buttons over physical or touch
src/main.cpp              M5 bring-up, button semantics, frame pacing
scripts/build-firmware.sh pio build + esptool merge_bin + manifests
web/                      SvelteKit + esptool-js flasher (from CYD-Physarum)
web/src/lib/galaxy.ts     the page's backdrop; corpus.ts is its generated data
```

## The galaxy — the page's backdrop and the CoreS3's arms

Both screens are arranged by the same layout and draw different things with it.

**The layout happens once, in `tools/generate_galaxy.py`**, which reads
`include/bird_data.h` and writes positions to `web/src/lib/corpus.ts` (base64, 54 KB
packed) and `include/galaxy_data.h` (PROGMEM, 54 KB of flash plus a 4.8 KB per-species
index). Re-run it whenever `generate_assets.py` runs. Every species gets an island on a
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
is 310 × 156 px, and that constraint is the whole design. Three versions:

| | what it drew | why it failed |
|---|---|---|
| 1 | 12724 marks accumulated into a brightness buffer | at that size twelve thousand marks is a texture, and the bird actually singing was four pixels lost inside it |
| 2 | the same marks, dim, as a bed under the songs | a ground made of twelve thousand dots on a 156 px band is not a ground, it is grain |
| 3 | the roster as a constellation, songs as beaded threads, camera following the singer | — |

So the static layer is the **roster**, not the corpus: the two dozen birds that can
actually sing right now, one dim dot each. A dozen distinct dots, since the two
individuals of a species share an island. They earn their place twice — a camera with
nothing static in frame is a camera you cannot see moving, and without them the travel
between birds reads as marks appearing from nowhere.

Over that, the song: a chain of **stretched dots** growing out of its own bird's dot, each
one a `drawWedgeLine` with its own radius at each end, so weight follows the envelope —
loud syllables swell the thread, quiet ones pinch it — with a brighter bead over the newest
dot. That is the parent's `shaders.ts` device (`uPointSize * (0.85 + vPeak * 2.4)`: a swell
says where the note *is*, more precisely than a thickening line) and its `ribbon.ts` finding
(a one-pixel thread disappears; a song needs real width).

The **camera** is `Framing.svelte` scaled to a panel: the loudest bird takes it, keeps it
while it sings, and holds it `FOCUS_HOLD_MS` after — without that hold, 34 songs a minute
would re-aim it twice a second and the picture would be nothing but travel. It follows the
*head* of the song rather than the species' island, so the thread streams away behind
instead of wandering out of frame, and eases at `CAM_EASE` (about 1.5 s to arrive; a cut on
this screen reads as a glitch).

It stays accurate under all of it. Every mark is a measurement: position is the species'
place in the corpus, distance from the middle is the f0 being sung on the spectrogram's own
log axis, weight is the envelope, hue is the individual — the same hue the sweep gives it.

Threads are geometry, not smears: points are kept in layout space and re-projected every
frame, so the camera can travel through a song already sung. Everything goes into an
`M5Canvas` and is pushed whole, which is what stops a moving line from flickering — 96 KB
of DRAM allocated at runtime, and if it will not allocate the band stays black and says so
over serial rather than crashing.

`tools/preview_galaxy.py` re-implements the whole model on the host — camera, focus,
easing, wedge dots — and writes a PNG. Four values were plainly wrong before it: `GROW` at
0.016 (left over from version 1) made every song a 15 px scribble; `TWIST_RATE` at 0.42
closed each thread into a loop, so the depth cue became the shape; the bed's `MARK_ADD` put
a lone syllable at RGB (0, 2, 34), which on a dark panel is black; and at `ZOOM` 105 with
no roster the band was one thread in a void.

**The grid is GridBurst, and that is the answer to "can we have a faint 3D grid".** The
parent tried both obvious versions and rejected them, in its own words: *a flat lattice over
the whole window does not move with the cloud, so it fights the depth it is supposed to
establish*, and a ground plane is *a permanent floor under a corpus that has no floor*. So
the grid is struck, not standing: each new song lays a cross of three axis-aligned dashed
rules through the bird that started it, rising over ~0.15 s and gone by ~1.8 s.

Two details of that are load-bearing and both come straight from GridBurst:

- **The reach is quoted against the view, not the world.** `STRIKE_REACH` is 1.2 of the
  band's visible half-height in *pixels*; the rules' directions are world axes, so they
  turn with the corpus, but their length does not change when the camera dollies. A fixed
  world length is only right at one zoom, and this camera moves.
- **The colour is held at strike time.** A cross outlives its note by more than a second,
  and re-resolving the hue per frame would have it reporting on a bird that had stopped.

Strikes closer than `STRIKE_MERGE` refresh one cross rather than drawing two — a duet
otherwise lays two crosses whose rules lie almost on top of each other, which reads as one
badly drawn cross.

**The roster dots carry a memory, so none of them is inert.** They were drawn at one fixed
brightness, which meant a bird that had not had a Poisson arrival in two minutes sat there
at full strength doing nothing — a scatter of marks that never changed. Brightness and size
now follow how recently that species sang, squared, decaying to a floor over
`ROSTER_MEMORY_MS`. They are also deduplicated by species: the two individuals of one
species share an island, so drawing both put two dots in the same place.

**The camera dollies as well as pans.** `CAM_DIST_NEAR` while anything is singing,
`CAM_DIST_IDLE` between songs, eased slower than the pan so a push-in reads as a push-in
rather than as the picture changing size.

**Nothing about the band has been seen on hardware.** The preview settles composition and
says nothing about how that panel treats a thin dark-green line at an angle.

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
gets 158 px instead of 124.

The band has to stop above `TOUCH_HIT_Y` (200 in include/buttons.h). The touch target is
deliberately taller than the drawn strip, so a band running past that line would turn a
tap on the picture into a button press. It ends at 197.

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

**Paused for a minute and the board turns itself off** (`PAUSE_OFF_MS` in src/main.cpp).
It draws a line saying why first, because a screen that goes black on its own is
indistinguishable from a flat battery or a crash.

What "off" means is not the same on both boards, and is not this code's choice:

| board | PMIC | `M5.Power.powerOff()` does |
|---|---|---|
| CoreS3 | AXP2101 | cuts the rail. Actually off. |
| Fire | IP5306 | releases the boost keep-on and **returns** |

So on the Fire, `powerOff()` is enough on battery — the regulator drops the rail under
light load — and cannot work on USB, where the charger is holding the rail up. `sleepNow()`
therefore calls `deepSleep(0, true)` after it: on USB the board ends up dark, drawing
little, waking on a button. Both calls, in that order, and the order matters.

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

## Known leftovers

- `web/static/favicon.{ico,png,svg}` and `apple-touch-icon.png` are byte-identical to
  CYD-Physarum's. So is the vestigial empty `web/static/media/`.
- `SITE_URL` in `web/src/lib/seo/index.ts` is `https://lyrebird.variable.gallery`, which
  is **the parent project's site** — the same URL `PARENT_PROJECT.url` points at. So the
  page's canonical link, `og:url` and JSON-LD `@id` all currently claim to be the browser
  instrument. Needs this site's own URL once the Railway deploy exists.
- `OG_IMAGE` / `OG_IMAGE_ALT` are exported and never imported; `web/static/og.jpg` does
  not exist and `+page.svelte` emits no `og:image`. Add a photo and the tags, or drop the
  exports.
- The page's copy explains the Fire's bridge as a CP2104, which is right for older units
  and not for the v2.7 on this desk.

## Out of scope (deliberately)

Improvise grammar walk, reverb/limiter, config sector, boot-report verification, OTA,
parity with the browser parity harness, any way to search the dial.
