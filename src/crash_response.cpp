#include "crash_response.h"
#include "config.h"
#include "backend.h"
#include "connectivity.h"
#include "gps.h"
#include "contact_cache.h"

#include <Arduino.h>

static void soundBuzzerPattern() {
  digitalWrite(BUZZER_PIN, (millis() / 250) % 2 == 0 ? HIGH : LOW);
}

void crashResponseInit() {
  pinMode(CANCEL_BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
}

// Three short beeps - deliberately distinct from the crash window's slow
// on/off blink, so the rider can tell "just a warning" apart from "emergency
// countdown in progress" by ear alone.
void handlePrecautionAlert() {
  Serial.println("*** PRECAUTION ALERT - abnormal balance/speed detected ***");

  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
  }
}

void handleCrash(float totalAccel, float totalGyro) {
  Serial.println("*** CRASH DETECTED - cancellation window started ***");
  Serial.printf("Press the cancel button within %lu seconds to abort.\n", CRASH_CANCEL_WINDOW_MS / 1000);

  bool cancelled = false;
  unsigned long windowStart = millis();

  while (millis() - windowStart < CRASH_CANCEL_WINDOW_MS) {
    soundBuzzerPattern();

    if (digitalRead(CANCEL_BUTTON_PIN) == LOW) {
      cancelled = true;
      break;
    }

    delay(20); // keeps the button responsive without pegging the CPU
  }

  digitalWrite(BUZZER_PIN, LOW);

  if (cancelled) {
    Serial.println("Crash report CANCELLED by rider.");
    return;
  }

  Serial.println("Cancellation window elapsed. Reporting crash...");
  reportCrash(totalAccel, totalGyro);

  // Sent independently of whether the HTTP report above succeeded - SMS only
  // needs cellular signal, not a working data/WiFi connection, so it can still
  // get through in exactly the scenario (rider/phone unreachable, weak signal)
  // this feature exists for.
  String riderName = getCachedRiderName();
  if (riderName.length() == 0) {
    riderName = "A registered ImpactSense rider";
  }

  String smsMessage = riderName + " may have been in a motorcycle accident. Severity: " +
                       severityFromImpact(totalAccel) + ". Location: https://maps.google.com/?q=" +
                       String(getLatitude(), 6) + "," + String(getLongitude(), 6);

  sendSms(getCachedContactPhone(), smsMessage);

  // Also dials the national emergency hotline directly from the device -
  // independent of the SMS/HTTP outcomes above, and independent of whether the
  // rider's own phone is reachable at all.
  callEmergencyHotline(EMERGENCY_HOTLINE_NUMBER);
}
