// Lyrebird syrinx synth — ESP32 port of the browser AudioWorklet
// (Lyrebird/app/src/lib/audio/worklets/syrinx-processor.js).
//
// Mindlin-Laje low-dimensional syrinx oscillator + f0-tracking vocal tract,
// float32, mono. Only what the 12 authored species use is ported: envelope
// path, timbre classes, attack/hold, AM, vibrato, harmonic band and the
// detuned second side (`two`). Respiration, coupled voices, formants, noise,
// rough and fricative paths are left out — no authored syllable carries them.
#pragma once

#include <stdint.h>
#include "bird_data.h"

#define SYRINX_MAX_VOICES 8

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
};

void syrinxInit(float sampleRate);

// Start a syllable on a free voice. Returns false when the pool is full.
bool syrinxNote(const SpeciesData* species, uint8_t syllableIdx, const SyrinxVoicing& v);

void syrinxPanic();

// Render n mono samples in [-1, 1] into out (adds nothing; it writes).
void syrinxRender(float* out, int n);

int syrinxActiveVoices();
float syrinxLastLevel();  // decaying peak, for the UI
