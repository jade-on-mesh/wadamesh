// SPDX-License-Identifier: GPL-3.0-or-later
//
// Quick replies (src/ui-touch/QuickReply.h, TeamWire::Reply): the golden
// bytes and report hashes MeshCore Open TAK's
// test/team/team_reply_wire_test.dart checks too.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui-touch/QuickReply.h"
#include "ui-touch/TeamWire.h"

using namespace quickreply;

static void hex(const uint8_t* b, size_t n, char* out) {
  for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

int main() {
  // Team reply golden bytes.
  TeamWire::Reply r;
  const uint8_t from[4] = {0xa1, 0xb2, 0xc3, 0xd4}, to[4] = {1, 2, 3, 4};
  memcpy(r.key4, from, 4);
  memcpy(r.to4, to, 4);
  r.ref = TeamWire::kRefAlert;
  r.ref_id = 7;
  r.code = kEta;
  r.arg = 10;
  strcpy(r.callsign, "Sam");
  uint8_t buf[64];
  const size_t n = TeamWire::encode(r, buf, sizeof buf);
  char h[160];
  hex(buf, n, h);
  assert(!strcmp(h, "51a1b2c3d4010203040100000007020a0353616d"));
  TeamWire::Reply b;
  assert(TeamWire::parse(buf, n, b));
  assert(b.ref == TeamWire::kRefAlert && b.ref_id == 7 && b.code == kEta && b.arg == 10 && !strcmp(b.callsign, "Sam"));
  buf[9] = 9;   // unknown kind
  assert(!TeamWire::parse(buf, n, b));

  // Report hashes, as the app computes them.
  assert(reportHash(1760000000, "IceBot", "\xF0\x9F\x94\xB4 CRITICAL / 10-03 11:30 / @Auburn") == 0x9aa8);
  assert(reportHash(1760000000, "IceBot", "ACTIVE / 10-03") == 0x477d);
  assert(reportHash(1760000000, "Sam", "hi") == 0x73e2);
  assert(reportHash(1760000000, "Sam", "abcd\xF0\x9F\x94\xB4x") == 0x01e8);   // cut inside a surrogate pair
  assert(reportHash(1760000000, "Zo\xC3\xAB", "D\xC3\xA9j\xC3\xA0 vu") == 0x0b16);

  // Watched-channel replies.
  char t[64];
  watchText(kOnMyWay, 0, 0x3fa1, t, sizeof t);
  assert(!strcmp(t, "On my way \xE2\x86\xA9" "3fa1"));
  watchText(kEta, 10, 0x3fa1, t, sizeof t);
  assert(!strcmp(t, "ETA 10 min \xE2\x86\xA9" "3fa1"));
  Parsed p;
  assert(parseWatch("On my way \xE2\x86\xA9" "3fa1", p) && p.hash == 0x3fa1 && p.code == kOnMyWay);
  assert(parseWatch("ETA 20 min \xE2\x86\xA9" "00ff", p) && p.code == kEta && p.arg == 20);
  assert(parseWatch("Heading over now \xE2\x86\xA9" "beef", p) && p.code == kCustom && !strcmp(p.said, "Heading over now"));
  assert(!parseWatch("no mark here", p));
  assert(!parseWatch("\xE2\x86\xA9" "3fa1", p));
  assert(!parseWatch("Seen it \xE2\x86\xA9" "3FA1", p));   // lower-case hex only, as the app

  // The book: one answer per person per item, oldest first.
  Book bk;
  char ref[28];
  refTeam(TeamWire::kRefAlert, to, 7, ref, sizeof ref);
  assert(!strcmp(ref, "alert:01020304:7"));
  bk.add(ref, "Sam", kOnMyWay, 0, nullptr, 100, false);
  bk.add(ref, "Lee", kCopy, 0, nullptr, 200, false);
  bk.add(ref, "Sam", kEta, 10, nullptr, 300, false);   // replaces Sam's
  const Entry* a[8];
  assert(bk.answers(ref, a, 8) == 2);
  assert(!strcmp(a[0]->who, "Lee") && a[1]->code == kEta);
  line(*a[1], t, sizeof t);
  assert(!strcmp(t, "Sam: ETA 10 min"));
  bk.add(ref, "Me", kCopy, 0, nullptr, 400, true);
  assert(bk.mine(ref) == kCopy);
  refWatch(3, 0x9aa8, ref, sizeof ref);
  assert(!strcmp(ref, "w:3:9aa8") && bk.answers(ref, a, 8) == 0);
  for (int i = 0; i < 40; i++) {   // full: the oldest go
    char who[8];
    snprintf(who, sizeof who, "p%d", i);
    bk.add(ref, who, kSeenIt, 0, nullptr, 1000 + i, false);
  }
  const Entry* all[Book::kMax];
  assert(bk.answers(ref, all, Book::kMax) == Book::kMax && !strcmp(all[0]->who, "p8"));
  refTeam(TeamWire::kRefAlert, to, 7, ref, sizeof ref);
  assert(bk.answers(ref, a, 8) == 0);   // the alert's answers were the oldest

  printf("quick reply: ok\n");
  return 0;
}
