// Lyrebird on the M5Stack Fire and CoreS3 — bird-chorus synth, no SD card.
//
// Boots into ALL BIRDS: every one of the 12 species at once, two individuals
// each, synthesized on-device by the Mindlin-Laje syrinx model (port of the
// Lyrebird web app's engine; see src/syrinx.cpp). A/C step off that slot into
// one species at a time.
//
// Buttons:
//   A short = previous bird     A hold = volume down
//   B short = chorus <-> solo   B hold = pause/resume
//   C short = next bird         C hold = volume up
//
// On the Fire those are the three physical buttons. The CoreS3 has none, so the
// same three are touch zones drawn along the bottom of the screen — see
// src/buttons.cpp, which makes both look identical from here.
#include <Arduino.h>
#include <M5Unified.h>

#include "audio.h"
#include "bird_data.h"
#include "buttons.h"
#include "chorus.h"
#include "syrinx.h"
#include "ui.h"

/**
 * Backlight brightness, and why it is pinned at maximum.
 *
 * The backlight is dimmed by PWM on a pin a couple of centimetres from GPIO25,
 * which on the Fire is a high-impedance analog DAC output feeding a speaker
 * amplifier — a switching load of some tens of milliamps right next to the one
 * analog line on the board. At full brightness the duty cycle saturates and the
 * pin sits constantly high, so there is no switching at all.
 *
 * Lower this if the speaker is quiet and you would rather have a dimmer screen.
 * It is also the experiment worth running first if the Fire still hisses with
 * nothing sounding: the difference between 255 and, say, 120 is the answer to
 * "is that noise the backlight or the DAC".
 *
 * The CoreS3 does not care — its audio never touches an analog pin.
 */
#define BACKLIGHT 255

#define VOL_HOLD_MS 300     // press longer than this and A/C mean volume
#define VOL_STEP_MS 120
#define VOL_STEP 0.02f
#define PAUSE_HOLD_MS 1000
#define FRAME_MS 40         // 25 fps sweep

static bool volHoldA = false;
static bool volHoldC = false;
static bool bHoldHandled = false;
static uint32_t lastVolStepMs = 0;

void setup() {
    auto cfg = M5.config();
    // On the Fire we drive the DAC on GPIO25 ourselves, so M5Unified must not
    // also claim it. On the CoreS3 the opposite: its Speaker owns the AW88298
    // bring-up over I2C, and src/audio_spk.cpp streams into it.
#if LYREBIRD_AUDIO_SPK
    cfg.internal_spk = true;
#else
    cfg.internal_spk = false;
#endif
    cfg.internal_mic = false;
    M5.begin(cfg);

    Serial.begin(115200);
    Serial.printf("Lyrebird starting on board %d\n", (int)M5.getBoard());

    M5.Display.setRotation(1);
    M5.Display.setBrightness(BACKLIGHT);

    buttonsBegin();
    uiInit();
    audioInit();
    chorusInit();
    audioSetRunning(true);  // boots into the all-birds chorus: the standard mode

    uiFullRedraw();
}

void loop() {
    M5.update();
    buttonsUpdate();
    chorusTick();

    bool chromeDirty = false;
    bool fullDirty = false;

    // Button A: hold = volume down, short release = previous bird
    if (btnPressedFor(BTN_A, VOL_HOLD_MS)) {
        volHoldA = true;
        uint32_t nowMs = millis();
        if (nowMs - lastVolStepMs >= VOL_STEP_MS) {
            audioSetVolume(audioGetVolume() - VOL_STEP);
            lastVolStepMs = nowMs;
            chromeDirty = true;
        }
    }
    if (btnWasReleased(BTN_A)) {
        if (!volHoldA) {
            chorusSetSlot(chorusSlot() - 1);
            fullDirty = true;
        }
        volHoldA = false;
    }

    // Button B: short = chorus <-> solo, hold = pause/resume. On the all-birds
    // slot a short press does nothing: that slot is a chorus by definition.
    if (btnPressedFor(BTN_B, PAUSE_HOLD_MS) && !bHoldHandled) {
        bHoldHandled = true;
        bool on = !chorusEnabled();
        chorusSetEnabled(on);
        audioSetRunning(on);
        chromeDirty = true;
    }
    if (btnWasReleased(BTN_B)) {
        if (!bHoldHandled && chorusEnabled() && !chorusAllBirds()) {
            chorusSetMode(chorusMode() == MODE_CHORUS ? MODE_SOLO : MODE_CHORUS);
            chromeDirty = true;
        }
        bHoldHandled = false;
    }

    // Button C: hold = volume up, short release = next bird
    if (btnPressedFor(BTN_C, VOL_HOLD_MS)) {
        volHoldC = true;
        uint32_t nowMs = millis();
        if (nowMs - lastVolStepMs >= VOL_STEP_MS) {
            audioSetVolume(audioGetVolume() + VOL_STEP);
            lastVolStepMs = nowMs;
            chromeDirty = true;
        }
    }
    if (btnWasReleased(BTN_C)) {
        if (!volHoldC) {
            chorusSetSlot(chorusSlot() + 1);
            fullDirty = true;
        }
        volHoldC = false;
    }

    // a new bird restarts the picture; everything else repaints text in place
    if (fullDirty) uiFullRedraw();
    else if (chromeDirty) uiChrome();

    static uint32_t lastFrameMs = 0;
    uint32_t now = millis();
    if (now - lastFrameMs >= FRAME_MS) {
        lastFrameMs = now;
        uiFrame();
    }

    delay(4);
}
