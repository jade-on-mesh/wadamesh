// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Team awareness on this radio, as a member of a MeshCore Open TAK team:
// teammates' positions, SOS alerts both ways, shared tactical pins and
// Bloodhound. The wire format is TeamWire.h; this is the state and the rules
// (beacon timing, alert repeats, pin merge), platform-neutral and host-tested.

#include <stddef.h>
#include <stdint.h>

#include "TeamWire.h"

namespace team {

constexpr int kMaxMembers = 32;
constexpr int kMaxAlerts = 16;
constexpr int kMaxMarkers = 64;

enum class Fresh : uint8_t { Live, Stale, Lost };   // < 12 min, < 30 min, older

struct Member {
  bool used = false;
  TeamWire::Position pos;
  uint32_t heard_ms = 0;
};

struct AlertRec {
  bool used = false;
  TeamWire::Alert a;
  bool acked = false;   // dismissed here
  uint32_t heard_ms = 0;
};

struct Fix {
  double lat = 0, lon = 0;
  bool has_alt = false, has_motion = false;
  int alt_m = 0;
  float speed_mps = 0, heading_deg = 0;
  uint8_t accuracy_m = 0;
  bool gps = false;   // false: the position set by hand in Settings
};

struct Settings {
  bool enabled = false;
  char channel[33] = {0};          // the team channel, by name
  char callsign[TeamWire::kMaxCallsign + 1] = {0};   // "" = node name
  uint8_t role = 0, color = 0, status = 0;
  bool share = true;               // send my position
};

class Io {
 public:
  virtual ~Io() {}
  virtual uint32_t nowMs() = 0;
  virtual uint32_t epoch() = 0;   // 0 if unknown
  virtual uint32_t random32() = 0;
  virtual void selfKey4(uint8_t out[4]) = 0;
  virtual const char* nodeName() = 0;
  virtual bool fix(Fix& f) = 0;
  virtual int batteryPct() = 0;   // -1 unknown
  virtual bool channelOk(const char* name) = 0;   // configured on this radio
  virtual bool send(const char* channel, const uint8_t* data, size_t len) = 0;
  virtual bool readFile(const char* name, uint8_t* buf, size_t cap, size_t* len) = 0;
  virtual bool writeFile(const char* name, const uint8_t* data, size_t len) = 0;
  virtual void changed() = 0;                   // redraw map / roster
  virtual void alarm(const AlertRec& r) = 0;    // a teammate's alert just went active
};

class Core {
 public:
  explicit Core(Io* io) : io_(io) {}
  void begin();
  Settings cfg;
  void saveSettings();
  bool active() { return cfg.enabled && cfg.channel[0] && io_->channelOk(cfg.channel); }
  const char* callsign() { return cfg.callsign[0] ? cfg.callsign : io_->nodeName(); }
  void identityChanged() { force_beacon_ = true; saveSettings(); }

  // incoming GRP_DATA on `channel`
  void onPayload(const char* channel, const uint8_t* p, size_t n);
  void tick();
  bool sendPosition();

  // members
  Member* member(int i) { return (i >= 0 && i < kMaxMembers && members_[i].used) ? &members_[i] : nullptr; }
  Fresh freshness(const Member& m) const;
  int memberCount() const;

  // alerts
  AlertRec* alert(int i) { return (i >= 0 && i < kMaxAlerts && alerts_[i].used) ? &alerts_[i] : nullptr; }
  AlertRec* firstUnacked();
  void acknowledge(AlertRec& r) { r.acked = true; io_->changed(); }
  bool sosActive() const { return own_active_; }
  const TeamWire::Alert& ownAlert() const { return own_; }
  void raiseAlert(uint8_t type, const char* note);
  void cancelAlert();

  // pins
  TeamWire::Marker* marker(int i) { return (i >= 0 && i < kMaxMarkers && mused_[i] && !markers_[i].deleted) ? &markers_[i] : nullptr; }
  bool isMine(const TeamWire::Marker& m);
  TeamWire::Marker* putMarker(TeamWire::Marker* existing, uint8_t kind, double lat, double lon, const char* label,
                              const char* note);
  void deleteMarker(TeamWire::Marker& m);

  // Bloodhound: a teammate (follows them) or a fixed point.
  void houndMember(const uint8_t key4[4]);
  void houndPoint(double lat, double lon, const char* label);
  void stopHound() { hound_ = 0; io_->changed(); }
  bool hound(double* lat, double* lon, char* label, size_t cap);

  // Roles: the team's names for the defaults and its added roles (edited in
  // the app; the radio follows them).
  void roleName(int id, char* out, size_t cap) const;
  char roleLetter(int id) const;
  bool roleAvailable(int id) const;
  int nextRole(int id) const;   // the next role to offer in Setup

  uint32_t lastBeaconMs() const { return last_beacon_ms_; }
  bool everBeaconed() const { return beaconed_; }

 private:
  Io* io_;
  Member members_[kMaxMembers];
  AlertRec alerts_[kMaxAlerts];
  TeamWire::Marker markers_[kMaxMarkers];
  bool mused_[kMaxMarkers] = {false};

  TeamWire::RoleDef roles_[TeamWire::kMaxRoleId + 1];
  bool rused_[TeamWire::kMaxRoleId + 1] = {false};

  bool force_beacon_ = false;
  bool beaconed_ = false;
  uint32_t last_beacon_ms_ = 0;
  Fix last_fix_;

  bool own_active_ = false;
  TeamWire::Alert own_;
  uint8_t own_sends_ = 3;          // of 3 (0, 15 s, 45 s)
  uint32_t own_next_ms_ = 0;
  uint8_t cancel_sends_ = 2;       // of 2 (0, 10 s); 2 = nothing to cancel
  TeamWire::Alert cancel_;
  uint32_t cancel_next_ms_ = 0;

  uint8_t hound_ = 0;              // 0 none, 1 member, 2 point
  uint8_t hound_key_[4] = {0};
  double hound_lat_ = 0, hound_lon_ = 0;
  char hound_label_[33] = {0};

  bool sendRaw(const uint8_t* p, size_t n);
  void onPosition(const TeamWire::Position& p);
  void onAlert(const TeamWire::Alert& a);
  void onMarker(const TeamWire::Marker& m);
  void onRoleDef(const TeamWire::RoleDef& d);
  void saveRoles();
  void loadRoles();
  const TeamWire::RoleDef* roleEdit(int id) const;
  void saveMarkers();
  void loadMarkers();
  void loadSettings();
};

}  // namespace team

// Called by MyMesh for GRP_DATA with TeamWire::kDataType (defined in the UI glue).
void teamOnChannelData(int channel_idx, const uint8_t* data, size_t len);
// Called by MyMesh for channel text: reports with a position on watched channels
// go on the map (WatchFeed.h; defined in the UI glue).
void teamOnChannelText(int channel_idx, const char* text, uint32_t sent_epoch);
