#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Renders a chat message with colour emoji, before and after src/ui-touch/EmojiFit.h,
# at the 12 and 14 px text sizes, and writes a PNG (2x) for review.
#
# Usage: test/emoji_fit_render/run.sh [out.png] [path/to/lvgl]
# Defaults to the LVGL that PlatformIO fetched for any env (run a pio build first).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${1:-$root/doc-shots/emoji_fit.png}"
lvgl="${2:-}"
if [ -z "$lvgl" ]; then
  for d in "$root"/.pio/libdeps/*/lvgl; do
    [ -f "$d/lvgl.h" ] && { lvgl="$d"; break; }
  done
fi
[ -n "$lvgl" ] && [ -f "$lvgl/lvgl.h" ] || {
  echo "LVGL sources not found; run a PlatformIO build first or pass the lvgl path" >&2
  exit 2
}
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
flags=(-O1 -w -DLV_CONF_INCLUDE_SIMPLE -I"$here" -I"$lvgl" -I"$root/src/ui-touch")
mkdir -p "$work/obj"
jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
export CC_CMD="${CC:-clang} -c ${flags[*]}" OBJ_DIR="$work/obj"
find "$lvgl/src" -name '*.c' -print0 |
  xargs -0 -P "$jobs" -I{} sh -c \
    '$CC_CMD "$1" -o "$OBJ_DIR/$(printf %s "$1" | cksum | cut -d" " -f1).o"' _ {}
"${CC:-clang}" -c "${flags[@]}" "$root/src/ui-touch/emoji_data.c" -o "$work/emoji.o"
cxx="${CXX:-clang++}"
"$cxx" -std=c++17 -c "${flags[@]}" "$here/render.cpp" -o "$work/render.o"
"$cxx" "$work/render.o" "$work/emoji.o" "$work/obj"/*.o -o "$work/render"
"$work/render" "$work/emoji_fit.bmp"
python3 - "$work/emoji_fit.bmp" "$out" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1])
im.resize((im.width * 2, im.height * 2), Image.NEAREST).save(sys.argv[2])
PY
echo "wrote $out"
