// One audio API, one backend per board: src/audio_dac.cpp on the Fire and
// src/audio_spk.cpp on the CoreS3, selected by build_src_filter. The comments
// below describe the Fire, which is where all the difficulty is.
//
// Fire: I2S0 in built-in DAC mode, DMA-paced. Its speaker amp is analog-in off
// DAC1 = GPIO25, which the I2S peripheral drives as its "left" built-in DAC
// channel. DMA pacing gives a clean 22.05 kHz clock — a delayMicroseconds
// bit-bang loop jitters, which a pitched synth hears immediately.
//
// CoreS3: no DAC on an ESP32-S3, so none of the 8-bit machinery here applies.
// 16-bit PCM goes into M5.Speaker, which owns the AW88298 bring-up.
#pragma once

#include <stdint.h>

/**
 * The next lever on noise, if hiss *during* a note is still the complaint.
 *
 * The DAC is 8 bits, so there is a hard noise floor around -48 dB. Dither and
 * first-order shaping push that noise up in frequency rather than removing it,
 * and at 22050 Hz "up in frequency" means 5-11 kHz — which is exactly where
 * birds sing and where the ear is most sensitive. Raising the rate moves the
 * shaped noise above the birds, and moves the DAC's zero-order-hold images out
 * of reach of the amplifier as well. M5Unified oversamples this same DAC for
 * that reason (`spk_cfg.sample_rate *= 2` on this board).
 *
 * It is not free: 32000 costs about 1.2x the synth CPU, 44100 about 1.7x. Watch
 * `dsp NN%` on screen after changing it — past ~90 % the crackle comes back, and
 * the fix then is SYRINX_MAX_VOICES 6 or a lower oversampling floor in
 * syrinxNote. Left at 22050 because that is the rate the port was measured at.
 */
#define AUDIO_SAMPLE_RATE 22050

void audioInit();              // starts the render task on core 1
void audioSetRunning(bool on); // false: output silenced (Fire: DAC disabled, GPIO25 high-Z)
void audioSetVolume(float v);  // 0..1
float audioGetVolume();

// Fraction of one DMA block's wall time spent rendering it, smoothed. Above
// ~0.9 the render task is about to miss the DMA and the output will crackle;
// the UI shows this so a heavy chorus is visible rather than just audible.
float audioGetLoad();
