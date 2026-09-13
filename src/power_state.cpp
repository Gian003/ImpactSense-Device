#include "power_state.h"
#include "config.h"

#include <Arduino.h>

static PowerState state = POWER_ACTIVE;

// Stillness is measured over one-second buckets rather than per sample. A
// single sample cannot tell "resting" from "moving smoothly" - at a constant
// speed on a smooth road the magnitude sits near 1 g too. What separates them
// is variation: a helmet on a table has almost none, a helmet on a moving bike
// always has some.
static unsigned long bucketStartMs = 0;
static float         bucketMinAccel = 0.0f;
static float         bucketMaxAccel = 0.0f;
static float         bucketMaxGyro = 0.0f;
static bool          bucketPrimed = false;

// millis() at which the current run of still buckets began; 0 while moving.
static unsigned long stillSinceMs = 0;

static void applyCpu(uint32_t mhz) {
  if (!IDLE_CPU_SCALING) {
    return;
  }
  // The Arduino core re-derives every initialised UART's divider when this
  // changes, so GPS, the SIM800L and the USB console keep their baud rates
  // across the switch. 80 MHz is the floor that still supports the radio.
  setCpuFrequencyMhz(mhz);
}

static void enterIdle() {
  state = POWER_IDLE;
  applyCpu(IDLE_CPU_MHZ);
  Serial.printf("\n[power] IDLE - still for %lus. Heartbeat -> %lus, CPU -> %uMHz. "
                "Crash detection unchanged.\n",
                IDLE_ENTER_MS / 1000, IDLE_STATUS_INTERVAL_MS / 1000,
                (unsigned) getCpuFrequencyMhz());
}

static void exitIdle() {
  state = POWER_ACTIVE;
  applyCpu(ACTIVE_CPU_MHZ);
  Serial.printf("\n[power] ACTIVE - movement detected. CPU -> %uMHz.\n",
                (unsigned) getCpuFrequencyMhz());
}

void powerStateInit() {
  state = POWER_ACTIVE;
  stillSinceMs = 0;
  bucketPrimed = false;
  bucketStartMs = millis();
  applyCpu(ACTIVE_CPU_MHZ);
}

void powerStateUpdate(const MotionReading &motion) {
  unsigned long now = millis();

  // A discarded I2C read carries substituted magnitudes from the last good
  // sample. Feeding those in would look like perfect stillness and could idle
  // a device whose only real problem is a flaking bus.
  if (motion.substituted) {
    return;
  }

  // ---- immediate wake -------------------------------------------------------
  // Checked every sample, not once a bucket: leaving idle has to be fast,
  // because everything downstream of it assumes a device that is awake by the
  // time the bike is actually moving. Deliberately far below the precaution
  // thresholds - this is "something moved", not "something happened".
  bool moving = fabsf(motion.totalAccel - 9.81f) > WAKE_ACCEL_DELTA ||
                motion.totalGyro > WAKE_GYRO;

  if (moving) {
    stillSinceMs = 0;
    bucketPrimed = false;
    bucketStartMs = now;
    if (state == POWER_IDLE) {
      exitIdle();
    }
    return;
  }

  // ---- one-second stillness buckets ----------------------------------------
  if (!bucketPrimed) {
    bucketMinAccel = bucketMaxAccel = motion.totalAccel;
    bucketMaxGyro  = motion.totalGyro;
    bucketStartMs  = now;
    bucketPrimed   = true;
    return;
  }

  if (motion.totalAccel < bucketMinAccel) bucketMinAccel = motion.totalAccel;
  if (motion.totalAccel > bucketMaxAccel) bucketMaxAccel = motion.totalAccel;
  if (motion.totalGyro  > bucketMaxGyro)  bucketMaxGyro  = motion.totalGyro;

  if (now - bucketStartMs < 1000) {
    return;
  }

  bool stillBucket = (bucketMaxAccel - bucketMinAccel) < STILL_ACCEL_BAND &&
                     bucketMaxGyro < STILL_GYRO_MAX;

  bucketPrimed = false; // start a fresh bucket next sample

  if (!stillBucket) {
    stillSinceMs = 0;
    return;
  }

  if (stillSinceMs == 0) {
    stillSinceMs = now;
  }

  if (state == POWER_ACTIVE && (now - stillSinceMs) >= IDLE_ENTER_MS) {
    enterIdle();
  }
}

PowerState currentPowerState() {
  return state;
}

bool isIdle() {
  return state == POWER_IDLE;
}

const char *powerStateName() {
  return state == POWER_IDLE ? "idle" : "active";
}

unsigned long statusIntervalMs() {
  return state == POWER_IDLE ? IDLE_STATUS_INTERVAL_MS : DEVICE_STATUS_INTERVAL_MS;
}

unsigned long stillForSeconds() {
  if (stillSinceMs == 0) {
    return 0;
  }
  return (millis() - stillSinceMs) / 1000;
}
