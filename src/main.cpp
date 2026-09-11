#include <Arduino.h>

#include "config.h"
#include "sensors.h"
#include "gps.h"
#include "connectivity.h"
#include "backend.h"
#include "crash_response.h"
#include "contact_cache.h"
#include "audio.h"
#include "mic.h"

unsigned long lastCrashReportAt     = 0;
unsigned long lastHelmetStatusAt    = 0;
unsigned long lastPrecautionAlertAt = 0;

void setup() {
  Serial.begin(115200);
  delay(500); // give Serial Monitor time to attach after upload
  Serial.println();
  Serial.println("ImpactSense - Full Firmware (Modules 1-7)");

  sensorsInit();
  crashResponseInit();
  gpsInit();
  audioInit();
  micInit();
  modemInit();          // brings up SIM800 for SMS, independent of WiFi/GPRS
  contactCacheInit();   // loads last-known emergency contact from flash
  ensureConnectivity(); // tries WiFi first, falls back to GSM
  refreshContactCache(); // fetch a fresh copy now if connectivity is up

  lastHelmetStatusAt = millis();

  Serial.println("Setup complete. Monitoring for crashes...");

  if (TELEMETRY_LABELLED) {
    // A one-off key, so the block below is readable without coming back to
    // the source to find out what the numbers are.
    Serial.println();
    Serial.println("---------------- LIVE SENSOR READINGS ----------------");
    Serial.println(" accel  per-axis X/Y/Z and |mag| in m/s2. At rest |mag|");
    Serial.println("        sits near 9.8 - that is gravity, not movement.");
    Serial.printf ("        Crash needs |mag| > %.1f AND gyro > %.0f.\n",
                   ACCEL_THRESHOLD, GYRO_THRESHOLD);
    Serial.printf ("        Precaution trips at %.1f or %.0f.\n",
                   PRECAUTION_ACCEL_THRESHOLD, PRECAUTION_GYRO_THRESHOLD);
    Serial.println(" gyro   per-axis X/Y/Z and |mag| in deg/s.");
    Serial.println(" peak   highest seen since the previous block, so a spike");
    Serial.println("        between prints is not missed.");
    Serial.println(" stale  I2C read failed; last good magnitudes substituted.");
    Serial.println("-----------------------------------------------------");
    Serial.println();
  } else {
    Serial.println("total_accel_m_s2 , total_gyro_deg_s , status");
  }

  audioPlay(TRACK_STARTUP); // "ImpactSense helmet system is now active. Stay safe."
}

void loop() {
  pollGps();
  checkManualSos(); // detects button held ≥ MANUAL_SOS_HOLD_MS outside a crash window

  MotionReading motion;
  readMotion(motion);

  float totalAccel = motion.totalAccel;
  float totalGyro  = motion.totalGyro;

  bool crashDetected     = (totalAccel > ACCEL_THRESHOLD) && (totalGyro > GYRO_THRESHOLD);
  bool precautionDetected = (totalAccel > PRECAUTION_ACCEL_THRESHOLD) || (totalGyro > PRECAUTION_GYRO_THRESHOLD);

  if (TELEMETRY_LABELLED) {
    // The loop samples ten times a second but the block prints far more
    // slowly, so without these the one reading that mattered would almost
    // always fall between two prints.
    static float         peakAccel    = 0.0f;
    static float         peakGyro     = 0.0f;
    static bool          sawStale     = false;
    static unsigned long lastPrintAt  = 0;

    if (totalAccel > peakAccel) peakAccel = totalAccel;
    if (totalGyro  > peakGyro)  peakGyro  = totalGyro;
    if (motion.substituted)     sawStale  = true;

    if (millis() - lastPrintAt >= TELEMETRY_INTERVAL_MS) {
      lastPrintAt = millis();

      const char *net = currentNetwork == NET_WIFI ? "wifi"
                      : currentNetwork == NET_GSM  ? "gsm"
                                                   : "offline";

      Serial.printf("\n[%7.1fs] temp %.1fC   net %s%s\n",
                    millis() / 1000.0f, motion.tempC, net,
                    sawStale ? "   STALE I2C READS - check SDA/SCL wiring" : "");

      Serial.printf("            accel  X %+7.2f  Y %+7.2f  Z %+7.2f   "
                    "|mag| %6.2f m/s2   peak %6.2f   %3.0f%% of %.0f\n",
                    motion.ax, motion.ay, motion.az,
                    totalAccel, peakAccel,
                    (totalAccel / ACCEL_THRESHOLD) * 100.0f, ACCEL_THRESHOLD);

      Serial.printf("            gyro   X %+7.1f  Y %+7.1f  Z %+7.1f   "
                    "|mag| %6.1f deg/s  peak %6.1f   %3.0f%% of %.0f\n",
                    motion.gx, motion.gy, motion.gz,
                    totalGyro, peakGyro,
                    (totalGyro / GYRO_THRESHOLD) * 100.0f, GYRO_THRESHOLD);

      peakAccel = 0.0f;
      peakGyro  = 0.0f;
      sawStale  = false;
    }
  } else {
    Serial.print(totalAccel, 2);
    Serial.print(" , ");
    Serial.print(totalGyro, 2);
    Serial.print(" , ");
  }

  // Full crash takes priority — it's a strict superset of the precaution condition.
  if (crashDetected && (millis() - lastCrashReportAt > CRASH_REPORT_COOLDOWN_MS)) {
    lastCrashReportAt = millis();
    handleCrash(totalAccel, totalGyro);
  } else if (precautionDetected && (millis() - lastPrecautionAlertAt > PRECAUTION_COOLDOWN_MS)) {
    lastPrecautionAlertAt = millis();
    handlePrecautionAlert();
  } else if (!TELEMETRY_LABELLED) {
    // Only meaningful as the third CSV column; the labelled block already
    // says what the readings are, and printing "normal" ten times a second
    // would bury it.
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
