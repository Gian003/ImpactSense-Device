#include "mic.h"
#include "config.h"

#include <Arduino.h>

static int   gVoiceScore = 0;
static float gBaseline   = 2048.0f;

void micInit() {
  // GPIO 35 is input-only on ESP32 — no pinMode needed, but explicit is clearer.
  pinMode(MIC_PIN, INPUT);
  gVoiceScore = 0;

  // Measure where the microphone actually rests rather than assuming mid-rail.
  // The MAX9814 output sits on a ~1.25 V DC bias, which the ESP32's ADC reads
  // somewhere around 1500-1700 - and its nonlinearity moves that further. The
  // old fixed 2048 made the detector lopsided: a downward swing counted as
  // speech after ~300 counts, an upward one only after ~1100.
  long sum = 0;
  for (int i = 0; i < 64; i++) {
    sum += analogRead(MIC_PIN);
    delay(2);
  }
  gBaseline = sum / 64.0f;

  Serial.printf("MAX9814 mic ready on GPIO %d (resting level %.0f).\n", MIC_PIN, gBaseline);
}

void resetVoiceScore() {
  gVoiceScore = 0;
}

// Returns true once the leaky-integrator score reaches VOICE_SCORE_TARGET.
// +3 per sample above threshold, -1 per sample below, so sustained speech
// climbs while a single impact spike drains away before it gets there.
//
// This only answers "is the mic hearing something loud and sustained". It
// cannot tell the rider's voice from the device's own buzzer or from wind -
// that gating lives in crash_response.cpp, which knows when those are running.
bool checkVoiceActivity() {
  int raw = analogRead(MIC_PIN);

  // Tracks slow drift in the bias - temperature, the supply sagging while the
  // modem transmits - without following speech, which is far too fast to move
  // a 1/512 average.
  gBaseline += (raw - gBaseline) / 512.0f;

  int deviation = abs(raw - (int) gBaseline);

  if (deviation > MIC_VOICE_THRESHOLD) {
    // Capped AT the target. This used to read min(score + 3, 100) against a
    // target of 200, so the score could never arrive and voice cancel never
    // fired once - the feature was silently dead in every build.
    gVoiceScore = min(gVoiceScore + 3, VOICE_SCORE_TARGET);
  } else {
    gVoiceScore = max(gVoiceScore - 1, 0);
  }

  return (gVoiceScore >= VOICE_SCORE_TARGET);
}
