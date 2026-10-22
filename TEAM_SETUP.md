# Sharing a team in one scan

Getting someone onto your team means sharing the team channel, picking it as their team channel, adding the channels you watch, and setting MediaMesh to the right frequency. **Share team setup** puts all of that in one QR code, and plays it by Sound link for a standalone, which has no camera to scan with.

## Sharing

Team › Setup › **Share team setup (QR / sound)** (once a team channel is picked) shows a QR code with:

- the **team channel** (its name and secret),
- the **channels you watch** (hashtag channels by name, private ones with their secret),
- the **MediaMesh profile** (frequency, bandwidth, SF, CR), when radio2 is on.

A phone running MeshCore Open TAK scans it (Team › Setup › Join a team). **Play by sound** plays it for Sound link on another standalone or a phone: a longer setup goes as two or three short parts, one after the other. Anyone who scans or hears it can join your team channel and see your positions, so mind where you show or play it.

Callsigns aren't in it (each person picks their own), and neither are roles: the team's role names come over the team channel once you've joined.

## Joining on a standalone

Team › Setup › **Join a team by sound** listens for two minutes (Sound link's own listening works too). Play the team setup on the other phone or standalone. Once every part is heard, a dialog shows what joining does: add the channels (the team channel and the watched ones) if this standalone hasn't them, turn team mode on with the team channel, watch the watched channels, and set radio2 to the team's MediaMesh. **Join** does it all.

Sound link is on the T-Deck and the T-Display P4 (the boards with a microphone). Other standalones show and play nothing by sound, but still show the QR code for a phone.

## The code

```
meshcore://team/join?n=<team channel>&s=<32 hex>[&w=<watched>,...][&m=<MHz × 1000>,<bandwidth Hz>,<sf>,<cr>]
```

A watched hashtag channel is its name (`%23icewatch`), a private one `name:<32 hex>`; names are percent-encoded, so `,` and `:` only separate. Over sound, a link longer than one 140-byte message goes as `MCT:<id>:<i>/<n>:<piece>` parts.

| What | Where |
| --- | --- |
| The link and the parts (host-tested) | `src/ui-touch/TeamSetup.h`, `test/test_team_setup.cpp` (golden strings shared with MeshCore Open TAK's `test/team/team_setup_test.dart`) |
| Share and join | `openShareSetup`, `teamSetupHeard` and `joinApply` in `src/ui-touch/TeamUI.inc` |
| Hearing the parts | `handleHeard` in `src/ui-touch/SoundLinkUI.inc` |
