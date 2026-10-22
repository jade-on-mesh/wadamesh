// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Quick replies (QUICK_REPLIES.md): one-tap answers tied to the thing they
// answer, the same as MeshCore Open TAK's (lib/team/team_reply_wire.dart).
//
//   A teammate's alert, report or pin: a team message, TeamWire::Reply (0x51).
//   A watched channel report (#icewatch...): plain text on that channel,
//     "On my way ↩3fa1", where 3fa1 is the report's reaction hash: the app's
//     ReactionHelper.computeReactionHash over the sender's time, name and the
//     first five UTF-16 units of the text, with the Dart VM's string hash.
//
// Who answered what is kept in a small Book (one answer per person per item).
// Pure logic, header-only, host-tested in test/test_quick_reply.cpp against
// the app's golden values.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace quickreply {

enum Code : uint8_t { kCopy, kOnMyWay, kEta, kCantHelp, kSeenIt, kNeedBackup, kAllClear, kCodes, kCustom = 0xFF };

inline const char* english(uint8_t c) {
  static const char* const n[kCodes] = {"Copy",    "On my way",   "ETA",      "Can't help",
                                        "Seen it", "Need backup", "All clear"};
  return c < kCodes ? n[c] : "";
}

// The answers offered, in order.
static const uint8_t kAlertCodes[] = {kCopy, kOnMyWay, kEta, kCantHelp};
static const uint8_t kReportCodes[] = {kSeenIt, kOnMyWay, kNeedBackup, kAllClear};
static const uint8_t kEtas[] = {5, 10, 20, 30};

// The words for an answer: "ETA 10 min", "On my way".
inline void words(uint8_t code, uint8_t arg, char* out, size_t cap) {
  if (code == kEta) snprintf(out, cap, "ETA %u min", (unsigned)arg);
  else snprintf(out, cap, "%s", english(code));
}

// ---- the report hash ------------------------------------------------------------------------
// The Dart VM's String.hashCode (one-at-a-time over UTF-16 units, 30 bits),
// as the app's ReactionHelper.stableStringHash, fed unit by unit.
struct DartHash {
  uint32_t h = 0;
  void unit(uint16_t u) {
    h += u;
    h += h << 10;
    h ^= h >> 6;
  }
  // UTF-8 in, UTF-16 units out; at most `max_units` of them (-1: all).
  // Returns the units fed.
  int utf8(const char* s, int max_units = -1) {
    int fed = 0;
    const uint8_t* p = (const uint8_t*)s;
    while (*p && (max_units < 0 || fed < max_units)) {
      uint32_t cp;
      int k;
      if (*p < 0x80) { cp = *p; k = 1; }
      else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; k = 2; }
      else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; k = 3; }
      else { cp = *p & 0x07; k = 4; }
      for (int i = 1; i < k; i++) {
        if ((p[i] & 0xC0) != 0x80) { k = i; cp = 0xFFFD; break; }
        cp = (cp << 6) | (p[i] & 0x3F);
      }
      p += k;
      if (cp >= 0x10000) {   // a surrogate pair, which the cut may split
        cp -= 0x10000;
        unit((uint16_t)(0xD800 | (cp >> 10)));
        fed++;
        if (max_units >= 0 && fed >= max_units) break;
        unit((uint16_t)(0xDC00 | (cp & 0x3FF)));
      } else {
        unit((uint16_t)cp);
      }
      fed++;
    }
    return fed;
  }
  uint32_t done() const {
    uint32_t x = h;
    x += x << 3;
    x ^= x >> 11;
    x += x << 15;
    x &= 0x3FFFFFFF;
    return x ? x : 1;
  }
};

// The app's reaction hash of a channel message: its time, sender and text.
inline uint16_t reportHash(uint32_t sent_epoch, const char* sender, const char* text) {
  DartHash d;
  char ts[12];
  snprintf(ts, sizeof ts, "%lu", (unsigned long)sent_epoch);
  d.utf8(ts);
  if (sender) d.utf8(sender);
  d.utf8(text ? text : "", 5);
  return (uint16_t)(d.done() & 0xFFFF);
}

// ---- watched-channel replies ------------------------------------------------------------------
// "↩" in UTF-8.
static constexpr const char* kMark = "\xE2\x86\xA9";

// "On my way ↩3fa1"
inline void watchText(uint8_t code, uint8_t arg, uint16_t hash, char* out, size_t cap) {
  char w[24];
  words(code, arg, w, sizeof w);
  snprintf(out, cap, "%s %s%04x", w, kMark, (unsigned)hash);
}

struct Parsed {
  uint16_t hash = 0;
  uint8_t code = kCustom;
  uint8_t arg = 0;
  char said[44] = {0};   // what it says, before the mark
};

inline bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

// A quick reply on a watched channel (the text after "Sender: "), or false.
inline bool parseWatch(const char* text, Parsed& out) {
  out = Parsed{};
  if (!text) return false;
  size_t n = strlen(text);
  while (n && (text[n - 1] == ' ' || text[n - 1] == '\r' || text[n - 1] == '\n')) n--;
  if (n < 7) return false;
  const char* h = text + n - 4;
  for (int i = 0; i < 4; i++)
    if (!isHex(h[i])) return false;
  if (memcmp(h - 3, kMark, 3) != 0) return false;
  size_t s = (size_t)(h - 3 - text);
  while (s && (text[s - 1] == ' ' || text[s - 1] == '\t')) s--;
  size_t b = 0;
  while (b < s && text[b] == ' ') b++;
  if (s <= b || s - b > 40) return false;
  memcpy(out.said, text + b, s - b);
  out.said[s - b] = 0;
  out.hash = (uint16_t)strtoul(h, nullptr, 16);
  unsigned m = 0;
  char tail[8] = {0};
  if (sscanf(out.said, "ETA %3u mi%7s", &m, tail) == 2 && !strcmp(tail, "n")) {
    out.code = kEta;
    out.arg = (uint8_t)(m > 255 ? 255 : m);
    return true;
  }
  for (uint8_t c = 0; c < kCodes; c++)
    if (c != kEta && !strcmp(out.said, english(c))) out.code = c;
  return true;
}

// ---- who answered what --------------------------------------------------------------------------
// Items are named as the app names them: "alert:a1b2c3d4:7", "report:...",
// "pin:...", and "w:<channel>:<hash>" for a watched report.
struct Entry {
  bool used = false;
  bool own = false;
  uint8_t code = kCustom;
  uint8_t arg = 0;
  uint32_t at_ms = 0;
  char ref[28] = {0};
  char who[21] = {0};
  char said[24] = {0};   // for an answer outside the known ones
};

inline void refTeam(uint8_t kind, const uint8_t key4[4], uint32_t id, char* out, size_t cap) {
  static const char* const k[4] = {"", "alert", "report", "pin"};
  snprintf(out, cap, "%s:%02x%02x%02x%02x:%lu", k[kind < 4 ? kind : 0], key4[0], key4[1], key4[2], key4[3],
           (unsigned long)id);
}
inline void refWatch(uint8_t channel, uint16_t hash, char* out, size_t cap) {
  snprintf(out, cap, "w:%u:%04x", (unsigned)channel, (unsigned)hash);
}

class Book {
 public:
  static constexpr int kMax = 32;
  Entry e[kMax];

  // Records an answer; a person's newer answer to the same item replaces theirs.
  void add(const char* ref, const char* who, uint8_t code, uint8_t arg, const char* said, uint32_t at_ms, bool own) {
    Entry* slot = nullptr;
    for (Entry& x : e)
      if (x.used && !strcmp(x.ref, ref) && !strcmp(x.who, who)) { slot = &x; break; }
    if (!slot)
      for (Entry& x : e)
        if (!x.used) { slot = &x; break; }
    if (!slot) {   // full: the oldest goes
      slot = &e[0];
      for (Entry& x : e)
        if ((int32_t)(x.at_ms - slot->at_ms) < 0) slot = &x;
    }
    *slot = Entry{};
    slot->used = true;
    slot->own = own;
    slot->code = code;
    slot->arg = arg;
    slot->at_ms = at_ms;
    snprintf(slot->ref, sizeof slot->ref, "%s", ref);
    snprintf(slot->who, sizeof slot->who, "%s", who);
    snprintf(slot->said, sizeof slot->said, "%s", said ? said : "");
  }

  // The answers to `ref`, oldest first; returns how many (at most `cap`).
  int answers(const char* ref, const Entry** out, int cap) const {
    int n = 0;
    for (const Entry& x : e)
      if (x.used && !strcmp(x.ref, ref) && n < cap) out[n++] = &x;
    for (int i = 1; i < n; i++)   // by time
      for (int j = i; j > 0 && (int32_t)(out[j]->at_ms - out[j - 1]->at_ms) < 0; j--) {
        const Entry* t = out[j];
        out[j] = out[j - 1];
        out[j - 1] = t;
      }
    return n;
  }

  // This radio's own answer to `ref`, or kCustom.
  uint8_t mine(const char* ref) const {
    for (const Entry& x : e)
      if (x.used && x.own && !strcmp(x.ref, ref)) return x.code;
    return kCustom;
  }
};

// "Sam: On my way", one answer's line.
inline void line(const Entry& x, char* out, size_t cap) {
  char w[24];
  if (x.code < kCodes) words(x.code, x.arg, w, sizeof w);
  else snprintf(w, sizeof w, "%s", x.said);
  snprintf(out, cap, "%s: %s", x.own ? "You" : x.who, w);
}

}  // namespace quickreply
