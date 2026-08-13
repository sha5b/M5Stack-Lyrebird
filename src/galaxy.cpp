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
#define CAM_DIST_NEAR 1.5f   // one bird singing: pushed right in
#define CAM_DIST_FAR 6.0f    // a long song, or several far apart: as far back as it goes
#define CAM_DIST_IDLE 2.2f   // nothing singing: eased back a little

// The share of the band the singing should fill, measured **in screen space** on both
// axes — the largest of |x| / (w/2) and |y| / (h/2) over the current gesture.
//
// This is what makes the framing adaptive, and it is measured over *everything being
// drawn*, not just over the heads. Two consequences, both wanted:
//
//   two birds at once   the distance is whatever fits both, so they are shown
//                       together instead of the camera choosing one and losing the
//                       other off the frame.
//   one long song       the thread grows as it is sung, so the fit grows with it and
//                       the camera eases back to keep the gesture in view. Measured on
//                       heads alone it never moved, and a long song simply ran off the
//                       edge at full zoom.
//
// The *target* stays the centroid of the heads, though — not of the whole gesture — so
// the note being sung stays near the middle and the thread streams away behind it
// rather than the head drifting out to an edge.
//
// Screen space rather than a world radius, and that is a correction. Solving
// `radius * ZOOM * CAM_F / (FIT_FRAC * h/2)` quoted the fit against the half-*height*
// alone, so on a 310 x 156 band a broadside gesture — which is what the yaw steering
// works to produce — was fitted to the short axis and used a third of the long one.
// Measuring the projected extent instead is naturally right on both axes and folds in
// STRETCH_X without having to think about it. It costs being a feedback loop on last
// frame's distance rather than a closed form, which at these easing rates is invisible.
#define FIT_FRAC 0.80f

// A bird stays in the frame's reckoning this long after it stops sounding, so the
// shot does not snap wider the instant a syllable ends.
#define FRAME_HOLD_MS 1200
#define CAM_F 2.0f

// How fast the dolly moves between those two, per frame. Slower than the pan, so a
// push-in reads as a push-in rather than as the picture changing size.
#define DOLLY_EASE 0.035f

// Pixels per layout unit at the target's depth.
//
// Set so that *activity owns the screen*. A song is one to two layout units of
// gesture, and this band is 310 x 156, so anything under about 140 leaves the thing
// you are meant to be watching as a small squiggle in a large dark rectangle — which
// is what it was at 78 and still was at 100. Long songs now run off the frame, which
// is fine: the camera holds the head, and the head is the part worth seeing.
#define ZOOM 145.0f

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

// Turn periods, in seconds per revolution: the slow drift that keeps a still moment
// from being quite still.
#define TURN_X_S 197.0f
#define TURN_Y_S 131.0f

/**
 * How hard the yaw is pulled toward seeing the current gesture broadside, per frame,
 * and how far back along the thread the gesture's direction is measured.
 *
 * A song is mostly a line through space, and a line seen end-on is a dot. Left to a
 * clock, the drift spends part of every cycle doing exactly that — the gesture
 * collapses, and the most legible thing on the screen becomes the least. So the yaw
 * has a target as well as a rate: the angle that puts the thread across the view
 * rather than along it. The clock keeps pushing away from it and this keeps pulling
 * back, which leaves the camera hovering near broadside with a slow wobble instead of
 * either sitting still or rolling through the bad angle.
 *
 * Only the yaw is steered. Pitch stays on its clock, because a gesture that is
 * vertical in world terms is already broadside on the screen's other axis, and
 * steering both would leave nothing moving.
 */
#define YAW_EASE 0.020f
#define GESTURE_SPAN 10

// ---- the gesture ----------------------------------------------------------

// How far a song's thread advances per frame, in layout units. The median song is six
// syllables of 67 ms, so it has about twenty frames — at this zoom that is a gesture
// most of the way across the band, and a short two-syllable song still crosses a
// third of it.
#define GROW 0.10f

// Pitch, across the thread — outward from the middle of the corpus, since that is
// the thread's cross axis. A high note pushes away from the centre, a low one pulls
// in, on the same log axis the spectrogram uses.
#define WIGGLE 0.55f

// The corkscrew, which is what gives perspective something to work on.
#define TWIST_AMP 0.15f
#define TWIST_RATE 0.16f  // radians per frame: about half a turn per song

// ---- what is *not* drawn --------------------------------------------------
//
// There is no static layer at all, at the third attempt. The corpus was drawn whole,
// then as a faint bed, then as the roster — the two dozen birds that could sing — and
// every version had the same fault in a smaller form: marks that sit there not
// meaning anything. Fading the roster dots by how recently their bird sang did not
// fix it either, because a dot on its way out still reads as a dot that is doing
// nothing.
//
// So the band shows only what is happening: songs, the crosses struck through them,
// and the haloed bead at the note being sung. Between songs it goes dark, and that is
// the correct picture of nothing happening. Trails last about three seconds and songs
// arrive about every two, so it is rarely empty for long.

// ---- the lattice ----------------------------------------------------------
//
// A very faint 3D grid, and the distinction from what GridBurst rejected matters:
// that was a *screen-space* lattice, which does not move with the cloud and so fights
// the depth it is meant to establish. This one is in world space. It rotates, it
// parallaxes, and it is the thing the camera moves against — which only became worth
// having once the camera started moving at all.
//
// The lattice is snapped to the camera in whole steps rather than pinned to the origin,
// so it is effectively infinite: the camera can travel anywhere in the corpus and the
// grid is always around it, sliding by in step increments as it goes. That sliding *is*
// the parallax; a lattice fixed to the origin would run out as soon as the camera left
// the middle.
//
// Faint to the point of being barely there, but **not by multiplying a dark colour
// down**, which is how the first version of this came out invisible. The panel is
// RGB565: five bits of blue, so the smallest step it can show at all is 8/255. Taking a
// grey-blue of (26, 34, 52), converting to 565, and scaling it by 0.07 for depth gives
// (1.7, 2.3, 3.5) — and every one of those truncates to zero on the way back. The grid
// was not faint, it was black, and it was black at 0.10 too.
//
// So the levels are hand-picked in 565 terms instead: four entries whose blue channel is
// 1, 2, 3 and 4 of 31, which is the whole usable range between invisible and noticeable.
// Depth chooses between them. Nothing is multiplied.
#define GRID_STEP 0.55f   // lattice spacing, layout units
#define GRID_N 2          // lattice lines each side of the camera, per axis
#define GRID_SEGS 8       // segments per line: enough to survive perspective
#define GRID_LEVELS 4

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


static M5Canvas s_canvas(&M5.Display);
static bool s_ready = false;

static int s_x = 0, s_y = 0, s_w = 0, s_h = 0;
static uint16_t s_frame = 0;

// Where the camera is looking and how far back it is.
static float s_camX = 0, s_camY = 0, s_camZ = 0;
static float s_camDist = CAM_DIST_IDLE;

// The camera's angles, carried rather than derived from the frame counter: the yaw is
// steered as well as driven (see YAW_EASE) so it cannot be a pure function of time.
static float s_yaw = 0, s_pitch = 0;
static float s_cax = 1, s_sax = 0, s_cay = 1, s_say = 0;

// When each tag last sounded, for the framing. 0 = never.
static uint32_t s_sangMs[TRAIL_TAGS];

// The lattice's four depth levels, built once. See the note on GRID_LEVELS for why
// these are literals in 565 terms rather than a colour scaled by a float.
static uint16_t s_grid[GRID_LEVELS];

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
 * As project(), but false when the point is at or behind the near plane — where the
 * perspective divide flips a line inside out instead of clipping it. The lattice needs
 * this because its lines pass through the camera; nothing else in the picture does.
 */
static inline bool projectFront(float x, float y, float z, float& outX, float& outY,
                                float& outK) {
    const float dx = x - s_camX;
    const float dy = y - s_camY;
    const float dz = z - s_camZ;
    const float z1 = -dx * s_say + dz * s_cay;
    const float z2 = dy * s_sax + z1 * s_cax;
    if (s_camDist + z2 < 0.35f) return false;
    project(x, y, z, outX, outY, outK);
    return true;
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

/** Wrap an angle difference into [-pi, pi]. */
static inline float wrapPi(float a) {
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
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

/**
 * Which way a thread is running, from its head back GESTURE_SPAN points. False when
 * it is too short or too tight to have a direction worth aiming at.
 */
static bool trailDirection(const Trail& tr, float* d) {
    if (tr.n < 3) return false;
    const int span = tr.n - 1 < GESTURE_SPAN ? tr.n - 1 : GESTURE_SPAN;
    const uint8_t a = (uint8_t)((tr.head + TRAIL_LEN - 1) % TRAIL_LEN);
    const uint8_t b = (uint8_t)((tr.head + TRAIL_LEN - 1 - span) % TRAIL_LEN);
    d[0] = tr.x[a] - tr.x[b];
    d[1] = tr.y[a] - tr.y[b];
    d[2] = tr.z[a] - tr.z[b];
    const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-3f) return false;
    d[0] /= len; d[1] /= len; d[2] /= len;
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
    // Blue channel 1, 2, 3, 4 of 31. Checked against color565(): every one of these
    // survives the round trip, which is the whole point of writing them out.
    s_grid[0] = M5.Display.color565(0, 4, 8);
    s_grid[1] = M5.Display.color565(4, 8, 16);
    s_grid[2] = M5.Display.color565(8, 12, 24);
    s_grid[3] = M5.Display.color565(12, 16, 33);

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
    memset(s_sangMs, 0, sizeof(s_sangMs));
    s_frame = 0;
    s_yaw = 0.0f;
    s_pitch = 0.0f;
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

    // The clock's share of the drift. The yaw gets a target as well, further down,
    // once there is a gesture to aim at.
    s_pitch += 6.283185f / TURN_X_S * 0.04f;  // uiFrame() is paced at 25 Hz
    s_yaw += 6.283185f / TURN_Y_S * 0.04f;

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
        s_sangMs[tag] = now;

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

    // ---- who is loudest, and which birds are in the shot -------------------
    // Every bird that has sounded within FRAME_HOLD_MS is framed, so the shot does not
    // snap wider the instant a syllable ends.
    int loudestTag = -1;
    float loudestEnv = 0.0f;
    int inShot[TRAIL_TAGS];
    float head[TRAIL_TAGS][3];
    int shot = 0;
    float sum[3] = {0.0f, 0.0f, 0.0f};

    for (int tag = 0; tag < TRAIL_TAGS; tag++) {
        if (loudest[tag] > loudestEnv) {
            loudestEnv = loudest[tag];
            loudestTag = tag;
        }
        if (!s_sangMs[tag] || (now - s_sangMs[tag]) > FRAME_HOLD_MS) continue;
        if (!trailHead(s_trail[tag], head[shot])) continue;
        sum[0] += head[shot][0];
        sum[1] += head[shot][1];
        sum[2] += head[shot][2];
        inShot[shot] = tag;
        shot++;
    }

    // ---- steer the yaw away from looking down the gesture -------------------
    // The view direction in world terms is (-say*cax, sax, cay*cax); its horizontal
    // part is (-say, cay). Broadside means that is perpendicular to the thread's
    // horizontal direction, which is one atan2. The two solutions are pi apart, so take
    // whichever is the shorter turn from here — the other is a 180 degree swing to an
    // identical-looking shot.
    float g[3];
    if (loudestTag >= 0 && trailDirection(s_trail[loudestTag], g)) {
        if (g[0] * g[0] + g[2] * g[2] > 0.02f) {  // a thread straight up has no yaw to fix
            const float wantYaw = atan2f(g[2], g[0]);
            float d = wrapPi(wantYaw - s_yaw);
            const float alt = wrapPi(wantYaw + 3.14159265f - s_yaw);
            if (fabsf(alt) < fabsf(d)) d = alt;
            s_yaw += d * YAW_EASE;
        }
    }

    s_cax = cosf(s_pitch);
    s_sax = sinf(s_pitch);
    s_cay = cosf(s_yaw);
    s_say = sinf(s_yaw);

    // ---- pan to the singers, then fit them ---------------------------------
    if (shot > 0) {
        const float cx = sum[0] / (float)shot;
        const float cy = sum[1] / (float)shot;
        const float cz = sum[2] / (float)shot;
        s_camX += (cx - s_camX) * CAM_EASE;
        s_camY += (cy - s_camY) * CAM_EASE;
        s_camZ += (cz - s_camZ) * CAM_EASE;

        // How much of the band the current gesture actually covers, at the distance we
        // are at now. Walked backwards from each head and stopped at the first `brk`,
        // which is where this song started: the ring still holds the previous song's
        // points and they sit a long way off, so including them drove the fit to nearly
        // twice the distance it wanted and shrank a growing song instead of following it.
        float fill = 0.0f;
        for (int i = 0; i < shot; i++) {
            const Trail& tr = s_trail[inShot[i]];
            for (int j = 0; j < tr.n; j++) {
                const uint8_t k = (uint8_t)((tr.head + TRAIL_LEN - 1 - j) % TRAIL_LEN);
                if ((uint16_t)(s_frame - tr.born[k]) >= TRAIL_LEN) break;
                float X, Y, persp;
                project(tr.x[k], tr.y[k], tr.z[k], X, Y, persp);
                const float fx = fabsf(X - (float)s_w * 0.5f) / ((float)s_w * 0.5f);
                const float fy = fabsf(Y - (float)s_h * 0.5f) / ((float)s_h * 0.5f);
                if (fx > fill) fill = fx;
                if (fy > fill) fill = fy;
                if (tr.brk[k]) break;  // start of this song; earlier points are another
            }
        }

        float want = fill > 0.01f ? s_camDist * fill / FIT_FRAC : CAM_DIST_NEAR;
        if (want < CAM_DIST_NEAR) want = CAM_DIST_NEAR;
        else if (want > CAM_DIST_FAR) want = CAM_DIST_FAR;
        s_camDist += (want - s_camDist) * DOLLY_EASE;
    } else {
        s_camDist += (CAM_DIST_IDLE - s_camDist) * DOLLY_EASE;
    }

    // ---- draw: the lattice, under everything -------------------------------
    s_canvas.fillSprite(TFT_BLACK);

    {
        // Snap the lattice to the camera in whole steps: the grid is always around
        // wherever the camera has got to, and slides by as it travels.
        const float base[3] = {
            floorf(s_camX / GRID_STEP) * GRID_STEP,
            floorf(s_camY / GRID_STEP) * GRID_STEP,
            floorf(s_camZ / GRID_STEP) * GRID_STEP,
        };
        const float span = GRID_STEP * (float)GRID_N;

        for (int axis = 0; axis < 3; axis++) {
            const int b = (axis + 1) % 3;
            const int c = (axis + 2) % 3;
            for (int i = -GRID_N; i <= GRID_N; i++) {
                for (int j = -GRID_N; j <= GRID_N; j++) {
                    float p[3];
                    p[b] = base[b] + (float)i * GRID_STEP;
                    p[c] = base[c] + (float)j * GRID_STEP;

                    float prevX = 0, prevY = 0;
                    bool havePrev = false;
                    for (int k = 0; k <= GRID_SEGS; k++) {
                        p[axis] = base[axis] - span
                                  + 2.0f * span * (float)k / (float)GRID_SEGS;
                        float X, Y, persp;
                        if (!projectFront(p[0], p[1], p[2], X, Y, persp)) {
                            havePrev = false;
                            continue;
                        }
                        if (havePrev) {
                            // Depth is the only thing keeping this readable: without it
                            // the far lattice is as strong as the near one and the band
                            // turns to mesh. It picks a level, it does not scale a colour.
                            float d = persp * s_camDist / CAM_F;  // 1 at the target depth
                            int lvl = (int)((d - 0.55f) * (float)GRID_LEVELS / 0.9f);
                            if (lvl < 0) lvl = 0;
                            else if (lvl >= GRID_LEVELS) lvl = GRID_LEVELS - 1;
                            s_canvas.drawLine((int)prevX, (int)prevY, (int)X, (int)Y,
                                              s_grid[lvl]);
                        }
                        prevX = X;
                        prevY = Y;
                        havePrev = true;
                    }
                }
            }
        }
    }

    // ---- draw: the strikes, over the lattice --------------------------------
    // Three axis-aligned rules through each live strike. The rules are world axes, so
    // they turn with the corpus and read as depth, but their *length* is quoted in
    // pixels against the band — GridBurst's `reachOfView`, and the reason a strike is
    // the same size on screen whether the camera is dollied in or out.
    //
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
        if (!alive) tr.n = 0;
    }

    s_canvas.pushSprite(s_x, s_y);
}
