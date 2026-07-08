#pragma once

#include <Arduino.h>

// Posts to the Laravel backend over whichever transport connectivity.h
// currently has active (WiFi or GSM) - the caller doesn't need to know which.

String severityFromImpact(float totalAccel);
bool reportCrash(float totalAccel, float totalGyro);
void retryPendingCrashReport();
bool reportHelmetStatus();
bool fetchEmergencyContact(String &riderName, String &contactName, String &contactPhone);
