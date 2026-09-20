#include "backend.h"
#include "config.h"
#include "connectivity.h"
#include "device_identity.h"
#include "gps.h"
#include "sensors.h"

#include <ArduinoJson.h>
#include <ArduinoHttpClient.h>
#include <Preferences.h>

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

  // The backend writes a speed_reports row whenever all three of these are
  // present (RiderDeviceController::updateStatus), which is what feeds "Speed
  // Reports per Area" - so the heartbeat doubles as the speed telemetry and no
  // second periodic call is needed.
  //
  // Only attached on a real fix. getLatitude()/getLongitude() fall back to the
  // stub coordinates in config.h, and sending those would file a reading the
  // device never actually took at a place it has never been, permanently, in a
  // table meant for aggregation.
  int speedKph = getSpeedKph();
  if (hasGpsFix() && speedKph >= 0) {
    doc["latitude"]  = getLatitude();
    doc["longitude"] = getLongitude();
    doc["speed_kph"] = speedKph;
  }

  String body;
  serializeJson(doc, body);

  int status;
  String response;
  if (!httpPostJson("/api/rider/device/status", body, status, response)) {
    Serial.println("Device status update failed: connection error.");
    return false;
  }

  Serial.printf("Device status HTTP %d: %s\n", status, response.c_str());
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
