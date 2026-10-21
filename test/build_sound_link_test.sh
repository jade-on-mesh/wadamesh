#!/usr/bin/env bash
# Builds and runs test/test_sound_link.cpp against lib/ggwave with the radios'
# recording cap (lib/ggwave/library.json). Usage: test/build_sound_link_test.sh [outdir]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$(mktemp -d)}
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
G="$ROOT/lib/ggwave"
DEFS="-DGGWAVE_MAX_RECORDED_FRAMES=640"
mkdir -p "$OUT"
g++ -std=c++17 -O2 -g $SAN -w $DEFS -I "$G/include" -c "$G/src/ggwave.cpp" -o "$OUT/ggwave.o"
g++ -std=c++17 -O2 -g $SAN -Wall -Wextra -Werror $DEFS -I "$ROOT/src" -I "$G/include" \
    "$ROOT/test/test_sound_link.cpp" "$OUT/ggwave.o" -lm -o "$OUT/test_sound_link"
"$OUT/test_sound_link"
