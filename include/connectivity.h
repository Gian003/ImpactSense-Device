#pragma once

#include <Arduino.h>
#include <Client.h>

// Prefers WiFi; falls back to a SIM800L GSM modem only when WiFi is
// unavailable. activeClient points at whichever transport can currently carry
// HTTP, so backend.cpp can send requests without caring which one that is.
//
// Two states are easy to confuse here:
//
//   currentNetwork tells you what the device can reach anyone by at all.
//   activeClient tells you whether HTTP specifically will work.
//
// They are not the same. NET_GSM with a null activeClient is the normal,
// expected state whenever WiFi is down: the modem is registered, so SMS and
// the voice call to the TOC hotline both work, but HTTP does not, because the
// backend is HTTPS-only and the SIM800L cannot do that handshake
// (GSM_DATA_ENABLED in config.h has the full reasoning). Callers that need to
// get a message out should not look at activeClient.

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
