#include "gps.h"
#include "config.h"

#include <Arduino.h>
#include <TinyGPSPlus.h>

static TinyGPSPlus gps;
static HardwareSerial SerialGPS(2);

// How long to listen for NMEA before deciding the module is not there. The
// NEO-6M streams continuously from power-on whether or not it has a fix, and
// by the time this runs it has already been powered through the provisioning
// window, so a live module answers almost immediately. Only a missing one
// pays the full wait.
static const unsigned long GPS_PROBE_MS = 1500;

void gpsInit() {
  SerialGPS.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // Said out loud because the alternative is silence, and silence here reads
  // the same whether the module is unwired or simply has no fix yet. Those
  // need completely different responses - one is a soldering job, the other
  // is a walk outside - so the boot log should not make you guess.
  unsigned long start = millis();
  int bytes = 0;

  while (millis() - start < GPS_PROBE_MS) {
    while (SerialGPS.available() > 0) {
      gps.encode(SerialGPS.read());
      bytes++;
    }
    if (bytes > 10) break; // streaming confirmed; no reason to keep waiting
    delay(10);
  }

  if (bytes == 0) {
    Serial.println("NEO-6M GPS NOT CONNECTED - no NMEA arriving on GPIO 16.");
    Serial.println("  1. VCC on 5V. It is a 5 V board and is completely silent");
    Serial.println("     on 3V3 - no NMEA, no LED, indistinguishable from dead.");
    Serial.println("  2. GND shared with the ESP32.");
    Serial.println("  3. Module TX -> GPIO 16 (its RX goes to 17, not the reverse).");
    Serial.println("  Location will be reported as the stub, location_verified false.");
    return;
  }

  Serial.printf("NEO-6M GPS connected - receiving NMEA (%d bytes).\n", bytes);

  if (hasGpsFix()) {
    Serial.printf("  FIX ACQUIRED: %.5f, %.5f", getLatitude(), getLongitude());
    int sats = getSatelliteCount();
    if (sats >= 0) Serial.printf(" on %d satellite(s)", sats);
    Serial.println(".");
  } else {
    int sats = getSatelliteCount();
    Serial.printf("  No fix yet (%d satellite(s) in view). This is normal indoors:\n",
                  sats < 0 ? 0 : sats);
    Serial.println("  the wiring is proven, the sky is not. A cold start outdoors");
    Serial.println("  takes 2-15 minutes. Until then the stub location is sent with");
    Serial.println("  location_verified false, which is honest rather than wrong.");
  }
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
