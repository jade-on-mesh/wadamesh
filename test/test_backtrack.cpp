// SPDX-License-Identifier: GPL-3.0-or-later
//
// Back-track (BACKTRACK.md): a recorded track thinned to fit and followed back
// to its start with turn cues (src/ui-touch/TeamRoute.h).

#include <assert.h>
#include <stdio.h>

#include "ui-touch/TeamRoute.h"

int main() {
  // An L: 600 points north (about 670 m), then 600 points east, with GPS jitter.
  static int32_t la[1200], lo[1200];
  int n = 0;
  for (int i = 0; i < 600; i++, n++) { la[n] = 4760000 + i; lo[n] = -12230000 + ((i * 7) % 3) - 1; }
  for (int i = 0; i < 600; i++, n++) { la[n] = 4760600 + ((i * 5) % 3) - 1; lo[n] = -12230000 + i; }
  int32_t ola[teamroute::kMax], olo[teamroute::kMax];
  const int k = teamroute::simplifyTrack(la, lo, n, ola, olo, teamroute::kMax);
  assert(k >= 3 && k <= teamroute::kMax);
  assert(ola[0] == la[0] && olo[k - 1] == lo[n - 1]);   // both ends kept
  // The corner is kept (within a few metres).
  bool corner = false;
  for (int i = 0; i < k; i++) corner |= (ola[i] >= 4760595 && olo[i] <= -12229995 && olo[i] >= -12230005);
  assert(corner);

  // Followed back from the far end: first heading west, then a left turn south.
  teamroute::Follower f;
  f.start(ola, olo, k, true, true, 4760600 / 1e5, (-12230000 + 599) / 1e5);
  const teamroute::Cue c = f.update(4760600 / 1e5, (-12230000 + 590) / 1e5);
  assert(!c.arrived);
  assert(c.turn_at >= 0 && c.turn_deg < -60);   // turn left at the corner
  assert(c.remaining_m > 1100 && c.remaining_m < 1400);
  // Short tracks are copied as they are.
  assert(teamroute::simplifyTrack(la, lo, 50, ola, olo, teamroute::kMax) == 50 && ola[49] == la[49]);
  printf("backtrack: ok (%d points)\n", k);
  return 0;
}
