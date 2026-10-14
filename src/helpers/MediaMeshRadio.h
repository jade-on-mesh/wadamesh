// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaMesh radio: the board's SX126x wrapper, extended to time-share the one
// transceiver between the main mesh profile and the MediaMesh (`radio2`)
// profile. Variants declare `RADIO_DRIVER_CLASS radio_driver`, which is this
// class on supported boards and the plain wrapper everywhere else.
//
//  * A scan task on core 0 alternates the receiver between the profiles on the
//    schedule from MediaMeshCore.h. It never leaves a profile while a packet is
//    being received there, or while one is being transmitted.
//  * A packet the firmware marked as media (MediaMesh.h) is transmitted on
//    radio2 with the longer preamble scanning receivers need; anything else on
//    the main profile with its normal preamble.
//  * All radio access goes through the wrapper's recursive radio mutex, which
//    exists once buffered receive is on. MediaMesh turns buffered receive on
//    while radio2 is active, since its drain task is what lifts a packet out of
//    the radio quickly enough for the scan to move on.
//
// Requires an ESP32 with an SX126x whose DIO1 reaches a GPIO; elsewhere
// MEDIAMESH_RADIO stays undefined and the board keeps its stock wrapper.

#pragma once

#include "MediaMeshCore.h"

#if defined(ESP32) && (defined(USE_SX1262) || defined(USE_SX1268)) && !defined(MESH_RADIO_DIO1_POLLED) \
    && !defined(MEDIAMESH_DISABLE)

#define MEDIAMESH_RADIO 1

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>

using MediaMeshClassifier = bool (*)(const uint8_t* raw, int len);

template <class Base>
class MediaMeshRadio : public Base {
  using Params = mediamesh::Params;

  Params _main;
  bool _main_known = false;
  mediamesh::Config _cfg;
  mediamesh::Timing _timing;
  volatile bool _scan = false;      // radio2 active: scan both profiles
  volatile bool _tx = false;        // a transmit is in progress
  volatile bool _paused = false;    // spectrum analyzer owns the radio
  volatile uint8_t _cur = 0;        // profile the radio is tuned to
  bool _user_rxq = false;           // buffered receive as the user set it
  uint32_t _hold_start = 0;
  uint32_t _switch_max_us = 0;
  uint8_t _switch_samples = 0;
  uint32_t _switches = 0;
  uint32_t _tx_count[2] = {};
  uint32_t _rx_count[2] = {};
  uint32_t _last_recv_total = 0;
  TaskHandle_t _task = nullptr;
  MediaMeshClassifier _classify = nullptr;

  CustomSX1262* chip() { return static_cast<CustomSX1262*>(this->_radio); }

  uint16_t mainPreamble() const { return RadioLibWrapper::preambleLengthForSF(_main.sf); }

  uint32_t switchBudgetUs() const {
    // Until four hops are measured, assume a conservative allowance.
    if (_switch_samples < 4) return mediamesh::kDefaultSwitchUs;
    return (_switch_max_us * 11 + 9) / 10;
  }

  void recalc() {
    if (!_main_known || !mediamesh::validParams(_cfg.p)) { _timing = mediamesh::Timing(); return; }
    _timing = mediamesh::calcTiming(_main, mainPreamble(), _cfg.p, switchBudgetUs());
  }

  // Caller holds the radio mutex.
  void applyProfile(uint8_t profile) {
    const Params& p = profile ? _cfg.p : _main;
    this->idle();
    Base::setParams(p.freq, p.bw, p.sf, p.cr);   // also restores the stock preamble
    if (profile == 1 && _timing.valid) {
      const uint16_t pre = _timing.preamble[1];
      chip()->setPreambleLength(pre);
      PacketMillis pm = this->calcMaxPacketMillis(p.sf, p.bw, p.cr, pre > 255 ? 255 : (uint8_t)pre);
      chip()->setPreambleMillis(pm.preambleMillis);
      chip()->setMaxPayloadMillis(pm.payloadMillis);
    }
    _cur = profile;
  }

  void sampleSwitch(uint32_t us) {
    if (_switch_samples < 255) ++_switch_samples;
    if (us > _switch_max_us) {
      _switch_max_us = us;
      if (_switch_samples >= 4) recalc();
    } else if (_switch_samples == 4) {
      recalc();
    }
  }

  // The longest a profile may hold the receiver for one packet before a stuck
  // preamble/header flag is treated as noise.
  uint32_t maxHoldMs(uint8_t profile) const {
    const Params& p = profile ? _cfg.p : _main;
    const uint16_t pre = profile ? _timing.preamble[1] : mainPreamble();
    return mediamesh::timeOnAirUs(p, pre, 255) / 1000 * 3 / 2 + 50;
  }

  uint32_t visitTicks(uint8_t profile) const {
    const uint32_t us = _timing.dwell_us[profile];
    // Round the slow visit up (it must cover 4.6 symbols) and the fast one down
    // (it must end inside half the slow preamble).
    uint32_t ms = profile == _timing.slow ? (us + 999) / 1000 : us / 1000;
    if (ms == 0) ms = 1;
    TickType_t t = pdMS_TO_TICKS(ms);
    return t ? t : 1;
  }

  static void scanTask(void* arg) { static_cast<MediaMeshRadio*>(arg)->scanLoop(); }

  void scanLoop() {
    for (;;) {
      if (!_scan || _paused || !_timing.valid) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        continue;
      }
      vTaskDelay(visitTicks(_cur));
      this->radioAcquire();
      if (_scan && !_paused && !_tx && this->isInRecvMode()) {
        const uint32_t recv = this->getPacketsRecv();
        if (recv != _last_recv_total) { _rx_count[_cur] += recv - _last_recv_total; _last_recv_total = recv; }
        if (this->isReceivingPacket()) {
          const uint32_t now = millis();
          if (!_hold_start) _hold_start = now;
          if (now - _hold_start < maxHoldMs(_cur)) {
            this->radioRelease();
            vTaskDelay(1);
            continue;   // stay until the packet is in
          }
        }
        _hold_start = 0;
        const uint32_t t0 = micros();
        applyProfile(_cur ^ 1);
        this->startRecv();
        sampleSwitch(micros() - t0);
        ++_switches;
      } else {
        _hold_start = 0;
      }
      this->radioRelease();
    }
  }

  void wake() { if (_task) xTaskNotifyGive(_task); }

 public:
  using Base::Base;

  // ---- configuration ------------------------------------------------------

  void setMediaClassifier(MediaMeshClassifier fn) { _classify = fn; }

  void mediaMeshConfigure(const mediamesh::Config& cfg) {
    this->radioAcquire();
    const bool params_changed = !mediamesh::sameParams(cfg.p, _cfg.p);
    _cfg = cfg;
    const bool scan = mediamesh::scanning(cfg) && _main_known;
    if (params_changed) { _switch_samples = 0; _switch_max_us = 0; }
    recalc();
    if (!scan && _cur != 0 && !_tx) {
      applyProfile(0);
      this->startRecv();
    } else if (scan && params_changed && _cur == 1 && !_tx) {
      applyProfile(1);   // retune to the new radio2 settings
      this->startRecv();
    }
    _scan = scan;
    this->radioRelease();
    Base::rxQueueEnable(_user_rxq || scan);
    if (scan && !_task) {
      xTaskCreatePinnedToCore(scanTask, "mediamesh", 3072, this, 9, &_task, 0);
    }
    wake();
  }

  const mediamesh::Config& mediaMeshConfig() const { return _cfg; }
  const mediamesh::Timing& mediaMeshTiming() const { return _timing; }
  bool mediaMeshScanning() const { return _scan; }
  uint8_t mediaMeshProfile() const { return _cur; }
  uint32_t mediaMeshSwitches() const { return _switches; }
  uint32_t mediaMeshSwitchUs() const { return _switch_samples ? _switch_max_us : 0; }
  uint32_t mediaMeshTxCount(uint8_t profile) const { return _tx_count[profile & 1]; }
  uint32_t mediaMeshRxCount(uint8_t profile) const { return _rx_count[profile & 1]; }
  const Params& mainParams() const { return _main; }

  // ---- wrapper overrides --------------------------------------------------

  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) override {
    this->radioAcquire();
    _main.freq = freq; _main.bw = bw; _main.sf = sf; _main.cr = cr; _main.preamble = 0;
    const bool first = !_main_known;
    _main_known = true;
    Base::setParams(freq, bw, sf, cr);
    _cur = 0;
    if (!first) { _switch_samples = 0; _switch_max_us = 0; }
    recalc();
    if (first && mediamesh::scanning(_cfg)) _scan = true;
    this->radioRelease();
    wake();
  }

  bool startSendRaw(const uint8_t* bytes, int len) override {
    uint8_t profile = 0;
    if (_scan && mediamesh::mediaOnRadio2(_cfg) && _timing.valid && _classify && _classify(bytes, len)) profile = 1;
    this->radioAcquire();
    _tx = true;
    if (_cur != profile) applyProfile(profile);
    const bool ok = Base::startSendRaw(bytes, len);
    if (ok) ++_tx_count[profile];
    else _tx = false;
    this->radioRelease();
    return ok;
  }

  void onSendFinished() override {
    Base::onSendFinished();
    _tx = false;
    _hold_start = 0;
  }

  uint32_t getEstAirtimeFor(int len_bytes) override {
    if (!_scan || !_timing.valid) return Base::getEstAirtimeFor(len_bytes);
    const uint32_t a = mediamesh::timeOnAirUs(_main, mainPreamble(), len_bytes);
    const uint32_t b = mediamesh::timeOnAirUs(_cfg.p, _timing.preamble[1], len_bytes);
    return (a > b ? a : b) / 1000;
  }

  void powerOff() override {
    _scan = false;
    Base::powerOff();
  }

  // Hides the wrapper's methods: buffered receive stays on while radio2 is
  // active, and the scan stops while the spectrum analyzer drives the radio.
  void rxQueueEnable(bool en) {
    _user_rxq = en;
    Base::rxQueueEnable(en || _scan);
  }
  void rxQueueSuspend(bool s) {
    _paused = s;
    Base::rxQueueSuspend(s);
    if (!s) wake();
  }
};

#define RADIO_DRIVER_CLASS MediaMeshRadio<WRAPPER_CLASS>

#else

#define RADIO_DRIVER_CLASS WRAPPER_CLASS

#endif
