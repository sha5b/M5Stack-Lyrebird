// Lyrebird syrinx synth — ESP32 port of the browser AudioWorklet
// (Lyrebird/app/src/lib/audio/worklets/syrinx-processor.js).
//
// Mindlin-Laje low-dimensional syrinx oscillator + f0-tracking vocal tract,
// float32, mono. Only what the 12 authored species use is ported: envelope
// path, timbre classes, attack/hold, AM, vibrato, harmonic band and the
// detuned second side (`two`). Respiration, coupled voices, formants, noise,
// rough and fricative paths are left out — no authored syllable carries them.
//
// The pitch/beta/amplitude control path runs at SYRINX_CTRL (32 samples), not
// per sample: the per-sample expf/logf/sinf calls it used to make were the
// bulk of the CPU cost and were what starved the I2S DMA into crackling.
#pragma once

#include <stdint.h>
#include "bird_data.h"

#define SYRINX_MAX_VOICES 8
#define SYRINX_CTRL 32  // samples between control-rate updates (must be 2^n)

// Per-note voicing applied by the chorus (the "individual bird" layer of
// Lyrebird's individual.ts): held per bird, with a touch of per-note tremor.
struct SyrinxVoicing {
    float pitchMul;      // 2^(cents/1200) * tremor
    float durScale;      // * tremor
    float levelScale;
    float amScale;
    float vibScale;
    float harmonicScale;
    float gain;          // note gain (distance-ish loudness)
    uint8_t tag;         // who is singing — the UI colours voices by this
};

// A voice as the display sees it. Sampled without locking: a torn read costs
// one wrong pixel, and locking the render task for the UI would cost a click.
struct SyrinxVoiceInfo {
    float f0;      // Hz, current control-block pitch
    float env;     // 0..1 envelope
    uint8_t tag;   // SyrinxVoicing::tag of the note that owns this voice
};

void syrinxInit(float sampleRate);

// Start a syllable on a free voice. Returns false when the pool is full.
bool syrinxNote(const SpeciesData* species, uint8_t syllableIdx, const SyrinxVoicing& v);

void syrinxPanic();

// Render n mono samples in [-1, 1] into out (adds nothing; it writes).
void syrinxRender(float* out, int n);

int syrinxActiveVoices();
float syrinxLastLevel();  // decaying peak, for the UI

// Fills out[] with the currently sounding voices, returns how many. max is the
// caller's array size; SYRINX_MAX_VOICES is always enough.
int syrinxSnapshot(SyrinxVoiceInfo* out, int max);
