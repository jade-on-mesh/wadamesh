// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MeshCore Open TAK photos and voice clips on this radio: wires VoxFetch to the
// mesh (MyMesh), the internal filesystem (finished photos /vox_<sid>.vi1, voice
// clips /vox_<sid>.ve3) and the chat UI.
// Everything runs on the Arduino loop task, like the mesh and LVGL.

#include <stddef.h>
#include <stdint.h>

#include "VoxFetch.h"
#include "VoxWire.h"

namespace fs { class FS; }

namespace VoxPhotos {

// Where finished photos are kept (the internal filesystem; set once by the UI).
void setStore(fs::FS* fs);

// Mesh hooks.
void onDirectText(const char* text, const uint8_t sender_pub[32]);
void onChannelText(const char* text, int channel_idx);   // "Name: VI1:..." as posted
void onPayload(const uint8_t* data, size_t len, int channel_idx);   // raw (-1) or GRP_DATA 0x5643
void tick();

// UI side. `text` is a message body as shown (sender already split off).
bool parse(const char* text, VoxWire::Envelope* env);
// Registers what the bubble knows about the photo (after a reboot the mesh
// hooks never saw it) and starts or retries the download.
void request(const VoxWire::Envelope& env, const uint8_t* dm_pub, const char* sender_name,
             int channel_idx);
VoxFetch::Status status(uint32_t sid, int* received = nullptr, int* total = nullptr);
const char* statusText(uint32_t sid, char* buf, size_t cap);
// Whether this radio fetches the clip (photos; Codec2 voice where there's a
// speaker and the mode is built in) and whether it can play it.
bool canFetch(const VoxWire::Envelope& env);
bool canPlay(const VoxWire::Envelope& env);
// A finished photo's payload ([0x01][OBUs]); free with free().
bool load(uint32_t sid, uint8_t** data, size_t* len);
// A finished voice clip: its VE3 wire mode and codec frames; free with free().
bool loadVoice(uint32_t sid, int* mode, uint8_t** frames, size_t* len);
bool loadFile(uint32_t sid, bool voice, uint8_t** data, size_t* len);
// Bumped whenever any photo's status changes; the chat redraws on a change.
uint32_t version();

}  // namespace VoxPhotos
