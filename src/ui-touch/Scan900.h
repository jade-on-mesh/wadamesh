// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025-2026 BlackAtlas LLC (the scanner and classifier)
#pragma once

// The 900 MHz drone scanner's logic: GridDown Secure Messenger's sweep
// bookkeeping and multi-discriminator classifier (src/main.cpp scan_tick /
// _scanClassify, BlackAtlas LLC, GPL-3.0), moved out of its radio code into a
// state struct so it is host-tested (test/test_scan900.cpp). The radio side,
// which samples one channel at a time, is in DroneUI.inc.
//
// 52 channels, 902-928 MHz in 500 kHz steps. Each channel gets a CAD check
// (alternating SF6/BW500, the ELRS/Crossfire signature, and SF10/BW125, the
// LoRaWAN/mesh one) and a BW500 RSSI reading, plus a BW125 one when active.
// After every sweep the classifier looks for:
//   LoRa FHSS (ELRS/Crossfire): CAD across 3+ channels, by frequency zone
//     (915.0-923.2 MHz holds no LoRaWAN), SF mix, and sub-band clustering;
//   GFSK FHSS (SiK/RFD900 telemetry): RSSI across many channels;
//   Fixed LoRa, LoRaWAN infrastructure, and unknown ISM emitters.
// GridDown's RF baseline (a saved survey of the quiet band) is left out, so
// its "delta" upgrade never applies; ambiguous LoRa stays infrastructure.
// Wadamesh adds: the standalone's own mesh and radio2 channels are marked as
// infrastructure up front (Scan900::begin's `own` list).

#include <stddef.h>
#include <stdint.h>

namespace scan900 {

constexpr int kChannels = 52;
constexpr float kFirstMHz = 902.0f;
constexpr float kStepMHz = 0.5f;
inline float channelMHz(int i) { return kFirstMHz + kStepMHz * (float)i; }

// GridDown's thresholds.
constexpr float kRssiThresholdDb = 20.0f;   // noise floor + this = a hit
constexpr uint8_t kDebounceHits = 3;
constexpr float kWidebandDelta = 6.0f;      // BW500 this far over BW125: wideband
constexpr uint8_t kInfraThreshold = 16;     // active 80% of 20 sweeps: infrastructure
constexpr float kGfskClassifyDb = 22.0f;
constexpr float kStrongPeakDb = 25.0f;
constexpr float kSpurDb = 12.0f;            // calibration: this far over the median = own spur
constexpr uint32_t kAgeMs = 15000;          // a detection not refreshed this long is dropped
constexpr float kOwnGuardMHz = 1.25f;       // around our own mesh/radio2 channel: its splatter

enum Class : uint8_t { kNone = 0, kLoraFhss, kGfskFhss, kFixedLora, kIsmUnknown, kLorawan };

/** A detection that means a drone's control or telemetry link. */
inline bool isDroneClass(uint8_t c) { return c == kLoraFhss || c == kGfskFhss; }
const char* classLabel(uint8_t c);        // "LoRa FHSS (ELRS/Crossfire)"...
const char* rateLabel(uint8_t r);         // hop-rate category: "~150 Hz (FPV)"...

struct Channel {
  float rssi = -130, rssiNarrow = -130, rssiPeak = -130;
  uint8_t cadHits = 0, cadHitsLowSF = 0, cadHitsHighSF = 0, rssiHits = 0;
  bool active = false, isWideband = false;
  uint8_t infraCount = 0;
  bool infrastructure = false;
  uint32_t lastTransientSweep = 0;
};

struct Detection {
  uint8_t classification = kNone;
  float peakRSSI = 0, peakFreq = 0;
  uint8_t channelCount = 0;
  uint32_t firstSeen = 0, lastSeen = 0;
  bool valid = false;
  float rssiHistory[30] = {0};
  uint8_t historyHead = 0, historyCount = 0;
  float trendSlope = 0;   // dB/s, positive: getting closer
  float hitRatio = 0;
  uint8_t rateCategory = 0;
};

struct State {
  Channel ch[kChannels];
  Detection det[4];
  float noiseFloor = -120;
  uint32_t sweeps = 0;
  uint32_t startMs = 0;
  uint8_t cadRotation = 0;   // 0: SF6/BW500 next, 1: SF10/BW125
};

/** Start a session from a calibration sweep (one median RSSI per channel at
 *  BW500). Channels over the median by kSpurDb are the standalone's own
 *  spurs; `own`/`n_own` are frequencies it uses itself (mesh, radio2), whose
 *  channels are marked as infrastructure too. */
void begin(State& s, const float cal[kChannels], const float* own, int n_own, uint32_t now_ms);

/** Which CAD to run on the next channel: true SF6/BW500, false SF10/BW125.
 *  Rotates every channel, as GridDown's All Drone profile does. */
bool nextCadLowSF(State& s);

/** One channel's readings. `rssi_narrow` is ignored unless `have_narrow`. */
void sample(State& s, int idx, bool cad_detected, bool cad_low_sf, float rssi_wide, bool have_narrow,
            float rssi_narrow);

/** Whether the BW125 reading is wanted for this channel (active or CAD hit). */
inline bool wantNarrow(const State& s, float rssi_wide, bool cad_detected) {
  return cad_detected || rssi_wide > s.noiseFloor + kRssiThresholdDb;
}

/** A sweep is complete: noise floor, decay, debounce, classify, age, trends.
 *  Returns true when a NEW drone link (LoRa or GFSK FHSS) appeared. */
bool endSweep(State& s, uint32_t now_ms);

/** Rough distance in metres to a transmitter of `tx_dbm` heard at `rssi`
 *  (free-space path loss at 915 MHz, GridDown's scan_estimateRange). */
float estimateRangeM(float rssi, float tx_dbm);

}  // namespace scan900
