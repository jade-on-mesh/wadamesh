// SPDX-License-Identifier: GPL-3.0-or-later
// Team awareness: TeamWire.h against golden bytes that MeshCore Open TAK's own
// test (test/team/team_wire_golden_test.dart) checks its encoder against, and
// TeamCore's rules (beacons, alert repeats, pin merge) on a simulated clock.
//
//   g++ -std=c++17 -I src test/test_team_wire.cpp src/ui-touch/TeamCore.cpp -o /tmp/t && /tmp/t
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "ui-touch/TeamCore.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::string hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
  return s;
}
static std::vector<uint8_t> unhex(const std::string& h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)strtol(h.substr(i, 2).c_str(), nullptr, 16));
  return v;
}
static void key(uint8_t* k, uint32_t v) { k[0] = v >> 24; k[1] = v >> 16; k[2] = v >> 8; k[3] = v; }

// Golden payloads. The same objects are built in the app's Dart test, which
// must produce exactly these bytes.
static const char* kP1 =
    "5001020304071c60218fb7159b54003803c0084d2501084a61646520e29ca8";
static const char* kP2 =
    "500102030418ebd0073b5a20b51b0000000000ff8b0507416c7068612d31";
static const char* kA1 =
    "53aabbccddbeef131c4fecc0b72221600c05427261766f174e65656420776174657220617420746865207269646765";
static const char* kA2 =
    "53aabbccdd0001000000000000000000000000";
static const char* kM1 =
    "4d0a0b0c0d1234567800041bed17f8b76cda4868eb2c5008526f636b66616c6c1d547261696c20626c6f636b65642070617374207468652062726964676500";
static const char* kM2 =
    "4d0a0b0c0d000000ff0109000000000000000000000001204142434445464748494a4b4c4d4e4f505152535455565758595a3031323334350028deadbeef0c0102";

static void golden() {
  uint8_t buf[200];
  TeamWire::Position p1;
  key(p1.key4, 0x01020304);
  p1.lat = 47.6062095; p1.lon = -122.3320748;
  p1.has_alt = true; p1.alt_m = 56;
  p1.has_motion = true; p1.speed_mps = 1.4f; p1.heading_deg = 270.5f;
  p1.accuracy_m = 8; p1.battery_pct = 77; p1.role = 2; p1.color = 5; p1.status = 1;
  p1.from_radio = true;
  snprintf(p1.callsign, sizeof p1.callsign, "Jade \xE2\x9C\xA8");
  size_t n = TeamWire::encode(p1, buf, sizeof buf);
  printf("P1 %s\n", hex(buf, n).c_str());
  CHECK(hex(buf, n) == kP1);

  TeamWire::Position p2;
  key(p2.key4, 0x01020304);
  p2.lat = -33.8688197; p2.lon = 151.2092955;
  p2.role = 8; p2.color = 11; p2.status = 5;
  p2.sos = true; p2.relayed = true;
  snprintf(p2.callsign, sizeof p2.callsign, "  Alpha-1  ");
  n = TeamWire::encode(p2, buf, sizeof buf);
  printf("P2 %s\n", hex(buf, n).c_str());
  CHECK(hex(buf, n) == kP2);

  TeamWire::Alert a1;
  key(a1.key4, 0xAABBCCDD);
  a1.id = 0xBEEF; a1.active = true; a1.type = 1; a1.has_pos = true;
  a1.lat = 47.5; a1.lon = -122.25; a1.accuracy_m = 12;
  snprintf(a1.callsign, sizeof a1.callsign, "Bravo");
  snprintf(a1.note, sizeof a1.note, "Need water at the ridge");
  n = TeamWire::encode(a1, buf, sizeof buf);
  printf("A1 %s\n", hex(buf, n).c_str());
  CHECK(hex(buf, n) == kA1);

  TeamWire::Alert a2;
  key(a2.key4, 0xAABBCCDD);
  a2.id = 1;
  n = TeamWire::encode(a2, buf, sizeof buf);
  printf("A2 %s\n", hex(buf, n).c_str());
  CHECK(hex(buf, n) == kA2);

  TeamWire::Marker m1;
  key(m1.key4, 0x0A0B0C0D);
  m1.id = 0x12345678; m1.kind = 4; m1.lat = 46.8523; m1.lon = -121.7603; m1.time = 1760242768;
  snprintf(m1.label, sizeof m1.label, "Rockfall");
  snprintf(m1.note, sizeof m1.note, "Trail blocked past the bridge");
  n = TeamWire::encode(m1, buf, sizeof buf);
  printf("M1 %s\n", hex(buf, n).c_str());
  CHECK(hex(buf, n) == kM1);

  TeamWire::Marker m2;
  key(m2.key4, 0x0A0B0C0D);
  m2.id = 0xFF; m2.deleted = true; m2.kind = 9; m2.time = 1;
  // 36 characters: cut to the 32-byte label limit
  snprintf(m2.label, sizeof m2.label, "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345");
  m2.att_mode = 40; key(m2.att_sid, 0xDEADBEEF); m2.att_packets = 12; m2.att_extra = 0x0102;
  n = TeamWire::encode(m2, buf, sizeof buf);
  printf("M2 %s\n", hex(buf, n).c_str());
  CHECK(hex(buf, n) == kM2);

  // and back
  TeamWire::Position q;
  auto v = unhex(kP1);
  CHECK(TeamWire::parse(v.data(), v.size(), q));
  CHECK(!strcmp(q.callsign, "Jade \xE2\x9C\xA8") && q.role == 2 && q.color == 5 && q.status == 1);
  CHECK(q.has_alt && q.alt_m == 56 && q.has_motion && q.speed_mps == 1.5f && q.battery_pct == 77 && q.from_radio);
  CHECK(fabs(q.lat - 47.6062095) < 1e-7 && fabs(q.lon + 122.3320748) < 1e-7);
  v = unhex(kP2);
  CHECK(TeamWire::parse(v.data(), v.size(), q));
  CHECK(!strcmp(q.callsign, "Alpha-1") && q.sos && q.relayed && q.battery_pct == -1 && !q.has_motion);
  TeamWire::Alert b;
  v = unhex(kA1);
  CHECK(TeamWire::parse(v.data(), v.size(), b));
  CHECK(b.id == 0xBEEF && b.active && b.type == 1 && b.has_pos && !strcmp(b.note, "Need water at the ridge"));
  TeamWire::Marker mm;
  v = unhex(kM2);
  CHECK(TeamWire::parse(v.data(), v.size(), mm));
  CHECK(mm.deleted && mm.att_mode == 40 && mm.att_packets == 12 && mm.att_extra == 0x0102 && strlen(mm.label) == 32);

  char rb[40];
  TeamWire::rangeBearing(1234, 44.6, rb, sizeof rb);
  CHECK(!strcmp(rb, "1.2 km 045\xC2\xB0 NE"));
  TeamWire::rangeBearing(87.4, 359.7, rb, sizeof rb);
  CHECK(!strcmp(rb, "87 m 000\xC2\xB0 N"));
}

// ---- TeamCore on a simulated clock -------------------------------------------------
struct TestIo : team::Io {
  uint32_t now = 1000000;
  std::vector<std::vector<uint8_t>> sent;
  std::map<std::string, std::vector<uint8_t>> files;
  team::Fix f;
  bool have_fix = true;
  int alarms = 0;
  uint32_t rnd = 7;
  uint32_t nowMs() override { return now; }
  uint32_t epoch() override { return 1760000000u + now / 1000; }
  uint32_t random32() override { rnd = rnd * 1103515245u + 12345u; return rnd; }
  void selfKey4(uint8_t out[4]) override { key(out, 0xC0FFEE00); }
  const char* nodeName() override { return "TDeck"; }
  bool fix(team::Fix& out) override { out = f; return have_fix; }
  int batteryPct() override { return 64; }
  bool channelOk(const char* name) override { return !strcmp(name, "ops"); }
  bool send(const char*, const uint8_t* d, size_t n) override { sent.emplace_back(d, d + n); return true; }
  bool readFile(const char* name, uint8_t* buf, size_t cap, size_t* len) override {
    auto it = files.find(name);
    if (it == files.end() || it->second.size() > cap) return false;
    memcpy(buf, it->second.data(), it->second.size());
    *len = it->second.size();
    return true;
  }
  bool writeFile(const char* name, const uint8_t* d, size_t n) override { files[name] = std::vector<uint8_t>(d, d + n); return true; }
  void changed() override {}
  void alarm(const team::AlertRec&) override { alarms++; }
};

static void core() {
  TestIo io;
  team::Core c(&io);
  c.begin();
  io.f.lat = 47.6; io.f.lon = -122.3; io.f.gps = true;
  c.tick();
  CHECK(io.sent.empty());   // off
  c.cfg.enabled = true;
  snprintf(c.cfg.channel, sizeof c.cfg.channel, "ops");
  c.cfg.role = 6;
  c.saveSettings();
  c.tick();
  CHECK(io.sent.size() == 1 && io.sent[0][0] == TeamWire::kPosition);   // first beacon at once
  TeamWire::Position me;
  CHECK(TeamWire::parse(io.sent[0].data(), io.sent[0].size(), me) && me.role == 6 && me.battery_pct == 64 &&
        !strcmp(me.callsign, "TDeck") && me.from_radio);
  // still: nothing for 10 minutes
  for (int i = 0; i < 590; i++) { io.now += 1000; c.tick(); }
  CHECK(io.sent.size() == 1);
  io.now += 11000; c.tick();
  CHECK(io.sent.size() == 2);
  // moving 100 m: after 45 s
  io.f.lat += 0.0009; io.f.has_motion = true; io.f.speed_mps = 1.5; io.f.heading_deg = 0;
  io.now += 44000; c.tick();
  CHECK(io.sent.size() == 2);
  io.now += 2000; c.tick();
  CHECK(io.sent.size() == 3);
  // sharp turn after the 30 s minimum
  io.f.heading_deg = 90;
  io.now += 31000; c.tick();
  CHECK(io.sent.size() == 4);

  // SOS: sent at once, again at 15 s and 45 s, and positions carry the flag
  io.sent.clear();
  c.raiseAlert(0, "fell");
  int alerts = 0;
  for (auto& s : io.sent) if (s[0] == TeamWire::kAlert) alerts++;
  CHECK(alerts == 1);
  for (int i = 0; i < 60; i++) { io.now += 1000; c.tick(); }
  alerts = 0;
  bool pos_sos = false;
  for (auto& s : io.sent) {
    if (s[0] == TeamWire::kAlert) alerts++;
    TeamWire::Position p;
    if (s[0] == TeamWire::kPosition && TeamWire::parse(s.data(), s.size(), p) && p.sos) pos_sos = true;
  }
  CHECK(alerts == 3);
  CHECK(pos_sos);
  io.sent.clear();
  c.cancelAlert();
  for (int i = 0; i < 20; i++) { io.now += 1000; c.tick(); }
  int cancels = 0;
  for (auto& s : io.sent) {
    TeamWire::Alert a;
    if (s[0] == TeamWire::kAlert && TeamWire::parse(s.data(), s.size(), a) && !a.active) cancels++;
  }
  CHECK(cancels == 2);
  CHECK(!c.sosActive());

  // receiving: our own echo ignored, teammates kept, alarm once per alert
  std::vector<uint8_t> v = unhex(kP1);
  c.onPayload("ops", v.data(), v.size());
  c.onPayload("other", v.data(), v.size());   // not the team channel
  CHECK(c.memberCount() == 1);
  v = unhex(kA1);
  c.onPayload("ops", v.data(), v.size());
  c.onPayload("ops", v.data(), v.size());   // sent three times by the app
  CHECK(io.alarms == 1);
  CHECK(c.firstUnacked() != nullptr);
  c.acknowledge(*c.firstUnacked());
  CHECK(c.firstUnacked() == nullptr);
  TeamWire::Alert off;
  TeamWire::parse(v.data(), v.size(), off);
  off.active = false;
  uint8_t buf[200];
  size_t n = TeamWire::encode(off, buf, sizeof buf);
  c.onPayload("ops", buf, n);
  CHECK(c.firstUnacked() == nullptr && io.alarms == 1);

  // pins: newer wins, deletes stick, own pins persist across a restart
  v = unhex(kM1);
  c.onPayload("ops", v.data(), v.size());
  int pins = 0;
  for (int i = 0; i < team::kMaxMarkers; i++) if (c.marker(i)) pins++;
  CHECK(pins == 1);
  TeamWire::Marker older;
  TeamWire::parse(v.data(), v.size(), older);
  older.time -= 10;
  snprintf(older.label, sizeof older.label, "stale");
  n = TeamWire::encode(older, buf, sizeof buf);
  c.onPayload("ops", buf, n);
  for (int i = 0; i < team::kMaxMarkers; i++) if (c.marker(i)) CHECK(!strcmp(c.marker(i)->label, "Rockfall"));
  TeamWire::Marker* mine = c.putMarker(nullptr, 3, 47.61, -122.31, "Boot print", "size 10");
  CHECK(mine && c.isMine(*mine));
  CHECK(io.sent.back()[0] == TeamWire::kMarker);
  c.deleteMarker(*mine);
  TeamWire::Marker gone;
  CHECK(TeamWire::parse(io.sent.back().data(), io.sent.back().size(), gone) && gone.deleted);

  team::Core c2(&io);
  c2.begin();
  CHECK(c2.cfg.enabled && !strcmp(c2.cfg.channel, "ops") && c2.cfg.role == 6);
  pins = 0;
  for (int i = 0; i < team::kMaxMarkers; i++) if (c2.marker(i)) pins++;
  CHECK(pins == 1);   // Rockfall; the deleted one stays deleted

  // Bloodhound follows a teammate
  TeamWire::Position p1;
  v = unhex(kP1);
  TeamWire::parse(v.data(), v.size(), p1);
  c.houndMember(p1.key4);
  double la, lo;
  char lbl[33];
  CHECK(c.hound(&la, &lo, lbl, sizeof lbl) && !strcmp(lbl, "Jade \xE2\x9C\xA8"));
  c.stopHound();
  CHECK(!c.hound(&la, &lo, lbl, sizeof lbl));
}

int main() {
  golden();
  core();
  if (g_fail) { printf("%d check(s) failed\n", g_fail); return 1; }
  printf("all team checks passed\n");
  return 0;
}
