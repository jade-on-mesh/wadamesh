#!/usr/bin/env bash
# Builds and runs test/test_otp_interop.cpp: the native OTP core against
# OTP_3_RC1.lua on the firmware's vendored Lua. Usage: test/build_otp_test.sh [outdir]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$(mktemp -d)}
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
mkdir -p "$OUT/lua"
for f in "$ROOT"/lib/lua/src/*.c; do
  case "$(basename "$f")" in lua.c|luac.c) continue ;; esac
  gcc -std=gnu99 -O1 -g -w $SAN -I "$ROOT/lib/lua/src" -c "$f" -o "$OUT/lua/$(basename "$f").o" &
done
wait
g++ -std=gnu++17 -O1 -g $SAN -Wall -Wextra -Wno-format-truncation -I "$ROOT/src" -I "$ROOT/lib/lua/src" \
    "$ROOT/test/test_otp_interop.cpp" "$ROOT/src/ui-touch/OtpCore.cpp" "$OUT"/lua/*.o -lm \
    -o "$OUT/test_otp_interop"
"$OUT/test_otp_interop" "$ROOT"
