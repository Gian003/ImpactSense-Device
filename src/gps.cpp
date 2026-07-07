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

double getSpeedKph() {
  return gps.speed.isValid() ? gps.speed.kmph() : 0.0;
}

bool gpsHasFix() {
  return gps.location.isValid();
}
