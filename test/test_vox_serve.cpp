// SPDX-License-Identifier: GPL-3.0-or-later
//
// VoxServe (answering fetches for clips recorded on this radio) against a
// simulated mesh. The requests are the bytes MeshCore Open TAK sends
// (lib/vox/voice_wire.dart: VoxVoiceFetchRequest.encode and
// VoxVoiceTextRequest.encode), written out by hand so a change on either side
// shows up here, and the answers are reassembled with VoxWire::Assembly the
// way any listener does.
//
// Build: g++ -std=c++17 -Wall -Wextra -I src test/test_vox_serve.cpp src/ui-touch/VoxServe.cpp

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "ui-touch/VoxServe.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                    \
  do {                                                      \
    if (!(cond)) {                                          \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);           \
      printf(__VA_ARGS__);                                  \
      printf("\n");                                         \
      g_fail++;                                             \
    }                                                       \
  } while (0)

namespace {

struct Sent {
  uint32_t at;
  int channel;                 // -1 raw
  std::vector<uint8_t> path;   // raw only
  std::vector<uint8_t> data;
};

struct Sim : VoxServeIo {
  uint32_t now = 10000;
  uint32_t air = 500;
  uint32_t rng = 1;
  uint8_t self6[6] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
  std::map<std::string, std::vector<uint8_t>> routes;   // key6 hex -> route
  std::map<std::string, uint32_t> route_from;           // route appears at this time
  std::map<uint32_t, std::pair<uint8_t, std::vector<uint8_t>>> clips;
  std::vector<Sent> sent;
  int fail_sends = 0;
  int allocs = 0;
  bool verbose = false;

  static std::string hex(const uint8_t* k, int n) {
    std::string s;
    char b[3];
    for (int i = 0; i < n; i++) { snprintf(b, 3, "%02x", k[i]); s += b; }
    return s;
  }

  uint32_t nowMs() override { return now; }
  uint32_t random32() override { rng = rng * 1103515245u + 12345u; return rng >> 8; }
  void selfKey6(uint8_t out[6]) override { memcpy(out, self6, 6); }
  bool routeToKey6(const uint8_t key6[6], uint8_t* path, int* len) override {
    auto it = routes.find(hex(key6, 6));
    if (it == routes.end()) return false;
    auto f = route_from.find(it->first);
    if (f != route_from.end() && now < f->second) return false;
    memcpy(path, it->second.data(), it->second.size());
    *len = (int)it->second.size();
    return true;
  }
  bool sendRaw(const uint8_t* path, int len, const uint8_t* data, size_t n) override {
    if (fail_sends > 0) { fail_sends--; return false; }
    sent.push_back(Sent{now, -1, std::vector<uint8_t>(path, path + len), std::vector<uint8_t>(data, data + n)});
    return true;
  }
  bool sendChannelData(int ch, uint16_t type, const uint8_t* data, size_t n) override {
    if (type != VoxWire::kChannelDataType) return false;
    sent.push_back(Sent{now, ch, {}, std::vector<uint8_t>(data, data + n)});
    return true;
  }
  uint32_t packetAirMs() override { return air; }
  bool loadClip(uint32_t sid, uint8_t* mode, uint8_t** bits, size_t* len) override {
    auto it = clips.find(sid);
    if (it == clips.end()) return false;
    *mode = it->second.first;
    *len = it->second.second.size();
    *bits = (uint8_t*)malloc(*len);
    memcpy(*bits, it->second.second.data(), *len);
    allocs++;
    return true;
  }
  void freeClip(uint8_t* bits) override { free(bits); allocs--; }
  void log(const char* line) override { if (verbose) printf("  %6u %s\n", now, line); }

  void run(VoxServe& s, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) { s.tick(); now += 10; }
  }
};

const uint8_t kKeyA[6] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6};
const uint8_t kKeyB[6] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6};
const uint32_t kSid = 0x0A0B0C0D;

// A 10 s clip at 1200: 1500 bytes = 9 full packets of 156 and one of 96.
std::vector<uint8_t> clip1200(size_t len = 1500) {
  std::vector<uint8_t> b(len);
  for (size_t i = 0; i < len; i++) b[i] = (uint8_t)(i * 7 + 3);
  return b;
}

// Dart: VoxVoiceFetchRequest(sid, key, missing, saltOk, requesterPath).encode()
std::vector<uint8_t> dartFetch(uint32_t sid, const uint8_t key[6], const std::vector<int>* missing, bool salt_ok,
                               const std::vector<uint8_t>* path) {
  std::vector<uint8_t> b = {0x72, (uint8_t)(sid >> 24), (uint8_t)(sid >> 16), (uint8_t)(sid >> 8), (uint8_t)sid};
  b.push_back((uint8_t)((missing ? 0x01 : 0) | (salt_ok ? 0x02 : 0) | (path ? 0x04 : 0)));
  b.insert(b.end(), key, key + 6);
  b.push_back(missing ? (uint8_t)missing->size() : 0);
  if (missing) for (int v : *missing) b.push_back((uint8_t)v);
  if (path) { b.push_back((uint8_t)path->size()); b.insert(b.end(), path->begin(), path->end()); }
  return b;
}

// Reassembles everything sent for `sid` and returns it if complete.
std::vector<uint8_t> reassemble(const Sim& sim, uint32_t sid, int total, size_t from = 0) {
  std::vector<uint8_t> store((size_t)total * 156);
  VoxWire::Assembly a;
  a.begin(sid, (uint8_t)total, store.data(), store.size(), 156);
  for (size_t i = from; i < sim.sent.size(); i++) {
    VoxWire::Packet p;
    if (VoxWire::parsePacket(sim.sent[i].data.data(), sim.sent[i].data.size(), &p)) a.add(p);
  }
  if (!a.complete()) return {};
  return std::vector<uint8_t>(a.payload(), a.payload() + a.payloadLen());
}

void testRawAlongRequestPath() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200()};
  const std::vector<uint8_t> path = {0x11, 0x22, 0x33};   // requester -> us
  const auto req = dartFetch(kSid, kKeyA, nullptr, true, &path);
  s.onPayload(req.data(), req.size(), -1);
  sim.run(s, 60000);
  CHECK(sim.sent.size() == 10, "sent %zu packets, want 10", sim.sent.size());
  bool rev = true, salted = true;
  for (auto& p : sim.sent) {
    rev = rev && p.channel == -1 && p.path == std::vector<uint8_t>({0x33, 0x22, 0x11});
    salted = salted && p.data[0] == 0x57;
  }
  CHECK(rev, "reply must go along the reverse of the request's path");
  CHECK(salted, "requester said salt OK, so packets are salted");
  CHECK(reassemble(sim, kSid, 10) == clip1200(), "reassembled clip differs");
  // Pacing: 3 hops -> air + 2 * 2.5 * 1.15 * air + 400 = 500 + 2875 + 400.
  const uint32_t gap = sim.sent[1].at - sim.sent[0].at;
  CHECK(gap >= 3770 && gap <= 3790, "gap %u ms", gap);
  CHECK(sim.allocs == 0, "clip buffer leaked (%d)", sim.allocs);
}

void testRawUsesContactRoute() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200(300)};   // 2 packets
  sim.routes[Sim::hex(kKeyA, 6)] = {0x44};
  const auto req = dartFetch(kSid, kKeyA, nullptr, false, nullptr);
  s.onPayload(req.data(), req.size(), -1);
  sim.run(s, 20000);
  CHECK(sim.sent.size() == 2, "sent %zu", sim.sent.size());
  CHECK(!sim.sent.empty() && sim.sent[0].path == std::vector<uint8_t>({0x44}), "should use the contact's route");
  CHECK(!sim.sent.empty() && sim.sent[0].data[0] == 0x56, "no salt unless asked");
  CHECK(sim.sent.size() == 2 && sim.sent[1].data.size() == 6 + 144, "last packet is the short remainder");
}

void testZeroHopRequestPath() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200(156)};
  const std::vector<uint8_t> path;   // the requester is our neighbour
  const auto req = dartFetch(kSid, kKeyA, nullptr, true, &path);
  s.onPayload(req.data(), req.size(), -1);
  sim.run(s, 5000);
  CHECK(sim.sent.size() == 1 && sim.sent[0].path.empty(), "an empty path means send zero-hop");
}

void testNoRouteGivesUp() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200()};
  const auto req = dartFetch(kSid, kKeyA, nullptr, true, nullptr);
  s.onPayload(req.data(), req.size(), -1);
  sim.run(s, 4000);
  CHECK(sim.sent.empty(), "no route: nothing to send");
  CHECK(!s.busy(), "gave up after 3 s");
  CHECK(sim.allocs == 0, "leak");
}

void testVr3WaitsForRoute() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200()};
  // The flooded DM teaches both radios a route ~8 s later.
  sim.routes[Sim::hex(kKeyB, 6)] = {0x55, 0x66};
  sim.route_from[Sim::hex(kKeyB, 6)] = sim.now + 8000;
  // Dart: VoxVoiceTextRequest.encode(sid 0x0a0b0c0d -> base36 "2sbggd", missing [1,2,3,7])
  const char* vr3 = "VR3:2sbggd:m1-3.7:b1b2b3b4b5b6";
  VoxWire::FetchRequest chk;
  CHECK(VoxWire::parseTextFetch(vr3, &chk) && chk.sid == kSid, "VR3 parse");
  CHECK(s.onDirectText(vr3), "VR3 is a control message");
  CHECK(!s.onDirectText("hello"), "plain text isn't");
  sim.run(s, 40000);
  CHECK(sim.sent.size() == 4, "sent %zu, want the 4 missing", sim.sent.size());
  std::vector<int> idx;
  for (auto& p : sim.sent) idx.push_back(p.data[5]);
  CHECK(idx == std::vector<int>({1, 2, 3, 7}), "wrong packets");
  CHECK(!sim.sent.empty() && sim.sent[0].at >= 18000, "sent before the route existed");
  CHECK(!sim.sent.empty() && sim.sent[0].path == std::vector<uint8_t>({0x55, 0x66}), "route");
  CHECK(!sim.sent.empty() && sim.sent[0].data[0] == 0x56, "VR3 doesn't negotiate salt");
}

void testVr3TimesOut() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200()};
  s.onDirectText("VR3:2sbggd:a:b1b2b3b4b5b6");
  sim.run(s, 26000);
  CHECK(sim.sent.empty() && !s.busy(), "gives up after 25 s without a route");
}

void testChannelOwnOnly() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200(400)};   // 3 packets
  const std::vector<int> miss = {0, 2};
  const auto req = dartFetch(kSid, kKeyA, &miss, true, nullptr);
  s.onPayload(req.data(), req.size(), 3);
  sim.run(s, 20000);
  CHECK(sim.sent.empty(), "not our clip: leave channel requests to its author");

  s.markOwn(kSid);
  const uint32_t t0 = sim.now;
  // A fresh request (a different listener, so not a repeat).
  const auto req2 = dartFetch(kSid, kKeyB, &miss, true, nullptr);
  s.onPayload(req2.data(), req2.size(), 3);
  sim.run(s, 20000);
  CHECK(sim.sent.size() == 2, "sent %zu", sim.sent.size());
  CHECK(!sim.sent.empty() && sim.sent[0].channel == 3, "answer on the request's channel");
  CHECK(!sim.sent.empty() && sim.sent[0].at - t0 >= 300 && sim.sent[0].at - t0 < 400, "author answers after 300 ms");
  const uint32_t gap = sim.sent.size() == 2 ? sim.sent[1].at - sim.sent[0].at : 0;
  CHECK(gap >= 2150 && gap <= 2170, "flood gap %u (4 * air + 150)", gap);
  for (auto& p : sim.sent) CHECK(p.data.size() <= 163, "GRP_DATA limit");
}

void testDuplicateIgnored() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200(156)};
  const std::vector<uint8_t> path = {0x11};
  const auto req = dartFetch(kSid, kKeyA, nullptr, true, &path);
  s.onPayload(req.data(), req.size(), -1);
  s.onPayload(req.data(), req.size(), -1);   // heard twice
  sim.run(s, 5000);
  CHECK(sim.sent.size() == 1, "duplicate served: %zu", sim.sent.size());
  s.onPayload(req.data(), req.size(), -1);   // still inside 6 s
  sim.run(s, 5000);
  CHECK(sim.sent.size() == 1, "repeat inside 6 s served");
  s.onPayload(req.data(), req.size(), -1);   // a genuine retry later
  sim.run(s, 5000);
  CHECK(sim.sent.size() == 2, "retry after 6 s not served");
}

void testNewerRequestReplaces() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200()};
  const std::vector<uint8_t> path = {0x11};
  const auto all = dartFetch(kSid, kKeyA, nullptr, true, &path);
  s.onPayload(all.data(), all.size(), -1);
  sim.run(s, 5000);   // a few packets out
  const size_t before = sim.sent.size();
  CHECK(before >= 2 && before < 10, "%zu sent before the new request", before);
  const std::vector<int> miss = {8, 9};
  const auto some = dartFetch(kSid, kKeyA, &miss, true, &path);
  s.onPayload(some.data(), some.size(), -1);
  sim.run(s, 30000);
  std::vector<int> after;
  for (size_t i = before; i < sim.sent.size(); i++) after.push_back(sim.sent[i].data[5]);
  CHECK(after == std::vector<int>({8, 9}), "after the newer request: %zu packets", after.size());
  CHECK(sim.allocs == 0, "leak");
}

void testQueuedListeners() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200(300)};
  const std::vector<uint8_t> pa = {0x11}, pb = {0x22};
  const auto ra = dartFetch(kSid, kKeyA, nullptr, true, &pa);
  const auto rb = dartFetch(kSid, kKeyB, nullptr, true, &pb);
  s.onPayload(ra.data(), ra.size(), -1);
  s.onPayload(rb.data(), rb.size(), -1);
  sim.run(s, 30000);
  CHECK(sim.sent.size() == 4, "%zu", sim.sent.size());
  CHECK(sim.sent.size() == 4 && sim.sent[0].path[0] == 0x11 && sim.sent[1].path[0] == 0x11 &&
            sim.sent[2].path[0] == 0x22 && sim.sent[3].path[0] == 0x22,
        "one listener at a time, in arrival order");
}

void testIgnores() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200()};
  const std::vector<uint8_t> path = {0x11};
  const auto own = dartFetch(kSid, sim.self6, nullptr, true, &path);
  s.onPayload(own.data(), own.size(), -1);   // our own request echoed back
  const auto unknown = dartFetch(0x12345678, kKeyA, nullptr, true, &path);
  s.onPayload(unknown.data(), unknown.size(), -1);
  const uint8_t voice[] = {0x56, 0x0A, 0x0B, 0x0C, 0x0D, 0, 1, 2, 3};
  s.onPayload(voice, sizeof voice, -1);       // a data packet, not a request
  const uint8_t junk[] = {0x72, 1, 2};
  s.onPayload(junk, sizeof junk, -1);
  sim.run(s, 10000);
  CHECK(sim.sent.empty(), "sent %zu", sim.sent.size());
  CHECK(sim.allocs == 0, "leak");
  // Missing indices past the clip's end are skipped, not sent as garbage.
  const std::vector<int> miss = {200};
  const auto past = dartFetch(kSid, kKeyB, &miss, true, &path);
  s.onPayload(past.data(), past.size(), -1);
  sim.run(s, 5000);
  CHECK(sim.sent.empty() && sim.allocs == 0, "out-of-range index");
}

void testSendRetry() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200(156)};
  sim.fail_sends = 3;   // the packet pool is momentarily full
  const std::vector<uint8_t> path = {0x11};
  const auto req = dartFetch(kSid, kKeyA, nullptr, false, &path);
  s.onPayload(req.data(), req.size(), -1);
  sim.run(s, 5000);
  CHECK(sim.sent.size() == 1, "should retry a refused send");
}

void testParsers() {
  VoxWire::FetchRequest r;
  // Ranges and case, as Dart writes and reads them.
  CHECK(VoxWire::parseTextFetch("VR3:2sbggd:a:A1A2A3A4A5A6", &r) && r.all && r.key6[0] == 0xA1, "upper-case key");
  CHECK(VoxWire::parseTextFetch(" VR3:1:m0.a-c:a1a2a3a4a5a6\n", &r) && r.n_missing == 4 && r.missing[3] == 12,
        "ranges");
  CHECK(!VoxWire::parseTextFetch("VR3:1:m:a1a2a3a4a5a6", &r), "empty range list");
  CHECK(!VoxWire::parseTextFetch("VR3:1:m3-1:a1a2a3a4a5a6", &r), "backwards range");
  CHECK(!VoxWire::parseTextFetch("VR3:1:m1-2-3:a1a2a3a4a5a6", &r), "double dash");
  CHECK(!VoxWire::parseTextFetch("VR3:1:m73:a1a2a3a4a5a6", &r), "index 255");
  CHECK(!VoxWire::parseTextFetch("VR3:1:x:a1a2a3a4a5a6", &r), "bad want");
  CHECK(!VoxWire::parseTextFetch("VR3:1:a:a1a2a3a4a5", &r), "short key");
  CHECK(!VoxWire::parseTextFetch("VR3:1:a:a1a2a3a4a5a6:x", &r), "extra field");
  // Round trip through the firmware's own encoder.
  const uint8_t miss[] = {9, 1, 2, 3, 40};
  char txt[128];
  CHECK(VoxWire::encodeTextFetch(kSid, kKeyA, miss, 5, txt, sizeof txt) > 0, "encode");
  CHECK(!strcmp(txt, "VR3:2sbggd:m1-3.9.14:a1a2a3a4a5a6"), "encoded %s", txt);
  CHECK(VoxWire::parseTextFetch(txt, &r) && r.n_missing == 5 && r.missing[4] == 40, "round trip");
  // Binary: Dart's bytes, and a path that's too long is ignored rather than rejected.
  const std::vector<int> m2 = {4};
  const std::vector<uint8_t> longpath(9, 0x77);
  const auto b = dartFetch(kSid, kKeyA, &m2, true, &longpath);
  CHECK(VoxWire::parseFetch(b.data(), b.size(), &r) && !r.all && r.n_missing == 1 && r.salt_ok && !r.has_path,
        "binary parse");
  // The envelope the radio posts.
  char env[48];
  CHECK(VoxWire::encodeVoiceEnvelope(kSid, 1, 10, 10, env, sizeof env) > 0, "envelope");
  CHECK(!strcmp(env, "VE3:2sbggd:1:a:a"), "envelope %s", env);
  VoxWire::Envelope e;
  CHECK(VoxWire::parseEnvelope(env, &e) && e.kind == VoxWire::kCodec2 && e.total == 10 && e.seconds() == 10 &&
            e.sid == kSid,
        "envelope parses as Codec2");
  CHECK(!VoxWire::encodeVoiceEnvelope(kSid, 32, 10, 10, env, sizeof env), "never a VE3 envelope for Mimi");
}

}  // namespace

int main(int argc, char** argv) {
  (void)argv;
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc > 1) printf("(verbose)\n");
  testParsers();
  testRawAlongRequestPath();
  testRawUsesContactRoute();
  testZeroHopRequestPath();
  testNoRouteGivesUp();
  testVr3WaitsForRoute();
  testVr3TimesOut();
  testChannelOwnOnly();
  testDuplicateIgnored();
  testNewerRequestReplaces();
  testQueuedListeners();
  testIgnores();
  testSendRetry();
  if (g_fail) {
    printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  printf("test_vox_serve: all passed\n");
  return 0;
}
