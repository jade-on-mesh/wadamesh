// SPDX-License-Identifier: GPL-3.0-or-later
// Watched channels on the map: WatchFeed.h against the messages the bots post
// (the same samples MeshCore Open TAK's watch_feed test uses).
//
//   g++ -std=c++17 -Wall -I src test/test_watch_feed.cpp -o /tmp/w && /tmp/w
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui-touch/WatchFeed.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b))) { fprintf(stderr, "FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); g_fail++; } } while (0)
static bool near(double a, double b) { return fabs(a - b) < 1e-6; }

using namespace watchfeed;

static void iceBot() {
  const char* raw = "IceBot: \xF0\x9F\x9F\xA2 OBSERVED\n10-12 15:00\n@Portland NE Killingsworth St\n1-\xF0\x9F\xA5\xBE\n"
                    "\xF0\x9F\x93\x8D 45.5625,-122.6108";
  char sender[32];
  const char* t = messageText(raw, sender, sizeof sender);
  CHECK_STR(sender, "IceBot");
  Report r;
  CHECK(parse(t, r));
  CHECK(near(r.lat, 45.5625) && near(r.lon, -122.6108));
  CHECK(!r.has2);
  CHECK(r.level == kObserved);
  CHECK(!r.track[0]);
  CHECK_STR(r.title, "Portland NE Killingsworth St");
  CHECK_STR(r.body, "OBSERVED \xC2\xB7 10-12 15:00 \xC2\xB7 1-\xF0\x9F\xA5\xBE");
}

static void iceBotLinkOnly() {
  Report r;
  CHECK(!parse("https://iceout.org/en/reportInfo/154209", r));
  CHECK(!parse("a drone overhead near the park", r));
  CHECK(!r.track[0]);
}

static void drone() {
  const char* t = "DRONE 1581F9DEV26590296G6G h251m 1m/s https://maps.google.com/?q=47.60378,-122.34052 "
                  "Op https://maps.google.com/?q=47.60531,-122.34016";
  Report r;
  CHECK(parse(t, r));
  CHECK_STR(r.track, "1581F9DEV26590296G6G");
  CHECK_STR(r.detail, "h251m 1m/s");
  CHECK(near(r.lat, 47.60378) && near(r.lon, -122.34052));
  CHECK(r.has2 && near(r.lat2, 47.60531) && near(r.lon2, -122.34016));
  CHECK_STR(r.label2, "Op");
  CHECK(!r.has_bearing);
  CHECK(r.level == kActive);
  CHECK_STR(r.title, "DRONE 1581F9DE");
  char lbl[64];
  droneLabel(r, false, 0, lbl, sizeof lbl);
  CHECK_STR(lbl, "DRONE 1581F9DE h251m 1m/s");
  droneLabel(r, true, 44.6f, lbl, sizeof lbl);
  CHECK_STR(lbl, "DRONE 1581F9DE h251m 1m/s 045\xC2\xB0 NE");
}

static void droneVariants() {
  Report r;
  // after an emoji and a bot prefix, with a heading
  CHECK(parse("\xF0\x9F\x9B\xB8 Detected drone: AB-12CD34 h80m 4.5m/s hdg 270 47.1,-122.0 x 47.1234,-122.1234", r));
  CHECK_STR(r.track, "AB-12CD34");
  CHECK_STR(r.detail, "h80m 4.5m/s");
  CHECK(r.has_bearing && near(r.bearing, 270));
  CHECK(near(r.lat, 47.1234));   // "47.1" has too few decimals to be a position
  char lbl[64];
  droneLabel(r, false, 0, lbl, sizeof lbl);
  CHECK_STR(lbl, "DRONE AB12CD34 h80m 4.5m/s 270\xC2\xB0 W");
  // a degree sign
  CHECK(parse("DRONE XYZ123456 12\xC2\xB0 45.0001,-120.0001", r));
  CHECK(r.has_bearing && near(r.bearing, 12));
  // not a serial: no digit, or "drones" (a longer word)
  CHECK(parse("DRONE ABCDEFGH 45.0001,-120.0001", r) && !r.track[0]);
  CHECK(parse("drones 123456 45.0001,-120.0001", r) && !r.track[0]);
}

static void markerAndLevels() {
  Report r;
  CHECK(parse("Rally point m:45.123456,-122.654321|label", r));
  CHECK(near(r.lat, 45.123456) && near(r.lon, -122.654321));
  CHECK(parse("\xF0\x9F\x94\xB4 CRITICAL\n@Auburn E Main St\n47.3073,-122.2285", r));
  CHECK(r.level == kCritical);
  CHECK_STR(r.title, "Auburn E Main St");
  CHECK_STR(r.body, "CRITICAL");
  CHECK(parse("URGENT checkpoint at 45.50001, -122.60001", r));
  CHECK(r.level == kCritical);
  CHECK_STR(r.title, "URGENT checkpoint at 45.50001, -122.60001");
  CHECK(!parse("bad 95.0000,10.0000", r));   // not a latitude
}

static void store() {
  Store s;
  CHECK(s.ttlMs() == 6UL * 3600 * 1000);
  Report a, b;
  parse("DRONE 1581F9DEV26590296G6G h251m 1m/s q=47.60378,-122.34052", a);
  parse("DRONE 1581F9DEV26590296G6G h260m 3m/s q=47.60478,-122.34052", b);   // ~111 m north
  Item* x = s.add(a, 2, 1000);
  CHECK(x && !x->has_course);
  Item* y = s.add(b, 2, 5000);
  CHECK(y == x && y->has_course && (y->course < 1 || y->course > 359));
  CHECK(s.liveCount(6000) == 1);
  CHECK(s.add(a, 2, 3000) == nullptr);   // older than what is on the map
  char lbl[64];
  droneLabel(y->r, y->has_course, y->course, lbl, sizeof lbl);
  CHECK_STR(lbl, "DRONE 1581F9DE h260m 3m/s 000\xC2\xB0 N");
  // expiry
  CHECK(s.live(*y, 5000 + 6UL * 3600 * 1000 - 1));
  CHECK(!s.live(*y, 5000 + 6UL * 3600 * 1000));
  s.ttl = 8;   // always
  CHECK(s.live(*y, 4000000000UL));
  // full: the oldest goes
  Store f;
  Report r;
  parse("x 45.0001,-120.0001", r);
  for (int i = 0; i < kMaxItems; i++) f.add(r, 0, 100 + (uint32_t)i);
  Item* z = f.add(r, 1, 900);
  CHECK(z == &f.items[0]);
  // settings
  CHECK(s.setWatched("#icewatch", true) && s.setWatched("#dronewatch", true));
  CHECK(s.isWatched("#icewatch") && s.watchedCount() == 2);
  char buf[300];
  s.ttl = 2;
  s.save(buf, sizeof buf);
  CHECK_STR(buf, "2|#icewatch|#dronewatch");
  Store t;
  t.load(buf);
  CHECK(t.ttl == 2 && t.isWatched("#dronewatch") && t.isWatched("#icewatch") && !t.isWatched("public"));
  t.setWatched("#icewatch", false);
  CHECK(!t.isWatched("#icewatch") && t.watchedCount() == 1);
  ttlLabel(4, buf, sizeof buf);
  CHECK_STR(buf, "6 h");
  ttlLabel(7, buf, sizeof buf);
  CHECK_STR(buf, "3 days");
}

static void iceTag() {
  CHECK(isIce("#icewatch", "Somebody"));
  CHECK(isIce("#ibtest", "IceBot"));
  CHECK(!isIce("#dronewatch", "DARS-C6L"));
  Report r;
  parse("\xF0\x9F\x9F\xA2 OBSERVED\n@Portland NE Killingsworth St\n45.5625,-122.6108", r);
  tagIce(r);
  CHECK_STR(r.title, "ICE Portland NE Killingsworth St");
}

static bool posOf(const char* t, double la, double lo) {
  Report r;
  return parse(t, r) && fabs(r.lat - la) < 1e-4 && fabs(r.lon - lo) < 1e-4;
}

static void quakesAndFormats() {
  Report r;
  CHECK(parse("Earthquake M3.9 mw | 24 km ESE of Castle Rock, Washington | 11:09:28 UTC | depth 9 km | "
              "46.22N 122.60W", r));
  CHECK(r.quake_m10 == 39 && r.level == kOther);
  CHECK(near(r.lat, 46.22) && near(r.lon, -122.60));
  CHECK_STR(r.title, "QUAKE M3.9 Castle Rock");
  CHECK_STR(r.body, "24 km ESE of Castle Rock, Washington \xC2\xB7 depth 9 km \xC2\xB7 11:09:28 UTC");
  CHECK(parse("Earthquake M5.2 ml | 5 km WNW of Wauna, Washington | 21:05:56 UTC | depth 26 km | 47.40N 122.71W", r));
  CHECK(r.level == kCritical);
  CHECK_STR(r.title, "QUAKE M5.2 Wauna");

  CHECK(posOf("seen at 46.22N 122.60W", 46.22, -122.60));
  CHECK(posOf("fire 33.9\xC2\xB0 S, 18.4\xC2\xB0 E", -33.9, 18.4));
  CHECK(posOf("N47.6 W122.3 smoke", 47.6, -122.3));
  CHECK(posOf("lat: 45.52 lon: -122.68", 45.52, -122.68));
  CHECK(posOf("Latitude=45.5 Longitude=-122.6", 45.5, -122.6));
  CHECK(posOf("pos 45.5625 -122.6108", 45.5625, -122.6108));
  CHECK(posOf("45\xC2\xB0" "31'30\"N 122\xC2\xB0" "40'30\"W", 45.525, -122.675));
  CHECK(posOf("45\xC2\xB0 31.5' N, 122\xC2\xB0 40.5' W", 45.525, -122.675));
  // Maidenhead: CN85 is 45-46N 124-122W; CN85pm is one subsquare of it.
  CHECK(posOf("net control CN85pm", 45.5208, -122.7083));
  CHECK(posOf("grid CN85", 45.5, -123.0));
  CHECK(posOf("cn85pm42", 45.5104, -122.7125));
  CHECK(!parse("M3.9 at 11:09 UTC, 5 km away", r));
  CHECK(!parse("Room 5 Steps north", r));
  CHECK(!parse("version 1.2.3 build 45", r));
  CHECK(!parse("the AB12 bus", r));   // a 4-character square needs a cue
}

int main() {
  quakesAndFormats();
  iceTag();
  iceBot();
  iceBotLinkOnly();
  drone();
  droneVariants();
  markerAndLevels();
  store();
  if (g_fail) { fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
  printf("watch feed: all checks passed\n");
  return 0;
}
