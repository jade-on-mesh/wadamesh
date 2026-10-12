// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoxPhotos.h"

#include <Arduino.h>
#include <FS.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../MyMesh.h"
#include "ChannelSenderSplit.h"

namespace VoxPhotos {
namespace {

fs::FS* g_fs = nullptr;
uint32_t g_version = 0;

void storePath(uint32_t sid, char* out, size_t cap) {
  snprintf(out, cap, "/vox_%08lx.vi1", (unsigned long)sid);
}

// Case-insensitive name match that ignores emoji variation selectors (U+FE0E,
// U+FE0F), as the app does when it matches channel senders to contacts.
void normName(const char* in, char* out, size_t cap) {
  size_t o = 0;
  for (const unsigned char* p = (const unsigned char*)in; *p && o + 1 < cap;) {
    if (p[0] == 0xEF && p[1] == 0xB8 && (p[2] == 0x8E || p[2] == 0x8F)) { p += 3; continue; }
    out[o++] = (char)tolower(*p++);
  }
  out[o] = '\0';
  while (o && out[o - 1] == ' ') out[--o] = '\0';
}

class MeshIo : public VoxFetchIo {
 public:
  uint32_t nowMs() override { return millis(); }

  void selfKey6(uint8_t out[6]) override { memcpy(out, the_mesh.getSelfPubKey(), 6); }

  bool contactRoute(const uint8_t pub[32], uint8_t* path, uint8_t* packed) override {
    return the_mesh.voxContactRoute(pub, path, packed);
  }
  int contactHops(const uint8_t pub[32]) override { return the_mesh.voxContactHops(pub); }

  bool contactByName(const char* name, uint8_t pub_out[32]) override {
    char want[40], have[40];
    normName(name, want, sizeof want);
    if (!want[0]) return false;
    const int n = the_mesh.getNumContacts();
    for (int i = 0; i < n; i++) {
      ContactInfo c;
      if (!the_mesh.getContactByIdx((uint32_t)i, c)) continue;
      normName(c.name, have, sizeof have);
      if (strcmp(want, have) == 0) {
        memcpy(pub_out, c.id.pub_key, 32);
        return true;
      }
    }
    return false;
  }

  bool sendRaw(const uint8_t* path, uint8_t packed, const uint8_t* data, size_t len) override {
    if ((packed >> 6) != 0) return false;   // raw data takes 1-byte hashes only
    return the_mesh.voxSendRaw(path, packed, data, len);
  }

  bool sendDmFlood(const uint8_t pub[32], const char* text) override {
    // Flood it even when we have a (possibly one-way) route, so the sender's
    // radio learns a route back to us from the path exchange.
    the_mesh.voxForgetRoute(pub);
    ContactInfo c;
    if (!the_mesh.voxContact(pub, c)) return false;
    uint32_t expected_ack = 0, est_timeout = 0;
    const uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
    return the_mesh.sendMessage(c, ts, 0, text, expected_ack, est_timeout) != MSG_SEND_FAILED;
  }

  bool sendChannelData(int ch, uint16_t type, const uint8_t* data, size_t len) override {
    return the_mesh.voxSendChannelData(ch, type, data, len);
  }

  // One full photo packet on air at the radio's current settings (the app's
  // voxEstimateAirtime, with no path bytes).
  uint32_t packetAirMs() override {
    const NodePrefs* p = the_mesh.getNodePrefs();
    const int sf = p && p->sf ? p->sf : 8;
    const double bw = p && p->bw > 0 ? p->bw : 62.5;
    const int cr = p && p->cr >= 5 && p->cr <= 8 ? p->cr : 5;
    const int payload = 2 + VoxWire::kPacketHeaderLen + VoxWire::kBytesPerPacket;
    const double t_sym = pow(2.0, sf) / (bw * 1000.0);
    const int de = (sf >= 11 && bw <= 125) ? 1 : 0;
    const double num = 8.0 * payload - 4.0 * sf + 28 + 16;
    const double den = 4.0 * (sf - 2 * de);
    const double n_payload = 8 + fmax(ceil(num / den) * (cr - 4 + 4), 0);
    return (uint32_t)((8 + 4.25 + n_payload) * t_sym * 1000.0);
  }

  bool hasPayload(uint32_t sid) override {
    if (!g_fs) return false;
    char path[32];
    storePath(sid, path, sizeof path);
    return g_fs->exists(path);
  }

  bool savePayload(uint32_t sid, const uint8_t* data, size_t len) override {
    if (!g_fs) return false;
    char path[32];
    storePath(sid, path, sizeof path);
    File f = g_fs->open(path, "w");
    if (!f) return false;
    const size_t w = f.write(data, len);
    f.close();
    if (w != len) { g_fs->remove(path); return false; }
    Serial.printf("[VOX] photo %08lx complete, %u bytes\n", (unsigned long)sid, (unsigned)len);
    return true;
  }

  void changed(uint32_t) override { g_version++; }
};

MeshIo g_io;
VoxFetch g_fetch(g_io);

bool parseChannelPost(const char* text, char* name, size_t name_cap, VoxWire::Envelope* env) {
  const char* body = ChannelSenderSplit::split(text, name, name_cap);
  return VoxWire::parseImageEnvelope(body, env);
}

}  // namespace

void setStore(fs::FS* fs) { g_fs = fs; }

void onDirectText(const char* text, const uint8_t sender_pub[32]) {
  VoxWire::Envelope env;
  if (!VoxWire::parseImageEnvelope(text, &env)) return;
  g_fetch.onEnvelope(env, sender_pub, nullptr, -1, false);
}

void onChannelText(const char* text, int channel_idx) {
  char name[ChannelSenderSplit::kMaxWireName + 1];
  VoxWire::Envelope env;
  if (!parseChannelPost(text, name, sizeof name, &env)) return;
  g_fetch.onEnvelope(env, nullptr, name, channel_idx, false);
}

void onPayload(const uint8_t* data, size_t len, int channel_idx) { g_fetch.onPayload(data, len, channel_idx); }

void tick() { g_fetch.tick(); }

bool parse(const char* text, VoxWire::Envelope* env) { return VoxWire::parseImageEnvelope(text, env); }

void request(const VoxWire::Envelope& env, const uint8_t* dm_pub, const char* sender_name,
             int channel_idx) {
  g_fetch.onEnvelope(env, dm_pub, sender_name, channel_idx, false);
  g_fetch.request(env.sid);
}

VoxFetch::Status status(uint32_t sid, int* received, int* total) {
  return g_fetch.status(sid, received, total);
}

const char* statusText(uint32_t sid, char* buf, size_t cap) { return g_fetch.statusText(sid, buf, cap); }

bool load(uint32_t sid, uint8_t** data, size_t* len) {
  if (!g_fs || !data || !len) return false;
  char path[32];
  storePath(sid, path, sizeof path);
  File f = g_fs->open(path, "r");
  if (!f) return false;
  const size_t n = f.size();
  if (n < 2 || n > 64 * 1024) { f.close(); return false; }
  uint8_t* b = (uint8_t*)malloc(n);
  if (!b) { f.close(); return false; }
  const size_t r = f.read(b, n);
  f.close();
  if (r != n) { free(b); return false; }
  *data = b;
  *len = n;
  return true;
}

uint32_t version() { return g_version; }

}  // namespace VoxPhotos
