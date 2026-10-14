// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaMesh settings, CLI and scan timing (src/helpers/MediaMeshCore.h).
// Timing figures are checked against mikecarper/MeshCore's published
// radio_profiles.md table for a SF7/62.5 main profile and an 840 us guarded
// switch: radio2 SF9/500 -> 32, SF8/500 -> 40, SF7/500 -> 64 symbols, and a
// 21.667 ms fast visit with a 32-symbol main preamble.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "helpers/MediaMeshCore.h"

using namespace mediamesh;

static Params P(float f, float bw, uint8_t sf, uint8_t cr, uint16_t pre = 0) {
  Params p; p.freq = f; p.bw = bw; p.sf = sf; p.cr = cr; p.preamble = pre; return p;
}

static void testTiming() {
  const Params main = P(910.525f, 62.5f, 7, 5);
  Timing t = calcTiming(main, 32, P(916.0f, 500, 9, 5), 840);
  assert(t.valid && t.slow == 0 && t.need[1] == 32 && t.preamble[1] == 32);
  t = calcTiming(main, 32, P(916.0f, 500, 8, 5), 840);
  assert(t.need[1] == 40 && t.preamble[1] == 40);
  t = calcTiming(main, 32, P(916.0f, 500, 7, 5), 840);
  assert(t.need[1] == 64 && t.preamble[1] == 64);
  assert(t.dwell_us[0] == 9421);
  assert(t.dwell_us[1] == 21667);
  assert(t.main_ok && t.preamble[0] == 32 && !t.media_capped);

  // An explicit radio2 preamble is used as entered.
  t = calcTiming(main, 32, P(916.0f, 500, 7, 5, 96), 840);
  assert(t.preamble[1] == 96 && t.need[1] == 64);

  // A slower radio2 picks its own preamble so the stock main preamble still works.
  t = calcTiming(P(910.525f, 250, 7, 5), 32, P(916.0f, 62.5f, 9, 5), 840);
  assert(t.valid && t.slow == 1);
  assert(t.preamble[1] >= 32 && t.preamble[1] % 8 == 0);
  assert(t.dwell_us[1] == (uint32_t)ceil(4.6 * symbolUs(62.5f, 9)));

  // A very slow switch cannot fit inside a 16-symbol SF10 main preamble.
  t = calcTiming(P(910.525f, 250, 10, 5), 16, P(916.0f, 500, 7, 5), 20000);
  assert(t.valid && !t.main_ok);
}

static void testAirtime() {
  // SF7/62.5, 32-symbol preamble, 255 bytes: about 0.85 s.
  const uint32_t us = timeOnAirUs(P(910.525f, 62.5f, 7, 5), 32, 255);
  assert(us > 840000 && us < 860000);
  // 500 kHz is 8x faster per symbol.
  assert(timeOnAirUs(P(916.0f, 500, 7, 5), 32, 255) * 7 < us);
}

static void testBlob() {
  Config c; defaults(c);
  c.mode = MODE_RXTX; c.route = ROUTE_RADIO2; c.p.preamble = 72;
  uint8_t b[kBlobSize];
  assert(encode(c, b) == kBlobSize);
  Config d;
  assert(decode(b, sizeof b, d));
  assert(d.mode == MODE_RXTX && d.route == ROUTE_RADIO2 && d.p.preamble == 72 && sameParams(d.p, c.p));
  b[2] = 99;
  assert(!decode(b, sizeof b, d));
  assert(!decode(b, 4, d));
}

static void testCli() {
  Config c; defaults(c);
  const Params main = P(910.525f, 62.5f, 7, 5);
  Timing t;
  char out[160];

  CliResult r = handleCli("get radio2", c, main, t, true, out, sizeof out);
  assert(r.handled && !r.changed && !strcmp(out, "> off"));
  r = handleCli("get tx.media", c, main, t, true, out, sizeof out);
  assert(!strcmp(out, "> radio"));

  r = handleCli("set radio2 916.0,250,7,5,rxtx", c, main, t, true, out, sizeof out);
  assert(r.handled && r.changed && !strncmp(out, "OK", 2));
  assert(c.mode == MODE_RXTX && c.p.sf == 7 && c.p.bw == 250.0f);
  r = handleCli("get radio2", c, main, t, true, out, sizeof out);
  assert(!strcmp(out, "> 916.000,250.00,7,5,rxtx"));

  r = handleCli("set tx.media radio2", c, main, t, true, out, sizeof out);
  assert(r.changed && mediaOnRadio2(c) && !strcmp(out, "OK - tx.media radio2"));
  r = handleCli("set tx.media radio2", c, main, t, true, out, sizeof out);
  assert(!r.changed);

  r = handleCli("set radio2 916.0,250,7,5,rx,64", c, main, t, true, out, sizeof out);
  assert(r.changed && c.mode == MODE_RX && c.p.preamble == 64 && !mediaOnRadio2(c));
  r = handleCli("get radio2", c, main, t, true, out, sizeof out);
  assert(!strcmp(out, "> 916.000,250.00,7,5,rx,64"));

  // Rejected: same channel as the main radio, bad bandwidth, bad mode, short input.
  Config before = c;
  r = handleCli("set radio2 910.525,62.5,7,5,rxtx", c, main, t, true, out, sizeof out);
  assert(r.handled && !r.changed && !strncmp(out, "Error", 5) && sameParams(c.p, before.p));
  r = handleCli("set radio2 916.0,300,7,5,rxtx", c, main, t, true, out, sizeof out);
  assert(!strncmp(out, "Error", 5));
  r = handleCli("set radio2 916.0,250,7,5,both", c, main, t, true, out, sizeof out);
  assert(!strncmp(out, "Error", 5));
  r = handleCli("set radio2 916.0,250", c, main, t, true, out, sizeof out);
  assert(!strncmp(out, "Error", 5));
  r = handleCli("set tx.media maybe", c, main, t, true, out, sizeof out);
  assert(!strncmp(out, "Error", 5));

  r = handleCli("set radio2.cross off", c, main, t, true, out, sizeof out);
  assert(r.handled && !r.changed && !strncmp(out, "OK", 2));
  r = handleCli("get radio2.cross", c, main, t, true, out, sizeof out);
  assert(!strcmp(out, "> off"));

  r = handleCli("set radio2 off", c, main, t, true, out, sizeof out);
  assert(r.changed && c.mode == MODE_OFF && !scanning(c));

  // Unsupported radios still answer, so a client can tell.
  r = handleCli("set radio2 916.0,250,7,5,rxtx", c, main, t, false, out, sizeof out);
  assert(r.handled && !r.changed && strstr(out, "not supported"));
  r = handleCli("get mediamesh", c, main, t, false, out, sizeof out);
  assert(strstr(out, "unsupported"));

  // Not ours.
  r = handleCli("get radio", c, main, t, true, out, sizeof out);
  assert(!r.handled);
  r = handleCli("reboot", c, main, t, true, out, sizeof out);
  assert(!r.handled);

  // Timing readout.
  c.mode = MODE_RXTX;
  Timing tt = calcTiming(main, 32, P(916.0f, 500, 7, 5), 840);
  c.p = P(916.0f, 500, 7, 5);
  r = handleCli("get radio2.timing", c, main, tt, true, out, sizeof out);
  assert(strstr(out, "preamble=32,64") && strstr(out, "switch=840us"));
}

static void testMarks() {
  MarkRing m;
  uint8_t h[kHashSize] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t g[kHashSize] = {9, 9, 9, 9, 9, 9, 9, 9};
  assert(!m.has(h, 0));
  m.add(h, 1000);
  assert(m.has(h, 2000) && !m.has(g, 2000));
  assert(!m.has(h, 1000 + kMarkTtlMs + 1));
  // The ring keeps the newest kSlots.
  for (uint8_t i = 0; i < MarkRing::kSlots; ++i) { uint8_t x[kHashSize] = {i, 0xAA}; m.add(x, 3000); }
  assert(!m.has(h, 3000));
  uint8_t last[kHashSize] = {MarkRing::kSlots - 1, 0xAA};
  assert(m.has(last, 3000));
}

int main() {
  testTiming();
  testAirtime();
  testBlob();
  testCli();
  testMarks();
  printf("mediamesh core: ok\n");
  return 0;
}
