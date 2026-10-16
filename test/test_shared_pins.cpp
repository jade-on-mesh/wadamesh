// Host test for src/ui-touch/SharedPins.h: the app's "m:lat,lon|label|flags"
// pins, parsed and de-duplicated the way lib/screens/map_screen.dart does.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui-touch/SharedPins.h"

using namespace sharedpins;

int main() {
  double la, lo;
  char label[33], flags[9];

  // As the app writes it (formatMarkerText), and inside a channel post.
  assert(parse("m:47.606200,-122.332100|Camp 2|", &la, &lo, label, sizeof label, flags, sizeof flags));
  assert(fabs(la - 47.6062) < 1e-9 && fabs(lo + 122.3321) < 1e-9);
  assert(!strcmp(label, "Camp 2") && !strcmp(flags, ""));
  assert(parse("Alice: look m:1.5,2.5| water |h", &la, &lo, label, sizeof label, flags, sizeof flags));
  assert(la == 1.5 && lo == 2.5 && !strcmp(label, "water") && !strcmp(flags, "h"));

  // Not pins: no second bar, letters in a number, impossible positions.
  assert(!parse("m:47.6,-122.3|no flags", &la, &lo, label, sizeof label, flags, sizeof flags));
  assert(!parse("m:4x.6,-122.3|a|", &la, &lo, label, sizeof label, flags, sizeof flags));
  assert(!parse("m:95,10|a|", &la, &lo, label, sizeof label, flags, sizeof flags));
  assert(!parse("time: 12:00", &la, &lo, label, sizeof label, flags, sizeof flags));
  // A later "m:" can still be the pin.
  assert(parse("rm:x m:3,4|b|", &la, &lo, label, sizeof label, flags, sizeof flags) && la == 3 && lo == 4);

  // Newest first: the same label from the same sender in the same chat is one pin.
  Pin pins[kMaxPins];
  Collector c(pins, kMaxPins);
  c.add("m:10,20|Truck|", "#ops", "Bob", true, false, 300, "Me");
  c.add("m:11,21|Truck|", "#ops", "bob", true, false, 200, "Me");   // older move: dropped
  c.add("m:12,22|Truck|", "#ops", "Carol", true, false, 100, "Me"); // another sender: kept
  c.add("m:13,23|Truck|", "Bob", nullptr, false, false, 90, "Me");  // a DM: kept, from the contact
  c.add("m:14,24|Home|", "Bob", nullptr, false, true, 80, "Jade");  // ours: from our name
  c.add("hello", "#ops", "Bob", true, false, 70, "Me");
  assert(c.n == 4);
  assert(pins[0].lat == 10 && !strcmp(pins[0].from, "Bob"));
  assert(!strcmp(pins[2].from, "Bob") && !pins[2].channel);
  assert(!strcmp(pins[3].from, "Jade") && pins[3].outgoing);

  printf("all shared pin checks pass\n");
  return 0;
}
