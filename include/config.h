#pragma once

#include <Arduino.h>

// DEPLOYMENT_MODE, plus the ordered runbook for taking a unit off the bench
// and into the field. Several constants below derive from it.
#include "deployment.h"

// ===================================================================
// PIN MAP - wire hardware to exactly these GPIOs
// ===================================================================
// MPU6050 (I2C):      SDA=21, SCL=22, VCC=3.3V, GND=GND
// NEO-6M GPS (UART2):  GPS TX -> ESP32 RX=16, GPS RX -> ESP32 TX=17
// SIM800L GSM (UART1): SIM800 TX -> ESP32 RX=26, SIM800 RX -> ESP32 TX=27
//
//   FIRST, WHICH BOARD? There are two kinds, and they want OPPOSITE supplies.
//
//   V2.x / "5V" boards (pins labelled 5V and VDD): carry their own regulator
//   and logic level shifting. Feed the 5V pin from a 5 V supply that can
//   deliver ~2 A in bursts. Do NOT wire a Li-ion straight to it - 3.0-4.2 V
//   in, minus the onboard regulator's drop, leaves the radio under its 3.4 V
//   minimum, and it will answer AT and then fall over the moment it
//   transmits. Tie VDD to the ESP32's 3.3 V, not 5 V: VDD sets the level of
//   the module's TX line, and 5 V logic on GPIO 26 is outside the ESP32's
//   rating. Arduino wiring guides put VDD on 5 V because an Arduino is 5 V.
//
//   Bare module (small board, a single VCC pin, no regulator): everything
//   below applies.
//
//   A multimeter cannot confirm either supply is adequate. It shows the
//   average, and the transmit bursts that collapse a weak supply last well
//   under a millisecond - a steady reading can sit on top of dips deep enough
//   to reset the radio.
//
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

// Serial rate for the SIM800L link.
//
// The module leaves the factory in autobaud mode, and whatever rate it last
// locked onto is what it keeps using. Autobaud detection is far more reliable
// at 9600 than at 115200 - which is what modemInit() used to open the port at,
// unconditionally. If the module had settled on 9600, every AT command was
// being sent at a rate it could not read, which looks exactly like a dead
// modem.
//
// modemInit() probes the rates below when this one does not answer, and prints
// whichever one works so it can be set here permanently.
const uint32_t GSM_BAUD = 9600;
const int CANCEL_BUTTON_PIN = 4;

// ===== Bench console (development only) =====
//
// Lets the button be pressed, and a crash or SOS be triggered, by typing
// into the serial monitor. See dev_console.h for why that is worth having
// and how the simulated press reaches the same code a real one does.
//
// Tied to DEPLOYMENT_MODE rather than set by hand, and deliberately not a
// switch of its own. A console that can raise a real SOS has no business on
// a helmet in the field, and the one way that happens is somebody flipping
// a flag for an afternoon and forgetting it. Deriving it means going to
// deployment turns it off whether anyone remembered or not.
const bool DEV_CONSOLE_ENABLED = !DEPLOYMENT_MODE;
const int BUZZER_PIN = 5;
const int BATTERY_PIN = 34;

// ===== WiFi credentials =====
// Any network with a working internet connection will do - the backend is
// reached through its public Cloudflare Tunnel hostname (see "Backend config"
// below), not over the LAN, so the device and the machine running the backend
// no longer have to be on the same WiFi. Update SSID and PASSWORD to whichever
// network is being used, then re-flash.
// NOTE: "const char* const" (not just "const char*") is required here because this
// header is included by multiple .cpp files - the trailing const gives each string
// internal linkage, avoiding a "multiple definition" error at link time.
const char *const WIFI_SSID = "BIDA_dFDEEF";  // ← UPDATE if testing at home
const char *const WIFI_PASSWORD = "tF2teH9s"; // ← UPDATE if testing at home

// ===== GSM/GPRS fallback config =====
// Set SIM_CARRIER to the network of the SIM physically in the modem; the APN
// follows from it. This used to be a hardcoded Globe APN sitting beside a TNT
// SIM, with nothing anywhere to suggest they disagreed.
//
// Only two choices matter for a SIM800L, because the brands pair up on shared
// networks: TNT and Sun are Smart's own brands, TM is Globe's. Switching
// within a pair changes nothing - same towers, same 2G, same coverage. DITO
// has no 2G at all and cannot register on this module under any
// circumstances.
//
// The APN only matters for GPRS data. SMS and voice calls - which the crash
// path depends on - need cellular registration alone.
enum SimCarrier { CARRIER_SMART_TNT, CARRIER_GLOBE_TM };
const SimCarrier SIM_CARRIER = CARRIER_GLOBE_TM;

const char *const GPRS_APN  = (SIM_CARRIER == CARRIER_GLOBE_TM) ? "internet.globe.com.ph"
                                                                : "internet";
const char *const GPRS_USER = "";
const char *const GPRS_PASS = "";

// Boot-time answer to "can this SIM carry data on this module at all?"
//
// A phone cannot answer that. It does its data over LTE, while the SIM800L can
// only use 2G GPRS - and carriers have been winding down 2G data independently
// of 2G voice and SMS. So a SIM with working data in a phone may still carry
// none on the device, and STEP 1 in deployment.h depends entirely on it.
//
// When true, modemInit() attaches to GPRS once after registering, reports
// whether it got an IP address, and detaches. Adds up to ~30 s to boot and
// uses a few KB. Turn it on while choosing a carrier, then back off.
const bool GPRS_SELF_TEST = true;

// Whether to route HTTP traffic over GPRS when WiFi is unavailable.
//
// OFF, deliberately. The backend is HTTPS-only on port 443 (Cloudflare
// Tunnel), and connectivity.cpp hands HTTP a plain TinyGsmClient with no TLS
// layer - so over GPRS the device opens a raw socket to an HTTPS port, gets a
// 400 back, and queues the report anyway. Turning it on costs ~30 s of
// connect and timeout per attempt and achieves nothing.
//
// Wrapping it in TinyGsmClientSecure is not the fix either: most SIM800L
// firmware revisions send no SNI - which Cloudflare requires, since one IP
// serves many hostnames - the onboard SSL stack only offers old cipher
// suites, and the CA chain would have to be loaded into the module's limited
// cert store. Each of those alone is enough to fail the handshake.
//
// None of this weakens the crash path. GSM is still the fallback; it just
// carries SMS and the voice call instead of an HTTP request - both of which
// need only cellular registration, never a data session, and work on a far
// weaker signal than GPRS would. Offline, a crash sends three things: a text
// to the rider's emergency contact, a text to the TOC carrying the details
// the recorded call cannot speak (TOC_SMS_NUMBER), and the call itself. See
// handleCrash() in crash_response.cpp.
//
// Follows DEPLOYMENT_MODE, because it is only safe to enable together with
// the port-80 switch below - and only after "Always Use HTTPS" is off in
// Cloudflare. See STEP 1 in deployment.h; do not set this by hand.
const bool GSM_DATA_ENABLED = DEPLOYMENT_MODE;

// Development kill-switch: set to false while the SIM800L is unpowered/being
// tuned (e.g. waiting on a multimeter to set the MT3608's output voltage).
// All the SIM800L wiring and code stays exactly as-is - this just skips the
// actual AT communication so boot and crash-handling aren't stuck waiting on
// timeouts against a dead modem while testing everything else. Flip back to
// true once the module is properly powered and registering.
// 2026-11-17: OFF for WiFi-based demos. The modem answers and reads the SIM,
// but will not register (signal 99 on a Globe SIM, so 2G exists - suspect the
// u.FL connector or local coverage). Leaving it on costs ~30 s of boot waiting
// on registration timeouts and prints a screen of errors. Nothing in the crash
// path depends on it while WiFi is up: the incident goes over WiFi and the
// backend sends the SMS and the TOC call. Turn back ON for offline testing.
const bool SIM800L_ENABLED = false;

// ===== Emergency voice call =====
// TOC / dispatch number the device calls when a crash is confirmed.
const char *const EMERGENCY_HOTLINE_NUMBER = "09456725632";

// Where the device texts the crash details when it could not reach the
// backend. Defaults to the hotline above, which is a mobile number.
//
// The backup voice call can only play a fixed recording - it cannot say who
// crashed, how badly, or where, because those are not in the audio file. So
// on its own it tells the TOC that something happened to somebody, somewhere.
// This text carries what the call cannot, and says plainly that the crash is
// NOT on the dashboard, so nobody goes looking for a record that was never
// filed.
//
// Sent only when the backend was unreachable. When the report got through,
// the server has already called the hotline with the full details and the
// incident is on the board - a second text would be noise.
//
// Set to "" to switch it off, or to a duty desk mobile if the hotline number
// is a landline and cannot receive SMS.
const char *const TOC_SMS_NUMBER = EMERGENCY_HOTLINE_NUMBER;

// How long after dialling to wait before playing the TOC audio track.
// Allows the ring + answer cycle to complete before the message starts.
const unsigned long CALL_CONNECT_WAIT_MS = 8000;

// How long to hold the line open after the audio track starts playing.
// 30 s is enough for any track and gives the dispatcher time to respond.
const unsigned long CALL_HOLD_MS = 30000;

// ===== Backend config =====
// The backend is exposed publicly via a Cloudflare Tunnel at impactsense.org.
// The device connects over HTTPS (port 443) from any internet-connected WiFi —
// no LAN mDNS needed, no hardcoded IPs that break when DHCP changes.
const char *const API_PUBLIC_HOST = "impactsense.org";

// 443 on the bench (WiFi, TLS). 80 in the field, because the SIM800L cannot
// complete a TLS handshake against Cloudflare and WiFi will not be there -
// see STEP 1 in deployment.h for the full reasoning and the Cloudflare-side
// change this has to be paired with. connectivity.cpp picks a plain or TLS
// WiFi client from this value, so the two never disagree.
const uint16_t API_PORT = DEPLOYMENT_MODE ? 80 : 443;

// ===== Device identity =====
// device_code is deliberately NOT here. It is per-unit, so it lives in NVS
// instead and one firmware image serves every device — see
// include/device_identity.h for how to set it on a unit.
//
// How long the boot-time provisioning window listens on USB serial before
// startup continues on its own.
//
// Two figures, because they answer different questions. The first is how long
// to wait for someone who may not be there - paid on every single boot, so it
// stays short. The second is how long to keep waiting once somebody has
// actually typed something, where the cost is only that a person at the desk
// has stopped typing, and the pain of it expiring mid-sequence is having to
// reset the board and start over.
//
// Raise PROVISION_WINDOW_MS if you want longer to click into the monitor after
// a reset; raise PROVISION_IDLE_MS if a minute and a half between keystrokes
// is not enough.
const unsigned long PROVISION_WINDOW_MS = 20000;
const unsigned long PROVISION_IDLE_MS   = 90000;

// In deployment mode the full window is skipped unless someone is actually
// typing: the device waits only this long to notice a keystroke. Eight seconds
// of no crash detection on every boot is a bad trade in the field, especially
// on the reboot after a brownout, when power is already marginal. Plug in USB
// and hold a key during reset to open the full window on a deployed unit.
const unsigned long PROVISION_PROBE_MS = 300;

// ===== GPS fallback coordinates =====
// Used only until the NEO-6M gets a real satellite fix (or if it never does indoors).
const double STUB_LATITUDE = 15.9754;
const double STUB_LONGITUDE = 120.5650;

// ===== Battery voltage divider calibration =====
// Assumes a 100k/100k divider (halves the voltage) and a single-cell LiPo (3.3V empty, 4.2V full).
// Re-measure with a multimeter and adjust if your divider resistors differ.
//
// IS THE DIVIDER ACTUALLY FITTED? Two 100k resistors in series from the cell's
// + terminal to GND, with GPIO 34 on the midpoint. Set this false on a unit
// built without them.
//
// It matters because GPIO 34 is input-only and has no internal pull. With
// nothing on it the pin floats, analogRead() returns whatever charge happens
// to be sitting on it, and the heartbeat publishes a battery level that
// wanders for no reason - which looks far more like a failing device than an
// unfitted sensor does.
//
// NEVER wire the cell straight to GPIO 34. A charged cell sits at 4.2 V and
// the ADC pin is rated to 3.3 V; the divider is what makes the reading safe,
// not just accurate.
// Three flags describe this one physical fact, and they have to move
// together or the dashboards start lying:
//
//   here                                 BATTERY_SENSE_FITTED
//   ImpactSenseAdmin/config/ui.php       'battery_sense_fitted'
//   ImpactSense-Mobile/lib/core/         kBatterySenseFitted
//       device_features.dart
//
// The two UI flags exist because this one cannot be expressed over the
// API: battery_level is validated required|integer|between:0,100, so an
// unfitted device still has to send a number and sends
// BATTERY_UNFITTED_REPORT. With the UI flags false nothing renders that
// number anywhere, which is the only honest thing to do with it.
const bool BATTERY_SENSE_FITTED = false;

// What to report while the divider is absent.
//
// It cannot be "unknown". RiderDeviceController::updateStatus validates
// battery_level as required|integer|between:0,100, so a sentinel like -1
// fails the whole heartbeat with a 422 and takes the device's liveness
// reporting down with it. Omitting the field fails the same way.
//
// So this is a placeholder, and deliberately a high one: 0 renders as a flat
// battery in the dashboard, which is a more misleading thing to show than a
// full one. The honest fix is a nullable battery_level column and a real
// "unknown" state end to end - worth doing, but it is a schema change plus a
// UI change, and it is not what an unfitted resistor pair is owed.
const int BATTERY_UNFITTED_REPORT = 100;

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

// --- Assurance cues: optional, and only ever good news ---------------------
//
// 9 and 10 exist so the rider is told the device is WORKING, not only that it
// thinks they have crashed. Everything the helmet said before this point was
// bad news, which means a silent helmet and a healthy helmet sounded exactly
// the same - and a rider who cannot tell those apart has no reason to trust
// the thing on their head.
//
// Until the two clips are recorded the device simply says nothing extra: a
// track that is not on the card makes the module answer FileMismatch and play
// silence. Nothing else changes, and nothing breaks.
const int TRACK_ONLINE = 9;      // "ImpactSense is online..."
const int TRACK_VOLUME_SET = 10; // "Volume set." - heard AT the new volume

// The range PLAY lists and range-checks, as names rather than literals, so
// adding a clip later means editing these two lines and nothing else.
const int TRACK_FIRST = TRACK_STARTUP;
const int TRACK_LAST  = TRACK_VOLUME_SET;

// ===== Audio behaviour =====

// Master switch for the assurance cues (tracks 9 and 10) ONLY.
//
// The emergency tracks are not assurance and are never gated by this: the
// crash alarm, the SOS clip and the hotline clip play whatever it is set to.
// Nor is the startup announcement, which predates this flag - turning cues
// off should not quietly change what the helmet did at boot yesterday.
const bool AUDIO_ASSURANCE_ENABLED = true;

// Do not re-announce "online" more often than this. A hotspot at the edge of
// range associates and drops repeatedly, and a helmet that says the same
// sentence every forty seconds is a helmet the rider switches off.
const unsigned long AUDIO_ONLINE_CUE_COOLDOWN_MS = 300000UL; // 5 minutes

// How long an assurance cue stays quiet after an emergency clip. The rider is
// listening to something that matters; "ImpactSense is online" must never
// talk over "press the button to cancel".
const unsigned long AUDIO_ASSURANCE_QUIET_MS = 60000UL;

// Minimum gap between ANY clip and an assurance cue. Without this the
// online cue talks over the startup announcement on a board that joins its
// hotspot quickly - the two are only a couple of seconds apart in setup(),
// and the second command simply cuts the first clip off mid-word.
const unsigned long AUDIO_CUE_MIN_GAP_MS = 6000UL;

// Send the DFPlayer's serial frames with interrupts off on this core.
//
// THIS IS THE FIX FOR TRACKS GOING SILENT ONCE WIFI IS UP, and it is worth
// knowing why, because the symptom looks like bad wiring or a bad card.
//
// The DFPlayer is on SoftwareSerial (all three hardware UARTs are taken), so
// every command is ten bytes timed in software: 104 us per bit, 10.4 ms per
// frame, each edge busy-waited against the cycle counter. EspSoftwareSerial
// leaves interrupts ENABLED during that by default, which is fine on an idle
// board.
//
// Once WiFi is up it is not fine, and the reason is specific to this
// framework. Checked against the Arduino-ESP32 sdkconfig this project builds
// with: CONFIG_ARDUINO_RUNNING_CORE is 1, and CONFIG_ARDUINO_EVENT_RUNNING_CORE
// is ALSO 1 - so the network event task shares this core with the loop and
// outranks it. WiFi.begin() is likewise called from setup() on core 1, which
// is where the driver's interrupt handlers get allocated. (The lwIP task is
// the one thing that is NOT to blame: CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0
// pins it to the other core.)
//
// So a frame being clocked out gets preempted part-written, the bits come out
// the wrong length, the module sees a bad checksum, and it discards the
// command - silently, because a frame it never accepted is a frame it cannot
// complain about. Silence is the only symptom.
//
// That is why the startup clip was the one that went missing: it was the only
// track played after ensureConnectivity(), so it was the only one sent through
// a radio-busy board. PLAY worked on the bench because the provisioning
// window runs BEFORE the radio comes up.
//
// With this true, each frame goes out inside taskENTER_CRITICAL - on the ESP32
// that is this core only, and the library re-enables between bytes, so the
// longest blocked stretch is about 1 ms. WiFi lives on the other core, the GPS
// UART has a 128-byte FIFO (about 13 ms of headroom at 9600) and the MPU6050
// is polled, so nothing here is harmed by 1 ms.
const bool AUDIO_PRECISE_TX = true;

// After asking for a track, ask the module whether it actually started, and
// re-send once if it answers that it did not.
//
// Deliberately does NOT retry when the query times out. A timeout says the
// module did not answer, which is not the same as "it is not playing" - on a
// clone that never answers queries, retrying on silence would restart every
// clip from the beginning, every time. Only a module that speaks up and says
// "stopped" earns a second attempt.
const bool AUDIO_VERIFY_PLAY = true;

// How long to settle before ASKING whether it started.
//
// This used to be 150 ms, chosen as "long enough for the module to start".
// It was double-counting: readState() goes through sendStack(), which blocks
// on the previous command's outstanding ACK before it writes anything, so the
// play command's own ACK round-trip - a 10-byte frame at 9600 baud, 104 ms on
// the wire plus the module's few tens of ms - is already spent inside the
// query. 60 ms here still puts the state sample roughly 280 ms after the play
// command went out, which is ample, and saves 90 ms on every verified clip.
const int AUDIO_PLAY_VERIFY_MS = 60;

// How long the library waits for any reply before calling it a timeout.
//
// DFRobotDFPlayerMini.h ships 500 ms, and that number gets paid twice per
// verified play rather than once: sendStack() waits up to 500 ms for the
// PREVIOUS command's ACK before writing a new frame, and then the query waits
// up to 500 ms for its own answer. A clip that started correctly 20 ms in
// could still cost a full second of dead time - four of those in one crash
// response, which is where "the audio feels delayed every time" came from.
//
// 300 ms is about double the measured worst case for a real reply, and takes
// the worst verified play from ~1.15 s to ~0.66 s.
const unsigned long AUDIO_QUERY_TIMEOUT_MS = 300UL;

// readFileCounts() genuinely takes longer than an ACK on a card holding many
// files, and it is only ever asked for by audioReport(), at a bench, by hand.
// Raised for the duration of that one call so a slow card is not misreported
// as a dead link - which is the exact mistake audioReport() warns about.
const unsigned long AUDIO_REPORT_TIMEOUT_MS = 1500UL;

// Print the elapsed time of any play that took longer than this.
//
// Worth the line, because the three reasons a clip goes unheard - a slow or
// lossy serial link, a module that browned out and reset, and a clip that
// played perfectly underneath the buzzer - are indistinguishable by ear and
// easy to tell apart by this number.
// Set above the cost of a HEALTHY verified play, not above zero. A verified
// emergency clip legitimately spends about 280 ms - 60 ms settling, then an
// ACK round-trip and a state reply at roughly 110 ms each - and a line printed
// every time that happens is noise that hides the real thing. 400 ms is clear
// of the healthy case and below one consumed timeout, so the line appears
// when, and only when, something actually waited.
const unsigned long AUDIO_SLOW_PLAY_WARN_MS = 400UL;

// ===== Letting a clip finish before the next one replaces it =====
//
// playMp3Folder() does not queue, it interrupts. The crash sequence was
// written on the assumption that real work separates the narration clips:
// "reporting to the command centre" plays, then the SMS and the HTTP POST
// take their seconds, and only then does "report sent" replace it.
//
// That assumption does not hold on this bench, and not only in a dry run.
// With SIM800L_ENABLED false there is no SMS to wait for, a dry run skips
// both channels outright, and a backend on a local tunnel can answer in
// under a second. The gap collapses to nothing and each clip is cut off a
// few hundred milliseconds in - which sounds exactly like clips failing to
// play, while the module is in fact doing precisely what it was told.
//
// So the wait goes BEFORE the clip that would interrupt, never before the
// outbound work. On a real crash the work has already taken longer than
// the clip and this costs nothing; when the work was instant, it waits out
// the remainder. Nothing that reaches another person is ever delayed by
// it - by the time the first of these runs, the SMS and the report have
// already gone out. The only thing held back is the rider being told so.

// Hard ceiling on one wait, honoured whatever the module says. Deliberately
// not generous: a stuck wait here would stall a crash response, and the
// cost of cutting a clip slightly short is a clipped word.
const unsigned long AUDIO_CLIP_MAX_WAIT_MS = 6000UL;

// What a clip is assumed to run for when the module cannot be asked - the
// no-ACK clones, which answer no queries at all. AUDIO_SCRIPT.md writes the
// clips to a few seconds each, so this is an estimate with a real basis
// rather than a guess.
const unsigned long AUDIO_CLIP_NOMINAL_MS = 3000UL;

// How long after a clip starts before "stopped" is believed. Asked any
// sooner, a module that has not begun yet answers 0, and the wait would
// return immediately having waited for nothing.
const unsigned long AUDIO_CLIP_SETTLE_MS = 400UL;

// ===== Vibration motor (haptic alerts) =====
// NOT FITTED YET. Leave VIBRATION_ENABLED false until the motor is physically
// wired, then flip it and re-flash - nothing else has to change. With it
// false the pin is never touched, so this is inert on a unit without one.
//
// Why haptic at all: at 60-80 km/h wind noise inside a helmet runs 90-100 dB,
// and a piezo buzzer manages ~85 dB at 10 cm before the shell and padding get
// in the way. A precaution alert the rider cannot hear at the speeds where
// precautions matter is not much of a warning. Vibration is felt, so wind
// noise is irrelevant to it. It supplements the buzzer rather than replacing
// it - two channels, in step, on the same pattern.
//
// WIRING - never drive the motor straight from the GPIO. An ERM pulls
// 80-150 mA against a ~40 mA pin rating, and the coil's collapse throws a
// reverse spike that will destroy the pin.
//
//   GPIO 13 --[1k]--> gate/base of an N-MOSFET (AO3400) or NPN (S8050)
//   3V3 ----> motor ----> drain/collector ----> GND
//   flyback diode (1N4148) across the motor, cathode to 3V3   <-- NOT optional
//   100 uF across the motor, mounted close to it, to absorb the inrush
//
// A 3-pin module with the transistor already on board needs none of that:
// signal / VCC / GND, and this pin drives it directly.
//
// Off the 3V3 rail, not the battery - coin and bar ERMs are rated 2.5-3.8 V
// and a freshly charged cell sits at 4.2 V. The 100 uF matters for more than
// the motor: ADC1 also carries the microphone and battery sense, and brush
// noise on a shared rail can read as voice activity. If the crash countdown
// starts cancelling itself the instant the motor runs, that is the cause.
//
// GPIO 13 has no boot-strapping role (unlike 0, 2, 5, 12, 15). 14, 18, 19, 23
// and 25 are equally safe if the layout prefers one of them.
//
// MOUNTING decides more than motor size. Bonded to the outer shell, most of
// the energy is absorbed by the shell and EPS before it reaches the rider -
// a strong motor can feel like nothing. Against the comfort liner, near the
// temple or crown, even a small coin motor is unmistakable. Prefer a bar type
// (1027/1034) over a 10 mm coin: more eccentric mass for the same money.
const bool VIBRATION_ENABLED = false;
const int  VIBRATION_PIN = 13;

// ===== MAX9814 microphone (ADC1_CH7 — input-only GPIO) =====
const int MIC_PIN = 35;

// Voice Activity Detection — leaky-integrator parameters.
//
// MIC_VOICE_THRESHOLD: how far a raw ADC sample (0-4095) must sit from the
// microphone's measured resting level to count as sound. NEVER VALIDATED:
// until the score-cap fix in mic.cpp, voice cancel could not fire at all, so
// nobody has ever seen whether 700 is right. Tune it on the real helmet -
// lower if speaking does not cancel, higher if a quiet room does.
//
// VOICE_SCORE_TARGET: +3 per loud sample, -1 per quiet one. Because the
// countdown only listens while the buzzer is off (see voiceMayCountNow() in
// crash_response.cpp), roughly 40% of samples are taken - so this needs about
// 3 s of fairly continuous speech, after the grace period below.
const int MIC_VOICE_THRESHOLD = 700;
const int VOICE_SCORE_TARGET = 200;

// Voice cancel ignores the microphone for this long after a crash is detected.
// Covers the impact, scraping, and the spoken prompt. MUST be longer than
// 0002.mp3 (TRACK_CRASH_DETECTED) - it plays through a speaker next to the
// mic, and a prompt that outlasts this would be heard as the rider replying.
const unsigned long VOICE_CANCEL_GRACE_MS = 5000;

// 2026-11-17: OFF for bench and demo work.
//
// The gating below assumes the microphone's main enemy is the device's own
// noise - the alarm track and the buzzer. In a room of people watching a
// demonstration the enemy is the room, and a stray conversation inside the
// 30 s window cancels the crash before it ever reports. That looks like a
// failure of crash detection when it is the opposite: voice cancel working
// exactly as designed, on the wrong voice.
//
// MIC_VOICE_THRESHOLD has also never been validated on a real helmet, so
// there is no measured basis yet for trusting it in a noisy room.
//
// Turn back ON once the threshold has been tuned at Stage 8. micInit() still
// runs and still reports its resting level either way, so the microphone is
// still proven at boot. The cancel button is unaffected and remains the
// rider's way out.
const bool VOICE_CANCEL_ENABLED = false;

// After the buzzer or motor switches off, how long before the mic is trusted
// again - a piezo and an ERM both keep ringing briefly.
const unsigned long SELF_NOISE_SETTLE_MS = 40;

// Voice is ignored while the helmet is moving harder than this. Set to allow
// a rider who is standing up or walking, while excluding one still sliding -
// who is in wind loud enough to clear MIC_VOICE_THRESHOLD on its own.
const float VOICE_MOTION_ACCEL_DELTA = 4.0;  // m/s^2 away from 1 g
const float VOICE_MOTION_GYRO        = 90.0; // deg/s

// ===== Manual SOS =====
// Hold the cancel button for this long outside a crash window to send SOS.
const unsigned long MANUAL_SOS_HOLD_MS = 3000;   

const float ACCEL_THRESHOLD = 25.0; // m/s^2 (total acceleration, inc
// ===== Crash detection thresholds =====ludes gravity) //25.0
const float GYRO_THRESHOLD = 200.0; // deg/s (total rotation) // 200.0

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

// How often to push a device status update (battery level, and a GPS speed
// sample when there is a real fix) to the backend.
const unsigned long DEVICE_STATUS_INTERVAL_MS = 60000;

// ===== Idle power state =====
// See power_state.h for what idle does and, more importantly, what it refuses
// to do. Short version: it never slows the sampling loop, so crash detection
// is identical in both states.
//
// How long the device must be continuously still before idling. Two minutes
// rather than seconds: a rider waiting at a long light should not be cycling
// in and out of idle, and the saving from idling those extra seconds is
// nothing next to the churn.
const unsigned long IDLE_ENTER_MS = 120000;

// What counts as "still", measured over one-second buckets. A helmet resting
// on a table varies by only a few hundredths of a m/s^2; a helmet on a bike
// idling at a light varies by far more than this band even before the bike
// moves, because the engine is running. Widen STILL_ACCEL_BAND if a genuinely
// parked device never reaches idle.
const float STILL_ACCEL_BAND = 0.35; // m/s^2 peak-to-peak within one second
const float STILL_GYRO_MAX   = 8.0;  // deg/s

// What ends idle, checked every sample. Deliberately far below the precaution
// thresholds - this only has to mean "something moved", and being too eager
// here costs a little power while being too slow costs readiness.
const float WAKE_ACCEL_DELTA = 1.5;  // m/s^2 away from 1 g
const float WAKE_GYRO        = 25.0; // deg/s

// Heartbeat interval while idle. The device is stationary, so the position and
// battery it would report have barely changed - and this is the one saving
// that actually matters, because it is the modem transmitting.
const unsigned long IDLE_STATUS_INTERVAL_MS = 300000; // 5 min

// CPU frequency scaling. 80 MHz is the floor that still supports the radio,
// and is far more than the loop needs. Set IDLE_CPU_SCALING false if the GPS
// or SIM800L link misbehaves after an idle transition - the Arduino core is
// supposed to re-derive UART dividers on the switch, and this is the first
// thing to rule out if it does not.
const bool     IDLE_CPU_SCALING = true;
const uint32_t IDLE_CPU_MHZ     = 80;
const uint32_t ACTIVE_CPU_MHZ   = 240;

// ===== Serial telemetry =====
// The monitor has two audiences that want opposite things, so there are three
// formats rather than one compromise. Switch with TELEMETRY_MODE below.
//
//   TELEMETRY_DEMO      For showing the device to people who are not reading
//                       the source: panel, adviser, PNP. Names the state in
//                       words, and draws how close the readings are to the
//                       crash level as bars, because "31% of crash level"
//                       lands instantly and "|mag| 7.8 m/s2" does not.
//
//   TELEMETRY_DETAILED  Per-axis figures, peaks between prints, I2C health.
//                       What you want when tuning ACCEL_THRESHOLD or chasing
//                       a wiring fault. This was the previous default.
//
//   TELEMETRY_PLOTTER   Bare "9.81 , 0.42 , normal" CSV, which is the format
//                       the Arduino Serial Plotter graphs.
//
// All three print the same >> event lines, so the story of a crash reads the
// same way whichever one is selected.
enum TelemetryMode { TELEMETRY_DEMO, TELEMETRY_DETAILED, TELEMETRY_PLOTTER };

// DETAILED while bringing the new board up module by module: it is the only
// mode that prints per-axis figures, the peak between prints, and whether an
// I2C read was substituted. DEMO hides all three behind a progress bar, which
// is right for a panel and wrong for a bench.
//
// Put this back to TELEMETRY_DEMO before showing the device to anyone.
const TelemetryMode TELEMETRY_MODE = TELEMETRY_DETAILED;

// How often the block refreshes. 1.5 s suits tuning; 2-3 s reads more calmly
// in a demo, where a panel scrolling too fast is harder to follow than one
// that updates a little behind the action.
const unsigned long TELEMETRY_INTERVAL_MS = 1500;
