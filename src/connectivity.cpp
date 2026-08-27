#include "connectivity.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>

#define TINY_GSM_MODEM_SIM800
#include <TinyGsmClient.h>

static HardwareSerial SerialGSM(1);
static TinyGsm modem(SerialGSM);
static WiFiClientSecure secureWifiClient;
static TinyGsmClient gsmClient(modem);

NetworkMode currentNetwork = NET_NONE;
Client* activeClient = nullptr;

const char* getApiHost() {
  return API_PUBLIC_HOST;
}

// Brings the SIM800 modem up once at boot so SMS - which only needs cellular
// registration, not an active GPRS/data session - works regardless of whether
// WiFi or GPRS ends up carrying HTTP traffic. Without this, the modem was only
// ever touched inside connectGSM(), which only runs when WiFi fails - meaning
// SMS would silently never work on a device that has working WiFi.
void modemInit() {
  if (!SIM800L_ENABLED) {
    Serial.println("SIM800L disabled in config.h - skipping modem init.");
    return;
  }

  Serial.println("Initializing SIM800 modem for SMS...");
  SerialGSM.begin(9600, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);

  if (!modem.restart()) {
    Serial.println("SIM800 modem not responding. SMS will be unavailable until it is.");
    return;
  }

  Serial.print("Waiting for cellular network (for SMS)...");
  if (!modem.waitForNetwork(20000)) {
    Serial.println("GSM network registration failed. SMS will be unavailable until signal improves.");
    return;
  }

  Serial.println("SIM800 modem ready (registered on cellular network).");
}

bool connectWiFi(unsigned long timeoutMs) {
  Serial.printf("Connecting to WiFi '%s'", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected. Device IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connection failed.");
  return false;
}

bool connectGSM(unsigned long timeoutMs) {
  if (!SIM800L_ENABLED) {
    return false;
  }

  // modemInit() already brought the modem up at boot; only (re-)wait for
  // network registration here if that hasn't succeeded yet.
  if (!modem.isNetworkConnected()) {
    Serial.println("Attempting GSM/GPRS fallback...");
    if (!modem.waitForNetwork(timeoutMs)) {
      Serial.println("GSM network registration failed.");
      return false;
    }
  }

  Serial.printf("Connecting to APN '%s'...\n", GPRS_APN);
  if (!modem.gprsConnect(GPRS_APN, GPRS_USER, GPRS_PASS)) {
    Serial.println("GPRS connect failed.");
    return false;
  }

  Serial.println("GSM/GPRS connected.");
  return true;
}

// Called before every network operation so the device recovers automatically if WiFi drops.
void ensureConnectivity() {
  if (WiFi.status() == WL_CONNECTED) {
    currentNetwork = NET_WIFI;
    secureWifiClient.setInsecure(); // skip certificate verification — Cloudflare Tunnel handles TLS
    activeClient = &secureWifiClient;
    return;
  }

  if (connectWiFi(5000)) {
    currentNetwork = NET_WIFI;
    secureWifiClient.setInsecure();
    activeClient = &secureWifiClient;
    return;
  }

  if (SIM800L_ENABLED && (modem.isGprsConnected() || connectGSM(20000))) {
    currentNetwork = NET_GSM;
    activeClient = &gsmClient; // GSM uses plain TinyGsmClient (no TLS wrapper available)
    return;
  }

  currentNetwork = NET_NONE;
  activeClient = nullptr;
  Serial.println("No connectivity available (WiFi and GSM both failed).");
}

// SMS uses the GSM signaling channel (cellular registration only) - it does not
// need GPRS/data or WiFi to be up, which is exactly why it's the fallback for
// reaching someone when the rider or their phone may be unreachable.
bool sendSms(const String &number, const String &message) {
  if (!SIM800L_ENABLED) {
    Serial.println("SIM800L disabled in config.h - skipping SMS.");
    return false;
  }

  if (number.length() == 0) {
    Serial.println("Cannot send SMS: no emergency contact cached yet.");
    return false;
  }

  Serial.printf("Sending SMS to %s: %s\n", number.c_str(), message.c_str());

  bool sent = modem.sendSMS(number, message);
  Serial.println(sent ? "SMS sent successfully." : "SMS send failed.");

  return sent;
}

// Dials a number and returns immediately — does NOT block.
// Call callEnd() when done. Audio can be played between callStart() and callEnd()
// and will be transmitted through SIM800L MIC+ if DAC_R is wired to MIC+.
bool callStart(const String &number) {
  if (!SIM800L_ENABLED) {
    Serial.println("SIM800L disabled in config.h - skipping emergency call.");
    return false;
  }

  if (!modem.isNetworkConnected()) {
    Serial.println("Cannot place emergency call: no cellular registration.");
    return false;
  }

  Serial.printf("Dialling %s...\n", number.c_str());
  bool dialled = modem.callNumber(number);

  if (!dialled) {
    Serial.println("Emergency call failed to dial.");
    return false;
  }

  Serial.println("Call dialled. Waiting for answer...");
  return true;
}

// Hangs up the active call.
void callEnd() {
  modem.callHangup();
  Serial.println("Emergency call ended.");
}
