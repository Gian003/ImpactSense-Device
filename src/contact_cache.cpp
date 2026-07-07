#include "contact_cache.h"
#include "backend.h"

#include <Preferences.h>

static Preferences preferences;
static const char* NVS_NAMESPACE = "impactsense";

static String cachedRiderName;
static String cachedContactPhone;

void contactCacheInit() {
  preferences.begin(NVS_NAMESPACE, true); // read-only
  cachedRiderName    = preferences.getString("rider_name", "");
  cachedContactPhone = preferences.getString("ec_phone", "");
  preferences.end();

  if (cachedContactPhone.length() > 0) {
    Serial.printf("Loaded cached emergency contact from flash: rider=%s phone=%s\n",
                  cachedRiderName.c_str(), cachedContactPhone.c_str());
  } else {
    Serial.println("No emergency contact cached yet.");
  }
}

void refreshContactCache() {
  String riderName, contactName, contactPhone;

  if (!fetchEmergencyContact(riderName, contactName, contactPhone)) {
    return; // fetchEmergencyContact() already logged the reason
  }

  cachedRiderName    = riderName;
  cachedContactPhone = contactPhone;

  preferences.begin(NVS_NAMESPACE, false); // read-write
  preferences.putString("rider_name", cachedRiderName);
  preferences.putString("ec_phone", cachedContactPhone);
  preferences.end();

  Serial.printf("Emergency contact cache refreshed: rider=%s contact=%s phone=%s\n",
                cachedRiderName.c_str(), contactName.c_str(), cachedContactPhone.c_str());
}

String getCachedRiderName() {
  return cachedRiderName;
}

String getCachedContactPhone() {
  return cachedContactPhone;
}
