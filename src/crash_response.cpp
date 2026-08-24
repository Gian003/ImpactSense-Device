#include "crash_response.h"
#include "config.h"
#include "backend.h"
#include "connectivity.h"
#include "gps.h"
#include "contact_cache.h"
#include "audio.h"
#include "mic.h"

#include <Arduino.h>

// ── init ──────────────────────────────────────────────────────────────────────

void crashResponseInit() {
  pinMode(CANCEL_BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
}

// ── helpers ───────────────────────────────────────────────────────────────────

static void soundBuzzerPattern() {
  // Slow 250 ms on/off blink — distinct from the precaution alert's three quick beeps
  // so the rider knows by ear that an emergency countdown is in progress.
  digitalWrite(BUZZER_PIN, (millis() / 250) % 2 == 0 ? HIGH : LOW);
}

static void sendCrashSms(float totalAccel) {
  String riderName = getCachedRiderName();
  if (riderName.length() == 0) {
    riderName = "A registered ImpactSense rider";
  }

  String msg = riderName + " may have been in a motorcycle accident. Severity: " +
               severityFromImpact(totalAccel) + ". Location: https://maps.google.com/?q=" +
               String(getLatitude(), 6) + "," + String(getLongitude(), 6);

  sendSms(getCachedContactPhone(), msg);
}

static void sendManualSosSms() {
  String riderName = getCachedRiderName();
  if (riderName.length() == 0) {
    riderName = "A registered ImpactSense rider";
  }

  String msg = riderName + " has triggered a manual SOS. Location: https://maps.google.com/?q=" +
               String(getLatitude(), 6) + "," + String(getLongitude(), 6);

  sendSms(getCachedContactPhone(), msg);
}

// ── precaution alert ──────────────────────────────────────────────────────────

// Three short beeps — deliberately different from the crash countdown's slow
// blink so the rider can tell "just a warning" from "emergency in progress."
void handlePrecautionAlert() {
  Serial.println("*** PRECAUTION ALERT - abnormal balance/speed detected ***");

  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
  }
}

// ── crash response ────────────────────────────────────────────────────────────

void handleCrash(float totalAccel, float totalGyro) {
  Serial.println("*** CRASH DETECTED - cancellation window started ***");
  Serial.printf("Press the cancel button within %lu seconds, or speak to cancel.\n",
                CRASH_CANCEL_WINDOW_MS / 1000);

  audioPlay(TRACK_CRASH_DETECTED);
  resetVoiceScore(); // start fresh so a previous event's score doesn't carry over

  bool cancelled      = false;
  bool voiceCancelled = false;
  unsigned long windowStart = millis();

  while (millis() - windowStart < CRASH_CANCEL_WINDOW_MS) {
    soundBuzzerPattern();

    if (digitalRead(CANCEL_BUTTON_PIN) == LOW) {
      cancelled = true;
      break;
    }

    if (checkVoiceActivity()) {
      cancelled      = true;
      voiceCancelled = true;
      break;
    }

    delay(20); // ~50 Hz polling — keeps button and mic responsive without pegging the CPU
  }

  digitalWrite(BUZZER_PIN, LOW);

  if (cancelled) {
    if (voiceCancelled) {
      Serial.println("Crash report CANCELLED — voice activity detected (rider is conscious).");
      audioPlay(TRACK_VOICE_CANCEL);
    } else {
      Serial.println("Crash report CANCELLED — button pressed by rider.");
      audioPlay(TRACK_CANCELLED);
    }
    return;
  }

  Serial.println("Cancellation window elapsed. Reporting crash...");
  audioPlay(TRACK_REPORTING);

  // HTTP POST to backend (creates incident, triggers Pusher + Twilio).
  // Non-fatal if internet is down — queued in NVS and retried later.
  reportCrash(totalAccel, totalGyro);

  // SMS to rider's emergency contact — cellular only, no internet needed.
  sendCrashSms(totalAccel);

  audioPlay(TRACK_REPORT_SENT);

  // Voice call to TOC — dial, wait for answer, then transmit pre-recorded message.
  // TRACK_CALL_TOC plays through the helmet speaker AND into the call via SIM800L MIC+
  // (hardware: DFPlayer DAC_R pin 4 → 100 kΩ → SIM800L MIC+, SIM800L MIC− → GND).
  if (callStart(EMERGENCY_HOTLINE_NUMBER)) {
    delay(CALL_CONNECT_WAIT_MS);  // allow ring + answer before message starts
    audioPlay(TRACK_CALL_TOC);    // TOC hears this through the call
    delay(CALL_HOLD_MS);          // hold line while message plays
    callEnd();
  }
}

// ── manual SOS ────────────────────────────────────────────────────────────────

// Hold the cancel button for MANUAL_SOS_HOLD_MS outside any crash window to fire SOS.
// Tracked with simple edge detection — no RTOS timer needed.
static unsigned long gSosPressStart = 0;
static bool          gSosButtonHeld = false;

static void triggerManualSos() {
  Serial.println("*** MANUAL SOS - button held for 3 seconds ***");
  audioPlay(TRACK_MANUAL_SOS);

  // Treat as a "high"-severity crash so the backend creates a proper incident record.
  reportCrash(ACCEL_THRESHOLD * 1.5f, GYRO_THRESHOLD);
  sendManualSosSms();

  // Same call+audio sequence as a detected crash.
  if (callStart(EMERGENCY_HOTLINE_NUMBER)) {
    delay(CALL_CONNECT_WAIT_MS);
    audioPlay(TRACK_CALL_TOC);
    delay(CALL_HOLD_MS);
    callEnd();
  }
}

void checkManualSos() {
  bool pressed = (digitalRead(CANCEL_BUTTON_PIN) == LOW);

  if (pressed && !gSosButtonHeld) {
    // Fresh press — start the hold timer.
    gSosPressStart = millis();
    gSosButtonHeld = true;
    return;
  }

  if (!pressed) {
    // Released before threshold — reset.
    gSosButtonHeld = false;
    gSosPressStart = 0;
    return;
  }

  // Still held — check if we've crossed the threshold.
  if (millis() - gSosPressStart >= MANUAL_SOS_HOLD_MS) {
    gSosButtonHeld = false; // prevent re-triggering on the same hold
    gSosPressStart = 0;
    triggerManualSos();
  }
}
