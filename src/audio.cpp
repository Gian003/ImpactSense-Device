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

// Whether the module acknowledges commands. False on the clones that play
// perfectly well but never send a frame back - worth remembering, because on
// those every query times out and a timeout must not be reported as a fault.
static bool dfpAckMode = true;

// The volume the module is supposed to be at. Not necessarily DFP_VOLUME:
// the rider sets this from the app (see applyRemoteVolume in backend.cpp), so
// a resync after a module reset has to restore THEIR number, not the
// compiled-in default they overrode.
static int sVolume = DFP_VOLUME;

// When anything last started playing, and when an EMERGENCY clip last
// started. Assurance cues consult both: they must not talk over the crash
// alarm (long quiet window), and must not cut off the startup announcement
// either (short one). A clip interrupted mid-word is worse than no clip.
static unsigned long sLastPlayAt = 0;
static bool sHasPlayed = false;
static unsigned long sLastEmergencyPlayAt = 0;
static bool sHasPlayedEmergency = false;

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
  // Set first, because everything below it is subject to it. The library's
  // own default is 500 ms - see AUDIO_QUERY_TIMEOUT_MS for why that is paid
  // twice per verified play and why it is the bulk of the delay.
  dfPlayer.setTimeOut(AUDIO_QUERY_TIMEOUT_MS);
  dfPlayer.outputDevice(DFPLAYER_DEVICE_SD);
  delay(200);
  dfPlayer.volume(sVolume);
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

  // Interrupts off while a frame is clocked out. Without this the module
  // stops accepting commands as soon as the radio is busy, because the
  // Arduino loop gets preempted mid-byte and the bit timing comes out
  // wrong. AUDIO_PRECISE_TX in config.h has the full reasoning; it is the
  // difference between a helmet that speaks on the bench and one that
  // speaks on the road.
  if (AUDIO_PRECISE_TX) {
    dfpSerial.enableIntTx(false);
  }

  delay(200); // DFPlayer needs a moment after power-on before it acks begin()

  if (dfPlayer.begin(dfpSerial)) {
    dfpReady = true;
    dfpAckMode = true;
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
  dfpAckMode = false;
  configureModule();
}

void audioResync() {
  if (!dfpReady) return;
  configureModule();
  Serial.printf("[Audio] resynced: SD output, volume %d.\n", sVolume);
}

bool audioLinkAlive() {
  if (!dfpReady) return false;

  // readVolume() is the cheapest query with a definite right answer: we set
  // that number ourselves, so a different one is evidence rather than noise.
  int vol = dfPlayer.readVolume();

  if (vol < 0) {
    if (dfpAckMode) {
      Serial.println("[Audio] No reply to a volume query. Either the module reset,");
      Serial.println("[Audio] or replies are no longer getting back on GPIO 33.");
    } else {
      Serial.println("[Audio] No reply to a volume query - expected on this module:");
      Serial.println("[Audio] it came up in no-ACK mode and never answers queries.");
      Serial.println("[Audio] That is not a fault, and it says nothing about playback.");
    }
    return false;
  }

  if (vol != sVolume) {
    // A module that answers but disagrees has been power-cycled under us -
    // almost always its supply sagging while the radio transmits, which is
    // what bulk capacitance across its VCC and GND is for. Deliberately not
    // named as "the boost output": on the bench the module is fed from the
    // ESP32's 5V/VIN pin instead, and the cap belongs at the module either
    // way - naming one supply sends the reader to a part that is not there.
    Serial.printf("[Audio] Module reports volume %d, we set %d - it has RESET.\n",
                  vol, sVolume);
    return false;
  }

  return true;
}

void audioSetVolume(int level) {
  if (level < 0)  level = 0;
  if (level > 30) level = 30;

  // Remembered even when the module is absent, so a volume the rider chose
  // while the DFPlayer was unplugged is still the volume it comes up at.
  sVolume = level;
  if (!dfpReady) return;

  dfPlayer.volume(level);
  delay(50);
}

// Whether a clip is one of the emergency ones - the 2-8 band, played to a
// rider who may be lying on a road. Named because three separate decisions
// now turn on it and they must not drift apart.
static bool isEmergencyTrack(int track) {
  return track >= TRACK_CRASH_DETECTED && track <= TRACK_CALL_TOC;
}

// Says how long a play took, but only when that is news.
static void reportIfSlow(int track, unsigned long began) {
  unsigned long took = millis() - began;
  if (took < AUDIO_SLOW_PLAY_WARN_MS) return;
  Serial.printf("[Audio] Track %d: the command took %lu ms to get out.\n",
                track, took);
  Serial.println("[Audio] That is the serial link waiting, not the clip. If the clip");
  Serial.println("[Audio] itself seems late or absent, this number is where to look.");
}

void audioPlay(int track) {
  if (!dfpReady) return;

  const unsigned long began = millis();

  // 0x12 - play /mp3/000N.mp3, which is the layout BRINGUP_GUIDE.md and
  // ASSEMBLY_GUIDE.md both specify. This used to call play(), which is 0x03:
  // an index across every file on the card in FAT order. With the clips in
  // /mp3/ those two disagree, and a index that does not resolve makes the
  // module answer FileMismatch and play nothing - audible as simple silence.
  dfPlayer.playMp3Folder(track);

  sLastPlayAt = millis();
  sHasPlayed = true;

  // Tracks 1, 9 and 10 are reassurance; 2-8 are the ones a rider is lying
  // on a road listening to. The second kind earns a much longer silence.
  if (isEmergencyTrack(track)) {
    sLastEmergencyPlayAt = millis();
    sHasPlayedEmergency = true;
  }

  // What verification is worth paying for.
  //
  // dfpAckMode false does not mean "a module that is merely quiet": it means
  // begin() never got the card-online frame, which this firmware treats as a
  // clone that answers nothing - audioLinkAlive() says so in those words.
  // On one of those, readState() can only ever time out, so verifying costs
  // the full AUDIO_QUERY_TIMEOUT_MS twice and can never return an answer.
  // That was pure latency on exactly the boards most likely to need help.
  //
  // The assurance cues are skipped for the opposite reason: they are
  // optional by definition, and a cue is not worth half a second of a loop
  // that also has a crash to notice.
  const bool worthVerifying = AUDIO_VERIFY_PLAY && dfpAckMode && isEmergencyTrack(track);

  if (!worthVerifying) {
    reportIfSlow(track, began);
    return;
  }

  // Did it actually start? A command frame that arrived corrupted is
  // discarded without complaint, so silence is the only symptom - and for
  // track 2 that silence is the whole warning a conscious rider gets.
  delay(AUDIO_PLAY_VERIFY_MS);
  int state = dfPlayer.readState(); // 0 stopped, 1 playing, 2 paused, -1 no reply

  if (state == 1 || state == 2) {
    reportIfSlow(track, began);
    return; // playing: nothing to do
  }

  if (state < 0) {
    // No answer. That is not evidence it failed - see AUDIO_VERIFY_PLAY.
    reportIfSlow(track, began);
    return;
  }

  Serial.printf("[Audio] Track %d did not start (state %d). Resending once.\n",
                track, state);
  audioResync();
  dfPlayer.playMp3Folder(track);
  audioPollMessages(); // surfaces FileMismatch if the clip is simply absent
}

void audioWaitForClip(unsigned long maxMs) {
  if (!dfpReady) return;
  if (!sHasPlayed) return; // nothing has ever played, so nothing to wait for

  // Starts out true only on a module that answers queries. dfpAckMode false
  // means begin() never got the card-online frame, which this firmware reads
  // as a clone that answers nothing at all - see audioLinkAlive().
  bool canAsk = dfpAckMode;

  while (true) {
    const unsigned long elapsed = millis() - sLastPlayAt;

    // The ceiling is checked first and applies on every path, so no reply,
    // a stuck module and a very long clip all end the wait at the same place.
    if (elapsed >= maxMs) return;

    if (canAsk && elapsed >= AUDIO_CLIP_SETTLE_MS) {
      const int state = dfPlayer.readState(); // 0 stopped, 1 playing, 2 paused
      if (state == 0) return;                 // it says it has finished
      if (state < 0) canAsk = false;          // gone quiet - fall back below
    }

    // No usable answer available, so time it instead. Note this is reached
    // both by a clone that never answered and by a module that stopped
    // answering midway: neither earns an early return, because the clip is
    // probably still playing and cutting it off is the fault being fixed.
    if (!canAsk && elapsed >= AUDIO_CLIP_NOMINAL_MS) return;

    delay(100); // a query every 100 ms is plenty on a bit-banged link
  }
}

bool audioAssuranceCue(int track) {
  if (!AUDIO_ASSURANCE_ENABLED) return false;
  if (!dfpReady) return false;

  if (sHasPlayedEmergency &&
      millis() - sLastEmergencyPlayAt < AUDIO_ASSURANCE_QUIET_MS) {
    // An alert is in progress or has just finished. Good news can wait;
    // cutting off "press the button to cancel" to say "online" cannot.
    return false;
  }

  if (sHasPlayed && millis() - sLastPlayAt < AUDIO_CUE_MIN_GAP_MS) {
    // Something else is probably still being spoken - most often the
    // startup announcement, moments earlier in setup().
    return false;
  }

  audioPlay(track);
  return true;
}

void audioOnlineCueTick(bool online) {
  if (!AUDIO_ASSURANCE_ENABLED) return;

  // One announcement per spell of being online, not one per loop: the cue
  // belongs to the moment the helmet BECOMES reachable, which is the moment
  // the rider learns something they did not already know.
  static bool announced = false;
  static unsigned long lastCueAt = 0;
  static bool everCued = false;

  if (!online) {
    announced = false; // armed again, so a recovery mid-ride is announced
    return;
  }

  if (announced) return;

  if (everCued && millis() - lastCueAt < AUDIO_ONLINE_CUE_COOLDOWN_MS) {
    // Inside the cooldown. Counted as handled rather than left pending, so
    // a hotspot flapping at the edge of range does not queue the sentence
    // up to be blurted out the instant the cooldown expires.
    announced = true;
    return;
  }

  // Deliberately NOT latched on a refusal. audioAssuranceCue() declines
  // while an alert or the startup clip is still being spoken, and the right
  // answer there is to say it a moment later rather than to lose it. The
  // retry costs two comparisons per loop until it lands.
  if (!audioAssuranceCue(TRACK_ONLINE)) return;

  announced = true;
  lastCueAt = millis();
  everCued = true;
  Serial.println("[Audio] Online cue played - the rider knows reports can go out.");
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

  // readFileCounts() can take over a second on a card with many files, and
  // the running timeout is deliberately short for playback. Raised just for
  // these three queries, then put back - otherwise this command reports a
  // slow card as a dead link, which is the mistake it exists to prevent.
  dfPlayer.setTimeOut(AUDIO_REPORT_TIMEOUT_MS);
  int files = dfPlayer.readFileCounts();
  int state = dfPlayer.readState();
  int vol   = dfPlayer.readVolume();
  dfPlayer.setTimeOut(AUDIO_QUERY_TIMEOUT_MS);

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
