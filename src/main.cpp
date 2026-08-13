// Lyrebird on the M5Stack Fire — bird-chorus synth, no SD card.
//
// Boots straight into chorus mode: a Poisson chorus of individual birds of
// the selected species, synthesized on-device by the Mindlin-Laje syrinx
// model (port of the Lyrebird web app's engine; see src/syrinx.cpp).
//
// Buttons:
//   A short = previous species   A hold = volume down
//   B short = chorus <-> solo    B hold = pause/resume
//   C short = next species       C hold = volume up
#include <Arduino.h>
#include <M5Stack.h>

#include "audio.h"
#include "bird_data.h"
#include "chorus.h"
#include "syrinx.h"

static bool needsRedraw = true;
static bool volHoldA = false;
static bool volHoldC = false;
static bool bHoldHandled = false;
static uint32_t lastVolStepMs = 0;

static void render() {
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setRotation(1);

    const SpeciesData& sp = SPECIES[chorusSpecies()];

    M5.Lcd.setTextColor(TFT_ORANGE, TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(8, 8);
    M5.Lcd.print("LYREBIRD");

    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(8, 44);
    M5.Lcd.printf("%-20.20s", sp.common);

    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(8, 72);
    M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Lcd.printf("species %d/%d", chorusSpecies() + 1, SPECIES_COUNT);

    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(8, 100);
    M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Lcd.printf("%s", chorusEnabled()
        ? (chorusMode() == MODE_CHORUS ? "chorus" : "solo  ")
        : "paused");

    // volume bar
    M5.Lcd.setCursor(8, 140);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextSize(1);
    M5.Lcd.printf("vol %3d%%", (int)(audioGetVolume() * 100.0f + 0.5f));
    M5.Lcd.drawRect(8, 152, 200, 12, TFT_DARKGREY);
    M5.Lcd.fillRect(9, 153, (int)(198 * audioGetVolume()), 10, TFT_CYAN);

    M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Lcd.setCursor(8, M5.Lcd.height() - 36);
    M5.Lcd.print("A/C species  B chorus/solo");
    M5.Lcd.setCursor(8, M5.Lcd.height() - 24);
    M5.Lcd.print("hold A/C vol  hold B pause");

    needsRedraw = false;
}

// live line: active voices + level, refreshed without a full redraw
static void renderStatus() {
    M5.Lcd.fillRect(8, 176, 304, 16, TFT_BLACK);
    M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(8, 180);
    M5.Lcd.printf("voices %d  level %.2f   ", syrinxActiveVoices(), syrinxLastLevel());
}

void setup() {
    M5.begin(true, false, true, true);  // LCD on, SD off, Serial on, I2C on
    M5.Power.begin();

    Serial.begin(115200);
    Serial.println("M5Stack Lyrebird starting...");

    M5.Lcd.setRotation(1);
    M5.Lcd.setBrightness(120);

    audioInit();
    chorusInit();
    audioSetRunning(true);  // boots into chorus: the standard mode

    needsRedraw = true;
}

void loop() {
    M5.update();
    chorusTick();

    // Button A: hold = volume down, short release = previous species
    if (M5.BtnA.pressedFor(300)) {
        volHoldA = true;
        uint32_t nowMs = millis();
        if (nowMs - lastVolStepMs >= 120) {
            audioSetVolume(audioGetVolume() - 0.02f);
            lastVolStepMs = nowMs;
            needsRedraw = true;
        }
    }
    if (M5.BtnA.wasReleased()) {
        if (!volHoldA) {
            chorusSetSpecies(chorusSpecies() - 1);
            needsRedraw = true;
        }
        volHoldA = false;
    }

    // Button B: short = chorus <-> solo, hold (1s) = pause/resume
    if (M5.BtnB.pressedFor(1000) && !bHoldHandled) {
        bHoldHandled = true;
        bool on = !chorusEnabled();
        chorusSetEnabled(on);
        audioSetRunning(on);
        needsRedraw = true;
    }
    if (M5.BtnB.wasReleased()) {
        if (!bHoldHandled && chorusEnabled()) {
            chorusSetMode(chorusMode() == MODE_CHORUS ? MODE_SOLO : MODE_CHORUS);
            needsRedraw = true;
        }
        bHoldHandled = false;
    }

    // Button C: hold = volume up, short release = next species
    if (M5.BtnC.pressedFor(300)) {
        volHoldC = true;
        uint32_t nowMs = millis();
        if (nowMs - lastVolStepMs >= 120) {
            audioSetVolume(audioGetVolume() + 0.02f);
            lastVolStepMs = nowMs;
            needsRedraw = true;
        }
    }
    if (M5.BtnC.wasReleased()) {
        if (!volHoldC) {
            chorusSetSpecies(chorusSpecies() + 1);
            needsRedraw = true;
        }
        volHoldC = false;
    }

    if (needsRedraw) render();

    static uint32_t lastStatusMs = 0;
    uint32_t now = millis();
    if (now - lastStatusMs >= 100) {
        lastStatusMs = now;
        renderStatus();
    }

    delay(5);
}
