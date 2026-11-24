#include "audio.h"      // audioSetVolume(), for the rider-set speaker volume
#include "backend.h"
#include "config.h"
#include "connectivity.h"
#include "device_identity.h"
#include "gps.h"
#include "sensors.h"

#include <ArduinoJson.h>
#include <ArduinoHttpClient.h>
#include <Preferences.h>
// For WiFi.macAddress() in attemptDeviceEnrolment(). The MAC is burned in at
// the factory and readable whether or not WiFi is associated, so this works
// even on a unit that came up over GSM.
#include <WiFi.h>

// Persists a crash report that failed to send so it can be retried later,
// including across a reboot/brownout (SIM800 TX bursts can brown out the
// ESP32 - see the pin map note in config.h). Single slot only: crash reports
// are rare and rate-limited by CRASH_REPORT_COOLDOWN_MS, so a second failed
// report while one is already queued and offline is an extremely unlikely
// edge case, not worth a full queue/array for.
static Preferences crashQueue;
static const char* CRASH_QUEUE_NS = "impactsense-cq";

static void savePendingCrashReport(const String& body) {
  crashQueue.begin(CRASH_QUEUE_NS, false);
  crashQueue.putString("body", body);
  crashQueue.putBool("pending", true);
  crashQueue.end();
}

static void clearPendingCrashReport() {
  crashQueue.begin(CRASH_QUEUE_NS, false);
  crashQueue.putBool("pending", false);
  crashQueue.end();
}

static bool loadPendingCrashReport(String& bodyOut) {
  crashQueue.begin(CRASH_QUEUE_NS, true);
  bool pending = crashQueue.getBool("pending", false);
  if (pending) {
    bodyOut = crashQueue.getString("body", "");
  }
  crashQueue.end();
  return pending && bodyOut.length() > 0;
}

// Adds the two headers that prove this report came from this device, when a
// signing secret has been provisioned. Without one the request goes out
// unsigned and the server treats the device as not yet provisioned — which is
// what keeps a fleet already in the field working. See device_identity.h.
//
// `signedBytes` is what the signature covers: the JSON body on a POST, or the
// query string on a GET, exactly as sent.
static void addSignatureHeaders(HttpClient& http, const String& signedBytes) {
  if (!hasSigningSecret()) {
    return;
  }

  unsigned long counter = nextRequestCounter();
  String signature = signRequest(counter, signedBytes);

  if (signature.length() == 0) {
    return; // signing failed; it already said so on serial
  }

  http.sendHeader("X-Device-Counter", String(counter));
  http.sendHeader("X-Device-Signature", signature);
}

static bool httpPostJson(const char* path, const String& jsonBody, int& statusOut, String& bodyOut) {
  if (activeClient == nullptr) {
    return false;
  }

  HttpClient http(*activeClient, getApiHost(), API_PORT);
  http.setTimeout(10000);

  // Assembled header by header rather than through post(path, type, body),
  // which cannot carry extra headers.
  http.beginRequest();
  int err = http.post(path);
  if (err == 0) {
    http.sendHeader("Content-Type", "application/json");
    http.sendHeader("Content-Length", jsonBody.length());
    addSignatureHeaders(http, jsonBody);
    http.beginBody();
    http.print(jsonBody);
    http.endRequest();
  }

  if (err != 0) {
    Serial.printf("HTTP connection error: %d\n", err);
    return false;
  }

  statusOut = http.responseStatusCode();
  bodyOut = http.responseBody();
  return true;
}

static bool httpGetJson(const char* path, int& statusOut, String& bodyOut) {
  if (activeClient == nullptr) {
    return false;
  }

  HttpClient http(*activeClient, getApiHost(), API_PORT);
  http.setTimeout(10000);

  // The server signs the query string, so hand the same bytes to the signer:
  // everything after the '?', or nothing if the path carries no query.
  String pathStr(path);
  int q = pathStr.indexOf('?');
  String query = (q >= 0) ? pathStr.substring(q + 1) : String("");

  http.beginRequest();
  int err = http.get(path);
  if (err == 0) {
    addSignatureHeaders(http, query);
    http.endRequest();
  }

  if (err != 0) {
    Serial.printf("HTTP connection error: %d\n", err);
    return false;
  }

  statusOut = http.responseStatusCode();
  bodyOut = http.responseBody();
  return true;
}

// Maps how far past threshold the impact was to the severity levels the backend
// accepts (low/medium/high/critical). Tune the ratios once you have real crash-test data.
String severityFromImpact(float totalAccel) {
  float ratio = totalAccel / ACCEL_THRESHOLD;
  if (ratio > 2.0) return "critical";
  if (ratio > 1.5) return "high";
  return "medium";
}

bool reportCrash(float totalAccel, float totalGyro) {
  JsonDocument doc;
  doc["device_code"] = getDeviceCode();
  doc["latitude"]    = getLatitude();
  doc["longitude"]   = getLongitude();
  doc["type"]        = "collision";
  doc["severity"]    = severityFromImpact(totalAccel);

  // Whether the coordinates above are a real satellite fix. Without it, a
  // crash during GPS cold start reports the config.h stub coordinates -
  // central Urdaneta - indistinguishably from a real fix. The coordinates are
  // still sent either way: a report with a doubtful location beats no report.
  //
  // The backend uses this to refuse the nearest-patrol alert on a guessed
  // location, which would otherwise send the unit nearest to a place the
  // rider has never been. A backend that predates the field ignores it.
  doc["location_verified"] = hasGpsFix();

  String body;
  serializeJson(doc, body);

  ensureConnectivity();
  if (activeClient == nullptr) {
    Serial.println("Cannot report crash: no data connection. Queuing for retry.");
    savePendingCrashReport(body);
    return false;
  }

  Serial.printf("Reporting crash via %s:\n", currentNetwork == NET_WIFI ? "WiFi" : "GSM");
  Serial.println(body);

  int status = 0; // stays 0 (not a real HTTP status) if httpPostJson fails before setting it
  String response;
  if (!httpPostJson("/api/device/incident", body, status, response) || status != 201) {
    Serial.printf("Crash report failed (HTTP %d). Queuing for retry.\n", status);
    savePendingCrashReport(body);
    return false;
  }

  Serial.printf("HTTP %d\n", status);
  Serial.println(response);
  return true;
}

// Re-attempts a crash report that failed to send at the time of the crash
// (e.g. GSM/GPRS wasn't attached yet, or the backend was briefly unreachable).
// Called periodically from the main loop - see main.cpp. This is what lets
// the rider's phone still get its FCM "crash detected" push and 911 backup
// countdown even when the device's own HTTP report only succeeds later,
// instead of that whole downstream chain silently never firing.
void retryPendingCrashReport() {
  String body;
  if (!loadPendingCrashReport(body)) {
    return;
  }

  ensureConnectivity();
  if (activeClient == nullptr) {
    return; // still offline - try again on the next interval
  }

  Serial.println("Retrying queued crash report...");

  int status = 0; // stays 0 (not a real HTTP status) if httpPostJson fails before setting it
  String response;
  if (!httpPostJson("/api/device/incident", body, status, response) || status != 201) {
    Serial.printf("Queued crash report retry failed (HTTP %d) - will try again later.\n", status);
    return;
  }

  Serial.println("Queued crash report delivered successfully.");
  clearPendingCrashReport();
}

// Reads speaker_volume out of a device status response and applies it.
//
// Silent and harmless when the field is absent, malformed, or unchanged: an
// older backend, a truncated body or a repeated value must never cost the
// heartbeat its success, because the heartbeat is also how the dashboard
// knows this unit is alive.
static void applyRemoteVolume(const String &response) {
  JsonDocument doc;
  if (deserializeJson(doc, response)) return;          // not JSON - ignore

  JsonVariant value = doc["data"]["speaker_volume"];
  if (value.isNull() || !value.is<int>()) return;      // field absent - ignore

  int wanted = value.as<int>();
  if (wanted < 0 || wanted > 30) return;               // out of range - ignore

  // Only speak up when it actually changes. This runs every 60 s, and a log
  // line per heartbeat would bury everything else in the monitor.
  static int lastApplied = -1;
  if (wanted == lastApplied) return;

  bool first = (lastApplied < 0);
  lastApplied = wanted;
  audioSetVolume(wanted);
  Serial.printf("Speaker volume set to %d by the rider's app.\n", wanted);

  // Say it back, at the new level. The rider moved a slider in an app and
  // then put their phone away; without this the only confirmation is the
  // next crash alarm, which is a poor time to discover the setting was too
  // quiet. Hearing the confirmation AT the chosen volume is the test.
  //
  // Skipped at 0 - there is nothing to hear, and a silent confirmation of
  // silence would read as a failure. Skipped on the first heartbeat after a
  // boot too: that is the device learning the stored value, not the rider
  // changing it, and the helmet has just finished announcing itself.
  if (wanted == 0) {
    Serial.println("Volume is 0 - no confirmation clip, because nothing would be heard.");
  } else if (!first) {
    audioAssuranceCue(TRACK_VOLUME_SET);
  }
}

bool reportDeviceStatus() {
  ensureConnectivity();
  if (activeClient == nullptr) {
    Serial.println("Skipping device status: no data connection.");
    return false;
  }

  JsonDocument doc;
  doc["device_code"]   = getDeviceCode();
  doc["battery_level"] = readBatteryPercent();
  doc["is_active"]     = true;

  // Stamped into the binary from `git describe` at build time, so this is the
  // commit the unit is actually running - not whatever someone typed into SET
  // months ago. See inject_version.py.
  //
  // Sent on every heartbeat, not once per boot, even though it cannot change
  // within one. Thirty-odd bytes buys a dashboard that corrects itself: sent
  // once and that one request lost, the TOC would show the pre-reflash
  // version indefinitely with nothing to reveal it. That is the opposite of
  // the speed fields removed below, which were bytes that bought nothing at
  // all because no one read them.
  doc["firmware_version"] = getFirmwareVersion();

  // No position or speed here, deliberately.
  //
  // This used to attach latitude, longitude and speed_kph on any heartbeat
  // with a satellite fix, and the comment claimed the backend stored them in
  // speed_reports. It never did: RiderDeviceController::updateStatus validated
  // none of the three and dropped them, and nothing anywhere in the backend
  // read or wrote that table. Speed reporting was dropped from the system
  // because the PNP does not operate it, so the fields were paying for
  // themselves in GSM bytes on every heartbeat and buying nothing.
  //
  // The crash report still carries a position - see reportCrash(). That is the
  // one place a location actually matters.

  String body;
  serializeJson(doc, body);

  int status;
  String response;
  if (!httpPostJson("/api/rider/device/status", body, status, response)) {
    Serial.println("Device status update failed: connection error.");
    return false;
  }

  Serial.printf("Device status HTTP %d: %s\n", status, response.c_str());

  // The rider's chosen speaker volume rides back on this same response.
  //
  // No extra request, no polling loop, no push channel: the heartbeat is
  // already made every 60 s while riding, and the backend already returns
  // the whole device record. Carrying the setting on a request that happens
  // anyway costs nothing, and means a helmet that is out of signal simply
  // keeps the last volume it knew rather than falling silent or resetting.
  //
  // Up to 60 s to take effect while riding, up to 5 minutes when idle - the
  // app says so rather than pretending the slider moved the speaker.
  if (status == 200) {
    applyRemoteVolume(response);
  }

  return status == 200;
}

bool fetchEmergencyContact(String &riderName, String &contactName, String &contactPhone) {
  ensureConnectivity();
  if (activeClient == nullptr) {
    Serial.println("Cannot fetch emergency contact: no data connection.");
    return false;
  }

  String path = String("/api/device/emergency-contact?device_code=") + getDeviceCode();

  int status;
  String body;
  if (!httpGetJson(path.c_str(), status, body)) {
    Serial.println("Emergency contact fetch failed: connection error.");
    return false;
  }

  if (status != 200) {
    Serial.printf("Emergency contact fetch failed: HTTP %d\n", status);
    Serial.println(body);
    return false;
  }

  JsonDocument doc;
  DeserializationError parseError = deserializeJson(doc, body);
  if (parseError) {
    Serial.printf("Emergency contact response parse error: %s\n", parseError.c_str());
    return false;
  }

  const char* rider = doc["data"]["rider_name"];
  const char* name  = doc["data"]["name"];
  const char* phone = doc["data"]["phone_number"];

  if (rider == nullptr || name == nullptr || phone == nullptr) {
    Serial.println("Emergency contact response missing expected fields.");
    return false;
  }

  riderName    = String(rider);
  contactName  = String(name);
  contactPhone = String(phone);
  return true;
}

// ── automatic enrolment ──────────────────────────────────────────────────────
//
// Asked for by the pre-final defence panel: device_code and model should be
// assigned when the device connects, not typed into a serial monitor once per
// unit. See device_identity.h for the identity half.
//
// Identification is by MAC address, because it is the only thing this board
// has before it has an identity — device_code is precisely what is being
// handed out, so it cannot also be what identifies the board asking for it.

// How long to keep asking after the server says "seen you, not registered
// yet". Long enough for an operator to find the board on the page and submit
// the form; short enough that a unit powered on with nobody at the console
// finishes booting rather than hanging.
static const unsigned long ENROLMENT_WAIT_MS   = 90000;
static const unsigned long ENROLMENT_RETRY_MS  = 5000;

bool attemptDeviceEnrolment() {
  if (!deviceNeedsEnrolment()) {
    return false;
  }

  if (activeClient == nullptr) {
    Serial.println("[Enrol] No data connection - cannot enrol. Use SET / SECRET over serial.");
    return false;
  }

  String mac = WiFi.macAddress();
  if (mac.length() == 0 || mac == "00:00:00:00:00:00") {
    Serial.println("[Enrol] No MAC address available - cannot enrol.");
    return false;
  }

  JsonDocument doc;
  doc["mac_address"]      = mac;
  doc["firmware_version"] = getFirmwareVersion();

  String body;
  serializeJson(doc, body);

  Serial.println();
  Serial.println("=========================================================");
  Serial.printf (" ENROLLING - this unit has no identity yet.\n");
  Serial.printf ("   MAC      : %s\n", mac.c_str());
  Serial.printf ("   firmware : %s\n", getFirmwareVersion().c_str());
  Serial.println("   Register it on the TOC device page. Open enrolment there");
  Serial.println("   first, or this will be refused.");
  Serial.println("=========================================================");

  unsigned long startedAt = millis();

  while (millis() - startedAt < ENROLMENT_WAIT_MS) {
    int status = 0;
    String response;

    if (!httpPostJson("/api/device/announce", body, status, response)) {
      Serial.println("[Enrol] Could not reach the backend. Retrying...");
      delay(ENROLMENT_RETRY_MS);
      continue;
    }

    // Closed window. No amount of retrying changes this, and hammering a
    // credential endpoint after being refused is exactly the behaviour the
    // window exists to prevent.
    if (status == 403) {
      Serial.println("[Enrol] Enrolment is CLOSED on the server.");
      Serial.println("[Enrol] Open it on the TOC device page, then reset this board.");
      return false;
    }

    if (status == 202) {
      Serial.println("[Enrol] Announced. Waiting for an operator to register it...");
      delay(ENROLMENT_RETRY_MS);
      continue;
    }

    if (status != 200) {
      Serial.printf("[Enrol] Unexpected response (HTTP %d). Giving up.\n", status);
      return false;
    }

    JsonDocument reply;
    if (deserializeJson(reply, response)) {
      Serial.println("[Enrol] Server reply was not valid JSON. Giving up.");
      return false;
    }

    const char* code   = reply["data"]["device_code"] | "";
    const char* model  = reply["data"]["model"]       | "";
    const char* secret = reply["data"]["secret"]      | "";

    if (!applyEnrolledIdentity(String(code), String(model), String(secret))) {
      return false;
    }

    Serial.println();
    Serial.println("=========================================================");
    Serial.printf (" ENROLLED as %s\n", getDeviceCode().c_str());
    Serial.printf ("   model  : %s\n", getDeviceModel().c_str());
    Serial.println("   Signing secret stored. Reports from this unit are signed.");
    Serial.println("=========================================================");
    Serial.println();
    return true;
  }

  Serial.println("[Enrol] Nobody registered this board in time. Booting unprovisioned.");
  Serial.println("[Enrol] Reset to try again, or use SET / SECRET over serial.");
  return false;
}
