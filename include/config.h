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
// Device and backend must be on the SAME network. Backend is currently at 192.168.1.6
// (home/lab WiFi). Update both SSID and PASSWORD to match whichever network you are
// testing on, then re-flash.
// NOTE: "const char* const" (not just "const char*") is required here because this
// header is included by multiple .cpp files - the trailing const gives each string
// internal linkage, avoiding a "multiple definition" error at link time.
const char *const WIFI_SSID = "BIDA_dFDEEF";  // ← UPDATE if testing at home
const char *const WIFI_PASSWORD = "tF2teH9s"; // ← UPDATE if testing at home

// ===== GSM/GPRS fallback config =====
// TODO: confirm the correct APN for your SIM's carrier (e.g. Globe: "internet.globe.com.ph",
// Smart/TNT: "internet"). Most Philippine prepaid data APNs need no username/password.
const char *const GPRS_APN = "internet.globe.com.ph";
const char *const GPRS_USER = "";
const char *const GPRS_PASS = "";

// Development kill-switch: set to false while the SIM800L is unpowered/being
// tuned (e.g. waiting on a multimeter to set the MT3608's output voltage).
// All the SIM800L wiring and code stays exactly as-is - this just skips the
// actual AT communication so boot and crash-handling aren't stuck waiting on
// timeouts against a dead modem while testing everything else. Flip back to
// true once the module is properly powered and registering.
const bool SIM800L_ENABLED = true;

// ===== Emergency voice call =====
// TOC / dispatch number the device calls when a crash is confirmed.
const char *const EMERGENCY_HOTLINE_NUMBER = "09652145185";

// How long after dialling to wait before playing the TOC audio track.
// Allows the ring + answer cycle to complete before the message starts.
const unsigned long CALL_CONNECT_WAIT_MS = 8000;

// How long to hold the line open after the audio track starts playing.
// 30 s is enough for any track and gives the dispatcher time to respond.
const unsigned long CALL_HOLD_MS = 30000;

// ===== Backend config =====
// Must match a Device row in the DB (device_code) that is already paired to a rider.
//
// The backend is exposed publicly via a Cloudflare Tunnel at impactsense.org.
// The device connects over HTTPS (port 443) from any internet-connected WiFi —
// no LAN mDNS needed, no hardcoded IPs that break when DHCP changes.
const char *const API_PUBLIC_HOST = "impactsense.org";
const uint16_t API_PORT = 443;
const char *const DEVICE_CODE = "IMP-001";

// ===== GPS fallback coordinates =====
// Used only until the NEO-6M gets a real satellite fix (or if it never does indoors).
const double STUB_LATITUDE = 15.9754;
const double STUB_LONGITUDE = 120.5650;

// ===== Battery voltage divider calibration =====
// Assumes a 100k/100k divider (halves the voltage) and a single-cell LiPo (3.3V empty, 4.2V full).
// Re-measure with a multimeter and adjust if your divider resistors differ.
const float BATTERY_DIVIDER_RATIO = 2.0;
const float BATTERY_MIN_V = 3.3;
const float BATTERY_MAX_V = 4.2;

// ===== DFPlayer Mini (SoftwareSerial on GPIO 32/33) =====
// All three ESP32 HW UARTs are occupied (UART0=USB, UART1=SIM800L, UART2=GPS).
const int DFP_TX_PIN = 32; // ESP32 GPIO32 → DFPlayer RX
const int DFP_RX_PIN = 33; // ESP32 GPIO33 ← DFPlayer TX
const int DFP_VOLUME = 20; // 0-30; capped at 20 for a 0.5 W speaker

// Audio track numbers — SD card /mp3/ folder, named 0001.mp3 … 0008.mp3
const int TRACK_STARTUP = 1;        // plays locally on helmet speaker
const int TRACK_CRASH_DETECTED = 2; // plays locally on helmet speaker
const int TRACK_REPORTING = 3;      // plays locally on helmet speaker
const int TRACK_REPORT_SENT = 4;    // plays locally on helmet speaker
const int TRACK_CANCELLED = 5;      // plays locally on helmet speaker
const int TRACK_MANUAL_SOS = 6;     // plays locally on helmet speaker
const int TRACK_VOICE_CANCEL = 7;   // plays locally on helmet speaker
// Transmitted through the SIM800L voice call to the TOC dispatcher.
// Hardware: DFPlayer DAC_R (pin 4) → 100 kΩ → SIM800L MIC+, SIM800L MIC− → GND.
// Also plays locally on the helmet speaker at the same time.
const int TRACK_CALL_TOC = 8;

// ===== MAX9814 microphone (ADC1_CH7 — input-only GPIO) =====
const int MIC_PIN = 35;

// Voice Activity Detection — leaky-integrator parameters.
// deviation: how far the raw ADC value (0-4095) must be from midpoint (2048) to count as speech.
// VOICE_SCORE_TARGET: score that must be accumulated before VAD confirms voice.
// At 20 ms polling: +3 per voiced sample, -1 per silent sample → ~1.5 s of speech to confirm.
const int MIC_VOICE_THRESHOLD = 700;
const int VOICE_SCORE_TARGET = 200;

// ===== Manual SOS =====
// Hold the cancel button for this long outside a crash window to send SOS.
const unsigned long MANUAL_SOS_HOLD_MS = 3000;

// ===== Crash detection thresholds =====
const float ACCEL_THRESHOLD = 25.0; // m/s^2 (total acceleration, includes gravity)
const float GYRO_THRESHOLD = 200.0; // deg/s (total rotation)

// ===== Precaution (early-warning) thresholds =====
// Lower than the full crash thresholds above, and checked with OR instead of
// AND: hard braking/acceleration alone, OR swerving/fishtailing alone, is
// already worth a heads-up - a real impact is what needs both at once. Tune
// these with real test-ride data, same as the crash thresholds.
const float PRECAUTION_ACCEL_THRESHOLD = 15.0; // m/s^2
const float PRECAUTION_GYRO_THRESHOLD = 100.0; // deg/s

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
