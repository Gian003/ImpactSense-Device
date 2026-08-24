#pragma once

// MAX9814 microphone — Voice Activity Detection (VAD).
//
// Uses a leaky-integrator score: sustained speech raises the score above
// VOICE_SCORE_TARGET; a single impact spike decays before it can reach the
// threshold, so brief mechanical noise from the crash itself doesn't cancel
// the report.  Call checkVoiceActivity() every ~20 ms during the crash
// countdown.  Call resetVoiceScore() at the start of each new countdown so
// a leftover score from a previous event doesn't carry over.

void micInit();
bool checkVoiceActivity();
void resetVoiceScore();
