#pragma once

#include <Arduino.h>

// Phone numbers as they should appear in anything that gets written down.
//
// The serial monitor is not a private channel. platformio.ini sets
// `monitor_filters = log2file`, so every boot log is kept as a file in the
// project root - and those files get pasted into chat threads, attached to bug
// reports, and copied into a capstone appendix. The emergency contact on this
// device is somebody's mother, and her number was appearing in all of it.
//
// What survives masking is enough to recognise a number you already know -
// which is what the log is actually for ("did it load the right contact?") -
// and not enough for a reader to dial it.
//
//   09633337350   ->  0963*****50
//   +639456725632 ->  +639*******32
//
// Length is preserved on purpose: a number the wrong length is a real fault
// worth seeing (a truncated field, a missing country code), and collapsing
// everything to a fixed mask would hide it.
String maskPhone(const String &number);
