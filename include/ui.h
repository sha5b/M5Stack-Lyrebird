// The screen: a sweeping spectrogram of the chorus.
//
// Every frame plots each sounding voice at its current pitch on a log axis
// (250 Hz - 10 kHz), coloured by which individual is singing and dimmed by its
// envelope, into a playhead column that wraps around the band. Syllable
// contours draw themselves as the notes sound, so a duet is visible as two
// traces and the all-birds slot as a field of them.
//
// Nothing here clears the band except uiFullRedraw() — the sweep *is* the
// history, so partial updates repaint text over itself (opaque background
// colour) rather than blanking regions.
#pragma once

#include <stdint.h>

void uiInit();

// Wipes the screen, draws all chrome, restarts the sweep at the left edge.
// Use when the selected bird changes: the picture should start over with it.
void uiFullRedraw();

// Repaints the header, name, sub-line, volume bar and hints in place. The band
// keeps its contents. Use for volume, mode and pause changes.
void uiChrome();

// One animation step: advances the playhead one column and refreshes the live
// readouts. Call at roughly 25 Hz; it is cheap and does not block.
void uiFrame();
