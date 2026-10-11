// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "ui-touch/VoxWire.h"

using namespace VoxWire;

static const uint8_t kKey6[6] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6};

// Packetizes like voxPacketize() in meshcore-open-tak (156-byte slices).
static int packetize(uint32_t sid, const uint8_t* payload, int len, uint8_t pkts[][kPacketHeaderLen + kBytesPerPacket + 1],
                     int* lens, bool salted) {
  int n = 0;
  for (int off = 0; off < len; off += kBytesPerPacket, n++) {
    const int chunk = len - off < kBytesPerPacket ? len - off : kBytesPerPacket;
    pkts[n][0] = salted ? kPacketSaltedMagic : kPacketMagic;
    writeSid(sid, pkts[n] + 1);
    pkts[n][5] = (uint8_t)n;
    memcpy(pkts[n] + kPacketHeaderLen, payload + off, (size_t)chunk);
    lens[n] = kPacketHeaderLen + chunk;
    if (salted) pkts[n][lens[n]++] = (uint8_t)(0x5A ^ n);
  }
  return n;
}

int main() {
  // --- envelope (expected strings computed independently of this code)
  Envelope env;
  assert(parseImageEnvelope("VI1:79e6a5:14:10:46", &env));
  assert(env.sid == 0x1a2b3c4d && env.total == 36 && env.aspect == 150);
  assert(parseImageEnvelope("  VI1:79e6a5:14:10:46\n", &env));
  assert(!parseImageEnvelope("VE3:79e6a5:1:a:5", &env));        // voice, not a photo
  assert(!parseImageEnvelope("VI1:79e6a5:w:10:46", &env));        // Mimi mode id under VI1
  assert(!parseImageEnvelope("VI1:79e6a5:14:0:46", &env));        // zero packets
  assert(!parseImageEnvelope("VI1:79e6a5:14:10", &env));          // missing field
  assert(!parseImageEnvelope("VI1:79e6a5:14:10:46:x", &env));     // extra field
  assert(!parseImageEnvelope("VI1:79E6A5:14:10:46", &env));       // uppercase isn't base36 here
  assert(!parseImageEnvelope("VI1:79e6a5:14:10:46 hi", &env));    // trailing text
  assert(!parseImageEnvelope("VI1:zzzzzzzz:14:10:46", &env));     // sid too long
  assert(!parseImageEnvelope(nullptr, &env));

  // --- text fetch request
  char txt[128];
  const uint8_t miss[] = {35, 2, 1, 3, 2};
  assert(encodeTextFetch(0x1a2b3c4d, kKey6, miss, 5, txt, sizeof txt) > 0);
  assert(strcmp(txt, "VR3:79e6a5:m1-3.z:a1b2c3d4e5f6") == 0);
  assert(encodeTextFetch(0x1a2b3c4d, kKey6, nullptr, 0, txt, sizeof txt) > 0);
  assert(strcmp(txt, "VR3:79e6a5:a:a1b2c3d4e5f6") == 0);
  assert(encodeTextFetch(0x1a2b3c4d, kKey6, nullptr, 0, txt, 10) == 0);

  // --- binary fetch request
  uint8_t req[200];
  int n = encodeFetch(0x1a2b3c4d, kKey6, miss, 5, req, sizeof req);
  const uint8_t want[] = {0x72, 0x1a, 0x2b, 0x3c, 0x4d, 0x01, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 4, 1, 2, 3, 35};
  assert(n == (int)sizeof want && memcmp(req, want, sizeof want) == 0);
  n = encodeFetch(0x1a2b3c4d, kKey6, nullptr, 0, req, sizeof req);
  assert(n == kFetchMinLen && req[5] == 0x00 && req[12] == 0);
  uint8_t many[200];
  for (int i = 0; i < 200; i++) many[i] = (uint8_t)i;
  n = encodeFetch(1, kKey6, many, 200, req, sizeof req);
  assert(n == kFetchMinLen + kMaxFetchIndices && req[12] == kMaxFetchIndices);
  assert(encodeFetch(1, kKey6, nullptr, 0, req, 12) == 0);

  // --- packet parsing
  uint8_t raw[] = {0x56, 0, 0, 0, 7, 3, 9, 8, 7};
  Packet p;
  assert(parsePacket(raw, sizeof raw, &p) && p.sid == 7 && p.index == 3 && p.len == 3 && p.data[0] == 9);
  uint8_t salted[] = {0x57, 0, 0, 0, 7, 3, 9, 8, 7, 0xEE};
  assert(parsePacket(salted, sizeof salted, &p) && p.len == 3 && p.data[2] == 7);
  uint8_t empty[] = {0x56, 0, 0, 0, 7, 3};
  assert(!parsePacket(empty, sizeof empty, &p));
  assert(!parsePacket(want, sizeof want, &p));                    // a fetch request isn't data

  // --- reassembly of a 5,350-byte "More detail" payload, out of order, with
  //     duplicates, salted resends and another session's packets mixed in.
  const int kLen = 5350;
  uint8_t* payload = (uint8_t*)malloc(kLen);
  payload[0] = kImagePayloadVersion;
  for (int i = 1; i < kLen; i++) payload[i] = (uint8_t)(i * 31 + 7);
  static uint8_t pk[64][kPacketHeaderLen + kBytesPerPacket + 1];
  static uint8_t pks[64][kPacketHeaderLen + kBytesPerPacket + 1];
  int lens[64], lens_s[64];
  const uint32_t sid = 0xdeadbeef;
  const int total = packetize(sid, payload, kLen, pk, lens, false);
  packetize(sid, payload, kLen, pks, lens_s, true);
  assert(total == 35);

  static uint8_t store[64 * kBytesPerPacket];
  Assembly a;
  assert(!a.begin(sid, 0, store, sizeof store));
  assert(!a.begin(sid, (uint8_t)(kMaxImagePackets + 1), store, sizeof store));
  assert(!a.begin(sid, (uint8_t)total, store, 10));
  assert(a.begin(sid, (uint8_t)total, store, sizeof store));

  for (int i = total - 1; i >= 0; i -= 2) {                       // odd ones, backwards
    assert(parsePacket(pk[i], (size_t)lens[i], &p));
    assert(a.add(p));
    assert(!a.add(p));                                            // duplicate ignored
  }
  assert(!a.complete());
  uint8_t gaps[kMaxPackets];
  int g = a.missing(gaps);
  assert(g == total / 2 && gaps[0] == 1);

  uint8_t other[16] = {0x56, 0, 0, 0, 1, 1, 1};                  // wrong session
  assert(parsePacket(other, 8, &p) && !a.add(p));
  uint8_t shortmid[] = {0x56, 0xde, 0xad, 0xbe, 0xef, 1, 1, 2};   // a middle packet must be full
  assert(parsePacket(shortmid, sizeof shortmid, &p) && !a.add(p));

  for (int i = 0; i < g; i++) {                                   // salted resends fill the gaps
    assert(parsePacket(pks[gaps[i]], (size_t)lens_s[gaps[i]], &p));
    assert(a.add(p));
  }
  assert(a.complete() && a.received() == total);
  assert(a.payloadLen() == (size_t)kLen && memcmp(a.payload(), payload, kLen) == 0);

  size_t obu_len = 0;
  const uint8_t* obus = imageObus(a.payload(), a.payloadLen(), &obu_len);
  assert(obus == a.payload() + 1 && obu_len == (size_t)kLen - 1);
  uint8_t badver[] = {0x02, 0x12};
  assert(!imageObus(badver, 2, &obu_len));

  free(payload);
  return 0;
}
