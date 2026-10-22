# Who got it

Open one of your channel messages' info (tap the message): besides *Repeats
heard* and the repeaters that re-sent it, it now lists the **teammates who
heard it**, and *Show route* draws them on the map's star with the repeaters,
each as a line from you. MeshCore Open TAK shows the same (tap a sent channel
message there).

## How a teammate says they heard it

With team mode on, every position beacon also carries (position flag `0x40`,
after the callsign and the added-role byte, if any):

```
[nameHash u16][n u8 (0-4)][heard u16 x n]
```

- `nameHash`: the low 16 bits of this radio name's string hash (the Dart VM
  string hash MeshCore Open TAK's reactions use), so teammates can tell which
  channel messages are from a teammate.
- `heard`: the reaction hashes (time, sender, first five characters) of the
  last four teammates' channel messages this radio heard, on any channel.

A standalone that hears a teammate's message sends its next beacon 15–45 s
later (at most one extra every 2 minutes), so the sender sees it soon. A
message counts as a teammate's when its sender's name hash matches a
teammate's beacon, or when it is on the team channel. Older firmware and app
versions don't send this, so they never show as having heard.

## Code

| Piece | Where |
| --- | --- |
| The beacon fields | `src/ui-touch/TeamWire.h` (`Position`), golden bytes in `test/test_team_wire.cpp` (the app's `test/team/message_reach_test.dart` checks the same) |
| What was heard, what teammates heard, the extra beacon | `TeamCore::noteChannelText`, `heardBy`, `onPosition` |
| Message info and the map | `buildRouteFromMessage` and the message info in `src/ui-touch/UITask.cpp`; `teamHeardBy` in `TeamUI.inc` |
