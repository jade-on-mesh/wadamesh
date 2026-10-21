// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Following a team route on a standalone radio: where along the route you are,
// the next turn and how far to it, what is left. MeshCore Open TAK shares
// road directions as ordinary team routes (TeamWire::Shape, thinned to fit one
// packet, which keeps the corners), so the turns are read from the route's
// shape: the angle at each vertex. Bends under 30 degrees are not turns.
//
// Platform-neutral; host-tested by test/test_team_route.cpp.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "TeamWire.h"

namespace teamroute {

constexpr int kMax = TeamWire::kMaxShapePoints;
constexpr double kTurnDeg = 30;     // smaller bends are "continue"
constexpr double kArriveM = 25;     // from the end
constexpr double kOffRouteM = 60;

struct Cue {
  int next = 0;              // the vertex being headed for (Bloodhound points at it)
  int turn_at = -1;          // vertex of the next turn, -1: none before the end
  int turn_deg = 0;          // its angle: + right, - left
  double to_next_m = 0;      // along the route to `next`
  double to_turn_m = 0;      // ... to the next turn (or the end, with no turn)
  double remaining_m = 0;    // ... to the end
  double off_m = 0;          // from the route line
  bool arrived = false;
};

class Follower {
 public:
  // The route's points (1e-5 degree grid, as TeamWire::Shape), the first
  // `n` of them, optionally backwards. Starts on the segment nearest (lat, lon)
  // when given a position.
  void start(const int32_t* lat5, const int32_t* lon5, int n, bool reverse, bool have_pos = false, double lat = 0,
             double lon = 0) {
    n_ = n < 0 ? 0 : n > kMax ? kMax : n;
    for (int i = 0; i < n_; i++) {
      const int k = reverse ? n_ - 1 - i : i;
      la_[i] = lat5[k];
      lo_[i] = lon5[k];
    }
    if (!n_) return;
    lat0_ = la_[0] / 1e5;
    cos_ = cos(lat0_ * M_PI / 180.0);
    for (int i = 0; i < n_; i++) project(la_[i] / 1e5, lo_[i] / 1e5, &x_[i], &y_[i]);
    cum_[0] = 0;
    for (int i = 1; i < n_; i++) cum_[i] = cum_[i - 1] + hypot(x_[i] - x_[i - 1], y_[i] - y_[i - 1]);
    seg_ = 0;
    if (have_pos && n_ > 1) {
      double best = 1e18;
      double px, py;
      project(lat, lon, &px, &py);
      for (int i = 0; i + 1 < n_; i++) {
        double t;
        const double d = toSegment(px, py, i, &t);
        if (d < best) { best = d; seg_ = i; }
      }
    }
  }

  int count() const { return n_; }
  double lat(int i) const { return la_[i] / 1e5; }
  double lon(int i) const { return lo_[i] / 1e5; }
  double lengthM() const { return n_ ? cum_[n_ - 1] : 0; }

  // Signed turn at vertex i (+ right, - left), 0 at the ends.
  int turnAt(int i) const {
    if (i <= 0 || i >= n_ - 1) return 0;
    const double a = atan2(x_[i] - x_[i - 1], y_[i] - y_[i - 1]) * 180.0 / M_PI;
    const double b = atan2(x_[i + 1] - x_[i], y_[i + 1] - y_[i]) * 180.0 / M_PI;
    double d = fmod(b - a + 540.0, 360.0) - 180.0;
    if (d < -180) d += 360;
    return (int)lround(d);
  }

  Cue update(double lat, double lon) {
    Cue c;
    if (n_ < 2) { c.arrived = n_ == 1; return c; }
    double px, py;
    project(lat, lon, &px, &py);
    // Near where it was last (a route can come back past itself), else anywhere.
    double best = 1e18, bt = 0;
    int bs = seg_;
    auto scan = [&](int from, int to) {
      for (int i = from < 0 ? 0 : from; i < to && i + 1 < n_; i++) {
        double t;
        const double d = toSegment(px, py, i, &t);
        if (d < best) { best = d; bs = i; bt = t; }
      }
    };
    scan(seg_ - 1, seg_ + 6);
    if (best > kOffRouteM) scan(0, n_);
    seg_ = bs;
    const double along = cum_[bs] + bt * (cum_[bs + 1] - cum_[bs]);
    c.off_m = best;
    c.remaining_m = lengthM() - along > 0 ? lengthM() - along : 0;
    c.next = bs + 1;
    c.to_next_m = cum_[c.next] - along;
    c.turn_at = -1;
    for (int i = bs + 1; i < n_ - 1; i++) {
      const int t = turnAt(i);
      if (abs(t) >= kTurnDeg && cum_[i] - along > 2) { c.turn_at = i; c.turn_deg = t; break; }
    }
    c.to_turn_m = c.turn_at >= 0 ? cum_[c.turn_at] - along : c.remaining_m;
    c.arrived = c.remaining_m <= kArriveM && best <= kOffRouteM;
    return c;
  }

 private:
  int n_ = 0, seg_ = 0;
  int32_t la_[kMax] = {0}, lo_[kMax] = {0};
  double x_[kMax] = {0}, y_[kMax] = {0}, cum_[kMax] = {0};
  double lat0_ = 0, cos_ = 1;

  void project(double lat, double lon, double* x, double* y) const {
    *x = (lon - lo_[0] / 1e5) * 111320.0 * cos_;
    *y = (lat - lat0_) * 110540.0;
  }
  double toSegment(double px, double py, int i, double* t_out) const {
    const double ax = x_[i], ay = y_[i], dx = x_[i + 1] - ax, dy = y_[i + 1] - ay;
    const double len2 = dx * dx + dy * dy;
    double t = len2 == 0 ? 0 : ((px - ax) * dx + (py - ay) * dy) / len2;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    *t_out = t;
    return hypot(px - (ax + t * dx), py - (ay + t * dy));
  }
};

// "Turn left", "Slight right", "Sharp left", "U-turn".
inline const char* turnWord(int deg) {
  const int a = abs(deg);
  if (a < kTurnDeg) return "Continue";
  if (a >= 160) return "U-turn";
  if (deg < 0) return a < 60 ? "Slight left" : a < 120 ? "Turn left" : "Sharp left";
  return a < 60 ? "Slight right" : a < 120 ? "Turn right" : "Sharp right";
}

// "350 m", "1.2 km", "14 km" (as the app's roadDistance).
inline void distanceText(double m, char* out, size_t cap) {
  if (m < 1000) {
    const long r = m < 100 ? lround(m / 10) * 10 : lround(m / 50) * 50;
    snprintf(out, cap, "%ld m", r);
  } else {
    snprintf(out, cap, m >= 10000 ? "%.0f km" : "%.1f km", m / 1000);
  }
}

// The line under the route's name: "Turn left in 150 m · 2.3 km left",
// "Arrive in 80 m", "Arrived", or how far off the route you are.
inline void cueText(const Cue& c, char* out, size_t cap) {
  char a[16], b[16];
  if (c.arrived) { snprintf(out, cap, "%s", "Arrived"); return; }
  if (c.off_m > kOffRouteM) {
    distanceText(c.off_m, a, sizeof a);
    snprintf(out, cap, "%s %s", a, "off the route: head for the arrow");
    return;
  }
  distanceText(c.to_turn_m, a, sizeof a);
  distanceText(c.remaining_m, b, sizeof b);
  if (c.turn_at < 0) snprintf(out, cap, "Arrive in %s", a);
  else snprintf(out, cap, "%s in %s \xC2\xB7 %s left", turnWord(c.turn_deg), a, b);
}

}  // namespace teamroute
