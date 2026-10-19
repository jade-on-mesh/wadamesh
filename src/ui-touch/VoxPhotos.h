// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MeshCore Open TAK photos and voice clips on this radio: wires VoxFetch
// (downloading) and VoxServe (answering downloads of clips recorded here) to the
// mesh (MyMesh), the internal filesystem (finished photos /vox_<sid>.vi1, voice
// clips /vox_<sid>.ve3, received or recorded) and the chat UI.
// Everything runs on the Arduino loop task, like the mesh and LVGL.

#include <stddef.h>
#include <stdint.h>

#include "VoxFetch.h"
#include "VoxWire.h"

namespace fs { class FS; }

namespace VoxPhotos {

// Where finished photos are kept (the internal filesystem; set once by the UI).
void setStore(fs::FS* fs);

// Mesh hooks. onDirectText returns true for a VR3 fetch request (a control
// message between apps; it never belongs in the chat).
bool onDirectText(const char* text, const uint8_t sender_pub[32]);
void onChannelText(const char* text, int channel_idx);   // "Name: VI1:..." as posted
// raw (-1) or GRP_DATA 0x5643; `via` is the radio profile it arrived on
// (VoxFetch::kViaMain / kViaRadio2, or kViaUnknown).
// `hops`: how far a channel packet came (0 = heard directly; -1 unknown).
void onPayload(const uint8_t* data, size_t len, int channel_idx, uint8_t via = VoxFetch::kViaUnknown, int hops = -1);
void tick();

// UI side. `text` is a message body as shown (sender already split off).
bool parse(const char* text, VoxWire::Envelope* env);
// Registers what the bubble knows about the photo (after a reboot the mesh
// hooks never saw it) and starts or retries the download.
void request(const VoxWire::Envelope& env, const uint8_t* dm_pub, const char* sender_name,
             int channel_idx);
VoxFetch::Status status(uint32_t sid, int* received = nullptr, int* total = nullptr);
const char* statusText(uint32_t sid, char* buf, size_t cap);
// "Received via MediaMesh" etc. for a finished download, or nullptr when not known.
const char* viaText(uint32_t sid);
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

// ---- Voice clips recorded on this radio --------------------------------------
// Stores a clip (taking ownership of `bits`, a malloc'd buffer), makes it
// available to anyone who asks for it, and writes its VE3 envelope (the text
// to post in the chat) to `envelope`. False if it couldn't be stored.
bool addRecording(uint8_t wire_mode, uint8_t* bits, size_t len, uint32_t ms, char* envelope, size_t cap,
                  uint32_t* sid_out = nullptr);
// Deletes a recording made with addRecording that was never sent.
void discardRecording(uint32_t sid);
// Whether a voice clip with this session id is stored here (recorded or
// downloaded), so the bubble can play it.
bool haveVoice(uint32_t sid);

}  // namespace VoxPhotos
