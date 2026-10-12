// SPDX-License-Identifier: GPL-3.0-or-later
//
// Decodes Codec2 voice clips through the firmware's VoxVoice and lib/codec2
// (built as the firmware builds it: __EMBEDDED__, 700C compiled out) and checks
// every sample against the app's own Codec2 build decoding the same bits.
// Build: test/build_vox_voice_test.sh

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>

#include "ui-touch/VoxVoice.h"
#include "ui-touch/VoxWire.h"

static uint64_t fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

// One decode in a fresh process: --hash|--hash-stream <file> <mode> <samples>
static int hashOne(const char* path, int mode, size_t samples, bool stream) {
  FILE* f = fopen(path, "rb");
  if (!f) return 1;
  uint8_t bits[4096];
  const size_t n = fread(bits, 1, sizeof bits, f);
  fclose(f);
  int16_t* pcm = (int16_t*)malloc(samples * 2);
  const clock_t t0 = clock();
  long got = -1;
  if (stream) {   // frame by frame, as the speaker path plays it
    VoxCodec2Stream st;
    if (st.begin(mode)) {
      got = 0;
      for (size_t i = 0; i + (size_t)st.bytesPerFrame() <= n && (size_t)(got + st.samplesPerFrame()) <= samples;
           i += (size_t)st.bytesPerFrame(), got += st.samplesPerFrame())
        st.decode(bits + i, pcm + got);
    }
  } else {
    got = voxCodec2Decode(mode, bits, n, pcm, samples);
  }
  const double ms = (clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
  printf("%016llx %.3f\n",
         (unsigned long long)(got == (long)samples ? fnv1a((const uint8_t*)pcm, samples * 2) : 0), ms);
  free(pcm);
  return 0;
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc == 5 && (!strcmp(argv[1], "--hash") || !strcmp(argv[1], "--hash-stream")))
    return hashOne(argv[2], atoi(argv[3]), (size_t)atol(argv[4]), !strcmp(argv[1], "--hash-stream"));
  const char* dir = argc > 1 ? argv[1] : "test/data/vox";
  char path[512];
  snprintf(path, sizeof path, "%s/voice_expected.txt", dir);
  FILE* m = fopen(path, "r");
  assert(m);
  char line[256];
  int checked = 0;
  while (fgets(line, sizeof line, m)) {
    if (line[0] == '#') continue;
    char name[64];
    int mode;
    size_t bytes, samples;
    unsigned long long want;
    if (sscanf(line, "%63s %d %zu %zu %llx", name, &mode, &bytes, &samples, &want) != 5) continue;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "rb");
    assert(f);
    uint8_t* bits = (uint8_t*)malloc(bytes);
    assert(fread(bits, 1, bytes, f) == bytes);
    fclose(f);

    assert(voxCodec2CanDecode(mode));
    assert(voxCodec2Samples(mode, bytes) == samples);
    int16_t* pcm = (int16_t*)malloc(samples * 2);
    assert(voxCodec2Decode(mode, bits, bytes, pcm, samples - 1) == -1);   // too small: refused
    // Codec2's unvoiced phases come from one process-wide seed, so compare the
    // first decode of a fresh process (this binary re-run with --hash), as the
    // reference was made.
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "'%s' --hash '%s' %d %zu", argv[0], path, mode, samples);
    FILE* child = popen(cmd, "r");
    assert(child);
    unsigned long long got = 0;
    double ms = 0;
    assert(fscanf(child, "%llx %lf", &got, &ms) == 2);
    pclose(child);
    printf("%-16s mode %d  %4zu B -> %.1f s  %s (%.1f ms on this host)\n", name, mode, bytes,
           samples / 8000.0, got == want ? "matches the app" : "MISMATCH", ms);
    assert(got == want);
    snprintf(cmd, sizeof cmd, "'%s' --hash-stream '%s' %d %zu", argv[0], path, mode, samples);
    child = popen(cmd, "r");
    assert(child && fscanf(child, "%llx %lf", &got, &ms) == 2);
    pclose(child);
    assert(got == want);   // streamed frame by frame: identical
    assert(voxCodec2Decode(mode, bits, bytes, pcm, samples) == (long)samples);
    // A trailing partial frame is ignored, not read past.
    assert(voxCodec2Decode(mode, bits, bytes - 1, pcm, samples) ==
           (long)(samples - (VoxWire::packetBytes(mode) ? (size_t)(mode == 2 || mode == 6 ? 160 : 320) : 0)));
    free(pcm);
    free(bits);
    checked++;
  }
  fclose(m);
  assert(checked == 4);

  VoxCodec2Stream st;
  assert(!st.begin(0) && !st.begin(VoxWire::kMimiModeId));

  // 700C is compiled out; Mimi and photos aren't Codec2 at all.
  int16_t buf[640];
  const uint8_t junk[8] = {0};
  assert(!voxCodec2CanDecode(0) && voxCodec2Decode(0, junk, 8, buf, 640) == -1);
  assert(!voxCodec2CanDecode(VoxWire::kMimiModeId) && !voxCodec2CanDecode(VoxWire::kImageModeId));
  // Random bytes decode to noise without crashing (sanitizers watch).
  uint8_t noise[600];
  for (size_t i = 0; i < sizeof noise; i++) noise[i] = (uint8_t)(i * 97 + 13);
  int16_t* out = (int16_t*)malloc(40000 * 2);
  for (int mode = 1; mode <= 6; mode++) assert(voxCodec2Decode(mode, noise, sizeof noise, out, 40000) > 0);
  free(out);
  printf("4 clips decoded sample-exact; 700C/Mimi refused; noise survives\n");
  return 0;
}
