# Back-track

*Map › options › Follow my track back* turns the track recorded this session
(*Record track*, on the same screen) into a route and follows it from where you
are back to where it began, with the same guidance as following a shared team
route: the Bloodhound arrow points at the next corner, and the strip says
"Turn left in 150 m · 2.3 km left", how far off the track you are, and
"Arrived" at the start.

The track is thinned to the 120 points a route holds: the corners stay, the
straight stretches lose their in-between points (bends under 30° are "continue").
Stop it from the Bloodhound strip like any other route.

It needs a recorded track from this session (at least two points); after a
restart, the earlier track is still in its GPX file on the card, but isn't
followed from there yet.

## Code

| Piece | Where |
| --- | --- |
| Thinning a track (Douglas-Peucker) | `teamroute::simplifyTrack` in `src/ui-touch/TeamRoute.h`, tested by `test/test_backtrack.cpp` |
| Following it back | `team::Core::followTrack` (`TeamCore.cpp`), `teamFollowTrack` (`TeamUI.inc`) |
| The button | `mapOptBacktrackCb` in `src/ui-touch/UITask.cpp` |
