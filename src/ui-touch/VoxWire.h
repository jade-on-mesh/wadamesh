// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Wire formats for MeshCore Open TAK photo and voice messages (VOX), so a
// wadamesh radio can fetch photos and voice clips sent from that app. Mirrors
// lib/vox/voice_wire.dart in jade-on-mesh/meshcore-open-tak. Pure C++, no
// Arduino, so it is host-testable (test/test_vox_wire.cpp).
//
//   Envelope (ordinary text message, DM or channel), all fields lowercase base36:
//     VE3:{sid}:{mode}:{total}:{seconds}   Codec2 voice (the published VE3 format)
//     VM1:{sid}:{mode}:{total}:{seconds}   Mimi neural voice (VOX only)
//     VI1:{sid}:{mode}:{total}:{aspect}    AV1 still image (VOX only)
//       sid    32-bit session id
//       mode   VE3: Codec2 mode 0..6 (700C, 1200, 2400, 1300, 1400, 1600, 3200);
//              VM1: 32 ("w"); VI1: 40 ("14")
//       total  packet count, 1..255
//       last   clip length in whole seconds, or for a photo width * 100 / height
//
//   Data packet (raw data on a direct route, or GRP_DATA type 0x5643 on a channel):
//     [0x56][sid:4 BE][index:1][bytes...]          plain
//     [0x57][sid:4 BE][index:1][bytes...][salt:1]  salted resend
//   Every packet but the last carries packetBytes(mode): as many whole codec
//   frames as fit in 156 bytes (156 for photos).
//
//   Fetch request (same planes as the data):
//     [0x72][sid:4 BE][flags:1][requesterKey6:6][count:1][missing indices...]
//       flags bit0 = only the listed indices are wanted; otherwise all.
//
//   Text fetch request (flooded DM when there is no direct route):
//     VR3:{sid}:{a | m<ranges>}:{requesterKey6 hex}
//
// Reassembled payload: a photo is [0x01 version][AV1 OBUs, temporal delimiter
// stripped]; a voice clip is the codec's frames back to back.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace VoxWire {

constexpr uint8_t  kPacketMagic       = 0x56;   // 'V'
constexpr uint8_t  kPacketSaltedMagic = 0x57;   // 'W'
constexpr uint8_t  kFetchMagic        = 0x72;   // 'r'
constexpr uint8_t  kFetchFlagMissing  = 0x01;
constexpr uint8_t  kFetchFlagSaltOk   = 0x02;   // requester takes salted (0x57) resends
constexpr uint8_t  kFetchFlagHasPath  = 0x04;   // [len][path] follows: requester -> sender route
constexpr int      kMaxReplyPathBytes = 8;      // senders ignore longer reply paths
constexpr uint16_t kChannelDataType   = 0x5643; // 'VC'
constexpr int      kImageModeId       = 40;
constexpr int      kBytesPerPacket    = 156;
constexpr int      kPacketHeaderLen   = 6;
constexpr int      kMaxPackets        = 255;
constexpr int      kFetchMinLen       = 13;
constexpr int      kMaxFetchIndices   = 140;    // keeps a request (and a reply path) inside one packet
constexpr uint8_t  kImagePayloadVersion = 0x01;

constexpr int      kMimiModeId        = 32;

// Largest clip Open TAK sends today is a "More detail" photo (36 packets) or 20 s
// of Codec2 3200 (54 packets). Allow some growth without letting a hostile
// envelope make us reserve 40 KB.
constexpr int      kMaxClipPackets    = 64;
constexpr int      kMaxImagePackets   = kMaxClipPackets;

enum Kind : uint8_t { kImage = 0, kCodec2, kMimi };

// Bytes in one codec frame for a wire mode id, or 0 for an unknown mode.
inline int bytesPerFrame(int mode) {
  switch (mode) {
    case 0: return 4;                       // 700C
    case 1: case 2: return 6;               // 1200, 2400
    case 3: case 4: return 7;               // 1300, 1400
    case 5: case 6: return 8;               // 1600, 3200
    case kMimiModeId: return 11;
    case kImageModeId: return 1;
    default: return 0;
  }
}
// Codec bytes in every packet but the last: whole frames that fit in 156.
inline int packetBytes(int mode) {
  const int f = bytesPerFrame(mode);
  return f ? (kBytesPerPacket / f) * f : 0;
}
// The CODEC2_MODE_* value for a VE3 wire mode id, or -1.
inline int codec2Mode(int mode) {
  static const int8_t ids[7] = {8, 5, 1, 4, 3, 2, 0};   // 700C 1200 2400 1300 1400 1600 3200
  return mode >= 0 && mode <= 6 ? ids[mode] : -1;
}

struct Envelope {
  uint32_t sid = 0;
  uint8_t  total = 0;    // packets
  uint16_t aspect = 0;   // photo: width * 100 / height; voice: length in seconds
  uint8_t  mode = kImageModeId;   // wire mode id
  Kind     kind = kImage;
  int packetBytes() const { return VoxWire::packetBytes(mode); }
  uint16_t seconds() const { return kind == kImage ? 0 : aspect; }
};

inline bool parseBase36(const char* s, size_t n, uint32_t* out) {
  if (n == 0 || n > 7) return false;
  uint32_t v = 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'z') d = (uint32_t)(c - 'a' + 10);
    else return false;
    uint64_t nv = (uint64_t)v * 36u + d;
    if (nv > 0xFFFFFFFFull) return false;
    v = (uint32_t)nv;
  }
  *out = v;
  return true;
}

// Writes `v` in lowercase base36 (at most 7 chars + NUL). Returns chars written.
inline int toBase36(uint32_t v, char* out) {
  char tmp[8];
  int n = 0;
  do { uint32_t d = v % 36u; tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= 36u; } while (v);
  for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  out[n] = '\0';
  return n;
}

// Parses a VE3 / VM1 / VI1 envelope (surrounding whitespace allowed).
inline bool parseEnvelope(const char* text, Envelope* env) {
  if (!text) return false;
  while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
  Kind kind;
  if (strncmp(text, "VI1:", 4) == 0) kind = kImage;
  else if (strncmp(text, "VE3:", 4) == 0) kind = kCodec2;
  else if (strncmp(text, "VM1:", 4) == 0) kind = kMimi;
  else return false;
  const char* p = text + 4;
  uint32_t f[4];
  for (int k = 0; k < 4; k++) {
    const char* start = p;
    while (*p && *p != ':' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    if (!parseBase36(start, (size_t)(p - start), &f[k])) return false;
    if (k < 3) { if (*p != ':') return false; p++; }
  }
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (*p) return false;
  // The mode must belong to the prefix, as voice_wire.dart checks.
  if (kind == kImage && f[1] != (uint32_t)kImageModeId) return false;
  if (kind == kMimi && f[1] != (uint32_t)kMimiModeId) return false;
  if (kind == kCodec2 && f[1] > 6) return false;
  if (f[2] < 1 || f[2] > (uint32_t)kMaxPackets) return false;
  if (f[3] > 600) return false;   // VE3's duration range; photo aspect is clamped to 20..600
  env->sid = f[0];
  env->total = (uint8_t)f[2];
  env->aspect = (uint16_t)f[3];
  env->mode = (uint8_t)f[1];
  env->kind = kind;
  return true;
}

// True for a well-formed VI1 photo envelope.
inline bool parseImageEnvelope(const char* text, Envelope* env) {
  Envelope e;
  if (!parseEnvelope(text, &e) || e.kind != kImage) return false;
  *env = e;
  return true;
}

struct Packet {
  uint32_t sid;
  uint8_t  index;
  const uint8_t* data;
  int      len;
};

inline uint32_t readSid(const uint8_t* b) {
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
inline void writeSid(uint32_t sid, uint8_t* b) {
  b[0] = (uint8_t)(sid >> 24); b[1] = (uint8_t)(sid >> 16); b[2] = (uint8_t)(sid >> 8); b[3] = (uint8_t)sid;
}

// Parses a data packet (plain or salted). `pkt->data` points into `buf`.
inline bool parsePacket(const uint8_t* buf, size_t len, Packet* pkt) {
  if (!buf || len == 0) return false;
  if (buf[0] != kPacketMagic && buf[0] != kPacketSaltedMagic) return false;
  const size_t end = buf[0] == kPacketSaltedMagic ? len - 1 : len;
  if (end <= (size_t)kPacketHeaderLen) return false;
  pkt->sid = readSid(buf + 1);
  pkt->index = buf[5];
  pkt->data = buf + kPacketHeaderLen;
  pkt->len = (int)(end - kPacketHeaderLen);
  return pkt->index < kMaxPackets;
}

// Binary fetch request. `missing` == nullptr asks for every packet. Indices are
// written sorted and de-duplicated, capped at kMaxFetchIndices. `salt_ok` lets
// the sender salt resends (0x57). `reply_path` (requester -> sender, 1-byte
// hashes) asks the sender to answer along the reverse of the route the request
// took; pass nullptr to let it use its own route to us. Returns bytes written,
// or 0 if `cap` is too small.
inline int encodeFetch(uint32_t sid, const uint8_t key6[6], const uint8_t* missing, int n_missing,
                       uint8_t* out, size_t cap, bool salt_ok = false,
                       const uint8_t* reply_path = nullptr, int reply_path_len = 0) {
  uint8_t sorted[kMaxPackets];
  int n = 0;
  if (missing) {
    bool seen[kMaxPackets] = {false};
    for (int i = 0; i < n_missing; i++) if (missing[i] < kMaxPackets) seen[missing[i]] = true;
    for (int v = 0; v < kMaxPackets && n < kMaxFetchIndices; v++) if (seen[v]) sorted[n++] = (uint8_t)v;
  }
  // A non-null path of length 0 is meaningful: the sender is our direct neighbour.
  const bool has_path = reply_path != nullptr && reply_path_len >= 0 &&
                        reply_path_len <= kMaxReplyPathBytes;
  const size_t need = (size_t)kFetchMinLen + (size_t)n + (has_path ? 1 + (size_t)reply_path_len : 0);
  if (cap < need) return 0;
  out[0] = kFetchMagic;
  writeSid(sid, out + 1);
  out[5] = (uint8_t)((missing ? kFetchFlagMissing : 0) | (salt_ok ? kFetchFlagSaltOk : 0) |
                     (has_path ? kFetchFlagHasPath : 0));
  memcpy(out + 6, key6, 6);
  out[12] = (uint8_t)n;
  if (n) memcpy(out + kFetchMinLen, sorted, (size_t)n);
  if (has_path) {
    out[kFetchMinLen + n] = (uint8_t)reply_path_len;
    if (reply_path_len) memcpy(out + kFetchMinLen + n + 1, reply_path, (size_t)reply_path_len);
  }
  return (int)need;
}

// A binary fetch request (data packets start 0x56 / 0x57).
inline bool isFetchRequest(const uint8_t* buf, size_t len) {
  return buf && len >= (size_t)kFetchMinLen && buf[0] == kFetchMagic;
}

// "VR3:" DMs are fetch requests between apps; they never belong in a chat.
inline bool isControlText(const char* text) { return text && strncmp(text, "VR3:", 4) == 0; }

// VR3 text request. Ranges are written like the Dart side: "m1-3.7". Returns
// chars written (excluding NUL), or 0 if `cap` is too small.
inline int encodeTextFetch(uint32_t sid, const uint8_t key6[6], const uint8_t* missing, int n_missing,
                           char* out, size_t cap) {
  char buf[512];
  int pos = snprintf(buf, sizeof buf, "VR3:");
  pos += toBase36(sid, buf + pos);
  buf[pos++] = ':';
  bool seen[kMaxPackets] = {false};
  int n = 0;
  if (missing) for (int i = 0; i < n_missing; i++) if (missing[i] < kMaxPackets && !seen[missing[i]]) { seen[missing[i]] = true; n++; }
  if (n == 0) {
    buf[pos++] = 'a';
  } else {
    buf[pos++] = 'm';
    bool first = true;
    for (int v = 0; v < kMaxPackets; ) {
      if (!seen[v]) { v++; continue; }
      int e = v;
      while (e + 1 < kMaxPackets && seen[e + 1]) e++;
      if (!first) buf[pos++] = '.';
      first = false;
      pos += toBase36((uint32_t)v, buf + pos);
      if (e > v) { buf[pos++] = '-'; pos += toBase36((uint32_t)e, buf + pos); }
      v = e + 1;
      if (pos > (int)sizeof buf - 32) return 0;
    }
  }
  buf[pos++] = ':';
  for (int i = 0; i < 6; i++) pos += snprintf(buf + pos, sizeof buf - pos, "%02x", key6[i]);
  if ((size_t)pos + 1 > cap) return 0;
  memcpy(out, buf, (size_t)pos);
  out[pos] = '\0';
  return pos;
}

// Collects one clip's packets into caller-owned storage of
// total * packet_bytes bytes (PSRAM on the device).
class Assembly {
 public:
  bool begin(uint32_t sid, uint8_t total, uint8_t* storage, size_t storage_len,
             int packet_bytes = kBytesPerPacket) {
    if (total < 1 || total > kMaxClipPackets) return false;
    if (packet_bytes < 1 || packet_bytes > kBytesPerPacket) return false;
    if (!storage || storage_len < (size_t)total * (size_t)packet_bytes) return false;
    sid_ = sid; total_ = total; buf_ = storage; last_len_ = -1; have_count_ = 0; pb_ = packet_bytes;
    memset(have_, 0, sizeof have_);
    return true;
  }

  // Returns true if the packet belonged to this photo and was new.
  bool add(const Packet& p) {
    if (!buf_ || p.sid != sid_ || p.index >= total_) return false;
    const bool last = p.index == total_ - 1;
    if (last ? (p.len < 1 || p.len > pb_) : p.len != pb_) return false;
    if (have(p.index)) return false;
    memcpy(buf_ + (size_t)p.index * (size_t)pb_, p.data, (size_t)p.len);
    if (last) last_len_ = p.len;
    have_[p.index >> 3] |= (uint8_t)(1u << (p.index & 7));
    have_count_++;
    return true;
  }

  bool have(uint8_t i) const { return (have_[i >> 3] >> (i & 7)) & 1; }
  bool complete() const { return buf_ && have_count_ == total_; }
  int  received() const { return have_count_; }
  uint8_t total() const { return total_; }
  uint32_t sid() const { return sid_; }

  // Missing indices into `out` (capacity kMaxPackets). Returns the count.
  int missing(uint8_t* out) const {
    int n = 0;
    for (int i = 0; i < total_; i++) if (!have((uint8_t)i)) out[n++] = (uint8_t)i;
    return n;
  }

  // The reassembled payload; valid once complete().
  const uint8_t* payload() const { return buf_; }
  size_t payloadLen() const {
    return complete() ? (size_t)(total_ - 1) * (size_t)pb_ + (size_t)last_len_ : 0;
  }

 private:
  uint32_t sid_ = 0;
  uint8_t  total_ = 0;
  uint8_t* buf_ = nullptr;
  int      last_len_ = -1;
  int      have_count_ = 0;
  int      pb_ = kBytesPerPacket;
  uint8_t  have_[(kMaxPackets + 7) / 8] = {0};
};

// The AV1 OBUs inside a complete payload, or nullptr if this build can't read it.
inline const uint8_t* imageObus(const uint8_t* payload, size_t len, size_t* obu_len) {
  if (!payload || len < 2 || payload[0] != kImagePayloadVersion) return nullptr;
  *obu_len = len - 1;
  return payload + 1;
}

}  // namespace VoxWire
