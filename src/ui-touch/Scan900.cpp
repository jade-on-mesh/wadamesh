// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025-2026 BlackAtlas LLC (the scanner and classifier)
// The 900 MHz drone scanner's logic (Scan900.h). The bookkeeping in sample()
// and endSweep() and the classifier in classify() follow GridDown Secure
// Messenger's scan_tick() and _scanClassify() (src/main.cpp) step for step;
// the comments marked "GridDown:" are its own reasoning.

#include "Scan900.h"

#include <math.h>
#include <string.h>

namespace scan900 {

const char* classLabel(uint8_t c) {
  switch (c) {
    case kLoraFhss: return "LoRa FHSS (ELRS/Crossfire)";
    case kGfskFhss: return "GFSK telemetry (SiK/RFD900)";
    case kFixedLora: return "Fixed LoRa";
    case kIsmUnknown: return "Unknown ISM";
    case kLorawan: return "LoRaWAN infrastructure";
    default: return "None";
  }
}

const char* rateLabel(uint8_t r) {
  switch (r) {
    case 1: return "~25 Hz (long range)";
    case 2: return "~50 Hz (mid range)";
    case 3: return "~150 Hz (FPV)";
    case 4: return "250 Hz+ (FPV racing)";
    default: return "";
  }
}

static void sortFloats(float* a, int n) {
  for (int i = 1; i < n; i++) {
    const float key = a[i];
    int j = i - 1;
    while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
    a[j + 1] = key;
  }
}

void begin(State& s, const float cal[kChannels], const float* own, int n_own, uint32_t now_ms) {
  s = State{};
  s.startMs = now_ms;
  float sorted[kChannels];
  memcpy(sorted, cal, sizeof sorted);
  sortFloats(sorted, kChannels);
  const float median = sorted[kChannels / 2];
  s.noiseFloor = median;
  for (int i = 0; i < kChannels; i++) {
    Channel& c = s.ch[i];
    c.rssi = cal[i];
    // GridDown: channels >12 dB above the median are the board's own spurs.
    if (cal[i] > median + kSpurDb) {
      c.infrastructure = true;
      c.infraCount = kInfraThreshold;
    }
    // Wadamesh: our own mesh and radio2 channels are not drones either.
    for (int k = 0; k < n_own; k++) {
      if (fabsf(channelMHz(i) - own[k]) <= kOwnGuardMHz) {
        c.infrastructure = true;
        c.infraCount = kInfraThreshold;
      }
    }
  }
}

bool nextCadLowSF(State& s) {
  const bool low = s.cadRotation == 0;
  s.cadRotation = (uint8_t)((s.cadRotation + 1) % 2);
  return low;
}

void sample(State& s, int idx, bool cad_detected, bool cad_low_sf, float rssi_wide, bool have_narrow,
            float rssi_narrow) {
  if (idx < 0 || idx >= kChannels) return;
  Channel& r = s.ch[idx];
  if (cad_detected) {
    if (cad_low_sf) { if (r.cadHitsLowSF < 255) r.cadHitsLowSF++; }
    else if (r.cadHitsHighSF < 255) r.cadHitsHighSF++;
    if (r.cadHits < 255) r.cadHits++;
  }
  if (have_narrow) {
    r.rssiNarrow = rssi_narrow;
    r.isWideband = (rssi_wide - rssi_narrow) > kWidebandDelta;
  }
  r.rssi = rssi_wide;
  if (rssi_wide > r.rssiPeak) r.rssiPeak = rssi_wide;
  if (rssi_wide > s.noiseFloor + kRssiThresholdDb) {
    if (r.rssiHits < 255) r.rssiHits++;
    r.lastTransientSweep = s.sweeps;
  }
}

// One detection slot for `cls`: its own, else a free one, else -1.
static int findSlot(State& s, uint8_t cls) {
  for (int d = 0; d < 4; d++)
    if (s.det[d].valid && s.det[d].classification == cls) return d;
  for (int d = 0; d < 4; d++)
    if (!s.det[d].valid) return d;
  return -1;
}

// Record `cls` in its slot; true when it is new and a drone link (Wadamesh
// alerts on those only; GridDown also beeped for fixed LoRa, which a busy
// mesh neighbourhood produces all day).
static bool put(State& s, uint8_t cls, float peak_rssi, float peak_freq, int channels, uint32_t now) {
  const int slot = findSlot(s, cls);
  if (slot < 0) return false;
  Detection& d = s.det[slot];
  bool fresh = false;
  if (!d.valid || d.classification != cls) {
    d.firstSeen = now;
    fresh = isDroneClass(cls);
  }
  d.classification = cls;
  d.peakRSSI = peak_rssi;
  d.peakFreq = peak_freq;
  d.channelCount = (uint8_t)(channels > 255 ? 255 : channels);
  d.lastSeen = now;
  d.valid = true;
  return fresh;
}

static bool classify(State& s, uint32_t now) {
  bool fresh = false;
  int cadActive = 0, rssiOnly = 0;
  float cadPeak = -200, cadPeakFreq = 0, cadMin = 0;
  float rssiPeak = -200, rssiPeakFreq = 0, rssiMin = 0, rssiFmin = 999, rssiFmax = 0;
  for (int i = 0; i < kChannels; i++) {
    const Channel& c = s.ch[i];
    if (!c.active || c.infrastructure) continue;
    if (c.cadHits >= kDebounceHits) {
      cadActive++;
      if (c.rssiPeak > cadPeak) { cadPeak = c.rssiPeak; cadPeakFreq = channelMHz(i); }
      if (cadMin == 0 || c.rssiPeak < cadMin) cadMin = c.rssiPeak;
    } else if (c.rssiHits >= kDebounceHits) {
      rssiOnly++;
      if (c.rssiPeak > rssiPeak) { rssiPeak = c.rssiPeak; rssiPeakFreq = channelMHz(i); }
      if (rssiMin == 0 || c.rssiPeak < rssiMin) rssiMin = c.rssiPeak;
      if (channelMHz(i) < rssiFmin) rssiFmin = channelMHz(i);
      if (channelMHz(i) > rssiFmax) rssiFmax = channelMHz(i);
    }
  }

  // GridDown: LoRa FHSS by frequency zone (LoRaWAN US915 has nothing at
  // 915.0-923.2 MHz), SF mix (ELRS is SF5-6/BW500; LoRaWAN SF7-12/BW125), and
  // sub-band clustering (a LoRaWAN gateway listens on one 1.6 MHz sub-band).
  if (cadActive >= 3) {
    if (cadPeak - cadMin < 15.0f) {
      int gap = 0, lowSF = 0, highSF = 0;
      float fmin = 999, fmax = 0;
      for (int i = 0; i < kChannels; i++) {
        const Channel& c = s.ch[i];
        if (!c.active || c.infrastructure || c.cadHits < kDebounceHits) continue;
        const float f = channelMHz(i);
        if (f >= 915.0f && f < 923.3f) gap++;
        lowSF += c.cadHitsLowSF;
        highSF += c.cadHitsHighSF;
        if (f < fmin) fmin = f;
        if (f > fmax) fmax = f;
      }
      const int total = lowSF + highSF;
      const float lowRatio = total > 0 ? (float)lowSF / (float)total : 0.5f;
      const bool clustered = (fmax - fmin) <= 2.0f;
      if (gap > 0) {                                        // 1: the gap: a drone
        fresh |= put(s, kLoraFhss, cadPeak, cadPeakFreq, cadActive, now);
      } else if (lowRatio > 0.60f && !clustered) {          // 2: ELRS's SF across the uplink zone
        fresh |= put(s, kLoraFhss, cadPeak, cadPeakFreq, cadActive, now);
      } else {                                              // 3-5: LoRaWAN (no baseline to say otherwise)
        put(s, kLorawan, cadPeak, cadPeakFreq, cadActive, now);
      }
    }
  } else if (cadActive >= 1) {
    // GridDown: fixed LoRa on 1-2 channels: in the gap or low-SF is interesting.
    bool inGap = false;
    int lowSF = 0, highSF = 0;
    for (int i = 0; i < kChannels; i++) {
      const Channel& c = s.ch[i];
      if (!c.active || c.infrastructure || c.cadHits < kDebounceHits) continue;
      const float f = channelMHz(i);
      if (f >= 915.0f && f < 923.3f) inGap = true;
      lowSF += c.cadHitsLowSF;
      highSF += c.cadHitsHighSF;
    }
    const bool drone = inGap || lowSF > highSF;
    put(s, drone ? kFixedLora : kLorawan, cadPeak, cadPeakFreq, cadActive, now);
  }

  // GridDown: broadband noise gate. Many channels lit by a weak peak is noise;
  // a strong peak is a real wideband hopper (SiK lights 15%+ of the band).
  int nonInfra = 0;
  for (int i = 0; i < kChannels; i++) nonInfra += s.ch[i].infrastructure ? 0 : 1;
  const int totalActive = cadActive + rssiOnly;
  const bool many = nonInfra > 0 && totalActive > nonInfra * 15 / 100;
  const bool strong = rssiPeak > s.noiseFloor + kStrongPeakDb;
  const bool broadband = many && !strong;
  if (broadband) rssiOnly = 0;

  // GridDown: GFSK FHSS (SiK/RFD900/Holybro). Path A: debounced channels.
  // Wadamesh: across 5 MHz or more, as Path B asks: a hopper spans the band,
  // while a few neighbouring channels are one transmitter (a LoRaWAN sub-band
  // read before its CAD hits debounce was called telemetry).
  bool gfsk = false;
  if (rssiOnly >= 3 && rssiPeak > s.noiseFloor + kGfskClassifyDb && rssiPeak - rssiMin < 25.0f &&
      rssiFmax - rssiFmin >= 5.0f) {
    fresh |= put(s, kGfskFhss, rssiPeak, rssiPeakFreq, rssiOnly, now);
    gfsk = true;
  }
  // Path B: transient hits across many channels in the last 10 sweeps.
  if (!gfsk && !broadband && s.sweeps >= 10) {
    int n = 0;
    float tPeak = -200, tPeakFreq = 0, tMin = 0, tfMin = 999, tfMax = 0;
    const uint32_t recent = s.sweeps > 10 ? s.sweeps - 10 : 0;
    for (int i = 0; i < kChannels; i++) {
      const Channel& c = s.ch[i];
      if (c.infrastructure || c.cadHits >= kDebounceHits) continue;
      if (c.lastTransientSweep >= recent && c.lastTransientSweep > 0) {
        n++;
        const float f = channelMHz(i);
        if (c.rssiPeak > tPeak) { tPeak = c.rssiPeak; tPeakFreq = f; }
        if (tMin == 0 || c.rssiPeak < tMin) tMin = c.rssiPeak;
        if (f < tfMin) tfMin = f;
        if (f > tfMax) tfMax = f;
      }
    }
    if (n >= 6 && tPeak > s.noiseFloor + kGfskClassifyDb) {
      // GridDown: under 5 MHz is one strong narrowband transmitter's sidelobes.
      if (tfMax - tfMin < 5.0f) fresh |= put(s, kFixedLora, tPeak, tPeakFreq, n, now);
      else if (tPeak - tMin < 25.0f) fresh |= put(s, kGfskFhss, tPeak, tPeakFreq, n, now);
    }
  }
  // GridDown: 1-2 RSSI-only channels are an unknown ISM emitter.
  if (rssiOnly >= 1 && rssiOnly <= 2) put(s, kIsmUnknown, rssiPeak, rssiPeakFreq, rssiOnly, now);
  return fresh;
}

bool endSweep(State& s, uint32_t now_ms) {
  s.sweeps++;
  // GridDown alternates the CAD per channel; with 52 (even) channels each one
  // keeps its SF, which the decay (one hit a sweep) is tuned for: even
  // channels are checked for ELRS, odd ones for LoRaWAN.
  s.cadRotation = 0;
  // GridDown: adaptive noise floor (median of the sweep).
  float sorted[kChannels];
  for (int i = 0; i < kChannels; i++) sorted[i] = s.ch[i].rssi;
  sortFloats(sorted, kChannels);
  s.noiseFloor = sorted[kChannels / 2];
  // GridDown: decay CAD every 2 sweeps, RSSI every 3.
  if (s.sweeps % 2 == 0) {
    for (Channel& c : s.ch) {
      if (c.cadHits) c.cadHits--;
      if (c.cadHitsLowSF) c.cadHitsLowSF--;
      if (c.cadHitsHighSF) c.cadHitsHighSF--;
    }
  }
  if (s.sweeps % 3 == 0) {
    for (Channel& c : s.ch) if (c.rssiHits) c.rssiHits--;
  }
  // GridDown: debounce; a channel active 80% of the time is infrastructure.
  for (Channel& c : s.ch) {
    c.active = c.cadHits >= kDebounceHits || c.rssiHits >= kDebounceHits;
    if (c.active) { if (c.infraCount < 255) c.infraCount++; }
    else if (c.infraCount) c.infraCount--;
    if (s.sweeps >= 20 && c.infraCount >= kInfraThreshold) c.infrastructure = true;
  }
  const bool fresh = classify(s, now_ms);

  // Age out (GridDown: 10 s; longer here, since sweeps take turns with the mesh).
  for (Detection& d : s.det) {
    if (d.valid && now_ms - d.lastSeen > kAgeMs) {
      d.valid = false;
      d.historyCount = d.historyHead = 0;
    }
  }
  // GridDown: RSSI trend (least squares over the last 30 sweeps), in dB/s.
  const float elapsed_s = (float)(now_ms - s.startMs) / 1000.0f;
  const float sweep_rate = (s.sweeps > 1 && elapsed_s > 0) ? (float)s.sweeps / elapsed_s : 0.25f;
  for (Detection& d : s.det) {
    if (!d.valid) continue;
    d.rssiHistory[d.historyHead] = d.peakRSSI;
    d.historyHead = (uint8_t)((d.historyHead + 1) % 30);
    if (d.historyCount < 30) d.historyCount++;
    d.trendSlope = 0;
    if (d.historyCount >= 6) {
      const int n = d.historyCount;
      float sx = 0, sy = 0, sxy = 0, sx2 = 0;
      for (int i = 0; i < n; i++) {
        const float y = d.rssiHistory[(d.historyHead - n + i + 30) % 30];
        sx += (float)i; sy += y; sxy += (float)i * y; sx2 += (float)i * (float)i;
      }
      const float den = (float)n * sx2 - sx * sx;
      if (den != 0) d.trendSlope = ((float)n * sxy - sx * sy) / den * sweep_rate;
    }
  }
  // GridDown: hop rate of a LoRa FHSS link from its CAD hits per sweep.
  if (s.sweeps >= 5) {
    for (Detection& d : s.det) {
      d.rateCategory = 0;
      d.hitRatio = 0;
      if (!d.valid || d.classification != kLoraFhss) continue;
      float total = 0;
      int n = 0;
      for (const Channel& c : s.ch) {
        if (!c.active || c.infrastructure || c.cadHits < kDebounceHits) continue;
        float r = (float)c.cadHits / (float)s.sweeps;
        if (r > 1.0f) r = 1.0f;
        total += r;
        n++;
      }
      const float avg = n ? total / (float)n : 0;
      d.hitRatio = avg;
      d.rateCategory = avg > 0.50f ? 4 : avg > 0.25f ? 3 : avg > 0.10f ? 2 : avg > 0.02f ? 1 : 0;
    }
  }
  return fresh;
}

float estimateRangeM(float rssi, float tx_dbm) {
  const float fterm = 20.0f * log10f(915.0f);
  float r = powf(10.0f, (tx_dbm - rssi - fterm + 27.55f) / 20.0f);
  if (r < 1.0f) r = 1.0f;
  if (r > 100000.0f) r = 100000.0f;
  return r;
}

}  // namespace scan900
