# HANDOFF — M5Stack-Lyrebird

State as of 2026-08-13, second session. The project was built in one session from an
empty repo; a second session reworked the audio path, the screen and the mode dial.
Everything below reflects what is on disk, not plans.

## What this is

An embedded port of the [Lyrebird](../Lyrebird) bird-chorus app for the M5Stack Fire
(ESP32, no SD card), plus a browser webflasher copied from
[CYD-Physarum](../CYD-Physarum). The device boots into **all birds**: all twelve
species at once, two individuals of each, all synthesized on-device by the
Mindlin–Laje syrinx model. No samples, no filesystem — bird data is compiled into
the firmware as PROGMEM headers.

## Current state

- Both firmwares build clean: `pio run` — Fire 572 KB / 31 KB RAM, CoreS3 565 KB /
  29 KB RAM, of 3 MB flash each.
- `./scripts/build-firmware.sh` produces `lyrebird-fire.bin` (632 KB) and
  `lyrebird-cores3.bin` (620 KB) plus a per-board `manifest-<id>.json` and one
  `firmware.json` listing both.
- Webflasher: `cd web && npm run check` clean (0 errors), `npm run build` produces
  the static site in `web/build/`.
- **Not yet verified on hardware.** The device has never been flashed. See
  "Unverified" below.
- The DSP rework is verified on the host instead, by
  `python3 tools/verify_syrinx.py` — the render-loop cursor, the
  incremental envelope against the old closed form, the recursive AM/vibrato
  oscillators against libm, the Pade tanh, the O(1) table indexing against the old
  scan and binary search, and the control-rate pitch error in cents. All pass. It is
  a Python re-implementation of the changed math, not a build of the firmware: this
  machine has no host C++ compiler, so a waveform diff of old against new was not
  possible.

## Layout

```
platformio.ini            env m5stack-fire: espressif32@^6.5.0, arduino, M5Stack@^0.4.3
partitions.csv            factory-only, 3 MB app, no OTA, no data partitions
assets/                   vendored from Lyrebird: inventory.json (12 authored species)
                          + syrinx-calibration.json (beta/ampPP tables) — CC BY-NC-SA 4.0
tools/generate_assets.py  assets/ -> include/bird_data.h + include/calibration.h
include/syrinx.h          synth API: syrinxNote / syrinxRender / syrinxPanic
src/syrinx.cpp            the synth port (see "Key decisions")
include/audio.h
src/audio.cpp             I2S DMA driver + render task on core 1
include/chorus.h
src/chorus.cpp            Poisson scheduler + individual-bird roster + song players
include/ui.h
src/ui.cpp                the sweeping spectrogram and the page chrome
src/main.cpp              M5 bring-up, buttons, frame pacing
scripts/build-firmware.sh pio build + esptool merge_bin + manifests
web/                      SvelteKit + esptool-js flasher (from CYD-Physarum)
```

## Key decisions (and why)

- **Birds: the 12 hand-authored species** from Lyrebird's `data/inventory.json`
  (owner decision). 44 syllables, 21 songs. Song syllable ids are resolved to
  indices at generation time — the firmware never does string lookups.
- **Audio: I2S0 in built-in DAC mode, DMA-paced at 22050 Hz** — not the
  DeDeNoise-Mashine `dacWrite` + `delayMicroseconds` loop (11025 Hz, jittery;
  fatal for a pitched synth). The Fire's NS4168 amp hangs off DAC1 = GPIO25,
  which is the I2S "left" DAC channel (`I2S_DAC_CHANNEL_LEFT_EN`). The DAC reads
  the unsigned MSB of each 16-bit DMA slot, so mid-level is `0x8000` and
  `i2s_zero_dma_buffer` is a *pop*, not silence — `primeMidLevel()` in
  src/audio.cpp exists because of this. When paused: DAC disabled, GPIO25 high-Z.
- **Synth scope: only what the authored inventory uses.** Ported: envelope path,
  timbre classes (pure/reed/buzz), attack/hold, AM, vibrato, harmonic band,
  detuned second side (`two`), calibration-table beta/ampPP lookups. NOT ported:
  respiration, coupled voices, formants, noise, rough, fricative — no authored
  syllable carries them. float32, no parity with the browser harness is claimed
  or needed. 8 voices max (`SYRINX_MAX_VOICES`).
- **Chorus: faithful port of individual.ts + startAmbient.** 4-bird roster per
  species; pitch/vibrato spread via the golden-ratio low-discrepancy sequence,
  secondary traits via mulberry32 over an FNV hash of `speciesIdx#index`
  (deterministic, same birds every boot). Poisson arrivals at 12 songs/min,
  25 % duet answers 0.4–1.3 s later, 0.6 % per-note tremor. Pan was dropped
  (mono speaker). Distance/reverb/limiter master chain not ported.
- **Flasher: CYD-Physarum's web/ stripped.** esptool-js over Web Serial,
  single merged 0x0 image, no config sector, no boot report, no sim background.
  The Fire's CP2104 shares the CP2102 USB ID already in the filter list.
  Deploys via `web/railway.json` (RAILPACK, `npm run start` on sirv), same as CYD.
- **Buttons** (owner decision): A/C short = previous/next position on the dial,
  A/C hold = volume down/up (0.02 steps, 120 ms repeat), B short = chorus ↔ solo,
  B hold 1 s = pause/resume.
- **One dial, all-birds at position 0** (owner decision, second session). The dial
  has `SPECIES_COUNT + 1` positions and wraps. Position 0 is every species at once
  (2 individuals each, 24-bird roster, 34 songs/min); positions 1..12 are one species
  (4 individuals, 12 songs/min). B is a no-op on position 0 — that slot is a chorus
  by definition. Boots at position 0. The first session had no all-species mode at
  all: "chorus" only ever meant several individuals of *one* species.

## Unverified — check these first on real hardware

1. **I2S channel format.** `I2S_CHANNEL_FMT_ONLY_LEFT` + `I2S_DAC_CHANNEL_LEFT_EN`
  is the one combination never tested on the device. If there is no sound or
  wrong-rate sound, try `I2S_CHANNEL_FMT_RIGHT_LEFT` (write stereo frames, left
  slot only) or `I2S_DAC_CHANNEL_BOTH_EN` (GPIO26 is NC on the Fire, harmless).
2. **CPU headroom.** Worst case is 8 voices × 64 Euler substeps at 22050 Hz on
  core 1. Realistic gamma values give 16–24 substeps. The screen now shows the
  measured figure — `dsp NN%` bottom right, from `audioGetLoad()`, the fraction of a
  DMA block's wall time spent rendering it. Read it before changing anything: past
  ~90 % the render task is about to miss the DMA. If it sits high, drop
  `SYRINX_MAX_VOICES` to 6 or raise `DMA_BUF_LEN`.
3. **Loudness.** The browser mix runs a limiter because ampPP-divided sources
  are quiet; the firmware has none. If too quiet, raise `SOURCE_GAIN`
  (src/syrinx.cpp) before adding a limiter. Note the mix clipper is now
  1.28 / 0.95 rather than 1.6 / 0.8 — same small-signal gain to within half a dB,
  higher ceiling, so it saturates later.
4. **Button feel.** 300 ms hold threshold for volume vs dial-step is
  inherited from DeDeNoise; adjust to taste.
5. **Whether the crackle is actually gone.** The second session found four causes and
  fixed all four blind (see below). If any grit remains, `dsp %` says immediately
  whether it is still a timing problem or now a quantisation one.

## Second session — the crackle, the screen, the dial

**The crackle had four causes, all fixed, none verified on hardware.**

1. **CPU overrun, the main one.** The old inner loop called `expf` twice, `cosf` once
   and `tanhf` once per sample per voice, then walked two calibration tables: a
   192-entry binary search in `betaForF0` and a linear scan in `ampPP` that restarts at
   index 0 every call and runs ~85 steps to reach beta ≈ 1. At eight voices that is
   >100 M table steps a second on a 240 MHz core. When the render task misses the DMA
   deadline the DAC gets a late buffer, and a late buffer is a click. Fixed by
   exploiting the tables' regularity (see README) and by making the envelope, AM and
   contour incremental. No libm call is left in the sample loop.
2. **`tx_desc_auto_clear = true`.** On a late refill the driver handed the DAC 0x0000 —
   which in unsigned-MSB DAC terms is not silence but the *full negative rail*. Every
   underrun was a bang. Now false: the DMA repeats the previous buffer instead.
3. **8-bit truncation.** `(int)(s * 127.0f) + 128` truncates toward zero, i.e. a
   double-width step across zero that every syllable's decay tail passes through as
   granular buzz. Now rounds, with ±0.5 LSB TPDF dither and first-order noise shaping.
4. **A race on `s_dacBuf`.** `audioSetRunning` called `primeMidLevel()` from loopTask
   while audioTask could still be inside `i2s_write` on the same buffer. There is now
   an idle handshake (`s_idleAck` / `waitAudioIdle`).

Supporting changes: DMA buffers 4 → 8 (~93 ms of slack), audio task priority 1 → 3 so
an LCD repaint cannot delay a refill (still core 1 on purpose — the task watchdog
watches IDLE0, so an overrun on core 0 would reboot the board), and `-O2` over the
framework's `-Os`.

**The screen** is `src/ui.cpp` now, not inline in `main.cpp`. Nothing clears the band
except `uiFullRedraw()`; the text repaints over itself with an opaque background colour,
so there is no flicker and no full-screen wipe on a volume change. The old UI called
`fillScreen` on every redraw.

**Verification.** `python3 tools/verify_syrinx.py` re-implements the changed math in
Python, parses the real calibration and bird tables out of `include/`, and checks each
change against the code it replaced. Re-run it if the DSP is touched again. It is not as
strong as a waveform diff of old against new — that needs a host C++ compiler, which this
machine does not have — but it covers every line that changed semantics.

## Webflasher — the connect failure (second session)

Reported symptom: the board blanks and restarts over and over, then
"Failed to connect with the device". Four causes in `web/src/lib/installer.ts`,
all fixed, **none verified on hardware** — nobody has flashed this board yet.

1. **esptool-js's reset sequence has no strapping margin.** Its `ClassicReset` is
   `D0|R1|W100|D1|R0|W50|D0`, and driving a fake transport shows IO0 going low and
   EN going high *in the same millisecond*: the ESP32 latches GPIO0 while it is
   still transitioning, so it boots the application instead of the ROM loader. The
   M5Stack Core makes it worse — EN carries a debounce cap for the side reset
   button, so 100 ms may not pull it to a logic low at all, and over Web Serial each
   signal change is a separate async round trip. Replaced with two `CustomReset`
   sequences (`RESET_SEQUENCE_SHORT` / `_LONG`) that hold EN low 200/500 ms and wait
   40/80 ms between IO0 low and EN release. Injected through
   `LoaderOptions.resetConstructors.classicReset`, which esptool alternates across
   its seven attempts.
2. **Our timeout was below esptool's own retry budget.** `connect()` makes 7 attempts
   of (reset + 5 syncs); the old 20 s bound cut it off part-way, and we then started
   the whole thing again — seven more resets, never finishing. Now 60 s.
3. **We ran the whole connect twice, once per baud rate.** Sync always happens at
   `romBaudrate` (115200) whatever `baudrate` says, so the second run could not
   help and only doubled the resets. Now one connect; the 115200 fallback fires only
   when `loader.chip` is set, i.e. the board answered and it was the fast rate that
   failed.
4. **The reason was being thrown away.** `connect()` raises a flat "Failed to connect
   with the device" while the finding — "Wrong boot mode detected (0x13)" vs
   "Download mode detected, but getting no sync reply" — goes only to `debug()`.
   `debugLogging: true` now puts it in the log the page already shows.

If it still fails, read that log: *wrong boot mode* means the reset is not taking
(cable, or the sequence needs lengthening again); *no sync reply* means the cable or
the port is. The Core has no BOOT button, so there is no manual download mode —
`pio run -t upload` / esptool.py is the fallback, and it drives the same lines from
the OS with tighter timing than Web Serial allows.

## Third session — remaining noise, and the CoreS3

**Noise on the Fire.** Three causes found; all fixed, none verified on hardware.

1. **The dither ran over silence.** The previous session added TPDF dither and noise
   shaping to the 8-bit path, which is right where there is signal to decorrelate and
   wrong everywhere else — over digital silence it is a hiss the speaker plays
   continuously, and in chorus mode most of the wall clock *is* silence. A block that
   is exactly zero now gets the flat mid-level code and nothing else. This is the most
   likely explanation for "the tone is much better but still very noisy": the tone
   improved and a new steady noise floor arrived at the same time.
2. **Backlight PWM.** `M5Display::begin()` ran a 44.1 kHz LEDC PWM on GPIO32 to dim the
   screen — tens of milliamps switching next to GPIO25, which is a high-impedance
   analog DAC output into an amplifier. Brightness is now pinned at 255, where the duty
   saturates and the pin stops switching. `BACKLIGHT` in src/main.cpp is the experiment:
   if the hiss changes between 255 and 120, it is the backlight, not the DAC.
3. **A pin conflict.** `audioSetRunning` called `pinMode(25, OUTPUT)` *and*
   `dac_output_enable()` on the same pad, pointing the GPIO matrix and the RTC analog
   path at one pin and letting call order decide. `i2s_set_dac_mode()` configures the
   pad by itself now, plus the `i2s_set_pin(I2S_NUM_0, NULL)` that was missing.

If hiss remains *during* notes it is the 8-bit floor, and the lever is
`AUDIO_SAMPLE_RATE` (include/audio.h, documented there). Left at 22050 because that is
the rate the port was measured at; `dsp NN%` on screen is the budget meter.

Also corrected: the Fire's amplifier is **not** an NS4168, as the first session's
comments claimed. NS4168 is an I2S part; M5Unified has this board as
`use_dac = true, pin_data_out = GPIO_NUM_25`, i.e. analog off the internal DAC.

**CoreS3 support.** The ESP32-S3 has no DAC, so the Fire's whole audio backend is
inapplicable rather than merely different. The project moved from `M5Stack@0.4.x` to
**M5Unified + M5GFX** (owner decision) and now builds two envs:

- `m5stack-fire` — ESP32, `src/audio_dac.cpp`, bootloader at 0x1000
- `m5stack-cores3` — ESP32-S3, `src/audio_spk.cpp`, bootloader at **0x0**

That offset difference is easy to miss and produces an image that flashes cleanly and
never boots. It is encoded per board in scripts/build-firmware.sh.

`audio_spk.cpp` streams 16-bit PCM into `M5.Speaker` rather than driving I2S directly,
because M5Unified owns the AW88298's I2C bring-up and its AW9523 reset line, and
guessing at those registers against hardware gets you a silent board with no way to
tell why. Pacing comes from `M5.Speaker.isPlaying(ch) >= 2` — its two wave slots are
the backpressure, the same job `i2s_write` blocking does on the Fire. `magnification`
is forced to 1: M5Unified defaults it to 4 on this board and our source is already
full-scale.

Buttons needed no board case. M5Unified maps the touch strip in the bezel below the
CoreS3 screen onto BtnA/B/C (`_touch_button_height`, split in thirds by x), so
src/main.cpp is unchanged.

Not tried and unverified: the CoreS3 has never been flashed either. Most likely
snags, in order — `M5.Speaker` starving under our render task's priority; the
`magnification`/volume interaction being louder or quieter than the Fire; and
`M5.Speaker.end()` on pause being heavier than intended, since it powers the amplifier
down through M5Unified's enable callback.

**Adding an audio module.** M5Unified already knows the M-Bus pins for the stackable
Module Display and Module RCA, behind `cfg.external_speaker.module_display` /
`.module_rca` — so a module is a config flag, not a set of pins to type in. Module
Audio is an ES8388 at I2C 0x10 and ships two pin configurations, one for Basic/Core2
and one for CoreS3. If modules are added, the honest choice is a build per
board+module rather than a config sector: the pins are compile-time in M5Unified
anyway, and the website already asks which board you have.

## Regenerating data

```bash
# after updating assets/ from the Lyrebird repo:
python3 tools/generate_assets.py
```

Headers are committed; the script is only re-run when the vendored JSON changes.

## Building & flashing

```bash
pio run -e m5stack-fire -t upload          # direct
./scripts/build-firmware.sh                # merged image + manifests
esptool.py --chip esp32 write_flash 0x0 web/static/firmware/lyrebird.bin
```

Version stamp is `dev` until the repo has its first commit (`git describe`
fallback in build-firmware.sh).

## Known leftovers

- `web/static/favicon*` and `apple-touch-icon.png` are still CYD-Physarum's.
- `og.jpg` was deleted (CYD branding); `+page.svelte` no longer emits og:image.
  Add a Fire photo to `web/static/og.jpg` and restore the tags if wanted.
- The flasher site URL in `web/src/lib/seo/index.ts` is a placeholder
  (`https://lyrebird.variable.gallery`) until the Railway deploy exists.

## Out of scope (deliberately, v1)

Fitted/learned species (2489), improvise grammar walk, reverb/limiter, config
sector, boot-report verification, OTA, parity with the browser parity harness.
