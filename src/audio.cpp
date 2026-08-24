#include "audio.h"
#include "config.h"

#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>

// All three ESP32 hardware UARTs are already spoken for:
//   UART0 = USB/Serial debug
//   UART1 = SIM800L (GSM_RX_PIN / GSM_TX_PIN)
//   UART2 = NEO-6M GPS (GPS_RX_PIN / GPS_TX_PIN)
// DFPlayer gets a SoftwareSerial instance on GPIO 32/33.
static SoftwareSerial dfpSerial(DFP_RX_PIN, DFP_TX_PIN);
static DFRobotDFPlayerMini dfPlayer;
static bool dfpReady = false;

void audioInit() {
  dfpSerial.begin(9600);
  delay(200); // DFPlayer needs a moment after power-on before it acks begin()

  if (dfPlayer.begin(dfpSerial)) {
    dfPlayer.volume(DFP_VOLUME);
    dfpReady = true;
    Serial.println("DFPlayer Mini ready.");
  } else {
    Serial.println("DFPlayer Mini not found - audio alerts disabled. Check SD card and wiring (RX=33, TX=32).");
  }
}

void audioPlay(int track) {
  if (!dfpReady) return;
  dfPlayer.play(track);
}
