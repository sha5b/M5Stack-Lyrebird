# Lyrebird — M5Stack Fire & CoreS3

A pocket dawn chorus for the M5Stack Core family. Twelve songbird species, synthesized in real
time by the Mindlin–Laje physical model of the avian syrinx — no samples, no SD card.
The device boots into **all birds**: all twelve species at once, two individuals of each,
arriving as a Poisson process, with a 25 % chance a conspecific answers. Stepping the dial
takes you down to one species at a time.

The screen sweeps a spectrogram of what is sounding: every active voice is plotted at its
pitch on a log axis (250 Hz – 10 kHz), coloured by which individual is singing and dimmed
by its envelope, so syllable contours draw themselves as the notes play.

This is an embedded port of the [Lyrebird](https://github.com/sha5b/Lyrebird) web app's
synth engine (`app/src/lib/audio/worklets/syrinx-processor.js`) and chorus logic
(`app/src/lib/audio/engine.ts`, `individual.ts`). The webflasher stack is copied from
[CYD-Physarum](https://github.com/sha5b/CYD-Physarum).

## Controls

| Button | Short press | Hold |
|--------|-------------|------|
| A      | previous position on the dial | volume down |
| B      | chorus ↔ solo | pause / resume |
| C      | next position on the dial | volume up |

A and C step one dial of `SPECIES_COUNT + 1` positions, and it wraps:

| Position | What sings |
|----------|------------|
| 1 | **all birds** — all 12 species, 2 individuals each, ~34 songs/min. Boots here. |
| 2 – 13 | one species, roster of 4 individuals |

B toggles **chorus** (the roster answering each other, ~12 songs/min) against **solo**
(one bird, that species' songs back to back). Position 1 is a chorus by definition, so B
does nothing there.

## Flashing

### From the browser

Serve the flasher (`cd web && npm install && npm run dev`) or use the deployed page,
then connect the Fire over a data USB cable and click through. Chrome/Edge/Opera only —
Web Serial.

### From the command line

```bash
pio run -e m5stack-fire   -t upload
pio run -e m5stack-cores3 -t upload
```

or with the merged images:

```bash
./scripts/build-firmware.sh
esptool.py --chip esp32   write_flash 0x0 web/static/firmware/lyrebird-fire.bin
esptool.py --chip esp32s3 write_flash 0x0 web/static/firmware/lyrebird-cores3.bin
```

## Layout

```
platformio.ini            envs m5stack-fire (ESP32) and m5stack-cores3 (ESP32-S3),
                          both on M5Unified
partitions.csv            factory-only, 3 MB app, no OTA
assets/                   vendored Lyrebird inventory + calibration JSON (CC BY-NC-SA 4.0)
tools/generate_assets.py  JSON -> include/bird_data.h + include/calibration.h
include/, src/            firmware: syrinx synth, chorus, UI, and one audio
                          backend per board (audio_dac.cpp / audio_spk.cpp)
scripts/build-firmware.sh merged 0x0 image + manifest into web/static/firmware/
web/                      SvelteKit + esptool-js webflasher (from CYD-Physarum)
```

## Technical notes

- Boards: the Fire is an ESP32, the CoreS3 an ESP32-S3. Different architectures,
  so two binaries; the flasher reads the chip off the board and picks. Everything
  except the audio backend is shared. M5Unified covers both, including the
  CoreS3's AW88298 amplifier bring-up and the bezel touch strip it presents as
  BtnA/B/C — which is why the button code has no board case in it.
- Audio (Fire): I2S0 built-in DAC mode, DMA-paced at 22050 Hz. The Fire's speaker amp
  (NS4168) hangs off DAC1 = GPIO25, which is the I2S "left" DAC channel. The DAC
  reads the MSB of each 16-bit DMA slot, unsigned — mid-level is 0x8000. When
  paused, the DAC is disabled to keep the amp silent. 8 bits is a hard -48 dB
  noise floor, so the output is rounded with TPDF dither and first-order noise
  shaping — and gated off entirely over digital silence, because dither with no
  signal under it is just hiss.
- Audio (CoreS3): 16-bit PCM into M5Unified's Speaker, which owns the I2S and the
  AW88298's I2C setup. No DAC on the ESP32-S3, so none of the 8-bit machinery
  above applies.
- Synth: float32 port of the browser worklet. Envelope path, timbre classes
  (pure/reed/buzz), attack/hold, AM, vibrato, harmonic band, and the detuned
  second syringeal side (`two`). The respiration, two-voice coupling, formant,
  noise, rough and fricative paths are not ported — no authored syllable uses them.
  8 voices max, 16–64 Euler substeps per sample.
- Synth CPU: the calibration tables are indexed arithmetically rather than searched.
  `CAL_INV_F0_GRID` is geometric, `CAL_PRESSURE_GRID` uniform, and `CAL_BETA_GRID`
  uniform in three runs, so `betaForF0` and `ampPP` are O(1) per sample instead of a
  192-entry binary search plus an ~85-step linear scan. The envelope and the AM
  oscillator are incremental, and the pitch contour moves to a 32-sample control block
  with a per-sample ramp. There is no libm call left in the sample loop. See the header
  comment in `src/syrinx.cpp` for why: the old cost overran the I2S DMA, and a late DMA
  buffer is an audible click.
- Verification: `python3 tools/verify_syrinx.py` checks the synth's control-rate and
  incremental paths against the closed forms they replaced — the envelope, the AM and
  vibrato oscillators, the tanh approximation, the O(1) table indexing against the old
  scan and binary search, and the residual pitch error in cents. No dependencies.
- Data: the 12 hand-authored species from Lyrebird's `data/inventory.json` plus the
  offline beta/ampPP calibration tables, compiled in as PROGMEM headers (~155 KB
  total). Regenerate with `python3 tools/generate_assets.py` after updating `assets/`.

## Licence

CC BY-NC-SA 4.0, matching the Lyrebird project it is derived from. The bird data in
`assets/` and the generated headers come from that project and carry the same licence.
