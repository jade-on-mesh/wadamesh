# OTP Messenger

A native drawer app for one-time-pad messages over MeshCore DMs and private
channels. It is a port of Jade's Lua app `OTP_3_RC1.lua` (build
`20261006.109-colors`) and stays wire-compatible with it and with MeshCore Open
TAK's OTP mode: a native radio, a Lua radio and a phone can share one pad.

| file | what |
|---|---|
| `src/ui-touch/OtpCore.{h,cpp}` | the protocol: pads, roles, framing, sync, resync, nacks, retries, storage format, passphrase lock. Platform-neutral. |
| `src/ui-touch/OtpMessenger.{h,cpp}` | glue: the mesh (sends, airtime, repeats), SPIFFS files, PSRAM, hardware RNG |
| `src/ui-touch/OtpMessengerUI.inc` | the screens, included by `UITask.cpp` |
| `src/MyMesh.cpp` | receive hooks: OTP traffic for a conversation with a pad goes to the app and not to Chats |
| `test/test_otp_interop.cpp` | runs `OTP_3_RC1.lua` (on the firmware's own Lua) against the C++ core over a simulated mesh |

## On the air (unchanged from the Lua app)

- **DM**: bare lowercase hex of `framed XOR pad`. Every message is framed as
  `\1MSGCHNK\2` + tid + index + total + text, and each part is acknowledged with
  an encrypted `\1MSGACK\2` + tid + index, so the sender sees *Delivered*.
  Unframed single-shot ciphertext (as other senders may use) is accepted.
- **Channel**: `z2<offset base36>|<hex>`, one packet, sent after a random
  0.6 to 2.2 s so a competing post can claim the offset first.
- **Sync check**: `z3s<ctr>` / `z3S<ctr>` (channel), `z3p<role><mine>:<theirs>` /
  `z3P...` (DM), in the clear.
- **Nack**: `z4C z` collided, `z4Dz` decrypt error, `z4Gz` garbled.

DM pads are split by role: A sends from the front and reads the peer from the
back, B the reverse. The role comes from comparing the two public keys, so both
sides import the same pad unchanged. A channel pad is one counter every member
advances.

## What is different from the Lua app

- Messages are received, acknowledged and stored **with the app closed**; a
  toast names the sender (never the text) and the drawer tile shows unread.
- Conversation **history is kept** across reboots (150 per conversation).
- Pads are generated with the ESP32 hardware RNG, and a fresh pad can be
  **exported to the SD card** for the other device to import.
- The **passphrase lock** encrypts the conversation list and history as well as
  the pads (ENC2: HMAC-SHA256 keystream + truncated tag, the Lua app's scheme).
  Messages that arrive while locked are held in RAM and opened on unlock.
- A DM send checks there is enough pad for every part **before** it starts.
- A multi-part send gives up after 15 unacknowledged retries instead of retrying
  forever.
- The firmware's keyboard, bubbles, confirm dialogs and fonts.
- **Settings > Move pads from the Lua app** reads the Lua app's store and files
  and brings each pad over with its counters (asking for the Lua passphrase if it
  had one), so a conversation carries on in step.

Files live on the internal store as `/otp<kind><fnv32>` (SPIFFS names stop at 31
characters, and hashing keeps contact names out of the listing), plus
`/otpsettings` and `/otptargets`.

## Testing

```sh
test/build_otp_test.sh
```

Checks, against the Lua app itself: DMs both ways (one part and many), sync
checks, channel posts both ways with offsets and repeat counts, a lost channel
post and the automatic resync, mismatched pads producing a nack the Lua app
shows as *Failed: garbled*, roles both ways round, moving a plain and a
passphrase-protected Lua app's pads over, and the native passphrase lock across
a restart.
