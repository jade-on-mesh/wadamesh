// SPDX-License-Identifier: GPL-3.0-or-later
#include "OtpMessenger.h"

#include <Arduino.h>
#include <FS.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "../MyMesh.h"
#include "ChannelSenderSplit.h"
#include "helpers/esp32/WdtHeavyGuard.h"   // a SPIFFS GC pass during a write must stall, not reboot

namespace OtpMessenger {
namespace {

fs::FS* g_fs = nullptr;
uint32_t g_version = 0;
void (*g_toast)(const char*) = nullptr;
void (*g_incoming)(otp::Target&, const otp::Entry&) = nullptr;

void* psramAlloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
}
void psramFree(void* p) { heap_caps_free(p); }

// Files: SPIFFS on most boards, whose names stop at 31 characters and which has
// no directories. A conversation id can be longer than that, so every file is
// "/otp" + kind + FNV-32 of the core's name. That also keeps who you talk to
// out of the directory listing.
void filePath(const char* name, char* out, size_t cap) {
  if (!strcmp(name, "settings") || !strcmp(name, "targets")) {
    snprintf(out, cap, "/otp%s", name);
    return;
  }
  uint32_t h = 2166136261u;
  for (const char* c = name; *c; ++c) h = (h ^ (uint8_t)*c) * 16777619u;
  snprintf(out, cap, "/otp%c%08lx", name[0], (unsigned long)h);
}

class MeshIo : public otp::Io {
 public:
  uint32_t nowMs() override { return millis(); }
  uint32_t epoch() override {
    const uint32_t t = the_mesh.getRTCClock()->getCurrentTime();
    return t > 1700000000UL ? t : 0;
  }
  uint8_t randomByte() override { return (uint8_t)(esp_random() & 0xFF); }
  bool airtimeClear() override { return the_mesh.getRemainingTxBudget() > 0; }
  const char* selfName() override { return the_mesh.getNodePrefs()->node_name; }
  bool selfPub(uint8_t out[32]) override { memcpy(out, the_mesh.getSelfPubKey(), 32); return true; }

  // As a DM from the Chats composer: flood, so it finds the contact wherever
  // they are (the chunk acks are what tell us it arrived).
  bool sendDm(const uint8_t pub[32], const char* text) override {
    ContactInfo* c = the_mesh.lookupContactByPubKey(pub, PUB_KEY_SIZE);
    if (!c) return false;
    ContactInfo rcpt = *c;
    rcpt.out_path_len = OUT_PATH_UNKNOWN;
    const uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
    uint32_t expected_ack = 0, est_timeout = 0;
    return the_mesh.sendMessage(rcpt, ts, 0, text, expected_ack, est_timeout) != MSG_SEND_FAILED;
  }

  // By NAME at send time, never a cached slot (a stale slot sends on the wrong key).
  bool sendChannel(const char* name, const char* text, uint32_t* fp) override {
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      ChannelDetails cd;
      if (!the_mesh.getChannel(i, cd) || !cd.name[0]) continue;
      const char* n = cd.name;
      while (*n == '#') n++;
      if (strcmp(n, name) != 0) continue;
      const uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
      MyMesh::TxChannelSlot tx_slot(the_mesh, i);   // the slot's radio2 routing
      if (!the_mesh.sendGroupMessage(ts, cd.channel, the_mesh.getNodePrefs()->node_name, (char*)text,
                                     (int)strlen(text)))
        return false;
      if (fp) *fp = the_mesh.uiLastSentFp();
      return true;
    }
    return false;
  }
  uint8_t repeatsFor(uint32_t fp) override { return fp ? the_mesh.uiRepeatsForFp(fp) : 0; }

  bool readFile(const char* name, otp::Bytes& out) override {
    if (!g_fs) return false;
    char path[32];
    filePath(name, path, sizeof path);
    if (!g_fs->exists(path)) return false;
    File f = g_fs->open(path, "r");
    if (!f) return false;
    const size_t n = f.size();
    out.clear();
    if (!out.reserve((uint32_t)n + 1)) { f.close(); return false; }
    const size_t rd = n ? f.read(out.p, n) : 0;
    f.close();
    if (rd != n) return false;
    out.n = (uint32_t)n;
    out.p[n] = 0;
    return true;
  }
  bool writeFile(const char* name, const uint8_t* data, uint32_t len) override {
    if (!g_fs) return false;
    char path[32], tmp[34];
    filePath(name, path, sizeof path);
    // Write beside, then swap: a power cut never leaves half a pad.
    snprintf(tmp, sizeof tmp, "%s~", path);
    WdtHeavyGuard guard;
    File f = g_fs->open(tmp, "w");
    if (!f) return false;
    const size_t wr = len ? f.write(data, len) : 0;
    f.close();
    if (wr != len) { g_fs->remove(tmp); return false; }
    g_fs->remove(path);
    if (!g_fs->rename(tmp, path)) return false;
    return true;
  }
  bool removeFile(const char* name) override {
    if (!g_fs) return false;
    char path[32];
    filePath(name, path, sizeof path);
    return g_fs->remove(path);
  }
  void removeAll() override {
    if (!g_fs) return;
    // Collect first: deleting while iterating SPIFFS skips entries.
    char names[48][32];
    int n = 0;
    File root = g_fs->open("/");
    if (!root) return;
    for (File f = root.openNextFile(); f && n < 48; f = root.openNextFile()) {
      const char* nm = f.name();
      if (!nm) continue;
      const char* base = nm[0] == '/' ? nm + 1 : nm;
      if (!strncmp(base, "otp", 3)) snprintf(names[n++], sizeof names[0], "/%s", base);
      f.close();
    }
    root.close();
    for (int i = 0; i < n; i++) g_fs->remove(names[i]);
    if (n == 48) removeAll();   // more than one batch
  }

  void toast(const char* msg) override { if (g_toast) g_toast(msg); }
  void changed(otp::Target&) override { g_version++; }
  void incoming(otp::Target& t, const otp::Entry& e) override {
    g_version++;
    if (g_incoming) g_incoming(t, e);
  }
};

MeshIo g_io;
otp::Core* g_core = nullptr;

}  // namespace

void begin(fs::FS* fs) {
  if (g_core) return;
  g_fs = fs;
  otp::setAllocator(psramAlloc, psramFree);
  g_core = new otp::Core(&g_io);
  g_core->begin();
}

bool ready() { return g_core != nullptr; }
otp::Core& core() { return *g_core; }
uint32_t version() { return g_version; }

void setCallbacks(void (*toast)(const char*), void (*incoming)(otp::Target&, const otp::Entry&)) {
  g_toast = toast;
  g_incoming = incoming;
}

bool onDirectText(const char* text, const uint8_t sender_pub[32]) {
  if (!g_core || !text) return false;
  ContactInfo* c = the_mesh.lookupContactByPubKey(sender_pub, PUB_KEY_SIZE);
  const bool consumed = g_core->onDirect(sender_pub, c ? c->name : nullptr, text);
  if (consumed) g_version++;
  return consumed;
}

bool onChannelText(const char* text, int channel_idx) {
  if (!g_core || !text || channel_idx < 0) return false;
  ChannelDetails cd;
  if (!the_mesh.getChannel(channel_idx, cd) || !cd.name[0]) return false;
  char sender[ChannelSenderSplit::kMaxWireName + 1];
  const char* body = ChannelSenderSplit::split(text, sender, sizeof sender);
  if (!body) return false;
  const bool consumed = g_core->onChannel(cd.name, sender, body);
  if (consumed) g_version++;
  return consumed;
}

void tick() {
  if (g_core) g_core->tick();
}

int contactCount() { return (int)the_mesh.getNumContacts(); }

bool contactAt(int i, uint8_t pub_out[32], char* name, size_t cap) {
  ContactInfo c;
  if (!the_mesh.getContactByIdx((uint32_t)i, c)) return false;
  if (c.type != ADV_TYPE_CHAT) return false;
  memcpy(pub_out, c.id.pub_key, 32);
  snprintf(name, cap, "%s", c.name);
  return true;
}

int channelCount() { return MAX_GROUP_CHANNELS; }

bool channelAt(int i, char* name, size_t cap) {
  ChannelDetails cd;
  if (!the_mesh.getChannel(i, cd) || !cd.name[0]) return false;
  const char* n = cd.name;
  while (*n == '#') n++;
  snprintf(name, cap, "%s", n);
  return true;
}

bool exportPad(otp::Target& t, fs::FS* fs, const char* path) {
  if (!g_core || !fs) return false;
  otp::Bytes hex;
  if (!g_core->exportHex(t, hex)) return false;
  File f = fs->open(path, "w");
  if (!f) return false;
  const size_t wr = f.write(hex.p, hex.n);
  f.close();
  return wr == hex.n;
}

}  // namespace OtpMessenger
