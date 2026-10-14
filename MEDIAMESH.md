# MediaMesh: photos and voice on a second frequency

MediaMesh moves the photos and voice clips that
[MeshCore Open TAK](https://github.com/jade-on-mesh/meshcore-open-tak) sends onto
their own LoRa channel (`radio2`). Text, adverts, ACKs and everything else stay
on the main mesh. The same radio listens to both, so a node with MediaMesh on
still hears the main mesh and can be used with stock repeaters.

It follows the `radio2` design in
[mikecarper/MeshCore](https://github.com/mikecarper/MeshCore/blob/keymindCascade/docs/radio_profiles.md),
including its command names, so an app can drive either firmware the same way.

## Turning it on

**Settings → Radio → MediaMesh Settings**, under the main Radio card:

- **Photos & voice on MediaMesh**: on = listen on radio2 and send media there.
  Off = radio2 off; media goes back to the main mesh.
- **Frequency, Bandwidth, SF, CR**: the MediaMesh radio. A field applies when you
  leave it. It must differ from the main radio.
- **Listen main / media**: how long each scan visit lasts.
- **MediaMesh preamble**: the preamble this node sends media with (see below).

Default settings: 916.000 MHz, 250 kHz, SF7, CR 4/5. That is about 4× faster than
SF7/62.5 for roughly 6 dB less range. Everyone exchanging media has to use the
same MediaMesh settings.

## From an app (companion protocol)

The radio answers the local CLI frame `CMD_RUN_CLI_COMMAND` (`0x42`) with
`RESP_CODE_CLI_REPLY` (`0x1D`), with the same command text as mikecarper/MeshCore:

```text
get radio2                       > 916.000,250.00,7,5,rxtx    (or "> off")
set radio2 916.0,250,7,5,rxtx    OK - radio2 916.000,250.00,7,5,rxtx
set radio2 916.0,250,7,5,rx      listen only
set radio2 916.0,250,7,5,rxtx,72 with an explicit preamble (symbols)
set radio2 off
get tx.media                     > radio2
set tx.media radio|radio2        where photos and voice are sent
get radio2.timing                > dwell=9.4,21.7ms; preamble=32,64; need=32,64; switch=840us
get mediamesh                    summary, including "unsupported on this radio"
set radio2.cross auto|on|off     accepted; a companion does not forward between profiles
```

A two-character correlation tag is echoed: `ab|get radio2` → `ab|> off`.
Unknown text answers `Unknown command`.

## What goes where

Media means the packets that carry a photo or voice clip:

| Packet | Sent on |
| --- | --- |
| Raw data from the app (`CMD_SEND_RAW_DATA`): direct photo/voice transfers and fetch requests | radio2 |
| Channel data type `0x5643` from the app or the on-device photo fetch | radio2 |
| `VI1:` / `VE3:` announcements, `VR3:` fetch DMs, all other text | main |
| Team positions (`0x5654`) and every other channel-data type | main |
| Retries of a media packet | radio2 (same packet hash) |
| Anything this node forwards with Client repeat | main |

The firmware marks each media packet by its MeshCore packet hash when it is
created; the radio looks it up as the packet reaches the transmitter. Everything
received on radio2 is delivered exactly like main-mesh traffic.

**Both ends need MediaMesh.** A photo announced on the main mesh is fetched over
radio2, so a receiver without MediaMesh (or with different MediaMesh settings)
can see the announcement but not download it. The `VR3:` DM fetch still reaches
the sender, but its reply goes out on radio2.

## How the scan works

One SX1262 alternates between the two profiles. The timing is
mikecarper/MeshCore's, recomputed for the actual settings:

- The **slower** profile (longer LoRa symbol, normally the main mesh) gets a
  4.6-symbol visit. The faster one gets the rest of half the slow preamble,
  minus two retunes. With SF7/62.5 main and SF7/500 MediaMesh: 9.4 ms main,
  21.7 ms MediaMesh.
- The main mesh's preamble is fixed by stock nodes (MeshCore: 32 symbols at
  SF≤8, 16 above), so MediaMesh only fits main presets where half that preamble
  covers both visits. `get radio2.timing` warns otherwise.
- MediaMesh packets are sent with a longer preamble that covers a receiver's
  time on the main profile plus 16 symbols to lock on: 64 symbols for SF7/500,
  40 for SF8/500, 32 for SF9/500 (rounded up to 8, capped at 128 when automatic).
- Retune time is measured on the first hops and padded by 10%; until then
  1.5 ms is assumed.
- The receiver stays on a profile while a packet is arriving there, and while it
  transmits. A packet on one profile means the other is deaf until it ends.
  Heavy media traffic therefore costs main-mesh reception: a 255-byte SF7/250
  packet is ~0.2 s.
- Buffered receive is switched on while radio2 is active (its drain task lifts
  each packet out within ~1 ms so the scan can move on), and goes back to your
  setting when radio2 is off.

## Supported radios

SX1262/SX1268 boards whose DIO1 reaches a GPIO: T-Deck, T-Deck Pro/Max, Heltec V4,
T-LoRa Pager (SX1262), Wio Tracker L2, CrowPanel 3.5, Attaky, RAK WisMesh Tap V2.
LR1110/LR1121 boards (ThinkNode M9, T-LoRa Pager LR1121), the T-Display P4
(polled DIO1) and Tanmatsu show "This radio can't run MediaMesh", and answer
`set radio2` with an error.

## Limits

- One transceiver: this is a scan, not two receivers. Expect some loss on both
  profiles, as with mikecarper's dual-profile bench results (197/200 packets).
- Listen-before-talk checks the profile the radio is on at that moment, which may
  not be the one it is about to send on.
- Settings are stored separately from the node preferences (NVS key `mediamesh`)
  and are not in the settings backup yet.

## Code

| Piece | Where |
| --- | --- |
| Settings, CLI, scan timing, media marker (host-tested) | `src/helpers/MediaMeshCore.h`, `test/test_mediamesh_core.cpp` |
| Scanning radio wrapper | `src/helpers/MediaMeshRadio.h` (variants declare `RADIO_DRIVER_CLASS radio_driver`) |
| Storage, apply, CLI glue | `src/helpers/MediaMesh.{h,cpp}` |
| Media marking, `CMD_RUN_CLI_COMMAND` | `src/MyMesh.cpp`, `src/MyMesh.h` |
| MediaMesh Settings card | `src/ui-touch/UITask.cpp` (`buildMediaMeshCard`) |
