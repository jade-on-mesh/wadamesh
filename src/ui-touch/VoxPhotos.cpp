// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoxPhotos.h"

#include <esp_heap_caps.h>
#include <new>

#include <Arduino.h>
#include <FS.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../MyMesh.h"
#include "ChannelSenderSplit.h"
#include "VoxServe.h"
#include "VoxVoice.h"
#include "device_caps.h"

namespace VoxPhotos {
namespace {

fs::FS* g_fs = nullptr;
// Folder the files go in: "" (the root) on internal flash, "/meshcomod" when
// they follow the data store onto the SD card.
char g_root[24] = "";
uint32_t g_version = 0;
// Photos and talks whose fetch changed since stateVersion() last looked, and the
// status each was last seen in: a packet arriving changes only the progress, a
// new status (requesting, complete, failed) is what the chat must show soon.
uint32_t g_state_version = 0;
uint32_t g_touched[8];
int g_touched_n = 0;
struct LastSt { uint32_t sid; uint8_t st; };
LastSt g_last_st[16];
int g_last_st_next = 0;
void touched(uint32_t sid) {
  g_version++;
  for (int i = 0; i < g_touched_n; i++) if (g_touched[i] == sid) return;
  if (g_touched_n < 8) g_touched[g_touched_n++] = sid;
  else g_state_version++;   // too many at once: treat it as a real change
}
void bumpState() { g_version++; g_state_version++; }

// What each recent envelope was, so a finished clip is stored the right way:
// a photo as its payload (/vox_<sid>.vi1, opens in Files), a voice clip as
// [wire mode][codec frames] (/vox_<sid>.ve3).
struct Seen { uint32_t sid; VoxWire::Kind kind; uint8_t mode; };
Seen g_seen[16];
int g_seen_next = 0;

void remember(const VoxWire::Envelope& env) {
  for (auto& e : g_seen) if (e.sid == env.sid && (e.sid || e.mode)) { e.kind = env.kind; e.mode = env.mode; return; }
  g_seen[g_seen_next] = Seen{env.sid, env.kind, env.mode};
  g_seen_next = (g_seen_next + 1) % (int)(sizeof g_seen / sizeof g_seen[0]);
}
const Seen* seen(uint32_t sid) {
  for (auto& e : g_seen) if (e.sid == sid && (e.sid || e.mode)) return &e;
  return nullptr;
}

void storePath(uint32_t sid, bool voice, char* out, size_t cap) {
  snprintf(out, cap, voice ? "%s/vox_%08lx.ve3" : "%s/vox_%08lx.vi1", g_root, (unsigned long)sid);
}

// Photos and clips live on internal flash and nothing else ever removes them,
// so a radio that has been on the air long enough fills it: the download
// completes and then "couldn't store it", and a new recording can't be kept.
// When a write doesn't fit, the oldest stored photo or clip (other than the
// one being written) makes room, and the write is tried again. Only our own
// /vox_<sid>.vi1|.ve3 files are touched: OTP pads, settings and the rest of
// internal storage are never removed for this.
// Whether a photo or clip is stored, remembered. Every photo and voice bubble
// asks (download state, "tap to play", how it got here), several times each,
// on every layout of the open chat: all of the channel's bubbles, not just the
// visible ones, and once a second while a transfer runs. Each ask was a
// filesystem exists(), which SPIFFS answers by scanning its page tables (slow,
// and slower as it fills) and the SD card over the bus the T-Deck's screen and
// radio share: opening a channel with many photos and clips took many seconds.
// The answers live here; every write and remove below keeps them true.
struct HaveEntry { uint32_t sid; uint8_t bits; };   // bits: 1/4 photo/voice known, 2/8 present
static HaveEntry g_have[128];
static uint32_t g_fs_checks = 0;   // exists() calls made (About: chat open timing)
static uint8_t g_have_next = 0;

static void haveClear() { memset(g_have, 0, sizeof g_have); g_have_next = 0; }

static HaveEntry* haveFind(uint32_t sid, bool add) {
  for (auto& h : g_have) if (h.bits && h.sid == sid) return &h;
  if (!add) return nullptr;
  HaveEntry& h = g_have[g_have_next];
  g_have_next = (uint8_t)((g_have_next + 1) % (sizeof g_have / sizeof g_have[0]));
  h.sid = sid;
  h.bits = 0;
  return &h;
}

static void haveSet(uint32_t sid, bool voice, bool present) {
  HaveEntry* h = haveFind(sid, true);
  const uint8_t known = voice ? 4 : 1, has = voice ? 8 : 2;
  h->bits = (uint8_t)((h->bits & ~has) | known | (present ? has : 0));
}

// From a store path ("<root>/vox_<sid>.ve3|.vi1"): its sid and kind.
static bool havePathKey(const char* path, uint32_t* sid, bool* voice) {
  const char* b = strrchr(path, '/');
  b = b ? b + 1 : path;
  if (strlen(b) != 16 || strncmp(b, "vox_", 4)) return false;
  char hex[9];
  memcpy(hex, b + 4, 8);
  hex[8] = '\0';
  *sid = (uint32_t)strtoul(hex, nullptr, 16);
  *voice = !strcmp(b + 12, ".ve3");
  return true;
}

static void haveSetPath(const char* path, bool present) {
  uint32_t sid; bool voice;
  if (havePathKey(path, &sid, &voice)) haveSet(sid, voice, present);
}

static bool storeHas(uint32_t sid, bool voice) {
  if (!g_fs) return false;
  const uint8_t known = voice ? 4 : 1, has = voice ? 8 : 2;
  if (const HaveEntry* h = haveFind(sid, false)) {
    if (h->bits & known) return (h->bits & has) != 0;
  }
  char path[40];
  storePath(sid, voice, path, sizeof path);
  ++g_fs_checks;
  const bool present = g_fs->exists(path);
  haveSet(sid, voice, present);
  return present;
}

static bool isVoxFile(const char* n) {
  const size_t l = strlen(n);
  return l == 16 && !strncmp(n, "vox_", 4) && (!strcmp(n + 12, ".ve3") || !strcmp(n + 12, ".vi1"));
}

static const char* baseName(const char* p) {
  const char* b = strrchr(p, '/');
  return b ? b + 1 : p;
}

static bool pruneOldestVox(const char* keep) {
  File dir = g_fs->open(g_root[0] ? g_root : "/");
  if (!dir || !dir.isDirectory()) return false;
  char oldest[48] = "";
  time_t oldest_t = 0;
  int n = 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const char* nm = baseName(f.name());
    if (!f.isDirectory() && isVoxFile(nm) && strcmp(nm, baseName(keep))) {
      ++n;
      const time_t t = f.getLastWrite();   // 0 without timestamps: the first listed goes
      if (!oldest[0] || t < oldest_t) { snprintf(oldest, sizeof oldest, "%s/%s", g_root, nm); oldest_t = t; }
    }
    f.close();
  }
  dir.close();
  if (!oldest[0]) return false;
  const bool ok = g_fs->remove(oldest);
  haveSetPath(oldest, false);
  Serial.printf("[VOX] storage full: removed %s to make room (%d stored)\n", oldest, n);
  return ok;
}

// Writes [lead byte][data] to `path`, making room from the oldest photos and
// clips when it doesn't fit. False only if it still can't be written.
static bool writeStore(const char* path, const uint8_t* lead, const uint8_t* data, size_t len) {
  for (int tries = 0; tries < 16; ++tries) {
    File f = g_fs->open(path, "w");
    bool ok = (bool)f;
    if (ok) {
      ok = (!lead || f.write(lead, 1) == 1) && f.write(data, len) == len;
      f.close();
    }
    if (ok) { haveSetPath(path, true); return true; }
    g_fs->remove(path);
    haveSetPath(path, false);
    if (!pruneOldestVox(path)) return false;
  }
  return false;
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
  uint32_t random32() override { return esp_random(); }

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
    return storeHas(sid, false) || storeHas(sid, true);
  }

  bool savePayload(uint32_t sid, const uint8_t* data, size_t len) override {
    if (!g_fs) return false;
    const Seen* e = seen(sid);
    const bool voice = e && e->kind != VoxWire::kImage;
    char path[40];
    storePath(sid, voice, path, sizeof path);
    if (!writeStore(path, voice ? &e->mode : nullptr, data, len)) return false;
    Serial.printf("[VOX] %s %08lx complete, %u bytes\n", voice ? "voice" : "photo", (unsigned long)sid,
                  (unsigned)len);
    return true;
  }

  void changed(uint32_t sid) override { touched(sid); }
};

MeshIo g_io;
// The fetch and serve engines (~4 KB together) live in PSRAM: they are only ever
// used from the loop task, and internal RAM is what Wi-Fi, BLE and TLS need.
template <typename T, typename Io>
T& inPsram(Io& io) {
  void* p = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = malloc(sizeof(T));
  return *new (p) T(io);
}
VoxFetch& g_fetch = inPsram<VoxFetch>(g_io);

// Answers downloads of the clips this radio holds (VoxServe).
class ServeIo : public VoxServeIo {
 public:
  uint32_t nowMs() override { return millis(); }
  uint32_t random32() override { return esp_random(); }
  void selfKey6(uint8_t out[6]) override { memcpy(out, the_mesh.getSelfPubKey(), 6); }

  bool routeToKey6(const uint8_t key6[6], uint8_t* path, int* len) override {
    ContactInfo* c = the_mesh.lookupContactByPubKey(key6, 6);
    if (!c) return false;
    uint8_t packed = 0;
    uint8_t buf[MAX_PATH_SIZE];
    if (!the_mesh.voxContactRoute(c->id.pub_key, buf, &packed)) return false;
    if ((packed >> 6) != 0) return false;   // raw data takes 1-byte hashes only
    const int n = packed & 0x3F;
    memcpy(path, buf, (size_t)n);
    *len = n;
    return true;
  }

  bool sendRaw(const uint8_t* path, int len, const uint8_t* data, size_t n) override {
    if (len < 0 || len > 63) return false;
    return the_mesh.voxSendRaw(path, (uint8_t)len, data, n);
  }

  bool sendChannelData(int ch, uint16_t type, const uint8_t* data, size_t n, bool zero_hop) override {
    return the_mesh.voxSendChannelData(ch, type, data, n, zero_hop);
  }

  // Clips go out on radio2 alone (tx.media radio2): space them by its airtime,
  // not the main channel's, or a faster radio2 sends them no faster.
  uint32_t packetAirMs() override {
    const uint32_t r2 = mediamesh::mediaRadio2AirMs(2 + VoxWire::kPacketHeaderLen + VoxWire::kBytesPerPacket);
    return r2 ? r2 : g_io.packetAirMs();
  }

  bool loadClip(uint32_t sid, uint8_t* mode, uint8_t** bits, size_t* len) override {
    int m = 0;
    if (!loadVoice(sid, &m, bits, len)) return false;
    *mode = (uint8_t)m;
    return true;
  }
  void freeClip(uint8_t* bits) override { free(bits); }
  void log(const char* line) override { Serial.println(line); }
};

ServeIo g_serve_io;
VoxServe& g_serve = inPsram<VoxServe>(g_serve_io);

bool parseChannelPost(const char* text, char* name, size_t name_cap, VoxWire::Envelope* env) {
  const char* body = ChannelSenderSplit::split(text, name, name_cap);
  return VoxWire::parseEnvelope(body, env);
}

}  // namespace

bool canFetch(const VoxWire::Envelope& env) {
  return env.kind == VoxWire::kImage || canPlay(env);
}

bool canPlay(const VoxWire::Envelope& env) {
#if CAP_LUA_AUDIO
  return env.kind == VoxWire::kCodec2 && voxCodec2CanDecode(env.mode);
#else
  (void)env;
  return false;
#endif
}

void setStore(fs::FS* fs, const char* root) {
  g_fs = fs;
  haveClear();   // another store: nothing known about it yet
  snprintf(g_root, sizeof g_root, "%s", root ? root : "");
}

bool onDirectText(const char* text, const uint8_t sender_pub[32]) {
  if (g_serve.onDirectText(text)) return true;   // VR3: someone wants a clip of ours
  VoxWire::Envelope env;
  if (!VoxWire::parseEnvelope(text, &env) || !canFetch(env)) return false;
  remember(env);
  g_fetch.onEnvelope(env, sender_pub, nullptr, -1, false);
  return false;
}

void onChannelText(const char* text, int channel_idx) {
  char name[ChannelSenderSplit::kMaxWireName + 1];
  VoxWire::Envelope env;
  if (!parseChannelPost(text, name, sizeof name, &env) || !canFetch(env)) return;
  remember(env);
  g_fetch.onEnvelope(env, nullptr, name, channel_idx, false);
}

void onPayload(const uint8_t* data, size_t len, int channel_idx, uint8_t via, int hops) {
  g_fetch.onPayload(data, len, channel_idx, via, hops);
  g_serve.onPayload(data, len, channel_idx, hops);
}

void tick() {
  g_fetch.tick();
  g_serve.tick();
}

bool parse(const char* text, VoxWire::Envelope* env) { return VoxWire::parseEnvelope(text, env); }

void request(const VoxWire::Envelope& env, const uint8_t* dm_pub, const char* sender_name,
             int channel_idx) {
  if (!canFetch(env)) return;
  remember(env);
  g_fetch.onEnvelope(env, dm_pub, sender_name, channel_idx, false);
  g_fetch.request(env.sid);
}

VoxFetch::Status status(uint32_t sid, int* received, int* total) {
  return g_fetch.status(sid, received, total);
}

const char* statusText(uint32_t sid, char* buf, size_t cap) { return g_fetch.statusText(sid, buf, cap); }

const char* viaText(uint32_t sid) {
  if (g_fetch.status(sid) != VoxFetch::kComplete) return nullptr;
  return VoxFetch::viaText(g_fetch.via(sid));
}

bool load(uint32_t sid, uint8_t** data, size_t* len) { return loadFile(sid, false, data, len); }

bool loadVoice(uint32_t sid, int* mode, uint8_t** frames, size_t* len) {
  uint8_t* raw = nullptr;
  size_t n = 0;
  if (!mode || !frames || !len || !loadFile(sid, true, &raw, &n)) return false;
  if (n < 2) { free(raw); return false; }
  *mode = raw[0];
  memmove(raw, raw + 1, n - 1);
  *frames = raw;
  *len = n - 1;
  return true;
}

bool loadFile(uint32_t sid, bool voice, uint8_t** data, size_t* len) {
  if (!g_fs || !data || !len) return false;
  char path[40];
  storePath(sid, voice, path, sizeof path);
  File f = g_fs->open(path, "r");
  if (!f) return false;
  const size_t n = f.size();
  if (n < 2 || n > 64 * 1024) { f.close(); return false; }   // largest clip: 96 x 156 B
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
uint32_t fsChecks() { return g_fs_checks; }

uint32_t stateVersion() {
  for (int i = 0; i < g_touched_n; i++) {
    const uint32_t sid = g_touched[i];
    const uint8_t st = (uint8_t)g_fetch.status(sid);
    int at = -1;
    for (int j = 0; j < 16; j++) if (g_last_st[j].sid == sid) { at = j; break; }
    if (at < 0) {
      at = g_last_st_next;
      g_last_st_next = (g_last_st_next + 1) % 16;
      g_last_st[at].sid = sid;
      g_last_st[at].st = 0xFF;
    }
    if (g_last_st[at].st != st) { g_last_st[at].st = st; g_state_version++; }
  }
  g_touched_n = 0;
  return g_state_version;
}

bool addRecording(uint8_t wire_mode, uint8_t* bits, size_t len, uint32_t ms, char* envelope, size_t cap,
                  uint32_t* sid_out, uint32_t want_sid) {
  const int pb = VoxWire::packetBytes(wire_mode);
  if (!g_fs || !bits || len == 0 || pb <= 0) { free(bits); return false; }
  const int total = (int)((len + (size_t)pb - 1) / (size_t)pb);
  const int seconds = (int)((ms + 999) / 1000);
  uint32_t sid = want_sid;
  while (!sid) sid = esp_random();
  if (total > VoxWire::kMaxClipPackets || !VoxWire::encodeVoiceEnvelope(sid, wire_mode, total, seconds, envelope, cap)) {
    free(bits);
    return false;
  }
  // Stored like a downloaded clip ([wire mode][frames]), so the bubble plays it
  // and VoxServe can answer for it after a reboot too.
  char path[40];
  storePath(sid, true, path, sizeof path);
  const bool ok = writeStore(path, &wire_mode, bits, len);
  free(bits);
  if (!ok) return false;
  VoxWire::Envelope env;
  VoxWire::parseEnvelope(envelope, &env);
  remember(env);
  g_serve.markOwn(sid);
  bumpState();
  if (sid_out) *sid_out = sid;
  Serial.printf("[VOX] recorded clip %08lx: %u bytes, %d packets, %d s\n", (unsigned long)sid, (unsigned)len, total,
                seconds);
  return true;
}

bool storeHeard(uint32_t sid, uint8_t wire_mode, const uint8_t* bits, size_t len) {
  if (!g_fs || !sid || !bits || len == 0) return false;
  char path[40];
  storePath(sid, true, path, sizeof path);
  if (storeHas(sid, true)) return true;
  if (!writeStore(path, &wire_mode, bits, len)) return false;
  VoxWire::Envelope env;
  env.sid = sid;
  env.kind = VoxWire::kCodec2;
  env.mode = wire_mode;
  remember(env);
  bumpState();
  Serial.printf("[VOX] talk %08lx heard live, kept: %u bytes\n", (unsigned long)sid, (unsigned)len);
  return true;
}

void discardRecording(uint32_t sid) {
  if (!g_fs || !sid) return;
  char path[40];
  storePath(sid, true, path, sizeof path);
  g_fs->remove(path);
  haveSet(sid, true, false);
  bumpState();
  Serial.printf("[VOX] discarded clip %08lx\n", (unsigned long)sid);
}

bool haveVoice(uint32_t sid) { return storeHas(sid, true); }

}  // namespace VoxPhotos
