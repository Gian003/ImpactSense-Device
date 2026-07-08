#pragma once

#include <Arduino.h>

// ===================================================================
// PIN MAP - wire hardware to exactly these GPIOs
// ===================================================================
// MPU6050 (I2C):      SDA=21, SCL=22, VCC=3.3V, GND=GND
// NEO-6M GPS (UART2):  GPS TX -> ESP32 RX=16, GPS RX -> ESP32 TX=17
// SIM800L GSM (UART1): SIM800 TX -> ESP32 RX=26, SIM800 RX -> ESP32 TX=27
//   SIM800L needs a supply in its ~3.4-4.4V operating range - NOT the ESP32
//   3.3V rail (too low/no current headroom) and NOT the ESP32 5V/VIN pin
//   either (too high, and that pin can't source the ~2A TX-burst current
//   anyway).
//   If running off a single-cell Li-ion/LiPo (3.0-4.2V range): wire it
//   DIRECTLY to the SIM800L's VCC/GND, no converter needed - that range
//   already overlaps the module's spec, and a boost converter (e.g. MT3608)
//   can't regulate below its own input, so trying to "boost" a 4.2V-charged
//   cell down to a 4.0V setpoint just leaves it unregulated at the top of
//   the charge curve. A buck converter is only needed if the supply is
//   genuinely higher (e.g. 5V/USB); a boost converter is only needed if it's
//   genuinely lower (e.g. 2xAA/NiMH, ~1.2-3V).
//   Either way: a bulk cap (1000-3300uF) soldered right at the SIM800L's own
//   VCC/GND pins, and a confirmed common GND with the ESP32, matter more than
//   the exact regulator topology. "Modem not responding" at boot almost
//   always traces back to one of these, not to the firmware.
// Cancel button:       GPIO 4 -> button -> GND (internal pull-up, active LOW)
// Buzzer:              GPIO 5 -> buzzer +, buzzer - -> GND
// Battery sense:       GPIO 34 <- voltage divider midpoint (ADC1, input-only pin)

const int GPS_RX_PIN = 16;
const int GPS_TX_PIN = 17;
const int GSM_RX_PIN = 26;
const int GSM_TX_PIN = 27;
const int CANCEL_BUTTON_PIN = 4;
const int BUZZER_PIN = 5;
const int BATTERY_PIN = 34;

// ===== WiFi credentials =====
// Fill these in with the network the device and your Laravel backend both sit on.
// NOTE: "const char* const" (not just "const char*") is required here because this
// header is included by multiple .cpp files - the trailing const gives each string
// internal linkage, avoiding a "multiple definition" error at link time.
const char* const WIFI_SSID     = "CITE TEC-301 2.4g";
const char* const WIFI_PASSWORD = "2025ucucite";

// ===== GSM/GPRS fallback config =====
// TODO: confirm the correct APN for your SIM's carrier (e.g. Globe: "internet.globe.com.ph",
// Smart/TNT: "internet"). Most Philippine prepaid data APNs need no username/password.
const char* const GPRS_APN  = "internet.globe.com.ph";
const char* const GPRS_USER = "";
const char* const GPRS_PASS = "";

// ===== Emergency voice call =====
// National unified emergency hotline (Philippines) - matches the number the
// rider app itself dials (tel:911), for consistency across the system.
const char* const EMERGENCY_HOTLINE_NUMBER = "09652145185";

// How long to hold the call open before hanging up. No audio is fed into the
// call yet - that needs the MIC/SPK wiring + DFPlayer piece, which is a
// separate, not-yet-built feature - so this just keeps a live line open long
// enough for a human dispatcher to notice and respond.
const unsigned long CALL_HOLD_MS = 30000;

// ===== Backend config =====
// Must match a Helmet row in the DB (device_code) that is already paired to a rider.
//
// The backend machine's LAN IP changes every time it rejoins WiFi (DHCP), which
// previously meant editing a hardcoded IP here and reflashing every session. Instead,
// the firmware resolves this mDNS name to an IP at runtime (see connectivity.cpp:
// resolveApiHost()) and re-resolves it periodically, so a new IP doesn't need a reflash.
//
// Requirements for this to work:
//   - Backend machine must be running an mDNS responder that advertises this name
//     (Bonjour/"Bonjour Print Services" on Windows, built-in on macOS, avahi on Linux),
//     and Laravel must be served with `php artisan serve --host=0.0.0.0 --port=8000`
//     (the default --host=127.0.0.1 rejects connections from other devices).
//   - Only works over WiFi, not the GSM/GPRS fallback - mDNS is LAN-local, and a
//     private dev backend IP was never reachable from cellular data anyway.
const char* const API_MDNS_NAME = "impactsense"; // resolves "impactsense.local"
const uint16_t API_PORT = 8000;
const char* const DEVICE_CODE = "IMP-001";

// Manual fallback if mDNS never resolves on this network (some campus/enterprise
// WiFi blocks multicast between clients - "AP client isolation" - which breaks
// mDNS no matter how correctly it's configured on both ends). Leave "" to rely
// on mDNS only. Otherwise set to the backend machine's current LAN IP (run
// `ipconfig` on it, use the WiFi adapter's IPv4 address) as a last resort -
// mDNS resolution is still retried in the background and will take over
// automatically once/if it starts working.
const char* const API_FALLBACK_IP = "10.68.249.203";

// ===== GPS fallback coordinates =====
// Used only until the NEO-6M gets a real satellite fix (or if it never does indoors).
const double STUB_LATITUDE  = 15.9754;
const double STUB_LONGITUDE = 120.5650;

// ===== Battery voltage divider calibration =====
// Assumes a 100k/100k divider (halves the voltage) and a single-cell LiPo (3.3V empty, 4.2V full).
// Re-measure with a multimeter and adjust if your divider resistors differ.
const float BATTERY_DIVIDER_RATIO = 2.0;
const float BATTERY_MIN_V = 3.3;
const float BATTERY_MAX_V = 4.2;

// ===== Crash detection thresholds =====
const float ACCEL_THRESHOLD = 25.0;   // m/s^2 (total acceleration, includes gravity)
const float GYRO_THRESHOLD  = 200.0;  // deg/s (total rotation)

// ===== Precaution (early-warning) thresholds =====
// Lower than the full crash thresholds above, and checked with OR instead of
// AND: hard braking/acceleration alone, OR swerving/fishtailing alone, is
// already worth a heads-up - a real impact is what needs both at once. Tune
// these with real test-ride data, same as the crash thresholds.
const float PRECAUTION_ACCEL_THRESHOLD = 15.0;  // m/s^2
const float PRECAUTION_GYRO_THRESHOLD  = 100.0; // deg/s

// Minimum time between two precaution alerts, so continued unstable riding
// doesn't spam the buzzer every loop iteration.
const unsigned long PRECAUTION_COOLDOWN_MS = 5000;

// Minimum time between two crash reports, so one impact event (which stays above
// threshold for a few readings) doesn't create duplicate incidents.
const unsigned long CRASH_REPORT_COOLDOWN_MS = 10000;

// How long the rider has to cancel a detected crash before it's reported.
const unsigned long CRASH_CANCEL_WINDOW_MS = 30000;

// How often to push a device status update (battery level) to the backend.
const unsigned long DEVICE_STATUS_INTERVAL_MS = 60000;
