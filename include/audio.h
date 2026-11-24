#pragma once

// DFPlayer Mini audio alerts.
// Tracks must be placed in the /mp3/ folder on the SD card, named 0001.mp3 … 0010.mp3
// (0001-0008 are the alert clips; 0009 and 0010 are the optional assurance
// cues, and their absence costs nothing - see TRACK_ONLINE in config.h).
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

// Re-sends the two settings a module loses if it resets: SD as the output
// device, and the volume we last asked for (which may be the rider's choice
// from the app, not DFP_VOLUME).
//
// Cheap, inaudible, and safe to call whenever the module might have dropped
// out from under us - a 5 V rail that sagged while the radio transmitted
// leaves a DFPlayer that answers normally and plays at the wrong volume from
// the wrong storage.
void audioResync();

// Whether the module still answers, and still agrees with us about its own
// volume. False means one of: it reset, the serial link is not getting
// replies back, or this is a clone that never answers queries at all. The
// caller gets all three possibilities printed rather than a guess.
bool audioLinkAlive();

// Waits for whatever is playing to finish, up to maxMs, then returns.
//
// For use immediately before an audioPlay() that would otherwise interrupt
// the previous clip - playMp3Folder() replaces, it does not queue. Measured
// from when that clip STARTED, so work done in between is already counted:
// if it took longer than the clip, this returns at once and costs nothing.
//
// Never put one of these before an SMS, a report or a call. See the note at
// AUDIO_CLIP_MAX_WAIT_MS in config.h for why the placement is the whole
// safety argument.
void audioWaitForClip(unsigned long maxMs);

// Plays a track only if it is one of the optional assurance cues and the
// conditions for one are met: AUDIO_ASSURANCE_ENABLED, and no emergency clip
// in the last AUDIO_ASSURANCE_QUIET_MS. Returns whether it played.
bool audioAssuranceCue(int track);

// Call every loop with whether the backend is currently reachable. Announces
// TRACK_ONLINE on the transition into reachable - at boot and again after a
// recovery mid-ride - rate-limited by AUDIO_ONLINE_CUE_COOLDOWN_MS. Costs a
// comparison on the boring path.
void audioOnlineCueTick(bool online);
