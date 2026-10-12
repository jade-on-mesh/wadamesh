#!/usr/bin/env bash
# Re-vendors Codec2's speech codec into lib/codec2, for playing MeshCore Open
# TAK voice clips (VE3). Take it from the same tree the app builds, so decoding
# matches the sender exactly:
#
#   scripts/vendor_codec2.sh ../meshcore-open-tak/third_party/codec2
#
# Only the speech codec (the app's codec2_sources.cmake list); the FreeDV
# modems and LDPC tables are left out. Built with __EMBEDDED__ so the codebooks
# are const (flash, not RAM) and allocation goes through codec2_malloc & co.
set -euo pipefail

SRC=${1:?usage: vendor_codec2.sh <codec2 tree>}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DST="$ROOT/lib/codec2"

FILES="dump lpc nlp postfilter sine codec2 codec2_fft kiss_fft kiss_fftr interp lsp mbest
       newamp1 phase quantise pack codebook codebookd codebookjmv codebookge
       codebooknewamp1 codebooknewamp1_energy codebooknewamp2 codebooknewamp2_energy"

rm -rf "$DST/src" "$DST/include" "$DST/COPYING"
mkdir -p "$DST/src" "$DST/include/codec2"
cp "$SRC/COPYING" "$DST/"
# Only the headers the speech codec includes (the rest are FreeDV/LDPC tables).
HEADERS="_kiss_fft_guts bpf bpfb codec2 codec2_fft codec2_internal comp comp_prim debug_alloc
         defines dump interp kiss_fft kiss_fftr lpc lsp machdep mbest newamp1 nlp os phase
         postfilter quantise sine"
for h in $HEADERS; do cp "$SRC/src/$h.h" "$DST/src/"; done
for f in $FILES; do cp "$SRC/src/$f.c" "$DST/src/"; done
cp "$SRC/include/codec2/version.h" "$DST/include/codec2/"
# The public header, where users include it from.
cp "$SRC/src/codec2.h" "$DST/include/codec2/codec2.h"

echo "Codec2 speech codec vendored into lib/codec2"
