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
bool callEmergencyHotline(const String &number);

// Resolves API_MDNS_NAME (config.h) to an IP via mDNS, caching the result and
// re-resolving periodically so a backend IP change doesn't need a reflash.
// Returns 0.0.0.0 if no address has ever been resolved yet. WiFi-only - see
// config.h for why this doesn't apply to the GSM fallback.
IPAddress resolveApiHost();
