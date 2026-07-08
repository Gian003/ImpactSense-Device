#include "backend.h"
#include "config.h"
#include "connectivity.h"
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

static bool httpPostJson(const char* path, const String& jsonBody, int& statusOut, String& bodyOut) {
  if (activeClient == nullptr) {
    return false;
  }

  IPAddress host = resolveApiHost();
  if (host == IPAddress((uint32_t)0)) {
    Serial.println("Cannot reach backend: API host not resolved yet (mDNS lookup pending/failed).");
    return false;
  }

  HttpClient http(*activeClient, host, API_PORT);
  http.setTimeout(10000);

  int err = http.post(path, "application/json", jsonBody);
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

  IPAddress host = resolveApiHost();
  if (host == IPAddress((uint32_t)0)) {
    Serial.println("Cannot reach backend: API host not resolved yet (mDNS lookup pending/failed).");
    return false;
  }

  HttpClient http(*activeClient, host, API_PORT);
  http.setTimeout(10000);

  int err = http.get(path);
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
  doc["device_code"] = DEVICE_CODE;
  doc["latitude"]    = getLatitude();
  doc["longitude"]   = getLongitude();
  doc["type"]        = "collision";
  doc["severity"]    = severityFromImpact(totalAccel);

  String body;
  serializeJson(doc, body);

  ensureConnectivity();
  if (activeClient == nullptr) {
    Serial.println("Cannot report crash: no network connectivity. Queuing for retry.");
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

bool reportHelmetStatus() {
  ensureConnectivity();
  if (activeClient == nullptr) {
    Serial.println("Skipping device status: no network connectivity.");
    return false;
  }

  JsonDocument doc;
  doc["device_code"]   = DEVICE_CODE;
  doc["battery_level"] = readBatteryPercent();
  doc["is_active"]     = true;

  String body;
  serializeJson(doc, body);

  int status;
  String response;
  if (!httpPostJson("/api/rider/helmet/status", body, status, response)) {
    Serial.println("Device status update failed: connection error.");
    return false;
  }

  Serial.printf("Device status HTTP %d: %s\n", status, response.c_str());
  return status == 200;
}

bool fetchEmergencyContact(String &riderName, String &contactName, String &contactPhone) {
  ensureConnectivity();
  if (activeClient == nullptr) {
    Serial.println("Cannot fetch emergency contact: no network connectivity.");
    return false;
  }

  String path = String("/api/device/emergency-contact?device_code=") + DEVICE_CODE;

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
