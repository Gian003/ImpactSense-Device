#pragma once

#include <Arduino.h>

// Per-unit identity (device_code / model / firmware_version), held in NVS
// (flash) rather than as compile-time constants in config.h.
//
// device_code used to be a `const char*` in config.h, which meant every
// physical unit needed its own edited copy of the source and its own build —
// easy to get wrong, and impossible to tell afterwards which binary went onto
// which unit. Now the same firmware image is flashed to every unit, and each
// one is told what it is afterwards, over USB serial.
//
// ── PROVISIONING A UNIT (once per physical device) ──────────────────────────
//  1. Flash this firmware (PlatformIO: Upload).
//  2. Open the Serial Monitor at 115200 with line ending set to Newline.
//  3. Reset the board. On boot it prints its current identity and waits
//     PROVISION_WINDOW_MS for one of:
//
//       SET <device_code>,<model>,<firmware_version>
//           Saves to flash and uses it immediately. model and
//           firmware_version are optional — only device_code is required.
//             SET IMP-042,ImpactSense Pro X1,2.0.1
//             SET IMP-043
//
//       SHOW
//           Prints the saved identity without changing it.
//
//     Type nothing and it boots normally on whatever was saved last, or the
//     literal "UNPROVISIONED" on a unit that has never been set.
//
//  4. The device_code MUST match one already registered by TOC staff on the
//     admin site (toc/devices → "Register New Device"), which is where the
//     backend generates that device's pairing_key. Register there first,
//     otherwise every call from this unit returns "Device not registered".
//     The pairing_key is a backend secret — this firmware never sees it; the
//     rider enters it in the mobile app when pairing.
//
//  model / firmware_version are for local reference only (field debugging via
//  SHOW). They are not sent to the backend — the model and firmware shown on
//  the admin site come from what TOC staff typed into the registration form.

// Loads the saved identity from flash. Call once, early in setup(), before
// anything that talks to the backend.
void deviceIdentityInit();

// Blocks for up to PROVISION_WINDOW_MS listening for a SET/SHOW command on
// USB serial. Call immediately after deviceIdentityInit().
void runProvisioningWindow();

String getDeviceCode();
String getDeviceModel();
String getFirmwareVersion();
