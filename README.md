# Lyrebird — M5Stack Fire

A pocket dawn chorus for the M5Stack Fire. Twelve songbird species, synthesized in real
time by the Mindlin–Laje physical model of the avian syrinx — no samples, no SD card.
The device boots straight into chorus mode: a Poisson chorus of individual birds, each
with its own held pitch and timing, and a 25 % chance a second bird answers.

This is an embedded port of the [Lyrebird](https://github.com/sha5b/Lyrebird) web app's
synth engine (`app/src/lib/audio/worklets/syrinx-processor.js`) and chorus logic
(`app/src/lib/audio/engine.ts`, `individual.ts`). The webflasher stack is copied from
[CYD-Physarum](https://github.com/sha5b/CYD-Physarum).

## Controls

| Button | Short press | Hold |
|--------|-------------|------|
| A      | previous species | volume down |
| B      | chorus ↔ solo | pause / resume |
| C      | next species | volume up |

**Chorus** (default on boot): individual birds of the current species sing at Poisson
intervals (~12 songs/min). **Solo**: one bird sings the species' songs back to back.

## Flashing

### From the browser

Serve the flasher (`cd web && npm install && npm run dev`) or use the deployed page,
then connect the Fire over a data USB cable and click through. Chrome/Edge/Opera only —
Web Serial.

### From the command line

```bash
pio run -e m5stack-fire -t upload
```

or with the merged image:

```bash
./scripts/build-firmware.sh
esptool.py --chip esp32 write_flash 0x0 web/static/firmware/lyrebird.bin
```

## Layout

```
platformio.ini            env m5stack-fire (Arduino, M5Stack@^0.4.3)
partitions.csv            factory-only, 3 MB app, no OTA
assets/                   vendored Lyrebird inventory + calibration JSON (CC BY-NC-SA 4.0)
tools/generate_assets.py  JSON -> include/bird_data.h + include/calibration.h
include/, src/            firmware: syrinx synth, I2S-DAC audio, chorus, UI
scripts/build-firmware.sh merged 0x0 image + manifest into web/static/firmware/
web/                      SvelteKit + esptool-js webflasher (from CYD-Physarum)
```

## Technical notes

- Audio: I2S0 built-in DAC mode, DMA-paced at 22050 Hz. The Fire's speaker amp
  (NS4168) hangs off DAC1 = GPIO25, which is the I2S "left" DAC channel. The DAC
  reads the MSB of each 16-bit DMA slot, unsigned — mid-level is 0x8000. When
  paused, the DAC is disabled and GPIO25 goes high-Z to keep the amp silent.
- Synth: float32 port of the browser worklet. Envelope path, timbre classes
  (pure/reed/buzz), attack/hold, AM, vibrato, harmonic band, and the detuned
  second syringeal side (`two`). The respiration, two-voice coupling, formant,
  noise, rough and fricative paths are not ported — no authored syllable uses them.
  8 voices max, 16–64 Euler substeps per sample.
- Data: the 12 hand-authored species from Lyrebird's `data/inventory.json` plus the
  offline beta/ampPP calibration tables, compiled in as PROGMEM headers (~155 KB
  total). Regenerate with `python3 tools/generate_assets.py` after updating `assets/`.

## Licence

CC BY-NC-SA 4.0, matching the Lyrebird project it is derived from. The bird data in
`assets/` and the generated headers come from that project and carry the same licence.
