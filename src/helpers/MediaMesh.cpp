// SPDX-License-Identifier: GPL-3.0-or-later

#include "MediaMesh.h"

#include <Arduino.h>
#include <Packet.h>
#include <Utils.h>
#include <target.h>

#include "MediaMeshRadio.h"
#include "esp32/TouchPrefsStore.h"

namespace mediamesh {

namespace {

constexpr const char* kPrefsKey = "mediamesh";
constexpr const char* kRoutesKey = "mmroutes";

Runtime s_rt;
Routes s_routes;
bool s_loaded = false;
MarkRing s_marks;
OriginRing s_origins;
SavedPrimaryFn s_saved_primary = nullptr;
volatile uint8_t s_ctx = kNoContext;
bool s_temp1_applied = false;

void ensureLoaded() {
  if (s_loaded) return;
  s_loaded = true;
  defaults(s_rt.saved);
  uint8_t blob[kBlobSize + 8];
  size_t n = touchPrefsGetBlob(kPrefsKey, blob, sizeof blob);
  Config c;
  if (n && decode(blob, n, c)) s_rt.saved = c;
  static uint8_t rblob[Routes::kBlobMax];
  n = touchPrefsGetBlob(kRoutesKey, rblob, sizeof rblob);
  if (n) s_routes.decode(rblob, n);
}

bool save() {
  uint8_t blob[kBlobSize];
  const size_t n = encode(s_rt.saved, blob);
  return touchPrefsSetBlob(kPrefsKey, blob, n);
}

bool saveRoutes() {
  static uint8_t blob[Routes::kBlobMax];
  const size_t n = s_routes.encode(blob);
  return touchPrefsSetBlob(kRoutesKey, blob, n);
}

#if defined(MEDIAMESH_RADIO)
void packetHash(const uint8_t* raw, int len, uint8_t out[kHashSize], bool* ok) {
  *ok = false;
  if (!raw || len <= 0 || len > MAX_TRANS_UNIT) return;
  mesh::Packet pkt;
  if (!pkt.readFrom(raw, (uint8_t)len)) return;
  pkt.calculatePacketHash(out);
  *ok = true;
}

// Called from the radio for every packet about to be transmitted.
uint8_t classifyRaw(const uint8_t* raw, int len) {
  bool ok;
  uint8_t hash[kHashSize];
  packetHash(raw, len, hash, &ok);
  if (ok) {
    const uint8_t m = s_marks.find(hash, millis());
    if (m != kNoMark) return m;
  }
  return transmitMask(s_rt, 0);
}

// Called from the radio for every packet received.
void noteOrigin(const uint8_t* raw, int len, uint8_t origin) {
  bool ok;
  uint8_t hash[kHashSize];
  packetHash(raw, len, hash, &ok);
  if (ok) s_origins.add(hash, millis(), origin);
}
#endif

Params savedPrimary() {
  if (s_saved_primary) return s_saved_primary();
#if defined(MEDIAMESH_RADIO)
  return radio_driver.mainSavedParams();
#else
  return Params();
#endif
}

void applySecondary() {
#if defined(MEDIAMESH_RADIO)
  radio_driver.setMediaClassifier(classifyRaw);
  radio_driver.setOriginSink(noteOrigin);
  radio_driver.mediaMeshConfigure(s_rt.active2());
#endif
}

void applyPrimary() {
#if defined(MEDIAMESH_RADIO)
  radio_driver.setTempPrimary(s_rt.temp1 ? &s_rt.temp1_p : nullptr);
#else
  // Boards without the scanning wrapper: retune the plain radio, and back.
  const Params p = s_rt.temp1 ? s_rt.temp1_p : savedPrimary();
  if (validParams(p)) radio_driver.setParams(p.freq, p.bw, p.sf, p.cr);
#endif
  s_temp1_applied = s_rt.temp1;
}

Env makeEnv() {
  Env e;
  e.supported = supported();
  e.now_ms = millis();
  e.epoch = rtc_clock.getCurrentTime();
  if (e.epoch < 1600000000UL) e.epoch = 0;   // the clock was never set
#if defined(MEDIAMESH_RADIO)
  e.main = radio_driver.mainParams();
  e.main_saved = radio_driver.mainSavedParams();
  e.main_preamble = radio_driver.mainPreambleSymbols();
  e.switch_us = radio_driver.mediaMeshSwitchBudgetUs();
  for (uint8_t i = 0; i < 2; ++i) {
    e.stats.rx[i] = radio_driver.mediaMeshRxCount(i);
    e.stats.tx[i] = radio_driver.mediaMeshTxCount(i);
  }
  e.stats.switches = radio_driver.mediaMeshSwitches();
  e.stats.errors = radio_driver.mediaMeshSwitchErrors();
  e.stats.max_us = radio_driver.mediaMeshSwitchMaxUs();
  e.stats.samples = radio_driver.mediaMeshSwitchSamples();
#else
  e.main_saved = savedPrimary();
  e.main = s_rt.temp1 ? s_rt.temp1_p : e.main_saved;
#endif
  return e;
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
  return s_rt.saved;
}

const Runtime& runtime() {
  ensureLoaded();
  return s_rt;
}

void begin(SavedPrimaryFn saved_primary) {
  s_saved_primary = saved_primary;
  ensureLoaded();
  if (!supported()) s_rt.saved.mode = MODE_OFF;
  applySecondary();
}

void loop() {
  ensureLoaded();
  const TickResult t = tick(s_rt, millis());
  if (t.save) save();
  if (t.apply) applySecondary();
  if (t.apply_primary || s_temp1_applied != s_rt.temp1) applyPrimary();
}

bool set(const Config& c) {
  ensureLoaded();
  if (c.mode != MODE_OFF && !validParams(c.p)) return false;
  const bool profile_changed = c.mode != s_rt.saved.mode || !sameParams(c.p, s_rt.saved.p);
  s_rt.saved = c;
  if (!supported()) s_rt.saved.mode = MODE_OFF;
  if (profile_changed && c.mode == MODE_OFF) {
    // As `set radio2 off`: no temporary session or schedule outlives it.
    endTemp2(s_rt);
    for (auto& s : s_rt.sched) s = Schedule();
  }
  save();
  applySecondary();
  return true;
}

// ---- TX choices -------------------------------------------------------------

void markPacket(const mesh::Packet* pkt, uint8_t policy) {
  if (!pkt || policy == TX_AUTO) return;
  ensureLoaded();
  uint8_t hash[MAX_HASH_SIZE];
  pkt->calculatePacketHash(hash);
  s_marks.add(hash, millis(), policyMask(s_rt, policy));
}

bool isMarked(const mesh::Packet* pkt) {
  if (!pkt) return false;
  uint8_t hash[MAX_HASH_SIZE];
  pkt->calculatePacketHash(hash);
  return s_marks.has(hash, millis());
}

void markMedia(const mesh::Packet* pkt, uint8_t channel_policy) {
  ensureLoaded();
  markPacket(pkt, s_rt.saved.media != TX_AUTO ? s_rt.saved.media : channel_policy);
}

uint8_t setTxContext(uint8_t policy) {
  const uint8_t prev = s_ctx;
  s_ctx = policy;
  return prev;
}

uint8_t policyMaskNow(uint8_t policy) {
  ensureLoaded();
  return policyMask(s_rt, policy);
}

void MarkingPacketManager::queueOutbound(mesh::Packet* packet, uint8_t priority, uint32_t scheduled_for) {
  if (packet) {
    ensureLoaded();
    uint8_t hash[MAX_HASH_SIZE];
    packet->calculatePacketHash(hash);
    const uint32_t now = millis();
    if (s_marks.find(hash, now) == kNoMark) {
      // Fix the choice now, as the packet is created: a forward follows the
      // profile it arrived on, local traffic its contact/channel context.
      const uint8_t origin = s_origins.find(hash, now);
      uint8_t mask;
      if (origin != kNoMark) mask = transmitMask(s_rt, origin);
      else if (s_ctx != kNoContext) mask = policyMask(s_rt, s_ctx);
      else mask = transmitMask(s_rt, 0);
      s_marks.add(hash, now, mask);
    }
  }
  StaticPoolPacketManager::queueOutbound(packet, priority, scheduled_for);
}

// ---- receive ----------------------------------------------------------------

uint8_t originOf(const mesh::Packet* pkt) {
  if (!pkt) return 0;
  uint8_t hash[MAX_HASH_SIZE];
  pkt->calculatePacketHash(hash);
  const uint8_t o = s_origins.find(hash, millis());
  return o == kNoMark ? 0 : o;
}

bool radio2Defined() {
  ensureLoaded();
  return supported() && s_rt.enabled();
}

bool mediaActive() {
  ensureLoaded();
  return supported() && (policyMask(s_rt, s_rt.saved.media) & 2);
}

// ---- per-contact / per-channel choices ---------------------------------------

uint8_t userPolicy(const uint8_t* pub_key) {
  ensureLoaded();
  return pub_key ? s_routes.get(Routes::USER, pub_key) : (uint8_t)TX_AUTO;
}

uint8_t channelPolicyById(const uint8_t id[Routes::kIdSize]) {
  ensureLoaded();
  return s_routes.get(Routes::CHANNEL, id);
}

bool setUserPolicy(const uint8_t* pub_key, uint8_t policy) {
  ensureLoaded();
  if (!s_routes.set(Routes::USER, pub_key, policy)) return false;
  return saveRoutes();
}

bool setChannelPolicyById(const uint8_t id[Routes::kIdSize], uint8_t policy) {
  ensureLoaded();
  if (!s_routes.set(Routes::CHANNEL, id, policy)) return false;
  return saveRoutes();
}

int routeCount(uint8_t kind) {
  ensureLoaded();
  return s_routes.count(kind);
}

void channelRouteId(const uint8_t secret[32], uint8_t out[Routes::kIdSize]) {
  mesh::Utils::sha256(out, Routes::kIdSize, secret, 32);
}

// ---- CLI ----------------------------------------------------------------------

bool runCli(const char* cmd, char* reply, size_t cap) {
  ensureLoaded();
  const Env env = makeEnv();
  const CliResult r = handleCli(cmd, s_rt, env, reply, cap);
  if (!r.handled) return false;
  if (r.save) save();
  if (r.apply) applySecondary();
  if (r.apply_primary) applyPrimary();
  return true;
}

Status status() {
  ensureLoaded();
  Status s;
  const uint32_t now = millis();
  s.temp2 = s_rt.temp2;
  s.temp2_left_ms = s_rt.temp2 ? msLeft(now, s_rt.temp2_end_ms) : 0;
  s.temp1 = s_rt.temp1;
  s.temp1_left_ms = s_rt.temp1 ? msLeft(now, s_rt.temp1_end_ms) : 0;
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
