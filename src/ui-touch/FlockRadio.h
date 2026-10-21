// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Flock You's radio side: Wi-Fi promiscuous sniffing with channel hopping and
// a duty-cycled BLE scan, borrowed from the rest of the firmware only while it
// is idle. Detections queue up here (from the Wi-Fi and NimBLE tasks) and the
// UI loop drains them with pop(). The rules are in FlockCore.h.
//
// The Drone Detector shares it: while it wants the radios, Remote ID payloads
// (Wi-Fi beacons, NAN, BLE service data 0xFFFA) queue up separately and are
// drained with popRemoteId(). Either app keeps the radios listening.
//
// Who owns what:
//   Wi-Fi  Flock You sniffs only while the station is NOT connected, no AP is
//          up and no system scan runs. It lets go the moment any of that
//          changes (a connection comes up, the web remote starts an AP...).
//   BLE    Shares NimBLE's one scanner. Never while the Bluetooth keyboard is
//          scanning, pairing or reconnecting; never while Bluetooth is off.
//          stackGoingDown() must be called before NimBLEDevice::deinit().
//   GPS    Not touched: the UI reads the position when it records a hit.

#include <stddef.h>
#include <stdint.h>

#include "FlockCore.h"
#include "device_caps.h"

#if CAP_FLOCK_YOU
namespace FlockRadio {

struct Diag {
  uint32_t wifi_frames = 0;    // management frames seen
  uint32_t wifi_hits = 0;      // probe requests matching the signature
  uint32_t wifi_errors = 0;    // promiscuous mode refused
  uint32_t ble_adv = 0;        // advertisements seen
  uint32_t ble_hits = 0;       // advertisements matching a rule
  uint32_t queue_drops = 0;    // hits lost to a full queue
  uint32_t wifi_yield = 0;     // times Wi-Fi was handed back
  uint32_t ble_yield = 0;      // times BLE was handed back
  uint32_t rid_wifi = 0;       // Remote ID payloads from Wi-Fi
  uint32_t rid_ble = 0;        // Remote ID payloads from BLE
  uint32_t rid_drops = 0;      // Remote ID payloads lost to a full queue
};

// One captured Remote ID payload (DroneCore.h parses it).
struct RidRaw {
  uint8_t data[264];
  uint16_t len;
  uint8_t transport;           // RID_TRANSPORT_*
  int8_t rssi;
  uint8_t mac[6];              // the sender's address
};

// Scan or stop for Flock You. Idempotent; the work happens in tick().
void setWanted(bool on);
bool wanted();

// The same for the Drone Detector.
void setDroneWanted(bool on);
bool droneWanted();

// Next queued Remote ID payload; false when none.
bool popRemoteId(RidRaw& out);

// From the UI loop, every pass: takes and releases the radios, hops channels,
// restarts the BLE window.
void tick(uint32_t now_ms);

// Next queued detection; false when none.
bool pop(flockyou::Alert& out);

bool wifiActive();   // sniffing now
bool bleActive();    // our scan is running now
uint8_t wifiChannel();

// Why a radio is not in use, for the status line ("connected", "keyboard"...);
// "" when it is.
const char* wifiWhyNot();
const char* bleWhyNot();

const Diag& diag();

// Right before NimBLEDevice::deinit(): forget the scanner, it is about to be freed.
void stackGoingDown();

}  // namespace FlockRadio
#endif
