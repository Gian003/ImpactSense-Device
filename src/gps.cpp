#include "gps.h"
#include "config.h"

#include <Arduino.h>
#include <TinyGPSPlus.h>

static TinyGPSPlus gps;
static HardwareSerial SerialGPS(2);

void gpsInit() {
  SerialGPS.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
}

void pollGps() {
  while (SerialGPS.available() > 0) {
    gps.encode(SerialGPS.read());
  }
}

double getLatitude() {
  return gps.location.isValid() ? gps.location.lat() : STUB_LATITUDE;
}

double getLongitude() {
  return gps.location.isValid() ? gps.location.lng() : STUB_LONGITUDE;
}

bool hasGpsFix() {
  return gps.location.isValid();
}

int getSpeedKph() {
  if (!gps.speed.isValid()) {
    return -1;
  }
  return (int) lround(gps.speed.kmph());
}

int getSatelliteCount() {
  return gps.satellites.isValid() ? (int) gps.satellites.value() : -1;
}

bool gpsReceivingData() {
  // A handful of characters is enough to tell the difference. NMEA arrives
  // continuously, so a live module passes this within a second of boot and a
  // dead one never does.
  return gps.charsProcessed() > 10;
}
