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
Client *activeClient = nullptr;

const char *getApiHost()
{
  return API_PUBLIC_HOST;
}

// Finds the rate the modem is actually listening at.
//
// SIM800L ships in autobaud mode and keeps whichever rate it last locked onto,
// so a module that settled on 9600 is deaf to a port opened at 115200. An
// autobauding module also needs to see a few ATs before it syncs, hence the
// repeats rather than a single attempt per rate.
//
// Returns the working rate, or 0 if nothing answered at any of them.
static uint32_t detectModemBaud()
{
  static const uint32_t candidates[] = {GSM_BAUD, 9600, 115200, 57600, 38400, 19200, 4800};

  for (uint32_t baud : candidates)
  {
    SerialGSM.end();
    SerialGSM.begin(baud, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
    delay(200);

    for (int attempt = 0; attempt < 3; attempt++)
    {
      if (modem.testAT(800))
      {
        return baud;
      }
    }
  }

  return 0;
}

// Brings the SIM800 modem up once at boot so SMS - which only needs cellular
// registration, not an active GPRS/data session - works regardless of whether
// WiFi or GPRS ends up carrying HTTP traffic. Without this, the modem was only
// ever touched inside connectGSM(), which only runs when WiFi fails - meaning
// SMS would silently never work on a device that has working WiFi.
void modemInit()
{
  if (!SIM800L_ENABLED)
  {
    Serial.println("SIM800L disabled in config.h - skipping modem init.");
    return;
  }

  Serial.println("Initializing SIM800 modem for SMS...");

  // A SIM800L takes several seconds from power-up before it answers AT at all.
  // The previous 1000ms was inside that window, so a perfectly healthy module
  // could be written off before it had finished booting.
  SerialGSM.begin(GSM_BAUD, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  delay(3000);

  // Probe before restarting. restart() issues AT+CFUN=1,1, which reboots the
  // radio and pulls peak current; if the real problem is that we are talking
  // at the wrong baud rate, that reboot fails for a reason that has nothing to
  // do with the modem's health, and the old code reported exactly that.
  uint32_t baud = detectModemBaud();

  if (baud == 0)
  {
    Serial.println("SIM800 modem not responding at any baud rate.");
    Serial.println("  This is a wiring or power fault, not a SIM or load problem:");
    Serial.println("  - SIM800 TX -> ESP32 GPIO 26, SIM800 RX -> ESP32 GPIO 27 (crossed)");
    Serial.println("  - VCC 3.4-4.4V from a supply that can deliver ~2A bursts");
    Serial.println("  - grounds tied together, and a 1000uF+ cap at the module");
    Serial.println("  - status LED blinking; solid or dark means it is not running");
    return;
  }

  if (baud != GSM_BAUD)
  {
    Serial.printf("Modem answered at %lu baud, not the configured %lu.\n",
                  (unsigned long)baud, (unsigned long)GSM_BAUD);
    Serial.printf("  Set GSM_BAUD = %lu in config.h to skip this probe next boot.\n",
                  (unsigned long)baud);
  }
  else
  {
    Serial.printf("Modem answered at %lu baud.\n", (unsigned long)baud);
  }

  // Now that we know the link works, a failed restart means something real.
  if (!modem.restart())
  {
    Serial.println("Modem is talking but would not restart - usually an undersized supply.");
    Serial.println("  Trying a plain init instead...");
    if (!modem.init())
    {
      Serial.println("SIM800 modem not responding. SMS will be unavailable until it is.");
      return;
    }
  }

  // Separating these tells you which half is wrong. A locked or absent SIM and
  // a weak signal need completely different fixes, and both used to surface as
  // the same "registration failed" line.
  SimStatus sim = modem.getSimStatus();
  if (sim != SIM_READY)
  {
    Serial.printf("SIM not ready (status %d) - absent, badly seated, or PIN-locked.\n",
                  (int)sim);
    Serial.println("  Put the SIM in a phone and switch the PIN off, then retry.");
    return;
  }

  Serial.print("Waiting for cellular network (for SMS)...");
  if (!modem.waitForNetwork(20000))
  {
    Serial.println();
    int16_t csq = modem.getSignalQuality();
    Serial.printf("GSM registration failed. Signal quality %d/31%s\n", csq,
                  (csq == 99 || csq == 0) ? " - check the antenna" : "");
    Serial.println("  SIM800L is 2G only; confirm 2G is still available here.");
    return;
  }
  Serial.println(" done.");

  int16_t csq = modem.getSignalQuality();
  Serial.printf("SIM800 modem ready (registered, signal %d/31%s).\n", csq,
                csq < 10 ? " - marginal, SMS may fail" : "");
}

bool connectWiFi(unsigned long timeoutMs)
{
  Serial.printf("Connecting to WiFi '%s'", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs)
  {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.printf("WiFi connected. Device IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connection failed.");
  return false;
}

bool connectGSM(unsigned long timeoutMs)
{
  if (!SIM800L_ENABLED)
  {
    return false;
  }

  // modemInit() already brought the modem up at boot; only (re-)wait for
  // network registration here if that hasn't succeeded yet.
  if (!modem.isNetworkConnected())
  {
    Serial.println("Attempting GSM/GPRS fallback...");
    if (!modem.waitForNetwork(timeoutMs))
    {
      Serial.println("GSM network registration failed.");
      return false;
    }
  }

  Serial.printf("Connecting to APN '%s'...\n", GPRS_APN);
  if (!modem.gprsConnect(GPRS_APN, GPRS_USER, GPRS_PASS))
  {
    Serial.println("GPRS connect failed.");
    return false;
  }

  Serial.println("GSM/GPRS connected.");
  return true;
}

// Called before every network operation so the device recovers automatically if WiFi drops.
void ensureConnectivity()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    currentNetwork = NET_WIFI;
    secureWifiClient.setInsecure(); // skip certificate verification — Cloudflare Tunnel handles TLS
    activeClient = &secureWifiClient;
    return;
  }

  if (connectWiFi(5000))
  {
    currentNetwork = NET_WIFI;
    secureWifiClient.setInsecure();
    activeClient = &secureWifiClient;
    return;
  }

  if (SIM800L_ENABLED && (modem.isGprsConnected() || connectGSM(20000)))
  {
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
bool sendSms(const String &number, const String &message)
{
  if (!SIM800L_ENABLED)
  {
    Serial.println("SIM800L disabled in config.h - skipping SMS.");
    return false;
  }

  if (number.length() == 0)
  {
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
bool callStart(const String &number)
{
  if (!SIM800L_ENABLED)
  {
    Serial.println("SIM800L disabled in config.h - skipping emergency call.");
    return false;
  }

  if (!modem.isNetworkConnected())
  {
    Serial.println("Cannot place emergency call: no cellular registration.");
    return false;
  }

  Serial.printf("Dialling %s...\n", number.c_str());
  bool dialled = modem.callNumber(number);

  if (!dialled)
  {
    Serial.println("Emergency call failed to dial.");
    return false;
  }

  Serial.println("Call dialled. Waiting for answer...");
  return true;
}

// Hangs up the active call.
void callEnd()
{
  modem.callHangup();
  Serial.println("Emergency call ended.");
}
