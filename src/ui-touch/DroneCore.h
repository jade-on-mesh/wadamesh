// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Drone Detector: Remote ID (ASTM F3411 / Open Drone ID) on this standalone.
// GridDown Secure Messenger's parser and track table do the decoding
// (remoteid.h/.cpp, BlackAtlas LLC, GPL-3.0, vendored unchanged); this is the
// glue Wadamesh adds around them, pure logic and host-tested
// (test/test_drone_core.cpp):
//
//  * Wi-Fi: find the Remote ID payload in a captured management frame
//    (beacon vendor IE, or a NAN action frame's IE list).
//  * BLE: legacy adverts carry ONE message each, so a Location advert has no
//    UAS ID and GridDown's rid_ingest would drop it. Each address's UAS ID is
//    remembered from its Basic ID advert and lent to the address's other
//    messages (AddrIds).
//  * The channel post, in the grammar both Watch feeds read
//    (WatchFeed.h, MeshCore Open TAK's lib/team/watch_feed.dart):
//      DRONE 1581F9DEV26590296G6G h120m 7m/s hdg90
//      https://maps.google.com/?q=47.60378,-122.34052
//      Op https://maps.google.com/?q=47.60531,-122.34016

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "remoteid.h"

namespace drone {

// ---- Wi-Fi ----------------------------------------------------------------------------

/** The Remote ID payload inside an 802.11 management frame of `len` bytes
 *  (`len` as the radio reports it; a trailing FCS is harmless). Returns its
 *  length and sets *off and *transport, or 0 when the frame carries none.
 *  Beacons (subtype 8) keep their IEs after a 12-byte fixed body; action
 *  frames (subtype 13, NAN) are scanned from the header on, as GridDown does. */
inline uint16_t wifiPayload(const uint8_t* f, uint16_t len, uint16_t* off, uint8_t* transport) {
  if (!f || !off || !transport || len < 40 || len > 2048) return 0;
  if ((f[0] & 0x0C) != 0x00) return 0;   // not a management frame
  const uint8_t subtype = (uint8_t)((f[0] >> 4) & 0x0F);
  uint16_t ie;
  if (subtype == 0x08) ie = 24 + 12;
  else if (subtype == 0x0D) ie = 24;
  else return 0;
  if (len <= ie) return 0;
  uint16_t o = 0;
  const uint16_t n = rid_extractFromBeaconIe(f + ie, (uint16_t)(len - ie), &o);
  if (!n) return 0;
  *off = (uint16_t)(ie + o);
  *transport = subtype == 0x08 ? RID_TRANSPORT_WIFI_BEACON : RID_TRANSPORT_WIFI_NAN;
  return n;
}

/** The transmitter address (addr2) of a management frame. */
inline const uint8_t* wifiSource(const uint8_t* f) { return f + 10; }

// ---- which UAS ID an address is sending ------------------------------------------------

constexpr int kAddrs = 24;
constexpr uint32_t kAddrForgetMs = 120000;   // BLE addresses rotate; forget quiet ones

class AddrIds {
 public:
  /** The UAS ID last heard from `mac`, or nullptr. */
  const char* find(const uint8_t mac[6], uint32_t now) const {
    for (const Slot& s : _s)
      if (s.used && !memcmp(s.mac, mac, 6) && now - s.at <= kAddrForgetMs) return s.id;
    return nullptr;
  }
  void note(const uint8_t mac[6], const char* id, uint32_t now) {
    if (!id || !id[0]) return;
    Slot* slot = nullptr;
    for (Slot& s : _s)
      if (s.used && !memcmp(s.mac, mac, 6)) { slot = &s; break; }
    if (!slot) {
      for (Slot& s : _s) {
        if (!s.used) { slot = &s; break; }
        if (!slot || now - s.at > now - slot->at) slot = &s;   // the quietest goes
      }
    }
    slot->used = true;
    memcpy(slot->mac, mac, 6);
    snprintf(slot->id, sizeof slot->id, "%s", id);
    slot->at = now;
  }
  void clear() { for (Slot& s : _s) s.used = false; }

 private:
  struct Slot {
    bool used = false;
    uint8_t mac[6] = {0};
    char id[RID_UASID_LEN] = {0};
    uint32_t at = 0;
  };
  Slot _s[kAddrs];
};

/** One captured payload into GridDown's track table, lending the address's
 *  UAS ID to a message that came without one. Returns the track index, or -1. */
inline int ingest(const uint8_t* payload, uint16_t len, uint8_t transport, int8_t rssi, const uint8_t mac[6],
                  uint32_t now, AddrIds& ids) {
  RemoteIdTrack obs;
  memset(&obs, 0, sizeof obs);
  if (rid_parsePayload(payload, len, &obs) <= 0) return -1;
  if (mac) {
    if (obs.uasId[0]) {
      ids.note(mac, obs.uasId, now);
    } else if (const char* id = ids.find(mac, now)) {
      snprintf(obs.uasId, sizeof obs.uasId, "%s", id);
    }
  }
  return rid_ingest(&obs, transport, rssi, now);
}

// ---- the channel post ---------------------------------------------------------------------

/** The UAS ID as a Watch serial: letters, digits and '-' (WatchFeed findDrone).
 *  Returns false when what is left can't be tracked there (under 6 characters
 *  or no digit). */
inline bool serialOf(const char* id, char* out, size_t cap) {
  size_t n = 0;
  bool digit = false;
  for (const char* p = id; *p && n + 1 < cap; ++p) {
    const char c = *p;
    const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-';
    if (!ok) continue;
    digit |= c >= '0' && c <= '9';
    out[n++] = c;
  }
  out[n] = 0;
  return n >= 6 && digit;
}

/** "7m/s", "12.5m/s" (under 10 m/s with a tenth). */
inline void speedText(uint16_t cms, char* out, size_t cap) {
  if (cms >= 950) {
    snprintf(out, cap, "%um/s", (unsigned)((cms + 50) / 100));
  } else {
    const unsigned dm = (unsigned)((cms + 5) / 10);   // tenths of a m/s
    snprintf(out, cap, "%u.%um/s", dm / 10, dm % 10);
  }
}

/** The post for track `t`, or 0 when it has no position (Watch needs one) or
 *  its ID can't be a Watch serial. */
inline size_t formatPost(const RemoteIdTrack& t, char* out, size_t cap) {
  if (!t.hasPosition || !cap) return 0;
  char serial[RID_UASID_LEN];
  if (!serialOf(t.uasId, serial, sizeof serial)) return 0;
  char facts[48] = "";
  size_t k = 0;
  if (t.heightAglM) k += (size_t)snprintf(facts + k, sizeof facts - k, " h%dm", t.heightAglM);
  else if (t.altGeoM) k += (size_t)snprintf(facts + k, sizeof facts - k, " alt%dm", t.altGeoM);
  char sp[16];
  speedText(t.speedCms, sp, sizeof sp);
  k += (size_t)snprintf(facts + k, sizeof facts - k, " %s", sp);
  if (t.speedCms >= 50) snprintf(facts + k, sizeof facts - k, " hdg%u", (unsigned)(t.headingDeg % 360));
  int n = snprintf(out, cap, "DRONE %s%s\nhttps://maps.google.com/?q=%.5f,%.5f", serial, facts, t.lat, t.lon);
  if (n > 0 && (size_t)n < cap && t.hasOperator)
    n += snprintf(out + n, cap - (size_t)n, "\nOp https://maps.google.com/?q=%.5f,%.5f", t.opLat, t.opLon);
  return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

}  // namespace drone
