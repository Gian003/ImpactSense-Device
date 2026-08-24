#pragma once

// DFPlayer Mini audio alerts.
// Tracks must be placed in the /mp3/ folder on the SD card, named 0001.mp3 … 0007.mp3.
// Call audioPlay() with one of the TRACK_* constants defined in config.h.
// If the DFPlayer is absent or the SD card is missing, audioPlay() is a no-op —
// all other features continue working normally.

void audioInit();
void audioPlay(int track);
