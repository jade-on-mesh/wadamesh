// SPDX-License-Identifier: GPL-3.0-or-later
#include "TeamCore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

namespace team {

static bool keyEq(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, 4) == 0; }

// ---- settings ---------------------------------------------------------------
// "/teamcfg": enabled|role|color|status|share|channel|callsign
void Core::saveSettings() {
  char buf[96];
  const int n = snprintf(buf, sizeof buf, "%d|%u|%u|%u|%d|%s|%s", cfg.enabled ? 1 : 0, cfg.role, cfg.color,
                         cfg.status, cfg.share ? 1 : 0, cfg.channel, cfg.callsign);
  io_->writeFile("teamcfg", (const uint8_t*)buf, (size_t)n);
}

void Core::loadSettings() {
  char buf[97];
  size_t n = 0;
  if (!io_->readFile("teamcfg", (uint8_t*)buf, sizeof buf - 1, &n)) return;
  buf[n] = 0;
  char* f[7] = {buf};
  int k = 1;
  for (char* c = buf; *c && k < 7; ++c)
    if (*c == '|') { *c = 0; f[k++] = c + 1; }
  if (k < 7) return;
  cfg.enabled = atoi(f[0]) != 0;
  cfg.role = (uint8_t)atoi(f[1]);
  if (cfg.role > TeamWire::kMaxRoleId) cfg.role = 0;
  cfg.color = (uint8_t)(atoi(f[2]) % TeamWire::kColors);
  cfg.status = (uint8_t)(atoi(f[3]) % TeamWire::kStatuses);
  cfg.share = atoi(f[4]) != 0;
  snprintf(cfg.channel, sizeof cfg.channel, "%s", f[5]);
  snprintf(cfg.callsign, sizeof cfg.callsign, "%s", f[6]);
}

// "/teampins": [len u8][encoded marker] ...
static constexpr size_t kPinsFileMax = kMaxMarkers * 120;

void Core::saveMarkers() {
  uint8_t* buf = (uint8_t*)malloc(kPinsFileMax);   // PSRAM on the device; never on the loop stack
  if (!buf) return;
  size_t n = 0;
  for (int i = 0; i < kMaxMarkers; i++) {
    if (!mused_[i]) continue;
    uint8_t one[TeamWire::kMaxPayload];
    const size_t k = TeamWire::encode(markers_[i], one, sizeof one);
    if (!k || n + 1 + k > kPinsFileMax) continue;
    buf[n++] = (uint8_t)k;
    memcpy(buf + n, one, k);
    n += k;
  }
  io_->writeFile("teampins", buf, n);
  free(buf);
}

void Core::loadMarkers() {
  uint8_t* buf = (uint8_t*)malloc(kPinsFileMax);
  if (!buf) return;
  size_t n = 0;
  if (!io_->readFile("teampins", buf, kPinsFileMax, &n)) { free(buf); return; }
  size_t o = 0;
  int slot = 0;
  while (o < n && slot < kMaxMarkers) {
    const size_t k = buf[o++];
    if (o + k > n) break;
    if (TeamWire::parse(buf + o, k, markers_[slot])) mused_[slot++] = true;
    o += k;
  }
  free(buf);
}

void Core::begin() {
  loadSettings();
  loadMarkers();
  loadRoles();
}

// ---- roles --------------------------------------------------------------------
// "/teamroles": [len u8][encoded role edit] ...
static constexpr size_t kRolesFileMax = (TeamWire::kMaxRoleId + 1) * 40;

void Core::saveRoles() {
  uint8_t* buf = (uint8_t*)malloc(kRolesFileMax);
  if (!buf) return;
  size_t n = 0;
  for (int i = 0; i <= TeamWire::kMaxRoleId; i++) {
    if (!rused_[i]) continue;
    uint8_t one[40];
    const size_t k = TeamWire::encode(roles_[i], one, sizeof one);
    if (!k || n + 1 + k > kRolesFileMax) continue;
    buf[n++] = (uint8_t)k;
    memcpy(buf + n, one, k);
    n += k;
  }
  io_->writeFile("teamroles", buf, n);
  free(buf);
}

void Core::loadRoles() {
  uint8_t* buf = (uint8_t*)malloc(kRolesFileMax);
  if (!buf) return;
  size_t n = 0;
  if (!io_->readFile("teamroles", buf, kRolesFileMax, &n)) { free(buf); return; }
  size_t o = 0;
  while (o < n) {
    const size_t k = buf[o++];
    if (o + k > n) break;
    TeamWire::RoleDef d;
    if (TeamWire::parse(buf + o, k, d)) { roles_[d.role_id] = d; rused_[d.role_id] = true; }
    o += k;
  }
  free(buf);
}

void Core::onRoleDef(const TeamWire::RoleDef& d) {
  if (rused_[d.role_id] && !TeamWire::beats(d, roles_[d.role_id])) return;
  roles_[d.role_id] = d;
  rused_[d.role_id] = true;
  saveRoles();
  io_->changed();
}

const TeamWire::RoleDef* Core::roleEdit(int id) const {
  if (id < 0 || id > TeamWire::kMaxRoleId || !rused_[id] || roles_[id].reset) return nullptr;
  return &roles_[id];
}

bool Core::roleAvailable(int id) const {
  return (id >= 0 && id < TeamWire::kRoles) || roleEdit(id) != nullptr;
}

void Core::roleName(int id, char* out, size_t cap) const {
  const TeamWire::RoleDef* d = roleEdit(id);
  if (d && d->name[0]) snprintf(out, cap, "%s", d->name);
  else if (id >= 0 && id < TeamWire::kRoles) snprintf(out, cap, "%s", TeamWire::roleName(id));
  else snprintf(out, cap, "Role %d", id);
}

char Core::roleLetter(int id) const {
  const TeamWire::RoleDef* d = roleEdit(id);
  if (d)
    for (const char* c = d->name; *c; ++c) {
      if (*c >= 'a' && *c <= 'z') return (char)(*c - 32);
      if ((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) return *c;
    }
  return id < TeamWire::kRoles ? TeamWire::roleLetter(id) : '?';
}

int Core::nextRole(int id) const {
  for (int k = 1; k <= TeamWire::kMaxRoleId + 1; k++) {
    const int c = (id + k) % (TeamWire::kMaxRoleId + 1);
    if (roleAvailable(c)) return c;
  }
  return 0;
}

// ---- sending -----------------------------------------------------------------
bool Core::sendRaw(const uint8_t* p, size_t n) {
  if (!active() || !n) return false;
  return io_->send(cfg.channel, p, n);
}

bool Core::sendPosition() {
  Fix f;
  if (!active() || !io_->fix(f)) return false;
  TeamWire::Position p;
  io_->selfKey4(p.key4);
  p.lat = f.lat;
  p.lon = f.lon;
  p.has_alt = f.has_alt;
  p.alt_m = (int16_t)f.alt_m;
  p.has_motion = f.has_motion;
  p.speed_mps = f.speed_mps;
  p.heading_deg = f.heading_deg;
  p.accuracy_m = f.accuracy_m;
  p.battery_pct = io_->batteryPct();
  p.role = cfg.role;
  p.color = cfg.color;
  p.status = cfg.status;
  p.from_radio = true;
  p.sos = own_active_;
  snprintf(p.callsign, sizeof p.callsign, "%s", callsign());
  uint8_t buf[TeamWire::kMaxPayload];
  const size_t n = TeamWire::encode(p, buf, sizeof buf);
  if (!sendRaw(buf, n)) return false;
  force_beacon_ = false;
  beaconed_ = true;
  last_beacon_ms_ = io_->nowMs();
  last_fix_ = f;
  io_->changed();
  return true;
}

void Core::raiseAlert(uint8_t type, const char* note) {
  Fix f;
  const bool has = io_->fix(f);
  own_ = TeamWire::Alert();
  io_->selfKey4(own_.key4);
  own_.id = (uint16_t)io_->random32();
  own_.active = true;
  own_.type = type;
  own_.has_pos = has;
  own_.lat = has ? f.lat : 0;
  own_.lon = has ? f.lon : 0;
  own_.accuracy_m = has ? f.accuracy_m : 0;
  snprintf(own_.callsign, sizeof own_.callsign, "%s", callsign());
  snprintf(own_.note, sizeof own_.note, "%s", note ? note : "");
  own_active_ = true;
  own_sends_ = 0;
  own_next_ms_ = io_->nowMs();
  cancel_sends_ = 2;   // a cancel still pending is overtaken
  force_beacon_ = true;
  tick();
  io_->changed();
}

void Core::cancelAlert() {
  if (!own_active_) return;
  own_active_ = false;
  cancel_ = own_;
  cancel_.active = false;
  cancel_sends_ = 0;
  cancel_next_ms_ = io_->nowMs();
  own_sends_ = 3;
  force_beacon_ = true;
  tick();
  io_->changed();
}

void Core::tick() {
  const uint32_t now = io_->nowMs();
  if (!active()) return;
  // Own alert: three sends (radio collisions happen), then it rides on every position.
  if (own_active_ && own_sends_ < 3 && (int32_t)(now - own_next_ms_) >= 0) {
    uint8_t buf[TeamWire::kMaxPayload];
    if (sendRaw(buf, TeamWire::encode(own_, buf, sizeof buf))) {
      own_sends_++;
      own_next_ms_ = now + (own_sends_ == 1 ? 15000 : 30000);
    }
  }
  if (cancel_sends_ < 2 && (int32_t)(now - cancel_next_ms_) >= 0) {
    uint8_t buf[TeamWire::kMaxPayload];
    if (sendRaw(buf, TeamWire::encode(cancel_, buf, sizeof buf))) {
      cancel_sends_++;
      cancel_next_ms_ = now + 10000;
    }
  }
  if (!cfg.share) return;
  Fix f;
  if (!io_->fix(f)) return;
  const long since = beaconed_ ? (long)((now - last_beacon_ms_) / 1000) : -1;
  double turn = 0;
  if (beaconed_ && f.has_motion && last_fix_.has_motion) {
    turn = fmod(f.heading_deg - last_fix_.heading_deg + 540.0, 360.0) - 180.0;
  }
  const double moved = beaconed_ ? TeamWire::distanceM(last_fix_.lat, last_fix_.lon, f.lat, f.lon) : 0;
  bool send = force_beacon_ && (since < 0 || since >= 5);
  if (!send) send = TeamWire::shouldBeacon(since, moved, f.has_motion ? f.speed_mps : 0, turn);
  if (!send && own_active_ && since >= 60) send = true;
  if (send) sendPosition();
}

// ---- receiving -----------------------------------------------------------------
void Core::onPayload(const char* channel, const uint8_t* p, size_t n) {
  if (!cfg.enabled || !channel || strcmp(channel, cfg.channel) != 0) return;
  if (n < 5) return;
  uint8_t me[4];
  io_->selfKey4(me);
  if (keyEq(p + 1, me)) return;
  switch (p[0]) {
    case TeamWire::kPosition: { TeamWire::Position x; if (TeamWire::parse(p, n, x)) onPosition(x); break; }
    case TeamWire::kAlert: { TeamWire::Alert x; if (TeamWire::parse(p, n, x)) onAlert(x); break; }
    case TeamWire::kMarker: { TeamWire::Marker x; if (TeamWire::parse(p, n, x)) onMarker(x); break; }
    case TeamWire::kRole: { TeamWire::RoleDef x; if (TeamWire::parse(p, n, x)) onRoleDef(x); break; }
    default: break;   // reports: not on this radio yet
  }
}

void Core::onPosition(const TeamWire::Position& x) {
  int slot = -1;
  for (int i = 0; i < kMaxMembers && slot < 0; i++)
    if (members_[i].used && keyEq(members_[i].pos.key4, x.key4)) slot = i;
  for (int i = 0; i < kMaxMembers && slot < 0; i++)
    if (!members_[i].used) slot = i;
  if (slot < 0) {   // full: the one heard longest ago makes room
    slot = 0;
    for (int i = 1; i < kMaxMembers; i++)
      if ((int32_t)(members_[i].heard_ms - members_[slot].heard_ms) < 0) slot = i;
  }
  members_[slot].used = true;
  members_[slot].pos = x;
  members_[slot].heard_ms = io_->nowMs();
  io_->changed();
}

Fresh Core::freshness(const Member& m) const {
  const uint32_t age = io_->nowMs() - m.heard_ms;
  return age < 12UL * 60000UL ? Fresh::Live : age < 30UL * 60000UL ? Fresh::Stale : Fresh::Lost;
}

int Core::memberCount() const {
  int n = 0;
  for (auto& m : members_) if (m.used) n++;
  return n;
}

void Core::onAlert(const TeamWire::Alert& a) {
  int slot = -1;
  for (int i = 0; i < kMaxAlerts; i++)
    if (alerts_[i].used && keyEq(alerts_[i].a.key4, a.key4) && alerts_[i].a.id == a.id) { slot = i; break; }
  const bool was_active = slot >= 0 && alerts_[slot].a.active;
  if (slot < 0) {
    for (int i = 0; i < kMaxAlerts; i++) if (!alerts_[i].used) { slot = i; break; }
    if (slot < 0) {   // full: reuse the oldest inactive, else the oldest
      int best = 0;
      for (int i = 0; i < kMaxAlerts; i++) {
        if (!alerts_[i].a.active && alerts_[best].a.active) best = i;
        else if (alerts_[i].a.active == alerts_[best].a.active &&
                 (int32_t)(alerts_[i].heard_ms - alerts_[best].heard_ms) < 0) best = i;
      }
      slot = best;
    }
    alerts_[slot] = AlertRec();
  }
  AlertRec& r = alerts_[slot];
  r.used = true;
  r.a = a;
  r.heard_ms = io_->nowMs();
  if (a.active && !was_active) {
    r.acked = false;
    io_->alarm(r);
  }
  io_->changed();
}

AlertRec* Core::firstUnacked() {
  AlertRec* best = nullptr;
  for (auto& r : alerts_)
    if (r.used && r.a.active && !r.acked && (!best || (int32_t)(r.heard_ms - best->heard_ms) > 0)) best = &r;
  return best;
}

bool Core::isMine(const TeamWire::Marker& m) {
  uint8_t me[4];
  io_->selfKey4(me);
  return keyEq(m.key4, me);
}

void Core::onMarker(const TeamWire::Marker& m) {
  int slot = -1, free_slot = -1, oldest = -1;
  for (int i = 0; i < kMaxMarkers; i++) {
    if (!mused_[i]) { if (free_slot < 0) free_slot = i; continue; }
    if (keyEq(markers_[i].key4, m.key4) && markers_[i].id == m.id) { slot = i; break; }
    if (oldest < 0 || markers_[i].deleted > markers_[oldest].deleted ||
        (markers_[i].deleted == markers_[oldest].deleted && markers_[i].time < markers_[oldest].time))
      oldest = i;
  }
  if (slot >= 0) {
    const TeamWire::Marker& cur = markers_[slot];
    if (m.time <= cur.time && cur.deleted == m.deleted) return;   // not newer
  } else {
    slot = free_slot >= 0 ? free_slot : oldest;
    if (slot < 0) return;
  }
  markers_[slot] = m;
  mused_[slot] = true;
  saveMarkers();
  io_->changed();
}

TeamWire::Marker* Core::putMarker(TeamWire::Marker* existing, uint8_t kind, double lat, double lon,
                                  const char* label, const char* note) {
  TeamWire::Marker m;
  if (existing) m = *existing;
  else {
    io_->selfKey4(m.key4);
    m.id = io_->random32();
  }
  m.deleted = false;
  m.kind = kind;
  m.lat = lat;
  m.lon = lon;
  uint32_t t = io_->epoch();
  if (existing && t <= existing->time) t = existing->time + 1;   // newer wins at the other end
  m.time = t;
  snprintf(m.label, sizeof m.label, "%s", label ? label : "");
  snprintf(m.note, sizeof m.note, "%s", note ? note : "");
  onMarker(m);
  uint8_t buf[TeamWire::kMaxPayload];
  sendRaw(buf, TeamWire::encode(m, buf, sizeof buf));
  for (int i = 0; i < kMaxMarkers; i++)
    if (mused_[i] && keyEq(markers_[i].key4, m.key4) && markers_[i].id == m.id) return &markers_[i];
  return nullptr;
}

void Core::deleteMarker(TeamWire::Marker& m) {
  TeamWire::Marker d = m;
  d.deleted = true;
  uint32_t t = io_->epoch();
  if (t <= m.time) t = m.time + 1;
  d.time = t;
  onMarker(d);
  uint8_t buf[TeamWire::kMaxPayload];
  sendRaw(buf, TeamWire::encode(d, buf, sizeof buf));
}

// ---- Bloodhound -------------------------------------------------------------------
void Core::houndMember(const uint8_t key4[4]) {
  hound_ = 1;
  memcpy(hound_key_, key4, 4);
  io_->changed();
}
void Core::houndPoint(double lat, double lon, const char* label) {
  hound_ = 2;
  hound_lat_ = lat;
  hound_lon_ = lon;
  snprintf(hound_label_, sizeof hound_label_, "%s", label ? label : "");
  io_->changed();
}
bool Core::hound(double* lat, double* lon, char* label, size_t cap) {
  if (hound_ == 2) {
    *lat = hound_lat_;
    *lon = hound_lon_;
    snprintf(label, cap, "%s", hound_label_);
    return true;
  }
  if (hound_ == 1) {
    for (auto& m : members_) {
      if (!m.used || !keyEq(m.pos.key4, hound_key_)) continue;
      *lat = m.pos.lat;
      *lon = m.pos.lon;
      snprintf(label, cap, "%s", m.pos.callsign);
      return true;
    }
  }
  return false;
}

}  // namespace team
