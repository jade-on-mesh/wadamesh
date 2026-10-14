#!/usr/bin/env bash
# Builds and runs test/test_vox_record.cpp (recording voice clips on the radio)
# against lib/codec2 with the flags the firmware uses (lib/codec2/library.json).
#
#   test/build_vox_record_test.sh [outdir]
#   test/build_vox_record_test.sh --regen <meshcore-open-tak/third_party/codec2> [outdir]
#
# --regen re-creates test/data/vox/encode_1200.c2 by encoding test/vox_synth.h's
# signal with the app's own Codec2 tree, built the way the app builds it
# (android/app/src/main/cpp/codec2_sources.cmake), then runs the test.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REF=""
if [[ "${1:-}" == "--regen" ]]; then REF=${2:?usage: --regen <codec2 tree>}; shift 2; fi
OUT=${1:-$(mktemp -d)}
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
L="$ROOT/lib/codec2"

if [[ -n "$REF" ]]; then
  mkdir -p "$OUT/ref"
  SRCS="dump lpc nlp postfilter sine codec2 codec2_fft kiss_fft kiss_fftr interp lsp mbest newamp1
        phase quantise pack codebook codebookd codebookjmv codebookge codebooknewamp1
        codebooknewamp1_energy codebooknewamp2 codebooknewamp2_energy"
  for s in $SRCS; do
    gcc -std=gnu11 -O2 -DGIT_HASH='"Unknown"' -I "$REF/src" -I "$REF/include" -c "$REF/src/$s.c" -o "$OUT/ref/$s.o" &
  done
  wait
  gcc -std=gnu11 -O2 -I "$REF/src" -I "$REF/include" -I "$ROOT/test" "$ROOT/test/vox_ref_encode.c" "$OUT"/ref/*.o -lm \
      -o "$OUT/vox_ref_encode"
  "$OUT/vox_ref_encode" 5 "$ROOT/test/data/vox/encode_1200.c2"   # CODEC2_MODE_1200
  echo "regenerated test/data/vox/encode_1200.c2 from $REF"
fi

mkdir -p "$OUT/obj"
for f in "$L"/src/*.c; do
  gcc -std=gnu11 -O2 -g $SAN -D__EMBEDDED__ -DCODEC2_MODE_700C_EN=0 -DGIT_HASH='"vendored"' \
      -I "$L/src" -I "$L/include" -c "$f" -o "$OUT/obj/$(basename "$f").o" &
done
wait
g++ -std=c++17 -O2 -g $SAN -Wall -Wextra -I "$ROOT/src" -I "$ROOT/test" -I "$L/include" \
    "$ROOT/test/test_vox_record.cpp" "$ROOT/src/ui-touch/VoxVoice.cpp" "$OUT"/obj/*.o \
    -lm -o "$OUT/test_vox_record"
"$OUT/test_vox_record" "$ROOT/test/data/vox"

# How much stack Codec2 needs (no sanitizers: they inflate stack frames).
mkdir -p "$OUT/stack"
gcc -std=gnu11 -O2 -D__EMBEDDED__ -DCODEC2_MODE_700C_EN=0 -DGIT_HASH='"vendored"' \
    -I "$L/src" -I "$L/include" "$L"/src/*.c "$ROOT/test/test_codec2_stack.c" -lm -lpthread \
    -o "$OUT/stack/test_codec2_stack"
"$OUT/stack/test_codec2_stack"
