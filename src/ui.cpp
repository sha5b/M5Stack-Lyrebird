// See ui.h.
#include "ui.h"

#include <M5Unified.h>
#include <math.h>
#include <string.h>

#include "audio.h"
#include "bird_data.h"
#include "chorus.h"
#include "syrinx.h"

// ---- layout (320 x 240, rotation 1) ---------------------------------------
// Both the Fire and the CoreS3 are 320x240 in landscape. Read from the display
// rather than assuming, so a third board does not silently draw off-screen.
static int SCR_W = 320;
static int SCR_H = 240;

#define HEAD_H 20            // filled header bar
#define NAME_Y 26            // species / ALL BIRDS
#define SUB_Y 48             // slot, mode, roster size
#define BAND_X 4
#define BAND_Y 62
#define BAND_W 312
#define BAND_H 134
#define PLOT_X (BAND_X + 1)  // inner plot area, inside the frame
#define PLOT_Y (BAND_Y + 1)
#define PLOT_W (BAND_W - 2)
#define PLOT_H (BAND_H - 2)
#define INFO_Y 200
#define VOL_Y 212
#define VOL_H 9
#define HINT_Y 228

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

static int s_sweepX = 0;
static float s_logSpan = 0;

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

// One colour per singing individual. On the all-birds slot the roster is two
// birds per species with adjacent tags, so a species owns a region of the wheel
// and its two individuals differ only in brightness; on a single-species slot
// the four birds get four well-separated hues instead.
static uint16_t voiceColor(uint8_t tag, float env, bool allBirds) {
    float hue, val;
    if (allBirds) {
        hue = (float)(tag / 2) / (float)SPECIES_COUNT;
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

static int yForF0(float f0) {
    if (f0 < F_LO) f0 = F_LO;
    float lg = logf(f0 / F_LO) / s_logSpan;
    if (lg < 0.0f) lg = 0.0f;
    else if (lg > 1.0f) lg = 1.0f;
    int y = PLOT_Y + PLOT_H - 1 - (int)(lg * (float)(PLOT_H - 1));
    return y;
}

static void drawTextRight(int rightX, int y, uint8_t size, uint16_t fg, uint16_t bg,
                          const char* text) {
    int w = (int)strlen(text) * 6 * size;
    M5.Display.setTextSize(size);
    M5.Display.setTextColor(fg, bg);
    M5.Display.setCursor(rightX - w, y);
    M5.Display.print(text);
}

// ---- chrome ----------------------------------------------------------------

static void drawHeader() {
    M5.Display.fillRect(0, 0, SCR_W, HEAD_H, C_HEAD);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(C_ACCENT, C_HEAD);
    M5.Display.setCursor(6, 7);
    M5.Display.print("LYREBIRD");

    char buf[16];
    snprintf(buf, sizeof(buf), "vol %3d%%", (int)(audioGetVolume() * 100.0f + 0.5f));
    drawTextRight(SCR_W - 6, 7, 1, C_TEXT, C_HEAD, buf);
    M5.Display.drawFastHLine(0, HEAD_H, SCR_W, C_RULE);
}

static void drawName() {
    const char* label = chorusLabel();
    bool all = chorusAllBirds();
    // size 2 fits 25 characters; the long common names fall back to size 1
    uint8_t size = strlen(label) <= 25 ? 2 : 1;
    M5.Display.fillRect(0, NAME_Y - 2, SCR_W, 20, C_BG);
    M5.Display.setTextSize(size);
    M5.Display.setTextColor(all ? C_ACCENT : C_TEXT, C_BG);
    M5.Display.setCursor(6, size == 2 ? NAME_Y : NAME_Y + 4);
    M5.Display.print(label);
}

static void drawSub() {
    char buf[64];
    const char* state;
    if (!chorusEnabled()) state = "paused";
    else if (chorusAllBirds()) state = "chorus";
    else state = chorusMode() == MODE_CHORUS ? "chorus" : "solo";

    snprintf(buf, sizeof(buf), "%d/%d  %s  %d birds%s",
             chorusSlot() + 1, chorusSlotCount(), state, chorusRosterSize(),
             chorusAllBirds() ? ", all species" : "");
    M5.Display.fillRect(0, SUB_Y, SCR_W, 8, C_BG);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(chorusEnabled() ? C_DIM : C_ACCENT, C_BG);
    M5.Display.setCursor(6, SUB_Y);
    M5.Display.print(buf);
}

static void drawVolumeBar() {
    int w = SCR_W - 12;
    M5.Display.drawRect(6, VOL_Y, w, VOL_H, C_RULE);
    int fill = (int)((float)(w - 2) * audioGetVolume());
    M5.Display.fillRect(7, VOL_Y + 1, fill, VOL_H - 2, C_ACCENT);
    if (fill < w - 2) M5.Display.fillRect(7 + fill, VOL_Y + 1, w - 2 - fill, VOL_H - 2, C_BG);
}

static void drawHints() {
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(C_DIM, C_BG);
    M5.Display.setCursor(6, HINT_Y);
    M5.Display.print("A/C bird  B chorus/solo  hold A/C vol  hold B pause");
}

static void drawBandFrame() {
    M5.Display.drawRect(BAND_X, BAND_Y, BAND_W, BAND_H, C_RULE);
    M5.Display.fillRect(PLOT_X, PLOT_Y, PLOT_W, PLOT_H, C_BG);
    // octave marks, labelled at the left edge
    static const float marks[] = {500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f};
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(C_GRID, C_BG);
    for (float f : marks) {
        int y = yForF0(f);
        for (int x = PLOT_X; x < PLOT_X + PLOT_W; x += 6) M5.Display.drawPixel(x, y, C_GRID);
    }
}

// ---- public ----------------------------------------------------------------

void uiInit() {
    s_logSpan = logf(F_SPAN);
    M5.Display.setRotation(1);
    M5.Display.setTextWrap(false);
    SCR_W = M5.Display.width();
    SCR_H = M5.Display.height();
}

void uiFullRedraw() {
    M5.Display.fillScreen(C_BG);
    drawHeader();
    drawName();
    drawSub();
    drawBandFrame();
    drawVolumeBar();
    drawHints();
    s_sweepX = 0;
}

void uiChrome() {
    drawHeader();
    drawName();
    drawSub();
    drawVolumeBar();
    drawHints();
}

void uiFrame() {
    // ---- the sweep ---------------------------------------------------------
    SyrinxVoiceInfo voices[SYRINX_MAX_VOICES];
    int n = syrinxSnapshot(voices, SYRINX_MAX_VOICES);
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
        uint16_t c = voiceColor(voices[i].tag, voices[i].env, all);
        // 3 px tall so a trace reads as a line rather than a dotted path
        int top = y - 1 < PLOT_Y ? PLOT_Y : y - 1;
        int h = (top + 3 > PLOT_Y + PLOT_H) ? PLOT_Y + PLOT_H - top : 3;
        M5.Display.fillRect(PLOT_X + s_sweepX, top, SWEEP_STEP, h, c);
    }

    s_sweepX = (s_sweepX + SWEEP_STEP) % PLOT_W;
    // leading edge, so the eye can find "now" in a full band
    M5.Display.drawFastVLine(PLOT_X + s_sweepX, PLOT_Y, PLOT_H, C_RULE);

    // ---- live readouts (opaque text: no clear, no flicker) -----------------
    char buf[32];
    snprintf(buf, sizeof(buf), "voices %d/%d", n, SYRINX_MAX_VOICES);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(C_DIM, C_BG);
    M5.Display.setCursor(6, INFO_Y);
    M5.Display.print(buf);

    // DSP load. Past ~0.9 the render task is about to miss the DMA, which is
    // what a crackle sounds like — worth seeing before hearing.
    float load = audioGetLoad();
    int pct = (int)(load * 100.0f + 0.5f);
    if (pct > 999) pct = 999;
    snprintf(buf, sizeof(buf), "dsp %3d%%", pct);
    drawTextRight(SCR_W - 6, INFO_Y, 1, load > 0.9f ? C_ACCENT : C_DIM, C_BG, buf);
}
