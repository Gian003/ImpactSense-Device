#pragma once

#include <Arduino.h>

// Posts to the Laravel backend over whichever transport connectivity.h
// currently has active (WiFi or GSM) - the caller doesn't need to know which.

String severityFromImpact(float totalAccel);
bool reportCrash(float totalAccel, float totalGyro);
void retryPendingCrashReport();
bool reportDeviceStatus();

// Collects device_code, model and signing secret from the backend, identifying
// this board by its MAC address. Call once, after connectivity is up and
// before anything that talks to the backend as a provisioned unit.
//
// Does nothing and returns false unless deviceNeedsEnrolment() — a unit that
// already has an identity is left alone.
//
// The server answers one of three ways:
//   403  enrolment is closed. Gives up immediately; an operator has to open
//        the window on the TOC device page.
//   202  seen, but not registered yet. Keeps asking for up to
//        ENROLMENT_WAIT_MS so the operator has time to finish the form.
//   200  credentials. Saves them and returns true.
//
// Failure is never fatal: the unit boots unprovisioned, exactly as it does
// today, and SET / SECRET over serial still work.
bool attemptDeviceEnrolment();
bool fetchEmergencyContact(String &riderName, String &contactName, String &contactPhone);
