# HANDOFF — M5Stack-Lyrebird

State as of 2026-08-13. The project was built in one session from an empty repo.
Everything below reflects what is committed on disk, not plans.

## What this is

An embedded port of the [Lyrebird](../Lyrebird) bird-chorus app for the M5Stack Fire
(ESP32, no SD card), plus a browser webflasher copied from
[CYD-Physarum](../CYD-Physarum). The device boots straight into **chorus mode**:
a Poisson chorus of individual birds of the selected species, all synthesized
on-device by the Mindlin–Laje syrinx model. No samples, no filesystem — bird data
is compiled into the firmware as PROGMEM headers.

## Current state

- Firmware builds clean: `pio run -e m5stack-fire` — 487 KB of 3 MB flash, 27 KB RAM.
- `./scripts/build-firmware.sh` produces `web/static/firmware/lyrebird.bin` (544 KB
  merged 0x0 image) + `manifest.json` + `firmware.json`.
- Webflasher: `cd web && npm run check` clean (0 errors), `npm run build` produces
  the static site in `web/build/`.
- **Not yet verified on hardware.** The device has never been flashed. See
  "Unverified" below.

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
src/main.cpp              M5 bring-up, buttons, LCD UI
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
- **Buttons** (owner decision): A/C short = species prev/next, A/C hold = volume
  down/up (0.02 steps, 120 ms repeat), B short = chorus ↔ solo, B hold 1 s =
  pause/resume. Boots into chorus (standard mode, owner decision).

## Unverified — check these first on real hardware

1. **I2S channel format.** `I2S_CHANNEL_FMT_ONLY_LEFT` + `I2S_DAC_CHANNEL_LEFT_EN`
  is the one combination never tested on the device. If there is no sound or
  wrong-rate sound, try `I2S_CHANNEL_FMT_RIGHT_LEFT` (write stereo frames, left
  slot only) or `I2S_DAC_CHANNEL_BOTH_EN` (GPIO26 is NC on the Fire, harmless).
2. **CPU headroom.** Worst case is 8 voices × 64 Euler substeps at 22050 Hz on
  core 1. Realistic gamma values give 16–24 substeps, but if a duet of buzzy
  low birds glitches, drop `SYRINX_MAX_VOICES` to 6 or raise `DMA_BUF_LEN`.
3. **Loudness.** The browser mix runs a limiter because ampPP-divided sources
  are quiet; the firmware has none. If too quiet, raise `SOURCE_GAIN`
  (src/syrinx.cpp) before adding a limiter.
4. **Button feel.** 300 ms hold threshold for volume vs species-switch is
  inherited from DeDeNoise; adjust to taste.

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
