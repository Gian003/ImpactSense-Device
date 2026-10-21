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
//       SET <device_code>,<model>
//           Saves to flash and uses it immediately. model is optional —
//           only device_code is required.
//             SET IMP-042,ImpactSense Pro X1
//             SET IMP-043
//
//           A third field is still accepted and ignored, with a note saying
//           so: the old three-field form is in people's muscle memory and
//           failing the whole command over it would be the wrong trade.
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
//  model is for local reference only (field debugging via SHOW). It is not
//  sent to the backend — the model shown on the admin site comes from what
//  TOC staff typed into the registration form.
//
//  firmware_version is NOT typed in and is NOT stored in flash. It is stamped
//  into the binary at build time from `git describe` (see inject_version.py,
//  wired up by extra_scripts in platformio.ini), so what a unit reports is
//  always the commit it is actually running:
//
//      6fe3c36            a clean build of that commit
//      6fe3c36-dirty      built with uncommitted changes in the tree
//      v1.2.0-3-gabc123   three commits past tag v1.2.0
//
//  The "-dirty" suffix is the useful part: when a unit misbehaves in the
//  field it says whether the binary on it corresponds to anything in the
//  repository. Typed by hand, this field could claim 2.0.1 while running
//  anything at all, and nothing here or on the server could tell.
//
//  This is the first of the three fields the pre-final defence panel asked to
//  have assigned automatically. device_code and model are the other two; they
//  need a backend enrolment handshake, which this does not.

// Loads the saved identity from flash. Call once, early in setup(), before
// anything that talks to the backend.
void deviceIdentityInit();

// Blocks for up to PROVISION_WINDOW_MS listening for a SET/SHOW command on
// USB serial. Call immediately after deviceIdentityInit().
void runProvisioningWindow();

String getDeviceCode();
String getDeviceModel();
String getFirmwareVersion();

// ── AUTOMATIC ENROLMENT ──────────────────────────────────────────────────────
//
// The pre-final defence panel asked for device_code, model and firmware to be
// assigned automatically rather than typed into a serial monitor per unit.
// firmware_version is handled at build time (above). The other two are
// collected over the network, from a backend that identifies this board by the
// one thing it has before it has an identity: its MAC address.
//
// See backend.h's attemptDeviceEnrolment() for the exchange itself. These two
// are the identity side of it.

// Whether this unit still needs enrolling: no device_code, or a code but no
// signing secret. A unit that has both is left alone — enrolment is not
// something to repeat on every boot.
bool deviceNeedsEnrolment();

// Writes credentials handed back by the server and uses them immediately, so
// the rest of setup() runs as a provisioned unit rather than waiting for a
// reboot. Returns false if the server sent something unusable, in which case
// nothing is written — a half-applied identity is worse than none.
bool applyEnrolledIdentity(const String &code, const String &model, const String &secret);

// ── PROVING IT IS THIS DEVICE ────────────────────────────────────────────────
//
// device_code says which unit this is; it does not prove it. It is printed on
// the case, shown on the TOC screen and sent with every request, so anyone who
// knows one could file a crash as that device.
//
// The signing secret closes that. It is generated by the server when TOC
// registers the device, carried across once by hand over USB, and then never
// sent again: each report is signed with it and the server recomputes the
// signature. That holds even on plain HTTP, where a token in a header could
// simply be read off the wire and reused.
//
//   SECRET <64 hex characters>
//
// The TOC device page has the whole line ready to copy. SHOW says whether a
// secret is stored, never what it is.
//
// A unit with no secret still reports, unsigned, exactly as before — and the
// first time the server sees a valid signature from it, unsigned reports
// claiming to be that device stop being accepted.
bool hasSigningSecret();

// Lower-case hex HMAC-SHA256 of "<device_code>\n<counter>\n<body>", or an
// empty string when no secret is stored.
String signRequest(unsigned long counter, const String &body);

// A number that only ever goes up, including across reboots and power loss,
// so the server can refuse a captured request being sent a second time.
//
// Derived from a boot counter kept in flash rather than a clock: the device
// has no RTC battery, and no NTP on the fallback path. One flash write per
// boot, not per request.
unsigned long nextRequestCounter();
