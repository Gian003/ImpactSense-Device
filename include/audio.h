#pragma once

// DFPlayer Mini audio alerts.
// Tracks must be placed in the /mp3/ folder on the SD card, named 0001.mp3 … 0008.mp3.
// Call audioPlay() with one of the TRACK_* constants defined in config.h.
// If the DFPlayer is absent or the SD card is missing, audioPlay() is a no-op —
// all other features continue working normally.

void audioInit();
void audioPlay(int track);

// Stops whatever is playing. Used by the PLAY command in the provisioning
// window, so a 30 s alarm track does not have to be waited out to try another.
void audioStop();

// Whether the DFPlayer answered begin(). runProvisioningWindow() runs before
// audioInit() in setup(), so PLAY uses this to bring the module up on demand.
bool audioReady();

// Plays by index across every file on the card (command 0x03) instead of out
// of /mp3/ (0x12, what audioPlay uses). Only for finding out where the clips
// actually are when a card was copied into the root by mistake.
void audioPlayIndex(int track);

// Prints what the module reports: how many files it can see, what it is doing,
// and its volume. A healthy file count with no sound means the fault is after
// the decoder - the speaker or its pins. A count of 0 or -1 means the card.
void audioReport();

// Drains and explains any message frames the module volunteered, such as
// FileMismatch after asking for a track that is not there.
void audioPollMessages();

// 0-30, clamped. For ruling volume out at the bench. DFP_VOLUME is deliberately
// capped at 20 for the 0.5 W speaker, so leave anything higher set only as long
// as the test takes.
void audioSetVolume(int level);
