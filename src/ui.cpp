// See ui.h.
#include "ui.h"

#include <M5Unified.h>
#include <math.h>
#include <string.h>

#include "audio.h"
#include "bird_data.h"
#include "buttons.h"
#include "chorus.h"
#include "syrinx.h"

#if LYREBIRD_UI_GALAXY
#include "galaxy.h"
#endif

// ---- layout (320 x 240, rotation 1) ---------------------------------------
// Both the Fire and the CoreS3 are 320x240 in landscape. Read from the display
// rather than assuming, so a third board does not silently draw off-screen.
static int SCR_W = 320;
static int SCR_H = 240;

// Everything that is *text* lives in one filled block at the top, so the band
// gets the rest. The name is the largest thing on the screen because it is the
// thing you changed; the slot, the mode and the two load figures are one small
// line under it; the volume is the block's bottom edge rather than a widget.
//
// Text and picture used to alternate down the screen — header, name, sub-line,
// band, info line, volume bar, hints — which cost the band 40 px and left the
// name floating between two rules with no rank. One block, one band.
#define HEAD_H 36            // filled header block: name, sub-line, readouts
#define NAME_Y 4             // species / ALL BIRDS, size 2
#define SUB_Y 24             // slot + mode + roster, and dsp / voices at the right
#define VOL_Y HEAD_H         // the block's bottom edge *is* the volume readout
#define VOL_H 2
#define BAND_X 4
#define BAND_Y (VOL_Y + VOL_H + 2)
#define BAND_W 312
#define BAND_H 158
#define PLOT_X (BAND_X + 1)  // inner plot area, inside the frame
#define PLOT_Y (BAND_Y + 1)
#define PLOT_W (BAND_W - 2)
#define PLOT_H (BAND_H - 2)
// The bottom strip: the hint line on a board with real buttons, three labelled
// touch zones on one without. Same region either way. It must stay clear of
// TOUCH_HIT_Y (200) — the touch target is deliberately taller than the drawing,
// so the band has to end above it or a tap on the picture presses a button.
#define BAR_Y 211
#define BAR_H 29

// PLOT_W must stay a whole number of steps, or the playhead overruns the frame
// on the last column of a sweep.
#define SWEEP_STEP 2         // px of playhead advance per frame (310 / 2 = 155)

// pitch axis: 250 Hz .. 10 kHz, log
#define F_LO 250.0f
#define F_SPAN 40.0f         // F_LO * F_SPAN = 10 kHz

static const uint16_t C_BG = 0x0000;
static const uint16_t C_HEAD = 0x18E3;   // slate, the header bar fill
static const uint16_t C_RULE = 0x39C7;   // frame / separator grey
static const uint16_t C_DIM = 0x6B4D;    // secondary text
static const uint16_t C_TEXT = 0xFFFF;
static const uint16_t C_ACCENT = 0xFD20; // orange
static const uint16_t C_GRID = 0x1082;   // octave marks inside the band

#if !LYREBIRD_UI_GALAXY
static int s_sweepX = 0;
static float s_logSpan = 0;
#endif

// ---- helpers ---------------------------------------------------------------

static uint16_t hsv565(float h, float s, float v) {
    h -= floorf(h);
    float i = floorf(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    float r, g, b;
    switch ((int)i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return M5.Display.color565((uint8_t)(r * 255.0f), (uint8_t)(g * 255.0f), (uint8_t)(b * 255.0f));
}

// See ui.h. Not static: src/galaxy.cpp lights a sounding species in the same
// colour the sweep would have drawn it.
uint16_t uiVoiceColor(uint8_t tag, float env, bool allBirds) {
    float hue, val;
    if (allBirds) {
        // Hue spans the roster *sample*, not the corpus: with thousands of
        // species, dividing by SPECIES_COUNT would collapse every bird on screen
        // into the same few degrees of red.
        int pairs = chorusRosterSize() / 2;
        hue = (float)(tag / 2) / (float)(pairs > 0 ? pairs : 1);
        val = (tag & 1) ? 0.78f : 1.0f;
    } else {
        hue = 0.06f + 0.21f * (float)(tag & 3);
        val = 1.0f;
    }
    float e = env <= 0.0f ? 0.0f : (env > 1.0f ? 1.0f : env);
    // perceptual-ish: a quiet tail should still register, so pull the curve up
    val *= 0.25f + 0.75f * sqrtf(e);
    return hsv565(hue, 0.85f, val);
}

#if !LYREBIRD_UI_GALAXY
static int yForF0(float f0) {
    if (f0 < F_LO) f0 = F_LO;
    float lg = logf(f0 / F_LO) / s_logSpan;
    if (lg < 0.0f) lg = 0.0f;
    else if (lg > 1.0f) lg = 1.0f;
    int y = PLOT_Y + PLOT_H - 1 - (int)(lg * (float)(PLOT_H - 1));
    return y;
}
#endif

static void drawTextRight(int rightX, int y, uint8_t size, uint16_t fg, uint16_t bg,
                          const char* text) {
    int w = (int)strlen(text) * 6 * size;
    M5.Display.setTextSize(size);
    M5.Display.setTextColor(fg, bg);
    M5.Display.setCursor(rightX - w, y);
    M5.Display.print(text);
}

// ---- chrome ----------------------------------------------------------------

// The two live figures, in the header's right-hand corner. Repainted every frame
// straight over themselves — opaque background colour, no clear — so the format
// is fixed-width on purpose: right-aligned text that shrinks would leave the tail
// of the longer string behind it.
//
// dsp is the one worth watching. Past ~90 % the render task is about to miss the
// DMA, which is what a crackle sounds like — worth seeing before hearing.
static void drawReadouts() {
    const float load = audioGetLoad();
    int pct = (int)(load * 100.0f + 0.5f);
    if (pct > 999) pct = 999;
    char buf[40];
    snprintf(buf, sizeof(buf), "%d/%d voices   dsp %3d%%", syrinxActiveVoices(),
             SYRINX_MAX_VOICES, pct);
    drawTextRight(SCR_W - 6, SUB_Y, 1, load > 0.9f ? C_ACCENT : C_DIM, C_HEAD, buf);
}

// The header block, all of it. One fill and then text over it, because a
// partial repaint of a filled block has to repaint the fill anyway — and it is
// 36 rows, which is nothing next to the band it is protecting.
//
// The hierarchy, top to bottom and left to right: the bird's name, then what it
// is doing, then how hard the machine is working. Volume is the block's own
// bottom edge, since it is a quantity and not a control.
static void drawHead() {
    M5.Display.fillRect(0, 0, SCR_W, HEAD_H, C_HEAD);

    // ---- the name ----------------------------------------------------------
    const char* label = chorusLabel();
    const bool all = chorusAllBirds();
    // size 2 is 12 px per character, so 26 of them fill the width; the long
    // common names fall back to size 1 rather than being cut off.
    const uint8_t size = strlen(label) <= 24 ? 2 : 1;
    M5.Display.setTextSize(size);
    M5.Display.setTextColor(all ? C_ACCENT : C_TEXT, C_HEAD);
    M5.Display.setCursor(6, size == 2 ? NAME_Y : NAME_Y + 4);
    M5.Display.print(label);

    char buf[64];
    snprintf(buf, sizeof(buf), "vol %d%%", (int)(audioGetVolume() * 100.0f + 0.5f));
    drawTextRight(SCR_W - 6, NAME_Y + (size == 2 ? 4 : 4), 1, C_DIM, C_HEAD, buf);

    // ---- what it is doing --------------------------------------------------
    const char* state;
    if (!chorusEnabled()) state = "paused";
    else if (all) state = "chorus";
    else state = chorusMode() == MODE_CHORUS ? "chorus" : "solo";

    snprintf(buf, sizeof(buf), "%d/%d  %s  %d birds", chorusSlot() + 1,
             chorusSlotCount(), state, chorusRosterSize());
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(chorusEnabled() ? C_DIM : C_ACCENT, C_HEAD);
    M5.Display.setCursor(6, SUB_Y);
    M5.Display.print(buf);

    drawReadouts();
}

// Volume, as the 2 px edge the header block sits on. A quantity that changes
// while a button is held wants to be readable at a glance and to cost no
// vertical space at all, and a rule that fills does both.
static void drawVolumeBar() {
    const int fill = (int)((float)SCR_W * audioGetVolume());
    M5.Display.fillRect(0, VOL_Y, fill, VOL_H, C_ACCENT);
    if (fill < SCR_W) M5.Display.fillRect(fill, VOL_Y, SCR_W - fill, VOL_H, C_RULE);
}

// The bottom strip. On the Fire it names what the three physical buttons do; on
// the CoreS3 it *is* the three buttons, so it is drawn as targets rather than as
// a caption. The second line of each zone carries the hold action, which is the
// part that is otherwise undiscoverable on a touch board.
static void drawBottomStrip() {
    M5.Display.fillRect(0, BAR_Y, SCR_W, SCR_H - BAR_Y, C_BG);

    if (!buttonsAreTouch()) {
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(C_DIM, C_BG);
        M5.Display.setCursor(6, BAR_Y + 5);
        M5.Display.print("A/C bird  B chorus/solo  hold A/C vol  hold B pause");
        return;
    }

    static const char* top[3] = {"< BIRD", "MODE", "BIRD >"};
    static const char* sub[3] = {"hold vol-", "hold pause", "hold vol+"};
    const int zw = SCR_W / 3;
    for (int i = 0; i < 3; i++) {
        int x = i * zw;
        int w = (i == 2) ? SCR_W - x : zw;
        M5.Display.drawRect(x + 2, BAR_Y, w - 4, BAR_H, C_RULE);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(C_TEXT, C_BG);
        int tw = (int)strlen(top[i]) * 6;
        M5.Display.setCursor(x + (w - tw) / 2, BAR_Y + 6);
        M5.Display.print(top[i]);
        M5.Display.setTextColor(C_DIM, C_BG);
        int sw = (int)strlen(sub[i]) * 6;
        M5.Display.setCursor(x + (w - sw) / 2, BAR_Y + 17);
        M5.Display.print(sub[i]);
    }
}

static void drawBandFrame() {
    M5.Display.drawRect(BAND_X, BAND_Y, BAND_W, BAND_H, C_RULE);
    M5.Display.fillRect(PLOT_X, PLOT_Y, PLOT_W, PLOT_H, C_BG);
#if !LYREBIRD_UI_GALAXY
    // Octave marks. The galaxy has no frequency axis to rule — a mark's height
    // is its pitch, but the cloud turns, so a fixed line would be a lie.
    static const float marks[] = {500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f};
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(C_GRID, C_BG);
    for (float f : marks) {
        int y = yForF0(f);
        for (int x = PLOT_X; x < PLOT_X + PLOT_W; x += 6) M5.Display.drawPixel(x, y, C_GRID);
    }
#endif
}

// ---- public ----------------------------------------------------------------

void uiInit() {
#if !LYREBIRD_UI_GALAXY
    s_logSpan = logf(F_SPAN);
#endif
    M5.Display.setRotation(1);
    M5.Display.setTextWrap(false);
    SCR_W = M5.Display.width();
    SCR_H = M5.Display.height();
#if LYREBIRD_UI_GALAXY
    galaxyInit(PLOT_X, PLOT_Y, PLOT_W, PLOT_H);
#endif
}

void uiFullRedraw() {
    M5.Display.fillScreen(C_BG);
    drawHead();
    drawBandFrame();
    drawVolumeBar();
    drawBottomStrip();
#if LYREBIRD_UI_GALAXY
    galaxyReset();
#else
    s_sweepX = 0;
#endif
}

void uiChrome() {
    drawHead();
    drawVolumeBar();
    drawBottomStrip();
}

void uiFrame() {
    SyrinxVoiceInfo voices[SYRINX_MAX_VOICES];
    int n = syrinxSnapshot(voices, SYRINX_MAX_VOICES);

#if LYREBIRD_UI_GALAXY
    // ---- the cloud ---------------------------------------------------------
    galaxyFrame();
#else
    // ---- the sweep ---------------------------------------------------------
    bool all = chorusAllBirds();

    // clear the columns we are about to write, restoring the octave grid
    static const float marks[] = {500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f};
    for (int dx = 0; dx < SWEEP_STEP; dx++) {
        int col = s_sweepX + dx;
        M5.Display.drawFastVLine(PLOT_X + col, PLOT_Y, PLOT_H, C_BG);
        if ((col % 6) == 0) {
            for (float f : marks) M5.Display.drawPixel(PLOT_X + col, yForF0(f), C_GRID);
        }
    }

    for (int i = 0; i < n; i++) {
        if (voices[i].env <= 0.004f) continue;
        int y = yForF0(voices[i].f0);
        uint16_t c = uiVoiceColor(voices[i].tag, voices[i].env, all);
        // 3 px tall so a trace reads as a line rather than a dotted path
        int top = y - 1 < PLOT_Y ? PLOT_Y : y - 1;
        int h = (top + 3 > PLOT_Y + PLOT_H) ? PLOT_Y + PLOT_H - top : 3;
        M5.Display.fillRect(PLOT_X + s_sweepX, top, SWEEP_STEP, h, c);
    }

    s_sweepX = (s_sweepX + SWEEP_STEP) % PLOT_W;
    // leading edge, so the eye can find "now" in a full band
    M5.Display.drawFastVLine(PLOT_X + s_sweepX, PLOT_Y, PLOT_H, C_RULE);
#endif

    drawReadouts();
}
