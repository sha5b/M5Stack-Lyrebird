// Audio output for the M5Stack Fire: I2S0 in built-in DAC mode, DMA-paced.
// The Fire's speaker amp (NS4168) hangs off DAC1 = GPIO25, which the I2S
// peripheral drives as its "left" built-in DAC channel. DMA pacing gives a
// clean 22.05 kHz clock — a delayMicroseconds bit-bang loop jitters, which a
// pitched synth hears immediately.
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

void audioInit();              // installs the I2S driver, starts the render task on core 1
void audioSetRunning(bool on); // false: DAC disabled + GPIO25 high-Z (no idle hum)
void audioSetVolume(float v);  // 0..1
float audioGetVolume();

// Fraction of one DMA block's wall time spent rendering it, smoothed. Above
// ~0.9 the render task is about to miss the DMA and the output will crackle;
// the UI shows this so a heavy chorus is visible rather than just audible.
float audioGetLoad();
