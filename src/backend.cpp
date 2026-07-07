#include "backend.h"
#include "config.h"
#include "connectivity.h"
#include "gps.h"
#include "sensors.h"

#include <ArduinoJson.h>
#include <ArduinoHttpClient.h>

static bool httpPostJson(const char* path, const String& jsonBody, int& statusOut, String& bodyOut) {
  if (activeClient == nullptr) {
    return false;
  }

  HttpClient http(*activeClient, API_HOST, API_PORT);
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

  HttpClient http(*activeClient, API_HOST, API_PORT);
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
  ensureConnectivity();
  if (activeClient == nullptr) {
    Serial.println("Cannot report crash: no network connectivity.");
    return false;
  }

  JsonDocument doc;
  doc["device_code"] = DEVICE_CODE;
  doc["latitude"]    = getLatitude();
  doc["longitude"]   = getLongitude();
  doc["type"]        = "collision";
  doc["severity"]    = severityFromImpact(totalAccel);

  String body;
  serializeJson(doc, body);

  Serial.printf("Reporting crash via %s:\n", currentNetwork == NET_WIFI ? "WiFi" : "GSM");
  Serial.println(body);

  int status;
  String response;
  if (!httpPostJson("/api/device/incident", body, status, response)) {
    Serial.println("Crash report failed: connection error.");
    return false;
  }

  Serial.printf("HTTP %d\n", status);
  Serial.println(response);
  return status == 201;
}

bool reportHelmetStatus() {
  ensureConnectivity();
  if (activeClient == nullptr) {
    Serial.println("Skipping helmet status: no network connectivity.");
    return false;
  }

  JsonDocument doc;
  doc["device_code"]   = DEVICE_CODE;
  doc["battery_level"] = readBatteryPercent();
  doc["is_active"]     = true;

  // Only attach a speed sample once GPS has a real fix - STUB_LATITUDE/
  // STUB_LONGITUDE (indoors, no satellite lock) would otherwise pollute
  // Speed Reports per Area with a fake, always-identical location.
  if (gpsHasFix()) {
    doc["latitude"]  = getLatitude();
    doc["longitude"] = getLongitude();
    doc["speed_kph"] = (int) round(getSpeedKph());
  }

  String body;
  serializeJson(doc, body);

  int status;
  String response;
  if (!httpPostJson("/api/rider/helmet/status", body, status, response)) {
    Serial.println("Helmet status update failed: connection error.");
    return false;
  }

  Serial.printf("Helmet status HTTP %d: %s\n", status, response.c_str());
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
