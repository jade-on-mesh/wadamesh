// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaMesh glue: the saved settings, applying them to the radio, the media
// packet marker and the `radio2` / `tx.media` CLI. See MEDIAMESH.md.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "MediaMeshCore.h"

namespace mesh { class Packet; }

namespace mediamesh {

/** True when this build's radio can run MediaMesh (SX126x on ESP32). */
bool supported();

/** The saved settings (loaded on first use). */
const Config& config();

/** Load from storage and apply to the radio. Call once the main radio is set up. */
void begin();

/** Replace the settings: saves and applies. Returns false if invalid. */
bool set(const Config& c);

/** Mark a packet the firmware is about to send as media (photo or voice). */
void markPacket(const mesh::Packet* pkt);

/** Whether photos and voice are currently sent on radio2. */
bool mediaActive();

/** Runs `get/set radio2`, `radio2.cross`, `radio2.timing`, `tx.media` and
 *  `get mediamesh`. Returns false when `cmd` is none of these. */
bool runCli(const char* cmd, char* reply, size_t cap);

/** Live state for the settings page. */
struct Status {
  bool scanning = false;
  uint8_t profile = 0;
  uint32_t switches = 0;
  uint32_t switch_us = 0;
  uint32_t tx[2] = {};
  uint32_t rx[2] = {};
  Timing timing;
};
Status status();

}  // namespace mediamesh
