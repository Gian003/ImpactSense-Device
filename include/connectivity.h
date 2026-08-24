#pragma once

#include <Arduino.h>
#include <Client.h>
#include <IPAddress.h>

// Prefers WiFi; falls back to a SIM800L GSM/GPRS modem only when WiFi is
// unavailable. activeClient always points at whichever transport is currently
// usable (or nullptr if neither is), so backend.cpp can send HTTP requests
// without caring which network is actually carrying them.

enum NetworkMode { NET_NONE, NET_WIFI, NET_GSM };
extern NetworkMode currentNetwork;
extern Client* activeClient;

void modemInit();
bool connectWiFi(unsigned long timeoutMs);
bool connectGSM(unsigned long timeoutMs);
void ensureConnectivity();
bool sendSms(const String &number, const String &message);
// callStart() dials and returns immediately (non-blocking).
// Play DFPlayer audio between callStart() and callEnd() — it transmits through
// SIM800L MIC+ when DAC_R (DFPlayer pin 4) is wired to MIC+ via 100 kΩ.
bool callStart(const String &number);
void callEnd();

// Resolves API_MDNS_NAME (config.h) to an IP via mDNS, caching the result and
// re-resolving periodically so a backend IP change doesn't need a reflash.
// Returns 0.0.0.0 if no address has ever been resolved yet. WiFi-only - see
// config.h for why this doesn't apply to the GSM fallback.
IPAddress resolveApiHost();
