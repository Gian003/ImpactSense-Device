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
//   Boot provisioning  20 s -> ~0.3 s     (device_identity.cpp)
//                      ^ PROVISION_WINDOW_MS. This is the one with a safety
//                        cost: every boot spends it not watching for a crash,
//                        including the reboot after a brownout mid-ride. In
//                        deployment mode the window only opens if a key is
//                        held through reset, so the commands stay available
//                        without the per-boot penalty.
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
// The fix is a coordinated change on both sides.
//
//   1a. DONE. "Always Use HTTPS" is off in the Cloudflare dashboard for
//       impactsense.org (SSL/TLS -> Edge Certificates). While it was on, port
//       80 answered every request with a 301 to https:// - and
//       ArduinoHttpClient does not follow redirects, so the device saw a
//       redirect it could not act on and the report was queued and lost.
//
//   1b. DONE, confirmed 2026-09-22:
//
//         curl -i http://impactsense.org/api/health
//         HTTP/1.1 200 OK
//         {"status":"up"}
//
//       Plain HTTP, no redirect. Re-check with curl and NOT a browser if this
//       is ever in doubt - browsers silently upgrade to HTTPS and hide the
//       failure. A 301 means the setting has been turned back on.
//
//   1c. OUTSTANDING, and no longer blocked by the server.
//
//       What is still missing is a SIM that can actually carry 2G GPRS data.
//       The TNT SIM tested here had none for a week despite showing signal,
//       and a phone cannot answer the question because a phone does its data
//       over LTE. Turn GPRS_SELF_TEST on in config.h, boot once, and read
//       whether the modem gets an IP address. Only when it does is there any
//       point setting DEPLOYMENT_MODE = true and re-flashing.
//
// What this costs: device_code, GPS coordinates and severity travel in
// cleartext over the carrier network, so anyone positioned to read that
// traffic can read a crash report. State it plainly in the manuscript rather
// than leaving a panelist to find it.
//
// What it no longer costs, since the device-signing work: the ability to
// FORGE one. Every report carries an HMAC over its own body and a counter that
// only climbs, and the signing secret itself never travels - so an
// eavesdropper on plain HTTP can read a report and cannot fabricate, alter or
// replay one. Confidentiality is the whole of what is given up here.
//
// The proper fix, when there is budget: replace the SIM800L with a 4G module
// (SIM7600 or A7670). Those do TLS 1.2 with SNI correctly, so port 443 works
// over cellular and none of this section applies - and a 4G module also
// sidesteps 1c entirely, since the 2G data being withdrawn is exactly what
// the SIM800L depends on. That is a hardware change and a TinyGSM
// modem-define change, nothing more.
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
// MOSTLY DONE. What is in place:
//
//   Firmware - reportCrash() sends location_verified = hasGpsFix().
//
//   Backend - incidents.location_verified (migration 2026_09_18_000001),
//   accepted by DeviceController::reportIncident. The nearest-patrol alert
//   refuses to run unless it is true, and says why in the incident timeline.
//
// What is left:
//
//   1. Run `php artisan migrate` on the server. Until then the backend
//      ignores the field (deliberately - see App\Support\PatrolAlertSchema)
//      and every report is stored as "unknown".
//
//   2. Show it on the TOC dashboard. A plain "LOCATION UNVERIFIED" badge on
//      the incident row is enough: an operator who sees it works the phone
//      instead of the map. The value is stored; nothing displays it yet.
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
// Provisioning runbook - per unit. Prefer the first route; it is fewer steps
// and cannot get the secret wrong. The full version, with the recovery cases,
// is on the Device Management page under "How to register and set up a new
// device".
//
// Route A - the board enrols itself (needs a data path to the backend):
//   1. toc/devices -> Connect a Device -> "Enrol next device". Do this BEFORE
//      powering the board; the window is 10 minutes.
//   2. Flash and power the board. It announces itself by MAC and appears in
//      the list.
//   3. "Use this device" on its row, type the device_code printed on the unit,
//      then Register Device.
//   4. Nothing to type into the board - it collects its device_code, model
//      AND signing secret by itself and prints "ENROLLED as <code>".
//   5. Rider pairs in the mobile app using the pairing_key on that row.
//
// Route B - no data path on the board, so by hand:
//   1. Register it in toc/devices with the device_code and the board's MAC
//      (it prints "MAC : ..." on boot while unprovisioned; SHOW prints it any
//      time). Without the MAC that unit can never enrol itself later.
//   2. Flash, open Serial Monitor at 115200 (line ending: Newline), reset, and
//      send  SET <device_code>  within the window. A third <firmware> field is
//      accepted and ignored - that value comes from `git describe` at build
//      time now.
//   3. Send the SECRET line from that row ("Copy signing command" copies it
//      whole). SKIPPING THIS LEAVES THE UNIT REPORTING UNSIGNED, which the
//      server accepts from anyone claiming to be it.
//   4. Send SHOW to confirm the code saved and the secret reads "stored".
//   5. Rider pairs in the mobile app using the pairing_key from step 1.
//
// To recycle a board onto a different device_code:
//   Edit the row's device_code in the admin panel, then send SET <new_code>.
//   The signing secret survives, because SET writes only the code and model.
//   Do NOT delete the row - that destroys the secret the board is holding.
//
// To return a board to blank so it enrols from scratch: send FORGET, then
// FORGET CONFIRM. A board holding both a code and a secret never announces
// itself, so this is the only way back onto route A short of erasing flash.
//
// ============================================================================
// STEP 5 - DEEP SLEEP  [DEFERRED - read the cost before building it]
// ============================================================================
//
// The idle tier (power_state.h) is in and safe: it never slows the sampling
// loop, so a device that has idled detects a crash exactly as fast as one that
// has not. Deep sleep is the next tier and it does NOT have that property.
// Build it only with the cost below understood and written down.
//
// How it would work: the MPU6050 raises its motion interrupt on an INT pin,
// the ESP32 arms esp_sleep_enable_ext0_wakeup() on that pin and calls
// esp_deep_sleep_start(). Movement reboots the chip. Straightforward, and the
// hardware already supports it.
//
// WHAT IT COSTS. Waking is not instant:
//
//   ESP32 boot                    ~0.5-1 s
//   GSM re-registration           10-30 s
//   GPS re-acquisition            30 s to several minutes
//
// A crash is motion, so the MPU does wake the device - but a crash in the
// first minute of riding would then be reported by a unit with no fix and
// possibly no network. That turns STEP 2's stub-coordinate problem from
// occasional into systematic, and it lands it precisely when a rider is
// pulling into traffic. The riskiest moment becomes the moment after every
// wake.
//
// So if it is built: require a long stationary period (15+ minutes, i.e.
// genuinely parked, not waiting at a light), and hold off crash REPORTING
// - never crash detection - for a few seconds after wake so GPS has a chance,
// queueing rather than discarding. STEP 2's location_verified field carries
// the honest answer when it does not.
//
// DO NOT EXPECT MICROAMPS. On a DevKit board the AMS1117 regulator and the
// CP2102/CH340 USB chip keep drawing regardless of what the ESP32 does: real
// deep sleep lands around 5-15 mA, not the datasheet's 10 uA. Still a large
// win over ~150 mA active, but the datasheet figure needs a bare ESP32-WROOM
// on a custom board.
//
// THE APP CANNOT WAKE IT. A sleeping device has no radio, and even awake it is
// an HTTP client with no inbound channel. So "keep the device awake" belongs in
// the app as a SETTING, stored on the devices row and read in the response to a
// heartbeat the device already sends - not as a prompt the device waits on. A
// prompt would mean staying awake to poll for the answer, spending exactly the
// power the sleep was meant to save.
//
// CHEAPER FIRST. Before any of this, turn the WiFi radio off when
// DEPLOYMENT_MODE is set. In the field WiFi never associates, yet
// ensureConnectivity() still scans for 5 s per attempt and keeps the radio
// powered - roughly 80-100 mA continuous for nothing. WiFi.mode(WIFI_OFF) is a
// few lines, roughly doubles runtime, and takes latency off the crash path
// rather than adding it.
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
