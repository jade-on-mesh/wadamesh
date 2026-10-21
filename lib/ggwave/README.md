# ggwave (vendored)

Data over sound by Georgi Gerganov, MIT licence (LICENSE; the bundled
Reed-Solomon code is MIT too, src/reed-solomon/LICENSE). From
https://github.com/ggerganov/ggwave at 060aec7, only the library: src/ggwave.cpp,
src/fft.h, src/reed-solomon/, include/ggwave/ggwave.h. It is the same copy
MeshCore Open TAK carries in third_party/ggwave.

Used by Sound link (src/ui-touch/SoundLinkUI.inc, SOUND_LINK.md).

Four small Wadamesh changes, each marked "Wadamesh" in the source:

- `GGWAVE_MAX_RECORDED_FRAMES` sets `kMaxRecordedFrames` (2048 upstream).
  The radios listen to the audible protocols only, and a 140-byte message in
  the slowest of them is 626 frames, so library.json sets 640: a listener
  then needs about 1.4 MB rather than 4 MB.
- A receive never records more frames than that, whatever protocols are on.
- On ESP-IDF the instance's buffer comes from PSRAM (`GGWAVE_CALLOC`), and
  when it can't be had the instance says so (`heapSize()` is 0) rather than
  writing through a null pointer.
