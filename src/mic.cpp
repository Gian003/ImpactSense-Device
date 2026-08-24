#include "mic.h"
#include "config.h"

#include <Arduino.h>

static int gVoiceScore = 0;

void micInit() {
  // GPIO 35 is input-only on ESP32 — no pinMode needed, but explicit is clearer.
  pinMode(MIC_PIN, INPUT);
  gVoiceScore = 0;
  Serial.printf("MAX9814 mic ready on GPIO %d.\n", MIC_PIN);
}

void resetVoiceScore() {
  gVoiceScore = 0;
}

// Returns true once the leaky-integrator score reaches VOICE_SCORE_TARGET.
// +3 per sample above threshold, -1 per sample below — roughly 1.5 s of
// sustained speech at 20 ms polling to confirm, vs. a single-impact spike
// that drains before it accumulates enough.
bool checkVoiceActivity() {
  int raw = analogRead(MIC_PIN);
  int deviation = abs(raw - 2048); // 2048 = ADC midpoint at 3.3 V / 2

  if (deviation > MIC_VOICE_THRESHOLD) {
    gVoiceScore = min(gVoiceScore + 3, 100);
  } else {
    gVoiceScore = max(gVoiceScore - 1, 0);
  }

  return (gVoiceScore >= VOICE_SCORE_TARGET);
}
