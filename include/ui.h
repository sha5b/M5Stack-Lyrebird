// The screen: chrome, and a band that draws the chorus.
//
// The band has two pictures, chosen at build time by LYREBIRD_UI_GALAXY. Both
// boards ship with the flag at 1, so the second one is what you get:
//
//   sweeping spectrogram (flag at 0) — every frame plots each sounding voice at
//     its current pitch on a log axis (250 Hz - 10 kHz), coloured by which
//     individual is singing and dimmed by its envelope, into a playhead column
//     that wraps around the band. Syllable contours draw themselves as the notes
//     sound, so a duet is two traces and the all-birds slot a field of them. It
//     says what pitch is sounding now.
//
//   the corpus as a point cloud (flag at 1, both boards) — see galaxy.h. It says
//     which of 2423 species is sounding, and where that species sits in the corpus.
//
// Nothing here clears the band except uiFullRedraw(). For the sweep that is
// load-bearing — the sweep *is* the history — so partial updates repaint text
// over itself (opaque background colour) rather than blanking regions.
#pragma once

#include <stdint.h>

void uiInit();

// Wipes the screen, draws all chrome, restarts the sweep at the left edge.
// Use when the selected bird changes: the picture should start over with it.
void uiFullRedraw();

// Repaints the header, name, sub-line, volume bar and hints in place. The band
// keeps its contents. Use for volume, mode and pause changes.
void uiChrome();

// One animation step: advances whichever picture the band is drawing and
// refreshes the live readouts. Call at roughly 25 Hz; it does not block.
void uiFrame();

// One colour per singing individual, so both band pictures colour a voice the
// same way. On the all-birds slot the roster is two birds per species with
// adjacent tags, so a species owns a region of the wheel and its two individuals
// differ only in brightness; on a single-species slot the four birds get four
// well-separated hues instead. `env` dims it.
uint16_t uiVoiceColor(uint8_t tag, float env, bool allBirds);
