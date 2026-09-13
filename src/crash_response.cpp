#include "crash_response.h"
#include "config.h"
#include "backend.h"
#include "connectivity.h"
#include "gps.h"
#include "contact_cache.h"
#include "audio.h"
#include "mic.h"
#include "telemetry.h"

#include <Arduino.h>

// ── init ──────────────────────────────────────────────────────────────────────

void crashResponseInit() {
  pinMode(CANCEL_BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Left untouched entirely when no motor is fitted — see VIBRATION_ENABLED
  // in config.h, which also carries the wiring the driver needs.
  if (VIBRATION_ENABLED) {
    pinMode(VIBRATION_PIN, OUTPUT);
    digitalWrite(VIBRATION_PIN, LOW);
  }
}

// ── helpers ───────────────────────────────────────────────────────────────────

static void setVibration(bool on) {
  if (VIBRATION_ENABLED) {
    digitalWrite(VIBRATION_PIN, on ? HIGH : LOW);
  }
}

static void soundBuzzerPattern() {
  // Slow 250 ms on/off blink — distinct from the precaution alert's three quick beeps
  // so the rider knows by ear that an emergency countdown is in progress.
  digitalWrite(BUZZER_PIN, (millis() / 250) % 2 == 0 ? HIGH : LOW);

  // Driven from the same clock as the buzzer so the two channels never
  // disagree, but with a much longer duty: an ERM needs ~50 ms just to spin
  // up, so a symmetric 250 ms blink is felt as a weak flutter rather than a
  // pulse. 750 on / 250 off reads unambiguously as "sustained", which is what
  // an emergency countdown should feel like next to the precaution's three
  // distinct taps.
  setVibration((millis() % 1000) < 750);
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

// Three short pulses — deliberately different from the crash countdown's slow
// sustained pattern so the rider can tell "just a warning" from "emergency in
// progress" without looking at anything.
void handlePrecautionAlert() {
  telemetryEvent("PRECAUTION - unstable riding detected. Warning the rider.");
  telemetryDetail("Below crash level, so nothing is reported. Buzzer only.");

  // 100 ms is enough for a piezo, which is effectively instant, but an ERM
  // spends the first ~50 ms of that just getting up to speed and would be
  // felt as a faint tick. The longer pulse applies only when a motor is
  // actually fitted, so the buzzer-only cadence stays exactly as it was.
  const unsigned long onMs = VIBRATION_ENABLED ? 160 : 100;

  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    setVibration(true);
    delay(onMs);

    digitalWrite(BUZZER_PIN, LOW);
    setVibration(false);
    delay(100);
  }
}

// ── crash response ────────────────────────────────────────────────────────────

void handleCrash(float totalAccel, float totalGyro) {
  char line[96];
  telemetryEvent("CRASH DETECTED");
  snprintf(line, sizeof(line),
           "Rider has %lu seconds to cancel - press the button, or just speak.",
           CRASH_CANCEL_WINDOW_MS / 1000);
  telemetryDetail(line);

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
  setVibration(false); // the countdown loop can exit mid-pulse, on any of its three paths

  if (cancelled) {
    if (voiceCancelled) {
      telemetryEvent("CANCELLED - the rider spoke, so they are conscious.");
      telemetryDetail("Nothing reported. No SMS, no call, no incident record.");
      audioPlay(TRACK_VOICE_CANCEL);
    } else {
      telemetryEvent("CANCELLED - the rider pressed the button.");
      telemetryDetail("Nothing reported. No SMS, no call, no incident record.");
      audioPlay(TRACK_CANCELLED);
    }
    return;
  }

  telemetryEvent("NO RESPONSE from the rider - treating this as a real crash.");
  audioPlay(TRACK_REPORTING);

  // SMS to the rider's emergency contact — always fires via SIM800L (cellular
  // registration only, no internet needed) as a belt-and-suspenders channel
  // regardless of whether the backend is reachable, since it goes to a
  // different recipient on a completely independent path.
  //
  // Sent BEFORE the HTTP report, not after. reportCrash() can sit for the best
  // part of half a minute inside ensureConnectivity() — a 5 s WiFi retry, then
  // cellular registration, then a 10 s HTTP timeout — and every one of those
  // delays is longest in exactly the situation where the backend is going to
  // fail anyway. Running the SMS afterwards put the most reliable channel on
  // the crash path last and latest, gated behind the least reliable one.
  telemetryDetail("1. Texting the emergency contact...");
  sendCrashSms(totalAccel);

  // HTTP POST to backend (creates incident, triggers Pusher + Twilio).
  // Returns true if the backend acknowledged the report (HTTP 201).
  // Returns false if offline — queued in NVS flash and retried later.
  telemetryDetail("2. Reporting to the command centre...");
  bool backendReached = reportCrash(totalAccel, totalGyro);

  audioPlay(TRACK_REPORT_SENT);

  // TOC voice call — two paths, one outcome:
  //
  //   Backend reachable → Twilio (server-side) already placed an AI voice
  //   call to the emergency contact and the TOC dashboard lit up via Pusher.
  //   No need for a second call from the device — skip it.
  //
  //   Backend unreachable → Twilio never fired. Fall back to the SIM800L
  //   direct GSM call to the TOC hotline, playing the pre-recorded message
  //   through the call (DFPlayer DAC_R → 100 kΩ → SIM800L MIC+).
  if (!backendReached) {
    telemetryDetail("   Command centre unreachable - calling the hotline directly instead.");
    if (callStart(EMERGENCY_HOTLINE_NUMBER)) {
      delay(CALL_CONNECT_WAIT_MS);  // allow ring + answer before message starts
      audioPlay(TRACK_CALL_TOC);    // TOC hears this through the call
      delay(CALL_HOLD_MS);          // hold line while message plays
      callEnd();
    }
  } else {
    telemetryDetail("   Command centre has it. The dashboard and its callout take over.");
  }

  telemetryEvent("Response complete. Back to monitoring.");
}

// ── manual SOS ────────────────────────────────────────────────────────────────

// Hold the cancel button for MANUAL_SOS_HOLD_MS outside any crash window to fire SOS.
// Tracked with simple edge detection — no RTOS timer needed.
static unsigned long gSosPressStart = 0;
static bool          gSosButtonHeld = false;

static void triggerManualSos() {
  telemetryEvent("MANUAL SOS - the rider held the button down.");
  telemetryDetail("No cancellation window: this was deliberate.");
  audioPlay(TRACK_MANUAL_SOS);

  // SMS first, for the same reason as handleCrash() — see the note there.
  telemetryDetail("1. Texting the emergency contact...");
  sendManualSosSms();

  // Treat as a "high"-severity crash so the backend creates a proper incident record.
  telemetryDetail("2. Reporting to the command centre...");
  bool backendReached = reportCrash(ACCEL_THRESHOLD * 1.5f, GYRO_THRESHOLD);

  // Same two-path TOC call logic as handleCrash — Twilio if backend is up,
  // SIM800L pre-recorded fallback if backend is unreachable.
  if (!backendReached) {
    telemetryDetail("   Command centre unreachable - calling the hotline directly instead.");
    if (callStart(EMERGENCY_HOTLINE_NUMBER)) {
      delay(CALL_CONNECT_WAIT_MS);
      audioPlay(TRACK_CALL_TOC);
      delay(CALL_HOLD_MS);
      callEnd();
    }
  } else {
    telemetryDetail("   Command centre has it. The dashboard and its callout take over.");
  }

  telemetryEvent("Response complete. Back to monitoring.");
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
