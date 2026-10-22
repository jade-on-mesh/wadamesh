# MeshCore TEAM

[MeshCore TEAM](https://github.com/tmacinc/MeshCore-TEAM) is an app that tracks a group on a private channel. The Team app on Wadamesh TAK standalones understands it, the same as MeshCore Open TAK does: set the standalone's team channel to the **same private channel** as TEAM's tracking channel and each side sees the other. Nothing changes in how the Team app looks. TEAM users show up as ordinary members and pins.

## What you see from TEAM users

- **Positions.** Each TEAM user is a team member on the map and in the Team list. Their callsign is the name they set in TEAM (their alias), or their radio name until that arrives. The battery shows as a percent, from the radio's voltage (or the phone's when that's all they send). A radio tracking on its own GPS, with no phone, counts as a fix from the radio.
- **Waypoints** become pins, labelled with their name, with their description as the note:

  | TEAM | Pin |
  | --- | --- |
  | Camp | Staging / command post |
  | Meetup | Last known point |
  | Danger | Hazard |
  | Water | Water / resources |
  | Game area, deer stand, vehicle, custom | Point |

- **Routes** become team routes (Bloodhound can follow them), in the team colour nearest theirs. A long route is thinned to fit one team route.

TEAM's tracking messages (`#TEL:`, `#T:`, `#CAP:`, `#WAY:`, `#WRC:`) never show up as chat, on any channel.

## What TEAM users see from this standalone

Once a TEAM user has been heard on the team channel, and for 12 hours after (since the last reboot), the standalone also speaks TEAM's formats there:

- With each position beacon (at most every 15 seconds), a `#TEL:` with its position and battery.
- A `#CAP:` with its callsign as the alias and its radio key, so TEAM shows the callsign: a few seconds after TEAM is first heard, when the callsign changes, and every 6 hours.
- Each new or moved pin as a TEAM waypoint (kinds mapped back as in the table, others as Custom).
- An advert when a TEAM user asks for ours (`#CAP:R:`), at most every 10 minutes.

Before a TEAM user has been heard, none of this goes on the air. Deleting a pin doesn't reach TEAM (TEAM has no way to say it).

## On the air

TEAM sends channel text where our Team sends binary channel data:

| Message | Format |
| --- | --- |
| Position | `#TEL:` + unpadded base64 of 11 bytes: latitude and longitude (int32 big-endian, degrees × 10⁷), radio battery, phone battery, forwarding byte. Battery bytes: 0/1 unknown, else `(mV − 2750) / 6 + 2`, up to 254; a phone battery of 0xFF marks a radio on its own GPS. |
| Position + neighbours | `#T:` + unpadded base64: the same first 10 bytes, a node count, a neighbour bitmap (read for the position and battery only) |
| Alias | `#CAP:2:<flags hex>:<radio key, 12 hex>:<app id, 16 hex or ->:<alias>` (alias last, up to 24 bytes); `#CAP:1:<flags>` from older versions |
| Advert request | `#CAP:R:<key prefix or ->:<radio name>` |
| Waypoint or route | `#WAY:<id>\|<name>\|<lat>\|<lon>\|<description>\|<type>\|<route>`, the route as `lat,lon~lat,lon…` and its colour as an `@C:AARRGGBB` prefix on the description; a long route continues in `#WRC:<id>\|<chunk>\|<n>/<N>` parts. Messages stay within 140 bytes with the radio name in front. |

A TEAM sender is known by its radio name (channel messages carry no key). Its `#CAP` gives the first bytes of its radio key, which is what its position is filed under; before that, a contact with the same name, or failing that a number made from the name (the app does the same, so both file it alike).

## Code

| What | Where |
| --- | --- |
| The formats (host-tested) | `src/ui-touch/TeamCompat.h`, `test/test_team_compat.cpp` (golden strings shared with MeshCore Open TAK's `test/team/team_compat_test.dart`) |
| Hearing and speaking them | `team::Core::onText` and the `compat*` functions in `src/ui-touch/TeamCore.cpp` |
| From the mesh | `MyMesh::onChannelMessageRecv` -> `teamCompatOnChannelText` (`TeamUI.inc`) |

Written from TEAM's wire formats; TEAM's code (CC BY-NC-SA) isn't used.
