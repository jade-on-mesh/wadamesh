// SPDX-License-Identifier: GPL-3.0-or-later
// MapHighlight.h: new map points flash for the chosen time (as the app's
// test/team/map_highlights_test.dart).
#include <assert.h>
#include <stdio.h>

#include "ui-touch/MapHighlight.h"

int main() {
  using namespace maphl;
  const uint32_t t0 = 0xFFFF0000UL;   // close to millis() wrapping
  const uint32_t min = 60000UL;
  {
    Table t;
    assert(t.minutes() == 10);
    uint32_t until = t0 + 60 * min;
    assert(t.isNew(key('m', "abcd", 4), t0, 0, &until));
    assert(until == t0 + 10 * min);
    assert(t.isNew(key('m', "abcd", 4), t0 + 10 * min - 1));
    assert(!t.isNew(key('m', "abcd", 4), t0 + 10 * min));
    assert(key('m', "abcd", 4) != key('p', "abcd", 4));
  }
  {
    // Already old when it first showed up (loaded after a reboot): not new.
    Table t;
    assert(!t.isNew(key('w', "x", 1), t0, 2 * 60 * min));
    // A few minutes of clock difference still counts as new.
    assert(t.isNew(key('w', "y", 1), t0, 12 * min));
    // Unknown age: new.
    assert(t.isNew(key('w', "z", 1), t0));
  }
  {
    // The setting: 1 minute, then off.
    Table t;
    t.choice = 1;
    assert(t.isNew(key('m', "a", 1), t0));
    assert(!t.isNew(key('m', "a", 1), t0 + 2 * min));
    t.choice = 0;
    assert(!t.isNew(key('m', "b", 1), t0));
  }
  {
    // Full: the point that appeared longest ago makes room.
    Table t;
    for (int i = 0; i < Table::kSlots; i++) t.isNew(key('p', &i, sizeof i), t0 + (uint32_t)i);
    const int fresh = 1000;
    assert(t.isNew(key('p', &fresh, sizeof fresh), t0 + 500));
    // Point 2 kept its slot: its ten minutes are up.
    const int third = 2;
    assert(!t.isNew(key('p', &third, sizeof third), t0 + 10 * min + 2));
    // Point 0 lost its slot, so it reads as appearing again now.
    const int first = 0;
    assert(t.isNew(key('p', &first, sizeof first), t0 + 10 * min + 3));
  }
  {
    // Noted at boot as already there: never flashes. Noted later: flashes
    // from when it was noted, not from when the map first drew it.
    Table t;
    t.note(key('m', "boot", 4), t0, kNeverNew);
    assert(!t.isNew(key('m', "boot", 4), t0 + 1000));
    t.note(key('m', "late", 4), t0);
    assert(t.isNew(key('m', "late", 4), t0 + 9 * min));
    assert(!t.isNew(key('m', "late", 4), t0 + 10 * min));
  }
  printf("map highlight: ok\n");
  return 0;
}
