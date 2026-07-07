#pragma once

// Cancellation window: beeps the buzzer and polls the cancel button for
// CRASH_CANCEL_WINDOW_MS before actually reporting the crash to the backend.

void crashResponseInit();
void handleCrash(float totalAccel, float totalGyro);
void handlePrecautionAlert();
