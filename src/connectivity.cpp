#include "connectivity.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>

#define TINY_GSM_MODEM_SIM800
#include <TinyGsmClient.h>

static HardwareSerial SerialGSM(1);
static TinyGsm modem(SerialGSM);
static WiFiClientSecure secureWifiClient;
static WiFiClient plainWifiClient;
static TinyGsmClient gsmClient(modem);

// Which WiFi client HTTP should use, decided by the port rather than by a
// second flag - a TLS client pointed at port 80 (or a plain one at 443) fails
// in a way that looks like a network fault, and keeping the two in sync by
// hand is exactly the kind of thing that gets missed when switching into
// deployment mode. API_PORT is a compile-time constant, so this costs nothing
// at runtime.
static Client *wifiHttpClient()
{
  if (API_PORT == 443)
  {
    secureWifiClient.setInsecure(); // Cloudflare Tunnel terminates TLS; no cert store on-device
    return &secureWifiClient;
  }
  return &plainWifiClient;
}

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

// Attaches to GPRS once and reports whether the network hands out an address.
// Deliberately separate from connectGSM(): this has to run even with
// GSM_DATA_ENABLED off, because its whole purpose is to find out whether
// turning that on could ever work on the SIM that is fitted. Getting an IP
// proves the carrier still offers 2G data to this SIM; it does not prove the
// backend is reachable over it - STEP 1 in deployment.h covers that part.
static void runGprsSelfTest()
{
  Serial.printf("[GPRS test] Attaching to data with APN '%s' (%s SIM)...\n",
                GPRS_APN, SIM_CARRIER == CARRIER_GLOBE_TM ? "Globe/TM" : "Smart/TNT");

  unsigned long start = millis();
  if (!modem.gprsConnect(GPRS_APN, GPRS_USER, GPRS_PASS))
  {
    Serial.printf("[GPRS test] FAILED after %lus - no 2G data on this SIM.\n",
                  (millis() - start) / 1000);
    Serial.println("[GPRS test]   SMS and calls are unaffected; the crash path still works.");
    Serial.println("[GPRS test]   Check SIM_CARRIER matches the SIM, then try the other network.");
    Serial.println("[GPRS test]   If both fail, STEP 1 needs a 4G module, not a different SIM.");
    return;
  }

  Serial.printf("[GPRS test] OK in %lus - network assigned IP %s. 2G data works on this SIM.\n",
                (millis() - start) / 1000, modem.getLocalIP().c_str());
  modem.gprsDisconnect();
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

    // Reaching this point means the SIM already passed getSimStatus() above,
    // so it is readable and unlocked. Said explicitly because a failed
    // registration looks like a bad SIM, and swapping SIMs is the first thing
    // anyone tries - which cannot help when the fault is power or the network.
    Serial.printf("GSM registration failed (signal %d/31). The SIM card itself read fine.\n", csq);

    // 99 and 0 used to share one "check the antenna" hint, but they mean
    // different things. 0 is a real measurement of almost nothing. 99 is no
    // measurement at all: no 2G network was found, or the radio kept resetting
    // under its transmit bursts before it could take one. Only the second
    // points at the antenna, and neither points at the SIM.
    if (csq > 5 && csq != 99)
    {
      Serial.println("  Signal is usable, so the network refused or delayed registration:");
      Serial.println("  SIM not registered with the carrier, no load, or just slower than 20 s.");
      return;
    }

    Serial.println(csq == 99
        ? "  99 = no measurement: no 2G network found, or the radio reset before it could look."
        : "  Almost no signal: 2G is barely audible here - antenna, or weak coverage.");
    Serial.println("  The SIM800L's LED tells you which:");
    Serial.println("    blinks every ~1 s    searching - power is fine, it hears no 2G network");
    Serial.println("    blinks every ~3 s    registered - it was only slow; reset and retry");
    Serial.println("    stops or restarts    resetting under load - the supply, not the SIM");
    Serial.println("  To test the network without this hardware: put the SIM in a phone set to");
    Serial.println("  2G only. No service there means no 2G here, for any module.");
    return;
  }
  Serial.println(" done.");

  int16_t csq = modem.getSignalQuality();
  Serial.printf("SIM800 modem ready (registered, signal %d/31%s).\n", csq,
                csq < 10 ? " - marginal, SMS may fail" : "");

  if (GPRS_SELF_TEST)
  {
    runGprsSelfTest();
  }
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
    Serial.println("Attempting GSM fallback...");
    if (!modem.waitForNetwork(timeoutMs))
    {
      Serial.println("GSM network registration failed.");
      return false;
    }
  }

  // Registration alone is what SMS and voice calls need, and with
  // GSM_DATA_ENABLED off those are the only things GSM carries - so stop
  // here rather than spending ~20 s and prepaid data on a session no one
  // will use. See the flag's note in config.h for why it is off.
  if (!GSM_DATA_ENABLED)
  {
    Serial.println("GSM registered - SMS and voice available (GPRS data disabled in config.h).");
    return true;
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
    activeClient = wifiHttpClient();
    return;
  }

  if (connectWiFi(5000))
  {
    currentNetwork = NET_WIFI;
    activeClient = wifiHttpClient();
    return;
  }

  if (SIM800L_ENABLED && (modem.isGprsConnected() || connectGSM(20000)))
  {
    currentNetwork = NET_GSM;

    // NET_GSM means "the modem is registered", not "HTTP works". With
    // GSM_DATA_ENABLED off, activeClient stays null on purpose: backend.cpp
    // then fails fast and queues the report, rather than burning its 10 s
    // timeout writing plaintext into an HTTPS port. Reaching this branch is
    // still valuable - registration is exactly what sendSms() and callStart()
    // need, and on the crash path those are what actually get help moving.
    activeClient = GSM_DATA_ENABLED ? (Client *)&gsmClient : nullptr;
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
