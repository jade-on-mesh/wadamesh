// SPDX-License-Identifier: GPL-3.0-or-later
//
// VoxFetch against a simulated mesh and a simulated MeshCore Open TAK sender
// that answers the way lib/vox/voice_service.dart's _serve does: a direct
// request is answered along the reverse of the request's path when it carries
// one, else along the sender's own route; a VR3 DM makes both radios learn a
// route a few seconds later; a channel request is answered on the channel.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "ui-touch/VoxFetch.h"

namespace {

struct Event {
  uint32_t at;
  std::vector<uint8_t> data;
  int channel;   // -1 raw
};

struct Sim : VoxFetchIo {
  // ---- world
  uint32_t now = 1000;
  uint32_t air = 600;                 // ms per photo packet
  uint32_t rng = 1;
  double loss = 0.0;                  // per data packet
  bool we_have_route = true;          // our out_path to the sender
  bool our_route_works = true;        // requests along it arrive
  bool sender_route_works = true;     // the sender's own stored route back to us
  bool reverse_path_works = true;     // replying along our request's path
  bool flood_works = true;            // DMs and channel traffic
  bool sender_is_contact = true;
  bool sender_has_route_to_us = true;
  int hops_to_sender = 1;
  uint8_t sender_pub[32];
  uint8_t self6[6] = {1, 2, 3, 4, 5, 6};
  std::vector<uint8_t> payload;       // what the sender holds
  std::vector<Event> inbox;           // to deliver to the engine
  std::map<uint32_t, std::vector<uint8_t>> store;
  VoxFetch* fetch = nullptr;

  // ---- what happened
  int raw_requests = 0, dm_requests = 0, channel_requests = 0;
  int served_packets = 0, changes = 0;
  std::vector<bool> last_req_had_path;
  std::vector<std::string> dm_texts;

  double rnd() { rng = rng * 1103515245u + 12345u; return ((rng >> 8) & 0xFFFF) / 65536.0; }

  uint32_t nowMs() override { return now; }
  void selfKey6(uint8_t out[6]) override { memcpy(out, self6, 6); }
  bool contactRoute(const uint8_t pub[32], uint8_t* path, uint8_t* packed) override {
    if (memcmp(pub, sender_pub, 32) != 0 || !we_have_route) return false;
    for (int i = 0; i < hops_to_sender; i++) path[i] = (uint8_t)(0xA0 + i);
    *packed = (uint8_t)hops_to_sender;
    return true;
  }
  int contactHops(const uint8_t pub[32]) override {
    return memcmp(pub, sender_pub, 32) == 0 && we_have_route ? hops_to_sender : -1;
  }
  bool contactByName(const char* name, uint8_t out[32]) override {
    if (!sender_is_contact || strcmp(name, "Jade") != 0) return false;
    memcpy(out, sender_pub, 32);
    return true;
  }

  // The sender serves `want` (all if empty) as a paced burst on a plane.
  void serve(const std::vector<int>& want, bool on_channel, uint32_t start, bool lossy_link_ok) {
    std::vector<int> idx = want;
    const int total = (int)((payload.size() + VoxWire::kBytesPerPacket - 1) / VoxWire::kBytesPerPacket);
    if (idx.empty()) for (int i = 0; i < total; i++) idx.push_back(i);
    const uint32_t gap = on_channel ? 4 * air + 150 : 2 * air + 150;
    uint32_t t = start;
    for (int i : idx) {
      t += gap;
      served_packets++;
      if (!lossy_link_ok || rnd() < loss) continue;
      const size_t off = (size_t)i * VoxWire::kBytesPerPacket;
      const size_t n = payload.size() - off < (size_t)VoxWire::kBytesPerPacket ? payload.size() - off
                                                                                : VoxWire::kBytesPerPacket;
      Event e{t, {}, on_channel ? 2 : -1};
      e.data.push_back(VoxWire::kPacketSaltedMagic);
      uint8_t sid[4];
      VoxWire::writeSid(0xC0FFEE01, sid);
      e.data.insert(e.data.end(), sid, sid + 4);
      e.data.push_back((uint8_t)i);
      e.data.insert(e.data.end(), payload.begin() + off, payload.begin() + off + n);
      e.data.push_back((uint8_t)rnd());   // salt
      inbox.push_back(e);
    }
  }

  static std::vector<int> missingOf(const uint8_t* req, size_t len) {
    std::vector<int> m;
    if (req[5] & VoxWire::kFetchFlagMissing)
      for (int i = 0; i < req[12] && 13 + i < (int)len; i++) m.push_back(req[13 + i]);
    return m;
  }

  bool sendRaw(const uint8_t* path, uint8_t packed, const uint8_t* data, size_t len) override {
    (void)path;
    assert((packed >> 6) == 0);
    assert(VoxWire::isFetchRequest(data, len) && VoxWire::readSid(data + 1) == 0xC0FFEE01);
    assert(data[5] & VoxWire::kFetchFlagSaltOk);
    raw_requests++;
    const bool has_path = data[5] & VoxWire::kFetchFlagHasPath;
    last_req_had_path.push_back(has_path);
    if (has_path) {   // [len][path] after the indices, our route to the sender
      const int at = 13 + data[12];
      assert((int)len == at + 1 + data[at] && data[at] == packed);
    }
    if (!our_route_works) return true;   // vanishes on the way
    const bool back = has_path ? reverse_path_works : (sender_has_route_to_us && sender_route_works);
    serve(missingOf(data, len), false, now + 2 * air, back);
    return true;
  }
  bool sendDmFlood(const uint8_t pub[32], const char* text) override {
    assert(memcmp(pub, sender_pub, 32) == 0);
    dm_requests++;
    dm_texts.push_back(text);
    we_have_route = false;   // the firmware clears it so the DM floods
    if (!flood_works) return true;
    // ACK + path exchange: both sides learn a working route ~3 s later, then
    // the sender (which waits up to 25 s for one) serves over raw data.
    we_have_route = true;
    our_route_works = true;
    sender_has_route_to_us = true;
    sender_route_works = true;
    std::vector<int> m;
    const char* w = strchr(text + 4, ':') + 1;
    if (*w == 'm') {   // "m1-3.z" ranges
      for (const char* p = w + 1; *p && *p != ':';) {
        char* e;
        long a = strtol(p, &e, 36), b = a;
        if (*e == '-') b = strtol(e + 1, &e, 36);
        for (long v = a; v <= b; v++) m.push_back((int)v);
        p = *e == '.' ? e + 1 : e;
      }
    }
    serve(m, false, now + 3000, true);
    return true;
  }
  bool sendChannelData(int channel, uint16_t type, const uint8_t* data, size_t len) override {
    assert(channel == 2 && type == VoxWire::kChannelDataType && VoxWire::isFetchRequest(data, len));
    assert(!(data[5] & VoxWire::kFetchFlagHasPath));
    channel_requests++;
    if (flood_works) serve(missingOf(data, len), true, now + 3 * air + 300, true);
    return true;
  }
  uint32_t packetAirMs() override { return air; }
  bool hasPayload(uint32_t sid) override { return store.count(sid) > 0; }
  bool savePayload(uint32_t sid, const uint8_t* d, size_t n) override {
    store[sid].assign(d, d + n);
    return true;
  }
  void changed(uint32_t) override { changes++; }

  // Advance time, delivering what's due and ticking the engine.
  void run(uint32_t ms) {
    const uint32_t end = now + ms;
    while ((int32_t)(now - end) < 0) {
      now += 50;
      for (size_t i = 0; i < inbox.size();) {
        if ((int32_t)(now - inbox[i].at) >= 0) {
          Event e = inbox[i];
          inbox.erase(inbox.begin() + (long)i);
          fetch->onPayload(e.data.data(), e.data.size(), e.channel);
        } else {
          i++;
        }
      }
      fetch->tick();
    }
  }
};

constexpr uint32_t kSid = 0xC0FFEE01;

void setup(Sim& sim, size_t payload_len) {
  memset(sim.sender_pub, 0x42, 32);
  sim.payload.resize(payload_len);
  sim.payload[0] = VoxWire::kImagePayloadVersion;
  for (size_t i = 1; i < payload_len; i++) sim.payload[i] = (uint8_t)(i * 13 + 5);
}

VoxWire::Envelope envFor(const Sim& sim) {
  VoxWire::Envelope e;
  e.sid = kSid;
  e.total = (uint8_t)((sim.payload.size() + VoxWire::kBytesPerPacket - 1) / VoxWire::kBytesPerPacket);
  e.aspect = 150;
  return e;
}

bool gotIt(Sim& sim, VoxFetch& f) {
  return f.status(kSid) == VoxFetch::kComplete && sim.store.count(kSid) && sim.store[kSid] == sim.payload;
}

}  // namespace

int main() {
  // 1. Healthy direct route: one raw request, asks to be answered along its path.
  {
    Sim sim; setup(sim, 2750);
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, false);
    assert(f.status(kSid) == VoxFetch::kRemote);
    assert(f.request(kSid));
    assert(f.status(kSid) == VoxFetch::kRequesting && f.lastMethod(kSid) == VoxFetch::kDirect);
    sim.run(60000);
    assert(gotIt(sim, f) && sim.raw_requests == 1 && sim.last_req_had_path[0]);
    printf("direct: %d request, %d packets\n", sim.raw_requests, sim.served_packets);
  }

  // 2. 25% loss: re-asks only for what's missing until it has everything.
  {
    Sim sim; setup(sim, 5500); sim.loss = 0.25; sim.rng = 7;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    sim.run(300000);
    assert(gotIt(sim, f));
    assert(sim.served_packets < 36 * 3);   // re-requests were for the gaps, not everything
    printf("lossy: %d requests, %d packets served for 36\n", sim.raw_requests, sim.served_packets);
  }

  // 3. No route to the sender yet: a flooded VR3 DM, then the transfer.
  {
    Sim sim; setup(sim, 2750); sim.we_have_route = false;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    assert(f.lastMethod(kSid) == VoxFetch::kDm && sim.dm_requests == 1);
    assert(sim.dm_texts[0].rfind("VR3:", 0) == 0 && sim.dm_texts[0].find(":a:010203040506") != std::string::npos);
    sim.run(90000);
    assert(gotIt(sim, f) && sim.raw_requests == 0);
    printf("dm: %d DM request\n", sim.dm_requests);
  }

  // 4. The sender's own route back is broken but our path reversed works: the
  //    first request already carries it, so no fallbacks are needed.
  {
    Sim sim; setup(sim, 2750); sim.sender_route_works = false;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    sim.run(60000);
    assert(gotIt(sim, f) && sim.raw_requests == 1 && sim.dm_requests == 0);
  }

  // 5. Reverse path broken, sender's route fine: the next direct request drops
  //    the path so the sender uses its own route.
  {
    Sim sim; setup(sim, 2750); sim.reverse_path_works = false;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    sim.run(200000);
    assert(gotIt(sim, f));
    assert(sim.last_req_had_path.size() >= 1 && sim.last_req_had_path[0]);
    bool tried_without = false;
    for (bool b : sim.last_req_had_path) tried_without |= !b;
    assert(tried_without || sim.dm_requests > 0);
    printf("reverse path broken: %d raw, %d DM\n", sim.raw_requests, sim.dm_requests);
  }

  // 6. Our route to the sender is dead and DMs don't get through either: a
  //    channel post falls back to the channel and completes there.
  {
    Sim sim; setup(sim, 2750); sim.our_route_works = false; sim.flood_works = false;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), nullptr, "Jade", 2, true);
    sim.run(30000);
    sim.flood_works = true;   // the channel itself is fine; only the DM path was bad
    sim.run(600000);
    assert(gotIt(sim, f) && sim.channel_requests >= 1);
    printf("channel fallback: %d raw, %d DM, %d channel\n", sim.raw_requests, sim.dm_requests,
           sim.channel_requests);
  }

  // 7. Channel post from someone who isn't a contact: straight to the channel.
  {
    Sim sim; setup(sim, 2750); sim.sender_is_contact = false;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), nullptr, "Stranger", 2, true);
    assert(f.lastMethod(kSid) == VoxFetch::kChannel);
    sim.run(120000);
    assert(gotIt(sim, f) && sim.raw_requests == 0 && sim.dm_requests == 0);
  }

  // 8. Nothing gets through: gives up within its attempt budget, says so, and a
  //    tap starts over.
  {
    Sim sim; setup(sim, 2750); sim.our_route_works = false; sim.flood_works = false;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, 2, true);
    sim.run(3600000);
    assert(f.status(kSid) == VoxFetch::kUnavailable);
    assert(f.attempts(kSid) <= VoxFetch::kMaxAttempts);
    char buf[80];
    assert(strstr(f.statusText(kSid, buf, sizeof buf), "Tap to retry"));
    const int before = sim.raw_requests + sim.dm_requests + sim.channel_requests;
    sim.flood_works = true; sim.our_route_works = true;
    assert(f.request(kSid));
    sim.run(200000);
    assert(gotIt(sim, f) && sim.raw_requests + sim.dm_requests + sim.channel_requests > before);
    printf("gave up after %d attempts, then recovered on a tap\n", before);
  }

  // 9. Someone else's channel transfer arrives before we tap: kept quietly, and
  //    the photo is complete without sending anything.
  {
    Sim sim; setup(sim, 2750);
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), nullptr, "Jade", 2, false);
    sim.serve({}, true, sim.now, true);
    sim.run(60000);
    assert(gotIt(sim, f) && sim.raw_requests + sim.dm_requests + sim.channel_requests == 0);
  }

  // 10. Already stored: complete without a session doing anything.
  {
    Sim sim; setup(sim, 2750);
    sim.store[kSid] = sim.payload;
    VoxFetch f(sim); sim.fetch = &f;
    assert(f.status(kSid) == VoxFetch::kComplete);
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    assert(f.status(kSid) == VoxFetch::kComplete && sim.raw_requests == 0);
  }

  // 11. More envelopes than sessions: idle ones are recycled, the transfer in
  //     progress is kept, and junk payloads are ignored.
  {
    Sim sim; setup(sim, 2750);
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    for (uint32_t i = 0; i < 20; i++) {
      VoxWire::Envelope e{0x1000 + i, 3, 100};
      sim.now += 10;
      f.onEnvelope(e, nullptr, "x", 1, false);
    }
    const uint8_t junk[] = {0x56, 0xC0, 0xFF, 0xEE, 0x01, 200, 1, 2, 3};   // index out of range
    f.onPayload(junk, sizeof junk, -1);
    f.onPayload(nullptr, 0, -1);
    sim.run(60000);
    assert(gotIt(sim, f));
    assert(f.status(0x1000) == VoxFetch::kUnknown && f.status(0x1000 + 19) == VoxFetch::kRemote);
  }

  printf("all VoxFetch scenarios pass\n");
  return 0;
}
