#include "dev_console.h"
#include "config.h"
#include "button.h"
#include "crash_response.h"
#include "connectivity.h"
#include "contact_cache.h"
#include "audio.h"
#include "power_state.h"
#include "redact.h"

// ── state ─────────────────────────────────────────────────────────────────────

// True while a response this console started is running. Together with
// buttonSimActive() it is what tells handleCrash() that the trigger came from
// the bench.
static bool sInSimulation = false;

// Armed by "CRASH LIVE" / "SOS LIVE" once confirmed, and consumed by the first
// response that asks. One-shot on purpose: leaving it set would make the next
// absent-minded CRASH text a real person.
static bool sLiveArmed = false;
static unsigned long sLiveArmedAt = 0;
static const unsigned long LIVE_ARM_TIMEOUT_MS = 60000UL;

// What a bare CONFIRM is confirming, if anything.
enum PendingKind { PENDING_NONE, PENDING_CRASH_LIVE, PENDING_SOS_LIVE };
static PendingKind sPending = PENDING_NONE;
static unsigned long sPendingAt = 0;
static const unsigned long CONFIRM_WINDOW_MS = 20000UL;

// Defaults for the bare forms of the commands.
static const unsigned long DEFAULT_TAP_MS = 250; // the window polls at 50 Hz

// ── helpers ───────────────────────────────────────────────────────────────────

static unsigned long defaultSosHoldMs() {
  // Comfortably past the threshold, so the hold is not a photo finish with
  // whatever else the loop is doing that iteration.
  return MANUAL_SOS_HOLD_MS + 600;
}

// PLAY <n> - play a clip here, in loop(), with whatever the radio is doing.
//
// This exists because of a gap that made an audio fault impossible to pin
// down. The other PLAY lives in runProvisioningWindow(), which finishes
// before audioInit() and long before ensureConnectivity() - so every PLAY
// that has ever passed on this bench ran with WiFi and GSM completely down.
// Until now the scenario clips were the ONLY ones ever sent to the module
// with the radio live, which is precisely the condition AUDIO_PRECISE_TX
// exists to survive, and it could only be exercised by staging a whole
// crash. "PLAY works but the alert does not" was really "the only test we
// have does not test the thing that differs".
//
// The link is checked either side rather than once, because the failure
// worth separating out is a module that was answering a moment ago and is
// not now. That is the 5 V rail sagging under the radio, not the serial
// link - and the two get the same symptom with completely different fixes.
static void handlePlayCommand(String arg) {
  arg.trim(); // already upper-cased by the caller; the arguments are keywords

  if (!audioReady()) {
    Serial.println("[SIM] No DFPlayer - audioInit() did not find one at boot, so");
    Serial.println("[SIM] there is nothing here to play and nothing to diagnose.");
    return;
  }

  if (arg.length() == 0) {
    Serial.printf("[SIM] PLAY <%d-%d>, PLAY STOP, or PLAY INFO.\n",
                  TRACK_FIRST, TRACK_LAST);
    return;
  }

  if (arg == "STOP") {
    audioStop();
    Serial.println("[SIM] Stopped.");
    return;
  }

  if (arg == "INFO") {
    audioReport();
    return;
  }

  int track = arg.toInt(); // non-numeric gives 0, which the range check catches
  if (track < TRACK_FIRST || track > TRACK_LAST) {
    Serial.printf("[SIM] Track must be %d-%d.\n", TRACK_FIRST, TRACK_LAST);
    return;
  }

  Serial.printf("[SIM] Link check BEFORE playing %d:\n", track);
  const bool before = audioLinkAlive();

  const unsigned long t0 = millis();
  audioPlay(track);
  const unsigned long took = millis() - t0;

  Serial.printf("[SIM] audioPlay(%d) returned after %lu ms.\n", track, took);
  Serial.println("[SIM] Link check AFTER:");
  const bool after = audioLinkAlive();

  audioPollMessages();

  if (before && !after) {
    Serial.println("[SIM] *** It answered before the clip and does not after. ***");
    Serial.println("[SIM] The module lost power mid-command. That is the 5 V rail, not");
    Serial.println("[SIM] the UART: fit the 1000 uF across the DFPlayer's VCC and GND,");
    Serial.println("[SIM] at the module. On the enclosure build also check the MT3608");
    Serial.println("[SIM] reads 5.10 V; on the bench measure the ESP32 5V pin, which is");
    Serial.println("[SIM] USB VBUS behind a diode and sits nearer 4.7 V.");
  } else if (!before && !after) {
    Serial.println("[SIM] Silent on both checks. On a no-ACK clone that is normal and");
    Serial.println("[SIM] says nothing either way; otherwise replies are not getting");
    Serial.println("[SIM] back on GPIO 33 and only the outbound direction is working.");
  } else if (before && after) {
    Serial.println("[SIM] Answering on both sides, so the module survived the command.");
    Serial.println("[SIM] Heard nothing anyway? Then the command reached a healthy");
    Serial.println("[SIM] module and the clip is the suspect: the file, or the speaker");
    Serial.println("[SIM] on pins 6 and 8. During a crash, also the buzzer over it.");
  }
}

static void printHelp() {
  Serial.println();
  Serial.println("[SIM] Bench console. Type a line and press Enter.");
  Serial.println("[SIM]");
  Serial.println("[SIM]   BTN                tap the button - cancels an alert in progress");
  Serial.println("[SIM]   BTN DOWN           hold it down and leave it down");
  Serial.println("[SIM]   BTN UP             let go");
  Serial.printf( "[SIM]   BTN HOLD [ms]      hold for ms (default %lu - long enough for SOS)\n",
                 defaultSosHoldMs());
  Serial.println("[SIM]   BTN IN <ms> [hold] press after a delay - for a press that comes too late");
  Serial.println("[SIM]");
  Serial.println("[SIM]   CRASH              simulate a crash. NOTHING is sent to anyone");
  Serial.println("[SIM]   SOS                simulate the SOS hold. NOTHING is sent");
  Serial.println("[SIM]   CRASH LIVE         a real crash report - asks you to confirm first");
  Serial.println("[SIM]   SOS LIVE           a real SOS - asks you to confirm first");
  Serial.println("[SIM]");
  Serial.printf( "[SIM]   PLAY <%d-%d>        play a clip NOW, with the radio up\n",
                 TRACK_FIRST, TRACK_LAST);
  Serial.println("[SIM]   PLAY STOP          cut a clip short");
  Serial.println("[SIM]   PLAY INFO          what the module reports about itself");
  Serial.println("[SIM]");
  Serial.println("[SIM]   STATE              what the device currently thinks is going on");
  Serial.println("[SIM]   HELP               this list");
  Serial.println("[SIM]");
  Serial.println("[SIM] The simulated press goes in where the pin is read, so the cancel");
  Serial.println("[SIM] window and the SOS hold cannot tell it from a thumb.");
  Serial.println("[SIM] You can type BTN while an alarm is sounding - the window reads this");
  Serial.println("[SIM] console too, so a cancel can be timed by hand against the alarm.");
  Serial.println("[SIM] PLAY is the other half of that: the provisioning window's PLAY runs");
  Serial.println("[SIM] before the radio comes up, so it has never tested a clip under the");
  Serial.println("[SIM] conditions a real alert plays one. This PLAY does.");
  Serial.println();
}

static const char *networkName() {
  switch (currentNetwork) {
    case NET_WIFI: return "WiFi";
    case NET_GSM:  return "GSM";
    default:       return "none";
  }
}

static void printState() {
  Serial.println();
  Serial.println("[SIM] ---- device state ----");
  Serial.printf("[SIM] button    : pin says %s, simulation says %s, effective %s\n",
                buttonPhysicallyDown() ? "DOWN" : "up",
                buttonSimActive() ? "DOWN" : "up",
                buttonIsDown() ? "DOWN" : "up");
  Serial.printf("[SIM] network   : %s, HTTP %s\n", networkName(),
                activeClient != nullptr ? "available" : "NOT available");
  Serial.printf("[SIM] audio     : %s\n", audioReady() ? "module ready" : "disabled");
  Serial.printf("[SIM] power     : %s\n", isIdle() ? "idle" : "active");

  // Printed because it is who a LIVE run would wake up. A number that is
  // blank here means the cache has not been filled yet, not that nobody
  // would be contacted - the backend still has its own copy.
  // Masked, like every other number this firmware prints. Enough to
  // recognise, not enough to dial from a log somebody else is reading.
  Serial.printf("[SIM] would text: %s (emergency contact, cached)\n",
                maskPhone(getCachedContactPhone()).c_str());
  Serial.printf("[SIM] would call: %s (hotline, only if the backend is unreachable)\n",
                maskPhone(EMERGENCY_HOTLINE_NUMBER).c_str());
  Serial.printf("[SIM] live arm   : %s\n", sLiveArmed ? "ARMED - the next run is real" : "off");
  Serial.println();
}

static void warnAndAskConfirm(PendingKind kind) {
  // Masked here too. The question this warning has to answer is "is that
  // the person I think it is", and four digits plus the last two answers it
  // for a number you already know - which, if you are about to text it from
  // a workbench, you do.
  //
  // An empty cache must NOT read as "nobody gets contacted". The device
  // cannot text without a cached number, but the backend holds its own copy
  // and rings them through Twilio regardless - so a LIVE run reaches that
  // person either way, and saying otherwise here would be the one place a
  // wrong reassurance actually costs somebody something.
  String raw = getCachedContactPhone();
  String contact = raw.length()
      ? maskPhone(raw)
      : String("not cached here, but the backend has it and will still call");

  Serial.println();
  Serial.println("[SIM] ************************* STOP *************************");
  Serial.println("[SIM] A LIVE run reaches real people from a device on a desk.");
  Serial.printf( "[SIM]   - texts the emergency contact: %s\n", contact.c_str());
  Serial.println("[SIM]   - posts a real incident, which lights up the dashboard");
  Serial.println("[SIM]     and triggers the backend's own call and SMS");
  Serial.printf( "[SIM]   - may ring the hotline %s if the backend is unreachable\n",
                 maskPhone(EMERGENCY_HOTLINE_NUMBER).c_str());
  Serial.println("[SIM]");
  Serial.println("[SIM] Tell them first. An unexplained one of these is frightening.");
  Serial.printf( "[SIM] Type CONFIRM within %lu s to go ahead. Anything else abandons it.\n",
                 CONFIRM_WINDOW_MS / 1000);
  Serial.println("[SIM] ********************************************************");
  Serial.println();

  sPending = kind;
  sPendingAt = millis();
}

// ── running a simulated response ──────────────────────────────────────────────

static void runSimulatedCrash(bool live) {
  if (live) {
    sLiveArmed = true;
    sLiveArmedAt = millis();
  } else {
    Serial.println("[SIM] Simulated crash. Nothing will be sent - every outbound");
    Serial.println("[SIM] channel is suppressed and reported instead.");
    Serial.println("[SIM] Type BTN during the window to cancel it.");
  }

  // Values just past the thresholds, so the numbers in the log and in any
  // report look like something that could really have happened rather than
  // like a placeholder.
  sInSimulation = true;
  handleCrash(ACCEL_THRESHOLD + 5.0f, GYRO_THRESHOLD + 50.0f);
  sInSimulation = false;

  // A LIVE arm that was never consumed - the run was cancelled before it got
  // as far as sending anything - must not sit waiting for the next command.
  if (sLiveArmed) {
    sLiveArmed = false;
    Serial.println("[SIM] Cancelled before anything was sent. LIVE arm cleared.");
  }
}

static void runSimulatedSos(bool live) {
  if (live) {
    sLiveArmed = true;
    sLiveArmedAt = millis();
  } else {
    Serial.println("[SIM] Holding the button for the SOS threshold. Nothing will be sent.");
  }

  // Deliberately NOT a direct call into the SOS routine: this holds the
  // simulated button down and lets checkManualSos() in loop() notice, which
  // means the hold detection and its edge handling are part of what is being
  // tested rather than being stepped over.
  buttonSimTap(defaultSosHoldMs());
  Serial.printf("[SIM] Button held for %lu ms. SOS fires at %lu ms.\n",
                defaultSosHoldMs(), MANUAL_SOS_HOLD_MS);
}

// ── command handling ──────────────────────────────────────────────────────────

// Returns the integer after the first space in `rest`, or `fallback` when
// there is nothing parseable there. toInt() gives 0 on junk, which is a
// meaningful value here, so an explicit emptiness check comes first.
static unsigned long argMs(const String &rest, unsigned long fallback) {
  String s = rest;
  s.trim();
  if (s.length() == 0) return fallback;
  long v = s.toInt();
  if (v <= 0) return fallback;
  return (unsigned long) v;
}

static void handleButtonCommand(const String &arg) {
  if (arg.length() == 0) {
    buttonSimTap(DEFAULT_TAP_MS);
    Serial.printf("[SIM] button tapped (%lu ms).\n", DEFAULT_TAP_MS);
    return;
  }

  if (arg.equalsIgnoreCase("DOWN")) {
    buttonSimPress();
    Serial.println("[SIM] button DOWN, and staying down until BTN UP.");
    Serial.printf("[SIM] Held past %lu ms outside an alert, this fires a manual SOS.\n",
                  MANUAL_SOS_HOLD_MS);
    return;
  }

  if (arg.equalsIgnoreCase("UP")) {
    buttonSimRelease();
    Serial.println("[SIM] button UP.");
    return;
  }

  if (arg.startsWith("HOLD")) {
    unsigned long ms = argMs(arg.substring(4), defaultSosHoldMs());
    buttonSimTap(ms);
    Serial.printf("[SIM] button held for %lu ms.\n", ms);
    if (ms >= MANUAL_SOS_HOLD_MS) {
      Serial.println("[SIM] That is past the SOS threshold - expect a manual SOS.");
    }
    return;
  }

  if (arg.startsWith("IN")) {
    String rest = arg.substring(2);
    rest.trim();

    // "IN <delay> [hold]" - split on the first space by hand rather than
    // reaching for a tokeniser for two numbers.
    int sp = rest.indexOf(' ');
    unsigned long delayMs = argMs(sp < 0 ? rest : rest.substring(0, sp), 1000);
    unsigned long holdMs  = sp < 0 ? DEFAULT_TAP_MS : argMs(rest.substring(sp + 1), DEFAULT_TAP_MS);

    buttonSimSchedule(delayMs, holdMs);
    Serial.printf("[SIM] button will press in %lu ms and hold %lu ms.\n", delayMs, holdMs);
    Serial.println("[SIM] Useful for a press that lands just after the window closes -");
    Serial.printf("[SIM] the window is %lu ms long.\n", CRASH_CANCEL_WINDOW_MS);
    return;
  }

  Serial.println("[SIM] BTN takes nothing, DOWN, UP, HOLD [ms], or IN <ms> [hold]. HELP lists them.");
}

static void handleLine(const String &raw, bool duringAlert) {
  String line = raw;
  line.trim();
  if (line.length() == 0) return;

  // A pending LIVE confirmation takes the next line, whatever it is. Anything
  // other than CONFIRM abandons it - including another command, which is the
  // safe reading of "they typed something else".
  if (sPending != PENDING_NONE) {
    PendingKind kind = sPending;
    sPending = PENDING_NONE;

    if (millis() - sPendingAt > CONFIRM_WINDOW_MS) {
      Serial.println("[SIM] Too late - that confirmation expired. Nothing was sent.");
      return;
    }
    if (!line.equalsIgnoreCase("CONFIRM")) {
      Serial.println("[SIM] Abandoned. Nothing was sent.");
      return;
    }

    Serial.println("[SIM] Confirmed. This is a LIVE run.");
    if (kind == PENDING_CRASH_LIVE) runSimulatedCrash(true);
    else                            runSimulatedSos(true);
    return;
  }

  String upper = line;
  upper.toUpperCase();

  if (upper == "HELP" || upper == "?") {
    printHelp();
    return;
  }

  if (upper == "STATE") {
    printState();
    return;
  }

  if (upper == "BTN" || upper.startsWith("BTN ")) {
    // Matched against the upper-cased copy: the arguments are all keywords
    // and numbers, so case carries no information and "Btn Hold" typed by
    // hand should work the same as "BTN HOLD".
    String arg = upper.length() > 3 ? upper.substring(4) : String("");
    arg.trim();
    handleButtonCommand(arg);
    return;
  }

  // Everything below starts a response, which is exactly what must not happen
  // from inside one.
  if (duringAlert) {
    Serial.println("[SIM] Not during an alert. BTN, STATE and HELP work here;");
    Serial.println("[SIM] CRASH and SOS do not, because the device cannot run two");
    Serial.println("[SIM] responses at once and neither can a rider.");
    return;
  }

  if (upper == "PLAY" || upper.startsWith("PLAY ")) {
    handlePlayCommand(upper.length() > 4 ? upper.substring(5) : String(""));
    return;
  }

  if (upper == "CRASH") {
    runSimulatedCrash(false);
    return;
  }

  if (upper == "CRASH LIVE") {
    warnAndAskConfirm(PENDING_CRASH_LIVE);
    return;
  }

  if (upper == "SOS") {
    runSimulatedSos(false);
    return;
  }

  if (upper == "SOS LIVE") {
    warnAndAskConfirm(PENDING_SOS_LIVE);
    return;
  }

  Serial.printf("[SIM] Unknown: '%s'. Type HELP.\n", line.c_str());
}

// ── the reader ────────────────────────────────────────────────────────────────

static void poll(bool duringAlert) {
  if (!DEV_CONSOLE_ENABLED) return;

  // A LIVE arm nobody used must not be waiting there next time.
  if (sLiveArmed && millis() - sLiveArmedAt > LIVE_ARM_TIMEOUT_MS) {
    sLiveArmed = false;
    Serial.println("[SIM] LIVE arm expired unused - back to suppressing outbound.");
  }

  // One buffer for both entry points, because there is only one input
  // stream. It is also safe across the nesting this function does - poll()
  // can end up calling itself, through handleLine -> handleCrash ->
  // devConsolePollDuringAlert - because nesting only ever begins straight
  // after a completed line, when len is already 0. A partially typed line
  // never starts a response, so it can never be interrupted by one.
  //
  // Anything that makes handleLine() reachable with characters still buffered
  // would break that, so keep the reset-before-dispatch below as it is.
  static char buf[64];
  static size_t len = 0;

  while (Serial.available()) {
    char c = (char) Serial.read();

    if (c == '\n' || c == '\r') {
      if (len == 0) continue;        // bare newline, or the LF of a CRLF pair
      buf[len] = '\0';
      len = 0;
      handleLine(String(buf), duringAlert);
      continue;
    }

    if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    } else {
      // Overlong line: drop it rather than acting on a truncated command, and
      // say so, because a silently ignored command looks like a dead console.
      len = 0;
      Serial.println("[SIM] Line too long - ignored.");
    }
  }
}

void devConsolePoll() {
  poll(false);
}

void devConsolePollDuringAlert() {
  poll(true);
}

bool devSimOutboundSuppressed() {
  // In deployment this console does not exist, so nothing is ever suppressed
  // and a real crash always reports. That ordering matters: this check comes
  // before everything else so no bench flag can survive into the field.
  if (!DEV_CONSOLE_ENABLED) return false;

  bool simulated = sInSimulation || buttonSimActive();

  // A real impact, or a real thumb on the switch, is never suppressed - even
  // while the bench console is sitting there enabled.
  if (!simulated) return false;

  if (sLiveArmed) {
    sLiveArmed = false; // one-shot, consumed by this response
    Serial.println("[SIM] LIVE run: outbound channels are NOT suppressed.");
    return false;
  }

  return true;
}

void devSimSkipped(const char *channel) {
  Serial.printf("[SIM] SUPPRESSED: %s - nobody was contacted.\n", channel);
}

void devConsoleBanner() {
  if (!DEV_CONSOLE_ENABLED) return;
  Serial.println("Bench console is on: type HELP for the button and crash simulations.");
  Serial.println("  CRASH sends nothing. BTN cancels. Both work while the alarm sounds.");
}
