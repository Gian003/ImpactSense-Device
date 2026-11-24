#pragma once

#include <Arduino.h>

// A serial console for the bench, so the button and the whole crash response
// can be exercised on a unit that is sitting on a desk.
//
// Why this exists: every interesting behaviour in this firmware is reached by
// pressing a button during a 30-second window that only opens after a 25 m/s^2
// impact. Testing it for real meant shaking the board hard enough to trigger
// the MPU6050 while holding a thumb on a switch and watching the monitor -
// awkward with the enclosure closed, and impossible to repeat the same way
// twice. Half the cases that matter cannot be produced by hand at all: a press
// at 29.5 seconds, or a press 200 ms too late.
//
// The simulated press goes in through buttonIsDown() (see button.h), so the
// cancel window and the SOS hold detector cannot tell it from a thumb. There
// is no separate "test mode" path through the response, which is the point:
// a test that exercises its own code proves nothing about the real one.
//
// Everything here is compiled past in DEPLOYMENT_MODE - see DEV_CONSOLE_ENABLED
// in config.h.

// Call once per loop(). Non-blocking: reads whatever characters have arrived
// and acts only on a completed line.
void devConsolePoll();

// The same reader, called from inside the crash cancel window so a press can
// be typed while the alarm is actually sounding. Only the button and the
// read-only commands are honoured there - CRASH and SOS are refused, because
// starting a second response from inside the first is not a thing the device
// can do and not a thing worth simulating.
void devConsolePollDuringAlert();

// Whether the response now starting was triggered from this console or by a
// simulated press, and so must not touch a real phone.
//
// THIS IS A SAFETY INTERLOCK, not a convenience. The emergency contact in the
// database is a real person, and OUTBOUND_SMS_ENABLED and OUTBOUND_CALLS_ENABLED
// are both true on the backend - so an un-cancelled simulated crash would text
// and ring somebody who is not expecting it, from a device on a workbench.
//
// Call this ONCE, at the top of a response, and keep the answer in a local:
// the simulated press it depends on may be released while the response is
// still running, and a suppression that changes its mind halfway through would
// send some of the messages and not others.
bool devSimOutboundSuppressed();

// Prints what was not sent, so a dry run reads like the real thing minus the
// consequences rather than looking as though a step silently failed.
void devSimSkipped(const char *channel);

// One line at the end of setup() saying the console is there. Without it the
// commands are invisible to anyone who has not read this header.
void devConsoleBanner();
