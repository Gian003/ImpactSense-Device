#pragma once

#include <Arduino.h>
#include "sensors.h"

// Two-tier power management for a device that spends a lot of its life not
// moving - parked, on a shelf, or waiting at a light.
//
//   POWER_ACTIVE   the bike is moving, or moved within the last IDLE_ENTER_MS.
//                  Everything runs exactly as it always has.
//
//   POWER_IDLE     nothing has moved for IDLE_ENTER_MS. The heartbeat stretches
//                  from DEVICE_STATUS_INTERVAL_MS to IDLE_STATUS_INTERVAL_MS,
//                  the CPU drops to IDLE_CPU_MHZ, GPS polling and the telemetry
//                  block stop. Exits on the first hint of movement.
//
// WHAT IDLE DELIBERATELY DOES NOT DO: slow down the sampling loop. It would be
// the obvious saving and it is the wrong one. A crash spike lasts tens of
// milliseconds, so sampling at 2 Hz could step straight over one - and the
// scenario this would break is a rider stopped at a light being hit from
// behind, which is a real crash the device is supposed to catch. Idle is
// allowed to cost latency in reporting, never in detection. The MPU is read at
// the same 10 Hz in both states and the crash thresholds are untouched, so a
// parked-and-struck bike is detected identically to a moving one.
//
// The saving therefore comes from the two things that genuinely cost power and
// genuinely can wait: the modem transmitting a heartbeat nobody is reading, and
// the CPU running at 240 MHz to do nothing.
//
// Deep sleep is a separate, later step with a real safety cost attached - see
// STEP 5 in deployment.h before adding it.

enum PowerState { POWER_ACTIVE, POWER_IDLE };

void powerStateInit();

// Call once per loop with the sample just read. Owns the state machine and
// applies the transitions itself (CPU frequency, log lines).
void powerStateUpdate(const MotionReading &motion);

PowerState currentPowerState();
bool isIdle();
const char *powerStateName();

// Heartbeat interval for the state the device is currently in. Call this
// rather than reading DEVICE_STATUS_INTERVAL_MS directly.
unsigned long statusIntervalMs();

// Seconds the device has been continuously still, or 0 when it is moving.
// Only for the telemetry line - the state machine keeps its own timing.
unsigned long stillForSeconds();
