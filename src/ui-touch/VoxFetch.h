// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fetches MeshCore Open TAK photos for this radio: the requester side of the
// app's pull transfer (lib/vox/voice_service.dart in meshcore-open-tak), so a
// wadamesh radio with no phone attached can download a photo it saw announced.
//
// A photo is announced with a VI1 envelope; nothing is sent until someone asks.
// Each attempt picks the cheapest way that can work, as the app does:
//   direct   binary fetch over raw data along our route to the sender, asking
//            it to reply along the reverse of that route (alternating with its
//            own route when a direct attempt gets nothing back);
//   dm       a VR3 text DM, flooded (our route is cleared first), whose ACK and
//            path exchange give both radios a route; the sender answers over
//            raw data;
//   channel  the binary request as GRP_DATA (0x5643) on the channel the photo
//            was posted in; the sender, or anyone holding the photo, floods
//            the packets back to every listener.
// Attempts escalate on timeouts sized from the radio's airtime, re-asking only
// for missing packets, up to 15 attempts or 3 in a row that bring nothing.
//
// Pure C++ against VoxFetchIo, so the state machine is host-tested
// (test/test_vox_fetch.cpp). Single-threaded: call everything from one loop.

#include <stddef.h>
#include <stdint.h>

#include "VoxWire.h"

class VoxFetchIo {
 public:
  virtual ~VoxFetchIo() {}
  virtual uint32_t nowMs() = 0;
  virtual void selfKey6(uint8_t out[6]) = 0;
  // Route to a contact for raw data: its packed path length (MeshCore
  // out_path_len) and path bytes. False when there is no direct route.
  virtual bool contactRoute(const uint8_t pub[32], uint8_t* path, uint8_t* packed_len) = 0;
  // Hops to a contact, or -1 if unknown.
  virtual int contactHops(const uint8_t pub[32]) = 0;
  // A contact whose name matches (channel posts only carry a name).
  virtual bool contactByName(const char* name, uint8_t pub_out[32]) = 0;
  virtual bool sendRaw(const uint8_t* path, uint8_t packed_len, const uint8_t* data, size_t len) = 0;
  // Clear the route to the contact, then send `text` as a DM (so it floods).
  virtual bool sendDmFlood(const uint8_t pub[32], const char* text) = 0;
  virtual bool sendChannelData(int channel, uint16_t type, const uint8_t* data, size_t len) = 0;
  // Airtime of one full photo packet with the radio's current settings.
  virtual uint32_t packetAirMs() = 0;
  virtual bool hasPayload(uint32_t sid) = 0;
  virtual bool savePayload(uint32_t sid, const uint8_t* data, size_t len) = 0;
  // A session's status changed (the UI redraws its bubble).
  virtual void changed(uint32_t sid) = 0;
};

class VoxFetch {
 public:
  enum Status : uint8_t { kUnknown = 0, kRemote, kRequesting, kReceiving, kComplete, kUnavailable };
  enum Method : uint8_t { kNone = 0, kDirect, kDm, kChannel };

  static constexpr int kMaxSessions = 6;
  static constexpr int kMaxAttempts = 15;
  static constexpr int kMaxStalls = 3;

  explicit VoxFetch(VoxFetchIo& io) : io_(io) {}
  ~VoxFetch();

  // A VI1 envelope arrived (or a stored message was shown). `sender_pub` is the
  // DM sender; for channel posts pass nullptr, the post's author name, and the
  // channel index. `start` begins fetching straight away.
  void onEnvelope(const VoxWire::Envelope& env, const uint8_t* sender_pub, const char* sender_name,
                  int channel, bool start);

  // Start, or retry after a failure (a tap on the bubble). False if unknown.
  bool request(uint32_t sid);

  // A raw-data payload (channel < 0) or a GRP_DATA 0x5643 payload.
  void onPayload(const uint8_t* data, size_t len, int channel);

  // Timeouts; call every loop iteration.
  void tick();

  Status status(uint32_t sid, int* received = nullptr, int* total = nullptr) const;
  Method lastMethod(uint32_t sid) const;
  int attempts(uint32_t sid) const;

  // Short human status for a bubble, e.g. "Downloading 12/36" or "Voice 7s - tap to play".
  const char* statusText(uint32_t sid, char* buf, size_t cap) const;

 private:
  struct Session {
    bool used = false;
    uint32_t sid = 0;
    uint8_t total = 0;
    uint16_t aspect = 0;
    int packet_bytes = VoxWire::kBytesPerPacket;
    VoxWire::Kind kind = VoxWire::kImage;
    Status st = kRemote;
    bool has_pub = false;
    uint8_t pub[32] = {0};
    char name[32] = {0};
    int channel = -1;
    uint8_t* storage = nullptr;
    VoxWire::Assembly asm_;
    int attempts = 0;
    int stalls = 0;
    int direct_failures = 0;
    bool tried_channel = false;
    bool reply_along_path = true;
    bool sent_dm = false;
    Method last = kNone;
    int at_last_attempt = 0;
    int expected_last = -1;
    bool timer_on = false;
    uint32_t deadline = 0;
    uint32_t last_touch = 0;
    const char* note = nullptr;   // why it's unavailable
  };

  Session* find(uint32_t sid);
  const Session* find(uint32_t sid) const;
  Session* claim(uint32_t sid);
  void release(Session& s);
  bool ensureStorage(Session& s);
  Method choose(Session& s);
  void attempt(Session& s);
  void onTimeout(Session& s);
  void accept(Session& s, const VoxWire::Packet& p);
  void complete(Session& s);
  void fail(Session& s, const char* why);
  int hops(const Session& s);
  uint32_t packetGap(bool flood, int hops);
  uint32_t waitFor(Session& s);
  void arm(Session& s, uint32_t ms);

  VoxFetchIo& io_;
  Session sessions_[kMaxSessions];
};
