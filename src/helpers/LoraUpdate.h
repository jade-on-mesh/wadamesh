// SPDX-License-Identifier: GPL-3.0-or-later
//
// LoRa Update: this standalone serves `.mota` firmware from the SD card's
// /mota folder to a repeater that pulls it over LoRa (Mike Carper's LoRa OTA;
// LORA_UPDATE.md). The engine is his (helpers/ota, MIT) and only runs while
// a session is open: outside one, OTA packets are ignored and nothing is
// allocated. The page in ui-touch/LoraUpdate.inc drives the repeater.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "LoraUpdateCore.h"

namespace fs { class FS; }

namespace loraupdate {

/** The packet type the OTA engine rides in (Mike's core: PAYLOAD_TYPE_OTA). */
static constexpr uint8_t kPayloadType = 0x0C;

/** Puts one engine message on the air as a zero-path flood. False is
 *  backpressure: the engine keeps the message and tries again. */
using SendFn = bool (*)(const uint8_t* msg, uint16_t len);

/** Once, at boot: our id (its first 4 bytes name us as a source) and how to send. */
void begin(const uint8_t self_pub[32], SendFn send);

/** Re-read `/mota` on `sd`. Returns the number of servable files. */
uint8_t scan(fs::FS* sd);
uint8_t fileCount();
const FileInfo* file(uint8_t i);
/** Files in the folder that are not valid `.mota` containers (or past the limit). */
uint8_t skipped();

/** Start serving file `i` (alone) and announce it. False: no memory, no file. */
bool start(uint8_t i);
/** Stop serving and free the engine. */
void stop();
bool active();

/** An OTA packet heard over `hops` relays. Only taken while active, and only
 *  from a neighbour: the temporary radio is shared with the repeater alone. */
void onPacket(const uint8_t* payload, uint16_t len, uint8_t hops);

/** Clock, retries and paced replies. Call every loop. */
void loop();

/** Packets the engine has sent this session. */
uint32_t packetsSent();

}  // namespace loraupdate
