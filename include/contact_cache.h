#pragma once

#include <Arduino.h>

// Caches the paired rider's emergency contact in NVS (flash) so it survives
// reboots and stays available even if connectivity is lost by the time an
// actual crash happens - fetching it fresh at crash time would defeat the
// point, since that's precisely when the network may be unreachable.

void contactCacheInit();
void refreshContactCache();
String getCachedRiderName();
String getCachedContactPhone();
