// Audio output for the M5Stack Fire: I2S0 in built-in DAC mode, DMA-paced.
// The Fire's speaker amp (NS4168) hangs off DAC1 = GPIO25, which the I2S
// peripheral drives as its "left" built-in DAC channel. DMA pacing gives a
// clean 22.05 kHz clock — a delayMicroseconds bit-bang loop jitters, which a
// pitched synth hears immediately.
#pragma once

#include <stdint.h>

#define AUDIO_SAMPLE_RATE 22050

void audioInit();              // installs the I2S driver, starts the render task on core 1
void audioSetRunning(bool on); // false: DAC disabled + GPIO25 high-Z (no idle hum)
void audioSetVolume(float v);  // 0..1
float audioGetVolume();
