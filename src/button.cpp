#include "button.h"
#include "config.h"

// The button is wired to ground through the switch with an internal pull-up,
// so LOW means pressed. Named once here so no caller has to remember the
// polarity - getting it backwards reads as a button that is held down forever,
// which fires a manual SOS three seconds after boot.
static const int PRESSED_LEVEL = LOW;

static bool sSimDown = false;

// Absolute millis() deadlines, 0 meaning "not scheduled". Compared with a
// signed difference so they still behave correctly across the 49-day millis()
// wrap, rather than being skipped for the rest of the device's uptime.
static unsigned long sSimPressAt   = 0;
static unsigned long sSimReleaseAt = 0;
static unsigned long sSimHoldMs    = 0;

static bool due(unsigned long deadline) {
  return deadline != 0 && (long) (millis() - deadline) >= 0;
}

void buttonInit() {
  pinMode(CANCEL_BUTTON_PIN, INPUT_PULLUP);
}

bool buttonPhysicallyDown() {
  return digitalRead(CANCEL_BUTTON_PIN) == PRESSED_LEVEL;
}

bool buttonIsDown() {
  bool physical = buttonPhysicallyDown();

  // In deployment there is no simulated layer at all, and a real button is the
  // only thing that can cancel an alert or raise an SOS.
  if (!DEV_CONSOLE_ENABLED) return physical;

  // Timers are serviced here, not in loop(), because the two places that
  // matter most both block: handleCrash() sits in its own cancel window for
  // 30 seconds, and triggerManualSos() sits on an SMS and an HTTP post. A
  // press scheduled for "20 seconds into the window" has to be able to land
  // while nothing else in the firmware is running.
  if (due(sSimPressAt)) {
    sSimPressAt = 0;
    sSimDown = true;
    sSimReleaseAt = sSimHoldMs > 0 ? millis() + sSimHoldMs : 0;
    Serial.printf("[SIM] button DOWN (scheduled), %s.\n",
                  sSimHoldMs > 0 ? "releasing on time" : "held until released");
  }

  if (sSimDown && due(sSimReleaseAt)) {
    sSimDown = false;
    sSimReleaseAt = 0;
    Serial.println("[SIM] button UP (hold expired).");
  }

  return physical || sSimDown;
}

void buttonSimPress() {
  sSimDown = true;
  sSimPressAt = 0;
  sSimReleaseAt = 0; // held until explicitly released
  sSimHoldMs = 0;
}

void buttonSimRelease() {
  sSimDown = false;
  sSimPressAt = 0;
  sSimReleaseAt = 0;
  sSimHoldMs = 0;
}

void buttonSimTap(unsigned long holdMs) {
  if (holdMs == 0) holdMs = 1; // a zero-length tap would never be sampled
  sSimDown = true;
  sSimPressAt = 0;
  sSimHoldMs = holdMs;
  sSimReleaseAt = millis() + holdMs;

  // A deadline that lands exactly on 0 would read as "not scheduled" and the
  // button would stay down forever. One millisecond later is indistinguishable
  // to everything else and cannot be mistaken for the sentinel.
  if (sSimReleaseAt == 0) sSimReleaseAt = 1;
}

void buttonSimSchedule(unsigned long delayMs, unsigned long holdMs) {
  if (delayMs == 0) {
    buttonSimTap(holdMs);
    return;
  }
  sSimDown = false;
  sSimHoldMs = holdMs;
  sSimPressAt = millis() + delayMs;
  if (sSimPressAt == 0) sSimPressAt = 1;
  sSimReleaseAt = 0;
}

bool buttonSimActive() {
  if (!DEV_CONSOLE_ENABLED) return false;
  return sSimDown || sSimPressAt != 0;
}
