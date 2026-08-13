// The ambient chorus: Lyrebird's engine.ts startAmbient + individual.ts,
// ported to a millis()-driven scheduler. Boots into chorus mode.
//
// A fixed roster of "individual" birds per species (held pitch/timing/level
// deviations — without it a chorus is one bird with amnesia), Poisson song
// arrivals at ~12 songs/min, and a 25 % chance a different bird answers —
// a duet rather than an echo.
#pragma once

#include <stdint.h>

enum ChorusMode : uint8_t { MODE_CHORUS = 0, MODE_SOLO = 1 };

void chorusInit();
void chorusTick();  // call every loop() iteration

void chorusSetSpecies(int idx);  // wraps around; reseeds the roster, stops in-flight notes
int chorusSpecies();

void chorusSetMode(ChorusMode m);
ChorusMode chorusMode();

void chorusSetEnabled(bool on);  // false: panic + silence (pause)
bool chorusEnabled();
