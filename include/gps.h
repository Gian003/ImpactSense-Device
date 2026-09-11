#pragma once

// NEO-6M GPS reading via TinyGPS++. Falls back to stub coordinates
// (see config.h) until a real satellite fix is acquired.

void gpsInit();
void pollGps();
double getLatitude();
double getLongitude();

// True only on a real satellite fix. getLatitude()/getLongitude() always
// return something usable, so this is the only way for a caller to tell a
// genuine position from the config.h stub - which matters for anything that
// gets stored and later aggregated as an observation.
bool hasGpsFix();

// Ground speed in km/h, or -1 when there is no valid speed reading, so a
// caller can distinguish "stationary" from "unknown".
int getSpeedKph();
