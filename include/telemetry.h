#pragma once

#include <Arduino.h>
#include "sensors.h"

// Everything the device says about itself on the serial monitor, in whichever
// of three formats TELEMETRY_MODE selects (see config.h).
//
// The formats exist because the monitor has two completely different
// audiences. Someone tuning ACCEL_THRESHOLD needs per-axis numbers and peaks.
// Someone watching a demo needs to know, in a glance and without reading the
// code, that the device is alive, connected, and how close it currently is to
// calling for help. Serving both from one format served neither well.

void telemetryBanner();                            // once, at the end of setup()
void telemetryReport(const MotionReading &motion);  // every loop; rate-limits itself

// Plain-language event lines, so the demo narrative reads the same way in
// every mode. Kept here rather than scattered as Serial.println() calls so the
// wording stays consistent and the prefixes line up.
void telemetryEvent(const char *message);   //  >> something happened
void telemetryDetail(const char *message);  //     a consequence of it
