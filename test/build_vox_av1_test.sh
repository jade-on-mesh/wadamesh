#!/usr/bin/env bash
# Builds and runs test/test_vox_av1.cpp against the vendored dav1d, with the
# same sources the firmware compiles. Usage: test/build_vox_av1_test.sh [outdir]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$(mktemp -d)}
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
L="$ROOT/lib/dav1d"
mkdir -p "$OUT/obj"
for f in $(ls "$L"/src/*.c | grep -v '_tmpl\.c$') "$L"/src/8bpc/*.c; do
  gcc -std=gnu99 -O1 -g $SAN -DNDEBUG -I "$L" -I "$L/include" -I "$L/gen" \
      -c "$f" -o "$OUT/obj/$(basename "$(dirname "$f")")_$(basename "$f").o" &
done
wait
g++ -std=c++17 -O1 -g $SAN -Wall -Wextra -I "$ROOT/src" -I "$L/include" \
    "$ROOT/test/test_vox_av1.cpp" "$ROOT/src/ui-touch/VoxAv1Core.cpp" "$OUT"/obj/*.o \
    -lpthread -o "$OUT/test_vox_av1"
"$OUT/test_vox_av1" "$ROOT/test/data/vox"
