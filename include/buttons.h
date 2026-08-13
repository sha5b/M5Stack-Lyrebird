// Three logical buttons, whatever the board actually has.
//
// The Fire has A/B/C under the screen. The CoreS3 has no buttons at all — it is
// a touch panel — so there the same three live as zones along the bottom of the
// display, drawn by ui.cpp and fed into m5::Button_Class here.
//
// Going through Button_Class rather than reading touches directly is what keeps
// main.cpp free of board cases: press, release, and the hold thresholds that
// separate "next bird" from "volume up" all come out with identical semantics
// on both boards.
//
// M5Unified does map a strip below the CoreS3 screen onto BtnA/B/C, but that
// depends on the touch digitizer reporting coordinates past the bottom of the
// display, and a control surface you cannot see is a poor one regardless. The
// on-screen zones are drawn, labelled, and work whatever the panel reports.
#pragma once

#include <stdint.h>

enum : uint8_t { BTN_A = 0, BTN_B = 1, BTN_C = 2 };

void buttonsBegin();

// Call once per loop, after M5.update().
void buttonsUpdate();

bool btnPressedFor(uint8_t index, uint32_t ms);
bool btnWasReleased(uint8_t index);

// True when this board is driven by touch zones — the UI draws them instead of
// the keyboard-style hint line.
bool buttonsAreTouch();

// A touch at or below this y counts as a button press. Deliberately above where
// the labels are drawn: the visible strip is 29 px tall, which is a small target
// for a fingertip, so the area that responds is wider than the area that shows.
#define TOUCH_HIT_Y 200
