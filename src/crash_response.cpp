#include "crash_response.h"
#include "config.h"
#include "backend.h"
#include "connectivity.h"
#include "gps.h"
#include "contact_cache.h"
#include "device_identity.h"
#include "audio.h"
#include "mic.h"
#include "sensors.h"
#include "telemetry.h"

#include <Arduino.h>

// Countdown alert timing. Named once because two things must agree on it: the
// pattern that drives the buzzer and motor, and the voice gate that refuses to
// listen while they are running. If these drifted apart, the device would
// start hearing its own buzzer as the rider talking.
static const unsigned long BUZZER_HALF_PERIOD_MS = 250;  // 250 on, 250 off
static const unsigned long VIB_PERIOD_MS         = 1000;
static const unsigned long VIB_ON_MS             = 750;

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
  digitalWrite(BUZZER_PIN, (millis() / BUZZER_HALF_PERIOD_MS) % 2 == 0 ? HIGH : LOW);

  // Driven from the same clock as the buzzer so the two channels never
  // disagree, but with a much longer duty: an ERM needs ~50 ms just to spin
  // up, so a symmetric 250 ms blink is felt as a weak flutter rather than a
  // pulse. 750 on / 250 off reads unambiguously as "sustained", which is what
  // an emergency countdown should feel like next to the precaution's three
  // distinct taps.
  setVibration((millis() % VIB_PERIOD_MS) < VIB_ON_MS);
}

// Whether a microphone sample taken right now can be trusted to mean the rider
// is speaking.
//
// Voice cancel was dead until the score cap in mic.cpp was fixed. Bringing it
// to life also made it capable of cancelling a REAL crash on noise the device
// makes or experiences itself, so the fix and this gate have to ship together.
// Every rule below fails toward reporting: when in doubt, don't listen. The
// cancel button is never gated, so a conscious rider always has a way out.
static bool voiceMayCountNow(unsigned long windowStart) {
  unsigned long now = millis();

  // The loudest moments of a crash - the impact, scraping, the bike going
  // down - all land in its first seconds, and so does the spoken "crash
  // detected" prompt, played through a speaker next to the microphone.
  if (now - windowStart < VOICE_CANCEL_GRACE_MS) {
    return false;
  }

  // The buzzer sits centimetres from the mic, and the MAX9814's automatic gain
  // will turn it up. At a 50% duty cycle it scores +3 far more often than it
  // leaks -1, and would cancel the countdown within seconds with nobody
  // speaking. So listen only in its off half, after it has rung down.
  if ((now % (2 * BUZZER_HALF_PERIOD_MS)) < BUZZER_HALF_PERIOD_MS + SELF_NOISE_SETTLE_MS) {
    return false;
  }

  // Same for the motor: acoustic buzz, and brush noise on the rail the ADC shares.
  if (VIBRATION_ENABLED && (now % VIB_PERIOD_MS) < VIB_ON_MS + SELF_NOISE_SETTLE_MS) {
    return false;
  }

  // A helmet still sliding or tumbling is in wind, and wind noise easily
  // clears MIC_VOICE_THRESHOLD. Only a helmet that has come to rest - or is
  // moving about as much as someone standing up and walking - is listened to.
  // A failed read is treated as moving: unknown should never cancel a report.
  MotionReading m;
  readMotion(m);
  if (m.substituted ||
      fabsf(m.totalAccel - 9.81f) > VOICE_MOTION_ACCEL_DELTA ||
      m.totalGyro > VOICE_MOTION_GYRO) {
    return false;
  }

  return true;
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

// What the backup voice call cannot say.
//
// The call plays a fixed recording, so the TOC hears that there has been an
// accident and nothing else - not who, not where, not how bad. This text
// carries those, and states that no incident was filed, which is the part a
// dispatcher would otherwise have to work out by finding nothing on the board.
//
// Runs only on the offline path. Two segments on most networks; worth it.
static void sendTocFallbackSms(const String &what, float totalAccel, bool withSeverity) {
  if (strlen(TOC_SMS_NUMBER) == 0) {
    Serial.println("TOC fallback SMS skipped: TOC_SMS_NUMBER is empty (see config.h).");
    return;
  }

  String riderName = getCachedRiderName();
  if (riderName.length() == 0) {
    riderName = "An ImpactSense rider";
  }

  String msg = "ImpactSense: " + riderName + " " + what + ".";
  if (withSeverity) {
    msg += " Severity: " + severityFromImpact(totalAccel) + ".";
  }
  msg += " Map: https://maps.google.com/?q=" +
         String(getLatitude(), 6) + "," + String(getLongitude(), 6) +
         " Device " + getDeviceCode() +
         ". Server unreachable - this crash is NOT on the dashboard.";

  sendSms(TOC_SMS_NUMBER, msg);
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
           "Rider has %lu seconds to cancel - press the button at any time,",
           CRASH_CANCEL_WINDOW_MS / 1000);
  telemetryDetail(line);
  // Said out loud because otherwise it looks broken in a demo: someone speaks
  // straight away, nothing happens, and it reads as a failed feature rather
  // than the deliberate grace period it is.
  snprintf(line, sizeof(line),
           "or speak once the alert has finished (voice is ignored for %lu s).",
           VOICE_CANCEL_GRACE_MS / 1000);
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

    // Gate first: when the gate says no, the mic is not sampled at all, so the
    // score holds where it was instead of absorbing noise or decaying away
    // speech that was already heard.
    if (voiceMayCountNow(windowStart) && checkVoiceActivity()) {
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
    telemetryDetail("   Command centre unreachable - texting and calling the hotline directly.");

    // Text first, then call. The text arrives in seconds and the call takes
    // most of a minute, so the dispatcher is reading who and where while the
    // phone is still ringing - rather than hearing a recording about an
    // accident they cannot place.
    sendTocFallbackSms("may have been in a motorcycle accident", totalAccel, true);

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
    telemetryDetail("   Command centre unreachable - texting and calling the hotline directly.");

    // Same as handleCrash(): the recording cannot say this was deliberate,
    // or who pressed it, or where they are.
    sendTocFallbackSms("has triggered a manual SOS", 0.0f, false);

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
