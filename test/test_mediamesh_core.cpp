// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaMesh / radio2 settings, CLI, routing rules, schedules and scan timing
// (src/helpers/MediaMeshCore.h). Timing figures are checked against
// mikecarper/MeshCore's published radio_profiles.md table for a SF7/62.5 main
// profile and an 840 us guarded switch: radio2 SF9/500 -> 32, SF8/500 -> 40,
// SF7/500 -> 64 symbols, and a 21.667 ms fast visit with a 32-symbol main
// preamble. The crossing table is radio_profiles.md's "Choose whether traffic
// crosses between profiles".

#include <assert.h>
#include <initializer_list>
#include <stdio.h>
#include <string.h>

#include "helpers/MediaMeshCore.h"

using namespace mediamesh;

static Params P(float f, float bw, uint8_t sf, uint8_t cr, uint16_t pre = 0) {
  Params p; p.freq = f; p.bw = bw; p.sf = sf; p.cr = cr; p.preamble = pre; return p;
}

static Env E(uint32_t now_ms = 1000, uint32_t epoch = 1800000000UL) {
  Env e;
  e.main = e.main_saved = P(910.525f, 62.5f, 7, 5);
  e.main_preamble = 32;
  e.switch_us = 840;
  e.now_ms = now_ms;
  e.epoch = epoch;
  return e;
}

static CliResult run(const char* cmd, Runtime& r, const Env& e, char* out, size_t cap = 200) {
  out[0] = 0;
  return handleCli(cmd, r, e, out, cap);
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

  // Warnings: a short explicit radio2 preamble is reported, not rewritten.
  t = calcTiming(main, 32, P(916.0f, 500, 7, 5, 32), 840);
  char w[160] = "";
  appendWarnings(t, P(916.0f, 500, 7, 5, 32), w, sizeof w);
  assert(strstr(w, "WARN recommended preamble: radio2=64") && strstr(w, "short override: radio2"));
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
  assert(c.media == TX_AUTO && c.cross == CROSS_AUTO && c.mode == MODE_OFF);
  c.mode = MODE_RXTX; c.media = TX_BOTH; c.cross = CROSS_ON; c.p.preamble = 72;
  uint8_t b[kBlobSize];
  assert(encode(c, b) == kBlobSize);
  Config d;
  assert(decode(b, sizeof b, d));
  assert(d.mode == MODE_RXTX && d.media == TX_BOTH && d.cross == CROSS_ON && d.p.preamble == 72 && sameParams(d.p, c.p));
  b[2] = 99;
  assert(!decode(b, sizeof b, d));
  assert(!decode(b, 4, d));

  // A v1 blob keeps its behaviour: media where it was, nothing crossing.
  uint8_t v1[kBlobSize];
  encode(c, v1);
  v1[2] = 1; v1[4] = 1; v1[7] = 0;
  assert(decode(v1, sizeof v1, d) && d.media == TX_RADIO2 && d.cross == CROSS_OFF && mediaOnRadio2(d));
  v1[4] = 0;
  assert(decode(v1, sizeof v1, d) && d.media == TX_RADIO && d.cross == CROSS_OFF && !mediaOnRadio2(d));
}

// radio_profiles.md: | primary | secondary | auto | on | off |
static void testCrossTable() {
  Runtime r;
  defaults(r.saved);
  r.saved.mode = MODE_RXTX;
  r.saved.p = P(916.0f, 250, 7, 5);
  struct Row { bool t1, t2; bool cross_auto; };
  const Row rows[] = { {false, false, true}, {true, true, true}, {false, true, false}, {true, false, false} };
  for (const Row& row : rows) {
    r.temp1 = row.t1;
    r.temp2 = row.t2;
    r.temp2_prof = savedProfile(r.saved);
    r.saved.cross = CROSS_AUTO;
    assert(transmitMask(r, 0) == (row.cross_auto ? 3 : 1));
    assert(transmitMask(r, 1) == (row.cross_auto ? 3 : 2));
    r.saved.cross = CROSS_ON;
    assert(transmitMask(r, 0) == 3 && transmitMask(r, 1) == 3);
    r.saved.cross = CROSS_OFF;
    assert(transmitMask(r, 0) == 1 && transmitMask(r, 1) == 2);
  }
  r.temp1 = r.temp2 = false;
  // Crossing respects rx: an rx-only radio2 never transmits under the global rules.
  r.saved.mode = MODE_RX;
  r.saved.cross = CROSS_ON;
  assert(transmitMask(r, 0) == 1 && transmitMask(r, 1) == 1);

  // Explicit choices override crossing; radio2 never falls back.
  r.saved.mode = MODE_RXTX;
  r.saved.cross = CROSS_OFF;
  assert(policyMask(r, TX_BOTH) == 3 && policyMask(r, TX_RADIO2) == 2 && policyMask(r, TX_RADIO) == 1);
  assert(policyMask(r, TX_OFF) == 0 && policyMask(r, TX_AUTO) == 1);
  r.saved.mode = MODE_RX;
  assert(policyMask(r, TX_RADIO2) == 0 && policyMask(r, TX_BOTH) == 1);
  r.saved.mode = MODE_OFF;
  assert(policyMask(r, TX_RADIO2) == 0 && policyMask(r, TX_BOTH) == 1 && transmitMask(r, 0) == 1);
}

static void testCli() {
  Runtime r;
  defaults(r.saved);
  Env e = E();
  char out[200];

  CliResult res = run("get radio2", r, e, out);
  assert(res.handled && !res.save && !strcmp(out, "> off"));
  run("get tx.media", r, e, out);
  // Worded as keymindCascade answers.
  assert(!strcmp(out, "> tx.media auto; active=channel/global"));

  res = run("set radio2 916.0,250,7,5,rxtx", r, e, out);
  assert(res.handled && res.save && res.apply && !strncmp(out, "OK - radio2 rxtx; preamble=", 27));
  assert(r.saved.mode == MODE_RXTX && r.saved.p.sf == 7 && r.saved.p.bw == 250.0f);
  run("get radio2", r, e, out);
  assert(!strncmp(out, "> 916.000,250.000,7,5,rxtx,", 27) && strstr(out, "(auto)"));
  // auto follows each channel's choice (and radio2.cross), as keymindCascade.
  run("get tx.media", r, e, out);
  assert(!strcmp(out, "> tx.media auto; active=channel/global"));

  res = run("set tx.media radio2", r, e, out);
  assert(res.save && mediaOnRadio2(r.saved) && !strcmp(out, "OK - tx.media radio2; active=radio2"));
  run("get tx.media", r, e, out);
  assert(!strcmp(out, "> tx.media radio2; active=radio2"));
  res = run("set tx.media radio2", r, e, out);
  assert(!res.save);
  for (const char* p : {"auto", "radio", "both", "off"}) {
    char cmd[40];
    snprintf(cmd, sizeof cmd, "set tx.media %s", p);
    run(cmd, r, e, out);
    assert(!strncmp(out, "OK - tx.media ", 14));
  }
  run("set tx.media radio2", r, e, out);

  res = run("set radio2 916.0,250,7,5,rx,64", r, e, out);
  assert(res.save && r.saved.mode == MODE_RX && r.saved.p.preamble == 64 && !mediaOnRadio2(r.saved));
  run("get radio2", r, e, out);
  assert(!strncmp(out, "> 916.000,250.000,7,5,rx,64", 27) && !strstr(out, "(auto)"));
  run("set tx.media both", r, e, out);
  assert(!strcmp(out, "OK - tx.media both; active=radio (radio2 unavailable for TX)"));
  run("set tx.media radio2", r, e, out);
  assert(!strcmp(out, "OK - tx.media radio2; active=off (radio2 unavailable for TX)"));
  run("get tx.media", r, e, out);
  assert(!strcmp(out, "> tx.media radio2; active=off (radio2 unavailable for TX)"));

  // Rejected: same channel as the main radio, bad bandwidth, bad mode, short input.
  const Config before = r.saved;
  res = run("set radio2 910.525,62.5,7,5,rxtx", r, e, out);
  assert(res.handled && !res.save && !strncmp(out, "Error", 5) && sameParams(r.saved.p, before.p));
  run("set radio2 916.0,300,7,5,rxtx", r, e, out);
  assert(!strncmp(out, "Error", 5));
  run("set radio2 916.0,250,7,5,both", r, e, out);
  assert(!strncmp(out, "Error", 5));
  run("set radio2 916.0,250", r, e, out);
  assert(!strncmp(out, "Error", 5));
  run("set tx.media maybe", r, e, out);
  assert(!strcmp(out, "Error: use get tx.media or set tx.media auto|radio|radio2|both|off"));

  res = run("set radio2.cross off", r, e, out);
  assert(res.handled && res.save && !strcmp(out, "OK"));
  run("get radio2.cross", r, e, out);
  assert(!strcmp(out, "> off"));
  run("set radio2.cross sideways", r, e, out);
  assert(!strncmp(out, "Error", 5));

  // Diagnostics answer and stay read-only.
  run("get radio2.status", r, e, out);
  assert(!strncmp(out, "> rx; RX=0,0 TX=0,0 switches=0", 30) && strstr(out, "test=0/4 pending"));
  run("get radio2.scan", r, e, out);
  assert(!strncmp(out, "> slow=radio; listen_us=9421,", 29));
  run("get radio.timing", r, e, out);
  assert(!strncmp(out, "> chirps=4.60,", 14) && strstr(out, "need=32,"));
  run("set radio2.status x", r, e, out);
  assert(strstr(out, "read-only"));

  res = run("set radio2 off", r, e, out);
  assert(res.save && res.apply && r.saved.mode == MODE_OFF && !scanning(r.saved) && !strcmp(out, "OK - single radio"));

  // Unsupported radios still answer, so a client can tell.
  e.supported = false;
  run("set radio2 916.0,250,7,5,rxtx", r, e, out);
  assert(strstr(out, "not supported"));
  run("get mediamesh", r, e, out);
  assert(strstr(out, "unsupported"));
  run("set tx.media radio2", r, e, out);
  assert(strstr(out, "not supported"));
  e.supported = true;

  // Not ours.
  assert(!run("get radio", r, e, out).handled);
  assert(!run("reboot", r, e, out).handled);
  assert(!run("del radio2", r, e, out).handled);
  assert(!run("radio2", r, e, out).handled);

  // Timing readout with the default figures.
  r.saved.mode = MODE_RXTX;
  r.saved.p = P(916.0f, 500, 7, 5);
  run("get radio2.timing", r, e, out);
  assert(strstr(out, "preamble=32,64") && strstr(out, "switch=840us"));
}

static void testTemp2() {
  Runtime r;
  defaults(r.saved);
  r.saved.mode = MODE_RXTX;
  r.saved.p = P(916.0f, 250, 7, 5);
  Env e = E(1000);
  char out[200];

  CliResult res = run("set tempradio2 910.5,500,8,5,rxtx,120", r, e, out);
  assert(res.apply && !res.save && r.temp2 && !strncmp(out, "OK - tempradio2 0d2h0m (120 min); preamble=", 43));
  assert(r.active2().p.sf == 8);
  // Permanent primary + temporary radio2: isolated under auto.
  assert(transmitMask(r, 0) == 1);
  run("get tempradio2", r, e, out);
  assert(!strncmp(out, "> 910.500,500.000,8,5,rxtx,", 27) && strstr(out, "(120 min) left"));
  run("get radio2", r, e, out);
  assert(!strncmp(out, "> 916.000,250.000,7,5,rxtx", 26));   // the saved one
  // "tempradio2 <tuple>" without get/set also sets, as the fork.
  run("get tempradio2 910.5,500,8,5", r, e, out);
  assert(!strncmp(out, "Error", 5));

  // Expiry restores the saved profile.
  TickResult t = tick(r, 1000 + 119 * 60000UL);
  assert(!t.apply && r.temp2);
  t = tick(r, 1000 + 120 * 60000UL);
  assert(t.apply && !r.temp2 && r.active2().p.sf == 7);

  // set tempradio2 off ends it early.
  run("set tempradio2 910.5,500,8,5,rx,30", r, e, out);
  assert(r.temp2 && r.active2().mode == MODE_RX);
  res = run("set tempradio2 off", r, e, out);
  assert(res.apply && !r.temp2 && !strcmp(out, "OK - saved radio2 restored"));
  run("get tempradio2", r, e, out);
  assert(!strcmp(out, "> off"));

  // Bad minutes.
  run("set tempradio2 910.5,500,8,5,rxtx,0", r, e, out);
  assert(!strncmp(out, "Error", 5) && !r.temp2);
  run("set tempradio2 910.5,500,8,5,rxtx,10081", r, e, out);
  assert(!strncmp(out, "Error", 5));
}

static void testSchedules() {
  Runtime r;
  defaults(r.saved);
  const uint32_t epoch = 1800000000UL;
  Env e = E(5000, epoch);
  char out[200];

  CliResult res = run("set radioat2 910.5,500,8,5,rxtx,+5", r, e, out);
  assert(res.handled && !res.apply && !strncmp(out, "OK - radioat2 1 queued, preamble=", 33));
  run("get radioat2", r, e, out);
  char want[40];
  snprintf(want, sizeof want, "> slots: 1@%lu ", (unsigned long)(epoch + 300));
  assert(!strcmp(out, want));
  run("get radioat2 1", r, e, out);
  assert(!strncmp(out, "> 910.500,500.000,8,5,rxtx", 26) && strstr(out, "; @1800000300-0"));
  run("get radioat2 2", r, e, out);
  assert(!strcmp(out, "> off"));

  // The temporary schedule must not overlap another.
  res = run("set tempradioat2 910.7,500,9,5,rx,+10,+20", r, e, out);
  assert(!strncmp(out, "OK - tempradioat2 1 queued", 26));
  run("set tempradioat2 910.7,500,9,5,rx,+15,+25", r, e, out);
  assert(!strcmp(out, "Error: temporary schedules overlap"));
  // A tempradio2 is refused while a temporary schedule waits.
  run("set tempradio2 910.7,500,9,5,rx,5", r, e, out);
  assert(!strcmp(out, "Error: clear tempradioat2 schedules first"));

  // Bad times.
  run("set radioat2 910.5,500,8,5,rxtx,100", r, e, out);
  assert(strstr(out, "future UTC"));
  run("set radioat2 910.5,500,8,5,rxtx,+50000", r, e, out);
  assert(strstr(out, "future UTC"));
  Env noclock = E(5000, 0);
  run("set radioat2 910.5,500,8,5,rxtx,+5", r, noclock, out);
  assert(strstr(out, "future UTC"));

  // radioat2 fires at +5 min and becomes the saved profile.
  TickResult t = tick(r, 5000 + 5 * 60000UL);
  assert(t.save && t.apply && r.saved.mode == MODE_RXTX && r.saved.p.sf == 8 && !r.sched[0].active);
  // tempradioat2 runs from +10 to +20 min, then the saved profile returns.
  t = tick(r, 5000 + 10 * 60000UL);
  assert(t.apply && r.temp2 && r.active2().p.sf == 9 && r.sched[4].started);
  run("get tempradio2", r, e, out);
  assert(strstr(out, "910.700"));
  t = tick(r, 5000 + 20 * 60000UL);
  assert(t.apply && !r.temp2 && !r.sched[4].active && r.active2().p.sf == 8);

  // del, and set radio2 off clears everything.
  run("set radioat2 910.5,500,8,5,rxtx,+30", r, e, out);
  run("set radioat2 910.5,500,8,5,rxtx,+31", r, e, out);
  assert(!strncmp(out, "OK - radioat2 2 queued", 22));
  run("del radioat2 1", r, e, out);
  assert(!strcmp(out, "OK") && !r.sched[0].active && r.sched[1].active);
  run("del radioat2 9", r, e, out);
  assert(!strncmp(out, "Error: slot", 11));
  run("set tempradioat2 910.7,500,9,5,rx,+40,+50", r, e, out);
  run("set radio2 off", r, e, out);
  for (const Schedule& s : r.sched) assert(!s.active);

  // Four slots per family.
  for (int i = 0; i < 4; ++i) {
    char cmd[64];
    snprintf(cmd, sizeof cmd, "set radioat2 910.5,500,8,5,rxtx,+%d", 10 + i);
    run(cmd, r, e, out);
    assert(!strncmp(out, "OK", 2));
  }
  run("set radioat2 910.5,500,8,5,rxtx,+20", r, e, out);
  assert(!strcmp(out, "Error: all four schedule slots occupied"));
  run("del radioat2 all", r, e, out);
  for (int i = 0; i < 4; ++i) assert(!r.sched[i].active);
}

static void testTempradio() {
  Runtime r;
  defaults(r.saved);
  Env e = E(2000);
  char out[200];
  CliResult res = run("tempradio", r, e, out);
  assert(res.handled && !strcmp(out, "TempRadio inactive"));
  res = run("tempradio 869.525,250,11,5,30", r, e, out);
  assert(res.apply_primary && r.temp1 && !strcmp(out, "OK - temp params for 30 mins"));
  assert(r.temp1_p.sf == 11 && r.temp1_p.bw == 250.0f);
  run("get tempradio", r, e, out);
  assert(!strcmp(out, "TempRadio active: 869.525,250.00,11,5 1800s left"));
  run("tempradio 869.525,250,11,5,0", r, e, out);
  assert(!strncmp(out, "ERR usage", 9));
  TickResult t = tick(r, 2000 + 30 * 60000UL);
  assert(t.apply_primary && !r.temp1);
  run("set tempradio 869.525,250,11,5,30,64", r, e, out);
  assert(r.temp1 && r.temp1_p.preamble == 64);
  res = run("normalradio", r, e, out);
  assert(res.apply_primary && !r.temp1 && !strncmp(out, "OK - normal radio", 17));
}

static void testMarks() {
  MarkRing m;
  uint8_t h[kHashSize] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t g[kHashSize] = {9, 9, 9, 9, 9, 9, 9, 9};
  assert(m.find(h, 0) == kNoMark);
  m.add(h, 1000, 2);
  assert(m.find(h, 2000) == 2 && !m.has(g, 2000));
  m.add(h, 1500, 3);   // re-marked: the newer choice
  assert(m.find(h, 2000) == 3);
  assert(!m.has(h, 1500 + kMarkTtlMs + 1));
  // The ring keeps the newest kSlots.
  for (uint8_t i = 0; i < MarkRing::kSlots; ++i) { uint8_t x[kHashSize] = {i, 0xAA}; m.add(x, 3000, 1); }
  assert(!m.has(h, 3000));
  uint8_t last[kHashSize] = {MarkRing::kSlots - 1, 0xAA};
  assert(m.find(last, 3000) == 1);

  // The same packet heard on radio2, then on the main profile: each copy is
  // credited to the profile it came in on, oldest first.
  OriginRing o;
  assert(o.take(h, 0) == kNoMark);
  o.add(h, 100, 1);
  o.add(h, 300, 0);
  assert(o.take(h, 400) == 1);
  assert(o.take(h, 400) == 0);
  assert(o.take(h, 400) == kNoMark);
  o.add(h, 500, 1);
  assert(o.take(h, 500 + OriginRing::kTtlMs + 1) == kNoMark);

  // Queued packets keep their profiles until a profile changes.
  QueueMarks q;
  int a = 0, b = 0;
  const uint32_t g1[2] = {1, 1};
  assert(q.live(&a, g1) == kNoMark && !q.has(&a));
  assert(q.set(&a, 3, g1) && q.set(&b, 2, g1) && q.has(&a));
  assert(q.live(&a, g1) == 3 && q.live(&b, g1) == 2);
  const uint32_t g2[2] = {1, 2};   // radio2 changed
  assert(q.live(&a, g2) == 1 && q.live(&b, g2) == 0);
  const uint32_t g3[2] = {2, 1};   // the main profile changed (tempradio)
  assert(q.live(&a, g3) == 2);
  q.erase(&a);
  assert(!q.has(&a) && q.has(&b));
  int many[QueueMarks::kSlots];
  QueueMarks full;
  for (int i = 0; i < QueueMarks::kSlots; ++i) assert(full.set(&many[i], 1, g1));
  assert(!full.set(&a, 1, g1));
}

static void testRoutes() {
  Routes t;
  const uint8_t a[8] = {1, 1, 1, 1, 1, 1, 1, 1}, b[8] = {2, 2, 2, 2, 2, 2, 2, 2};
  assert(t.get(Routes::USER, a) == TX_AUTO);
  assert(t.set(Routes::USER, a, TX_RADIO2) && t.set(Routes::CHANNEL, a, TX_BOTH) && t.set(Routes::USER, b, TX_OFF));
  assert(t.get(Routes::USER, a) == TX_RADIO2 && t.get(Routes::CHANNEL, a) == TX_BOTH && t.get(Routes::USER, b) == TX_OFF);
  assert(t.count(Routes::USER) == 2 && t.count(Routes::CHANNEL) == 1);
  uint8_t blob[Routes::kBlobMax];
  const size_t n = t.encode(blob);
  Routes u;
  assert(u.decode(blob, n) && u.get(Routes::USER, a) == TX_RADIO2 && u.get(Routes::CHANNEL, a) == TX_BOTH);
  assert(t.set(Routes::USER, a, TX_AUTO) && t.get(Routes::USER, a) == TX_AUTO && t.count(Routes::USER) == 1);
  // Full table.
  Routes f;
  for (int i = 0; i < Routes::kMax; ++i) { uint8_t id[8] = {(uint8_t)i, 7}; assert(f.set(Routes::USER, id, TX_RADIO)); }
  uint8_t extra[8] = {0xEE};
  assert(!f.set(Routes::USER, extra, TX_RADIO));
  assert(f.set(Routes::USER, extra, TX_AUTO));   // clearing never fails

  RouteCommand c = parseRouteCommand("set tx.user \"Alice Jones\" radio2");
  assert(c.matched && c.valid && c.set && !c.channel && c.quoted && !strcmp(c.target, "Alice Jones") && c.policy == TX_RADIO2);
  c = parseRouteCommand("get tx.channel 0");
  assert(c.valid && !c.set && c.channel && !c.quoted && !strcmp(c.target, "0"));
  c = parseRouteCommand("set tx.channel #wardriving radio");
  assert(c.valid && !strcmp(c.target, "#wardriving") && c.policy == TX_RADIO);
  c = parseRouteCommand("get tx.user");
  assert(c.valid && !c.target[0]);
  c = parseRouteCommand("set tx.user Bob sideways");
  assert(c.matched && !c.valid);
  c = parseRouteCommand("set tx.user both");
  assert(c.matched && !c.valid);
  c = parseRouteCommand("get tx.users");
  assert(!c.matched);
  uint8_t key[32];
  size_t kb = 0;
  assert(parseRouteKey("0123456789AB", key, kb) && kb == 6 && key[0] == 0x01 && key[5] == 0xab);
  assert(!parseRouteKey("0123456789A", key, kb) && !parseRouteKey("01234567", key, kb) && !parseRouteKey("0123456789zz", key, kb));
}

// Not SHA-256: any fixed function shows the key replacement logic.
static void fakeHash(uint8_t* out, size_t n, const uint8_t* text, int len) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < len; ++i) h = (h ^ text[i]) * 16777619u;
  for (size_t i = 0; i < n; ++i) { h = h * 1103515245u + 12345u; out[i] = (uint8_t)(h >> 16); }
}

static void testAlias() {
  assert(parseAlias("#r2-emergency").kind == ALIAS_R2 && !strcmp(parseAlias("#r2-emergency").tail, "emergency"));
  assert(parseAlias("#emergency").kind == ALIAS_NONE);
  assert(parseAlias("r2-Private").kind == ALIAS_R2 && !parseAlias("r2-Private").hashtag);
  assert(parseAlias("#r2-").kind == ALIAS_INVALID && parseAlias("#r2-r2-x").kind == ALIAS_INVALID);
  assert(parseAlias("#r2-mt-x").kind == ALIAS_INVALID);

  // The app derives #r2-emergency's key from its name; the radio uses #emergency's.
  uint8_t secret[32] = {}, plain[32] = {};
  fakeHash(secret, 16, (const uint8_t*)"#r2-emergency", 13);
  fakeHash(plain, 16, (const uint8_t*)"#emergency", 10);
  assert(resolveAliasKey("#r2-emergency", secret, fakeHash) && !memcmp(secret, plain, 16));
  // A custom key is kept, and so is a non-hashtag name's.
  uint8_t custom[32] = {9, 9, 9};
  assert(resolveAliasKey("#r2-emergency", custom, fakeHash) && custom[0] == 9);
  uint8_t priv[32] = {};
  fakeHash(priv, 16, (const uint8_t*)"r2-Private", 10);
  uint8_t keep[32];
  memcpy(keep, priv, 32);
  assert(resolveAliasKey("r2-Private", priv, fakeHash) && !memcmp(priv, keep, 32));
  assert(!resolveAliasKey("#r2-", secret, fakeHash));

  // Receive order: radio2 packets prefer the r2- slot; radio1 never reaches it.
  assert(aliasReceivePriority("#r2-x", 1, true) == 0 && aliasReceivePriority("#x", 1, true) == 1);
  assert(aliasReceivePriority("#r2-x", 0, true) == -1 && aliasReceivePriority("#x", 0, true) == 0);
  // With radio2 off the prefix is ignored.
  assert(aliasReceivePriority("#r2-x", 0, false) == 0);
}

int main() {
  testTiming();
  testAirtime();
  testBlob();
  testCrossTable();
  testCli();
  testTemp2();
  testSchedules();
  testTempradio();
  testMarks();
  testRoutes();
  testAlias();
  printf("mediamesh core: ok\n");
  return 0;
}
