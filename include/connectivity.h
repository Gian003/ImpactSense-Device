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
bool callEmergencyHotline(const String &number);
