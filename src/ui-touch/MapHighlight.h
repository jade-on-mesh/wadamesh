// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// New points on the map flash for a while (10 minutes by default), as in
// MeshCore Open TAK (lib/team/map_highlights.dart): a teammate appearing, a
// teammate's pin, a watched channel's report. Remembers when each point
// first appeared; a point already older than the highlight time (plus 5
// minutes for clock differences) when it first shows up doesn't flash.
//
// Pure C++ on a millis() clock, so it is host-tested
// (test/test_map_highlight.cpp).

#include <stddef.h>
#include <stdint.h>

namespace maphl {

constexpr uint8_t kChoicesMin[] = {0, 1, 5, 10, 15, 30, 60, 120};
constexpr int kChoices = sizeof kChoicesMin / sizeof kChoicesMin[0];
constexpr uint8_t kDefaultChoice = 3;   // 10 min
constexpr uint32_t kSlackMs = 5UL * 60000UL;
constexpr uint32_t kUnknownAge = 0xFFFFFFFFUL;
constexpr uint32_t kNeverNew = 0xFFFFFFFEUL;   // there before we looked (at boot)

// FNV-1a over a kind letter and some bytes: a point's key.
inline uint32_t key(char kind, const void* data, size_t n, uint32_t h = 2166136261UL) {
  h = (h ^ (uint8_t)kind) * 16777619UL;
  const uint8_t* p = (const uint8_t*)data;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619UL;
  return h;
}

class Table {
 public:
  static constexpr int kSlots = 96;
  uint8_t choice = kDefaultChoice;

  uint32_t minutes() const { return kChoicesMin[choice % kChoices]; }
  uint32_t durationMs() const { return minutes() * 60000UL; }

  // Whether point `k` flashes at `now`. The first call for a key is when it
  // appeared; `age_ms` is how old the point already was then, if known.
  // `until`, when given, is lowered to the time this highlight runs out.
  bool isNew(uint32_t k, uint32_t now, uint32_t age_ms = kUnknownAge, uint32_t* until = nullptr) {
    Slot* s = find(k);
    if (!s) s = claim(k, now, age_ms);
    const uint32_t d = durationMs();
    if (!d) return false;
    if (s->age != kUnknownAge && s->age > d + kSlackMs) return false;
    const uint32_t end = s->seen + d;
    if ((int32_t)(now - end) >= 0) return false;
    if (until && (int32_t)(end - *until) < 0) *until = end;
    return true;
  }

  // Notes that point `k` is there (the first note is when it appeared).
  void note(uint32_t k, uint32_t now, uint32_t age_ms = kUnknownAge) {
    if (!find(k)) claim(k, now, age_ms);
  }

  void clear() {
    for (Slot& s : slots_) s.used = false;
  }

 private:
  struct Slot {
    bool used = false;
    uint32_t key = 0, seen = 0, age = kUnknownAge;
  };
  Slot slots_[kSlots];

  Slot* find(uint32_t k) {
    for (Slot& s : slots_) if (s.used && s.key == k) return &s;
    return nullptr;
  }
  // A free slot, else the one that appeared longest ago.
  Slot* claim(uint32_t k, uint32_t now, uint32_t age) {
    Slot* pick = nullptr;
    for (Slot& s : slots_) {
      if (!s.used) { pick = &s; break; }
      if (!pick || (int32_t)(s.seen - pick->seen) < 0) pick = &s;
    }
    pick->used = true;
    pick->key = k;
    pick->seen = now;
    pick->age = age;
    return pick;
  }
};

}  // namespace maphl
