# Team routes and turn-by-turn

Standalone radios can follow routes the team shares, with turn cues. The
routes come from MeshCore Open TAK:

- **Road directions.** In the app, start Bloodhound on a teammate or a point,
  tap **Directions**, pick Drive, Walk or Bike, then **Share with team**. The
  app asks a routing server for the route (that needs the internet), thins it
  to fit one mesh packet, keeping its corners, and sends it on the team channel.
- **Routes drawn in the app** (Team › draw a Route), which travel the same way.

Only the phone that plans the route needs a signal. Every standalone radio
and phone on the team channel can follow it from there, with no internet.

## On a standalone radio

**Team › Pins › Routes** lists the routes shared with the team: name,
length, and where each one starts. Tap one:

- **Follow** starts Bloodhound along it. The arrow points to the next corner,
  and the strip shows the next turn: *Turn left in 150 m · 2.3 km left*,
  *Arrive in 80 m*, then *Arrived*. More than 60 m from the line, it says how far
  off the route you are and the arrow leads back.
- **Follow back** is the same route the other way.
- **Map** shows where it starts.

On the map, shared routes are lines in their colour. The one being followed is
thicker and blue. Tap the Bloodhound strip to stop following.

Turns are read from the route's shape: a bend of 30° or more is a turn, and
bigger bends are *Turn* and *Sharp* rather than *Slight*. Street names don't fit
in the packet, so the radio says *Turn left* rather than *Turn left onto Pine
Street*. The phone that planned the route has the full instructions. A long
route is thinned harder to fit, so a very long drive is best shared in legs.

Routes are kept across reboots (eight at most; the oldest goes first). A route
its author deletes goes from every standalone radio and phone too.

## How

- Wire: the app's shape message (`0x44`, `TeamWire::Shape`), byte for byte.
  Points sit on a 1e-5 degree grid, and the golden bytes are shared with the
  app's `test/team/team_wire_golden_test.dart`.
- `TeamCore` keeps routes as they arrived (`/teamroutes`), and ignores other
  shapes for now.
- `TeamRoute.h` (`teamroute::Follower`) finds where you are along the route,
  the next corner, the next turn and what is left.
- Host test: `test/test_team_wire.cpp`.
