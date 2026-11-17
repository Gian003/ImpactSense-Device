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

// The library's begin() returns true ONLY if the module reports a card online
// within 2 s of a reset (DFRobotDFPlayerMini.cpp, the readType() check). So a
// false lumps together three faults that need completely different fixes:
//
//   nothing comes back at all  -> power, ground, or the two UART wires
//   frames come back, no card  -> the microSD, and the wiring is FINE
//   frames come back, no ACK   -> an MP3-TF-16P clone whose firmware never
//                                 sends the card-online frame, but which plays
//
// Telling someone to "check SD card and wiring" covers all three and helps
// with none, so on failure we re-ask the module directly and count raw bytes.

// A DFPlayer command frame: 7E FF 06 CMD FB PARAM_HI PARAM_LO CHK_HI CHK_LO EF
// The checksum is the two's complement of bytes 1..6. Built here rather than
// hardcoded so it stays correct if the command ever changes. Written straight
// to the port so the library cannot consume the reply before we count it.
static uint8_t sLastSent[10];

static void sendRawCommand(uint8_t cmd) {
  uint8_t frame[10] = {0x7E, 0xFF, 0x06, cmd, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEF};
  uint16_t sum = 0;
  for (int i = 1; i <= 6; i++) {
    sum += frame[i];
  }
  uint16_t chk = (uint16_t) (0 - sum);
  frame[7] = (uint8_t) (chk >> 8);
  frame[8] = (uint8_t) (chk & 0xFF);

  for (int i = 0; i < 10; i++) {
    sLastSent[i] = frame[i];
  }
  dfpSerial.write(frame, sizeof(frame));
}

// True when what came back is byte-for-byte what we just sent. That is not a
// reply: it means GPIO 33 is hearing GPIO 32 directly, so the module's TX has
// never reached us and nothing it ever "said" was real.
static bool isOwnEcho(const uint8_t *buf, int len) {
  if (len < 10) return false;
  for (int i = 0; i < 10; i++) {
    if (buf[i] != sLastSent[i]) return false;
  }
  return true;
}

// Run once the module is talking, on either init path.
//
// outputDevice() matters more than it looks: a number of MP3-TF-16P clones
// come up addressing internal flash or USB rather than the microSD, and then
// answer every serial command politely while playing nothing at all. That is
// indistinguishable from a dead speaker from the outside.
static void configureModule() {
  dfPlayer.outputDevice(DFPLAYER_DEVICE_SD);
  delay(200);
  dfPlayer.volume(DFP_VOLUME);
  delay(50);
}

// The reply to a reset is 7E FF 06 3F 00 <hi> <lo> <chk> <chk> EF, where the
// parameter is a bitmask of the storage the module found: bit 0 USB, bit 1 SD.
//
// This is worth decoding by hand because of how the library treats it. In
// parseStack(), case 0x3F only calls handleMessage() when a bit is set — so a
// module reporting NO storage falls through every branch, nothing is recorded,
// and begin() returns false with no explanation. "No card" and "no reply" end
// up looking identical from the outside. Reading the bytes separates them.
static void decodeFrames(const uint8_t *buf, int len) {
  for (int i = 0; i + 9 < len; i++) {
    if (buf[i] != 0x7E || buf[i + 9] != 0xEF) continue;

    uint8_t  cmd   = buf[i + 3];
    uint16_t param = ((uint16_t) buf[i + 5] << 8) | buf[i + 6];

    if (cmd != 0x3F) {
      Serial.printf("[Audio] frame 0x%02X, parameter %u\n", cmd, param);
      continue;
    }

    Serial.printf("[Audio] init frame 0x3F, storage bitmask %u - ", param);
    if (param & 0x02) {
      Serial.println("SD CARD PRESENT.");
      Serial.println("[Audio] The module can see the card, so the card is not the fault.");
    } else if (param & 0x01) {
      Serial.println("USB storage only, no SD.");
    } else {
      Serial.println("NO STORAGE AT ALL.");
      Serial.println("[Audio] The module is saying it cannot see the card. That is the card");
      Serial.println("[Audio] or the slot - not the rail, not the speaker, not the UART.");
      Serial.println("[Audio] Reseat it, and confirm FAT32 in a card reader, not over MTP.");
    }
  }
}

static int countReplyBytes(unsigned long windowMs) {
  unsigned long start = millis();
  uint8_t buf[32];
  int n = 0;

  while (millis() - start < windowMs) {
    while (dfpSerial.available()) {
      uint8_t b = dfpSerial.read();
      if (n < (int) sizeof(buf)) {
        buf[n] = b;
      }
      n++;
    }
    delay(1);
  }

  if (n > 0) {
    int shown = (n < (int) sizeof(buf)) ? n : (int) sizeof(buf);
    Serial.print("[Audio] raw reply:");
    for (int i = 0; i < shown; i++) {
      Serial.printf(" %02X", buf[i]);
    }
    Serial.println();

    if (isOwnEcho(buf, shown)) {
      Serial.println("[Audio] *** THAT IS OUR OWN COMMAND, ECHOED BACK - NOT A REPLY. ***");
      Serial.println("[Audio] GPIO 33 is hearing GPIO 32 directly, so the module's TX has");
      Serial.println("[Audio] never reached us. Nothing it appeared to 'say' was real.");
      Serial.println("[Audio] Almost always: both jumpers landed on the SAME header pin,");
      Serial.println("[Audio] or two adjacent pins are bridged. RX and TX are pins 2 and 3,");
      Serial.println("[Audio] right next to each other on an unlabelled header.");
      Serial.println("[Audio] Check: meter continuity GPIO 32 to GPIO 33 - it must NOT beep.");
      Serial.println("[Audio] Then GPIO 32 -> module pin 2 (RX), GPIO 33 -> module pin 3 (TX).");
      return -1; // an echo is not evidence of a module, so do not count it
    }

    decodeFrames(buf, shown);
  }

  return n;
}

void audioInit() {
  dfpSerial.begin(9600);
  delay(200); // DFPlayer needs a moment after power-on before it acks begin()

  if (dfPlayer.begin(dfpSerial)) {
    dfpReady = true;
    configureModule();
    Serial.println("DFPlayer Mini ready.");
    return;
  }

  while (dfpSerial.available()) {
    dfpSerial.read(); // drain whatever the failed begin() left behind
  }
  sendRawCommand(0x0C); // reset
  int bytes = countReplyBytes(2000);

  if (bytes < 0) {
    // countReplyBytes() returns -1 when the bytes were our own echo. The link
    // is miswired, so enabling playback would be claiming something untrue.
    Serial.println("DFPlayer Mini not found - the serial pair is MISWIRED (see above).");
    Serial.println("  Audio alerts are disabled until GPIO 32 and 33 are separated.");
    return;
  }

  if (bytes == 0) {
    Serial.println("DFPlayer Mini not found - NO REPLY AT ALL (0 bytes back).");
    Serial.println("  The module is not talking. Check, in this order:");
    Serial.println("   1. VCC on 3V3 instead of 5V - it browns out before it can answer.");
    Serial.println("   2. Module GND not shared with the ESP32's ground.");
    Serial.println("   3. GPIO 32 -> module RX (pin 2) and GPIO 33 <- module TX (pin 3).");
    Serial.println("   4. Pins miscounted on an unlabelled header - see figure_dfplayer_pinout.png.");
    Serial.println("  Audio alerts are disabled.");
    return;
  }

  Serial.printf("DFPlayer Mini: %d bytes came back, but no 'card online' frame.\n", bytes);
  Serial.println("  Power, ground and BOTH UART wires are good - do not rewire.");
  Serial.println("  Suspect, in this order:");
  Serial.println("   1. No microSD card seated, or the card is not FAT32.");
  Serial.println("   2. Clips not in an /mp3/ folder, named 0001.mp3 ... 0008.mp3.");
  Serial.println("   3. An MP3-TF-16P clone that never sends the card-online frame.");
  Serial.println("  Continuing in no-ACK mode - playback may well work. Try PLAY 1.");

  // Cause 3 is common on clones and costs us the crash alarm if we give up, so
  // drive the module without waiting for acknowledgements. begin() with
  // isACK=false returns true unconditionally, which is why the honest
  // diagnosis above is printed first and not replaced by a "ready" line.
  dfPlayer.begin(dfpSerial, false, false);
  dfpReady = true;
  configureModule();
}

void audioSetVolume(int level) {
  if (!dfpReady) return;
  if (level < 0)  level = 0;
  if (level > 30) level = 30;
  dfPlayer.volume(level);
  delay(50);
}

void audioPlay(int track) {
  if (!dfpReady) return;
  // 0x12 - play /mp3/000N.mp3, which is the layout BRINGUP_GUIDE.md and
  // ASSEMBLY_GUIDE.md both specify. This used to call play(), which is 0x03:
  // an index across every file on the card in FAT order. With the clips in
  // /mp3/ those two disagree, and a index that does not resolve makes the
  // module answer FileMismatch and play nothing - audible as simple silence.
  dfPlayer.playMp3Folder(track);
}

void audioPlayIndex(int track) {
  if (!dfpReady) return;
  dfPlayer.play(track);
}

void audioPollMessages() {
  if (!dfpReady) return;

  unsigned long start = millis();
  while (millis() - start < 500) {
    if (dfPlayer.available()) {
      uint8_t type = dfPlayer.readType();
      int value = dfPlayer.read();
      if (type != DFPlayerError) {
        Serial.printf("[Audio] module says: type %d, value %d\n", type, value);
      } else if (value == FileMismatch) {
        Serial.println("[Audio] ERROR FileMismatch - no such track. The clip is not");
        Serial.println("[Audio] where the firmware looked: /mp3/000N.mp3. Try PLAY ROOT <n>.");
      } else if (value == FileIndexOut) {
        Serial.println("[Audio] ERROR FileIndexOut - track number past the last file.");
      } else if (value == CheckSumNotMatch || value == SerialWrongStack) {
        Serial.println("[Audio] ERROR on the serial link - frames are arriving corrupted.");
        Serial.println("[Audio] Usually a long or noisy wire on GPIO 32/33, or a weak ground.");
      } else {
        Serial.printf("[Audio] ERROR %d from the module.\n", value);
      }
    }
    delay(5);
  }
}

void audioReport() {
  if (!dfpReady) {
    Serial.println("[Audio] Module not enabled - nothing to query.");
    return;
  }

  int files = dfPlayer.readFileCounts();
  int state = dfPlayer.readState();
  int vol   = dfPlayer.readVolume();

  Serial.printf("[Audio] files visible: %d   state: %d   volume: %d (configured %d)\n",
                files, state, vol, DFP_VOLUME);
  Serial.println("[Audio] (-1 on any of these means the query timed out, not zero.)");

  if (vol == 0) {
    Serial.println("[Audio] VOLUME READS 0. On its own that is enough for total silence,");
    Serial.println("[Audio] whatever the card and speaker are doing. Try:  PLAY VOL 25");
  }

  // -1 and 0 mean completely different things and must not be run together.
  // -1 is "the query timed out", which says nothing about the card at all; an
  // earlier version of this called that a card fault and was simply wrong.
  if (files < 0) {
    Serial.println("[Audio] The file-count query TIMED OUT. That is NOT evidence about");
    Serial.println("[Audio] the card - it means replies are not getting back reliably.");
    Serial.println("[Audio] Check the card by eye and in a PC, not from this reading.");
  } else if (files == 0) {
    Serial.println("[Audio] The module answered and reports ZERO files, so this is the");
    Serial.println("[Audio] CARD. Reseat it, confirm FAT32 (not exFAT - that means a card");
    Serial.println("[Audio] over 32 GB), and put 0001.mp3 ... 0008.mp3 in an /mp3/ folder.");
  } else {
    Serial.println("[Audio] The card is readable, so the decoder has something to play.");
    Serial.println("[Audio] Silence now points AFTER the decoder:");
    Serial.println("[Audio]   - speaker on pins 6 and 8 - NOT 4/5 (DAC, line level,");
    Serial.println("[Audio]     far too quiet to hear) and NOT 7 (GND, between them)");
    Serial.println("[Audio]   - meter the speaker itself: about 8 ohms across its leads.");
    Serial.println("[Audio]     Open circuit means a joint failed, not the module.");
  }

  audioPollMessages();
}

void audioStop() {
  if (!dfpReady) return;
  dfPlayer.stop();
}

bool audioReady() {
  return dfpReady;
}
