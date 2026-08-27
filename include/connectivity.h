#pragma once

#include <Arduino.h>
#include <Client.h>

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

// Returns the backend hostname to use for all HTTP requests.
// Always returns API_PUBLIC_HOST (config.h) — the backend is on Cloudflare Tunnel.
const char* getApiHost();
