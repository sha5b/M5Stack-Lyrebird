// Lyrebird on the M5Stack Fire and CoreS3 — bird-chorus synth, no SD card.
//
// Boots into ALL BIRDS: a rolling sample of the corpus, twelve species at a
// time and two individuals each, synthesized on-device by the Mindlin-Laje
// syrinx model (port of the Lyrebird web app's engine; see src/syrinx.cpp).
// A/C step off that slot into one species at a time.
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
#define FRAME_MS 40         // 25 fps

/**
 * Paused this long and the board turns itself off.
 *
 * A pause is someone putting the thing down, and a paused Lyrebird is a lit
 * screen drawing current to show nothing happening. What "off" means is not the
 * same on both boards, and neither is a choice this code gets to make:
 *
 *   CoreS3   AXP2101, so M5.Power.powerOff() cuts the rail. Actually off; the
 *            side button brings it back.
 *   Fire     IP5306, where powerOff() only releases the boost keep-on and
 *            returns. On battery the regulator then drops the rail, which is
 *            off. On USB it cannot — the charger is holding it up — so the call
 *            comes back and the board simply stays here, dark and paused.
 *
 * Which is why there is no deep sleep after it; see sleepNow().
 */
#define PAUSE_OFF_MS 60000

static bool volHoldA = false;
static bool volHoldC = false;
static bool bHoldHandled = false;
static uint32_t lastVolStepMs = 0;
static uint32_t lastToggleMs = 0;
static uint32_t pausedSinceMs = 0;  // 0 = not paused
// Screen off after the pause timeout on a board that could not cut its own power.
// Any button brings it back; the chorus stays paused, because nothing restarted it.
static bool dark = false;

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

/**
 * Paused long enough: say so, then go dark.
 *
 * **No deep sleep, and that is the fix for a real bug.** This used to call
 * `powerOff()` and then `deepSleep(0, true)`. On the Fire that pair fights itself —
 * `powerOff()` sets the IP5306's boost keep-on *false* and returns, then `deepSleep()`
 * sets it *true* again — and worse, a deep sleep ends in a **reboot**, which runs
 * setup() and starts the chorus. So a paused Fire would go quiet, sleep, take any
 * wake on its wakeup pin, and come back *singing*. From the outside that is exactly
 * "I paused it and after a while it started again", and the second attempt looking
 * like it worked is the same race landing the other way.
 *
 * So: silence the audio, blank the screen, and try `powerOff()`. On the CoreS3 that
 * is an AXP2101 rail cut and nothing below runs. On a Fire it returns, and the board
 * simply stays here — dark, paused, drawing next to nothing, and *not* rebooting. A
 * button press wakes the screen back up (see loop()), and the chorus is still paused
 * because nothing ever restarted it.
 */
static void sleepNow() {
    Serial.println("pause timeout: going dark");
    audioSetRunning(false);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(6, 6);
    M5.Display.print("paused - powering off");
    delay(900);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setBrightness(0);

    M5.Power.powerOff();
    // Only reached where the rail cannot be cut. Stay dark and stay paused.
    dark = true;
}

void loop() {
    M5.update();
    buttonsUpdate();
    chorusTick();

    if (!chorusEnabled()) {
        if (!pausedSinceMs) pausedSinceMs = millis();
        else if (millis() - pausedSinceMs >= PAUSE_OFF_MS && !dark) sleepNow();
    } else {
        pausedSinceMs = 0;
    }

    // Dark: the only thing that matters is a button, and the first one only wakes
    // the screen. Swallowing it is deliberate — waking into a volume change or a new
    // bird because of the press that woke you is worse than needing two presses.
    if (dark) {
        if (btnWasReleased(BTN_A) || btnWasReleased(BTN_B) || btnWasReleased(BTN_C)) {
            dark = false;
            pausedSinceMs = millis();
            M5.Display.setBrightness(BACKLIGHT);
            uiFullRedraw();
        }
        delay(20);
        return;
    }

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
    // pressedFor() is true on *every* pass once the hold threshold is crossed, not
    // once — bHoldHandled is what makes it one toggle per hold. The time guard is
    // belt and braces: whatever the button layer does, pause and resume cannot land
    // inside the same gesture.
    if (btnPressedFor(BTN_B, PAUSE_HOLD_MS) && !bHoldHandled
        && millis() - lastToggleMs > 400) {
        bHoldHandled = true;
        lastToggleMs = millis();
        bool on = !chorusEnabled();
        chorusSetEnabled(on);
        audioSetRunning(on);
        Serial.printf("B hold: chorus %s\n", on ? "on" : "paused");
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
