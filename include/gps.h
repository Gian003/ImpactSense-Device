#pragma once

// NEO-6M GPS reading via TinyGPS++. Falls back to stub coordinates
// (see config.h) until a real satellite fix is acquired.

void gpsInit();
void pollGps();
double getLatitude();
double getLongitude();
// Ground speed in km/h. Returns 0 if the fix doesn't include a valid speed
// reading yet (e.g. no satellite lock) - callers should treat 0 as "unknown",
// not "stationary", when deciding whether to report it.
double getSpeedKph();
// True once the NEO-6M has a real satellite lock, i.e. getLatitude()/
// getLongitude() are reporting the actual position rather than the
// STUB_LATITUDE/STUB_LONGITUDE fallback.
bool gpsHasFix();
