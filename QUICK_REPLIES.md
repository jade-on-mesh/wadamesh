# Quick replies

One-tap answers, tied to the thing they answer, so a teammate's SOS or a
watched report gets an answer without typing on the keyboard while walking.
MeshCore Open TAK sends and shows the same replies
(`documentation/quick-replies.md` there).

Not on the T-Display P4: its app space is full
(`CAP_TAK_EXTRAS` in `device_caps.h`).

## Where

| Item | Answers | Where |
| --- | --- | --- |
| A teammate's alert (SOS…) | Copy, On my way, ETA…, Can't help | The red alarm screen (an answer also acknowledges it), or the teammate's sheet while the alert is on |
| A teammate's pin | Seen it, On my way, Need backup, All clear | The pin's sheet on the map |
| A watched report (#icewatch, #dronewatch…) | Seen it, On my way, Need backup, All clear | The report's sheet on the map |

*ETA…* asks for 5, 10, 20 or 30 minutes. One answer per person per item: a new
one replaces the last.

Each sheet lists the answers so far ("Sam: On my way  2m"); your own answer's
button is ringed. While your SOS is on, the Team page lists the team's answers
under the SOS button, and each answer to your alert or pin pops up with a chime.
Team reports from the app aren't shown on standalones yet, so they can't be
answered here.

## On the air

Team items: a team message on the team channel (`TeamWire::Reply`, type
`0x51`): the item's author and id, the answer, and the ETA in minutes.

Watched reports: plain text on the report's own channel, which every MeshCore
client can read:

```
On my way ↩3fa1
```

`3fa1` is the report's id: MeshCore Open TAK's emoji-reaction hash of the
report's time, sender and first five characters (`quickreply::reportHash`).
Standalones and the app fold the answer into that one report, even when a bot
posts many; other clients show the line as written. A watched channel is
usually public, so everyone on it sees the answer and your name.

## Code

| Piece | Where |
| --- | --- |
| Answers, report hash, watched-channel text, the answer book (host-tested) | `src/ui-touch/QuickReply.h`, `test/test_quick_reply.cpp` |
| The team message | `src/ui-touch/TeamWire.h` (`Reply`), `TeamCore::sendReply` |
| Alarm buttons, sheets, Team page, watched channels | `src/ui-touch/TeamUI.inc` (`rqBlock`, `rqSend`) |
