# Walkie-talkie (push to talk)

Hold a button and talk: everyone on the channel with MediaMesh hears you about
two seconds later, while you're still talking, on standalones and in MeshCore
Open TAK alike. When you let go, the talk is posted in the chat as an ordinary
voice message, so anyone who missed it can play it later.

## Using it

1. Turn on MediaMesh (radio2 on rx+tx, photos and voice on radio2): talks go
   out on radio2 only, like voice messages. See [MEDIAMESH.md](MEDIAMESH.md).
2. Long-press a channel in Chats and pick **Walkie-talkie**.
3. **Hold to talk**, and let go to finish. A talk can be up to 20 seconds.

While the walkie-talkie is open, talks on that channel play as they arrive, and
the card shows who is talking. While someone else is talking, the button waits
until they finish. It's half duplex: nothing plays while you talk.

- **Reach** switches between *through repeaters* (the default: the whole
  MediaMesh hears it live, 2-3 s behind) and *direct* (radio to radio: only
  radios in range hear it live, a little sooner, and repeaters stay quiet).
  People out of range still get the voice message.
- **Keep listening when closed** plays that channel's talks as they come even
  with the card closed. One channel at a time.

A talk heard live in full is kept with its voice message, so the message plays
straight away with nothing to download.

Talking needs a microphone (T-Deck, T-Display P4). Boards with a speaker but no
microphone (the T-LoRa Pager, the Tanmatsu) can listen.

## On the air

Talk packets are channel data (GRP_DATA) of type `0x5643`, the same as MeshCore
Open TAK's channel photos and voice, so MediaMesh routing sends them on radio2
only (`markTransfer`):

| Packet | Bytes |
| --- | --- |
| Talk start | `[0x53][session id: 4, big-endian][mode: 1][name: UTF-8, up to 32]` |
| Talk audio | `[0x54][session id: 4][index: 1][flags: 1][Codec2 frames]` |

- `flags`: bit 7 marks the talk's last packet (which may carry no audio); bits
  0-4 are the VE3 mode (1 = Codec2 1200).
- Audio packet *i* is exactly packet *i* of the talk's VE3 clip: 156 bytes (26
  frames, 1.04 s), fewer in the last.
- After the last packet the talk is posted as `VE3:<id>:1:<packets>:<seconds>`
  on the channel, with the same session id, and served like any recording.

Packets are spaced like a photo transfer: through repeaters at least four
packet airtimes plus 150 ms (800 ms at least), so a packet doesn't talk over
the relays of the one before; direct, one airtime plus 60 ms. At the 250 kHz
MediaMesh preset a packet is about 0.16 s on air, so speech keeps up. At 125
kHz, talks through repeaters fall a little behind the longer they run; use
direct there.

Listeners wait half a second after the first packet before playing, play
packets in order, and skip one that hasn't come 0.4 s after it was needed. A
talk with nothing heard for 3.5 s has ended.

## Code

| What | Where |
| --- | --- |
| Packets and the jitter buffer (host-tested) | `src/ui-touch/WalkieWire.h`, `test/test_walkie.cpp` |
| The card, talking, live playback | `src/ui-touch/WalkieUI.inc` |
| Mic bytes while recording | `VoxMic::liveBits` (`src/ui-touch/VoxMic.cpp`) |
| Keeping a talk heard in full | `VoxPhotos::storeHeard` |
| From the mesh | `MyMesh::onChannelDataRecv` -> `walkieOnChannelData` |

The golden bytes in `test/test_walkie.cpp` are the ones MeshCore Open TAK's
`test/vox/walkie_wire_test.dart` checks.
