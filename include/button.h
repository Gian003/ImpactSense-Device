#pragma once

#include <Arduino.h>

// The cancel/SOS button, with a development-only simulated press layered over
// the physical one.
//
// Everything that cares about the button asks buttonIsDown() instead of
// reading CANCEL_BUTTON_PIN itself. That indirection is the whole trick: a
// simulated press is then indistinguishable from a thumb as far as the crash
// window and the SOS hold detector are concerned, so what you test is the real
// behaviour rather than a parallel code path that only exists on the bench.
//
// The physical button always wins in the sense that it is OR-ed in - a real
// press works whatever the simulation is doing, and in DEPLOYMENT_MODE the
// simulated layer is compiled past entirely (DEV_CONSOLE_ENABLED is false).

void buttonInit();

// True while the button is down, physically OR in simulation. Also services
// the simulated press/release timers, which is why it must be the only reader:
// handleCrash() blocks inside its own 30 s loop and never returns to loop(),
// so a timed press can only start or expire from inside this call.
bool buttonIsDown();

// --- simulation, development only -----------------------------------------

// Hold the button down until something releases it.
void buttonSimPress();

// Let go now. Also cancels a scheduled press that has not fired yet.
void buttonSimRelease();

// Press now and let go after holdMs. A tap for testing the crash-cancel
// window; anything over MANUAL_SOS_HOLD_MS fires a manual SOS instead.
void buttonSimTap(unsigned long holdMs);

// Press in delayMs, then let go after holdMs. For the cases a human cannot
// type fast enough to reach - "pressed at 29 seconds into the window", or
// testing that an alert is NOT cancelled by a press that comes too late.
void buttonSimSchedule(unsigned long delayMs, unsigned long holdMs);

// True while a simulated press is down or scheduled. Used to decide that a
// crash response was triggered by the bench rather than by a rider, which is
// what keeps a simulated SOS from texting a real person.
bool buttonSimActive();

// For the STATE command: what the pin itself says, ignoring simulation.
bool buttonPhysicallyDown();
