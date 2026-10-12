#!/usr/bin/env bash
# Builds and runs test/test_vox_voice.cpp against lib/codec2 with the flags the
# firmware uses (lib/codec2/library.json). Usage: test/build_vox_voice_test.sh [outdir]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$(mktemp -d)}
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
L="$ROOT/lib/codec2"
mkdir -p "$OUT/obj"
for f in "$L"/src/*.c; do
  gcc -std=gnu11 -O2 -g $SAN -D__EMBEDDED__ -DCODEC2_MODE_700C_EN=0 -DGIT_HASH='"vendored"' \
      -I "$L/src" -I "$L/include" -c "$f" -o "$OUT/obj/$(basename "$f").o" &
done
wait
g++ -std=c++17 -O2 -g $SAN -Wall -Wextra -I "$ROOT/src" -I "$L/include" \
    "$ROOT/test/test_vox_voice.cpp" "$ROOT/src/ui-touch/VoxVoice.cpp" "$OUT"/obj/*.o \
    -lm -o "$OUT/test_vox_voice"
"$OUT/test_vox_voice" "$ROOT/test/data/vox"
