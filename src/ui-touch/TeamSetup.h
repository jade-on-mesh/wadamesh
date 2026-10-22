// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One-scan team setup, as MeshCore Open TAK's lib/team/team_setup.dart writes
// and reads it (golden strings shared by test/test_team_setup.cpp and the
// app's test/team/team_setup_test.dart):
//
//   meshcore://team/join?n=<team channel>&s=<32 hex>[&w=<watched>,...]
//                       [&m=<MHz x 1000>,<bandwidth Hz>,<sf>,<cr>]
//
//   w: "%23name" for a hashtag channel, "name:<32 hex>" for a private one;
//      names percent-encoded (RFC 3986 unreserved kept).
//
// Over Sound link (140 bytes a message) a longer link goes as parts:
//   MCT:<id 4 hex>:<i>/<n>:<piece>
// See TEAM_SETUP.md.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace teamsetup {

constexpr const char* kPrefix = "meshcore://team/join?";
constexpr size_t kSoundBytes = 140;
constexpr int kMaxWatched = 8;
constexpr int kMaxParts = 4;
constexpr size_t kMaxLink = kMaxParts * (kSoundBytes - 15);

struct Channel {
  char name[33] = {0};
  bool hashtag = false;
  uint8_t secret[16] = {0};
};

struct Setup {
  Channel team;   // its name and secret (hashtag: false)
  int nw = 0;
  Channel watched[kMaxWatched];
  bool has_mm = false;
  uint32_t khz = 0, bw_hz = 0;
  uint8_t sf = 0, cr = 0;
};

inline int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
inline bool hex16(const char* s, size_t n, uint8_t out[16]) {
  if (n != 32) return false;
  for (int i = 0; i < 16; i++) {
    const int a = hexv(s[2 * i]), b = hexv(s[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    out[i] = (uint8_t)(a << 4 | b);
  }
  return true;
}

// Percent-encodes `s` into `out` (RFC 3986 unreserved kept). False if it won't fit.
inline bool enc(const char* s, char* out, size_t cap, size_t* n) {
  static const char* hx = "0123456789ABCDEF";
  for (; *s; s++) {
    const uint8_t b = (uint8_t)*s;
    const bool keep = (b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '-' ||
                      b == '_' || b == '.' || b == '~';
    if (*n + (keep ? 1 : 3) >= cap) return false;
    if (keep) out[(*n)++] = (char)b;
    else { out[(*n)++] = '%'; out[(*n)++] = hx[b >> 4]; out[(*n)++] = hx[b & 15]; }
  }
  out[*n] = 0;
  return true;
}
// Decodes [s, e) into out (cut to cap).
inline void dec(const char* s, const char* e, char* out, size_t cap) {
  size_t n = 0;
  while (s < e && n + 1 < cap) {
    if (*s == '%' && e - s >= 3 && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) {
      out[n++] = (char)(hexv(s[1]) << 4 | hexv(s[2]));
      s += 3;
    } else {
      out[n++] = *s++;
    }
  }
  out[n] = 0;
}

inline bool put(char* out, size_t cap, size_t* n, const char* s) {
  const size_t k = strlen(s);
  if (*n + k >= cap) return false;
  memcpy(out + *n, s, k + 1);
  *n += k;
  return true;
}
inline bool putHex(char* out, size_t cap, size_t* n, const uint8_t b[16]) {
  char h[33];
  for (int i = 0; i < 16; i++) snprintf(h + 2 * i, 3, "%02x", b[i]);
  return put(out, cap, n, h);
}

// The link (the QR code's text). 0 if it doesn't fit `cap`.
inline size_t toLink(const Setup& s, char* out, size_t cap) {
  size_t n = 0;
  if (!put(out, cap, &n, kPrefix) || !put(out, cap, &n, "n=") || !enc(s.team.name, out, cap, &n) ||
      !put(out, cap, &n, "&s=") || !putHex(out, cap, &n, s.team.secret))
    return 0;
  for (int i = 0; i < s.nw; i++) {
    if (!put(out, cap, &n, i == 0 ? "&w=" : ",") || !enc(s.watched[i].name, out, cap, &n)) return 0;
    if (!s.watched[i].hashtag && (!put(out, cap, &n, ":") || !putHex(out, cap, &n, s.watched[i].secret))) return 0;
  }
  if (s.has_mm) {
    char m[48];
    snprintf(m, sizeof m, "&m=%lu,%lu,%u,%u", (unsigned long)s.khz, (unsigned long)s.bw_hz, (unsigned)s.sf,
             (unsigned)s.cr);
    if (!put(out, cap, &n, m)) return 0;
  }
  return n;
}

inline bool isLink(const char* t) { return t && strncmp(t, kPrefix, strlen(kPrefix)) == 0; }

inline bool parse(const char* t, Setup& s) {
  if (!isLink(t)) return false;
  s = Setup();
  const char* p = t + strlen(kPrefix);
  bool have_n = false, have_s = false;
  while (*p) {
    const char* amp = strchr(p, '&');
    const char* end = amp ? amp : p + strlen(p);
    const char* eq = (const char*)memchr(p, '=', (size_t)(end - p));
    if (eq) {
      const char* v = eq + 1;
      const size_t kl = (size_t)(eq - p);
      if (kl == 1 && *p == 'n') {
        dec(v, end, s.team.name, sizeof s.team.name);
        have_n = s.team.name[0] != 0;
      } else if (kl == 1 && *p == 's') {
        have_s = hex16(v, (size_t)(end - v), s.team.secret);
      } else if (kl == 1 && *p == 'w') {
        const char* q = v;
        while (q < end && s.nw < kMaxWatched) {
          const char* comma = q;
          while (comma < end && *comma != ',') comma++;
          const char* colon = q;
          while (colon < comma && *colon != ':') colon++;
          Channel& c = s.watched[s.nw];
          c = Channel();
          if (colon == comma) {
            dec(q, comma, c.name, sizeof c.name);
            if (c.name[0] == '#' && c.name[1]) { c.hashtag = true; s.nw++; }
          } else {
            dec(q, colon, c.name, sizeof c.name);
            if (c.name[0] && hex16(colon + 1, (size_t)(comma - colon - 1), c.secret)) s.nw++;
          }
          q = comma + 1;
        }
      } else if (kl == 1 && *p == 'm') {
        char b[48];
        const size_t k = (size_t)(end - v) < sizeof b - 1 ? (size_t)(end - v) : sizeof b - 1;
        memcpy(b, v, k);
        b[k] = 0;
        unsigned long f = 0, bw = 0;
        unsigned sf = 0, cr = 0;
        if (sscanf(b, "%lu,%lu,%u,%u", &f, &bw, &sf, &cr) == 4 && f >= 137000 && f <= 1020000 && bw > 0 &&
            sf >= 5 && sf <= 12 && cr >= 5 && cr <= 8) {
          s.has_mm = true;
          s.khz = (uint32_t)f;
          s.bw_hz = (uint32_t)bw;
          s.sf = (uint8_t)sf;
          s.cr = (uint8_t)cr;
        }
      }
    }
    if (!amp) break;
    p = amp + 1;
  }
  return have_n && have_s;
}

inline uint32_t fnv32(const char* s) {
  uint32_t h = 0x811c9dc5u;
  for (; *s; s++) { h ^= (uint8_t)*s; h *= 0x01000193u; }
  return h;
}

// What to play: the link itself if it fits one message, else MCT: parts.
// Returns the number of parts (0: too long).
inline int soundParts(const char* link, char parts[kMaxParts][kSoundBytes + 1]) {
  const size_t len = strlen(link);
  if (len <= kSoundBytes) {
    memcpy(parts[0], link, len + 1);
    return 1;
  }
  const size_t room = kSoundBytes - 15;
  const int n = (int)((len + room - 1) / room);
  if (n > kMaxParts) return 0;
  const unsigned id = fnv32(link) & 0xFFFF;
  for (int i = 0; i < n; i++) {
    const size_t off = (size_t)i * room;
    const size_t k = len - off < room ? len - off : room;
    snprintf(parts[i], kSoundBytes + 1, "MCT:%04x:%d/%d:%.*s", id, i + 1, n, (int)k, link + off);
  }
  return n;
}

// Heard by Sound link: the link, or a part of one.
inline bool isPart(const char* t) {
  if (isLink(t)) return true;
  return t && strncmp(t, "MCT:", 4) == 0 && strlen(t) > 13 && t[8] == ':';
}

class Assembler {
 public:
  // True with `out` filled once `t` completes a setup.
  bool accept(const char* t, Setup& out) {
    if (isLink(t)) return parse(t, out);
    if (!isPart(t)) return false;
    char id[5];
    memcpy(id, t + 4, 4);
    id[4] = 0;
    char* x;
    const long i = strtol(t + 9, &x, 10);
    if (*x != '/') return false;
    const long n = strtol(x + 1, &x, 10);
    if (*x != ':' || n < 1 || n > kMaxParts || i < 1 || i > n) return false;
    if (strcmp(id, id_) != 0 || n != n_) {   // a new setup
      memcpy(id_, id, 5);
      n_ = (int)n;
      have_ = 0;
    }
    snprintf(part_[i - 1], sizeof part_[0], "%s", x + 1);
    have_ |= 1u << (i - 1);
    if (have_ != (1u << n_) - 1) return false;
    char link[kMaxLink + 1];
    link[0] = 0;
    for (int k = 0; k < n_; k++) strncat(link, part_[k], sizeof link - strlen(link) - 1);
    have_ = 0;
    id_[0] = 0;
    return parse(link, out);
  }
  int got() const { int c = 0; for (int k = 0; k < n_; k++) c += (have_ >> k) & 1; return c; }
  int of() const { return n_; }

 private:
  char id_[5] = {0};
  int n_ = 0;
  uint32_t have_ = 0;
  char part_[kMaxParts][kSoundBytes + 1];
};

}  // namespace teamsetup
