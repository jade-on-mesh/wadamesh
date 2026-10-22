// SPDX-License-Identifier: GPL-3.0-or-later
// One-scan team setup (src/ui-touch/TeamSetup.h) against the golden strings
// MeshCore Open TAK's test/team/team_setup_test.dart checks too.
//   g++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -I src test/test_team_setup.cpp -o /tmp/ts && /tmp/ts
#include <stdio.h>
#include <string.h>

#include "ui-touch/TeamSetup.h"

static int g_fail = 0;
#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      g_fail++;                                           \
    }                                                     \
  } while (0)

using namespace teamsetup;

static const char* kLink =
    "meshcore://team/join?n=Ops%20Team&s=00112233445566778899aabbccddeeff"
    "&w=%23icewatch,%23dronewatch,Hunt%20Camp:ffeeddccbbaa99887766554433221100"
    "&m=911150,250000,7,5";
static const char* kPart1 =
    "MCT:abdd:1/2:meshcore://team/join?n=Ops%20Team&s=00112233445566778899aabbccddeeff"
    "&w=%23icewatch,%23dronewatch,Hunt%20Camp:ffeeddccbbaa9988";
static const char* kPart2 = "MCT:abdd:2/2:7766554433221100&m=911150,250000,7,5";

int main() {
  Setup s;
  snprintf(s.team.name, sizeof s.team.name, "Ops Team");
  hex16("00112233445566778899aabbccddeeff", 32, s.team.secret);
  s.nw = 3;
  snprintf(s.watched[0].name, 33, "#icewatch");
  s.watched[0].hashtag = true;
  snprintf(s.watched[1].name, 33, "#dronewatch");
  s.watched[1].hashtag = true;
  snprintf(s.watched[2].name, 33, "Hunt Camp");
  hex16("ffeeddccbbaa99887766554433221100", 32, s.watched[2].secret);
  s.has_mm = true;
  s.khz = 911150;
  s.bw_hz = 250000;
  s.sf = 7;
  s.cr = 5;

  char link[kMaxLink + 1];
  CHECK(toLink(s, link, sizeof link) && !strcmp(link, kLink));

  Setup b;
  CHECK(parse(kLink, b));
  CHECK(!strcmp(b.team.name, "Ops Team") && !memcmp(b.team.secret, s.team.secret, 16));
  CHECK(b.nw == 3 && b.watched[0].hashtag && !strcmp(b.watched[1].name, "#dronewatch"));
  CHECK(!b.watched[2].hashtag && !strcmp(b.watched[2].name, "Hunt Camp") && b.watched[2].secret[0] == 0xff);
  CHECK(b.has_mm && b.khz == 911150 && b.bw_hz == 250000 && b.sf == 7 && b.cr == 5);

  char parts[kMaxParts][kSoundBytes + 1];
  CHECK(soundParts(kLink, parts) == 2);
  CHECK(!strcmp(parts[0], kPart1) && !strcmp(parts[1], kPart2));

  char shortLink[] = "meshcore://team/join?n=Ops&s=00112233445566778899aabbccddeeff";
  CHECK(soundParts(shortLink, parts) == 1 && !strcmp(parts[0], shortLink));

  Assembler* a = new Assembler();
  CHECK(!a->accept(kPart2, b));
  CHECK(a->got() == 1 && a->of() == 2);
  CHECK(a->accept(kPart1, b));
  CHECK(toLink(b, link, sizeof link) && !strcmp(link, kLink));
  CHECK(a->accept(shortLink, b) && !strcmp(b.team.name, "Ops") && b.nw == 0 && !b.has_mm);
  delete a;

  CHECK(!parse("meshcore://team/join?n=Ops&s=123", b));
  CHECK(parse("meshcore://team/join?n=Ops&s=00112233445566778899aabbccddeeff&w=notahashtag,ok:zz&m=1,2,3", b));
  CHECK(b.nw == 0 && !b.has_mm);
  CHECK(isPart(kPart1) && !isPart("U:Bob|hi"));

  if (g_fail) {
    printf("%d failure(s)\n", g_fail);
    return 1;
  }
  printf("team setup: all passed\n");
  return 0;
}
