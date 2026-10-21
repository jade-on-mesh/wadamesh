// SPDX-License-Identifier: GPL-3.0-or-later
// Flock You's radio side (FlockRadio.h): Wi-Fi sniffing and the BLE scan,
// taken only while the rest of the firmware leaves them idle.

#include "FlockRadio.h"

#if CAP_FLOCK_YOU

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <string.h>

#include "BleKeyboard.h"
#include "DroneCore.h"
#include "helpers/esp32/WifiRuntimeStore.h"

// The WifiRuntimeStore.h this resolves to in board builds does not declare it
// (defined in src/helpers/esp32/WifiRuntimeStore.cpp; FlockYou 1.2 declared it too).
extern bool wifiScanIsActive();

namespace FlockRadio {
namespace {

constexpr int kQueue = 32;
constexpr uint32_t kHopMs = 300;           // dwell per Wi-Fi channel
constexpr uint32_t kBleScanS = 4;          // BLE window...
constexpr uint32_t kBleGapMs = 6000;       // ...then a rest, for the phone link and Wi-Fi
constexpr uint32_t kBleGapDroneMs = 2000;  // shorter while the Drone Detector listens
constexpr int kRidQueue = 24;
constexpr uint32_t kRetryMs = 5000;        // after promiscuous mode was refused
// With a saved network out of reach the main loop retries it every 10 s, and
// channel hopping would spoil every attempt: sniff for a while, then step aside
// long enough for one retry to associate.
constexpr uint32_t kSniffWithSavedMs = 45000;
constexpr uint32_t kReconnectWindowMs = 12000;
constexpr uint32_t kBootGraceMs = 40000;
constexpr uint8_t kChannels = 13;

// The queue lives in PSRAM: internal RAM is the scarce one on these boards.
flockyou::Alert* s_q = nullptr;
volatile uint8_t s_head = 0, s_tail = 0;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

volatile bool s_wanted = false;   // either app
volatile bool s_flock = false;    // Flock You wants the radios
volatile bool s_drone = false;    // the Drone Detector does
RidRaw* s_rq = nullptr;           // Remote ID payloads (PSRAM)
volatile uint8_t s_rhead = 0, s_rtail = 0;
portMUX_TYPE s_rmux = portMUX_INITIALIZER_UNLOCKED;
// With duplicates on (the Drone Detector needs every advert), Flock You still
// reports each device once per window.
constexpr int kSeen = 48;
uint32_t s_seen[kSeen];
volatile uint8_t s_seen_n = 0;
bool s_wifi = false;           // promiscuous on, ours
bool s_ble = false;            // our scan configured on NimBLE's scanner
NimBLEScan* s_scan = nullptr;
uint8_t s_ch = 1;
uint32_t s_hop_ms = 0, s_ble_ms = 0, s_wifi_fail_ms = 0;
uint32_t s_wifi_on_ms = 0, s_wifi_pause_ms = 0;   // sniff/yield cycle with a saved network
const char* s_wifi_why = "off";
const char* s_ble_why = "off";
Diag s_diag;

void push(const flockyou::Alert& a) {
  if (!s_q) return;
  portENTER_CRITICAL(&s_mux);
  const uint8_t next = (uint8_t)((s_head + 1) % kQueue);
  if (next != s_tail) {
    s_q[s_head] = a;
    s_head = next;
  } else {
    s_diag.queue_drops++;
  }
  portEXIT_CRITICAL(&s_mux);
}

// From the Wi-Fi and NimBLE tasks.
void pushRid(const uint8_t* data, uint16_t len, uint8_t transport, int8_t rssi, const uint8_t mac[6]) {
  if (!s_rq || !len) return;
  if (len > sizeof s_rq[0].data) len = sizeof s_rq[0].data;
  portENTER_CRITICAL(&s_rmux);
  const uint8_t next = (uint8_t)((s_rhead + 1) % kRidQueue);
  if (next != s_rtail) {
    RidRaw& r = s_rq[s_rhead];
    memcpy(r.data, data, len);
    r.len = len;
    r.transport = transport;
    r.rssi = rssi;
    memcpy(r.mac, mac, 6);
    s_rhead = next;
  } else {
    s_diag.rid_drops++;
  }
  portEXIT_CRITICAL(&s_rmux);
}

// Flock You's once-per-window rule while duplicates are on.
bool firstThisWindow(const uint8_t mac[6], uint8_t method) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < 6; i++) h = (h ^ mac[i]) * 16777619u;
  h = (h ^ method) * 16777619u;
  const uint8_t n = s_seen_n;
  for (uint8_t i = 0; i < n; i++)
    if (s_seen[i] == h) return false;
  if (n < kSeen) { s_seen[n] = h; s_seen_n = (uint8_t)(n + 1); }
  return true;
}

// Wi-Fi task.
void wifiRx(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!s_wanted || type != WIFI_PKT_MGMT || !buf) return;
  const auto* p = static_cast<const wifi_promiscuous_pkt_t*>(buf);
  s_diag.wifi_frames++;
  if (s_drone) {
    uint16_t off = 0;
    uint8_t tr = 0;
    const uint16_t n = drone::wifiPayload(p->payload, (uint16_t)p->rx_ctrl.sig_len, &off, &tr);
    if (n) {
      s_diag.rid_wifi++;
      pushRid(p->payload + off, n, tr, (int8_t)p->rx_ctrl.rssi, drone::wifiSource(p->payload));
      return;
    }
  }
  if (!s_flock) return;
  if (p->rx_ctrl.rssi < -92) return;
  flockyou::Alert a;
  if (!flockyou::parseWifiFrame(p->payload, (int)p->rx_ctrl.sig_len, p->rx_ctrl.rssi, (uint8_t)p->rx_ctrl.channel, a))
    return;
  s_diag.wifi_hits++;
  push(a);
}

// NimBLE host task. Each device is reported once per scan window (no
// duplicates), so a busy street cannot flood the queue.
class BleCb final : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* d) override {
    if (!s_wanted || !d) return;
    s_diag.ble_adv++;
    const NimBLEAddress addr = d->getAddress();
    // getNative() is the stack's little-endian order; the OUI is the last three bytes there.
    const uint8_t* n = addr.getNative();
    const uint8_t disp[6] = {n[5], n[4], n[3], n[2], n[1], n[0]};
    if (s_drone) {
      uint16_t off = 0;
      const uint16_t len = rid_extractFromBleAdv(d->getPayload(), (uint16_t)d->getPayloadLength(), &off);
      if (len) {
        s_diag.rid_ble++;
        pushRid(d->getPayload() + off, len, RID_TRANSPORT_BLE_LEGACY, (int8_t)d->getRSSI(), disp);
        return;
      }
    }
    if (!s_flock) return;
    flockyou::BleAdv adv;
    flockyou::parseAdv(d->getPayload(), d->getPayloadLength(), adv);
    const flockyou::Method m = flockyou::classifyBle(disp, d->getAddressType() == BLE_ADDR_PUBLIC, adv);
    if (m == flockyou::Method::None) return;
    if (s_drone && !firstThisWindow(disp, (uint8_t)m)) return;
    s_diag.ble_hits++;
    flockyou::Alert a;
    a.source = flockyou::Source::Ble;
    a.method = m;
    flockyou::macToStr(disp, a.mac, sizeof a.mac);
    memcpy(a.name, adv.name, sizeof a.name);
    a.rssi = (int16_t)d->getRSSI();
    push(a);
  }
};
BleCb s_ble_cb;

bool ensureQueue() {
  if (s_q) return true;
  s_q = static_cast<flockyou::Alert*>(heap_caps_calloc(kQueue, sizeof(flockyou::Alert), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  return s_q != nullptr;
}

bool ensureRidQueue() {
  if (s_rq) return true;
  s_rq = static_cast<RidRaw*>(heap_caps_calloc(kRidQueue, sizeof(RidRaw), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  return s_rq != nullptr;
}

// ---- Wi-Fi ----
void wifiRelease() {
  if (!s_wifi) return;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  s_wifi = false;
  s_diag.wifi_yield++;
}

// Why the firmware needs Wi-Fi itself right now; nullptr when it is free.
const char* wifiBusy() {
  if (!wifiConfigGetRadioEnabled()) return "off";
  const wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_MODE_NULL) return "off";
  if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) return "AP on";   // web remote, file transfer
  if (WiFi.status() == WL_CONNECTED) return "connected";
  if (wifiScanIsActive()) return "scanning";
  return nullptr;
}

void wifiTick(uint32_t now) {
  const char* busy = s_wanted ? wifiBusy() : "off";
  if (busy) {
    wifiRelease();
    s_wifi_why = busy;
    return;
  }
  const bool saved = wifiConfigHasRuntime();
  if (saved && !s_wifi && now < kBootGraceMs) { s_wifi_why = "waiting for Wi-Fi"; return; }   // let the saved network connect first
  if (s_wifi && saved && now - s_wifi_on_ms >= kSniffWithSavedMs) {
    wifiRelease();
    s_wifi_pause_ms = now ? now : 1;
  }
  if (!s_wifi && s_wifi_pause_ms) {
    if (saved && now - s_wifi_pause_ms < kReconnectWindowMs) { s_wifi_why = "reconnecting"; return; }
    s_wifi_pause_ms = 0;
  }
  if (!s_wifi) {
    if (s_wifi_fail_ms && now - s_wifi_fail_ms < kRetryMs) return;
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(wifiRx);
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
      esp_wifi_set_promiscuous_rx_cb(nullptr);
      s_diag.wifi_errors++;
      s_wifi_fail_ms = now ? now : 1;
      s_wifi_why = "refused";
      return;
    }
    s_wifi_fail_ms = 0;
    s_wifi = true;
    s_wifi_on_ms = now;
    s_wifi_why = "";
    s_ch = 1;
    esp_wifi_set_channel(s_ch, WIFI_SECOND_CHAN_NONE);
    s_hop_ms = now;
    return;
  }
  if (now - s_hop_ms >= kHopMs) {
    s_ch = (uint8_t)(s_ch % kChannels + 1);
    esp_wifi_set_channel(s_ch, WIFI_SECOND_CHAN_NONE);
    s_hop_ms = now;
  }
}

// ---- BLE ----
void bleRelease(bool stop_scan) {
  if (!s_ble) return;
  if (stop_scan && s_scan && NimBLEDevice::getInitialized()) {
    if (s_scan->isScanning()) s_scan->stop();
    s_scan->setAdvertisedDeviceCallbacks(nullptr);
  }
  s_ble = false;
  s_diag.ble_yield++;
}

// Why Bluetooth is not ours to scan with; nullptr when it is.
const char* bleBusy() {
  if (!wifiConfigGetBleEnabled()) return "off";
  if (!NimBLEDevice::getInitialized()) return "off";
#if CAP_BLE_KEYBOARD
  // The keyboard's own scan and its connection attempts share the scanner and
  // the controller; leave both alone until it is connected or off.
  if (BleKbd::active()) {
    const BleKbd::State st = BleKbd::state();
    if (st != BleKbd::State::Connected && st != BleKbd::State::Idle) return "keyboard";
  }
#endif
  return nullptr;
}

void bleTick(uint32_t now) {
  const char* busy = s_wanted ? bleBusy() : "off";
  if (busy) {
    // The keyboard may have taken the scanner over: do not stop its scan.
    bleRelease(strcmp(busy, "keyboard") != 0);
    s_ble_why = busy;
    return;
  }
  s_scan = NimBLEDevice::getScan();
  if (!s_scan) { s_ble_why = "off"; return; }
  if (s_scan->isScanning()) return;   // ours running, or someone else's: wait it out
  if (s_ble && now - s_ble_ms < kBleScanS * 1000 + (s_drone ? kBleGapDroneMs : kBleGapMs)) return;
  // Flock You: once per device per window. The Drone Detector needs every
  // advert (a drone sends its ID, position and operator in turns), so then
  // duplicates come through and firstThisWindow() keeps Flock You's rule.
  s_seen_n = 0;
  s_scan->setAdvertisedDeviceCallbacks(&s_ble_cb, s_drone);
  s_scan->setMaxResults(0);                                 // keep nothing: the callback copies
  s_scan->setActiveScan(true);                              // names come in scan responses
  s_scan->setInterval(100);
  s_scan->setWindow(60);                                    // leave air for Wi-Fi and the phone
  if (s_scan->start(kBleScanS, nullptr, false)) {
    s_ble = true;
    s_ble_ms = now;
    s_ble_why = "";
  } else {
    s_ble_why = "busy";
  }
}

}  // namespace

static void applyWanted() {
  const bool ours = s_ble && s_scan;   // a running scan of ours keeps its duplicate setting
  s_wanted = s_flock || s_drone;
  if (!s_wanted) {
    wifiRelease();
    bleRelease(true);
    s_wifi_why = s_ble_why = "off";
  } else if (ours && s_scan && NimBLEDevice::getInitialized() && s_scan->isScanning()) {
    s_scan->stop();   // restart with the right duplicate setting on the next tick
  }
}

void setWanted(bool on) {
  if (on && !ensureQueue()) on = false;
  if (s_flock == on) return;
  s_flock = on;
  applyWanted();
}

bool wanted() { return s_flock; }

void setDroneWanted(bool on) {
  if (on && !ensureRidQueue()) on = false;
  if (s_drone == on) return;
  s_drone = on;
  applyWanted();
}

bool droneWanted() { return s_drone; }

bool popRemoteId(RidRaw& out) {
  if (!s_rq) return false;
  bool got = false;
  portENTER_CRITICAL(&s_rmux);
  if (s_rtail != s_rhead) {
    out = s_rq[s_rtail];
    s_rtail = (uint8_t)((s_rtail + 1) % kRidQueue);
    got = true;
  }
  portEXIT_CRITICAL(&s_rmux);
  return got;
}

void tick(uint32_t now) {
  if (!s_wanted && !s_wifi && !s_ble) return;
  wifiTick(now);
  bleTick(now);
}

bool pop(flockyou::Alert& out) {
  if (!s_q) return false;
  bool got = false;
  portENTER_CRITICAL(&s_mux);
  if (s_tail != s_head) {
    out = s_q[s_tail];
    s_tail = (uint8_t)((s_tail + 1) % kQueue);
    got = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return got;
}

bool wifiActive() { return s_wifi; }
bool bleActive() { return s_ble && s_scan && NimBLEDevice::getInitialized() && s_scan->isScanning(); }
uint8_t wifiChannel() { return s_wifi ? s_ch : 0; }
const char* wifiWhyNot() { return s_wifi ? "" : s_wifi_why; }
const char* bleWhyNot() { return s_ble ? "" : s_ble_why; }
const Diag& diag() { return s_diag; }

void stackGoingDown() {
  // deinit() frees the scanner: drop our pointer without calling into it.
  if (s_ble) s_diag.ble_yield++;
  s_ble = false;
  s_scan = nullptr;
  s_ble_why = "off";
}

}  // namespace FlockRadio

#endif  // CAP_FLOCK_YOU
