#include "redact.h"

// Kept from each end. Four at the front covers "0963" or "+639", which is the
// network, not the subscriber; two at the back is enough to tell two numbers
// on the same network apart at a glance.
static const size_t KEEP_FRONT = 4;
static const size_t KEEP_BACK  = 2;

String maskPhone(const String &number) {
  size_t len = number.length();

  // An empty number is a state worth saying out loud rather than printing as
  // nothing: "the cache has not been filled yet" and "the cache holds an empty
  // string" both look like a missing log line otherwise.
  if (len == 0) return String("<none>");

  // Too short to keep both ends and still hide anything in between, so hide
  // all of it. Anything this short is a typo or a shortcode, and neither is
  // worth leaking to save two characters of readability.
  if (len < KEEP_FRONT + KEEP_BACK + 1) {
    String out;
    out.reserve(len);
    for (size_t i = 0; i < len; i++) out += '*';
    return out;
  }

  String out;
  out.reserve(len);
  out += number.substring(0, KEEP_FRONT);
  for (size_t i = KEEP_FRONT; i < len - KEEP_BACK; i++) out += '*';
  out += number.substring(len - KEEP_BACK);
  return out;
}
