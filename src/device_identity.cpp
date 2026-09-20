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
static String firmwareVersion;

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

static void saveDeviceIdentity(const String &code, const String &model, const String &firmware) {
  preferences.begin(NVS_NAMESPACE, false); // read-write
  preferences.putString("device_code", code);
  preferences.putString("model", model);
  preferences.putString("firmware", firmware);
  preferences.end();
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
  Serial.printf("  firmware_version : %s\n", firmwareVersion.length() ? firmwareVersion.c_str() : "(not set)");
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
  firmwareVersion = preferences.getString("firmware", "");
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
  Serial.println(" Type:  SET <device_code>,<model>,<firmware>");
  Serial.println("   or:  SHOW");
  Serial.println("   or:  SECRET <64 hex characters>");
  Serial.println(" then press Enter. Full instructions are in");
  Serial.println(" include/device_identity.h. No input needed to");
  Serial.println(" skip - the device boots normally on its own.");
  Serial.println("===============================================");
  printDeviceIdentity();

  String line;
  unsigned long start = millis();

  // Only runs right after power-on/reset. Once this returns, typing on serial
  // no longer reconfigures anything, so a unit already in the field cannot be
  // silently reprovisioned without someone physically holding it, plugging in
  // USB, and resetting it on purpose.
  while (millis() - start < PROVISION_WINDOW_MS) {
    while (Serial.available()) {
      char c = Serial.read();

      // The window measures silence, not total time. Eight seconds is plenty
      // to notice somebody is there, but nowhere near enough to send SET, then
      // a 64-character SECRET, then SHOW - and having it close mid-sequence
      // means resetting the board and racing it again. Anything typed keeps
      // it open; it closes eight seconds after the last keystroke.
      start = millis();

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

      } else if (line.length() >= 7 && (line.startsWith("SECRET ") || line.startsWith("secret "))) {
        String secret = line.substring(7);
        secret.trim();
        secret.toLowerCase();

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
          saveDeviceIdentity(code, model, firmware);
          deviceCode      = code;
          deviceModel     = model;
          firmwareVersion = firmware;
          Serial.println("[Provision] Saved.");
          printDeviceIdentity();
          Serial.println("[Provision] Still listening - reset the device (or wait for the "
                         "window to close) to start normal operation with this identity.");
        }

      } else {
        Serial.println("[Provision] Not recognized. Use:  SET <device_code>,<model>,<firmware>   or   SHOW");
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
  return firmwareVersion;
}
