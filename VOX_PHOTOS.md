# MeshCore Open TAK photos on wadamesh

Work in progress on the `vox-images` branch: showing the photo messages that
[MeshCore Open TAK](https://github.com/jade-on-mesh/meshcore-open-tak) sends,
on the radio itself.

## How the app sends a photo

A photo is announced with an ordinary text message,
`VI1:{sid}:14:{packets}:{aspect}` (base36), and its bytes are only sent when a
receiver asks for them: direct raw-data fetch first, then a `VR3:` DM, then a
GRP_DATA flood (type `0x5643`) on the channel the photo was posted in. The
reassembled payload is `[0x01][AV1 OBUs]`: one 8-bit 4:2:0 key frame with a
reduced still-picture header and no AVIF container, 2,750 B ("Standard") or
5,500 B ("More detail"). Wire details: `src/ui-touch/VoxWire.h`.

## Pieces

| Piece | Where | State |
|---|---|---|
| Channel data reaches apps and the UI | `MyMesh::onChannelDataRecv` | done |
| Wire format: envelope, packets, fetch requests, reassembly | `src/ui-touch/VoxWire.h`, `test/test_vox_wire.cpp` | done |
| AV1 decoder (dav1d 1.5.1, 8-bit, no asm) | `lib/dav1d`, `scripts/vendor_dav1d.sh` | done |
| Decode + exact Open TAK colour conversion | `src/ui-touch/VoxAv1Core.*`, `test/test_vox_av1.cpp` | done |
| Background decode task | `src/ui-touch/VoxAv1Job.*` | done, verified on a T-Deck |
| `.vi1` files open in Files → image viewer | `UITask.cpp` (`fmVoxPhotoStart`) | done, for testing |
| Fetch client (request, retry, cache) | | next |
| Photo bubble in chat | | after that |

## The decoder on the device

- dav1d is vendored unmodified apart from `lib/dav1d/patches`:
  - every allocation goes through `dav1d_ext_*`, PSRAM first;
  - ~221 KB of tables upstream keeps in `.bss` (inter-prediction masks,
    quantizer matrices) move to the heap;
  - the biggest DSP scratch buffers (self-guided loop filter, inverse
    transform, motion compensation, up to 34 KB each) move from the stack to
    one 40 KB PSRAM arena.

  Tables and arena are allocated by the first decode and kept for the next.
- Each decode runs on its own task on core 0 at idle priority, with a 24 KB
  stack taken from internal RAM only while it runs (this Arduino build can't
  put task stacks in PSRAM). The host measures ~11 KB of stack at worst,
  including damaged input. Boards idling at 80 MHz are raised to 240 MHz for
  the decode. The first device build used a 64 KB stack, which the T-Deck
  couldn't spare: every photo failed with "Not enough memory".
- Heap per decode, measured on the host: up to ~0.9 MB including the RGB565
  output, plus the 221 KB of kept tables.
- Output is bit-exact with libaom's decode in the app (`vox_av1.c`), converted
  with the same full-range BT.601 formulas, then packed to RGB565.
- Flash: the decoder is ~285 KB of code at `-O2` (~255 KB at `-Os`). With it,
  CI's T-Deck `firmware.bin` is 3,855,984 B and the M9's 3,783,184 B, against
  4,063,232 B app slots: about 200 KB and 280 KB left.
- Tanmatsu and T-Display P4 builds don't link dav1d yet; `VOX_AV1_AVAILABLE` is
  0 there and opening a photo says it isn't supported.

## Testing on a device

1. Flash the build from this branch (CI uploads `firmware.bin` for the T-Deck
   and ThinkNode M9: the app-only image, for Launcher or `0x10000`).
2. Copy a few files from `test/data/vox/` to the SD card or internal storage
   (USB Files works). They are real Open TAK payloads of public-domain photos
   (NASA, SpaceX, and CC0 photos from scikit-image), at both detail levels.
3. Open one in **Files**. "Decoding photo..." appears, then the viewer, then a
   note with the decode time, clock, stack and heap used. The same line goes
   to the serial log as `[VOX] decode ...`. A failure shows its reason and code
   with the largest free internal-RAM block and free PSRAM at the time.

Worth trying `coffee_5500.vi1` and `astronaut_5500.vi1`: they take the deepest
stack path.

## Measured on a T-Deck (2026-10-11, build `d3c550d`)

| Photo | Size | Decode | Stack | Heap |
|---|---|---|---|---|
| `astronaut_5500.vi1` | 256×256 | 3,408 ms | 7 KB | 853 KB |
| `coffee_5500.vi1` | 384×256 | 3,057 ms | 7 KB | 729 KB |
| `hubble_deep_field` (both detail levels) | 384×335 | 2,851–3,277 ms | 5 KB | 883–886 KB |
| `rocket_*.vi1` | 320×214 | 1,928 ms | 5 KB | 669 KB |

All at 240 MHz. Colours and geometry look right. Peak stack is 7 KB of the
24 KB reserved. Decode takes 2–3.5 s, small next to the minutes a photo takes
to arrive over LoRa. Images using the self-guided loop filter (astronaut) are
slowest; that filter's scratch is in PSRAM, so trying the arena in internal RAM
first is an easy speed experiment.
