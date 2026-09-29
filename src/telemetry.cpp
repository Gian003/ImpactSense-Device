#include "telemetry.h"
#include "config.h"
#include "connectivity.h"
#include "device_identity.h"
#include "gps.h"
#include "power_state.h"

#include <Arduino.h>

static const int BAR_WIDTH = 20;

// ── shared helpers ────────────────────────────────────────────────────────────

static const char *networkLabel() {
  switch (currentNetwork) {
    case NET_WIFI: return "WiFi connected";
    // Accurate rather than flattering: with GSM_DATA_ENABLED off this state
    // reaches people by SMS and voice, but files no incident. Saying only
    // "GSM connected" during a demo would imply a dashboard update that is
    // not coming.
    case NET_GSM:  return activeClient ? "Mobile data connected"
                                       : "Mobile network - SMS and calls only";
    default:       return "Offline - searching";
  }
}

// A 0-100 bar. Percentages above the threshold are clamped, because past 100%
// the useful information is "it fired", not how far past it went.
//
// `rest` is what the reading is when nothing is happening, and the bar runs
// from there to the threshold. Acceleration needs it: ACCEL_THRESHOLD includes
// gravity, so a device lying still on a desk used to show "40% of crash
// level" - exactly the reading a demo audience would take to mean it was
// already close to going off. Crash detection itself is unchanged; this only
// moves where the bar's zero is, and 100% still lands exactly on the trigger.
static void printBar(const char *label, float value, float rest, float threshold) {
  float span = threshold - rest;
  float pct = (span > 0.0f) ? ((value - rest) / span) * 100.0f : 0.0f;
  if (pct < 0.0f)   pct = 0.0f;
  int filled = (int) ((pct / 100.0f) * BAR_WIDTH + 0.5f);
  if (filled > BAR_WIDTH) filled = BAR_WIDTH;

  Serial.printf("  %-10s [", label);
  for (int i = 0; i < BAR_WIDTH; i++) {
    Serial.print(i < filled ? '#' : '.');
  }
  Serial.printf("] %3.0f%% of crash level\n", pct > 999.0f ? 999.0f : pct);
}

// ── demo format ───────────────────────────────────────────────────────────────

static void printDemoBlock(const MotionReading &motion) {
  const char *status = isIdle() ? "RESTING" : "MONITORING";

  Serial.println();
  Serial.println("=========================================================");
  Serial.printf ("  ImpactSense  %-24s %11s\n", getDeviceCode().c_str(), status);
  Serial.println("=========================================================");

  Serial.printf("  %-10s %s\n", "Network", networkLabel());

  // Three different states, deliberately worded so they cannot be confused.
  // "No fix" on a healthy module means wait; on a dead one it means fix the
  // wiring — and from outside they look identical, which is why a first fix
  // is so often spent staring at hardware that was never going to answer.
  if (hasGpsFix()) {
    Serial.printf("  %-10s GPS fix  %.6f, %.6f   (%d satellites)\n", "Location",
                  getLatitude(), getLongitude(), getSatelliteCount());
  } else if (!gpsReceivingData()) {
    Serial.printf("  %-10s NO DATA from the GPS module - check its wiring and power\n", "Location");
  } else {
    // Never dressed up as a position. This is the same honesty the crash
    // report itself needs - see STEP 2 in deployment.h.
    char sats[24];
    int seen = getSatelliteCount();
    if (seen < 0) {
      snprintf(sats, sizeof(sats), "none yet");
    } else {
      snprintf(sats, sizeof(sats), "%d in view", seen);
    }
    Serial.printf("  %-10s searching, %s - location not confirmed\n", "Location", sats);
  }

  Serial.printf("  %-10s %d%%\n", "Battery", readBatteryPercent());

  printBar("Impact",   motion.totalAccel, 9.81f, ACCEL_THRESHOLD); // at rest: 1 g
  printBar("Rotation", motion.totalGyro,  0.0f,  GYRO_THRESHOLD);  // at rest: 0 deg/s

  // Two different faults wearing the same flag. A missing sensor is permanent
  // and means this unit cannot detect a crash at all; a substituted read is a
  // bus that flaked once. Telling a demo audience "check the wiring" when the
  // sensor was never there sends them looking for the wrong thing.
  if (!motionSensorPresent()) {
    Serial.println("  Sensor     NO MOTION SENSOR - crash detection is OFF");
  } else if (motion.substituted) {
    Serial.println("  Sensor     READ FAILED - check the SDA/SCL wiring");
  }

  Serial.println("=========================================================");
}

// ── detailed format ───────────────────────────────────────────────────────────

static void printDetailedBlock(const MotionReading &motion,
                               float peakAccel, float peakGyro, bool sawStale) {
  const char *net = currentNetwork == NET_WIFI ? "wifi"
                  : currentNetwork == NET_GSM  ? "gsm"
                                               : "offline";

  char gpsState[28];
  if (hasGpsFix()) {
    snprintf(gpsState, sizeof(gpsState), "fix/%dsat", getSatelliteCount());
  } else if (!gpsReceivingData()) {
    snprintf(gpsState, sizeof(gpsState), "NO DATA");
  } else {
    snprintf(gpsState, sizeof(gpsState), "searching/%dsat", max(getSatelliteCount(), 0));
  }

  const char *sensorNote = !motionSensorPresent()
                               ? "   NO MOTION SENSOR - crash detection is OFF"
                           : sawStale ? "   STALE I2C READS - check SDA/SCL wiring"
                                      : "";

  Serial.printf("\n[%7.1fs] temp %.1fC   net %s   power %s   gps %s%s\n",
                millis() / 1000.0f, motion.tempC, net, powerStateName(), gpsState,
                sensorNote);

  Serial.printf("            accel  X %+7.2f  Y %+7.2f  Z %+7.2f   "
                "|mag| %6.2f m/s2   peak %6.2f   %3.0f%% of %.0f\n",
                motion.ax, motion.ay, motion.az,
                motion.totalAccel, peakAccel,
                (motion.totalAccel / ACCEL_THRESHOLD) * 100.0f, ACCEL_THRESHOLD);

  Serial.printf("            gyro   X %+7.1f  Y %+7.1f  Z %+7.1f   "
                "|mag| %6.1f deg/s  peak %6.1f   %3.0f%% of %.0f\n",
                motion.gx, motion.gy, motion.gz,
                motion.totalGyro, peakGyro,
                (motion.totalGyro / GYRO_THRESHOLD) * 100.0f, GYRO_THRESHOLD);
}

// ── public ────────────────────────────────────────────────────────────────────

void telemetryBanner() {
  if (TELEMETRY_MODE == TELEMETRY_PLOTTER) {
    Serial.println("total_accel_m_s2 , total_gyro_deg_s , status");
    return;
  }

  if (TELEMETRY_MODE == TELEMETRY_DEMO) {
    Serial.println();
    Serial.println("ImpactSense is running. The panel below refreshes every");
    Serial.printf ("%lu seconds. Both bars sit near 0%% at rest. A crash needs\n",
                   TELEMETRY_INTERVAL_MS / 1000);
    Serial.println("Impact AND Rotation past 100% together. Events appear as >> lines.");
    return;
  }

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
  Serial.println("------------------------------------------------------");
  Serial.println();
}

void telemetryReport(const MotionReading &motion) {
  // The loop samples ten times a second but a block prints far more slowly, so
  // without these the one reading that mattered would usually fall between two
  // prints. Tracked in every mode because the demo bars need them just as much.
  static float         peakAccel   = 0.0f;
  static float         peakGyro    = 0.0f;
  static bool          sawStale    = false;
  static unsigned long lastPrintAt = 0;
  static unsigned long lastIdleAt  = 0;

  if (motion.totalAccel > peakAccel) peakAccel = motion.totalAccel;
  if (motion.totalGyro  > peakGyro)  peakGyro  = motion.totalGyro;
  if (motion.substituted)            sawStale  = true;

  if (TELEMETRY_MODE == TELEMETRY_PLOTTER) {
    if (isIdle()) return;
    Serial.print(motion.totalAccel, 2);
    Serial.print(" , ");
    Serial.print(motion.totalGyro, 2);
    Serial.println(" , normal");
    return;
  }

  // Resting: one line a minute instead of a full block. Enough to show the
  // monitor is alive and the device is deliberately quiet, without burying the
  // event lines that matter during a demo.
  if (isIdle()) {
    if (millis() - lastIdleAt >= 60000) {
      lastIdleAt = millis();
      Serial.printf("   resting - still for %lus, nothing to report "
                    "(wakes the moment it moves)\n", stillForSeconds());
    }
    return;
  }
  lastIdleAt = 0;

  if (millis() - lastPrintAt < TELEMETRY_INTERVAL_MS) {
    return;
  }
  lastPrintAt = millis();

  if (TELEMETRY_MODE == TELEMETRY_DEMO) {
    printDemoBlock(motion);
  } else {
    printDetailedBlock(motion, peakAccel, peakGyro, sawStale);
  }

  peakAccel = 0.0f;
  peakGyro  = 0.0f;
  sawStale  = false;
}

void telemetryEvent(const char *message) {
  Serial.println();
  Serial.printf(">> %s\n", message);
}

void telemetryDetail(const char *message) {
  Serial.printf("   %s\n", message);
}
