#include <Arduino.h>

#include "config.h"
#include "device_identity.h"
#include "sensors.h"
#include "gps.h"
#include "connectivity.h"
#include "backend.h"
#include "crash_response.h"
#include "contact_cache.h"
#include "audio.h"
#include "mic.h"
#include "power_state.h"
#include "telemetry.h"

// DEPLOYMENT - STEP 3 (see deployment.h): uncomment with the watchdog blocks
// in setup() and loop().
// #include <esp_task_wdt.h>

unsigned long lastCrashReportAt     = 0;
unsigned long lastDeviceStatusAt    = 0;
unsigned long lastPrecautionAlertAt = 0;

void setup() {
  Serial.begin(115200);
  delay(500); // give Serial Monitor time to attach after upload
  Serial.println();
  Serial.println("ImpactSense - Full Firmware (Modules 1-7)");

  // First, before anything reaches the backend: this unit has to know which
  // device_code it is, and boot is the only point where that can be changed.
  deviceIdentityInit();
  runProvisioningWindow();

  sensorsInit();
  powerStateInit();
  crashResponseInit();
  gpsInit();
  audioInit();
  micInit();
  modemInit();          // brings up SIM800 for SMS, independent of WiFi/GPRS
  contactCacheInit();   // loads last-known emergency contact from flash
  ensureConnectivity(); // tries WiFi first, falls back to GSM

  // Must come after connectivity and before anything that talks to the backend
  // as a provisioned unit. Does nothing on a board that already has an
  // identity, so this is inert on every boot after the first.
  attemptDeviceEnrolment();

  refreshContactCache(); // fetch a fresh copy now if connectivity is up

  lastDeviceStatusAt = millis();

  // DEPLOYMENT - STEP 3 (see deployment.h). Nothing currently recovers a unit
  // whose I2C bus or modem has wedged, and from the backend a dead unit looks
  // identical to one that simply has not crashed. 90 s, not the usual 5-10,
  // because handleCrash() blocks on purpose for CALL_CONNECT_WAIT_MS +
  // CALL_HOLD_MS + the HTTP timeout, and a shorter watchdog would reboot the
  // device in the middle of the emergency call.
  //
  // if (DEPLOYMENT_MODE) {
  //   esp_task_wdt_init(90, true); // panic + reboot on expiry
  //   esp_task_wdt_add(NULL);      // watch the Arduino loop task
  // }

  Serial.println("Setup complete. Monitoring for crashes...");
  telemetryBanner(); // one-off key for whichever format TELEMETRY_MODE selects

  audioPlay(TRACK_STARTUP); // "ImpactSense helmet system is now active. Stay safe."
}

void loop() {
  // DEPLOYMENT - STEP 3: pairs with esp_task_wdt_init() in setup().
  // if (DEPLOYMENT_MODE) esp_task_wdt_reset();

  checkManualSos(); // detects button held ≥ MANUAL_SOS_HOLD_MS outside a crash window

  // Read first, at the same 10 Hz in both power states — idle is never allowed
  // to cost crash-detection latency. See power_state.h.
  MotionReading motion;
  readMotion(motion);
  powerStateUpdate(motion);

  // A stationary device's position is not changing, so parsing NMEA it already
  // agrees with is pure work. Resumes the instant anything moves.
  if (!isIdle()) {
    pollGps();
  }

  float totalAccel = motion.totalAccel;
  float totalGyro  = motion.totalGyro;

  bool crashDetected     = (totalAccel > ACCEL_THRESHOLD) && (totalGyro > GYRO_THRESHOLD);
  bool precautionDetected = (totalAccel > PRECAUTION_ACCEL_THRESHOLD) || (totalGyro > PRECAUTION_GYRO_THRESHOLD);

  telemetryReport(motion); // owns format, rate-limiting and the resting line

  // Full crash takes priority — it's a strict superset of the precaution condition.
  if (crashDetected && (millis() - lastCrashReportAt > CRASH_REPORT_COOLDOWN_MS)) {
    lastCrashReportAt = millis();
    handleCrash(totalAccel, totalGyro);
  } else if (precautionDetected && (millis() - lastPrecautionAlertAt > PRECAUTION_COOLDOWN_MS)) {
    lastPrecautionAlertAt = millis();
    handlePrecautionAlert();
  }

  // Interval comes from the power state — 60 s active, 5 min idle.
  if (millis() - lastDeviceStatusAt > statusIntervalMs()) {
    lastDeviceStatusAt = millis();
    retryPendingCrashReport(); // flush any crash report that failed to send earlier
    reportDeviceStatus();
    refreshContactCache();
  }

  delay(100); // ~10 readings per second, easy to read in Serial Monitor
}
