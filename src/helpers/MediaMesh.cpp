// SPDX-License-Identifier: GPL-3.0-or-later

#include "MediaMesh.h"

#include <Arduino.h>
#include <Packet.h>
#include <target.h>

#include "MediaMeshRadio.h"
#include "esp32/TouchPrefsStore.h"

namespace mediamesh {

namespace {

constexpr const char* kPrefsKey = "mediamesh";

Config s_cfg;
bool s_loaded = false;
MarkRing s_marks;

void ensureLoaded() {
  if (s_loaded) return;
  s_loaded = true;
  defaults(s_cfg);
  uint8_t blob[kBlobSize + 8];
  const size_t n = touchPrefsGetBlob(kPrefsKey, blob, sizeof blob);
  Config c;
  if (n && decode(blob, n, c)) s_cfg = c;
}

bool save() {
  uint8_t blob[kBlobSize];
  const size_t n = encode(s_cfg, blob);
  return touchPrefsSetBlob(kPrefsKey, blob, n);
}

#if defined(MEDIAMESH_RADIO)
// Called from the radio for every packet about to be transmitted.
bool classifyRaw(const uint8_t* raw, int len) {
  if (!raw || len <= 0 || len > MAX_TRANS_UNIT) return false;
  mesh::Packet pkt;
  if (!pkt.readFrom(raw, (uint8_t)len)) return false;
  uint8_t hash[MAX_HASH_SIZE];
  pkt.calculatePacketHash(hash);
  return s_marks.has(hash, millis());
}
#endif

void apply() {
#if defined(MEDIAMESH_RADIO)
  radio_driver.setMediaClassifier(classifyRaw);
  radio_driver.mediaMeshConfigure(s_cfg);
#endif
}

}  // namespace

bool supported() {
#if defined(MEDIAMESH_RADIO)
  return true;
#else
  return false;
#endif
}

const Config& config() {
  ensureLoaded();
  return s_cfg;
}

void begin() {
  ensureLoaded();
  apply();
}

bool set(const Config& c) {
  ensureLoaded();
  if (c.mode != MODE_OFF && !validParams(c.p)) return false;
  s_cfg = c;
  if (!supported()) s_cfg.mode = MODE_OFF;
  save();
  apply();
  return true;
}

void markPacket(const mesh::Packet* pkt) {
  if (!pkt) return;
  uint8_t hash[MAX_HASH_SIZE];
  pkt->calculatePacketHash(hash);
  s_marks.add(hash, millis());
}

bool mediaActive() {
  ensureLoaded();
  return supported() && mediaOnRadio2(s_cfg);
}

bool runCli(const char* cmd, char* reply, size_t cap) {
  ensureLoaded();
  Params main;
  Timing timing;
#if defined(MEDIAMESH_RADIO)
  main = radio_driver.mainParams();
  timing = radio_driver.mediaMeshTiming();
#endif
  Config c = s_cfg;
  const CliResult r = handleCli(cmd, c, main, timing, supported(), reply, cap);
  if (!r.handled) return false;
  if (r.changed) {
    s_cfg = c;
    save();
    apply();
  }
  return true;
}

Status status() {
  Status s;
#if defined(MEDIAMESH_RADIO)
  s.scanning = radio_driver.mediaMeshScanning();
  s.profile = radio_driver.mediaMeshProfile();
  s.switches = radio_driver.mediaMeshSwitches();
  s.switch_us = radio_driver.mediaMeshSwitchUs();
  for (uint8_t i = 0; i < 2; ++i) {
    s.tx[i] = radio_driver.mediaMeshTxCount(i);
    s.rx[i] = radio_driver.mediaMeshRxCount(i);
  }
  s.timing = radio_driver.mediaMeshTiming();
#endif
  return s;
}

}  // namespace mediamesh
