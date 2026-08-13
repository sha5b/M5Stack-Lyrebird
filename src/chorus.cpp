// See chorus.h. Trait spreads mirror individual.ts: identity-carrying traits
// (pitch, vibrato rate) come from a golden-ratio low-discrepancy sequence so
// two birds never land too close to tell apart; secondary traits get seeded
// jitter. Tremor (per-note wobble) is the only per-note randomness.
#include "chorus.h"
#include "syrinx.h"
#include "bird_data.h"

#include <Arduino.h>
#include <math.h>

#define BIRDS_PER_SPECIES 4       // roster size on a single-species slot
#define BIRDS_PER_SPECIES_ALL 2   // ...and per species on the all-birds slot
#define ROSTER_MAX (SPECIES_COUNT * BIRDS_PER_SPECIES_ALL > BIRDS_PER_SPECIES \
                    ? SPECIES_COUNT * BIRDS_PER_SPECIES_ALL : BIRDS_PER_SPECIES)
#define SONG_PLAYERS 6            // concurrent songs in flight (duet answers overlap)
#define SONGS_PER_MINUTE 12.0f    // one species
#define SONGS_PER_MINUTE_ALL 34.0f// all twelve: a dawn chorus, not a solo with echoes
#define ANSWER_CHANCE 0.25f
#define TREMBOR 0.006f            // per-note wobble, as individual.ts applyVoice
#define GOLDEN 0.6180339887498949f

struct Individual {
    uint8_t species;
    uint8_t tag;        // roster index; the display colours voices by it
    float pitchCents;
    float durScale;
    float levelScale;
    float amScale;
    float vibScale;
    float harmonicScale;
};

// A song walking its syllable sequence, one syllable at a time.
struct SongPlayer {
    bool active = false;
    uint8_t songIdx = 0;
    uint8_t pos = 0;
    uint32_t nextAt = 0;  // millis when the next syllable starts
    Individual bird;
    float gain = 1;
};

static int s_slot = 0;  // 0 = all birds
static ChorusMode s_mode = MODE_CHORUS;
static bool s_enabled = true;
static Individual s_roster[ROSTER_MAX];
static int s_rosterN = 0;
static SongPlayer s_players[SONG_PLAYERS];
static uint32_t s_ambientUntil = 0;   // Poisson clock, millis
static uint32_t s_soloGapUntil = 0;   // solo mode: pause between songs
static bool s_soloWasBusy = false;

static float frand() {  // uniform [0, 1)
    return (float)(esp_random() & 0xFFFFFF) / 16777216.0f;
}

// Low-discrepancy spread in [-1, 1], as individual.ts's spread().
static float spread(int index, float phase) {
    float x = (index + 1) * GOLDEN + phase;
    return (x - floorf(x)) * 2.0f - 1.0f;
}

// mulberry32 over a hash of "speciesIdx#index", as individual.ts's rng().
static uint32_t fnv1a(int speciesIdx, int index) {
    uint32_t h = 0x811c9dc5u;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d#%d", speciesIdx, index);
    for (const char* p = buf; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 0x01000193u;
    }
    return h;
}

static float mulberry32(uint32_t& a) {
    a += 0x6d2b79f5u;
    uint32_t t = (a ^ (a >> 15)) * (1u | a);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (float)((t ^ (t >> 14))) / 4294967296.0f;
}

// Individual `index` of `speciesIdx`, deterministic across boots.
static void makeIndividual(Individual& b, int speciesIdx, int index, int tag) {
    uint32_t state = fnv1a(speciesIdx, index);
    b.species = (uint8_t)speciesIdx;
    b.tag = (uint8_t)tag;
    b.pitchCents = spread(index, 0.0f) * 90.0f;          // about a semitone of range
    b.vibScale = 1.0f + spread(index, 0.37f) * 0.18f;
    b.durScale = 1.0f + (mulberry32(state) * 2.0f - 1.0f) * 0.09f;
    b.levelScale = 1.0f + (mulberry32(state) * 2.0f - 1.0f) * 0.12f;
    b.amScale = 1.0f + (mulberry32(state) * 2.0f - 1.0f) * 0.10f;
    b.harmonicScale = 1.0f + (mulberry32(state) * 2.0f - 1.0f) * 0.22f;
}

static void reseedRoster() {
    s_rosterN = 0;
    if (s_slot == 0) {
        // every species, two individuals each — tags stay adjacent per species
        // so the display gives one species one region of the palette
        for (int sp = 0; sp < SPECIES_COUNT; sp++) {
            for (int i = 0; i < BIRDS_PER_SPECIES_ALL; i++) {
                makeIndividual(s_roster[s_rosterN], sp, i, s_rosterN);
                s_rosterN++;
            }
        }
    } else {
        for (int i = 0; i < BIRDS_PER_SPECIES; i++) {
            makeIndividual(s_roster[s_rosterN], s_slot - 1, i, s_rosterN);
            s_rosterN++;
        }
    }
}

// Start a song on a free player, voiced by bird, first syllable at delayMs.
static void startSong(uint8_t songIdx, const Individual& bird, float gain, uint32_t delayMs) {
    for (auto& p : s_players) {
        if (p.active) continue;
        p.active = true;
        p.songIdx = songIdx;
        p.pos = 0;
        p.bird = bird;
        p.gain = gain;
        p.nextAt = millis() + delayMs;
        return;
    }
}

// Another individual of the same species, or -1 if this species has only one
// bird in the roster (which is the case for nobody at present, but the roster
// shape is a tunable).
static int conspecific(int rosterIdx) {
    uint8_t sp = s_roster[rosterIdx].species;
    int candidates[ROSTER_MAX];
    int n = 0;
    for (int i = 0; i < s_rosterN; i++) {
        if (i != rosterIdx && s_roster[i].species == sp) candidates[n++] = i;
    }
    if (!n) return -1;
    return candidates[(int)(frand() * n) % n];
}

// One chorus event: a bird sings a random song; 25 % a rival of the same
// species answers. Cross-species "answers" would not be a duet, so answers
// stay conspecific even on the all-birds slot.
static void scheduleSong(uint32_t atMs) {
    if (!s_rosterN) return;
    // clamp: after a UI stall atMs can sit in the past, and an unsigned
    // atMs - millis() would wrap to weeks — sing immediately instead
    uint32_t now = millis();
    uint32_t delay = (int32_t)(atMs - now) > 0 ? atMs - now : 0;

    int birdIdx = (int)(frand() * s_rosterN) % s_rosterN;
    const SpeciesData* sp = &SPECIES[s_roster[birdIdx].species];
    if (!sp->nSongs) return;
    uint8_t song = (uint8_t)(frand() * sp->nSongs);
    startSong(song, s_roster[birdIdx], 0.45f + frand() * 0.5f, delay);

    if (frand() < ANSWER_CHANCE) {
        int rival = conspecific(birdIdx);
        if (rival >= 0) {
            const SpeciesData* rsp = &SPECIES[s_roster[rival].species];
            uint8_t answer = (uint8_t)(frand() * rsp->nSongs);
            startSong(answer, s_roster[rival], 0.6f,
                      delay + 400 + (uint32_t)(frand() * 900.0f));
        }
    }
}

// Advance every in-flight player whose next syllable is due.
static void pumpPlayers() {
    uint32_t now = millis();
    for (auto& p : s_players) {
        if (!p.active || (int32_t)(now - p.nextAt) < 0) continue;
        const SpeciesData* sp = &SPECIES[p.bird.species];
        if (p.songIdx >= sp->nSongs) { p.active = false; continue; }
        const SongData& song = sp->songs[p.songIdx];
        // drop syllables we slept past (scheduler starvation), don't burst them
        while (p.pos < song.n && (int32_t)(now - p.nextAt) > 250) {
            const SyllableData& skipped = sp->syllables[song.syllIdx[p.pos]];
            float gap = p.pos < song.n - 1 ? song.gaps[p.pos] : 0.0f;
            p.nextAt += (uint32_t)(skipped.dur * p.bird.durScale * 1000.0f)
                        + (uint32_t)(gap * 1000.0f);
            p.pos++;
        }
        if (p.pos >= song.n) {
            p.active = false;
            continue;
        }
        const SyllableData& syl = sp->syllables[song.syllIdx[p.pos]];
        // applyVoice: held traits + a touch of per-note tremor
        SyrinxVoicing v;
        float wobble1 = 1.0f + (frand() * 2.0f - 1.0f) * TREMBOR;
        float wobble2 = 1.0f + (frand() * 2.0f - 1.0f) * TREMBOR;
        v.pitchMul = powf(2.0f, p.bird.pitchCents / 1200.0f) * wobble1;
        v.durScale = p.bird.durScale * wobble2;
        v.levelScale = p.bird.levelScale;
        v.amScale = p.bird.amScale;
        v.vibScale = p.bird.vibScale;
        v.harmonicScale = p.bird.harmonicScale;
        v.gain = p.gain;
        v.tag = p.bird.tag;
        syrinxNote(sp, song.syllIdx[p.pos], v);
        float gap = p.pos < song.n - 1 ? song.gaps[p.pos] : 0.0f;
        p.nextAt += (uint32_t)(syl.dur * v.durScale * 1000.0f) + (uint32_t)(gap * 1000.0f);
        p.pos++;
        if (p.pos >= song.n) p.active = false;
    }
}

void chorusInit() {
    reseedRoster();
    s_ambientUntil = millis() + 500;  // first bird lands soon after boot
}

void chorusTick() {
    if (!s_enabled) return;
    bool all = s_slot == 0;
    if (all || s_mode == MODE_CHORUS) {
        float rate = all ? SONGS_PER_MINUTE_ALL : SONGS_PER_MINUTE;
        uint32_t horizon = millis() + 500;
        while ((int32_t)(s_ambientUntil - horizon) < 0) {
            // exponential inter-arrival = Poisson process
            float draw = 1.0f - frand();
            if (draw < 1e-6f) draw = 1e-6f;
            s_ambientUntil += (uint32_t)(-logf(draw) * 60000.0f / rate);
            scheduleSong(s_ambientUntil);
        }
    } else {
        // solo: one bird, the species' songs back to back, ~800 ms apart
        bool busy = false;
        for (auto& p : s_players) busy |= p.active;
        busy = busy || syrinxActiveVoices() > 0;
        if (busy) {
            s_soloWasBusy = true;
        } else if (s_soloWasBusy) {  // a song just ended: short breath, then the next
            s_soloWasBusy = false;
            s_soloGapUntil = millis() + 800;
        } else if ((int32_t)(millis() - s_soloGapUntil) >= 0) {
            const SpeciesData* sp = &SPECIES[s_slot - 1];
            if (sp->nSongs && s_rosterN) {
                startSong((uint8_t)(frand() * sp->nSongs), s_roster[0], 0.8f, 0);
                s_soloWasBusy = true;
            }
        }
    }
    pumpPlayers();
}

static void restart() {
    syrinxPanic();
    for (auto& p : s_players) p.active = false;
    s_soloWasBusy = false;
    s_ambientUntil = millis() + 300;
    s_soloGapUntil = millis() + 300;
}

void chorusSetSlot(int slot) {
    int n = SPECIES_COUNT + 1;
    s_slot = ((slot % n) + n) % n;
    restart();
    reseedRoster();
}

int chorusSlot() { return s_slot; }
int chorusSlotCount() { return SPECIES_COUNT + 1; }
bool chorusAllBirds() { return s_slot == 0; }
int chorusSpecies() { return s_slot == 0 ? -1 : s_slot - 1; }

const char* chorusLabel() {
    return s_slot == 0 ? "ALL BIRDS" : SPECIES[s_slot - 1].common;
}

void chorusSetMode(ChorusMode m) {
    if (m == s_mode || s_slot == 0) return;  // slot 0 is a chorus by definition
    s_mode = m;
    restart();
}

ChorusMode chorusMode() { return s_mode; }

void chorusSetEnabled(bool on) {
    s_enabled = on;
    if (!on) {
        syrinxPanic();
        for (auto& p : s_players) p.active = false;
    } else {
        restart();
    }
}

bool chorusEnabled() { return s_enabled; }

int chorusRosterSize() { return s_rosterN; }
