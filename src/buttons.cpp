// See buttons.h.
#include "buttons.h"

#include <M5Unified.h>

static bool s_touch = false;
static m5::Button_Class s_zone[3];

void buttonsBegin() {
    // The Fire has no touch panel, so M5.update() maintaining BtnA/B/C is all
    // that is needed there. Anything with touch gets the drawn zones.
    s_touch = M5.Touch.isEnabled();
}

bool buttonsAreTouch() { return s_touch; }

void buttonsUpdate() {
    if (!s_touch) return;

    const uint32_t ms = millis();
    const int w = M5.Display.width();
    bool down[3] = {false, false, false};

    // A finger can be down in more than one zone at once; that is fine, they are
    // independent buttons and the loop treats them as such.
    int n = M5.Touch.getCount();
    for (int i = 0; i < n; i++) {
        const auto& t = M5.Touch.getDetail(i);
        if (!t.isPressed() || t.y < TOUCH_HIT_Y) continue;
        int zone = (int)((int32_t)t.x * 3 / (w > 0 ? w : 320));
        if (zone < 0) zone = 0;
        else if (zone > 2) zone = 2;
        down[zone] = true;
    }

    // setRawState drives the same debounce and hold machinery the physical
    // buttons run through, so pressedFor()/wasReleased() mean the same thing.
    for (int i = 0; i < 3; i++) s_zone[i].setRawState(ms, down[i]);
}

static m5::Button_Class& physical(uint8_t index) {
    switch (index) {
        case BTN_A: return M5.BtnA;
        case BTN_B: return M5.BtnB;
        default: return M5.BtnC;
    }
}

bool btnPressedFor(uint8_t index, uint32_t ms) {
    if (index > BTN_C) return false;
    return s_touch ? s_zone[index].pressedFor(ms) : physical(index).pressedFor(ms);
}

bool btnWasReleased(uint8_t index) {
    if (index > BTN_C) return false;
    return s_touch ? s_zone[index].wasReleased() : physical(index).wasReleased();
}
