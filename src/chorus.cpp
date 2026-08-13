// See chorus.h. Trait spreads mirror individual.ts: identity-carrying traits
// (pitch, vibrato rate) come from a golden-ratio low-discrepancy sequence so
// two birds never land too close to tell apart; secondary traits get seeded
// jitter. Tremor (per-note wobble) is the only per-note randomness.
#include "chorus.h"
#include "syrinx.h"
#include "bird_data.h"

#include <Arduino.h>
#include <math.h>

#define BIRDS_PER_SPECIES 4
#define SONG_PLAYERS 4            // concurrent songs in flight (duet answers overlap)
#define SONGS_PER_MINUTE 12.0f
#define ANSWER_CHANCE 0.25f
#define TREMBOR 0.006f            // per-note wobble, as individual.ts applyVoice
#define GOLDEN 0.6180339887498949f

struct Individual {
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

static int s_species = 0;
static ChorusMode s_mode = MODE_CHORUS;
static bool s_enabled = true;
static Individual s_roster[BIRDS_PER_SPECIES];
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

static void reseedRoster() {
    for (int i = 0; i < BIRDS_PER_SPECIES; i++) {
        uint32_t state = fnv1a(s_species, i);
        Individual& b = s_roster[i];
        b.pitchCents = spread(i, 0.0f) * 90.0f;          // about a semitone of range
        b.vibScale = 1.0f + spread(i, 0.37f) * 0.18f;
        b.durScale = 1.0f + (mulberry32(state) * 2.0f - 1.0f) * 0.09f;
        b.levelScale = 1.0f + (mulberry32(state) * 2.0f - 1.0f) * 0.12f;
        b.amScale = 1.0f + (mulberry32(state) * 2.0f - 1.0f) * 0.10f;
        b.harmonicScale = 1.0f + (mulberry32(state) * 2.0f - 1.0f) * 0.22f;
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

// One chorus event: a bird sings a random song; 25 % a rival answers.
static void scheduleSong(uint32_t atMs) {
    const SpeciesData* sp = &SPECIES[s_species];
    if (!sp->nSongs) return;
    // clamp: after a UI stall atMs can sit in the past, and an unsigned
    // atMs - millis() would wrap to weeks — sing immediately instead
    uint32_t now = millis();
    uint32_t delay = (int32_t)(atMs - now) > 0 ? atMs - now : 0;
    int birdIdx = (int)(frand() * BIRDS_PER_SPECIES) % BIRDS_PER_SPECIES;
    uint8_t song = (uint8_t)(frand() * sp->nSongs);
    startSong(song, s_roster[birdIdx], 0.45f + frand() * 0.5f, delay);
    if (frand() < ANSWER_CHANCE) {
        int rival = (birdIdx + 1 + (int)(frand() * (BIRDS_PER_SPECIES - 1))) % BIRDS_PER_SPECIES;
        uint8_t answer = (uint8_t)(frand() * sp->nSongs);
        startSong(answer, s_roster[rival], 0.6f,
                  delay + 400 + (uint32_t)(frand() * 900.0f));
    }
}

// Advance every in-flight player whose next syllable is due.
static void pumpPlayers() {
    const SpeciesData* sp = &SPECIES[s_species];
    uint32_t now = millis();
    for (auto& p : s_players) {
        if (!p.active || (int32_t)(now - p.nextAt) < 0) continue;
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
    if (s_mode == MODE_CHORUS) {
        uint32_t horizon = millis() + 500;
        while ((int32_t)(s_ambientUntil - horizon) < 0) {
            // exponential inter-arrival = Poisson process
            float draw = 1.0f - frand();
            if (draw < 1e-6f) draw = 1e-6f;
            s_ambientUntil += (uint32_t)(-logf(draw) * 60000.0f / SONGS_PER_MINUTE);
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
            const SpeciesData* sp = &SPECIES[s_species];
            if (sp->nSongs) {
                startSong((uint8_t)(frand() * sp->nSongs), s_roster[0], 0.8f, 0);
                s_soloWasBusy = true;
            }
        }
    }
    pumpPlayers();
}

void chorusSetSpecies(int idx) {
    s_species = ((idx % SPECIES_COUNT) + SPECIES_COUNT) % SPECIES_COUNT;
    syrinxPanic();
    for (auto& p : s_players) p.active = false;
    reseedRoster();
    s_ambientUntil = millis() + 300;
}

int chorusSpecies() { return s_species; }

void chorusSetMode(ChorusMode m) {
    if (m == s_mode) return;
    s_mode = m;
    syrinxPanic();
    for (auto& p : s_players) p.active = false;
    s_ambientUntil = millis() + 300;
    s_soloGapUntil = millis() + 300;
}

ChorusMode chorusMode() { return s_mode; }

void chorusSetEnabled(bool on) {
    s_enabled = on;
    if (!on) {
        syrinxPanic();
        for (auto& p : s_players) p.active = false;
    } else {
        s_ambientUntil = millis() + 300;
        s_soloGapUntil = millis() + 300;
    }
}

bool chorusEnabled() { return s_enabled; }
