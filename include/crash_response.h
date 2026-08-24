#pragma once

// Crash detection response, manual SOS, and precaution alerts.
//
// handleCrash()    — plays audio, runs the cancellation window (button OR voice),
//                    then reports and calls if not cancelled.
// handlePrecautionAlert() — three short buzzer beeps for non-crash anomalies.
// checkManualSos() — call every loop iteration; fires SOS after MANUAL_SOS_HOLD_MS
//                    of continuous button hold outside an active crash window.

void crashResponseInit();
void handleCrash(float totalAccel, float totalGyro);
void handlePrecautionAlert();
void checkManualSos();
