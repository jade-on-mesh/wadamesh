// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoxFetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
inline bool due(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int imin(int a, int b) { return a < b ? a : b; }
}  // namespace

VoxFetch::~VoxFetch() {
  for (auto& s : sessions_) release(s);
  free(orphans_);
}

VoxFetch::Session* VoxFetch::find(uint32_t sid) {
  for (auto& s : sessions_) if (s.used && s.sid == sid) return &s;
  return nullptr;
}
const VoxFetch::Session* VoxFetch::find(uint32_t sid) const {
  for (auto& s : sessions_) if (s.used && s.sid == sid) return &s;
  return nullptr;
}

// A free slot, else the least recently touched session that isn't mid-transfer,
// else the least recently touched of all.
VoxFetch::Session* VoxFetch::claim(uint32_t sid) {
  Session* pick = nullptr;
  for (auto& s : sessions_) if (!s.used) { pick = &s; break; }
  if (!pick) {
    for (int pass = 0; pass < 2 && !pick; pass++) {
      for (auto& s : sessions_) {
        const bool busy = s.st == kRequesting || s.st == kReceiving;
        if (pass == 0 && busy) continue;
        if (!pick || (int32_t)(s.last_touch - pick->last_touch) < 0) pick = &s;
      }
    }
    release(*pick);
  }
  *pick = Session{};
  pick->used = true;
  pick->sid = sid;
  pick->last_touch = io_.nowMs();
  return pick;
}

void VoxFetch::release(Session& s) {
  free(s.storage);
  s.storage = nullptr;
  s.used = false;
}

bool VoxFetch::ensureStorage(Session& s) {
  if (s.storage) return true;
  const size_t n = (size_t)s.total * (size_t)s.packet_bytes;
  s.storage = (uint8_t*)malloc(n);   // > 4 KB: PSRAM on the device
  if (!s.storage) return false;
  if (!s.asm_.begin(s.sid, s.total, s.storage, n, s.packet_bytes)) {
    free(s.storage);
    s.storage = nullptr;
    return false;
  }
  return true;
}

void VoxFetch::onEnvelope(const VoxWire::Envelope& env, const uint8_t* sender_pub,
                          const char* sender_name, int channel, bool start) {
  if (env.total < 1 || env.total > VoxWire::kMaxClipPackets || env.packetBytes() <= 0) return;
  Session* s = find(env.sid);
  if (!s) {
    s = claim(env.sid);
    s->total = env.total;
    s->aspect = env.aspect;
    s->packet_bytes = env.packetBytes();
    s->kind = env.kind;
    s->st = io_.hasPayload(env.sid) ? kComplete : kRemote;
  }
  s->last_touch = io_.nowMs();
  if (sender_pub) {
    memcpy(s->pub, sender_pub, 32);
    s->has_pub = true;
  }
  if (sender_name && sender_name[0]) {
    strncpy(s->name, sender_name, sizeof s->name - 1);
    s->name[sizeof s->name - 1] = '\0';
  }
  if (channel >= 0) s->channel = channel;
  adoptOrphans(*s);
  if (start) request(env.sid);
}

void VoxFetch::keepOrphan(const VoxWire::Packet& p, uint8_t via, bool relayed) {
  if (p.len <= 0 || p.len > kOrphanBytes) return;
  if (!orphans_) {
    orphans_ = (Orphan*)calloc(kMaxOrphans, sizeof(Orphan));   // ~8 KB: PSRAM
    if (!orphans_) return;
  }
  for (int i = 0; i < kMaxOrphans; i++) {
    const Orphan& o = orphans_[i];
    if (o.len && o.sid == p.sid && o.index == p.index) return;   // have it
  }
  Orphan& o = orphans_[orphan_next_];
  orphan_next_ = (orphan_next_ + 1) % kMaxOrphans;
  o.sid = p.sid;
  o.index = p.index;
  o.len = (uint8_t)p.len;
  o.via = via;
  o.relayed = relayed;
  memcpy(o.data, p.data, (size_t)p.len);
}

void VoxFetch::adoptOrphans(Session& s) {
  if (!orphans_ || s.st == kComplete) return;
  for (int i = 0; i < kMaxOrphans; i++) {
    Orphan& o = orphans_[i];
    if (!o.len || o.sid != s.sid) continue;
    if (o.relayed) s.relayed = true;
    VoxWire::Packet p{o.sid, o.index, o.data, o.len};
    o.len = 0;
    if (s.st != kComplete) accept(s, p, o.via);
  }
}

bool VoxFetch::request(uint32_t sid) {
  Session* s = find(sid);
  if (!s) return false;
  s->last_touch = io_.nowMs();
  if (s->st == kComplete || s->st == kRequesting || s->st == kReceiving) return true;
  if (io_.hasPayload(sid)) {
    s->st = kComplete;
    io_.changed(sid);
    return true;
  }
  // A channel post names its author; look them up each time (they may have
  // been added as a contact since the photo arrived).
  if (!s->has_pub && s->name[0] && io_.contactByName(s->name, s->pub)) s->has_pub = true;
  s->attempts = 0;
  s->stalls = 0;
  s->direct_failures = 0;
  s->tried_channel = false;
  s->reply_along_path = true;
  s->note = nullptr;
  attempt(*s);
  return true;
}

// --------------------------------------------------------------- timing
//
// Same model as voice_service.dart: _clipHops, _packetGap and _waitFor.

int VoxFetch::hops(const Session& s) {
  if (!s.has_pub) return 3;
  return imax(io_.contactHops(s.pub), 0);
}

uint32_t VoxFetch::packetGap(bool flood, int h) {
  const uint32_t air = io_.packetAirMs();
  if (flood) return (uint32_t)imax(800, (int)(air * 4 + 150));
  if (h <= 0) return (uint32_t)imax(400, (int)(air * 2 + 150));
  const double relay_air = air * 1.15;
  return (uint32_t)(air + imin(h, 2) * 2.5 * relay_air + 400);
}

uint32_t VoxFetch::zeroHopGap() {
  const uint32_t g = io_.packetAirMs() + 60;   // VoxServe::zeroHopGap
  return g > 150 ? g : 150;
}

uint32_t VoxFetch::waitFor(Session& s) {
  const int h = hops(s);
  // Once packets flow the stream is live: silence for a few packet gaps means
  // its tail was lost, not that the request is still on its way.
  if (s.st == kReceiving) return stallWait(packetGap(s.last == kChannel, h));
  const uint32_t hop_ms = io_.packetAirMs() * 2 + 700;
  const uint32_t trips = s.last == kDm ? 5 : s.last == kChannel ? 3 : 2;
  return trips * (uint32_t)(h + 1) * hop_ms + 8000;
}

void VoxFetch::arm(Session& s, uint32_t ms) {
  s.timer_on = true;
  s.deadline = io_.nowMs() + ms;
}

void VoxFetch::tick() {
  const uint32_t now = io_.nowMs();
  for (auto& s : sessions_) {
    if (s.used && s.timer_on && due(now, s.deadline)) {
      s.timer_on = false;
      onTimeout(s);
    }
  }
}

// --------------------------------------------------------------- requests

VoxFetch::Method VoxFetch::choose(Session& s) {
  uint8_t path[64], packed = 0;
  // Raw data reads the path length as a byte count: 1-byte hashes only.
  const bool route = s.has_pub && io_.contactRoute(s.pub, path, &packed) && (packed >> 6) == 0;
  const bool can_channel = s.channel >= 0;
  // Two requests in a row brought nothing: try the channel (anyone holding the
  // photo can answer) before going round the direct options again.
  if (can_channel && s.stalls >= 2 && !s.tried_channel) return kChannel;
  if (s.has_pub) {
    if (route && s.direct_failures == 0) return kDirect;
    if (!s.sent_dm) return kDm;
    if (route && s.direct_failures < 3) return kDirect;
  }
  if (can_channel) return kChannel;
  if (s.has_pub && route) return kDirect;
  if (s.has_pub) return kDm;
  return kNone;
}

void VoxFetch::attempt(Session& s) {
  const Method m = choose(s);
  if (m == kNone) {
    fail(s, s.has_pub ? "no route to the sender. Tap to retry." : "the sender isn't a contact. Tap to retry.");
    return;
  }
  s.attempts++;
  s.last = m;
  if (m == kChannel) s.tried_channel = true;
  s.at_last_attempt = s.asm_.total() ? s.asm_.received() : 0;

  uint8_t missing[VoxWire::kMaxPackets];
  int n_missing = 0;
  if (s.storage) n_missing = s.asm_.missing(missing);
  const bool any = s.storage && s.asm_.received() > 0;
  s.expected_last = (any && n_missing) ? missing[n_missing - 1] : s.total - 1;

  uint8_t key6[6];
  io_.selfKey6(key6);
  bool sent = false;
  if (m == kDirect) {
    uint8_t path[64], packed = 0;
    io_.contactRoute(s.pub, path, &packed);
    const int path_bytes = packed & 0x3F;
    // Ask the sender to answer along the reverse of the way we reach it,
    // alternating with its own route when a direct attempt got nothing back.
    const bool with_path = s.reply_along_path && path_bytes <= VoxWire::kMaxReplyPathBytes;
    uint8_t req[VoxWire::kFetchMinLen + VoxWire::kMaxFetchIndices + 1 + VoxWire::kMaxReplyPathBytes];
    const int n = VoxWire::encodeFetch(s.sid, key6, any ? missing : nullptr, n_missing, req, sizeof req,
                                       true, with_path ? path : nullptr, path_bytes);
    sent = n > 0 && io_.sendRaw(path, packed, req, (size_t)n);
  } else if (m == kDm) {
    s.sent_dm = true;
    char text[200];
    const int n = VoxWire::encodeTextFetch(s.sid, key6, any ? missing : nullptr, n_missing, text, sizeof text);
    sent = n > 0 && io_.sendDmFlood(s.pub, text);
  } else {
    uint8_t req[VoxWire::kFetchMinLen + VoxWire::kMaxFetchIndices];
    const int n = VoxWire::encodeFetch(s.sid, key6, any ? missing : nullptr, n_missing, req, sizeof req, true);
    sent = n > 0 && io_.sendChannelData(s.channel, VoxWire::kChannelDataType, req, (size_t)n);
  }

  if (!sent) {
    if (m != kChannel) s.direct_failures++;
    if (s.attempts < kMaxAttempts) attempt(s);
    else fail(s, "couldn't send the request. Tap to retry.");
    return;
  }
  if (s.st != kReceiving) s.st = kRequesting;
  s.note = nullptr;
  arm(s, waitFor(s));
  io_.changed(s.sid);
}

void VoxFetch::onTimeout(Session& s) {
  if (s.st == kRemote) {
    // Unasked packets stopped arriving: finish it if most of it is here.
    const int got = s.storage ? s.asm_.received() : 0;
    if (shouldFinishPassive(got, s.total)) request(s.sid);
    return;
  }
  if (s.st != kRequesting && s.st != kReceiving) return;
  const int got = s.storage ? s.asm_.received() : 0;
  if (got > s.at_last_attempt) {
    s.stalls = 0;
  } else {
    s.stalls++;
    if (s.last != kChannel) s.direct_failures++;
    if (s.last == kDirect) s.reply_along_path = !s.reply_along_path;   // try the other way back
  }
  const bool channel_left = s.channel >= 0 && !s.tried_channel;
  if ((s.stalls < kMaxStalls || channel_left) && s.attempts < kMaxAttempts) {
    attempt(s);
    return;
  }
  fail(s, got == 0 ? "unavailable right now. Tap to retry." : "download stalled. Tap to retry.");
}

void VoxFetch::fail(Session& s, const char* why) {
  s.timer_on = false;
  s.st = kUnavailable;
  s.note = why;
  io_.changed(s.sid);
}

// --------------------------------------------------------------- packets

void VoxFetch::onPayload(const uint8_t* data, size_t len, int channel, uint8_t via, int hops) {
  if (!data || len == 0 || VoxWire::isFetchRequest(data, len)) return;   // this radio doesn't serve
  VoxWire::Packet p;
  if (!VoxWire::parsePacket(data, len, &p)) return;
  // Raw data doesn't say how far it came: count it as relayed.
  const bool relayed = channel < 0 || hops != 0;
  Session* s = find(p.sid);
  if (!s) {
    keepOrphan(p, via, relayed);
    return;
  }
  if (s->st == kComplete) return;
  if (relayed) s->relayed = true;
  accept(*s, p, via);
}

void VoxFetch::accept(Session& s, const VoxWire::Packet& p, uint8_t via) {
  if (!ensureStorage(s)) return;
  if (!s.asm_.add(p)) return;
  if (via == kViaMain || via == kViaRadio2) {
    if (s.via_count[via] < 0xFFFF) s.via_count[via]++;
  }
  s.last_touch = io_.nowMs();
  if (s.asm_.complete()) {
    complete(s);
    return;
  }
  // Packets for a photo nobody here asked for (another listener's channel
  // transfer) are kept quietly; they count when we do ask.
  if (s.st == kRequesting || s.st == kReceiving) {
    s.st = kReceiving;
    if (p.index == s.expected_last) {
      // The sender's burst is over, so the gaps are final: ask again soon.
      arm(s, gapRequestDelay(s.relayed, packetGap(s.last == kChannel, hops(s)), zeroHopGap()));
    } else {
      arm(s, waitFor(s));
    }
  } else if (s.st == kRemote && s.channel >= 0) {
    // Pushed, or sent for another listener: if it stops short, ask for the
    // rest. Everyone in range hears the same push stop, so spread out.
    const uint32_t gap = s.relayed ? packetGap(true, 0) : zeroHopGap();
    arm(s, stallWait(gap) + io_.random32() % 1500);
  }
  io_.changed(s.sid);
}

void VoxFetch::complete(Session& s) {
  s.timer_on = false;
  if (io_.savePayload(s.sid, s.asm_.payload(), s.asm_.payloadLen())) {
    s.st = kComplete;
    s.note = nullptr;
  } else {
    s.st = kUnavailable;
    s.note = "couldn't store it. Tap to retry.";
  }
  free(s.storage);
  s.storage = nullptr;
  s.asm_ = VoxWire::Assembly{};
  io_.changed(s.sid);
}

// --------------------------------------------------------------- status

VoxFetch::Status VoxFetch::status(uint32_t sid, int* received, int* total) const {
  const Session* s = find(sid);
  if (received) *received = 0;
  if (total) *total = 0;
  if (!s) return io_.hasPayload(sid) ? kComplete : kUnknown;
  if (total) *total = s->total;
  if (received) *received = s->st == kComplete ? s->total : (s->storage ? s->asm_.received() : 0);
  return s->st;
}

VoxFetch::Method VoxFetch::lastMethod(uint32_t sid) const {
  const Session* s = find(sid);
  return s ? s->last : kNone;
}

int VoxFetch::attempts(uint32_t sid) const {
  const Session* s = find(sid);
  return s ? s->attempts : 0;
}

uint8_t VoxFetch::via(uint32_t sid) const {
  const Session* s = find(sid);
  if (!s) return 0;
  return (uint8_t)((s->via_count[kViaMain] ? 1 : 0) | (s->via_count[kViaRadio2] ? 2 : 0));
}

const char* VoxFetch::viaText(uint8_t via) {
  switch (via & 3) {
    case 1: return "Received via main mesh";
    case 2: return "Received via MediaMesh";
    case 3: return "Received via MediaMesh and main mesh";
    default: return nullptr;
  }
}

const char* VoxFetch::statusText(uint32_t sid, char* buf, size_t cap) const {
  if (!buf || cap == 0) return "";
  int got = 0, total = 0;
  const Status st = status(sid, &got, &total);
  const Session* s = find(sid);
  const bool voice = s && s->kind != VoxWire::kImage;
  const char* noun = voice ? "Voice" : "Photo";
  char ready[32];
  if (voice) snprintf(ready, sizeof ready, "Voice %us - tap to play", (unsigned)s->aspect);
  switch (st) {
    case kUnknown:
    case kRemote:
      // Pushed packets arriving unasked show as a download in progress.
      if (got > 0) snprintf(buf, cap, "Downloading %d/%d", got, total);
      else snprintf(buf, cap, "%s", voice ? ready : "Photo - tap to download");
      break;
    case kRequesting:
      snprintf(buf, cap, "%s", !s ? "Requesting..."
                             : s->last == kDm      ? "Finding a route to the sender..."
                             : s->last == kChannel ? "Requesting on the channel..."
                                                   : "Requesting from the sender...");
      break;
    case kReceiving:
      snprintf(buf, cap, "Downloading %d/%d", got, total);
      break;
    case kComplete:
      snprintf(buf, cap, "%s", voice ? ready : "Photo - tap to view");
      break;
    case kUnavailable:
      snprintf(buf, cap, "%s %s", noun, s && s->note ? s->note : "unavailable. Tap to retry.");
      break;
  }
  return buf;
}
