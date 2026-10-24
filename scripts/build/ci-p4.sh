#!/usr/bin/env bash
# Wadamesh-tak: build one LilyGo T-Display P4 variant in CI.
#
# The P4 is an ESP-IDF project (tdisplay_p4/), not a PlatformIO env, and its
# build.sh was written for a Mac with a project-local ESP-IDF. Here it runs in
# the espressif/idf:v5.5.1 image instead:
#   - that image's IDF stands in for the project-local one (tanmatsu/esp-idf*);
#   - a small sed wrapper turns build.sh's BSD `sed -i ''` into GNU `sed -i`;
#   - the vendored payloads come from fetch-deps.sh, which needs the T-Deck env's
#     PlatformIO libdeps (run `pio pkg install -e LilyGo_TDeck_companion_radio_touch`
#     on the host first).
#
#   docker run --rm -v "$PWD:/w" -w /w espressif/idf:v5.5.1 \
#     bash scripts/build/ci-p4.sh <tag> <outdir> <variant-name> [ENV=1 ...]
#
# e.g. ci-p4.sh tak-beta_93.3 dist tdisplay-p4-lr2021 WADA_P4_LR2021=1
#
# Writes <outdir>/wadamesh-<variant>.bin (the OTA image) and -merged.bin (USB),
# after checking the image names itself <variant>: that name is what the device
# builds its update URL from, so a mismatch would update it to another variant.
set -euo pipefail

TAG="${1:?tag}"; OUT="${2:?outdir}"; NAME="${3:?variant name}"; shift 3
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
git config --global --add safe.directory '*'

# GNU sed in place of BSD sed's `-i ''`.
mkdir -p /tmp/sedshim
cat > /tmp/sedshim/sed <<'SH'
#!/bin/bash
a=()
while [ $# -gt 0 ]; do
  if [ "$1" = "-i" ] && [ $# -ge 2 ] && [ -z "$2" ]; then a+=("-i"); shift 2; continue; fi
  a+=("$1"); shift
done
exec /bin/sed "${a[@]}"
SH
chmod +x /tmp/sedshim/sed
export PATH="/tmp/sedshim:$PATH"

# The image's IDF as the project-local one.
: "${IDF_PATH:?not in the espressif/idf image}"
: "${IDF_TOOLS_PATH:=/opt/esp}"
[ -e tanmatsu/esp-idf ] || ln -s "$IDF_PATH" tanmatsu/esp-idf
[ -e tanmatsu/esp-idf-tools ] || ln -s "$IDF_TOOLS_PATH" tanmatsu/esp-idf-tools

[ -d tdisplay_p4/components/lvgl/upstream/src ] || bash tdisplay_p4/fetch-deps.sh

# Only this variant's switches (see p4-release.sh for why nothing may leak in).
unset WADA_P4_LCD WADA_P4_LR2021 WADA_P4_LEGACY_AT
for kv in "$@"; do export "$kv"; done

cd tdisplay_p4
./build.sh fullclean >/dev/null 2>&1 || true
WADA_FW_TAG="$TAG" ./build.sh build

if [ -n "${WADA_P4_LCD:-}" ]; then d=tdisplay_p4_lcd; else d=tdisplay_p4; fi
[ -n "${WADA_P4_LR2021:-}" ] && d="${d}_lr2021"
B="build/$d"
APP="$B/application.bin"
[ -f "$APP" ] || { echo "::error::no $APP"; exit 1; }

python3 - "$APP" "$NAME" "$TAG" <<'PY'
import re, sys
data = open(sys.argv[1], "rb").read()
want, tag = sys.argv[2].encode(), sys.argv[3].encode()
# WADA_BOARD_ID as a C string of its own: the name the update URL is built from.
ids = sorted(set(re.findall(rb"(?<=\0)tdisplay-p4[a-z0-9-]*(?=\0)", data)))
if ids != [want]:
    sys.exit("image identifies as %r, expected %r: it would update to the wrong variant" % (ids, want))
if tag not in data:
    sys.exit("%r is not embedded in the image" % tag)
print("verified", want.decode(), "carrying", tag.decode())
PY

mkdir -p "$ROOT/$OUT"
cp "$APP" "$ROOT/$OUT/wadamesh-$NAME.bin"
python3 -m esptool --chip esp32p4 merge_bin --flash_mode dio --flash_size 16MB --flash_freq 80m \
  -o "$ROOT/$OUT/wadamesh-$NAME-merged.bin" \
  0x2000 "$B/bootloader/bootloader.bin" \
  0x8000 "$B/partition_table/partition-table.bin" \
  0xf000 "$B/ota_data_initial.bin" \
  0x20000 "$APP"
# Where the memory goes (flash, internal RAM, PSRAM), for ram-report/<variant>.
ELF="$(ls "$B"/*.elf | head -1)"; MAP="$(ls "$B"/*.map | head -1)"
if [ -n "$ELF" ] && [ -n "$MAP" ]; then
  python3 "$ROOT/scripts/build/mem-report.py" "$ELF" "$MAP" riscv32-esp-elf- > "$ROOT/$OUT/mem-report-$NAME.txt" || true
  python3 -m esp_idf_size "$MAP" >> "$ROOT/$OUT/mem-report-$NAME.txt" 2>/dev/null || true
fi
ls -la "$ROOT/$OUT"
