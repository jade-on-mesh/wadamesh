// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaMesh: a second LoRa profile (`radio2`) on the same transceiver, used as a
// media-only side channel. Photos and voice clips from MeshCore Open TAK go out
// on the MediaMesh frequency; everything else stays on the main mesh. One radio
// time-shares the two profiles, so this is a scan, not a second receiver.
//
// This header is the portable part: settings, validation, the CLI shared with
// mikecarper/MeshCore's `radio2` commands, scan timing and the media-packet
// marker. No Arduino or radio code, so the host test builds it directly
// (test/test_mediamesh_core.cpp). The radio side is MediaMeshRadio.h.
//
// Scan timing follows mikecarper/MeshCore's RadioProfiles: listen 4.6 symbols on
// the slower profile, then fill the rest of half the slow preamble on the faster
// one, so a packet on either profile is still in its preamble when the radio
// comes back. The main mesh's preamble is fixed by stock nodes; MediaMesh senders
// lengthen their own preamble to cover the receiver's time on the main profile.

#pragma once

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace mediamesh {

enum Mode : uint8_t { MODE_OFF = 0, MODE_RX = 1, MODE_RXTX = 2 };
enum Route : uint8_t { ROUTE_RADIO = 0, ROUTE_RADIO2 = 1 };

struct Params {
  float freq = 0;          // MHz
  float bw = 0;            // kHz
  uint8_t sf = 0;
  uint8_t cr = 0;          // 5..8 = coding rate 4/5..4/8
  uint16_t preamble = 0;   // symbols; 0 = automatic
};

struct Config {
  uint8_t mode = MODE_OFF;       // radio2: off, rx (listen only) or rxtx
  uint8_t route = ROUTE_RADIO;   // tx.media: where photos and voice are sent
  Params p;
};

// Defaults for a new MediaMesh: same band as the US main mesh, 250 kHz SF7
// (about 4x faster than SF7/62.5 for ~6 dB less range).
static constexpr float kDefaultFreq = 916.000f;
static constexpr float kDefaultBw = 250.0f;
static constexpr uint8_t kDefaultSf = 7;
static constexpr uint8_t kDefaultCr = 5;

static constexpr double kListenSymbols = 4.6;       // minimum visit per profile
static constexpr uint8_t kAcquireSymbols = 16;      // fast-profile acquisition margin
static constexpr uint16_t kMinPreamble = 32;
static constexpr uint16_t kMaxAutoPreamble = 128;
static constexpr uint16_t kMaxPreamble = 1024;
static constexpr uint32_t kDefaultSwitchUs = 1500;  // until the first hops are measured
static constexpr uint32_t kMarkTtlMs = 10UL * 60UL * 1000UL;

inline void defaults(Config& c) {
  c.mode = MODE_OFF;
  c.route = ROUTE_RADIO;
  c.p.freq = kDefaultFreq;
  c.p.bw = kDefaultBw;
  c.p.sf = kDefaultSf;
  c.p.cr = kDefaultCr;
  c.p.preamble = 0;
}

inline bool validBw(float bw) {
  static const float bws[] = {7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f};
  for (float b : bws) if (fabsf(bw - b) < 0.01f) return true;
  return false;
}

inline bool validParams(const Params& p) {
  return isfinite(p.freq) && p.freq >= 150.0f && p.freq <= 960.0f && validBw(p.bw)
      && p.sf >= 5 && p.sf <= 12 && p.cr >= 5 && p.cr <= 8
      && (p.preamble == 0 || (p.preamble >= 8 && p.preamble <= kMaxPreamble));
}

inline bool sameParams(const Params& a, const Params& b) {
  return a.freq == b.freq && a.bw == b.bw && a.sf == b.sf && a.cr == b.cr && a.preamble == b.preamble;
}

inline bool sameChannel(const Params& a, const Params& b) {
  return fabsf(a.freq - b.freq) < 0.0005f && fabsf(a.bw - b.bw) < 0.01f && a.sf == b.sf;
}

// The radio scans both profiles whenever radio2 is on.
inline bool scanning(const Config& c) { return c.mode != MODE_OFF && validParams(c.p); }
// Photos and voice go out on radio2.
inline bool mediaOnRadio2(const Config& c) {
  return c.mode == MODE_RXTX && c.route == ROUTE_RADIO2 && validParams(c.p);
}

// ---- persisted form -------------------------------------------------------

static constexpr uint8_t kBlobVersion = 1;
static constexpr size_t kBlobSize = 18;

inline size_t encode(const Config& c, uint8_t out[kBlobSize]) {
  out[0] = 'M'; out[1] = 'M'; out[2] = kBlobVersion;
  out[3] = c.mode; out[4] = c.route; out[5] = c.p.sf; out[6] = c.p.cr; out[7] = 0;
  memcpy(&out[8], &c.p.freq, 4);
  memcpy(&out[12], &c.p.bw, 4);
  out[16] = (uint8_t)(c.p.preamble & 0xFF);
  out[17] = (uint8_t)(c.p.preamble >> 8);
  return kBlobSize;
}

inline bool decode(const uint8_t* in, size_t len, Config& c) {
  if (!in || len < kBlobSize || in[0] != 'M' || in[1] != 'M' || in[2] != kBlobVersion) return false;
  Config d;
  d.mode = in[3] <= MODE_RXTX ? in[3] : (uint8_t)MODE_OFF;
  d.route = in[4] <= ROUTE_RADIO2 ? in[4] : (uint8_t)ROUTE_RADIO;
  d.p.sf = in[5]; d.p.cr = in[6];
  memcpy(&d.p.freq, &in[8], 4);
  memcpy(&d.p.bw, &in[12], 4);
  d.p.preamble = (uint16_t)(in[16] | (in[17] << 8));
  if (!validParams(d.p)) return false;
  c = d;
  return true;
}

// ---- timing ---------------------------------------------------------------

inline double symbolUs(float bw_khz, uint8_t sf) {
  return bw_khz > 0 && sf <= 12 ? (double)(1UL << sf) * 1000.0 / bw_khz : 0;
}

inline uint16_t roundPreamble(double symbols) {
  if (symbols < kMinPreamble) symbols = kMinPreamble;
  double r = ceil(symbols / 8.0) * 8.0;
  return r > 65528 ? 65528 : (uint16_t)r;
}

// Semtech LoRa time on air in microseconds (explicit header, CRC on).
inline uint32_t timeOnAirUs(const Params& p, uint16_t preamble, int payload_len) {
  const double tsym = symbolUs(p.bw, p.sf);
  if (tsym <= 0) return 0;
  const int de = tsym >= 16000.0 ? 1 : 0;
  const double pre = (preamble + (p.sf <= 6 ? 6.25 : 4.25)) * tsym;
  const int num = 8 * payload_len - 4 * p.sf + (p.sf <= 6 ? 20 : 28) + 16;
  const int den = 4 * (p.sf - 2 * de);
  double symbols = den > 0 ? ceil((double)num / den) * p.cr : 0;
  if (symbols < 0) symbols = 0;
  return (uint32_t)(pre + (8 + symbols) * tsym);
}

struct Timing {
  bool valid = false;
  uint8_t slow = 0;                // profile with the longer symbol (0 = main)
  uint32_t dwell_us[2] = {};       // scheduled visit on each profile
  uint16_t preamble[2] = {};       // preamble each profile transmits with
  uint16_t need[2] = {};           // preamble a scanning receiver needs (rounded)
  uint32_t switch_us = 0;          // guarded per-retune allowance
  bool main_ok = true;             // stock main-mesh preamble is long enough
  bool media_capped = false;       // automatic radio2 preamble hit its cap
};

// `main_preamble` is what stock nodes on the main mesh send (MeshCore: 32 at
// SF<=8, 16 above). `switch_us` is the measured retune time with its guard.
inline Timing calcTiming(const Params& main, uint16_t main_preamble, const Params& media, uint32_t switch_us) {
  Timing t;
  const double T[2] = { symbolUs(main.bw, main.sf), symbolUs(media.bw, media.sf) };
  if (T[0] <= 0 || T[1] <= 0 || main_preamble == 0) return t;
  t.switch_us = switch_us;
  t.slow = T[0] >= T[1] ? 0 : 1;
  const uint8_t slow = t.slow, fast = slow ^ 1;
  const double overhead = 2.0 * switch_us;
  const uint32_t slow_dwell = (uint32_t)ceil(kListenSymbols * T[slow]);
  const uint32_t fast_min = (uint32_t)ceil(kListenSymbols * T[fast]);

  uint16_t pre[2] = { main_preamble, media.preamble };
  // A slow radio2 sets the cycle with its own (adjustable) preamble.
  if (slow == 1 && pre[1] == 0) {
    pre[1] = roundPreamble(2.0 * (slow_dwell + fast_min + overhead) / T[1]);
    if (pre[1] > kMaxAutoPreamble) { pre[1] = kMaxAutoPreamble; t.media_capped = true; }
  }
  const double half_slow = (double)pre[slow] * T[slow] / 2.0;
  const double fill = half_slow - slow_dwell - overhead;
  const uint32_t fast_dwell = fill > fast_min ? (uint32_t)floor(fill) : fast_min;
  t.dwell_us[slow] = slow_dwell;
  t.dwell_us[fast] = fast_dwell;

  const double cycle = (double)slow_dwell + fast_dwell + overhead;
  t.need[slow] = roundPreamble(2.0 * cycle / T[slow]);
  t.need[fast] = roundPreamble((slow_dwell + overhead) / T[fast] + kAcquireSymbols);

  if (fast == 1 && pre[1] == 0) {
    pre[1] = t.need[1];
    if (pre[1] > kMaxAutoPreamble) { pre[1] = kMaxAutoPreamble; t.media_capped = true; }
  }
  t.preamble[0] = pre[0];
  t.preamble[1] = pre[1];
  t.main_ok = t.need[0] <= pre[0];
  t.valid = true;
  return t;
}

// ---- media-packet marker ----------------------------------------------------
// The firmware marks each photo/voice packet it creates by its MeshCore packet
// hash; the radio looks the hash up when the packet reaches the transmitter.
// Retries keep the same hash, so they follow the original.

static constexpr int kHashSize = 8;   // MAX_HASH_SIZE

struct MarkRing {
  static constexpr int kSlots = 128;   // more than a full outbound queue of photo packets
  uint8_t hash[kSlots][kHashSize] = {};
  uint32_t at[kSlots] = {};
  bool used[kSlots] = {};
  uint8_t next = 0;

  void add(const uint8_t* h, uint32_t now_ms) {
    for (int i = 0; i < kSlots; ++i) {
      if (used[i] && memcmp(hash[i], h, kHashSize) == 0) { at[i] = now_ms; return; }
    }
    memcpy(hash[next], h, kHashSize);
    at[next] = now_ms;
    used[next] = true;
    next = (uint8_t)((next + 1) % kSlots);
  }
  bool has(const uint8_t* h, uint32_t now_ms) const {
    for (int i = 0; i < kSlots; ++i) {
      if (used[i] && now_ms - at[i] <= kMarkTtlMs && memcmp(hash[i], h, kHashSize) == 0) return true;
    }
    return false;
  }
  void clear() { memset(used, 0, sizeof(used)); next = 0; }
};

// ---- CLI ------------------------------------------------------------------
// Same command names as mikecarper/MeshCore so a client can drive either:
//   get radio2 | set radio2 off | set radio2 <MHz>,<kHz>,<sf>,<cr>,rx|rxtx[,preamble]
//   get tx.media | set tx.media radio|radio2
//   get radio2.cross | set radio2.cross auto|on|off   (accepted; nothing is forwarded)
//   get radio2.timing | get mediamesh

inline const char* modeName(uint8_t m) { return m == MODE_RXTX ? "rxtx" : m == MODE_RX ? "rx" : "off"; }
inline const char* routeName(uint8_t r) { return r == ROUTE_RADIO2 ? "radio2" : "radio"; }

inline void formatParams(const Config& c, char* out, size_t cap) {
  if (c.mode == MODE_OFF) { snprintf(out, cap, "off"); return; }
  if (c.p.preamble) {
    snprintf(out, cap, "%.3f,%.2f,%u,%u,%s,%u", c.p.freq, c.p.bw, c.p.sf, c.p.cr, modeName(c.mode), c.p.preamble);
  } else {
    snprintf(out, cap, "%.3f,%.2f,%u,%u,%s", c.p.freq, c.p.bw, c.p.sf, c.p.cr, modeName(c.mode));
  }
}

inline bool parseMode(const char* s, uint8_t& mode) {
  if (!strcmp(s, "rx")) { mode = MODE_RX; return true; }
  if (!strcmp(s, "rxtx") || !strcmp(s, "rx&tx")) { mode = MODE_RXTX; return true; }
  if (!strcmp(s, "off")) { mode = MODE_OFF; return true; }
  return false;
}

// Parses "<MHz>,<kHz>,<sf>,<cr>,<mode>[,<preamble>|auto]" into `c`.
inline bool parseRadio2(const char* text, Config& c, const char** why) {
  char buf[96];
  size_t n = strlen(text);
  if (n >= sizeof(buf)) { *why = "too long"; return false; }
  memcpy(buf, text, n + 1);
  char* fields[7] = {};
  int count = 0;
  for (char* tok = strtok(buf, ","); tok && count < 7; tok = strtok(nullptr, ",")) {
    while (*tok == ' ') ++tok;
    char* end = tok + strlen(tok);
    while (end > tok && end[-1] == ' ') *--end = 0;
    fields[count++] = tok;
  }
  if (count < 5 || count > 6) { *why = "use <MHz>,<kHz>,<sf>,<cr>,rx|rxtx[,preamble]"; return false; }
  Config d = c;
  char* e;
  d.p.freq = strtof(fields[0], &e); if (*e) { *why = "bad frequency"; return false; }
  d.p.bw = strtof(fields[1], &e);   if (*e) { *why = "bad bandwidth"; return false; }
  long sf = strtol(fields[2], &e, 10); if (*e) { *why = "bad SF"; return false; }
  long cr = strtol(fields[3], &e, 10); if (*e) { *why = "bad CR"; return false; }
  d.p.sf = (uint8_t)(sf < 0 || sf > 255 ? 0 : sf);
  d.p.cr = (uint8_t)(cr < 0 || cr > 255 ? 0 : cr);
  if (!parseMode(fields[4], d.mode) || d.mode == MODE_OFF) { *why = "mode must be rx or rxtx"; return false; }
  d.p.preamble = 0;
  if (count == 6 && strcmp(fields[5], "auto") != 0) {
    long pre = strtol(fields[5], &e, 10);
    if (*e || pre < 0 || pre > kMaxPreamble) { *why = "bad preamble"; return false; }
    d.p.preamble = (uint16_t)pre;
  }
  if (!validParams(d.p)) { *why = "invalid radio settings"; return false; }
  c = d;
  return true;
}

struct CliResult {
  bool handled = false;
  bool changed = false;
};

// `main` is the primary radio; `timing` the current scan timing (may be invalid
// when radio2 is off). `supported` is false on builds without MediaMesh radio.
inline CliResult handleCli(const char* cmd, Config& c, const Params& main, const Timing& timing,
                           bool supported, char* reply, size_t cap) {
  CliResult r;
  while (*cmd == ' ') ++cmd;
  const bool is_get = !strncmp(cmd, "get ", 4);
  const bool is_set = !strncmp(cmd, "set ", 4);
  if (!is_get && !is_set) return r;
  const char* key = cmd + 4;
  while (*key == ' ') ++key;
  char name[24];
  size_t k = 0;
  while (key[k] && key[k] != ' ' && k + 1 < sizeof(name)) { name[k] = key[k]; ++k; }
  name[k] = 0;
  const char* arg = key + k;
  while (*arg == ' ') ++arg;

  const bool radio2 = !strcmp(name, "radio2");
  const bool media = !strcmp(name, "tx.media");
  const bool cross = !strcmp(name, "radio2.cross");
  const bool timing_key = !strcmp(name, "radio2.timing");
  const bool summary = !strcmp(name, "mediamesh");
  if (!radio2 && !media && !cross && !timing_key && !summary) return r;
  r.handled = true;

  if (is_get) {
    char p[64];
    if (radio2) {
      formatParams(c, p, sizeof p);
      snprintf(reply, cap, "> %s", p);
    } else if (media) {
      snprintf(reply, cap, "> %s", routeName(c.route));
    } else if (cross) {
      snprintf(reply, cap, "> off");
    } else if (timing_key) {
      if (!scanning(c) || !timing.valid) snprintf(reply, cap, "> radio2 off");
      else snprintf(reply, cap, "> dwell=%.1f,%.1fms; preamble=%u,%u; need=%u,%u; switch=%luus%s%s",
                    timing.dwell_us[0] / 1000.0, timing.dwell_us[1] / 1000.0,
                    timing.preamble[0], timing.preamble[1], timing.need[0], timing.need[1],
                    (unsigned long)timing.switch_us,
                    timing.main_ok ? "" : "; WARN main preamble too short for scanning",
                    timing.media_capped ? "; WARN radio2 preamble capped" : "");
    } else {
      formatParams(c, p, sizeof p);
      snprintf(reply, cap, "> radio2=%s; tx.media=%s; %s", p, routeName(c.route),
               !supported ? "unsupported on this radio" : mediaOnRadio2(c) ? "media on radio2" : "media on radio");
    }
    return r;
  }

  if (summary || timing_key) { snprintf(reply, cap, "Error: %s is read-only", name); return r; }
  if (cross) {
    if (strcmp(arg, "auto") && strcmp(arg, "on") && strcmp(arg, "off")) {
      snprintf(reply, cap, "Error: use set radio2.cross auto|on|off");
    } else {
      snprintf(reply, cap, "OK - companion does not forward between profiles");
    }
    return r;
  }
  if (media) {
    if (!strcmp(arg, "radio") || !strcmp(arg, "radio2")) {
      const uint8_t route = !strcmp(arg, "radio2") ? ROUTE_RADIO2 : ROUTE_RADIO;
      r.changed = route != c.route;
      c.route = route;
      snprintf(reply, cap, "OK - tx.media %s%s", routeName(route),
               route == ROUTE_RADIO2 && c.mode != MODE_RXTX ? "; radio2 must be rxtx to send" : "");
    } else {
      snprintf(reply, cap, "Error: use set tx.media radio|radio2");
    }
    return r;
  }
  // set radio2
  if (!supported) { snprintf(reply, cap, "Error: radio2 is not supported on this radio"); return r; }
  if (!strcmp(arg, "off")) {
    r.changed = c.mode != MODE_OFF;
    c.mode = MODE_OFF;
    snprintf(reply, cap, "OK - radio2 off");
    return r;
  }
  Config d = c;
  const char* why = "";
  if (!parseRadio2(arg, d, &why)) { snprintf(reply, cap, "Error: %s", why); return r; }
  if (sameChannel(d.p, main)) { snprintf(reply, cap, "Error: radio2 matches the main radio"); return r; }
  r.changed = !sameParams(d.p, c.p) || d.mode != c.mode;
  c = d;
  char p[64];
  formatParams(c, p, sizeof p);
  snprintf(reply, cap, "OK - radio2 %s", p);
  return r;
}

}  // namespace mediamesh
