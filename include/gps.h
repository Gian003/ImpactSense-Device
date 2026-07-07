#pragma once

// NEO-6M GPS reading via TinyGPS++. Falls back to stub coordinates
// (see config.h) until a real satellite fix is acquired.

void gpsInit();
void pollGps();
double getLatitude();
double getLongitude();
