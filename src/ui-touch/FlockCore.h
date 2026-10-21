// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Flock You: passive detection of Flock Safety ALPR cameras (and their Raven
// gunshot sensors) from the Wi-Fi and BLE they emit, plus a DeFlock-style layer
// of known cameras from OpenStreetMap. This header is the pure logic: the
// signatures, the detection table, the camera database, the channel alert text
// and its rate limit. No Arduino, no radio, no LVGL: host-tested in
// test/test_flock_core.cpp. The radio glue is FlockRadio.cpp, the screens
// FlockYouUI.inc.
//
// Signatures adapted from colonelpanichacks/flock-you (MIT) and
// wgreenberg/flock-you; the Wadamesh T-Deck port by Chase H. Barnes.
//
// Every detection is a heuristic: the Wi-Fi modules and BLE chips Flock uses
// are sold to other vendors too. Each rule therefore carries a confidence, the
// map shows all of them, and only the confident ones go out on the mesh.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace flockyou {

// ---- small helpers ------------------------------------------------------------------------
inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
inline bool startsNoCase(const char* s, const char* w) {
  for (; *w; ++s, ++w)
    if (!*s || lower(*s) != lower(*w)) return false;
  return true;
}
inline bool containsNoCase(const char* s, const char* w) {
  if (!s || !w) return false;
  for (; *s; ++s)
    if (startsNoCase(s, w)) return true;
  return false;
}
inline void copyStr(char* out, size_t cap, const char* s, size_t n) {
  if (!cap) return;
  if (n >= cap) n = cap - 1;
  memcpy(out, s, n);
  out[n] = 0;
}

// Great-circle distance in metres and initial bearing in degrees.
inline double distanceM(double la1, double lo1, double la2, double lo2) {
  const double r = 6371000.0, d2r = M_PI / 180.0;
  const double dla = (la2 - la1) * d2r, dlo = (lo2 - lo1) * d2r;
  const double a = sin(dla / 2) * sin(dla / 2) + cos(la1 * d2r) * cos(la2 * d2r) * sin(dlo / 2) * sin(dlo / 2);
  return 2 * r * atan2(sqrt(a), sqrt(1 - a));
}
inline double bearingDeg(double la1, double lo1, double la2, double lo2) {
  const double d2r = M_PI / 180.0;
  const double y = sin((lo2 - lo1) * d2r) * cos(la2 * d2r);
  const double x = cos(la1 * d2r) * sin(la2 * d2r) - sin(la1 * d2r) * cos(la2 * d2r) * cos((lo2 - lo1) * d2r);
  const double b = atan2(y, x) / d2r;
  return b < 0 ? b + 360 : b;
}

// ---- what was heard -----------------------------------------------------------------------
enum class Source : uint8_t { Wifi = 0, Ble = 1 };

// Which rule matched. Stored by name in the CSV log (methodName / methodFromName).
enum class Method : uint8_t {
  None = 0,
  WifiProbe,     // the Flock camera's probe request: its IE sequence from a Flock OUI
  BleRaven,      // Raven: two or more of its own GATT services advertised
  BleRavenOne,   // one Raven service
  BleName,       // a Flock device name ("FS Ext Battery", "Pigvision")
  BleNameWeak,   // a name that only hints ("Penguin", "Flock...")
  BleMfr,        // XUNTONG manufacturer id (0x09C8)
  BleMac,        // a public address from a Flock module OUI
};

enum Conf : uint8_t { kConfLow = 1, kConfMed = 2, kConfHigh = 3 };

inline uint8_t confidence(Method m) {
  switch (m) {
    case Method::WifiProbe: case Method::BleRaven: case Method::BleName: return kConfHigh;
    case Method::BleRavenOne: case Method::BleNameWeak: case Method::BleMfr: return kConfMed;
    case Method::BleMac: return kConfLow;
    default: return 0;
  }
}

inline const char* methodName(Method m) {
  switch (m) {
    case Method::WifiProbe: return "wifi_probe_ie";
    case Method::BleRaven: return "raven_uuid";
    case Method::BleRavenOne: return "raven_uuid1";
    case Method::BleName: return "ble_name";
    case Method::BleNameWeak: return "ble_name_weak";
    case Method::BleMfr: return "ble_mfr_xuntong";
    case Method::BleMac: return "ble_mac";
    default: return "none";
  }
}
inline Method methodFromName(const char* s) {
  for (uint8_t i = 1; i <= (uint8_t)Method::BleMac; i++)
    if (s && !strcmp(s, methodName((Method)i))) return (Method)i;   // 1.2's names are the same
  return Method::None;
}
// What a person reads: "Wi-Fi probe", "Raven sensor"...
inline const char* methodLabel(Method m) {
  switch (m) {
    case Method::WifiProbe: return "Wi-Fi probe";
    case Method::BleRaven: return "Raven sensor";
    case Method::BleRavenOne: return "Raven-like BLE";
    case Method::BleName: return "BLE name";
    case Method::BleNameWeak: return "BLE name (weak)";
    case Method::BleMfr: return "BLE maker id";
    case Method::BleMac: return "BLE address";
    default: return "?";
  }
}

struct Alert {
  Source source = Source::Wifi;
  Method method = Method::None;
  char mac[18] = {0};    // "aa:bb:cc:dd:ee:ff", lower case
  char name[33] = {0};   // BLE local name, "" for Wi-Fi
  int16_t rssi = 0;
  uint8_t channel = 0;   // Wi-Fi channel, 0 for BLE
};

inline void macToStr(const uint8_t* m, char* out, size_t cap) {
  snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// ---- Wi-Fi ---------------------------------------------------------------------------------
// OUIs of the Wi-Fi modules seen in Flock cameras. 82:6b:f2 from the 1.2 list
// is gone: it is a locally administered prefix, which the check below rejects
// before it is ever compared.
static const uint8_t kWifiOuis[][3] = {
  {0x70,0xc9,0x4e},{0x3c,0x91,0x80},{0xd8,0xf3,0xbc},{0x80,0x30,0x49},
  {0xb8,0x35,0x32},{0x14,0x5a,0xfc},{0x74,0x4c,0xa1},{0x08,0x3a,0x88},
  {0x9c,0x2f,0x9d},{0xc0,0x35,0x32},{0x94,0x08,0x53},{0xe4,0xaa,0xea},
  {0xf4,0x6a,0xdd},{0xf8,0xa2,0xd6},{0x24,0xb2,0xb9},{0x00,0xf4,0x8d},
  {0xd0,0x39,0x57},{0xe8,0xd0,0xfc},{0xe0,0x4f,0x43},{0xb8,0x1e,0xa4},
  {0x70,0x08,0x94},{0x58,0x8e,0x81},{0xec,0x1b,0xbd},{0x3c,0x71,0xbf},
  {0x58,0x00,0xe3},{0x90,0x35,0xea},{0x5c,0x93,0xa2},{0x64,0x6e,0x69},
  {0x48,0x27,0xea},{0xa4,0xcf,0x12},
};

// A globally unique unicast address from one of those modules.
inline bool wifiOui(const uint8_t* mac) {
  if (!mac || (mac[0] & 0x03)) return false;   // multicast or locally administered
  for (const auto& o : kWifiOuis)
    if (!memcmp(mac, o, 3)) return true;
  return false;
}

// The probe request's tagged parameters, in order: wildcard SSID, supported
// rates (2), then 12, then extended capabilities (127), a Wi-Fi Alliance
// vendor IE (50:6f:9a:16 03 01 03), then Microsoft WPS (00:50:f2:08 00 00 00).
inline bool probeSignature(const uint8_t* body, int length) {
  if (!body || length < 2 || body[0] != 0 || body[1] != 0) return false;
  bool t2 = false, t12 = false, t127 = false, wfa = false, ms = false;
  int off = 0;
  while (off + 2 <= length) {
    const uint8_t tag = body[off];
    const int size = body[off + 1];
    if (off + 2 + size > length) break;
    const uint8_t* v = body + off + 2;
    if (tag == 2) t2 = true;
    else if (tag == 12 && t2) t12 = true;
    else if (tag == 127 && t12) t127 = true;
    else if (tag == 221 && size >= 7 && t127 && !memcmp(v, "\x50\x6f\x9a\x16\x03\x01\x03", 7)) wfa = true;
    else if (tag == 221 && size >= 7 && wfa && !memcmp(v, "\x00\x50\xf2\x08\x00\x00\x00", 7)) ms = true;
    off += size + 2;
  }
  return t2 && t12 && t127 && wfa && ms;
}

// A whole 802.11 frame as the promiscuous callback hands it over (it may end in
// the 4-byte FCS). True for a Flock camera's probe request.
inline bool parseWifiFrame(const uint8_t* frame, int len, int rssi, uint8_t channel, Alert& out) {
  constexpr int kHdr = 24;   // fc, duration, addr1..3, seq
  if (!frame || len < kHdr + 2) return false;
  const uint8_t fc = frame[0];
  if (((fc >> 2) & 3) != 0 || ((fc >> 4) & 15) != 4) return false;   // management / probe request
  const uint8_t* src = frame + 10;                                    // addr2: the transmitter
  if (!wifiOui(src)) return false;
  const uint8_t* body = frame + kHdr;
  const int blen = len - kHdr;
  if (!probeSignature(body, blen) && !(blen > 4 && probeSignature(body, blen - 4))) return false;
  out = Alert();
  out.source = Source::Wifi;
  out.method = Method::WifiProbe;
  macToStr(src, out.mac, sizeof out.mac);
  out.rssi = (int16_t)rssi;
  out.channel = channel;
  return true;
}

// ---- BLE -----------------------------------------------------------------------------------
static const uint8_t kBleOuis[][3] = {
  {0x58,0x8e,0x81},{0xec,0x1b,0xbd},{0x90,0x35,0xea},{0x04,0x0d,0x84},{0xf0,0x82,0xc0},
  {0x1c,0x34,0xf1},{0x38,0x5b,0x44},{0x94,0x34,0x69},{0xb4,0xe3,0xf9},{0x70,0xc9,0x4e},
  {0x3c,0x91,0x80},{0xd8,0xf3,0xbc},{0x80,0x30,0x49},{0x14,0x5a,0xfc},{0x74,0x4c,0xa1},
  {0x08,0x3a,0x88},{0x9c,0x2f,0x9d},{0x94,0x08,0x53},{0xe4,0xaa,0xea},{0xb4,0x1e,0x52},
  {0xd4,0x11,0xd6},
};   // cc:cc:cc from the 1.2 list is gone: placeholder firmware uses it everywhere

// Raven's own GATT services (GPS, power, network, upload, error).
static const uint16_t kRavenServices[] = {0x3100, 0x3200, 0x3300, 0x3400, 0x3500};

// What one advertisement says, from its raw AD structures.
struct BleAdv {
  char name[33] = {0};
  uint16_t mfr_id = 0;
  bool has_mfr = false;
  uint16_t uuid16[12] = {0};
  int n16 = 0;
};

// 0000xxxx-0000-1000-8000-00805f9b34fb, little-endian as advertised.
inline bool uuid128To16(const uint8_t* u, uint16_t* out) {
  static const uint8_t base[12] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00};
  if (memcmp(u, base, 12) || u[14] || u[15]) return false;
  *out = (uint16_t)(u[12] | (u[13] << 8));
  return true;
}

inline void parseAdv(const uint8_t* p, size_t len, BleAdv& a) {
  a = BleAdv();
  size_t i = 0;
  while (p && i + 1 < len) {
    const uint8_t l = p[i];
    if (!l || i + 1 + l > len) break;
    const uint8_t type = p[i + 1];
    const uint8_t* d = p + i + 2;
    const size_t dl = l - 1;
    if ((type == 0x08 || type == 0x09) && (type == 0x09 || !a.name[0])) {
      copyStr(a.name, sizeof a.name, (const char*)d, dl);
    } else if (type == 0xFF && dl >= 2 && !a.has_mfr) {
      a.has_mfr = true;
      a.mfr_id = (uint16_t)(d[0] | (d[1] << 8));
    } else if (type == 0x02 || type == 0x03) {
      for (size_t k = 0; k + 1 < dl && a.n16 < 12; k += 2) a.uuid16[a.n16++] = (uint16_t)(d[k] | (d[k + 1] << 8));
    } else if (type == 0x06 || type == 0x07) {
      for (size_t k = 0; k + 15 < dl && a.n16 < 12; k += 16) {
        uint16_t s;
        if (uuid128To16(d + k, &s)) a.uuid16[a.n16++] = s;
      }
    }
    i += 1 + l;
  }
}

// The strongest rule an advertisement matches, Method::None for none.
// `addr` is the address as NimBLE prints it; `public_addr` false for random
// addresses, whose first bytes are no vendor's.
inline Method classifyBle(const uint8_t addr[6], bool public_addr, const BleAdv& a) {
  int raven = 0;
  for (int i = 0; i < a.n16; i++)
    for (uint16_t s : kRavenServices)
      if (a.uuid16[i] == s) raven++;
  if (raven >= 2) return Method::BleRaven;
  if (a.name[0]) {
    if (startsNoCase(a.name, "FS Ext Battery") || containsNoCase(a.name, "pigvision")) return Method::BleName;
  }
  if (raven == 1) return Method::BleRavenOne;
  if (a.has_mfr && a.mfr_id == 0x09C8) return Method::BleMfr;
  if (a.name[0] && (startsNoCase(a.name, "penguin") || startsNoCase(a.name, "flock"))) return Method::BleNameWeak;
  if (public_addr && addr) {
    for (const auto& o : kBleOuis)
      if (addr[0] == o[0] && addr[1] == o[1] && addr[2] == o[2]) return Method::BleMac;
  }
  return Method::None;
}

// ---- the detection table -------------------------------------------------------------------
struct Detection {
  Alert alert;
  uint32_t first_ms = 0, last_ms = 0;   // millis() this boot; 0 for a loaded log row
  uint32_t epoch = 0;                    // wall time it was last heard, 0 unknown
  uint16_t count = 0;
  bool has_gps = false;
  bool historical = false;               // from the log, not heard this session
  bool sent = false;                     // already posted to the alert channel
  double lat = 0, lon = 0;
};

// Fixed capacity; when full a new device takes the place of the oldest
// historical row, else the device heard longest ago. It never stops recording.
template <int N>
class Table {
 public:
  Detection rows[N];
  int n = 0;

  int find(const Alert& a) const {
    for (int i = 0; i < n; i++)
      if (rows[i].alert.source == a.source && !strcmp(rows[i].alert.mac, a.mac)) return i;
    return -1;
  }

  // Heard now. *is_new: a device not in the table before.
  Detection& hear(const Alert& a, uint32_t now_ms, uint32_t epoch, bool* is_new) {
    int i = find(a);
    *is_new = i < 0;
    if (i < 0) {
      i = slotForNew();
      rows[i] = Detection();
      rows[i].alert = a;
      rows[i].first_ms = now_ms;
    } else {
      Detection& d = rows[i];
      // keep the name a later advertisement dropped, and the strongest rule
      if (!a.name[0] && d.alert.name[0]) {
        char keep[sizeof d.alert.name];
        memcpy(keep, d.alert.name, sizeof keep);
        const Method m = confidence(a.method) >= confidence(d.alert.method) ? a.method : d.alert.method;
        d.alert = a;
        memcpy(d.alert.name, keep, sizeof keep);
        d.alert.method = m;
      } else {
        const Method m = confidence(a.method) >= confidence(d.alert.method) ? a.method : d.alert.method;
        d.alert = a;
        d.alert.method = m;
      }
      if (d.historical) { d.historical = false; d.first_ms = now_ms; d.count = 0; d.sent = false; }
    }
    Detection& d = rows[i];
    d.last_ms = now_ms;
    if (epoch) d.epoch = epoch;
    if (d.count < 0xFFFF) d.count++;
    return d;
  }

  // A row from the log. Rows come oldest first, so the table ends up holding the newest.
  void load(const Detection& r) {
    int i = find(r.alert);
    if (i < 0) i = slotForNew();
    rows[i] = r;
    rows[i].historical = true;
  }

  // Drops this session's rows (the log is untouched).
  void clearLive() {
    int k = 0;
    for (int i = 0; i < n; i++)
      if (rows[i].historical) rows[k++] = rows[i];
    n = k;
  }

  int liveCount() const {
    int c = 0;
    for (int i = 0; i < n; i++) c += rows[i].historical ? 0 : 1;
    return c;
  }

 private:
  int slotForNew() {
    if (n < N) return n++;
    int best = -1;
    for (int i = 0; i < n; i++)   // the oldest historical row first
      if (rows[i].historical && (best < 0 || rows[i].epoch < rows[best].epoch)) best = i;
    if (best < 0)
      for (int i = 0; i < n; i++)
        if (best < 0 || (int32_t)(rows[i].last_ms - rows[best].last_ms) < 0) best = i;
    // keep the array in hearing order: close the gap, reuse the last slot
    for (int i = best; i + 1 < n; i++) rows[i] = rows[i + 1];
    return n - 1;
  }
};

// ---- the CSV log ---------------------------------------------------------------------------
// epoch,source,mac,name,method,rssi,channel,latitude,longitude
// (1.2 wrote uptime_ms in the first column; those rows load with epoch 0.)
constexpr const char* kLogHeader = "epoch,source,mac,name,method,rssi,channel,latitude,longitude";

inline size_t formatLogRow(const Detection& d, char* out, size_t cap) {
  char name[sizeof d.alert.name];
  snprintf(name, sizeof name, "%s", d.alert.name);
  for (char* p = name; *p; ++p)
    if (*p == ',' || *p == '\r' || *p == '\n' || *p == '"') *p = '_';
  char pos[32] = ",";
  if (d.has_gps) snprintf(pos, sizeof pos, "%.6f,%.6f", d.lat, d.lon);
  const int n = snprintf(out, cap, "%lu,%s,%s,%s,%s,%d,%u,%s", (unsigned long)d.epoch,
                         d.alert.source == Source::Wifi ? "wifi" : "ble", d.alert.mac, name,
                         methodName(d.alert.method), d.alert.rssi, d.alert.channel, pos);
  return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

// One log line into a row; false for the header, a comment or a broken line.
inline bool parseLogRow(const char* line, Detection& d) {
  char row[200];
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) n--;
  if (!n || n >= sizeof row || line[0] == '#' || startsNoCase(line, "epoch") || startsNoCase(line, "uptime")) return false;
  memcpy(row, line, n);
  row[n] = 0;
  char* f[9] = {0};
  char* c = row;
  for (int k = 0; k < 9; k++) {
    f[k] = c;
    if (k == 8) break;
    char* comma = strchr(c, ',');
    if (!comma) return false;
    *comma = 0;
    c = comma + 1;
  }
  if (!f[1][0] || strlen(f[2]) != 17 || !f[4][0]) return false;
  d = Detection();
  d.alert.source = !strcmp(f[1], "wifi") ? Source::Wifi : Source::Ble;
  copyStr(d.alert.mac, sizeof d.alert.mac, f[2], strlen(f[2]));
  for (char* p = d.alert.mac; *p; ++p) *p = lower(*p);
  copyStr(d.alert.name, sizeof d.alert.name, f[3], strlen(f[3]));
  d.alert.method = methodFromName(f[4]);
  if (d.alert.method == Method::None)
    d.alert.method = d.alert.source == Source::Wifi ? Method::WifiProbe : Method::BleMac;
  d.alert.rssi = (int16_t)atoi(f[5]);
  d.alert.channel = (uint8_t)atoi(f[6]);
  const unsigned long first = strtoul(f[0], nullptr, 10);
  d.epoch = first > 1700000000UL ? (uint32_t)first : 0;   // 1.2's uptime_ms is not a date
  d.count = 1;
  if (f[7][0] && f[8][0]) {
    d.lat = strtod(f[7], nullptr);
    d.lon = strtod(f[8], nullptr);
    d.has_gps = (d.lat != 0.0 || d.lon != 0.0) && fabs(d.lat) <= 90 && fabs(d.lon) <= 180;
  }
  return true;
}

// ---- known cameras (DeFlock / OpenStreetMap) --------------------------------------------------
// cameras.csv, one camera a line:  lat,lon[,direction[,label]]
// ("#" comments and a "lat,lon..." header are skipped). scripts/flock_cameras.py
// writes it from OpenStreetMap's ALPR nodes; marked.csv holds the ones marked
// on this radio, same format.
struct Cam {
  int32_t lat_e6 = 0, lon_e6 = 0;
  int16_t dir = -1;    // degrees the camera faces, -1 unknown
  uint8_t flags = 0;   // kCamMine: marked on this radio
};
constexpr uint8_t kCamMine = 1;

inline bool parseCamLine(const char* line, Cam& c) {
  while (*line == ' ' || *line == '\t') line++;
  if (!*line || *line == '#' || lower(*line) == 'l') return false;
  char* e;
  const double la = strtod(line, &e);
  if (e == line || *e != ',') return false;
  const char* p = e + 1;
  const double lo = strtod(p, &e);
  if (e == p || fabs(la) > 90 || fabs(lo) > 180 || (la == 0 && lo == 0)) return false;
  c = Cam();
  c.lat_e6 = (int32_t)lround(la * 1e6);
  c.lon_e6 = (int32_t)lround(lo * 1e6);
  if (*e == ',') {
    p = e + 1;
    const long d = strtol(p, &e, 10);
    if (e != p && d >= 0 && d < 360) c.dir = (int16_t)d;
  }
  return true;
}

inline size_t formatCamLine(const Cam& c, const char* label, char* out, size_t cap) {
  int n;
  if (c.dir >= 0) n = snprintf(out, cap, "%.6f,%.6f,%d,%s", c.lat_e6 / 1e6, c.lon_e6 / 1e6, c.dir, label ? label : "");
  else n = snprintf(out, cap, "%.6f,%.6f,,%s", c.lat_e6 / 1e6, c.lon_e6 / 1e6, label ? label : "");
  return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

// ---- downloading cameras on the radio ---------------------------------------------------------
// The same cameras scripts/flock_cameras.py fetches, asked of OpenStreetMap's
// Overpass API for a box around the radio, as CSV (far easier to stream than
// JSON) with '|' between fields:
//   lat|lon|direction|camera:direction|manufacturer|brand|operator
// Each row becomes a cameras.csv line, chosen the way the script chooses.
constexpr const char* kOverpassUrl = "https://overpass-api.de/api/interpreter";
constexpr int kAreaChoices = 4;
static const uint16_t kAreaKm[kAreaChoices] = {25, 50, 100, 200};   // half the box's side
constexpr uint32_t kDownloadMaxAgeS = 7UL * 86400;                    // refresh after a week

struct Box { double s, w, n, e; };

inline Box areaBox(double lat, double lon, int km) {
  const double dla = km / 111.32;
  const double cl = cos(lat * M_PI / 180.0);
  const double dlo = km / (111.32 * (cl > 0.05 ? cl : 0.05));
  Box b{lat - dla, lon - dlo, lat + dla, lon + dlo};
  if (b.s < -90) b.s = -90;
  if (b.n > 90) b.n = 90;
  if (b.w < -180) b.w = -180;
  if (b.e > 180) b.e = 180;
  return b;
}

inline size_t overpassQuery(const Box& b, char* out, size_t cap) {
  const int n = snprintf(out, cap,
                         "[out:csv(::lat,::lon,direction,\"camera:direction\",manufacturer,brand,operator;false;\"|\")]"
                         "[timeout:120];node[\"man_made\"=\"surveillance\"][\"surveillance:type\"=\"ALPR\"]"
                         "(%.4f,%.4f,%.4f,%.4f);out body;",
                         b.s, b.w, b.n, b.e);
  return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}

// "<kOverpassUrl>?data=<query, URL-encoded>". 0 when it doesn't fit.
inline size_t overpassUrl(const Box& b, char* out, size_t cap) {
  char q[400];
  if (!overpassQuery(b, q, sizeof q)) return 0;
  int o = snprintf(out, cap, "%s?data=", kOverpassUrl);
  if (o < 0 || (size_t)o >= cap) return 0;
  static const char* const hex = "0123456789ABCDEF";
  for (const char* p = q; *p; p++) {
    const unsigned char c = (unsigned char)*p;
    const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                       c == '_' || c == '.' || c == '~';
    if ((size_t)o + (plain ? 1 : 3) >= cap) return 0;
    if (plain) out[o++] = (char)c;
    else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
  }
  out[o] = 0;
  return (size_t)o;
}

// Degrees a direction tag means, or -1: "270", "W", "45-90" (its middle),
// "90;270" (the first). As flock_cameras.py's direction().
inline int directionFromTag(const char* v) {
  char t[24];
  size_t n = 0;
  while (*v == ' ') v++;
  for (; *v && *v != ';' && n + 1 < sizeof t; v++) t[n++] = *v;
  while (n && t[n - 1] == ' ') n--;
  t[n] = 0;
  if (!n) return -1;
  static const char* const card[16] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                       "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
  static const int16_t deg[16] = {0, 22, 45, 67, 90, 112, 135, 157, 180, 202, 225, 247, 270, 292, 315, 337};
  char up[24];
  for (size_t i = 0; i <= n; i++) up[i] = (t[i] >= 'a' && t[i] <= 'z') ? (char)(t[i] - 32) : t[i];
  for (int i = 0; i < 16; i++)
    if (!strcmp(up, card[i])) return deg[i];
  char* e;
  const double a = strtod(t, &e);
  if (e == t || a != a) return -1;
  const char* r = e;
  while (*r == ' ') r++;
  if (*r == '-' && t[0] != '-') {   // a range
    const char* bs = r + 1;
    while (*bs == ' ') bs++;
    char* e2;
    double b = strtod(bs, &e2);
    if (e2 == bs || *e2) return -1;
    if (b < a) b += 360;
    return (int)lround((a + b) / 2) % 360;
  }
  if (*r) return -1;
  int d = (int)lround(a) % 360;
  return d < 0 ? d + 360 : d;
}

// One Overpass CSV row as a cameras.csv line. False for anything else (a
// blank line, an error message).
inline bool osmRowToCamLine(const char* row, char* out, size_t cap) {
  // Up to 7 fields; a row with fewer (no tags at all) leaves the rest empty.
  const char* f[7];
  size_t len[7];
  const char* p = row;
  for (int i = 0; i < 7; i++) {
    if (!p) { f[i] = ""; len[i] = 0; continue; }
    const char* bar = i < 6 ? strchr(p, '|') : nullptr;
    f[i] = p;
    len[i] = bar ? (size_t)(bar - p) : strlen(p);
    p = bar ? bar + 1 : nullptr;
  }
  char num[32];
  copyStr(num, sizeof num, f[0], len[0]);
  char* e;
  const double la = strtod(num, &e);
  if (e == num || *e) return false;
  copyStr(num, sizeof num, f[1], len[1]);
  const double lo = strtod(num, &e);
  if (e == num || *e || fabs(la) > 90 || fabs(lo) > 180 || (la == 0 && lo == 0)) return false;
  Cam c;
  c.lat_e6 = (int32_t)lround(la * 1e6);
  c.lon_e6 = (int32_t)lround(lo * 1e6);
  char tag[48];
  for (int k = 2; k <= 3 && c.dir < 0; k++) {
    copyStr(tag, sizeof tag, f[k], len[k]);
    const int d = directionFromTag(tag);
    if (d >= 0) c.dir = (int16_t)d;
  }
  char label[48] = "ALPR";
  for (int k = 4; k <= 6; k++) {
    if (!len[k]) continue;
    copyStr(tag, sizeof tag, f[k], len[k]);
    size_t o = 0;
    for (const char* q = tag; *q; q++) label[o++] = (*q == ',' || *q == '\r' || *q == '\n') ? ' ' : *q;
    label[o] = 0;
    break;
  }
  // Trim, then cut to 32 bytes without splitting a character.
  char* l = label;
  while (*l == ' ') l++;
  size_t ln = strlen(l);
  while (ln && l[ln - 1] == ' ') ln--;
  if (ln > 32) {
    ln = 32;
    while (ln && ((uint8_t)l[ln] & 0xC0) == 0x80) ln--;
    while (ln && l[ln - 1] == ' ') ln--;
  }
  l[ln] = 0;
  return formatCamLine(c, ln ? l : "ALPR", out, cap) > 0;
}

// The cameras, in a buffer the caller owns (PSRAM on the radio).
class CamDb {
 public:
  void attach(Cam* buf, int cap) { buf_ = buf; cap_ = cap; n_ = 0; }
  int size() const { return n_; }
  int capacity() const { return cap_; }
  const Cam& at(int i) const { return buf_[i]; }
  bool full() const { return n_ >= cap_; }
  void clear() { n_ = 0; }

  bool add(const Cam& c) {
    if (!buf_ || n_ >= cap_) return false;
    buf_[n_++] = c;
    return true;
  }

  // Index of the nearest camera within max_m, or -1. Cheap enough to run over
  // tens of thousands every couple of seconds: a degree box first, then the
  // real distance for what is inside it.
  int nearest(double lat, double lon, double max_m, double* dist_m) const {
    const int32_t la6 = (int32_t)lround(lat * 1e6), lo6 = (int32_t)lround(lon * 1e6);
    const int32_t dla = (int32_t)(max_m / 111320.0 * 1e6) + 1;
    const double cl = cos(lat * M_PI / 180.0);
    const int32_t dlo = (int32_t)(max_m / (111320.0 * (cl > 0.01 ? cl : 0.01)) * 1e6) + 1;
    int best = -1;
    double bd = max_m;
    for (int i = 0; i < n_; i++) {
      const Cam& c = buf_[i];
      if (abs(c.lat_e6 - la6) > dla || abs(c.lon_e6 - lo6) > dlo) continue;
      const double d = distanceM(lat, lon, c.lat_e6 / 1e6, c.lon_e6 / 1e6);
      if (d <= bd) { bd = d; best = i; }
    }
    if (best >= 0 && dist_m) *dist_m = bd;
    return best;
  }

 private:
  Cam* buf_ = nullptr;
  int cap_ = 0;
  int n_ = 0;
};

// Whether a camera facing `dir` looks back toward someone at bearing `brg` from it.
inline bool facing(int16_t dir, double brg_from_cam) {
  if (dir < 0) return true;
  double d = fabs(fmod(brg_from_cam - dir + 540.0, 360.0) - 180.0);
  return d <= 60;
}

// ---- the alert channel ---------------------------------------------------------------------
// A detection posted to a channel, in the shape the map's watched-channel feed
// (WatchFeed.h, MeshCore Open TAK's Watch tab) reads: a red dot and ALERT for
// the level, the title, the detail, and the position.
//
//   🔴 ALERT Flock ALPR
//   Wi-Fi probe · -61dBm
//   📍 47.60378,-122.34052
inline size_t formatChannelAlert(const Detection& d, char* out, size_t cap) {
  const bool raven = d.alert.method == Method::BleRaven || d.alert.method == Method::BleRavenOne;
  int n = snprintf(out, cap, "\xF0\x9F\x94\xB4 ALERT %s\n%s \xC2\xB7 %ddBm", raven ? "Raven sensor" : "Flock ALPR",
                   methodLabel(d.alert.method), d.alert.rssi);
  if (n >= 0 && (size_t)n < cap && d.has_gps)
    n += snprintf(out + n, cap - (size_t)n, "\n\xF0\x9F\x93\x8D %.5f,%.5f", d.lat, d.lon);
  return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

// A camera marked by hand ("seen one here"), as an OBSERVED report.
//   🟡 OBSERVED Flock camera (marked)
//   facing 270°
//   📍 47.60378,-122.34052
inline size_t formatMarkedAlert(double lat, double lon, int dir, char* out, size_t cap) {
  int n = snprintf(out, cap, "\xF0\x9F\x9F\xA1 OBSERVED Flock camera (marked)");
  if (n >= 0 && (size_t)n < cap && dir >= 0) n += snprintf(out + n, cap - (size_t)n, "\nfacing %d\xC2\xB0", dir);
  if (n >= 0 && (size_t)n < cap) n += snprintf(out + n, cap - (size_t)n, "\n\xF0\x9F\x93\x8D %.5f,%.5f", lat, lon);
  return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

// Keeps the mesh quiet: one post per `gap_ms` at most, and nothing about a
// device posted within `repeat_ms` (the table's `sent` flag covers this
// session; this covers a device that dropped out of it and came back).
class SendGate {
 public:
  uint32_t gap_ms = 60000;
  uint32_t repeat_ms = 30UL * 60000;

  bool allow(const char* key, uint32_t now) const {
    if (any_ && now - last_ < gap_ms) return false;
    const uint32_t h = hash(key);
    for (const auto& r : recent_)
      if (r.used && r.h == h && now - r.at < repeat_ms) return false;
    return true;
  }
  void sent(const char* key, uint32_t now) {
    any_ = true;
    last_ = now;
    Rec* slot = &recent_[0];
    for (auto& r : recent_) {
      if (!r.used) { slot = &r; break; }
      if (now - r.at > now - slot->at) slot = &r;
    }
    slot->used = true;
    slot->h = hash(key);
    slot->at = now;
  }

 private:
  struct Rec { bool used = false; uint32_t h = 0, at = 0; };
  Rec recent_[16];
  bool any_ = false;
  uint32_t last_ = 0;
  static uint32_t hash(const char* s) {
    uint32_t h = 2166136261u;
    for (; s && *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
  }
};

// ---- settings ------------------------------------------------------------------------------
constexpr int kRadiusChoices = 4;
static const uint16_t kRadiusM[kRadiusChoices] = {100, 200, 400, 800};

struct Settings {
  bool armed = false;          // scanning (survives a reboot)
  bool show_cams = true;       // known cameras on the map
  bool show_hits = true;       // RF detections on the map
  bool proximity = true;       // warn when nearing a known camera
  uint8_t radius = 1;          // kRadiusM index
  bool share = false;          // post detections to `channel`
  uint8_t share_conf = kConfHigh;
  bool share_marked = true;    // post cameras marked by hand too
  char channel[32] = {0};      // e.g. "#flockwatch"
  // Downloading cameras on the radio (kAreaKm around it, over Wi-Fi).
  bool auto_dl = false;        // keep them current by itself
  uint8_t area = 1;            // kAreaKm index
  int32_t dl_lat_e6 = 0, dl_lon_e6 = 0;   // the last download's centre (0,0: none yet)
  uint32_t dl_at = 0;          // and when (epoch; 0 when the clock wasn't set)
};

// Whether the cameras around (lat, lon) want downloading now: never done, the
// radio has moved more than half the area away, or a week has passed.
inline bool downloadDue(const Settings& s, double lat, double lon, uint32_t now_epoch) {
  if (!s.auto_dl || (lat == 0 && lon == 0)) return false;
  if (!s.dl_lat_e6 && !s.dl_lon_e6) return true;
  const double km = kAreaKm[s.area % kAreaChoices];
  if (distanceM(lat, lon, s.dl_lat_e6 / 1e6, s.dl_lon_e6 / 1e6) > km * 500.0) return true;
  return now_epoch > 1700000000UL && s.dl_at && now_epoch > s.dl_at && now_epoch - s.dl_at > kDownloadMaxAgeS;
}

// "a=1;c=1;h=1;p=1;r=1;s=0;q=3;m=1;d=1;k=1;dla=47610000;dlo=-122330000;dlt=1790000000;ch=#flockwatch"
// (ch last: a channel name may hold anything but a ';')
inline size_t saveSettings(const Settings& s, char* out, size_t cap) {
  const int n = snprintf(out, cap, "a=%d;c=%d;h=%d;p=%d;r=%u;s=%d;q=%u;m=%d;d=%d;k=%u;dla=%ld;dlo=%ld;dlt=%lu;ch=%s",
                         s.armed, s.show_cams, s.show_hits, s.proximity, s.radius, s.share, s.share_conf,
                         s.share_marked, s.auto_dl, s.area, (long)s.dl_lat_e6, (long)s.dl_lon_e6,
                         (unsigned long)s.dl_at, s.channel);
  return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}
inline void loadSettings(const char* t, Settings& s) {
  s = Settings();
  if (!t) return;
  for (const char* p = t; *p;) {
    const char* e = strchr(p, ';');
    if (!e) e = p + strlen(p);
    const char* eq = (const char*)memchr(p, '=', (size_t)(e - p));
    if (eq) {
      const size_t kl = (size_t)(eq - p);
      const char* v = eq + 1;
      const int iv = atoi(v);
      if (kl == 2 && !strncmp(p, "ch", 2)) copyStr(s.channel, sizeof s.channel, v, (size_t)(e - v));
      else if (kl == 3 && !strncmp(p, "dla", 3)) s.dl_lat_e6 = (int32_t)strtol(v, nullptr, 10);
      else if (kl == 3 && !strncmp(p, "dlo", 3)) s.dl_lon_e6 = (int32_t)strtol(v, nullptr, 10);
      else if (kl == 3 && !strncmp(p, "dlt", 3)) s.dl_at = (uint32_t)strtoul(v, nullptr, 10);
      else if (kl == 1) {
        switch (*p) {
          case 'a': s.armed = iv != 0; break;
          case 'c': s.show_cams = iv != 0; break;
          case 'h': s.show_hits = iv != 0; break;
          case 'p': s.proximity = iv != 0; break;
          case 'r': s.radius = (uint8_t)(iv >= 0 && iv < kRadiusChoices ? iv : 1); break;
          case 's': s.share = iv != 0; break;
          case 'q': s.share_conf = (uint8_t)(iv >= kConfLow && iv <= kConfHigh ? iv : kConfHigh); break;
          case 'm': s.share_marked = iv != 0; break;
          case 'd': s.auto_dl = iv != 0; break;
          case 'k': s.area = (uint8_t)(iv >= 0 && iv < kAreaChoices ? iv : 1); break;
        }
      }
    }
    p = *e ? e + 1 : e;
  }
}

}  // namespace flockyou
