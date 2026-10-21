#include "device_identity.h"
#include "config.h"

#include <Preferences.h>
#include <mbedtls/md.h>

static Preferences preferences;

// Same namespace contact_cache.cpp uses. The keys differ (device_code/model/
// firmware vs rider_name/ec_phone) so they coexist, and sharing it means a
// unit already provisioned by the older .ino firmware keeps its identity.
static const char *NVS_NAMESPACE = "impactsense";

// Deliberately not a real code. A brand-new unit that has never been
// provisioned should fail loudly against the backend ("Device not
// registered") rather than quietly impersonate whatever code happened to be
// hardcoded in the build.
static const char *UNPROVISIONED = "UNPROVISIONED";

static String deviceCode;
static String deviceModel;

// firmware_version is NOT stored in NVS and is NOT typed in. It is stamped
// into the binary at build time by inject_version.py, from `git describe`, so
// what a unit reports is always the commit it is running. Typing it meant a
// board could claim 2.0.1 while running anything at all, and nothing - here
// or on the server - could tell.
//
// The fallback matters: a build from a source zip with no .git directory, or
// on a machine without git, still has to compile.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif

// Kept out of printDeviceIdentity() and out of every log line: this is the
// one value on the device that must never be readable from the outside.
static String signingSecret;

// Requests are numbered so a captured one cannot be sent again. The high part
// comes from a boot counter in flash and the low part counts requests within
// this run, which keeps the number climbing across reboots and power loss
// while costing one flash write per boot rather than one per request.
//
// A million requests in a single run would overflow into the next boot's
// range. At one heartbeat a minute that is about two years of continuous
// uptime on one power cycle, so the device would have to outlive its battery
// by a wide margin before it mattered.
static const unsigned long REQUESTS_PER_BOOT = 1000000UL;
static unsigned long bootCounterBase = 0;
static unsigned long requestsThisBoot = 0;

// No firmware argument: that field comes from the build now, so there is
// nothing per-unit left to persist. The old "firmware" NVS key is left where
// it is rather than deleted - nothing reads it, and erasing keys on boot is a
// flash write nobody asked for.
static void saveDeviceIdentity(const String &code, const String &model) {
  preferences.begin(NVS_NAMESPACE, false); // read-write
  preferences.putString("device_code", code);
  preferences.putString("model", model);
  preferences.end();
}

// Wipes this unit back to factory-blank so it enrols itself again.
//
// The alternative is erasing the whole flash, which needs the board in
// download mode twice (erase, then re-upload) and takes the firmware with
// it. This is the same outcome over serial, with the board left running.
//
// boot_no is deliberately NOT cleared. It feeds the request counter, and
// re-enrolling a known MAC hands back the SAME signing secret - so a counter
// restarting at zero would briefly accept a request captured before the
// forget. Letting it keep climbing closes that, and costs nothing.
static void clearDeviceIdentity() {
  preferences.begin(NVS_NAMESPACE, false); // read-write
  preferences.remove("device_code");
  preferences.remove("model");
  preferences.remove("sign_secret");
  preferences.end();

  deviceCode    = UNPROVISIONED;
  deviceModel   = "";
  signingSecret = "";
}

static void saveSigningSecret(const String &secret) {
  preferences.begin(NVS_NAMESPACE, false);
  preferences.putString("sign_secret", secret);
  preferences.end();
}

static void printDeviceIdentity() {
  Serial.println("-- Current device identity --");
  Serial.printf("  device_code      : %s\n", deviceCode.c_str());
  Serial.printf("  model            : %s\n", deviceModel.length() ? deviceModel.c_str() : "(not set)");
  Serial.printf("  firmware_version : %s  (from the build, not editable)\n", FIRMWARE_VERSION);
  // Whether, never what. Printing the secret here would put it in every
  // screenshot and every serial log of a boot.
  Serial.printf("  signing secret   : %s\n",
                signingSecret.length() ? "stored (reports are signed)"
                                       : "NOT SET (reports are unsigned)");
}

void deviceIdentityInit() {
  preferences.begin(NVS_NAMESPACE, true); // read-only
  deviceCode      = preferences.getString("device_code", UNPROVISIONED);
  deviceModel     = preferences.getString("model", "");
  signingSecret   = preferences.getString("sign_secret", "");
  preferences.end();

  // One write per boot, before anything can send a request. Read-modify-write
  // in its own session so a brownout mid-boot cannot leave the counter behind
  // where it was - which would let an old captured request through.
  preferences.begin(NVS_NAMESPACE, false);
  unsigned long boots = preferences.getULong("boot_no", 0) + 1;
  preferences.putULong("boot_no", boots);
  preferences.end();

  bootCounterBase  = boots * REQUESTS_PER_BOOT;
  requestsThisBoot = 0;

  if (deviceCode == UNPROVISIONED) {
    // Without this, an unprovisioned unit looks like a network fault: every
    // backend call fails with "Device not registered" and nothing on the
    // serial log says why.
    Serial.println("WARNING: this unit has no device_code yet. Backend calls "
                   "will be rejected until it is provisioned - use SET below.");
  }
}

void runProvisioningWindow() {
  // In the field nobody is holding a serial cable, and this window is eight
  // seconds of no crash detection on every single boot - including the reboot
  // after a brownout, which is precisely when the rider is least able to
  // afford it. So a deployed unit waits only long enough to notice that
  // someone is actually there: plug in USB and hold a key through the reset
  // and the full window opens as normal.
  if (DEPLOYMENT_MODE) {
    unsigned long probeStart = millis();
    while (millis() - probeStart < PROVISION_PROBE_MS && !Serial.available()) {
      delay(10);
    }

    if (!Serial.available()) {
      Serial.println("[Provision] Deployment mode - window skipped (no serial input). "
                     "Hold a key during reset to provision this unit.");
      return;
    }
  }

  Serial.println();
  Serial.println("===============================================");
  Serial.println(" PROVISIONING MODE - waiting for a command...");
  Serial.println(" Type:  SET <device_code>,<model>");
  Serial.println("   or:  SHOW");
  Serial.println("   or:  SECRET <64 hex characters>");
  Serial.println("   or:  FORGET          (wipe identity, re-enrol from scratch)");
  Serial.println(" then press Enter. Full instructions are in");
  Serial.println(" include/device_identity.h. No input needed to");
  Serial.println(" skip - the device boots normally on its own.");
  Serial.printf (" Waiting %lus for a first keystroke, then %lus\n",
                 PROVISION_WINDOW_MS / 1000, PROVISION_IDLE_MS / 1000);
  Serial.println(" between them. Nothing typed? It just continues.");
  Serial.println("===============================================");
  printDeviceIdentity();

  String line;
  unsigned long start  = millis();
  unsigned long window = PROVISION_WINDOW_MS;

  // Only runs right after power-on/reset. Once this returns, typing on serial
  // no longer reconfigures anything, so a unit already in the field cannot be
  // silently reprovisioned without someone physically holding it, plugging in
  // USB, and resetting it on purpose.
  while (millis() - start < window) {
    while (Serial.available()) {
      char c = Serial.read();

      // The window measures silence, not total time, and it gets far more
      // generous the moment somebody is clearly there. The short first wait
      // is paid on every boot; the long one is only ever paid by a person at
      // the desk who has paused. Closing mid-sequence - between SET and a
      // 64-character SECRET, say - means resetting the board and racing it
      // again, which is the thing worth avoiding.
      start  = millis();
      window = PROVISION_IDLE_MS;

      if (c != '\n' && c != '\r') {
        line += c;
        continue;
      }

      line.trim();
      if (line.length() == 0) {
        line = "";
        continue;
      }

      if (line.equalsIgnoreCase("SHOW")) {
        printDeviceIdentity();

      } else if (line.length() >= 6 && line.substring(0, 6).equalsIgnoreCase("SECRET")) {
        String secret = line.substring(6);
        secret.trim();
        secret.toLowerCase();

        // Caught separately from a malformed value, because it means something
        // different and has a different fix: the clipboard was empty or held
        // something stale, so nothing arrived after the word. Reporting this
        // as "not recognized" sent people looking at the command they typed
        // instead of at what actually got pasted.
        if (secret.length() == 0) {
          Serial.println("[Provision] SECRET needs the value after it, and nothing was pasted.");
          Serial.println("[Provision] The copy button on the TOC device page puts the whole line");
          Serial.println("[Provision] on the clipboard: SECRET followed by 64 characters.");
          line = "";
          continue;
        }

        // 32 bytes as hex. Checked here so a truncated paste is caught at the
        // desk, rather than as reports the server silently refuses later.
        bool wellFormed = secret.length() == 64;
        for (size_t i = 0; wellFormed && i < secret.length(); i++) {
          char ch = secret[i];
          wellFormed = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        }

        if (!wellFormed) {
          Serial.printf("[Provision] That is not a signing secret: expected 64 hex "
                        "characters, got %u. Copy the whole line from the TOC device page.\n",
                        (unsigned) secret.length());
        } else {
          saveSigningSecret(secret);
          signingSecret = secret;
          Serial.println("[Provision] Signing secret saved. Reports from this unit are now signed.");
          Serial.println("[Provision] Once the server accepts one, unsigned reports claiming to");
          Serial.println("[Provision] be this device are refused - so keep the secret if you reflash.");
        }

      } else if (line.length() >= 4 && (line.startsWith("SET ") || line.startsWith("set "))) {
        String rest = line.substring(4);
        rest.trim();

        // "code,model,firmware" - model and firmware are optional, so only
        // split on the commas that are actually there.
        int c1 = rest.indexOf(',');
        int c2 = (c1 == -1) ? -1 : rest.indexOf(',', c1 + 1);

        String code, model, firmware;
        if (c1 == -1) {
          code = rest;
        } else if (c2 == -1) {
          code  = rest.substring(0, c1);
          model = rest.substring(c1 + 1);
        } else {
          code     = rest.substring(0, c1);
          model    = rest.substring(c1 + 1, c2);
          firmware = rest.substring(c2 + 1);
        }
        code.trim();
        model.trim();
        firmware.trim();

        if (code.length() == 0) {
          Serial.println("[Provision] device_code can't be blank - try again, e.g. SET IMP-042");
        } else {
          // Accepted and ignored rather than rejected: the three-field form
          // is in the old instructions, on the TOC page and in people's
          // muscle memory. Failing the whole command over a field that is now
          // meaningless would be the wrong trade.
          if (firmware.length()) {
            Serial.printf("[Provision] Ignoring firmware \"%s\" - that field now comes from the\n",
                          firmware.c_str());
            Serial.printf("[Provision] build (%s) and cannot be set by hand.\n", FIRMWARE_VERSION);
          }

          saveDeviceIdentity(code, model);
          deviceCode  = code;
          deviceModel = model;
          Serial.println("[Provision] Saved.");
          printDeviceIdentity();
          Serial.println("[Provision] Still listening - reset the device (or wait for the "
                         "window to close) to start normal operation with this identity.");
        }

      } else if (line.equalsIgnoreCase("FORGET")) {
        // Deliberately two steps. SET overwrites a code, which is recoverable
        // by typing it again; this throws away the signing secret, and a
        // mistyped command should not be able to do that silently.
        Serial.println("[Provision] FORGET erases this device code, model and signing secret.");
        Serial.println("[Provision] The board then announces itself for enrolment on the next boot.");
        Serial.println("[Provision] Its credentials are recoverable only if its MAC is on record.");
        Serial.println("[Provision] To go ahead, type:  FORGET CONFIRM");

      } else if (line.equalsIgnoreCase("FORGET CONFIRM")) {
        clearDeviceIdentity();
        Serial.println("[Provision] Identity erased. This unit is now UNPROVISIONED.");
        Serial.println("[Provision] Open enrolment on the TOC device page, then reset this board");
        Serial.println("[Provision] and it will announce itself.");
        printDeviceIdentity();

      } else {
        Serial.println("[Provision] Not recognized. Use:  SET <device_code>,<model>   SHOW   SECRET <64 hex>   FORGET");
      }

      line = "";
    }
  }

  Serial.println("[Provision] Window closed - continuing startup with the identity above.");
  Serial.println();
}

bool hasSigningSecret() {
  return signingSecret.length() > 0;
}

unsigned long nextRequestCounter() {
  return bootCounterBase + (++requestsThisBoot);
}

String signRequest(unsigned long counter, const String &body) {
  if (signingSecret.length() == 0) {
    return "";
  }

  // Exactly what the server rebuilds: identity, counter and the bytes sent.
  // Signing the body too means a captured report cannot be edited to move the
  // crash somewhere else and still verify.
  String payload = deviceCode + "\n" + String(counter) + "\n" + body;

  const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (sha256 == nullptr) {
    return "";
  }

  unsigned char digest[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);

  bool ok = mbedtls_md_setup(&ctx, sha256, 1) == 0
         && mbedtls_md_hmac_starts(&ctx, (const unsigned char *) signingSecret.c_str(),
                                   signingSecret.length()) == 0
         && mbedtls_md_hmac_update(&ctx, (const unsigned char *) payload.c_str(),
                                   payload.length()) == 0
         && mbedtls_md_hmac_finish(&ctx, digest) == 0;

  mbedtls_md_free(&ctx);

  if (!ok) {
    Serial.println("Signing failed - sending unsigned.");
    return "";
  }

  static const char *HEX_DIGITS = "0123456789abcdef"; // not HEX: Arduino Print.h defines that as 16
  String out;
  out.reserve(64);
  for (unsigned char byte : digest) {
    out += HEX_DIGITS[byte >> 4];
    out += HEX_DIGITS[byte & 0x0F];
  }

  return out;
}

String getDeviceCode() {
  return deviceCode;
}

String getDeviceModel() {
  return deviceModel;
}

String getFirmwareVersion() {
  return String(FIRMWARE_VERSION);
}

// ── automatic enrolment ──────────────────────────────────────────────────────

bool deviceNeedsEnrolment() {
  // A code without a secret counts as needing enrolment too. That unit can
  // report, but only unsigned, and the server will accept unsigned reports
  // claiming to be it from anyone — which is the gap the signing secret exists
  // to close.
  return deviceCode == UNPROVISIONED || deviceCode.length() == 0
         || signingSecret.length() == 0;
}

bool applyEnrolledIdentity(const String &code, const String &model, const String &secret) {
  String cleanCode   = code;   cleanCode.trim();
  String cleanModel  = model;  cleanModel.trim();
  String cleanSecret = secret; cleanSecret.trim(); cleanSecret.toLowerCase();

  if (cleanCode.length() == 0) {
    Serial.println("[Enrol] Server sent no device_code. Nothing written.");
    return false;
  }

  // Same check the SECRET command applies to a pasted value. A truncated or
  // malformed secret written to flash would produce signatures the server
  // rejects, and the unit would look like a network fault rather than a
  // provisioning one.
  bool wellFormed = cleanSecret.length() == 64;
  for (size_t i = 0; wellFormed && i < cleanSecret.length(); i++) {
    char ch = cleanSecret[i];
    wellFormed = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
  }

  if (!wellFormed) {
    Serial.printf("[Enrol] Server sent a malformed signing secret (%u chars). Nothing written.\n",
                  (unsigned) cleanSecret.length());
    return false;
  }

  // Both writes, then both in-memory values, so a power loss between them
  // leaves flash consistent with itself rather than a code with no secret.
  saveDeviceIdentity(cleanCode, cleanModel);
  saveSigningSecret(cleanSecret);

  deviceCode    = cleanCode;
  deviceModel   = cleanModel;
  signingSecret = cleanSecret;

  return true;
}
