// SPDX-License-Identifier: GPL-3.0-or-later
// The 900 MHz drone scanner's classifier (src/ui-touch/Scan900.cpp, from
// GridDown Secure Messenger) on simulated sweeps: a quiet band, an ELRS-like
// hopper, a LoRaWAN gateway, SiK telemetry, the standalone's own mesh channel.
//   g++ -std=c++17 -Wall -Wextra -I src test/test_scan900.cpp src/ui-touch/Scan900.cpp -o /tmp/s && /tmp/s
#include <stdio.h>
#include <string.h>

#include "ui-touch/Scan900.h"

using namespace scan900;

static int g_fail = 0;
#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      g_fail++;                                                     \
    }                                                               \
  } while (0)

static uint32_t g_seed = 12345;
static uint32_t rnd() { g_seed = g_seed * 1103515245u + 12345u; return (g_seed >> 8) & 0xFFFF; }
static float noise() { return -121.0f + (float)(rnd() % 30) / 10.0f; }   // -121..-118 dBm

static const float kOwn[2] = {910.525f, 911.150f};   // mesh, radio2

static void start(State& s) {
  float cal[kChannels];
  for (int i = 0; i < kChannels; i++) cal[i] = noise();
  begin(s, cal, kOwn, 2, 0);
}

// One sweep. `lit(i, low_sf)` says whether channel i shows CAD at that SF,
// `power(i)` its RSSI (0: noise). Returns endSweep's result.
template <typename Lit, typename Power>
static bool sweep(State& s, uint32_t now, Lit lit, Power power) {
  for (int i = 0; i < kChannels; i++) {
    const bool low = nextCadLowSF(s);
    const bool cad = lit(i, low);
    const float p = power(i);
    const float w = p != 0 ? p : noise();
    const bool nar = wantNarrow(s, w, cad);
    sample(s, i, cad, low, w, nar, w - (cad && low ? 9.0f : 1.0f));
  }
  return endSweep(s, now);
}

static int count(const State& s, uint8_t cls) {
  int n = 0;
  for (const Detection& d : s.det) n += (d.valid && d.classification == cls) ? 1 : 0;
  return n;
}

static void testQuiet() {
  State s;
  start(s);
  for (int k = 0; k < 40; k++) {
    CHECK(!sweep(s, 4000u * (uint32_t)k, [](int, bool) { return false; }, [](int) { return 0.0f; }));
  }
  for (const Detection& d : s.det) CHECK(!d.valid);
  CHECK(s.noiseFloor < -115 && s.noiseFloor > -125);
}

// The mesh's own channel, loud every sweep: blanked, never a detection.
static void testOwnChannel() {
  State s;
  start(s);
  for (int i = 0; i < kChannels; i++) {
    const float f = channelMHz(i);
    const bool own = (f > 909.2f && f < 912.5f);
    CHECK(s.ch[i].infrastructure == own);
  }
  for (int k = 0; k < 30; k++) {
    sweep(s, 4000u * (uint32_t)k, [](int i, bool) { return channelMHz(i) == 910.5f; },
          [](int i) { return (channelMHz(i) > 909.0f && channelMHz(i) < 912.0f) ? -60.0f : 0.0f; });
  }
  for (const Detection& d : s.det) CHECK(!d.valid);
}

// ELRS-like: SF6/BW500 bursts on most of the band (gap included), every sweep.
static void testElrs() {
  State s;
  start(s);
  bool alerted = false;
  int alerts = 0;
  for (int k = 0; k < 12; k++) {
    const bool fresh = sweep(s, 4000u * (uint32_t)k, [](int i, bool low) { return low && (i % 3 != 1); },
                             [](int i) { return (i % 3 != 1) ? -78.0f - (float)(i % 5) : 0.0f; });
    if (fresh) { alerted = true; alerts++; }
  }
  CHECK(alerted);
  CHECK(alerts == 1);   // one alert for a detection that stays
  CHECK(count(s, kLoraFhss) == 1);
  for (const Detection& d : s.det)
    if (d.valid && d.classification == kLoraFhss) {
      CHECK(d.channelCount >= 10);
      CHECK(d.peakFreq >= 902.0f && d.peakFreq <= 927.5f);
      CHECK(d.rateCategory > 0);
    }
  // It goes quiet: hit counts decay a sweep at a time, then it ages out.
  int k = 12;
  for (; k < 60 && count(s, kLoraFhss); k++)
    sweep(s, 4000u * (uint32_t)k, [](int, bool) { return false; }, [](int) { return 0.0f; });
  CHECK(count(s, kLoraFhss) == 0);
  CHECK(k > 14 && k < 50);
}

// A LoRaWAN gateway's sub-band: SF10/BW125 on 903.0-904.5, outside the gap.
static void testLorawan() {
  State s;
  start(s);
  bool alerted = false;
  for (int k = 0; k < 12; k++) {
    alerted |= sweep(s, 4000u * (uint32_t)k,
                     [](int i, bool low) { return !low && channelMHz(i) >= 903.0f && channelMHz(i) <= 904.5f; },
                     [](int i) { return (channelMHz(i) >= 903.0f && channelMHz(i) <= 904.5f) ? -85.0f : 0.0f; });
  }
  CHECK(!alerted);
  CHECK(count(s, kLoraFhss) == 0);
  CHECK(count(s, kLorawan) == 1);
}

// SiK-like GFSK telemetry: energy (no CAD) across the band, strong.
static void testSik() {
  State s;
  start(s);
  bool alerted = false;
  for (int k = 0; k < 15; k++) {
    alerted |= sweep(s, 4000u * (uint32_t)k, [](int, bool) { return false; },
                     [](int i) { return (i % 4 == 0) ? -82.0f - (float)(i % 3) : 0.0f; });
  }
  CHECK(alerted);
  CHECK(count(s, kGfskFhss) == 1);
  CHECK(count(s, kLoraFhss) == 0);
}

static void testLabelsAndRange() {
  CHECK(!strcmp(classLabel(kLoraFhss), "LoRa FHSS (ELRS/Crossfire)"));
  CHECK(isDroneClass(kGfskFhss) && !isDroneClass(kFixedLora) && !isDroneClass(kLorawan));
  const float r = estimateRangeM(-70.0f, 24.0f);   // ELRS 250 mW heard at -70 dBm
  CHECK(r > 500.0f && r < 3000.0f);
  CHECK(estimateRangeM(20.0f, 0.0f) == 1.0f);
}

int main() {
  testQuiet();
  testOwnChannel();
  testElrs();
  testLorawan();
  testSik();
  testLabelsAndRange();
  if (g_fail) {
    printf("%d failure(s)\n", g_fail);
    return 1;
  }
  printf("scan900: all passed\n");
  return 0;
}
