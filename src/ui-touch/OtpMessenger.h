// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OTP Messenger on this radio: the protocol core (OtpCore) wired to the mesh,
// the internal filesystem and the UI. Runs on the Arduino loop, like the mesh.
// Messages are handled whether or not the app is open; the UI (UITask.cpp)
// only draws state and calls in.

#include <stddef.h>
#include <stdint.h>

#include "OtpCore.h"

namespace fs { class FS; }

namespace OtpMessenger {

void begin(fs::FS* fs);       // once, when the internal FS is mounted
bool ready();
otp::Core& core();

// Mesh hooks (MyMesh). True: OTP traffic for a conversation with a pad, which
// the normal chat must not show.
bool onDirectText(const char* text, const uint8_t sender_pub[32]);
bool onChannelText(const char* text, int channel_idx);   // "Name: body" as posted

void tick();

// Bumped whenever anything the app shows may have changed.
uint32_t version();

// UI callbacks (set by UITask).
void setCallbacks(void (*toast)(const char* msg),
                  void (*incoming)(otp::Target& t, const otp::Entry& e));

// Contacts that can hold a DM pad (chat nodes), for the picker.
int contactCount();
bool contactAt(int i, uint8_t pub_out[32], char* name, size_t name_cap);
// Configured channels by name ("" for empty slots).
int channelCount();
bool channelAt(int i, char* name, size_t cap);

// Writes the fresh pad of `t` as hex to `path` on `fs` (for the other device).
bool exportPad(otp::Target& t, fs::FS* fs, const char* path);

}  // namespace OtpMessenger
