// The band as arms: one growing 3D filament per singing bird.
//
// A bird starts a song, and an arm grows out of that species' place in the
// corpus — bending with the pitch it is singing, corkscrewing as it runs so the
// perspective has something to work on. It grows while the song lasts, then
// hangs and fades. Where an arm *starts* is the species' position in the same
// layout the flasher page draws (tools/generate_galaxy.py writes both, from
// include/bird_data.h), so the board and the page agree about where a bird is.
//
// It answers a different question than the spectrogram it replaces. The sweep
// says what pitch is sounding now; the arms say which of 2423 birds is singing,
// where that bird sits among the rest, and what its song is doing.
//
// Enabled per board by LYREBIRD_UI_GALAXY (platformio.ini), and both boards ship
// with it on. Costs the band as an off-screen canvas (~89 KB of DRAM allocated at
// runtime), 36 KB of static RAM for the trails, and 54 KB of flash for the layout.
#pragma once

#include <stdint.h>

// The band to draw into, in screen pixels. Call once, after the display is up.
// If the canvas will not allocate, every call below becomes a no-op and the band
// stays black — the rest of the screen is unaffected.
void galaxyInit(int x, int y, int w, int h);

// Drop every arm and restart the turn. Use where uiFullRedraw() would restart
// the sweep: the picture should begin again with the bird that is chosen.
void galaxyReset();

// One step: grow the arms of whoever is sounding, re-project every live point,
// and push the band. Call at the UI frame rate — unlike the sweep it wants all
// 25 Hz, because the shortest syllables are around 50 ms and an arm that misses
// them stops tracking the song.
void galaxyFrame();
