#include <Arduino.h>

#include "config.h"
#include "sensors.h"
#include "gps.h"
#include "connectivity.h"
#include "backend.h"
#include "crash_response.h"
#include "contact_cache.h"

unsigned long lastCrashReportAt = 0;
unsigned long lastHelmetStatusAt = 0;
unsigned long lastPrecautionAlertAt = 0;

void setup() {
  Serial.begin(115200);
  delay(500); // give Serial Monitor time to attach after upload
  Serial.println();
  Serial.println("ImpactSense - Full Firmware (Modules 1-7)");

  sensorsInit();
  crashResponseInit();
  gpsInit();
  modemInit();          // brings up SIM800 for SMS, independent of WiFi/GPRS
  contactCacheInit();   // loads last-known emergency contact from flash
  ensureConnectivity(); // tries WiFi first, falls back to GSM
  refreshContactCache(); // fetch a fresh copy now if connectivity is up

  lastHelmetStatusAt = millis();

  Serial.println("Setup complete. Monitoring for crashes...");
  Serial.println("total_accel_m_s2 , total_gyro_deg_s , status");
}

void loop() {
  pollGps();

  float totalAccel, totalGyro;
  readCrashMetrics(totalAccel, totalGyro);

  bool crashDetected = (totalAccel > ACCEL_THRESHOLD) && (totalGyro > GYRO_THRESHOLD);
  bool precautionDetected = (totalAccel > PRECAUTION_ACCEL_THRESHOLD) || (totalGyro > PRECAUTION_GYRO_THRESHOLD);

  Serial.print(totalAccel, 2);
  Serial.print(" , ");
  Serial.print(totalGyro, 2);
  Serial.print(" , ");

  // Full crash takes priority - it's a strict superset of the precaution
  // condition (higher thresholds, AND instead of OR), so this ordering just
  // makes sure a real crash never gets treated as "merely a warning."
  if (crashDetected && (millis() - lastCrashReportAt > CRASH_REPORT_COOLDOWN_MS)) {
    lastCrashReportAt = millis();
    handleCrash(totalAccel, totalGyro);
  } else if (precautionDetected && (millis() - lastPrecautionAlertAt > PRECAUTION_COOLDOWN_MS)) {
    lastPrecautionAlertAt = millis();
    handlePrecautionAlert();
  } else {
    Serial.println("normal");
  }

  if (millis() - lastHelmetStatusAt > DEVICE_STATUS_INTERVAL_MS) {
    lastHelmetStatusAt = millis();
    retryPendingCrashReport(); // flush any crash report that failed to send earlier
    reportHelmetStatus();
    refreshContactCache();
  }

  delay(100); // ~10 readings per second, easy to read in Serial Monitor
}
