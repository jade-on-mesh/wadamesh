// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reference encoder for test/data/vox/encode_*.c2: encodes test/vox_synth.h's
// signal with a Codec2 tree built the way MeshCore Open TAK builds it (NOT the
// vendored lib/codec2), so test_vox_record.cpp can check that the radio's
// encoder produces exactly the bits the app would.
//
//   test/build_vox_record_test.sh --regen ../meshcore-open-tak/third_party/codec2
//
// Usage: vox_ref_encode <codec2 mode id> <out.c2>

#include <stdio.h>
#include <stdlib.h>

#include "codec2.h"
#include "vox_synth.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <CODEC2_MODE id> <out.c2>\n", argv[0]);
    return 2;
  }
  static int16_t pcm[VOX_SYNTH_SAMPLES];
  voxSynth(pcm);
  struct CODEC2* c2 = codec2_create(atoi(argv[1]));
  if (!c2) return 1;
  const int spf = codec2_samples_per_frame(c2);
  const int bpf = codec2_bytes_per_frame(c2);
  FILE* f = fopen(argv[2], "wb");
  if (!f) return 1;
  unsigned char bits[16];
  for (int i = 0; i + spf <= VOX_SYNTH_SAMPLES; i += spf) {
    codec2_encode(c2, bits, pcm + i);
    fwrite(bits, 1, (size_t)bpf, f);
  }
  fclose(f);
  codec2_destroy(c2);
  return 0;
}
