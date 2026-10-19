// SPDX-License-Identifier: GPL-3.0-or-later
// Flock You's pure logic (src/ui-touch/FlockCore.h): the Wi-Fi and BLE
// signatures, the detection table, the log and camera files, the channel alert
// (read back through WatchFeed.h, as a radio watching the channel would) and
// the send limit.
//
//   g++ -std=c++17 -Wall -Wextra -I src test/test_flock_core.cpp -o /tmp/f && /tmp/f
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>   // POSIX flock(2): the namespace must not clash with it, as on the ESP32 toolchain

#include <vector>

#include "ui-touch/FlockCore.h"
#include "ui-touch/WatchFeed.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b))) { fprintf(stderr, "FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); g_fail++; } } while (0)

using namespace flockyou;

// A probe request as a Flock camera sends it, from `src`, with the FCS on the end.
static std::vector<uint8_t> probe(const uint8_t src[6], bool with_fcs, bool break_sig = false) {
  std::vector<uint8_t> f = {0x40, 0x00, 0x00, 0x00};                  // probe request, duration
  for (int i = 0; i < 6; i++) f.push_back(0xff);                     // addr1 broadcast
  for (int i = 0; i < 6; i++) f.push_back(src[i]);                   // addr2
  for (int i = 0; i < 6; i++) f.push_back(0xff);                     // addr3
  f.push_back(0x10); f.push_back(0x00);                               // seq
  const uint8_t ies[] = {
      0x00, 0x00,                                                     // wildcard SSID
      0x01, 0x04, 0x02, 0x04, 0x0b, 0x16,                             // rates
      0x02, 0x01, 0x05,                                               // tag 2
      0x0c, 0x01, 0x00,                                               // tag 12
      0x7f, 0x02, 0x00, 0x00,                                         // ext caps
      0xdd, 0x07, 0x50, 0x6f, 0x9a, 0x16, 0x03, 0x01, 0x03,           // WFA
      0xdd, 0x08, 0x00, 0x50, 0xf2, 0x08, 0x00, 0x00, 0x00, 0x00,     // MS WPS
  };
  f.insert(f.end(), ies, ies + sizeof ies);
  if (break_sig) f[f.size() - 5] = 0x09;                               // a different MS IE
  if (with_fcs) { f.push_back(0xde); f.push_back(0xad); f.push_back(0xbe); f.push_back(0xef); }
  return f;
}

static void wifi() {
  const uint8_t fmac[6] = {0x70, 0xc9, 0x4e, 0x12, 0x34, 0x56};
  Alert a;
  auto f = probe(fmac, true);
  CHECK(parseWifiFrame(f.data(), (int)f.size(), -61, 6, a));
  CHECK_STR(a.mac, "70:c9:4e:12:34:56");
  CHECK(a.method == Method::WifiProbe && a.rssi == -61 && a.channel == 6);
  CHECK(confidence(a.method) == kConfHigh);
  f = probe(fmac, false);
  CHECK(parseWifiFrame(f.data(), (int)f.size(), -70, 1, a));
  // Another vendor on the same module: the IEs differ.
  f = probe(fmac, true, true);
  CHECK(!parseWifiFrame(f.data(), (int)f.size(), -61, 6, a));
  // Not a Flock OUI; a locally administered address (the old 82:6b:f2 entry).
  const uint8_t other[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
  f = probe(other, true);
  CHECK(!parseWifiFrame(f.data(), (int)f.size(), -61, 6, a));
  const uint8_t la[6] = {0x82, 0x6b, 0xf2, 0x00, 0x00, 0x01};
  CHECK(!wifiOui(la));
  // A beacon from a Flock OUI is not a probe request.
  f = probe(fmac, true);
  f[0] = 0x80;
  CHECK(!parseWifiFrame(f.data(), (int)f.size(), -61, 6, a));
  // Truncated frames do not read past the end.
  for (size_t n = 0; n < 40; n++) CHECK(!parseWifiFrame(f.data(), (int)n, -61, 6, a));
}

static void ble() {
  BleAdv a;
  const uint8_t rnd[6] = {0xd3, 0x11, 0x22, 0x33, 0x44, 0x55};
  // Raven: 16-bit list with Device Information (which alone means nothing) and two of its services.
  const uint8_t raven[] = {0x02, 0x01, 0x06, 0x07, 0x03, 0x0a, 0x18, 0x00, 0x31, 0x00, 0x32};
  parseAdv(raven, sizeof raven, a);
  CHECK(a.n16 == 3);
  CHECK(classifyBle(rnd, false, a) == Method::BleRaven);
  // The same services as full 128-bit UUIDs.
  uint8_t r128[2 + 1 + 32] = {0x21, 0x07};
  const uint8_t base[16] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00};
  memcpy(r128 + 2, base, 16);
  memcpy(r128 + 18, base, 16);
  r128[18 + 13] = 0x34;
  parseAdv(r128, sizeof r128 - 1, a);
  CHECK(a.n16 == 2 && a.uuid16[0] == 0x3300 && a.uuid16[1] == 0x3400);
  CHECK(classifyBle(rnd, false, a) == Method::BleRaven);
  // Standard SIG services alone (a heart-rate strap, a thermometer): nothing.
  const uint8_t sig[] = {0x07, 0x03, 0x0a, 0x18, 0x09, 0x18, 0x19, 0x18};
  parseAdv(sig, sizeof sig, a);
  CHECK(classifyBle(rnd, false, a) == Method::None);
  // One Raven service: medium.
  const uint8_t one[] = {0x03, 0x03, 0x00, 0x35};
  parseAdv(one, sizeof one, a);
  CHECK(classifyBle(rnd, false, a) == Method::BleRavenOne);
  // Names.
  const uint8_t fs[] = {0x0f, 0x09, 'F', 'S', ' ', 'E', 'x', 't', ' ', 'B', 'a', 't', 't', 'e', 'r', 'y'};
  parseAdv(fs, sizeof fs, a);
  CHECK_STR(a.name, "FS Ext Battery");
  CHECK(classifyBle(rnd, false, a) == Method::BleName);
  const uint8_t pg[] = {0x08, 0x09, 'P', 'e', 'n', 'g', 'u', 'i', 'n'};
  parseAdv(pg, sizeof pg, a);
  CHECK(classifyBle(rnd, false, a) == Method::BleNameWeak);
  // XUNTONG manufacturer data.
  const uint8_t mfr[] = {0x05, 0xff, 0xc8, 0x09, 0x01, 0x02};
  parseAdv(mfr, sizeof mfr, a);
  CHECK(a.has_mfr && a.mfr_id == 0x09c8);
  CHECK(classifyBle(rnd, false, a) == Method::BleMfr);
  // OUI only counts for a public address.
  const uint8_t empty[] = {0x02, 0x01, 0x06};
  parseAdv(empty, sizeof empty, a);
  const uint8_t pub[6] = {0x58, 0x8e, 0x81, 0x01, 0x02, 0x03};
  CHECK(classifyBle(pub, true, a) == Method::BleMac);
  CHECK(classifyBle(pub, false, a) == Method::None);
  const uint8_t cc[6] = {0xcc, 0xcc, 0xcc, 0x01, 0x02, 0x03};
  CHECK(classifyBle(cc, true, a) == Method::None);
  // Broken AD structures stop cleanly.
  const uint8_t bad[] = {0x09, 0x09, 'a', 'b'};
  parseAdv(bad, sizeof bad, a);
  CHECK(!a.name[0]);
}

static Alert mk(const char* mac, Method m = Method::WifiProbe) {
  Alert a;
  a.method = m;
  a.source = m == Method::WifiProbe ? Source::Wifi : Source::Ble;
  snprintf(a.mac, sizeof a.mac, "%s", mac);
  a.rssi = -60;
  return a;
}

static void table() {
  Table<4> t;
  bool fresh;
  t.hear(mk("00:00:00:00:00:01"), 100, 0, &fresh);
  CHECK(fresh && t.n == 1);
  Detection& d = t.hear(mk("00:00:00:00:00:01"), 200, 1750000000, &fresh);
  CHECK(!fresh && d.count == 2 && d.last_ms == 200 && d.epoch == 1750000000);
  // A weaker rule later keeps the stronger one; a name that drops out stays.
  Alert b = mk("aa:aa:aa:aa:aa:aa", Method::BleName);
  snprintf(b.name, sizeof b.name, "FS Ext Battery");
  t.hear(b, 300, 0, &fresh);
  Alert b2 = mk("aa:aa:aa:aa:aa:aa", Method::BleMac);
  Detection& db = t.hear(b2, 400, 0, &fresh);
  CHECK(db.alert.method == Method::BleName);
  CHECK_STR(db.alert.name, "FS Ext Battery");
  // The 160-row bug: a full table of history must still take new devices.
  Table<4> h;
  for (int i = 0; i < 10; i++) {
    Detection r;
    r.alert = mk("00:00:00:00:00:00");
    snprintf(r.alert.mac, sizeof r.alert.mac, "00:00:00:00:00:%02d", i);
    r.epoch = 1750000000 + i;
    h.load(r);
  }
  CHECK(h.n == 4);
  CHECK_STR(h.rows[0].alert.mac, "00:00:00:00:00:06");   // the newest four, oldest first
  CHECK_STR(h.rows[3].alert.mac, "00:00:00:00:00:09");
  h.hear(mk("11:11:11:11:11:11"), 10, 0, &fresh);
  CHECK(fresh && h.n == 4 && h.find(mk("11:11:11:11:11:11")) == 3);
  CHECK(h.find(mk("00:00:00:00:00:06")) < 0);   // the oldest history made room
  // Live rows only: the one heard longest ago goes.
  Table<2> l;
  l.hear(mk("00:00:00:00:00:01"), 10, 0, &fresh);
  l.hear(mk("00:00:00:00:00:02"), 20, 0, &fresh);
  l.hear(mk("00:00:00:00:00:01"), 30, 0, &fresh);
  l.hear(mk("00:00:00:00:00:03"), 40, 0, &fresh);
  CHECK(l.find(mk("00:00:00:00:00:02")) < 0 && l.find(mk("00:00:00:00:00:01")) >= 0);
  // Heard again after a reboot: live again, counted from 1, not sent yet.
  Table<4> r;
  Detection old;
  old.alert = mk("00:00:00:00:00:07");
  old.count = 9;
  old.sent = true;
  r.load(old);
  Detection& again = r.hear(mk("00:00:00:00:00:07"), 5, 0, &fresh);
  CHECK(!fresh && !again.historical && again.count == 1 && !again.sent);
  r.clearLive();
  CHECK(r.n == 0);
}

static void logRows() {
  Detection d;
  d.alert = mk("70:c9:4e:12:34:56");
  d.alert.channel = 6;
  d.epoch = 1760900000;
  d.has_gps = true;
  d.lat = 47.603781;
  d.lon = -122.340521;
  char line[200];
  formatLogRow(d, line, sizeof line);
  CHECK_STR(line, "1760900000,wifi,70:c9:4e:12:34:56,,wifi_probe_ie,-60,6,47.603781,-122.340521");
  Detection back;
  CHECK(parseLogRow(line, back));
  CHECK(back.alert.method == Method::WifiProbe && back.has_gps && fabs(back.lat - 47.603781) < 1e-9);
  CHECK(back.epoch == 1760900000);
  // A name with a comma cannot break the columns.
  Detection n = d;
  n.alert = mk("aa:bb:cc:dd:ee:ff", Method::BleName);
  snprintf(n.alert.name, sizeof n.alert.name, "Flock, Inc\n");
  n.has_gps = false;
  formatLogRow(n, line, sizeof line);
  CHECK(parseLogRow(line, back));
  CHECK_STR(back.alert.name, "Flock_ Inc_");
  CHECK(!back.has_gps);
  // A 1.2 row (uptime in the first column) and the header.
  CHECK(parseLogRow("123456,ble,AA:BB:CC:DD:EE:FF,penguin,ble_name,-70,0,,", back));
  CHECK(back.epoch == 0 && !strcmp(back.alert.mac, "aa:bb:cc:dd:ee:ff"));
  CHECK(!parseLogRow("uptime_ms,source,mac,name,method,rssi,channel,latitude,longitude", back));
  CHECK(!parseLogRow(kLogHeader, back));
  CHECK(!parseLogRow("1,2,3", back));
}

static void cameras() {
  Cam buf[8];
  CamDb db;
  db.attach(buf, 8);
  Cam c;
  CHECK(!parseCamLine("lat,lon,direction,label", c));
  CHECK(!parseCamLine("# exported", c));
  CHECK(!parseCamLine("0,0", c));
  CHECK(!parseCamLine("91,10", c));
  CHECK(parseCamLine("47.6100,-122.3400,270,Flock", c) && c.dir == 270);
  db.add(c);
  CHECK(parseCamLine("47.6200,-122.3400", c) && c.dir == -1);
  db.add(c);
  CHECK(parseCamLine("47.6300,-122.3400,,x", c) && c.dir == -1);
  db.add(c);
  double dm = 0;
  CHECK(db.nearest(47.6105, -122.3400, 100, &dm) == 0 && dm > 50 && dm < 60);
  CHECK(db.nearest(47.6150, -122.3400, 100, &dm) < 0);
  CHECK(db.nearest(47.6150, -122.3400, 600, &dm) >= 0);
  char line[80];
  formatCamLine(buf[0], "mine", line, sizeof line);
  CHECK_STR(line, "47.610000,-122.340000,270,mine");
  // Facing: a camera looking west sees someone west of it.
  CHECK(facing(270, 265) && !facing(270, 90) && facing(-1, 90));
}

static void channelAlert() {
  Detection d;
  d.alert = mk("70:c9:4e:12:34:56");
  d.alert.rssi = -61;
  d.has_gps = true;
  d.lat = 47.603781;
  d.lon = -122.340521;
  char text[160];
  formatChannelAlert(d, text, sizeof text);
  // What another radio watching the channel makes of it (with the sender prefix).
  char heard[200];
  snprintf(heard, sizeof heard, "T-Deck Jade: %s", text);
  char sender[32];
  watchfeed::Report r;
  CHECK(watchfeed::parse(watchfeed::messageText(heard, sender, sizeof sender), r));
  CHECK_STR(sender, "T-Deck Jade");
  CHECK(fabs(r.lat - 47.60378) < 1e-6 && fabs(r.lon + 122.34052) < 1e-6);
  CHECK(r.level == watchfeed::kCritical);
  CHECK_STR(r.title, "ALERT Flock ALPR");
  CHECK(strstr(r.body, "Wi-Fi probe") != nullptr);
  CHECK(!r.track[0]);
  // Raven reads as such; no fix means no position line (and no map point).
  d.alert = mk("aa:bb:cc:dd:ee:ff", Method::BleRaven);
  d.has_gps = false;
  formatChannelAlert(d, text, sizeof text);
  CHECK(strstr(text, "Raven sensor") && !strstr(text, "\xF0\x9F\x93\x8D"));
  CHECK(!watchfeed::parse(text, r));
  // A marked camera.
  formatMarkedAlert(47.61, -122.34, 90, text, sizeof text);
  CHECK(watchfeed::parse(text, r));
  CHECK(r.level == watchfeed::kObserved);
  CHECK(strstr(r.title, "Flock camera") != nullptr);
  CHECK(strlen(text) < 140);
}

static void gate() {
  SendGate g;
  g.gap_ms = 1000;
  g.repeat_ms = 10000;
  CHECK(g.allow("a", 0));
  g.sent("a", 0);
  CHECK(!g.allow("b", 500));     // too soon after any post
  CHECK(g.allow("b", 1500));
  CHECK(!g.allow("a", 1500));    // same device within the repeat window
  CHECK(g.allow("a", 10001));
  // Wraps with millis().
  SendGate w;
  w.gap_ms = 1000;
  w.sent("x", 0xFFFFFF00u);
  CHECK(!w.allow("y", 0x00000010u));
  CHECK(w.allow("y", 0x00000400u));
}

static void settings() {
  Settings s;
  s.armed = true;
  s.share = true;
  s.radius = 3;
  s.share_conf = kConfMed;
  snprintf(s.channel, sizeof s.channel, "#flockwatch");
  char t[160];
  saveSettings(s, t, sizeof t);
  Settings b;
  loadSettings(t, b);
  CHECK(b.armed && b.share && b.radius == 3 && b.share_conf == kConfMed && b.show_cams && b.proximity);
  CHECK_STR(b.channel, "#flockwatch");
  loadSettings("r=9;q=7;junk", b);
  CHECK(b.radius == 1 && b.share_conf == kConfHigh && !b.armed);
}

int main() {
  wifi();
  ble();
  table();
  logRows();
  cameras();
  channelAlert();
  gate();
  settings();
  if (g_fail) { fprintf(stderr, "%d failure(s)\n", g_fail); return 1; }
  printf("flock core: all passed\n");
  return 0;
}
