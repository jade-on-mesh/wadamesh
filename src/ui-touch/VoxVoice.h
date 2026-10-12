// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Decodes MeshCore Open TAK voice clips sent with Codec2 (VE3) to 8 kHz 16-bit
// mono PCM, with the same Codec2 the app uses (lib/codec2). Mimi clips (VM1)
// need a 223 MB neural model and can't be played on a radio.
//
// Platform-neutral and host-tested (test/test_vox_voice.cpp). Builds without
// lib/codec2 (Tanmatsu, P4 for now) get a stub that decodes nothing.

#include <stddef.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include(<codec2/codec2.h>)
#define VOX_CODEC2_AVAILABLE 1
#endif
#endif
#ifndef VOX_CODEC2_AVAILABLE
#define VOX_CODEC2_AVAILABLE 0
#endif

constexpr int kVoxVoiceSampleRate = 8000;

// Whether this build can decode a VE3 wire mode (0..6). 700C is compiled out
// of lib/codec2 to save flash; Open TAK sends 1200 by default.
bool voxCodec2CanDecode(int wire_mode);

// Samples a clip of `len` codec bytes decodes to (whole frames only).
size_t voxCodec2Samples(int wire_mode, size_t len);

// Decodes `len` bytes of back-to-back frames into `out` (capacity in samples).
// Returns the number of samples written, or -1 if the mode isn't supported, the
// codec couldn't start, or `out` is too small.
long voxCodec2Decode(int wire_mode, const uint8_t* bits, size_t len, int16_t* out, size_t out_cap);

// Frame-by-frame decoding, for streaming a clip straight to the speaker.
class VoxCodec2Stream {
 public:
  VoxCodec2Stream() {}
  ~VoxCodec2Stream() { end(); }
  bool begin(int wire_mode);   // false: mode unsupported or no memory
  int bytesPerFrame() const { return bpf_; }
  int samplesPerFrame() const { return spf_; }
  void decode(const uint8_t* frame, int16_t* out);   // bpf_ bytes in, spf_ samples out
  void end();

 private:
  void* c2_ = nullptr;
  int bpf_ = 0;
  int spf_ = 0;
};
