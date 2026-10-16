// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared map pins in chat messages, as the MeshCore Open TAK app sends and reads
// them (lib/screens/map_screen.dart parseMarkerText / buildSharedMarkerKey):
//   m:<lat>,<lon>|<label>|<flags>
// anywhere in a DM or channel message. The map shows the newest pin for each
// (conversation, sender, label, flags), so moving a labelled pin moves it.
// Pure C++ (host-tested in test/test_shared_pins.cpp).

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace sharedpins {

constexpr int kMaxPins = 48;

struct Pin {
  double lat = 0, lon = 0;
  char label[33] = {0};
  char flags[9] = {0};
  char from[32] = {0};     // who shared it
  char thread[33] = {0};   // the DM contact or the channel
  bool channel = false;
  bool outgoing = false;
  uint32_t ts = 0;
};

namespace detail {
inline bool numChar(char c) { return (c >= '0' && c <= '9') || c == '-' || c == '.'; }

inline void copyTrimmed(char* out, size_t cap, const char* s, size_t n) {
  while (n && isspace((unsigned char)*s)) { s++; n--; }
  while (n && isspace((unsigned char)s[n - 1])) n--;
  if (!cap) return;
  if (n >= cap) n = cap - 1;
  memcpy(out, s, n);
  out[n] = 0;
}

inline bool eqNoCase(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
  }
  return *a == *b;
}
}  // namespace detail

// The first "m:<lat>,<lon>|<label>|<flags>" in [text]; false when none (or the
// position is impossible). [label] and [flags] are trimmed.
inline bool parse(const char* text, double* lat, double* lon, char* label, size_t label_cap, char* flags,
                  size_t flags_cap) {
  using namespace detail;
  if (!text) return false;
  for (const char* m = strstr(text, "m:"); m; m = strstr(m + 1, "m:")) {
    const char* a = m + 2;
    const char* p = a;
    while (numChar(*p)) p++;
    if (p == a || *p != ',') continue;
    const char* b = p + 1;
    const char* q = b;
    while (numChar(*q)) q++;
    if (q == b || *q != '|') continue;
    const char* l = q + 1;
    const char* le = l;
    while (*le && *le != '|' && *le != '\n') le++;
    if (*le != '|') continue;
    const char* f = le + 1;
    const char* fe = f;
    while (*fe && *fe != '\n') fe++;
    char* end = nullptr;
    const double la = strtod(a, &end);
    if (end != p) continue;
    const double lo = strtod(b, &end);
    if (end != q) continue;
    if (la < -90 || la > 90 || lo < -180 || lo > 180) continue;
    *lat = la;
    *lon = lo;
    copyTrimmed(label, label_cap, l, (size_t)(le - l));
    copyTrimmed(flags, flags_cap, f, (size_t)(fe - f));
    return true;
  }
  return false;
}

// The app's dedupe key: same conversation, sender, label and flags.
inline bool sameKey(const Pin& a, const Pin& b) {
  using detail::eqNoCase;
  return a.channel == b.channel && eqNoCase(a.thread, b.thread) && eqNoCase(a.from, b.from) &&
         eqNoCase(a.label, b.label) && eqNoCase(a.flags, b.flags);
}

// Feed messages NEWEST FIRST; keeps the newest pin per key, up to kMaxPins.
struct Collector {
  Pin* out;
  int cap;
  int n = 0;
  Collector(Pin* o, int c) : out(o), cap(c < kMaxPins ? c : kMaxPins) {}

  // [from] is the sender (or the contact for a DM); [self] stands in for an
  // outgoing message's sender, as the app uses the node's own name.
  void add(const char* text, const char* thread, const char* from, bool channel, bool outgoing, uint32_t ts,
           const char* self) {
    if (n >= cap) return;
    Pin p;
    if (!parse(text, &p.lat, &p.lon, p.label, sizeof p.label, p.flags, sizeof p.flags)) return;
    detail::copyTrimmed(p.thread, sizeof p.thread, thread ? thread : "", thread ? strlen(thread) : 0);
    const char* who = outgoing ? (self && *self ? self : "Me") : (from && *from ? from : thread);
    detail::copyTrimmed(p.from, sizeof p.from, who ? who : "", who ? strlen(who) : 0);
    p.channel = channel;
    p.outgoing = outgoing;
    p.ts = ts;
    for (int i = 0; i < n; i++) {
      if (sameKey(out[i], p)) return;   // a newer one is already in
    }
    out[n++] = p;
  }
};

}  // namespace sharedpins
