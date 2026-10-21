// SPDX-License-Identifier: GPL-3.0-or-later
// Drone Detector glue (src/ui-touch/DroneCore.h) around GridDown's Remote ID
// parser: Wi-Fi frames, BLE adverts one message at a time, and the channel post
// read back by the Watch feed. The post strings are the ones MeshCore Open TAK's
// test/vox/remote_id_test.dart expects.
//   g++ -std=c++17 -Wall -Wextra -I src test/test_drone_core.cpp src/ui-touch/remoteid.cpp -o /tmp/d && /tmp/d
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "ui-touch/DroneCore.h"
#include "ui-touch/WatchFeed.h"

static int g_fail = 0;
#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      g_fail++;                                                     \
    }                                                               \
  } while (0)
#define CHECK_STR(a, b)                                                           \
  do {                                                                            \
    if (strcmp((a), (b)) != 0) {                                                  \
      printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b));    \
      g_fail++;                                                                   \
    }                                                                             \
  } while (0)

using Bytes = std::vector<uint8_t>;

static void putI32(uint8_t* p, int32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void putU16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// F3411 messages (25 bytes each).
static Bytes basicId(const char* id) {
  Bytes m(25, 0);
  m[0] = RID_MSG_BASIC_ID << 4;
  m[1] = (1 << 4) | 2;   // serial number, rotorcraft
  memcpy(&m[2], id, strlen(id));
  return m;
}
static Bytes location(double lat, double lon, uint8_t speed_raw, uint8_t dir, int agl_m) {
  Bytes m(25, 0);
  m[0] = RID_MSG_LOCATION << 4;
  m[2] = dir;
  m[3] = speed_raw;   // x 0.25 m/s
  putI32(&m[5], (int32_t)llround(lat * 1e7));
  putI32(&m[9], (int32_t)llround(lon * 1e7));
  putU16(&m[15], (uint16_t)((agl_m + 40 + 1000) * 2));   // geodetic: a bit higher
  putU16(&m[17], (uint16_t)((agl_m + 1000) * 2));
  return m;
}
static Bytes systemMsg(double lat, double lon) {
  Bytes m(25, 0);
  m[0] = RID_MSG_SYSTEM << 4;
  putI32(&m[2], (int32_t)llround(lat * 1e7));
  putI32(&m[6], (int32_t)llround(lon * 1e7));
  return m;
}
static Bytes pack(std::initializer_list<Bytes> msgs) {
  Bytes p = {(uint8_t)(RID_MSG_PACK << 4), 25, (uint8_t)msgs.size()};
  for (const Bytes& m : msgs) p.insert(p.end(), m.begin(), m.end());
  return p;
}
// A BLE legacy advert: flags, then service data 0xFFFA, app code 0x0D, counter, message.
static Bytes bleAdv(const Bytes& msg, uint8_t counter) {
  Bytes a = {2, 0x01, 0x06, (uint8_t)(1 + 2 + 1 + 1 + msg.size()), 0x16, 0xFA, 0xFF, 0x0D, counter};
  a.insert(a.end(), msg.begin(), msg.end());
  return a;
}
// A beacon: 24-byte header (addr2 at 10), 12-byte fixed body, SSID IE, ASTM vendor IE, FCS.
static Bytes beacon(const Bytes& payload, const uint8_t mac[6]) {
  Bytes f(36, 0);
  f[0] = 0x80;
  memcpy(&f[10], mac, 6);
  const char* ssid = "DRONE-AP";
  f.push_back(0);
  f.push_back((uint8_t)strlen(ssid));
  f.insert(f.end(), ssid, ssid + strlen(ssid));
  f.push_back(0xDD);
  f.push_back((uint8_t)(5 + payload.size()));
  const uint8_t hdr[5] = {0xFA, 0x0B, 0xBC, 0x0D, 7};
  f.insert(f.end(), hdr, hdr + 5);
  f.insert(f.end(), payload.begin(), payload.end());
  for (int i = 0; i < 4; i++) f.push_back(0xEE);   // FCS
  return f;
}

static const char* kId = "1581F9DEV26590296G6G";

static void testWifi() {
  rid_reset();
  drone::AddrIds ids;
  const uint8_t mac[6] = {0x60, 0x60, 0x1F, 1, 2, 3};
  const Bytes f = beacon(pack({basicId(kId), location(47.60378, -122.34052, 28, 90, 120), systemMsg(47.60531, -122.34016)}), mac);
  uint16_t off = 0;
  uint8_t tr = 0;
  const uint16_t n = drone::wifiPayload(f.data(), (uint16_t)f.size(), &off, &tr);
  CHECK(n == 3 + 3 * 25);
  CHECK(tr == RID_TRANSPORT_WIFI_BEACON);
  const int idx = drone::ingest(f.data() + off, n, tr, -61, drone::wifiSource(f.data()), 1000, ids);
  CHECK(idx >= 0);
  const RemoteIdTrack* t = rid_getTrack(idx);
  CHECK(t && !strcmp(t->uasId, kId) && t->hasPosition && t->hasOperator && t->heightAglM == 120);

  // Not a beacon, a data frame, a beacon without Remote ID: nothing.
  Bytes probe = f;
  probe[0] = 0x40;
  CHECK(drone::wifiPayload(probe.data(), (uint16_t)probe.size(), &off, &tr) == 0);
  Bytes data = f;
  data[0] = 0x08;
  CHECK(drone::wifiPayload(data.data(), (uint16_t)data.size(), &off, &tr) == 0);
  Bytes plain(60, 0);
  plain[0] = 0x80;
  CHECK(drone::wifiPayload(plain.data(), (uint16_t)plain.size(), &off, &tr) == 0);
  CHECK(drone::wifiPayload(f.data(), 39, &off, &tr) == 0);
  // A truncated vendor IE is refused, not over-read.
  CHECK(drone::wifiPayload(f.data(), (uint16_t)(f.size() - 40), &off, &tr) == 0);
}

// BLE: Basic ID, Location and System arrive in separate adverts from one
// address; without the address memory the Location would be dropped.
static void testBleOneMessageEach() {
  rid_reset();
  drone::AddrIds ids;
  const uint8_t mac[6] = {0xC0, 1, 2, 3, 4, 5};
  uint16_t off = 0, n;

  // Location first: no ID known yet for this address, so no track.
  Bytes a = bleAdv(location(47.6, -122.3, 20, 10, 50), 1);
  n = rid_extractFromBleAdv(a.data(), (uint16_t)a.size(), &off);
  CHECK(n == 25);
  CHECK(drone::ingest(a.data() + off, n, RID_TRANSPORT_BLE_LEGACY, -70, mac, 100, ids) < 0);

  a = bleAdv(basicId(kId), 2);
  n = rid_extractFromBleAdv(a.data(), (uint16_t)a.size(), &off);
  const int idx = drone::ingest(a.data() + off, n, RID_TRANSPORT_BLE_LEGACY, -70, mac, 200, ids);
  CHECK(idx >= 0);

  a = bleAdv(location(47.60378, -122.34052, 28, 45, 120), 3);
  n = rid_extractFromBleAdv(a.data(), (uint16_t)a.size(), &off);
  CHECK(drone::ingest(a.data() + off, n, RID_TRANSPORT_BLE_LEGACY, -68, mac, 300, ids) == idx);
  a = bleAdv(systemMsg(47.60531, -122.34016), 4);
  n = rid_extractFromBleAdv(a.data(), (uint16_t)a.size(), &off);
  CHECK(drone::ingest(a.data() + off, n, RID_TRANSPORT_BLE_LEGACY, -68, mac, 400, ids) == idx);
  const RemoteIdTrack* t = rid_getTrack(idx);
  CHECK(t && t->hasPosition && t->hasOperator && t->headingDeg == 45 && t->speedCms == 700);
  CHECK(rid_trackCount() == 1);

  // Another address is a different drone, and a forgotten address lends nothing.
  const uint8_t other[6] = {0xC0, 9, 9, 9, 9, 9};
  a = bleAdv(location(1, 1, 0, 0, 0), 5);
  n = rid_extractFromBleAdv(a.data(), (uint16_t)a.size(), &off);
  CHECK(drone::ingest(a.data() + off, n, RID_TRANSPORT_BLE_LEGACY, -80, other, 500, ids) < 0);
  CHECK(drone::ingest(a.data() + off, n, RID_TRANSPORT_BLE_LEGACY, -80, mac, 300 + drone::kAddrForgetMs + 1, ids) < 0);
}

static void testAddrIdsEviction() {
  drone::AddrIds ids;
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  char id[16];
  for (int i = 0; i < drone::kAddrs + 4; i++) {
    mac[5] = (uint8_t)i;
    snprintf(id, sizeof id, "ID%06d", i);
    ids.note(mac, id, (uint32_t)(1000 + i));
  }
  mac[5] = 0;
  CHECK(ids.find(mac, 2000) == nullptr);   // the quietest went
  mac[5] = (uint8_t)(drone::kAddrs + 3);
  CHECK(ids.find(mac, 2000) && !strcmp(ids.find(mac, 2000), "ID000027"));
}

static void testPost() {
  RemoteIdTrack t;
  memset(&t, 0, sizeof t);
  snprintf(t.uasId, sizeof t.uasId, "%s", kId);
  char out[200];
  CHECK(drone::formatPost(t, out, sizeof out) == 0);   // no position: nothing to post
  t.hasPosition = true;
  t.lat = 47.60378;
  t.lon = -122.34052;
  t.heightAglM = 120;
  t.speedCms = 700;
  t.headingDeg = 90;
  CHECK(drone::formatPost(t, out, sizeof out) > 0);
  CHECK_STR(out, "DRONE 1581F9DEV26590296G6G h120m 7.0m/s hdg90\nhttps://maps.google.com/?q=47.60378,-122.34052");
  t.hasOperator = true;
  t.opLat = 47.60531;
  t.opLon = -122.34016;
  t.speedCms = 1240;
  drone::formatPost(t, out, sizeof out);
  CHECK_STR(out, "DRONE 1581F9DEV26590296G6G h120m 12m/s hdg90\nhttps://maps.google.com/?q=47.60378,-122.34052\n"
                 "Op https://maps.google.com/?q=47.60531,-122.34016");

  // Read back the way the Watch feed reads it, on this radio and in the app.
  watchfeed::Report r;
  CHECK(watchfeed::parse(out, r));
  CHECK_STR(r.track, kId);
  CHECK_STR(r.detail, "h120m 12m/s");
  CHECK(r.has_bearing && fabsf(r.bearing - 90) < 0.01f);
  CHECK(fabs(r.lat - 47.60378) < 1e-6 && fabs(r.lon + 122.34052) < 1e-6);
  CHECK(r.has2 && fabs(r.lat2 - 47.60531) < 1e-6);
  CHECK_STR(r.label2, "Op");

  // Hovering: no heading; geodetic altitude when there is no height.
  t.speedCms = 0;
  t.heightAglM = 0;
  t.altGeoM = 95;
  t.hasOperator = false;
  drone::formatPost(t, out, sizeof out);
  CHECK_STR(out, "DRONE 1581F9DEV26590296G6G alt95m 0.0m/s\nhttps://maps.google.com/?q=47.60378,-122.34052");
  CHECK(watchfeed::parse(out, r) && !r.has_bearing);

  // An ID that can't be a Watch serial: nothing posted. Odd characters dropped.
  snprintf(t.uasId, sizeof t.uasId, "ABC");
  CHECK(drone::formatPost(t, out, sizeof out) == 0);
  snprintf(t.uasId, sizeof t.uasId, "FIN.87astr dge12k8");
  CHECK(drone::formatPost(t, out, sizeof out) > 0);
  CHECK(!strncmp(out, "DRONE FIN87astrdge12k8 ", 23));
  char sp[16];
  drone::speedText(95, sp, sizeof sp);
  CHECK_STR(sp, "1.0m/s");
  drone::speedText(949, sp, sizeof sp);
  CHECK_STR(sp, "9.5m/s");
  drone::speedText(950, sp, sizeof sp);
  CHECK_STR(sp, "10m/s");
}

int main() {
  testWifi();
  testBleOneMessageEach();
  testAddrIdsEviction();
  testPost();
  if (g_fail) {
    printf("%d failure(s)\n", g_fail);
    return 1;
  }
  printf("drone core: all passed\n");
  return 0;
}
