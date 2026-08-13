// See galaxy.h.
#include "galaxy.h"

#include <M5Unified.h>
#include <math.h>
#include <string.h>

#include "chorus.h"
#include "galaxy_data.h"
#include "syrinx.h"
#include "ui.h"  // uiVoiceColor: a voice keeps its colour across both band pictures

// ---- the picture -----------------------------------------------------------
//
// A camera inside the corpus, following whichever bird is singing.
//
// The band is 310 x 156 px. That is the whole design constraint and the two
// versions before this one both broke it:
//
//   the whole corpus, static   12724 marks accumulated into a brightness buffer.
//                              At this size twelve thousand marks is a texture,
//                              and the four pixels of the bird actually singing
//                              were lost inside it.
//   the corpus as a faint bed  the same marks, dim, under the arms. Better, and
//                              still wrong: a ground made of twelve thousand dots
//                              on a 156 px band is not a ground, it is grain.
//
// A screen this small can hold a handful of marks that are each big enough to have
// a shape. So the static layer is not the corpus, it is the *roster*: the two dozen
// birds that can actually sing right now, one dim dot each. Twelve or so dots, not
// twelve thousand. They also earn their place twice over — a camera with nothing
// static in frame is a camera you cannot see moving, and without them the travel
// between birds reads as marks appearing out of nowhere.
//
// Over that, the song being sung: a chain of stretched dots growing out of its own
// bird's dot, fat where the note is loud and pinched where it is quiet, tapering
// away behind. When another bird takes over, the camera swings across to it and
// dollies in, and the roster slides past on the way — which is what shows the corpus
// has a shape at all.
//
// And under both, the grid — but only where something is singing. That is
// GridBurst.svelte's finding, and it is worth quoting because it is the answer to
// "can we have a faint 3D grid": *a flat lattice over the whole window does not move
// with the cloud, so it fights the depth it is supposed to establish; a ground plane
// is a permanent floor under a corpus that has no floor.* So each new song strikes a
// cross of three axis-aligned rules through the bird that started it, which brighten
// as the note arrives and run back out as it ends. The measured field appears around
// the sound and dissolves after it, and the strongest lines on screen are always at
// the place worth looking at.
//
// It stays accurate under all that. Every mark is a real measurement: the dot's
// place is its species' place in the corpus, its distance from the middle is the f0
// being sung on the spectrogram's own log axis, its weight is the envelope, and its
// hue is the individual — the same hue the sweep would have given it.
//
// The devices are the parent project's own, scaled to a 320 px panel:
//
//   Framing.svelte   flies the camera to a phrase and holds it framed, because
//                    "it is highlighted somewhere behind you" is not a way to find
//                    anything. Here the target is the head of the current song.
//   ribbon.ts        song threads need a real width — a one-pixel line disappears.
//                    Here that is drawWedgeLine, one radius per end.
//   shaders.ts       the travelling bead, `uPointSize * (0.85 + vPeak * 2.4)`: a
//                    swell that says where the note *is*, more precisely than a
//                    thickening line does.
//
// Trails are geometry, not smears: points are kept in layout space and re-projected
// every frame, so the camera can move through a song already sung. Drawn into an
// off-screen canvas and pushed whole, which is what stops a moving line from
// flickering.

// ---- the camera -----------------------------------------------------------

// Distance from the target, in layout units (the corpus is about 1.1 across), and
// the focal length quoted at it. Close enough that one song fills the band, which
// is the point of moving the camera at all — further out and this is the bed again
// with fewer marks.
#define CAM_DIST_NEAR 1.8f   // while a bird is singing: pushed in
#define CAM_DIST_IDLE 2.9f   // between songs: backed off, so the roster reads
#define CAM_F 2.0f

// How fast the dolly moves between those two, per frame. Slower than the pan, so a
// push-in reads as a push-in rather than as the picture changing size.
#define DOLLY_EASE 0.035f

// Pixels per layout unit at the target's depth. Close enough that a song fills a
// good part of the band and a dot has a size; far enough that two or three of the
// roster are usually in frame, so the picture is never one thread in the void.
#define ZOOM 100.0f

// The band has more room across than down. Mild, unlike the 2x this needed when it
// had to fit a whole sphere.
#define STRETCH_X 1.3f

// How fast the camera closes on its target, per frame. 0.06 at 25 Hz is about a
// second and a half to arrive — a glide, not a cut. A cut on a screen this size
// reads as a glitch.
#define CAM_EASE 0.06f

// A bird keeps the camera this long after it stops sounding. Songs arrive at about
// 34 a minute on the all-birds slot, so without a hold the camera would be re-aimed
// twice a second and the picture would be nothing but travel.
#define FOCUS_HOLD_MS 1500

// Turn periods, in seconds per revolution. The corpus still drifts under the
// camera, so a still moment is never quite still.
#define TURN_X_S 197.0f
#define TURN_Y_S 131.0f

// ---- the gesture ----------------------------------------------------------

// How far a song's thread advances per frame, in layout units. The median song is
// six syllables of 67 ms, so it has about twenty frames; at this zoom that comes out
// near the height of the band.
#define GROW 0.05f

// Pitch, across the thread — outward from the middle of the corpus, since that is
// the thread's cross axis. A high note pushes away from the centre, a low one pulls
// in, on the same log axis the spectrogram uses.
#define WIGGLE 0.5f

// The corkscrew, which is what gives perspective something to work on.
#define TWIST_AMP 0.15f
#define TWIST_RATE 0.16f  // radians per frame: about half a turn per song

// ---- the roster -----------------------------------------------------------

// A roster dot, in pixels at the target's depth, and how much of its bird's own hue
// it gets. Dim: it is a place, not an event. Bright enough to be a mark and never
// bright enough to be mistaken for a note.
//
// A dot is drawn *only* for a bird that has sung within ROSTER_MEMORY_MS, and fades
// out across it. There is no resting brightness, on purpose and at the second
// attempt: with a floor, a bird that has not had a Poisson arrival in two minutes
// still sat there as a permanent mark doing nothing, which is the thing that was
// wrong with drawing the whole roster in the first place. Now the constellation *is*
// the recent activity — every dot on the band is a bird you heard a moment ago.
#define ROSTER_R 1.8f
#define ROSTER_ENV_MAX 0.30f
#define ROSTER_MEMORY_MS 9000

// ---- the strike (GridBurst) -----------------------------------------------

// Live strikes. Six is more than the eight-voice pool can usefully start at once,
// and a seventh cross on a 310 px band would be scaffolding rather than a strike.
#define STRIKES 6

// The rules run off the frame in every direction — the band's diagonal, so whatever
// the camera is doing a rule always leaves the picture rather than stopping in it. A
// rule with visible ends is a cross sitting on a dot; a rule that leaves is a ruled
// line through space, which is the only version that reads as a grid.
//
// Quoted in pixels against the view, not in world units, for GridBurst's reason: a
// fixed world length is right at exactly one zoom, and this camera dollies.
#define STRIKE_REACH 1.0f

// Dashes, in pixels on and off, and how much of the bird's colour a rule gets.
// Sparse and very faint: at full strength the rules overlaid the song they were
// supposed to be a reference for, which is the wrong way round. This is scaffolding
// you notice second.
#define STRIKE_DASH 2
#define STRIKE_GAP 7
#define STRIKE_AMP 0.30f

// Attack and life, in frames at 25 Hz: brighten over ~0.15 s, gone by ~1.8 s. The
// cross outlives the note that made it, which is why its colour is held at strike
// time rather than resolved per frame — a cross that changed hue halfway through
// would be reporting on a bird that had stopped.
#define STRIKE_ATTACK 4
#define STRIKE_LIFE 45

// ---- the stroke -----------------------------------------------------------

// Dot radius in pixels at the target's depth, quiet and loud. Every mark is a dot
// stretched into the next one (drawWedgeLine, one radius per end), so a song is a
// chain of beads rather than a line of constant weight — loud syllables swell it and
// quiet ones pinch it.
#define DOT_R_QUIET 0.55f
#define DOT_R_LOUD 3.0f

// The head bead, over the newest dot: where the note is *now*, plus a halo of
// concentric fainter rings around it. The halo is what makes the head read as a
// light rather than as a dot — and on a panel with no bloom pass to spend, three
// rings of dimming spot is the whole trick.
#define BEAD_R 3.4f
#define HALO_RINGS 3
#define HALO_STEP 0.85f  // extra radius per ring, as a share of BEAD_R
#define HALO_AMP 0.22f   // the innermost ring's share of the bead's colour

// Arms per bird are keyed by roster tag. ROSTER_MAX in src/chorus.cpp is 24; 32 is
// the next power of two and leaves room for a bigger roster.
#define TRAIL_TAGS 32
// Points per thread. At the UI's 25 Hz this is how long a thread can get *and* how
// long it survives after the bird stops, both about three seconds.
#define TRAIL_LEN 72

// A bird is still singing the same song if it sounded within this long. Songs put
// 40-120 ms between syllables, so anything past a few hundred ms is a new song and a
// new thread.
#define SONG_GAP_MS 500

// ---- state ----------------------------------------------------------------

struct Trail {
    float x[TRAIL_LEN];
    float y[TRAIL_LEN];
    float z[TRAIL_LEN];
    uint16_t born[TRAIL_LEN];  // s_frame the point was laid down at
    uint8_t env[TRAIL_LEN];    // how loud the bird was, 0..255: the dot's weight
    // 1 = start of a new song; do not stretch a dot back to the point before it,
    // which belongs to the previous song and is a long way away.
    uint8_t brk[TRAIL_LEN];
    uint8_t n = 0;             // points held, up to TRAIL_LEN
    uint8_t head = 0;          // next write index
    uint16_t songAge = 0;      // frames since this thread started
    uint32_t lastMs = 0;       // when this tag last sounded
};

static Trail s_trail[TRAIL_TAGS];

struct Strike {
    // The *bird*, not a position: the cross is resolved from that bird's current head
    // every frame so it follows the song rather than staying where the song started.
    // GridBurst had to do the same once its knots could move — a strike that keeps
    // its old coordinates ends up ruling lines through empty space.
    uint8_t tag;
    uint16_t born;
    uint16_t colour;  // held at strike time; see STRIKE_ATTACK
    bool live;
};

static Strike s_strike[STRIKES];

// When each roster tag last sounded, for the dot's memory. 0 = never.
static uint32_t s_lastSang[TRAIL_TAGS];

static M5Canvas s_canvas(&M5.Display);
static bool s_ready = false;

static int s_x = 0, s_y = 0, s_w = 0, s_h = 0;
static uint16_t s_frame = 0;

// Where the camera is looking, how far back it is, and who it is following.
static float s_camX = 0, s_camY = 0, s_camZ = 0;
static float s_camDist = CAM_DIST_IDLE;
static int s_focusTag = -1;
static uint32_t s_focusMs = 0;

// The drift this frame.
static float s_cax = 1, s_sax = 0, s_cay = 1, s_say = 0;

// ---- helpers --------------------------------------------------------------

/**
 * Deterministic [0, 1) per species. Same shape as the generator's, and for the same
 * reason: a species has to sweep the same way every boot, or the picture stops being
 * about the corpus and starts being about the last reset.
 */
static inline float hash01(uint32_t n, uint32_t salt) {
    uint32_t h = n * 0x9E3779B1u + salt * 0x85EBCA6Bu;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return (float)h * (1.0f / 4294967296.0f);
}

/** Scale an RGB565 colour toward black. The canvas ground is black, so this is the
 *  fade — there is no alpha to spend on a 16-bit sprite. */
static inline uint16_t dimColour(uint16_t c, float amp) {
    if (amp <= 0.0f) return 0;
    if (amp > 1.0f) amp = 1.0f;
    return M5.Display.color565((uint8_t)(((c >> 11) & 0x1F) * (255.0f / 31.0f) * amp),
                               (uint8_t)(((c >> 5) & 0x3F) * (255.0f / 63.0f) * amp),
                               (uint8_t)((c & 0x1F) * (255.0f / 31.0f) * amp));
}

/** The log pitch axis the spectrogram uses: 250 Hz .. 10 kHz -> 0..1. */
static inline float pitchNorm(float f0) {
    if (f0 <= 250.0f) return 0.0f;
    const float v = logf(f0 / 250.0f) * (1.0f / 3.6888795f);  // logf(40)
    return v > 1.0f ? 1.0f : v;
}

/**
 * Project a layout-space point through the camera. `outK` is the perspective
 * factor, which scales dot radii as well as position — a near bead has to be a
 * bigger bead, or the depth the camera works for is thrown away in the one channel
 * that carries it best on a screen this size.
 */
static inline void project(float x, float y, float z, float& outX, float& outY,
                           float& outK) {
    const float dx = x - s_camX;
    const float dy = y - s_camY;
    const float dz = z - s_camZ;

    const float x1 = dx * s_cay + dz * s_say;
    const float z1 = -dx * s_say + dz * s_cay;
    const float y2 = dy * s_cax - z1 * s_sax;
    const float z2 = dy * s_sax + z1 * s_cax;

    // Behind the camera, or nearly: clamp rather than divide by something tiny.
    float denom = s_camDist + z2;
    if (denom < 0.25f) denom = 0.25f;
    const float k = CAM_F / denom;

    outX = (float)s_w * 0.5f + x1 * ZOOM * STRETCH_X * k;
    outY = (float)s_h * 0.5f - y2 * ZOOM * k;
    outK = k;
}

/**
 * Where a species sits, and the three axes its thread is drawn on.
 *
 * The anchor is the species' first syllable, which is inside its island — close
 * enough to the island's centre for this, and free, where an averaged centre would
 * be another table.
 *
 * `grow` is *tangential*: the thread sweeps around the body of the corpus rather
 * than shooting out of it, then the frame is spun about the radius by a per-species
 * angle so two birds do not sweep the same way on screen. `out` is the radius and
 * carries pitch; `twist` completes the frame and carries the corkscrew.
 */
static void armFrame(int sp, float* a, float* grow, float* out, float* twist) {
    const GalaxyPoint& p = GALAXY[GALAXY_ISLAND[sp]];
    a[0] = (float)p.x * GALAXY_SCALE;
    a[1] = (float)p.y * GALAXY_SCALE;
    a[2] = (float)p.z * GALAXY_SCALE;

    float len = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (len < 1e-4f) {
        // A species at the very centre of the corpus has no outward direction.
        out[0] = 0; out[1] = 1; out[2] = 0;
    } else {
        out[0] = a[0] / len; out[1] = a[1] / len; out[2] = a[2] / len;
    }

    grow[0] = out[2]; grow[1] = 0.0f; grow[2] = -out[0];
    len = sqrtf(grow[0] * grow[0] + grow[2] * grow[2]);
    if (len < 1e-4f) {
        grow[0] = 1.0f; grow[1] = 0.0f; grow[2] = 0.0f;
    } else {
        grow[0] /= len; grow[2] /= len;
    }

    twist[0] = out[1] * grow[2] - out[2] * grow[1];
    twist[1] = out[2] * grow[0] - out[0] * grow[2];
    twist[2] = out[0] * grow[1] - out[1] * grow[0];

    const float th = hash01((uint32_t)sp, 11) * 6.283185f;
    const float ct = cosf(th);
    const float st = sinf(th);
    for (int i = 0; i < 3; i++) {
        const float g = grow[i] * ct + twist[i] * st;
        twist[i] = -grow[i] * st + twist[i] * ct;
        grow[i] = g;
    }
}

/** The newest point of a thread, or false if it has none. */
static bool trailHead(const Trail& tr, float* p) {
    if (!tr.n) return false;
    const uint8_t k = (uint8_t)((tr.head + TRAIL_LEN - 1) % TRAIL_LEN);
    p[0] = tr.x[k];
    p[1] = tr.y[k];
    p[2] = tr.z[k];
    return true;
}

// ---- public ---------------------------------------------------------------

void galaxyInit(int x, int y, int w, int h) {
    s_x = x;
    s_y = y;
    s_w = w;
    s_h = h;

    // DRAM, not PSRAM: this is redrawn and pushed every frame, and PSRAM would pay
    // for the size with the bandwidth. If it will not fit, galaxyFrame() becomes a
    // no-op rather than a crash — the band stays black and everything else on the
    // screen still works.
    s_canvas.setPsram(false);
    s_canvas.setColorDepth(16);
    s_ready = s_canvas.createSprite(s_w, s_h) != nullptr;
    // Say so either way. A silently black band is the kind of symptom that gets
    // blamed on the drawing code for an hour before anyone checks the heap.
    Serial.printf("galaxy: %dx%d canvas (%d bytes) %s, %u bytes of heap free\n", s_w, s_h,
                  s_w * s_h * 2, s_ready ? "ok" : "FAILED — band stays black",
                  (unsigned)ESP.getFreeHeap());

    galaxyReset();
}

void galaxyReset() {
    for (auto& t : s_trail) {
        t.n = 0;
        t.head = 0;
        t.songAge = 0;
        t.lastMs = 0;
    }
    for (auto& st : s_strike) st.live = false;
    memset(s_lastSang, 0, sizeof(s_lastSang));
    s_frame = 0;
    s_focusTag = -1;
    s_focusMs = 0;
    s_camX = s_camY = s_camZ = 0.0f;
    s_camDist = CAM_DIST_IDLE;
    if (s_ready) {
        s_canvas.fillSprite(TFT_BLACK);
        s_canvas.pushSprite(s_x, s_y);
    }
}

void galaxyFrame() {
    if (!s_ready) return;
    s_frame++;

    const float t = (float)s_frame * 0.04f;  // uiFrame() is paced at 25 Hz
    const float ax = t * (6.283185f / TURN_X_S);
    const float ay = t * (6.283185f / TURN_Y_S);
    s_cax = cosf(ax);
    s_sax = sinf(ax);
    s_cay = cosf(ay);
    s_say = sinf(ay);

    // ---- who is singing ----------------------------------------------------
    // One point per tag per frame, whatever the voice count: two syllables of the
    // same bird overlapping is still one bird, and one thread.
    SyrinxVoiceInfo voices[SYRINX_MAX_VOICES];
    const int n = syrinxSnapshot(voices, SYRINX_MAX_VOICES);
    const bool all = chorusAllBirds();
    const uint32_t now = millis();

    float loudest[TRAIL_TAGS];
    float pitch[TRAIL_TAGS];
    memset(loudest, 0, sizeof(loudest));

    for (int i = 0; i < n; i++) {
        const uint8_t tag = voices[i].tag;
        if (tag >= TRAIL_TAGS || voices[i].env <= 0.004f) continue;
        if (voices[i].env > loudest[tag]) {
            loudest[tag] = voices[i].env;
            pitch[tag] = pitchNorm(voices[i].f0);
        }
    }

    // ---- grow their threads ------------------------------------------------
    for (int tag = 0; tag < TRAIL_TAGS; tag++) {
        if (loudest[tag] <= 0.0f) continue;
        const int sp = chorusSpeciesForTag((uint8_t)tag);
        if (sp < 0 || (size_t)(sp + 1) >= sizeof(GALAXY_ISLAND) / sizeof(GALAXY_ISLAND[0])) {
            continue;
        }

        Trail& tr = s_trail[tag];
        const bool fresh = (now - tr.lastMs) > SONG_GAP_MS;
        if (fresh) tr.songAge = 0;
        tr.lastMs = now;

        float a[3], grow[3], out[3], twist[3];
        armFrame(sp, a, grow, out, twist);

        const float len = (float)tr.songAge * GROW;
        const float w = (pitch[tag] - 0.5f) * WIGGLE;
        const float tw = sinf((float)tr.songAge * TWIST_RATE) * TWIST_AMP;

        const uint8_t k = tr.head;
        tr.x[k] = a[0] + grow[0] * len + out[0] * w + twist[0] * tw;
        tr.y[k] = a[1] + grow[1] * len + out[1] * w + twist[1] * tw;
        tr.z[k] = a[2] + grow[2] * len + out[2] * w + twist[2] * tw;
        tr.born[k] = s_frame;
        tr.env[k] = (uint8_t)(loudest[tag] * 255.0f);
        tr.brk[k] = fresh ? 1 : 0;

        tr.head = (uint8_t)((k + 1) % TRAIL_LEN);
        if (tr.n < TRAIL_LEN) tr.n++;
        tr.songAge++;
        s_lastSang[tag] = now;

        // A new song strikes the grid where it started. An existing cross close
        // enough to be the same mark is refreshed instead of joined by a second one.
        if (fresh) {
            // One cross per bird: a bird that starts another song refreshes its own
            // rather than adding a second set of rules through nearly the same place.
            int slot = -1;
            int oldest = -1;
            uint16_t oldestAge = 0;
            for (int i = 0; i < STRIKES; i++) {
                if (s_strike[i].live && s_strike[i].tag == (uint8_t)tag) { slot = i; break; }
                if (!s_strike[i].live) { if (slot < 0) slot = i; continue; }
                const uint16_t age = (uint16_t)(s_frame - s_strike[i].born);
                if (age >= oldestAge) { oldestAge = age; oldest = i; }
            }
            if (slot < 0) slot = oldest < 0 ? 0 : oldest;
            s_strike[slot].tag = (uint8_t)tag;
            s_strike[slot].born = s_frame;
            s_strike[slot].colour = uiVoiceColor((uint8_t)tag, 1.0f, all);
            s_strike[slot].live = true;
        }
    }

    // ---- aim the camera ----------------------------------------------------
    // The loudest bird takes the camera, but only once the last one has been quiet
    // for FOCUS_HOLD_MS. Whoever holds it keeps it while they sing.
    int loudestTag = -1;
    float loudestEnv = 0.0f;
    for (int tag = 0; tag < TRAIL_TAGS; tag++) {
        if (loudest[tag] > loudestEnv) {
            loudestEnv = loudest[tag];
            loudestTag = tag;
        }
    }
    if (s_focusTag >= 0 && loudest[s_focusTag] > 0.0f) {
        s_focusMs = now;
    } else if (loudestTag >= 0 && (s_focusTag < 0 || (now - s_focusMs) > FOCUS_HOLD_MS)) {
        s_focusTag = loudestTag;
        s_focusMs = now;
    }

    const float wantDist = (loudestTag >= 0) ? CAM_DIST_NEAR : CAM_DIST_IDLE;
    s_camDist += (wantDist - s_camDist) * DOLLY_EASE;

    // Follow the *head* of the focused song rather than the species' island: the
    // head is where the note is, and keeping it near the middle is what makes the
    // trail stream away behind it instead of wandering out of frame.
    float want[3];
    if (s_focusTag >= 0 && trailHead(s_trail[s_focusTag], want)) {
        s_camX += (want[0] - s_camX) * CAM_EASE;
        s_camY += (want[1] - s_camY) * CAM_EASE;
        s_camZ += (want[2] - s_camZ) * CAM_EASE;
    }

    // ---- draw: the strikes, under everything -------------------------------
    // Three axis-aligned rules through each live strike. The rules are world axes, so
    // they turn with the corpus and read as depth, but their *length* is quoted in
    // pixels against the band — GridBurst's `reachOfView`, and the reason a strike is
    // the same size on screen whether the camera is dollied in or out.
    s_canvas.fillSprite(TFT_BLACK);

    // Long enough to leave the frame from anywhere in it, whatever the camera is
    // doing: a rule that stops inside the picture is a cross, not a grid line.
    const float reachPx = STRIKE_REACH * sqrtf((float)(s_w * s_w + s_h * s_h));
    for (auto& st : s_strike) {
        if (!st.live) continue;
        const uint16_t age = (uint16_t)(s_frame - st.born);
        if (age >= STRIKE_LIFE || st.tag >= TRAIL_TAGS) {
            st.live = false;
            continue;
        }
        // Follow the bird: the cross hangs on wherever its song has got to.
        float at[3];
        if (!trailHead(s_trail[st.tag], at)) {
            st.live = false;
            continue;
        }
        float cx, cy, k;
        project(at[0], at[1], at[2], cx, cy, k);

        // In fast, out slow, exactly as the note it belongs to.
        const float rise = age < STRIKE_ATTACK ? (float)age / (float)STRIKE_ATTACK : 1.0f;
        const float fall = 1.0f - (float)age / (float)STRIKE_LIFE;
        const float amp = rise * fall * fall * STRIKE_AMP;
        if (amp <= 0.015f) continue;
        const uint16_t c = dimColour(st.colour, amp);
        if (!c) continue;

        for (int axis = 0; axis < 3; axis++) {
            // Where this world axis points on screen, from a short step along it.
            float ax = at[0], ay = at[1], az = at[2];
            const float eps = 0.05f;
            if (axis == 0) ax += eps; else if (axis == 1) ay += eps; else az += eps;
            float px, py, pk;
            project(ax, ay, az, px, py, pk);
            float dx = px - cx;
            float dy = py - cy;
            const float len = sqrtf(dx * dx + dy * dy);
            if (len < 1e-3f) continue;  // axis is pointing at the camera
            dx /= len;
            dy /= len;

            // Dashed, from the bird outward both ways, until it is off the band.
            for (int side = -1; side <= 1; side += 2) {
                for (float d = 0; d < reachPx; d += (float)(STRIKE_DASH + STRIKE_GAP)) {
                    const float x0 = cx + dx * d * (float)side;
                    const float y0 = cy + dy * d * (float)side;
                    if (x0 < -8.0f || y0 < -8.0f || x0 > (float)s_w + 8.0f
                        || y0 > (float)s_h + 8.0f) {
                        break;  // left the frame; the rest of this side is off-screen
                    }
                    const float x1 = cx + dx * (d + (float)STRIKE_DASH) * (float)side;
                    const float y1 = cy + dy * (d + (float)STRIKE_DASH) * (float)side;
                    s_canvas.drawLine((int)x0, (int)y0, (int)x1, (int)y1, c);
                }
            }
        }
    }

    // ---- draw: the roster, over the strikes ---------------------------------
    // One dot per *species*, not per tag: the two individuals of a species share an
    // island, so drawing both put two dots in the same place and made the constellation
    // look doubled. Brightness is the more recent of the pair's memories.
    const int roster = chorusRosterSize();
    for (int tag = 0; tag < roster && tag < TRAIL_TAGS; tag++) {
        const int sp = chorusSpeciesForTag((uint8_t)tag);
        if (sp < 0 || (size_t)(sp + 1) >= sizeof(GALAXY_ISLAND) / sizeof(GALAXY_ISLAND[0])) {
            continue;
        }
        // Skip a tag whose species an earlier tag already drew.
        bool drawn = false;
        for (int j = 0; j < tag; j++) {
            if (chorusSpeciesForTag((uint8_t)j) == sp) { drawn = true; break; }
        }
        if (drawn) continue;

        // How recently this species sang, over either of its individuals.
        uint32_t last = 0;
        for (int j = 0; j < roster && j < TRAIL_TAGS; j++) {
            if (chorusSpeciesForTag((uint8_t)j) == sp && s_lastSang[j] > last) {
                last = s_lastSang[j];
            }
        }
        if (!last) continue;  // never sung: not a mark
        const uint32_t since = now - last;
        if (since >= ROSTER_MEMORY_MS) continue;  // sang, but long enough ago to be gone
        float mem = 1.0f - (float)since / (float)ROSTER_MEMORY_MS;
        mem *= mem;  // most of the glow belongs to the last second or two

        const GalaxyPoint& p = GALAXY[GALAXY_ISLAND[sp]];
        float X, Y, persp;
        project((float)p.x * GALAXY_SCALE, (float)p.y * GALAXY_SCALE,
                (float)p.z * GALAXY_SCALE, X, Y, persp);
        if (X < -4.0f || Y < -4.0f || X > (float)s_w + 4.0f || Y > (float)s_h + 4.0f) continue;
        s_canvas.drawSpot((int)X, (int)Y, ROSTER_R * persp * (0.7f + 0.6f * mem),
                          uiVoiceColor((uint8_t)tag, ROSTER_ENV_MAX * mem, all));
    }

    // ---- draw: the songs, over it ------------------------------------------

    for (int tag = 0; tag < TRAIL_TAGS; tag++) {
        Trail& tr = s_trail[tag];
        if (!tr.n) continue;

        float prevX = 0, prevY = 0, prevR = 0;
        bool havePrev = false;
        int alive = 0;

        for (int i = 0; i < tr.n; i++) {
            const uint8_t k = (uint8_t)((tr.head + TRAIL_LEN - tr.n + i) % TRAIL_LEN);
            const uint16_t age = (uint16_t)(s_frame - tr.born[k]);
            if (age >= TRAIL_LEN) {
                havePrev = false;  // expired; the next live point starts a run
                continue;
            }
            alive++;

            float X, Y, persp;
            project(tr.x[k], tr.y[k], tr.z[k], X, Y, persp);

            // The dot's weight is how loud the bird was at that moment, faded by how
            // long ago it was and scaled by how near it is. uiVoiceColor's env
            // argument is the same dimming, so a song carries the hue the sweep
            // would have given that individual.
            const float fade = 1.0f - (float)age / (float)TRAIL_LEN;
            const float env = (float)tr.env[k] * (1.0f / 255.0f);
            const uint16_t c = uiVoiceColor((uint8_t)tag, fade * fade, all);
            float r = (DOT_R_QUIET + (DOT_R_LOUD - DOT_R_QUIET) * env) * fade * persp;
            if (r < 0.35f) r = 0.35f;

            if (havePrev && !tr.brk[k]) {
                // One dot stretched into the next: a wedge with its own radius at
                // each end. This is the whole reason a song reads as beads on a
                // thread rather than as a wire of constant weight.
                s_canvas.drawWedgeLine((int)prevX, (int)prevY, (int)X, (int)Y, prevR, r, c);
            } else {
                s_canvas.drawSpot((int)X, (int)Y, r, c);
            }
            prevX = X;
            prevY = Y;
            prevR = r;
            havePrev = true;

            // The travelling bead, over the newest dot: where the note is *now*.
            // Halo first, largest and faintest, so the rings land under the bead.
            if (i == tr.n - 1 && age < 2) {
                for (int ring = HALO_RINGS; ring >= 1; ring--) {
                    const float hr = BEAD_R * persp * (1.0f + HALO_STEP * (float)ring);
                    s_canvas.drawSpot((int)X, (int)Y, hr,
                                      dimColour(c, HALO_AMP / (float)ring));
                }
                s_canvas.drawSpot((int)X, (int)Y, BEAD_R * persp, c);
            }
        }

        // Nothing of this thread is still alive; let the slot go so a new song
        // starts from an empty ring rather than walking dead points.
        if (!alive) {
            tr.n = 0;
            if (s_focusTag == tag) s_focusTag = -1;
        }
    }

    s_canvas.pushSprite(s_x, s_y);
}
