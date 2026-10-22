// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MeshCore TEAM interop: its channel-text formats, as MeshCore Open TAK's
// lib/team/team_compat.dart reads and writes them (golden strings shared by
// test/test_team_compat.cpp and the app's test/team/team_compat_test.dart).
// Written here from TEAM's wire formats; TEAM's code (CC BY-NC-SA) isn't used.
//
//   #TEL:<base64, unpadded, 11 bytes> [lat i32 BE x1e7][lon][comp batt][phone batt][fwd]
//   #T:<base64, unpadded> [lat][lon][comp batt][phone batt][nodeCount][bitmap]  (read only)
//   #CAP:2:<flags hex>:<key 12 hex|->:<app id 16 hex|->:<alias, last, <= 24 bytes>
//   #CAP:R:<key 12 hex|->:<radio name>
//   #WAY:<meshId>|<name>|<lat>|<lon>|<desc>|<TYPE>|<lat,lon~lat,lon...>[|n/N]
//   #WAY:<name>|<lat>|<lon>|<desc>|<TYPE>           (older form)
//   #WRC:<meshId>|<chunk>|n/N
// Battery bytes: 0/1 unknown, else (mV - 2750) / 6 + 2 (<= 254); a phone byte of
// 0xFF is a radio tracking on its own GPS. A route's colour leads its
// description as "@C:AARRGGBB". See MESHCORE_TEAM.md.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace teamcompat {

constexpr size_t kMaxBytes = 140;   // a whole message, the radio name in front included
constexpr size_t kMaxAlias = 24;
constexpr int kMaxRoute = 64;   // more than a team route carries (TeamWire shapes fit ~30)

inline bool startsWith(const char* s, const char* p) { return s && strncmp(s, p, strlen(p)) == 0; }

inline bool isCompatText(const char* t) {
  return startsWith(t, "#TEL:") || startsWith(t, "#T:") || startsWith(t, "#CAP:") || startsWith(t, "#WAY:") ||
         startsWith(t, "#WRC:");
}

// ---- battery ---------------------------------------------------------------------
inline uint8_t batteryByte(int mv) {
  if (mv <= 0) return 1;
  if (mv < 2750) mv = 2750;
  if (mv > 4280) mv = 4280;
  const int b = (mv - 2750) / 6 + 2;
  return (uint8_t)(b > 254 ? 254 : b);
}
inline int batteryMv(uint8_t b) { return (b <= 1 || b == 0xFF) ? -1 : 2750 + (b - 2) * 6; }
// As our Team shows it: the 3.3-4.2 V line the app uses.
inline int batteryPct(int mv) {
  if (mv <= 0) return -1;
  const long p = lround((mv - 3300) / 9.0);
  return (int)(p < 0 ? 0 : p > 100 ? 100 : p);
}

// ---- base64 ----------------------------------------------------------------------
inline int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
// Decodes (padded or not) up to `cap` bytes; -1 on a bad character or length.
inline int b64decode(const char* s, uint8_t* out, size_t cap) {
  uint32_t acc = 0;
  int bits = 0;
  size_t n = 0, chars = 0;
  for (; *s && *s != '='; s++, chars++) {
    const int v = b64val(*s);
    if (v < 0) return -1;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= cap) return -1;
      out[n++] = (uint8_t)(acc >> bits);
    }
  }
  if (chars % 4 == 1) return -1;
  return (int)n;
}
inline size_t b64encode(const uint8_t* b, size_t n, char* out, size_t cap) {   // unpadded
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < n; i++) {
    acc = (acc << 8) | b[i];
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      if (o + 1 < cap) out[o++] = k[(acc >> bits) & 63];
    }
  }
  if (bits > 0 && o + 1 < cap) out[o++] = k[(acc << (6 - bits)) & 63];
  if (cap) out[o < cap ? o : cap - 1] = 0;
  return o;
}

// ---- positions -------------------------------------------------------------------
struct Pos {
  double lat = 0, lon = 0;
  int comp_mv = -1, phone_mv = -1;
  bool autonomous = false;
  int mv() const { return comp_mv > 0 ? comp_mv : phone_mv; }
};

inline int32_t rd32(const uint8_t* p) {
  return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]);
}

inline bool parsePos(const char* t, Pos& p) {
  bool topo;
  const char* body;
  if (startsWith(t, "#TEL:")) { topo = false; body = t + 5; }
  else if (startsWith(t, "#T:")) { topo = true; body = t + 3; }
  else return false;
  uint8_t raw[64];
  const int n = b64decode(body, raw, sizeof raw);
  if (topo ? n < 11 : n != 11) return false;
  p.lat = rd32(raw) / 1e7;
  p.lon = rd32(raw + 4) / 1e7;
  if (fabs(p.lat) > 90 || fabs(p.lon) > 180) return false;
  p.autonomous = raw[9] == 0xFF;
  p.comp_mv = batteryMv(raw[8]);
  p.phone_mv = batteryMv(raw[9]);
  return true;
}

// `#TEL:` for a position (no forwarding asked for). Latitude/longitude truncate
// toward zero, as TEAM's Dart `(lat * 1e7).toInt()`.
inline size_t encodeTel(double lat, double lon, int comp_mv, int phone_mv, char* out, size_t cap) {
  uint8_t b[11];
  const int32_t la = (int32_t)(lat * 1e7), lo = (int32_t)(lon * 1e7);
  for (int i = 0; i < 4; i++) {
    b[i] = (uint8_t)((uint32_t)la >> (24 - 8 * i));
    b[4 + i] = (uint8_t)((uint32_t)lo >> (24 - 8 * i));
  }
  b[8] = batteryByte(comp_mv);
  b[9] = batteryByte(phone_mv);
  b[10] = 1;
  if (cap < 6 + 15) return 0;
  memcpy(out, "#TEL:", 5);
  return 5 + b64encode(b, sizeof b, out + 5, cap - 5);
}

// ---- #CAP ------------------------------------------------------------------------
inline bool isHex(const char* s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}
inline void lower(char* s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32); }

struct Cap {
  int version = 0, flags = 0;
  char key[13] = {0};        // "" when not sent
  char app[17] = {0};
  bool has_alias = false;    // false for v1
  char alias[kMaxAlias * 2 + 1] = {0};
};

inline bool parseCap(const char* t, Cap& c) {
  if (!startsWith(t, "#CAP:") || startsWith(t, "#CAP:R:")) return false;
  const char* p = t + 5;
  char* end;
  const long v = strtol(p, &end, 10);
  if (end == p || *end != ':' || v < 1) return false;
  p = end + 1;
  const long f = strtol(p, &end, 16);
  if (end == p || (*end != ':' && *end != 0)) return false;
  c.version = (int)v;
  c.flags = (int)(f & 0xFF);
  c.key[0] = c.app[0] = c.alias[0] = 0;
  c.has_alias = false;
  if (*end == 0) return true;
  p = end + 1;
  const char* k = strchr(p, ':');
  if (!k) return true;   // no alias field: treat as v1-ish
  if (k - p == 12 && isHex(p, 12)) { memcpy(c.key, p, 12); c.key[12] = 0; lower(c.key); }
  p = k + 1;
  const char* a = strchr(p, ':');
  if (a && ((a - p == 1 && *p == '-') || (a - p == 16 && isHex(p, 16)))) {
    if (*p != '-') { memcpy(c.app, p, 16); c.app[16] = 0; lower(c.app); }
    p = a + 1;
  }
  c.has_alias = true;
  while (*p == ' ') p++;
  snprintf(c.alias, sizeof c.alias, "%s", p);
  size_t n = strlen(c.alias);
  while (n && c.alias[n - 1] == ' ') c.alias[--n] = 0;
  return true;
}

// Our #CAP: no TEAM flags, our key prefix (12 hex), no app id, the alias cut to
// 24 bytes on a character boundary.
inline size_t encodeCap(const char* key12, const char* alias, char* out, size_t cap) {
  size_t n = strlen(alias ? alias : "");
  if (n > kMaxAlias) {
    n = kMaxAlias;
    while (n && ((uint8_t)alias[n] & 0xC0) == 0x80) n--;
  }
  const int w = snprintf(out, cap, "#CAP:2:00:%s:-:%.*s", key12 && key12[0] ? key12 : "-", (int)n, alias ? alias : "");
  return w < 0 || (size_t)w >= cap ? 0 : (size_t)w;
}

struct CapReq {
  char key[13] = {0};
  char name[33] = {0};
};
inline bool parseCapReq(const char* t, CapReq& r) {
  if (!startsWith(t, "#CAP:R:")) return false;
  const char* p = t + 7;
  const char* c = strchr(p, ':');
  if (!c) return false;
  r.key[0] = 0;
  if (c - p == 12 && isHex(p, 12)) { memcpy(r.key, p, 12); r.key[12] = 0; lower(r.key); }
  p = c + 1;
  while (*p == ' ') p++;
  snprintf(r.name, sizeof r.name, "%s", p);
  size_t n = strlen(r.name);
  while (n && r.name[n - 1] == ' ') r.name[--n] = 0;
  return n > 0;
}

// ---- #WAY / #WRC -------------------------------------------------------------------
enum Type : uint8_t { kCamp, kMeetup, kDanger, kGame, kStand, kWater, kVehicle, kRoute, kCustom, kTypes };
inline const char* typeWire(int t) {
  static const char* n[kTypes] = {"CAMP", "MEETUP", "DANGER", "GAME", "STAND", "WATER", "VEHICLE", "ROUTE", "CUSTOM"};
  return (t >= 0 && t < kTypes) ? n[t] : "CUSTOM";
}
inline int typeFromWire(const char* s, size_t n) {
  for (int t = 0; t < kTypes; t++) {
    const char* w = typeWire(t);
    if (strlen(w) != n) continue;
    bool eq = true;
    for (size_t i = 0; i < n && eq; i++) {
      char c = s[i];
      if (c >= 'a' && c <= 'z') c = (char)(c - 32);
      eq = c == w[i];
    }
    if (eq) return t;
  }
  return kCustom;
}

// Our marker kinds (point lastKnown staging clue hazard landingZone medical
// subjectFound water searched) and TEAM's types, as the app maps them.
inline uint8_t markerKindForType(int t) {
  switch (t) {
    case kCamp: return 2;     // staging
    case kMeetup: return 1;   // last known
    case kDanger: return 4;   // hazard
    case kWater: return 8;    // water
    default: return 0;        // point
  }
}
inline int typeForMarkerKind(int k) {
  switch (k) {
    case 2: return kCamp;
    case 1: case 5: return kMeetup;
    case 4: return kDanger;
    case 8: return kWater;
    default: return kCustom;
  }
}

struct Way {
  char mesh_id[48] = {0};
  char name[33] = {0};
  double lat = 0, lon = 0;
  char desc[61] = {0};
  int type = kCustom;
  bool has_color = false;
  uint32_t color = 0;   // ARGB
  int n = 0;            // route points
  double rlat[kMaxRoute] = {0}, rlon[kMaxRoute] = {0};
  bool isRoute() const { return type == kRoute; }
};

// Copies field [s, e) into out (cut to cap).
inline void field(const char* s, const char* e, char* out, size_t cap) {
  size_t n = (size_t)(e - s);
  if (n >= cap) {
    n = cap - 1;
    while (n && ((uint8_t)s[n] & 0xC0) == 0x80) n--;
  }
  memcpy(out, s, n);
  out[n] = 0;
}

inline bool parsePart(const char* s, const char* e, int* part, int* parts) {
  char b[12];
  if (e - s <= 0 || e - s >= (long)sizeof b) return false;
  field(s, e, b, sizeof b);
  const char* sl = strchr(b, '/');
  if (!sl) return false;
  char* x;
  const long a = strtol(b, &x, 10);
  if (x != sl) return false;
  const long z = strtol(sl + 1, &x, 10);
  if (*x || a < 1 || z < 1 || z > 64) return false;
  *part = (int)a;
  *parts = (int)z;
  return true;
}

// Appends "lat,lon~..." points from [s, e) to w (bad pairs skipped).
inline void addRoute(Way& w, const char* s, const char* e) {
  while (s < e && w.n < kMaxRoute) {
    const char* t = s;
    while (t < e && *t != '~') t++;
    char b[48];
    if (t - s > 0 && t - s < (long)sizeof b) {
      field(s, t, b, sizeof b);
      char* x;
      const double la = strtod(b, &x);
      if (*x == ',') {
        char* y;
        const double lo = strtod(x + 1, &y);
        if (*y == 0 && fabs(la) <= 90 && fabs(lo) <= 180) {
          w.rlat[w.n] = la;
          w.rlon[w.n] = lo;
          w.n++;
        }
      }
    }
    s = t + 1;
  }
}

// One #WAY. `chunk` (if non-null) gets the route text's [start, end) and part
// info when it's the first of several messages.
inline bool parseWay(const char* t, Way& w, int* part, int* parts, const char** cs, const char** ce) {
  if (!startsWith(t, "#WAY:")) return false;
  const char* f[9];
  const char* e[9];
  int k = 0;
  const char* p = t + 5;
  while (k < 9) {
    f[k] = p;
    const char* q = strchr(p, '|');
    e[k] = q ? q : p + strlen(p);
    k++;
    if (!q) break;
    p = q + 1;
  }
  int o;   // index of the name field
  if (k >= 6) { o = 1; field(f[0], e[0], w.mesh_id, sizeof w.mesh_id); }
  else if (k == 5) { o = 0; w.mesh_id[0] = 0; }
  else return false;
  field(f[o], e[o], w.name, sizeof w.name);
  char nb[32];
  field(f[o + 1], e[o + 1], nb, sizeof nb);
  char* x;
  w.lat = strtod(nb, &x);
  if (x == nb || *x) return false;
  field(f[o + 2], e[o + 2], nb, sizeof nb);
  w.lon = strtod(nb, &x);
  if (x == nb || *x || fabs(w.lat) > 90 || fabs(w.lon) > 180) return false;
  const char* ds = f[o + 3];
  w.has_color = false;
  if (e[o + 3] - ds >= 11 && startsWith(ds, "@C:") && isHex(ds + 3, 8)) {
    char hb[9];
    memcpy(hb, ds + 3, 8);
    hb[8] = 0;
    w.color = (uint32_t)strtoul(hb, nullptr, 16);
    w.has_color = true;
    ds += 11;
  }
  field(ds, e[o + 3], w.desc, sizeof w.desc);
  w.type = typeFromWire(f[o + 4], (size_t)(e[o + 4] - f[o + 4]));
  *part = *parts = 1;
  w.n = 0;
  const char* rs = k > o + 5 ? f[o + 5] : e[o + 4];
  const char* re = k > o + 5 ? e[o + 5] : e[o + 4];
  if (k > o + 6) parsePart(f[o + 6], e[o + 6], part, parts);
  if (*parts <= 1) addRoute(w, rs, re);
  if (cs) *cs = rs;
  if (ce) *ce = re;
  return true;
}

// Writes a waypoint (no route) as one #WAY: `overhead` is the "Name: " the
// firmware puts in front. Fields lose '|' and newlines.
inline size_t encodeWay(const char* mesh_id, const char* name, double lat, double lon, const char* desc, int type,
                        size_t overhead, char* out, size_t cap) {
  auto clean = [](const char* s, char* o, size_t c) {
    size_t i = 0;
    for (; s && *s && i + 1 < c; s++) o[i++] = (*s == '|') ? '/' : (*s == '\n' || *s == '\r') ? ' ' : *s;
    o[i] = 0;
  };
  char n[40], d[64];
  clean(name, n, sizeof n);
  clean(desc, d, sizeof d);
  const size_t limit = kMaxBytes > overhead ? kMaxBytes - overhead : 0;
  for (int dl = (int)strlen(d); dl >= 0; dl--) {   // drop note text until it fits
    while (dl > 0 && ((uint8_t)d[dl] & 0xC0) == 0x80) dl--;
    const int w = snprintf(out, cap, "#WAY:%s|%s|%.6f|%.6f|%.*s|%s|", mesh_id, n, lat, lon, dl, d, typeWire(type));
    if (w > 0 && (size_t)w < cap && (size_t)w <= limit) return (size_t)w;
    if (dl == 0) break;
  }
  return 0;
}

// Long routes: their #WAY and #WRC parts back together, per sender. Small and
// fixed: a couple of routes in flight at once (about 6 KB: allocate it).
class Assembler {
 public:
  // A whole waypoint in `out` when `t` completes one (or is one).
  bool accept(const char* sender, const char* t, uint32_t now_ms, Way& out) {
    for (Slot& s : slots_)
      if (s.used && now_ms - s.at > 120000UL) s.used = false;
    int part, parts;
    const char *cs, *ce;
    if (parseWay(t, out, &part, &parts, &cs, &ce)) {
      if (parts <= 1) return true;
      Slot* s = slot(sender, out.mesh_id, now_ms, true);
      if (!s) return false;
      s->head = out;
      s->has_head = true;
      s->parts = parts;
      put(*s, part, cs, ce);
      return finish(*s, out);
    }
    if (!startsWith(t, "#WRC:")) return false;
    const char* p = t + 5;
    const char* a = strchr(p, '|');
    if (!a) return false;
    const char* b = strchr(a + 1, '|');
    if (!b) return false;
    char id[48];
    field(p, a, id, sizeof id);
    if (!parsePart(b + 1, b + 1 + strlen(b + 1), &part, &parts)) return false;
    Slot* s = slot(sender, id, now_ms, true);
    if (!s) return false;
    if (!s->has_head) s->parts = parts;
    put(*s, part, a + 1, b);
    return finish(*s, out);
  }

 private:
  struct Slot {
    bool used = false, has_head = false;
    char key[96] = {0};
    uint32_t at = 0;
    int parts = 0;
    uint64_t have = 0;
    char text[12][150];
    Way head;
  };
  Slot slots_[2];

  Slot* slot(const char* sender, const char* id, uint32_t now, bool create) {
    char k[96];
    snprintf(k, sizeof k, "%s|%s", sender, id);
    for (Slot& s : slots_) if (s.used && strcmp(s.key, k) == 0) return &s;
    if (!create) return nullptr;
    Slot* pick = &slots_[0];
    for (Slot& s : slots_) {
      if (!s.used) { pick = &s; break; }
      if ((int32_t)(s.at - pick->at) < 0) pick = &s;
    }
    pick->used = true;
    pick->has_head = false;
    pick->have = 0;
    pick->parts = 0;
    pick->at = now;
    snprintf(pick->key, sizeof pick->key, "%s", k);
    return pick;
  }
  static void put(Slot& s, int part, const char* a, const char* b) {
    if (part < 1 || part > 12) return;
    field(a, b, s.text[part - 1], sizeof s.text[0]);
    s.have |= 1ull << (part - 1);
  }
  static bool finish(Slot& s, Way& out) {
    if (!s.has_head || s.parts < 1 || s.parts > 12) return false;
    for (int i = 0; i < s.parts; i++) if (!(s.have & (1ull << i))) return false;
    out = s.head;
    out.n = 0;
    for (int i = 0; i < s.parts; i++) {
      const char* t = s.text[i];
      size_t n = strlen(t);
      if (n && t[n - 1] == '~') n--;
      addRoute(out, t, t + n);
    }
    s.used = false;
    return true;
  }
};

}  // namespace teamcompat
