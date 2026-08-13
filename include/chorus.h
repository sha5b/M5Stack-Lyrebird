// The ambient chorus: Lyrebird's engine.ts startAmbient + individual.ts,
// ported to a millis()-driven scheduler.
//
// Selection is one dial of SPECIES_COUNT + 1 slots:
//
//   slot 0            ALL BIRDS — every species at once, two individuals each,
//                     the loudest arrival rate. This is where the device boots.
//   slot 1..N         one species, with its own roster of 4 individuals.
//
// A "roster" is what stops a chorus sounding like one bird with amnesia: held
// per-individual deviations in pitch, timing, level and vibrato. Identity-
// carrying traits come from a golden-ratio low-discrepancy sequence so two
// birds never land close enough to be confusable; secondary traits are seeded
// jitter. Poisson song arrivals, and a 25 % chance a *conspecific* answers —
// a duet rather than an echo.
#pragma once

#include <stdint.h>

enum ChorusMode : uint8_t { MODE_CHORUS = 0, MODE_SOLO = 1 };

void chorusInit();
void chorusTick();  // call every loop() iteration

// The dial. 0 = all birds, 1..SPECIES_COUNT = that species alone.
void chorusSetSlot(int slot);  // wraps around; reseeds the roster, stops in-flight notes
int chorusSlot();
int chorusSlotCount();         // SPECIES_COUNT + 1
bool chorusAllBirds();         // slot == 0
int chorusSpecies();           // species index for the current slot; -1 on slot 0
const char* chorusLabel();     // "ALL BIRDS" or the species' common name

// Chorus (several individuals) vs solo (one bird, songs back to back). Only
// meaningful on a single-species slot — slot 0 is always a chorus.
void chorusSetMode(ChorusMode m);
ChorusMode chorusMode();

void chorusSetEnabled(bool on);  // false: panic + silence (pause)
bool chorusEnabled();

// Number of individuals currently in the roster, for the display.
int chorusRosterSize();

// Which species the roster bird carrying this tag belongs to, or -1 if the tag
// is not in the roster. For the display: the sweep colours a voice by its tag,
// and the galaxy band has to find that voice's island as well.
//
// The answer can be stale by one bird. A song player holds a *copy* of its bird,
// and rollRoster() can replace a roster entry while that copy is still singing —
// it declines to evict a bird that is mid-song, which closes the common case and
// not the racy one. The cost is one wrong island for one syllable.
int chorusSpeciesForTag(uint8_t tag);
