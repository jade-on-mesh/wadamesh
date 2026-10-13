// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Watched channels on the map, as in MeshCore Open TAK's Watch tab: text
// reports on chosen channels that carry a position go on the map for a while.
//
//   IceBot (#icewatch):  🟢 OBSERVED / 10-12 15:00 / @Portland NE Killingsworth St
//                        / 1-🥾 / 📍 45.5625,-122.6108
//   drone detector:      DRONE 1581F9DEV26590296G6G h251m 1m/s
//                        https://maps.google.com/?q=47.60378,-122.34052
//                        Op https://maps.google.com/?q=47.60531,-122.34016
//
// A drone is tracked by serial: only its newest report stays on the map, with
// its operator ("Op") and its direction of travel (reported, or from its last
// two positions). The rules mirror lib/team/watch_feed.dart in the app.
//
// Pure logic, header-only, host-tested in test/test_watch_feed.cpp.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "TeamWire.h"

namespace watchfeed {

enum Level : uint8_t { kNone, kCritical, kActive, kObserved, kOther };

inline uint32_t levelRgb(uint8_t l) {
  switch (l) {
    case kCritical: return 0xE53935;
    case kActive: return 0xFDD835;
    case kObserved: return 0x43A047;
    case kOther: return 0x1E88E5;
    default: return 0x9E9E9E;
  }
}

struct Report {
  uint8_t level = kNone;
  bool has2 = false;          // a second position (a drone's operator)
  bool has_bearing = false;   // direction of travel the sender reported
  float bearing = 0;
  double lat = 0, lon = 0, lat2 = 0, lon2 = 0;
  char track[24] = {0};       // a drone's serial; "" for anything else
  char detail[24] = {0};      // "h251m 1m/s"
  char label2[12] = {0};      // "Op"
  char title[48] = {0};       // the place, else the status, else the first line
  char body[128] = {0};       // the rest, one line, " · " between parts
};

// ---- small text helpers -------------------------------------------------------------------
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isAlnum(char c) { return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool isWord(char c) { return isAlnum(c) || c == '_'; }
inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

inline bool startsNoCase(const char* s, const char* w) {
  for (; *w; ++s, ++w)
    if (lower(*s) != *w) return false;
  return true;
}
inline bool containsNoCase(const char* s, size_t n, const char* w) {
  const size_t k = strlen(w);
  for (size_t i = 0; i + k <= n; i++) {
    size_t j = 0;
    while (j < k && lower(s[i + j]) == w[j]) j++;
    if (j == k) return true;
  }
  return false;
}

inline void copyN(char* out, size_t cap, const char* s, size_t n) {
  if (!cap) return;
  if (n >= cap) n = cap - 1;
  memcpy(out, s, n);
  out[n] = 0;
}

// Appends "a · b" style; cuts at a UTF-8 boundary.
inline void append(char* out, size_t cap, const char* s, size_t n) {
  size_t o = strlen(out);
  if (o && o + 5 < cap) { memcpy(out + o, " \xC2\xB7 ", 4); o += 4; }
  while (n && o + 1 < cap) {
    size_t ch = 1;
    const uint8_t b = (uint8_t)*s;
    if (b >= 0xF0) ch = 4; else if (b >= 0xE0) ch = 3; else if (b >= 0xC0) ch = 2;
    if (ch > n || o + ch >= cap) break;
    memcpy(out + o, s, ch);
    o += ch; s += ch; n -= ch;
  }
  out[o] = 0;
}

// A level named in text: CRITICAL / ACTIVE / OBSERVED / OTHER, URGENT or ALERT.
inline uint8_t levelFromWord(const char* s, size_t n) {
  if (containsNoCase(s, n, "critical")) return kCritical;
  if (containsNoCase(s, n, "active")) return kActive;
  if (containsNoCase(s, n, "observed")) return kObserved;
  if (containsNoCase(s, n, "other")) return kOther;
  if (containsNoCase(s, n, "urgent") || containsNoCase(s, n, "alert")) return kCritical;
  return kNone;
}

// -?\d{a..b}\.\d{minFrac,} at p
inline bool number(const char* p, int maxInt, int minFrac, double* v, const char** end) {
  const char* q = p;
  if (*q == '-') q++;
  int d = 0;
  while (isDigit(q[d])) d++;
  if (d < 1 || d > maxInt || q[d] != '.') return false;
  q += d + 1;
  int f = 0;
  while (isDigit(q[f])) f++;
  if (f < minFrac) return false;
  *v = strtod(p, nullptr);
  *end = q + f;
  return true;
}

// "45.5625,-122.6108" (spaces allowed around the comma) at p
inline bool coordAt(const char* p, double* la, double* lo, const char** end) {
  const char* e;
  if (!number(p, 2, 3, la, &e)) return false;
  while (isSpace(*e)) e++;
  if (*e != ',') return false;
  e++;
  while (isSpace(*e)) e++;
  return number(e, 3, 3, lo, end);
}

inline bool validPos(double la, double lo) { return fabs(la) <= 90 && fabs(lo) <= 180; }

// "045° NE"
inline void bearingLabel(float b, char* out, size_t cap) {
  static const char* const names[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  double d = fmod(fmod((double)b, 360.0) + 360.0, 360.0);
  int r = (int)lround(d);
  snprintf(out, cap, "%03d\xC2\xB0 %s", r, names[((int)((d + 22.5) / 45)) % 8]);
}

// The map's label for a drone: "DRONE 1581F9DE h251m 1m/s 045° NE" (the first
// 8 letters and digits of its serial, its facts, its direction of travel).
inline void droneLabel(const Report& r, bool has_course, float course, char* out, size_t cap) {
  char shortId[9];
  int k = 0;
  for (const char* s = r.track; *s && k < 8; ++s)
    if (isAlnum(*s)) shortId[k++] = *s;
  shortId[k] = 0;
  char brg[16] = "";
  if (r.has_bearing || has_course) bearingLabel(r.has_bearing ? r.bearing : course, brg, sizeof brg);
  snprintf(out, cap, "DRONE %s%s%s%s%s", shortId, r.detail[0] ? " " : "", r.detail, brg[0] ? " " : "", brg);
}

// ---- parsing ------------------------------------------------------------------------------

// "DRONE <serial>" anywhere (any case); the serial holds a digit, so chat like
// "a drone overhead" is not a detection. Returns the end of the serial.
inline const char* findDrone(const char* t, char* serial, size_t cap) {
  for (const char* p = t; *p; ++p) {
    if (!startsNoCase(p, "drone") || (p > t && isWord(p[-1])) || isWord(p[5])) continue;
    const char* q = p + 5;
    while (isSpace(*q) || *q == ':' || *q == '#' || *q == '-') q++;
    int n = 0;
    bool digit = false;
    while (isAlnum(q[n]) || q[n] == '-') { digit |= isDigit(q[n]); n++; }
    if (n >= 6 && digit) { copyN(serial, cap, q, (size_t)n); return q + n; }
  }
  return nullptr;
}

// h251m, -3m/s, 12.5m/s, 40km/h, alt...
inline bool isFact(const char* w, size_t n) {
  if (n >= 3 && !strncmp(w, "alt", 3)) return true;
  size_t i = 0;
  if (w[0] == 'h') {
    i = 1;
    if (i < n && w[i] == '-') i++;
    size_t d = 0;
    while (i + d < n && isDigit(w[i + d])) d++;
    return d && i + d + 1 == n && w[n - 1] == 'm';
  }
  if (w[0] == '-') i = 1;
  size_t d = 0;
  while (i + d < n && isDigit(w[i + d])) d++;
  if (!d) return false;
  i += d;
  if (i < n && w[i] == '.') {
    size_t f = 0;
    while (i + 1 + f < n && isDigit(w[i + 1 + f])) f++;
    if (!f) return false;
    i += 1 + f;
  }
  if (n - i == 3 && !memcmp(w + i, "m/s", 3)) return true;
  if (n - i == 4 && !memcmp(w + i, "km/h", 4) && w[0] != '-') return true;
  return false;
}

// \d{1,3}(\.\d+)? at p
inline bool degNum(const char* p, double* v, const char** end) {
  int d = 0;
  while (isDigit(p[d])) d++;
  if (d < 1 || d > 3) return false;
  const char* e = p + d;
  if (*e == '.' && isDigit(e[1])) { e++; while (isDigit(*e)) e++; }
  *v = strtod(p, nullptr);
  *end = e;
  return true;
}

// A direction of travel: "hdg123", "brg:45", "dir 270", "heading 90", or "123°".
inline bool findBearing(const char* t, float* out) {
  static const char* const kw[] = {"hdg", "brg", "dir", "crs", "cog", "heading", "bearing", "course"};
  for (const char* p = t; *p; ++p) {
    double v;
    const char* e;
    if (isWord(*p) && (p == t || !isWord(p[-1]))) {
      for (const char* k : kw) {
        if (!startsNoCase(p, k)) continue;
        const char* q = p + strlen(k);
        while (isSpace(*q) || *q == ':' || *q == '=') q++;
        // \d{1,3} with nothing required after it
        int d = 0;
        while (isDigit(q[d])) d++;
        if (!d) continue;
        char tmp[8] = {0};
        int take = d > 3 ? 3 : d;
        memcpy(tmp, q, (size_t)take);
        if (take == d && q[d] == '.' && isDigit(q[d + 1])) {
          v = strtod(q, nullptr);
        } else {
          v = atof(tmp);
        }
        if (v <= 360) { *out = (float)v; return true; }
        return false;
      }
    }
    if (isDigit(*p) && (p == t || (!isDigit(p[-1]) && p[-1] != '.')) && degNum(p, &v, &e)) {
      if (*e == ' ') e++;
      if ((uint8_t)e[0] == 0xC2 && (uint8_t)e[1] == 0xB0) {
        if (v <= 360) { *out = (float)v; return true; }
        return false;
      }
    }
  }
  return false;
}

inline bool startsWithDot(const char* s, uint8_t* level, size_t* len) {
  static const struct { const char* d; uint8_t l; } dots[] = {
      {"\xF0\x9F\x94\xB4", kCritical},   // red
      {"\xF0\x9F\x9F\xA0", kActive},     // orange
      {"\xF0\x9F\x9F\xA1", kActive},     // yellow
      {"\xF0\x9F\x9F\xA2", kObserved},   // green
      {"\xF0\x9F\x94\xB5", kOther},      // blue
      {"\xF0\x9F\x9F\xA3", kOther},      // purple
      {"\xE2\x9A\xAA", kNone},           // white
  };
  for (const auto& x : dots) {
    const size_t n = strlen(x.d);
    if (!strncmp(s, x.d, n)) { *level = x.l; *len = n; return true; }
  }
  return false;
}

// \d{1,2}[-/]\d{1,2}\s+\d{1,2}:\d{2} as the whole line
inline bool isWhen(const char* s, size_t n) {
  size_t i = 0;
  auto digits = [&](size_t lo, size_t hi) {
    size_t d = 0;
    while (i < n && isDigit(s[i])) { i++; d++; }
    return d >= lo && d <= hi;
  };
  if (!digits(1, 2) || i >= n || (s[i] != '-' && s[i] != '/')) return false;
  i++;
  if (!digits(1, 2)) return false;
  size_t sp = 0;
  while (i < n && isSpace(s[i])) { i++; sp++; }
  if (!sp || !digits(1, 2) || i >= n || s[i] != ':') return false;
  i++;
  return digits(2, 2) && i == n;
}

// A line that is only a position ("📍 45.5625,-122.6108").
inline bool onlyCoords(const char* s, size_t n) {
  size_t i = 0;
  while (i < n) {
    double a, b;
    const char* e;
    if (coordAt(s + i, &a, &b, &e) && (size_t)(e - s) <= n) { i = (size_t)(e - s); continue; }
    if (!strncmp(s + i, "\xF0\x9F\x93\x8D", 4)) { i += 4; continue; }   // 📍
    const char c = s[i];
    if (isSpace(c) || c == ',' || c == ';' || c == ':') { i++; continue; }
    return false;
  }
  return true;
}

// Strips a "Name: " prefix (MeshCore channel text is "sender: message").
inline const char* messageText(const char* text, char* sender, size_t cap) {
  if (sender && cap) sender[0] = 0;
  const char* c = strstr(text, ": ");
  if (!c || c - text > 40 || memchr(text, '\n', (size_t)(c - text))) return text;
  if (sender) copyN(sender, cap, text, (size_t)(c - text));
  return c + 2;
}

// Parses a message (without its sender prefix). True when it has a position,
// i.e. when it belongs on the map.
inline bool parse(const char* t, Report& r) {
  r = Report();
  bool has = false;
  // A shared map marker "m:<lat>,<lon>|" first, else every "lat,lon" in the text:
  // the first is the report's, a second takes the word before it as its label.
  if (const char* m = strstr(t, "m:")) {
    char* e1;
    const double a = strtod(m + 2, &e1);
    if (e1 != m + 2 && *e1 == ',') {
      char* e2;
      const double b = strtod(e1 + 1, &e2);
      if (e2 != e1 + 1 && *e2 == '|' && validPos(a, b)) { r.lat = a; r.lon = b; has = true; }
    }
  }
  if (!has) {
    const char* prev_end = t;
    for (const char* p = t; *p;) {
      double a, b;
      const char* e;
      if (!coordAt(p, &a, &b, &e)) { ++p; continue; }
      if (validPos(a, b)) {
        if (!has) {
          r.lat = a; r.lon = b; has = true;
        } else if (!r.has2) {
          r.lat2 = a; r.lon2 = b; r.has2 = true;
          // the last word between the previous position and this one, not a link
          const char* best = nullptr;
          size_t best_n = 0;
          for (const char* w = prev_end; w < p;) {
            while (w < p && isSpace(*w)) w++;
            const char* we = w;
            while (we < p && !isSpace(*we)) we++;
            const size_t n = (size_t)(we - w);
            if (n && !memchr(w, '=', n) && !containsNoCase(w, n, "://")) {
              best = w; best_n = n;
            }
            w = we;
          }
          if (best) copyN(r.label2, sizeof r.label2, best, best_n);
        }
      }
      prev_end = e;
      p = e;
    }
  }

  // A drone: its serial, the facts after it, a direction of travel.
  const char* de = findDrone(t, r.track, sizeof r.track);
  if (de) {
    r.level = kActive;
    for (const char* w = de; *w;) {
      while (isSpace(*w)) w++;
      const char* we = w;
      while (*we && !isSpace(*we)) we++;
      if (we > w && isFact(w, (size_t)(we - w))) {
        const size_t o = strlen(r.detail);
        if (o + (o ? 1 : 0) + (size_t)(we - w) < sizeof r.detail) {
          if (o) r.detail[o] = ' ', r.detail[o + 1] = 0;
          strncat(r.detail, w, (size_t)(we - w));
        }
      }
      w = we;
    }
    float b;
    if (findBearing(de, &b)) { r.has_bearing = true; r.bearing = b; }
    char sh[9];
    int k = 0;
    for (const char* s = r.track; *s && k < 8; ++s)
      if (isAlnum(*s)) sh[k++] = *s;
    sh[k] = 0;
    snprintf(r.title, sizeof r.title, "DRONE %s", sh);
    if (r.detail[0]) append(r.body, sizeof r.body, r.detail, strlen(r.detail));
    return has;
  }

  // A report: status dot, time, @place, then whatever else.
  bool status = false, when = false, place = false;
  char first[48] = "";
  for (const char* line = t; *line;) {
    const char* le = strchr(line, '\n');
    if (!le) le = line + strlen(line);
    const char* s = line;
    const char* e = le;
    line = *le ? le + 1 : le;
    while (s < e && isSpace(*s)) s++;
    while (e > s && isSpace(e[-1])) e--;
    if (s == e) continue;
    // a link: dropped (the T-Deck can't open it)
    char buf[160];
    size_t n = (size_t)(e - s);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    char* u = strstr(buf, "http://");
    if (!u) u = strstr(buf, "https://");
    if (u) {
      char* ue = u;
      while (*ue && !isSpace(*ue)) ue++;
      memmove(u, ue, strlen(ue) + 1);
      n = strlen(buf);
      size_t a = 0;
      while (a < n && isSpace(buf[a])) a++;
      while (n > a && isSpace(buf[n - 1])) n--;
      memmove(buf, buf + a, n - a);
      n -= a;
      buf[n] = 0;
      if (!n) continue;
    }
    uint8_t lv;
    size_t dl;
    if (!status && startsWithDot(buf, &lv, &dl)) {
      status = true;
      const char* st = buf + dl;
      while (isSpace(*st)) st++;
      const uint8_t byWord = levelFromWord(st, strlen(st));
      r.level = byWord != kNone ? byWord : lv;
      if (*st) {
        copyN(first, sizeof first, st, strlen(st));
        append(r.body, sizeof r.body, st, strlen(st));
      }
      continue;
    }
    if (!when && isWhen(buf, n)) { when = true; append(r.body, sizeof r.body, buf, n); continue; }
    const char* at = (const char*)memchr(buf, '@', n < 5 ? n : 5);
    if (!place && at) {
      place = true;
      const char* pl = at + 1;
      while (isSpace(*pl)) pl++;
      copyN(r.title, sizeof r.title, pl, strlen(pl));
      continue;
    }
    if (has && onlyCoords(buf, n)) continue;   // shown as the map point
    if (!first[0]) copyN(first, sizeof first, buf, n);
    append(r.body, sizeof r.body, buf, n);
  }
  if (!status) {
    const char* le = strchr(t, '\n');
    const uint8_t w = levelFromWord(t, le ? (size_t)(le - t) : strlen(t));
    if (w != kNone) r.level = w;
  }
  if (!r.title[0]) copyN(r.title, sizeof r.title, first, strlen(first));
  return has;
}

// "ICE" for ICE watch reports: a channel named for it (#icewatch) or IceBot
// posting. The map labels those "ICE <place>" rather than with the channel name.
inline bool isIce(const char* channel, const char* sender) {
  return (channel && containsNoCase(channel, strlen(channel), "ice")) ||
         (sender && containsNoCase(sender, strlen(sender), "icebot"));
}

// Puts "ICE " before a report's title.
inline void tagIce(Report& r) {
  char t[sizeof r.title + 8];
  snprintf(t, sizeof t, "ICE%s%s", r.title[0] ? " " : "", r.title);
  size_t n = strlen(t);
  if (n >= sizeof r.title) {   // cut at a character boundary
    n = sizeof r.title - 1;
    while (n && ((uint8_t)t[n] & 0xC0) == 0x80) n--;
  }
  memcpy(r.title, t, n);
  r.title[n] = 0;
}

// ---- the reports on the map -----------------------------------------------------------------
constexpr int kMaxItems = 48;
constexpr int kMaxWatched = 8;
constexpr int kTtlChoices = 9;
// How long a report stays on the map, in minutes; 0 = always. Default 6 h.
static const uint16_t kTtlMin[kTtlChoices] = {15, 30, 60, 120, 360, 720, 1440, 4320, 0};
constexpr uint8_t kDefaultTtl = 4;

inline void ttlLabel(uint8_t idx, char* out, size_t cap) {
  const unsigned m = kTtlMin[idx % kTtlChoices];
  if (!m) snprintf(out, cap, "always");
  else if (m < 60) snprintf(out, cap, "%u min", m);
  else if (m < 48 * 60) snprintf(out, cap, "%u h", m / 60);
  else snprintf(out, cap, "%u days", m / 1440);
}

struct Item {
  bool used = false;
  uint8_t channel = 0;
  bool has_course = false;   // from its previous position (a drone)
  float course = 0;
  uint32_t at_ms = 0;        // when it happened, on this radio's millis() clock
  Report r;
};

class Store {
 public:
  Item items[kMaxItems];
  uint8_t ttl = kDefaultTtl;
  char watched[kMaxWatched][32] = {{0}};

  uint32_t ttlMs() const { return (uint32_t)kTtlMin[ttl % kTtlChoices] * 60000UL; }
  bool live(const Item& it, uint32_t now) const {
    return it.used && (!ttlMs() || now - it.at_ms < ttlMs());
  }
  int liveCount(uint32_t now) const {
    int n = 0;
    for (const Item& it : items) n += live(it, now) ? 1 : 0;
    return n;
  }

  bool isWatched(const char* name) const {
    if (!name || !*name) return false;
    for (const auto& w : watched)
      if (w[0] && !strcmp(w, name)) return true;
    return false;
  }
  // false when there is no room for another channel
  bool setWatched(const char* name, bool on) {
    for (auto& w : watched)
      if (w[0] && !strcmp(w, name)) { if (!on) w[0] = 0; return true; }
    if (!on) return true;
    for (auto& w : watched)
      if (!w[0]) { copyN(w, sizeof w, name, strlen(name)); return true; }
    return false;
  }
  int watchedCount() const {
    int n = 0;
    for (const auto& w : watched) n += w[0] ? 1 : 0;
    return n;
  }

  // A report heard at `at_ms`. A drone already on the map is moved (keeping
  // its direction of travel); anything else takes a free or the oldest slot.
  Item* add(const Report& r, uint8_t channel, uint32_t at_ms) {
    Item* slot = nullptr;
    if (r.track[0]) {
      for (Item& it : items) {
        if (!it.used || strcmp(it.r.track, r.track)) continue;
        if ((int32_t)(at_ms - it.at_ms) < 0) return nullptr;   // an older report of it
        if (TeamWire::distanceM(it.r.lat, it.r.lon, r.lat, r.lon) >= 15) {
          it.has_course = true;
          it.course = (float)TeamWire::bearingDeg(it.r.lat, it.r.lon, r.lat, r.lon);
        }
        slot = &it;
        break;
      }
    }
    if (!slot) {
      uint32_t oldest = 0;
      for (Item& it : items) {
        if (!it.used) { slot = &it; slot->has_course = false; break; }
        if (!slot || at_ms - it.at_ms > oldest) { slot = &it; oldest = at_ms - it.at_ms; }
      }
      slot->has_course = false;
    }
    slot->used = true;
    slot->channel = channel;
    slot->at_ms = at_ms;
    slot->r = r;
    return slot;
  }

  void clear() { for (Item& it : items) it.used = false; }

  // "<ttl>|<channel>|<channel>..." (channel names can't hold '|' here)
  size_t save(char* out, size_t cap) const {
    int n = snprintf(out, cap, "%u", ttl);
    for (const auto& w : watched)
      if (w[0] && n >= 0 && (size_t)n < cap) n += snprintf(out + n, cap - (size_t)n, "|%s", w);
    return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
  }
  void load(const char* s) {
    ttl = (uint8_t)(atoi(s) % kTtlChoices);
    for (auto& w : watched) w[0] = 0;
    int k = 0;
    for (const char* p = strchr(s, '|'); p && k < kMaxWatched; p = strchr(p + 1, '|')) {
      const char* e = strchr(p + 1, '|');
      const size_t n = e ? (size_t)(e - p - 1) : strlen(p + 1);
      if (n) copyN(watched[k++], sizeof watched[0], p + 1, n);
    }
  }
};

}  // namespace watchfeed
