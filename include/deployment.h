#pragma once

// ============================================================================
// DEPLOYMENT MODE
// ============================================================================
//
// One switch separating "on the bench, with lab WiFi and a USB cable" from
// "strapped to a helmet in Urdaneta with nobody watching". Everything that has
// to change between those two situations is listed below, in the order it has
// to be done.
//
// Leave this false until the whole of STEP 1-4 is genuinely finished. Flipping
// it early does not get you a partly-deployed device; it gets you a device
// that cannot reach the backend at all, because step 1 is a coordinated change
// on both sides and half of it is worse than neither half.
//
// To go back to bench testing at any time, set it to false and re-flash. That
// is the whole rollback - nothing else needs undoing.
const bool DEPLOYMENT_MODE = false;

// ============================================================================
// WHAT THIS FLAG ALREADY DOES (no further edits needed)
// ============================================================================
//
//   API_PORT           443 -> 80          (config.h)
//   GSM_DATA_ENABLED   false -> true      (config.h)
//   WiFi HTTP client   TLS -> plain       (connectivity.cpp, follows API_PORT)
//   Boot provisioning  8 s -> ~0.3 s      (device_identity.cpp)
//
// The rest of this file is the work that cannot be done by a flag, because it
// needs changes outside this firmware.
//
// ============================================================================
// STEP 1 - GIVE THE DEVICE A DATA PATH THAT WORKS WITHOUT WIFI  [BLOCKING]
// ============================================================================
//
// This is the one that decides whether the deployed system does anything at
// all. Read it before the others.
//
// ensureConnectivity() tries WiFi first, against the hardcoded SSID in
// config.h. On a moving motorcycle that never matches, so a deployed unit
// falls to GSM - and with the backend on HTTPS-only port 443, GSM cannot
// carry HTTP at all (see GSM_DATA_ENABLED in config.h for why). The unit then
// runs permanently in the SMS + hotline-call fallback:
//
//   - no incident rows are created
//   - the TOC dashboard never updates
//   - Pusher, Twilio and the rider's FCM push never fire
//
// None of that shows up on the bench, because the bench has WiFi.
//
// The fix is a coordinated change on both sides. Do them in this order:
//
//   1a. In the Cloudflare dashboard for impactsense.org, turn OFF
//       "Always Use HTTPS" (SSL/TLS -> Edge Certificates). Until this is off,
//       port 80 answers every request with a 301 to https:// - and
//       ArduinoHttpClient does not follow redirects, so the device sees a
//       redirect it cannot act on and the report is queued and lost.
//
//   1b. Confirm from a computer, NOT a browser (browsers silently upgrade to
//       HTTPS and will hide the failure):
//
//         curl -v http://impactsense.org/api/health
//
//       Expect 200 and {"status":"up"}. A 301 means 1a has not taken effect
//       yet - Cloudflare can take a minute, and the old redirect may be
//       cached.
//
//   1c. Only then set DEPLOYMENT_MODE = true and re-flash.
//
// What this costs: device_code, GPS coordinates and severity travel in
// cleartext over the carrier network. That is a real and permanent tradeoff,
// not a temporary shortcut, so state it plainly in the manuscript rather than
// leaving a panelist to find it. The reason it is acceptable here is that the
// alternative is no backend connectivity whatsoever, and the payload contains
// no credentials - the pairing_key never reaches this firmware.
//
// The proper fix, when there is budget: replace the SIM800L with a 4G module
// (SIM7600 or A7670). Those do TLS 1.2 with SNI correctly, so port 443 works
// over cellular and none of this section applies. That is a hardware change
// and a TinyGSM modem-define change, nothing more.
//
// ============================================================================
// STEP 2 - STOP REPORTING A GUESSED LOCATION AS IF IT WERE REAL  [SAFETY]
// ============================================================================
//
// getLatitude()/getLongitude() fall back to STUB_LATITUDE/STUB_LONGITUDE -
// central Urdaneta - whenever there is no satellite fix. reportCrash() sends
// them unconditionally, and nothing in the payload says which kind it is. So a
// crash during GPS cold start (minutes, on a NEO-6M, and crashes happen early
// in rides) dispatches responders, confidently, to a place the rider has never
// been.
//
// Sending nothing is not the alternative - the backend requires the fields,
// and a report with a doubtful location still beats no report. What responders
// need is to know which one they are looking at.
//
// This needs a backend change, so it is not behind the flag. Both halves:
//
//   Backend - add a nullable boolean to incidents:
//
//     php artisan make:migration add_location_verified_to_incidents_table
//
//     $table->boolean('location_verified')->nullable()->after('longitude');
//
//     Add 'location_verified' to the validate() array and the create() array
//     in DeviceController::reportIncident, then surface it on the TOC
//     dashboard - a plain "LOCATION UNVERIFIED" badge on the incident row is
//     enough. An operator who sees it works the phone instead of the map.
//
//   Firmware - in backend.cpp, inside reportCrash(), add one line:
//
//     doc["location_verified"] = hasGpsFix();
//
//   hasGpsFix() already exists (gps.h) and is already used to guard the speed
//   telemetry in reportDeviceStatus() for exactly this reason. The crash path
//   was deliberately left alone because it needed the backend field first.
//
// ============================================================================
// STEP 3 - MAKE A WEDGED UNIT RECOVER BY ITSELF  [RELIABILITY]
// ============================================================================
//
// There is no watchdog. A hung I2C bus, a wedged modem or a brownout lockup
// leaves the unit silently dead - and dead is indistinguishable from "has not
// crashed" when viewed from the backend, so nobody finds out.
//
// The complication is that handleCrash() blocks for a long time on purpose:
// CALL_CONNECT_WAIT_MS (8 s) plus CALL_HOLD_MS (30 s) plus the HTTP timeout.
// A naive watchdog reboots the device in the middle of the emergency call.
// So the timeout has to clear that worst case with room to spare, which is
// why 90 s below and not the usual 5-10 s. It is still far better than never
// recovering.
//
// Uncomment in main.cpp:
//
//   #include <esp_task_wdt.h>
//
//   // in setup(), after the modules are up:
//   if (DEPLOYMENT_MODE) {
//     esp_task_wdt_init(90, true);   // 90 s, panic+reboot on expiry
//     esp_task_wdt_add(NULL);        // watch the Arduino loop task
//   }
//
//   // as the first line of loop():
//   if (DEPLOYMENT_MODE) esp_task_wdt_reset();
//
// Then pair it with the other half, which is on the backend: alert on
// heartbeats that stop arriving. reportDeviceStatus() runs every
// DEVICE_STATUS_INTERVAL_MS (60 s), so a unit whose devices.updated_at is
// older than ~10 minutes is not quiet, it is broken. Without that, a rebooting
// unit and a dead one still look the same from the office.
//
// ============================================================================
// STEP 4 - FIELD READINESS  [DO BEFORE HANDING A UNIT TO A RIDER]
// ============================================================================
//
// Power - the thing that actually kills field units:
//   - Size the pack for the worst case (modem transmitting + DFPlayer + amp +
//     GPS acquiring), not idle. The SIM800L's ~2 A bursts come out of the same
//     cell as everything else once USB is gone.
//   - Keep the 1000-3300 uF bulk cap at the SIM800L's own VCC/GND pins.
//   - Calibrate BATTERY_DIVIDER_RATIO against a multimeter on the real pack.
//     Uncalibrated, the heartbeat cheerfully reports a healthy battery on a
//     unit that is minutes from cutting out.
//   - Measure real runtime on a full charge before promising anyone a number.
//
// Thresholds - ACCEL_THRESHOLD and GYRO_THRESHOLD are still the original
// guesses. Tune them on real Urdaneta roads before deployment: potholes and
// hard braking are what generate false positives, and a false positive
// dispatches PNP. Check the cancel button is pressable with riding gloves on.
//
// SIM and carrier:
//   - Prepaid load runs out silently and the backend cannot tell you it did.
//     Someone has to own topping it up.
//   - SIM800L is 2G only. Confirm 2G is still live on your carrier in Urdaneta
//     and check the announced sunset date - this is what ages the design out.
//
// Firmware updates - there is no OTA. Every fix means physically collecting
// every unit and plugging in USB. Past a handful of units, add OTA before
// deploying rather than after.
//
// Provisioning runbook - per unit, in this order:
//   1. Register the device_code in the admin panel (toc/devices -> Register
//      New Device). The backend generates the pairing_key there.
//   2. Flash, open Serial Monitor at 115200 (line ending: Newline), reset,
//      and send  SET <device_code>,<model>,<firmware>  within the window.
//   3. Send SHOW to confirm it saved.
//   4. Rider pairs in the mobile app using the pairing_key from step 1.
//
// ============================================================================
// KNOWN AND ACCEPTED - raise with the adviser, do not silently ship
// ============================================================================
//
// device_code is the only credential on POST /api/device/incident. Anyone who
// learns or guesses one can file fake incidents against a real rider, and
// those reach PNP. The 30/minute throttle bounds the volume but not the
// premise. Fixing it properly means a per-device shared secret and a signed
// payload, which is a larger change than a capstone timeline allows - so the
// honest move is to name it as a known limitation with a stated mitigation,
// not to leave it unmentioned.
//
// The device also emits continuous GPS speed telemetry tied to a device, and
// devices are paired to identified riders. Have an answer ready for retention
// and consent before PNP asks.
