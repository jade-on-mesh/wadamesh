// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoxVoice.h"

#include <stdlib.h>
#include <string.h>

#include "VoxWire.h"

#if VOX_CODEC2_AVAILABLE

#include <codec2/codec2.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

// lib/codec2 is built with __EMBEDDED__, which routes its allocations here.
// Its state is a few KB; PSRAM first, internal RAM if PSRAM is short.
extern "C" {
void* codec2_malloc(size_t size) {
#ifdef ESP_PLATFORM
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
#else
  return malloc(size);
#endif
}
void* codec2_calloc(size_t nmemb, size_t size) {
  if (size && nmemb > (size_t)-1 / size) return nullptr;
  void* p = codec2_malloc(nmemb * size);
  if (p) memset(p, 0, nmemb * size);
  return p;
}
void codec2_free(void* ptr) {
#ifdef ESP_PLATFORM
  heap_caps_free(ptr);
#else
  free(ptr);
#endif
}
}

bool voxCodec2CanDecode(int wire_mode) {
  const int m = VoxWire::codec2Mode(wire_mode);
  return m >= 0 && m != CODEC2_MODE_700C;   // 700C: compiled out (CODEC2_MODE_700C_EN=0)
}

// Frame geometry per mode, without starting a codec: 40 ms frames except 2400
// and 3200 (20 ms), as in voice_wire.dart.
static int samplesPerFrame(int wire_mode) {
  return (wire_mode == 2 || wire_mode == 6) ? 160 : 320;
}

size_t voxCodec2Samples(int wire_mode, size_t len) {
  const int bpf = VoxWire::bytesPerFrame(wire_mode);
  if (!voxCodec2CanDecode(wire_mode) || bpf <= 0) return 0;
  return (len / (size_t)bpf) * (size_t)samplesPerFrame(wire_mode);
}

long voxCodec2Decode(int wire_mode, const uint8_t* bits, size_t len, int16_t* out, size_t out_cap) {
  if (!voxCodec2CanDecode(wire_mode) || !bits || !out) return -1;
  struct CODEC2* c2 = codec2_create(VoxWire::codec2Mode(wire_mode));
  if (!c2) return -1;
  const int bpf = codec2_bytes_per_frame(c2);
  const int spf = codec2_samples_per_frame(c2);
  const size_t frames = len / (size_t)bpf;
  if (bpf != VoxWire::bytesPerFrame(wire_mode) || spf != samplesPerFrame(wire_mode) ||
      frames * (size_t)spf > out_cap) {
    codec2_destroy(c2);
    return -1;
  }
  for (size_t i = 0; i < frames; i++) {
    // codec2_decode takes a non-const pointer but only reads the frame.
    codec2_decode(c2, out + i * (size_t)spf, const_cast<unsigned char*>(bits + i * (size_t)bpf));
  }
  codec2_destroy(c2);
  return (long)(frames * (size_t)spf);
}

bool VoxCodec2Stream::begin(int wire_mode) {
  end();
  if (!voxCodec2CanDecode(wire_mode)) return false;
  struct CODEC2* c2 = codec2_create(VoxWire::codec2Mode(wire_mode));
  if (!c2) return false;
  bpf_ = codec2_bytes_per_frame(c2);
  spf_ = codec2_samples_per_frame(c2);
  c2_ = c2;
  return true;
}

void VoxCodec2Stream::decode(const uint8_t* frame, int16_t* out) {
  if (c2_) codec2_decode((struct CODEC2*)c2_, out, const_cast<unsigned char*>(frame));
}

void VoxCodec2Stream::end() {
  if (c2_) codec2_destroy((struct CODEC2*)c2_);
  c2_ = nullptr;
  bpf_ = spf_ = 0;
}

#else  // !VOX_CODEC2_AVAILABLE

bool VoxCodec2Stream::begin(int) { return false; }
void VoxCodec2Stream::decode(const uint8_t*, int16_t*) {}
void VoxCodec2Stream::end() {}

bool voxCodec2CanDecode(int) { return false; }
size_t voxCodec2Samples(int, size_t) { return 0; }
long voxCodec2Decode(int, const uint8_t*, size_t, int16_t*, size_t) { return -1; }

#endif
