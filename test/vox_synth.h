// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A deterministic 4 s speech-like test signal at 8 kHz: vowels made from a
// glottal pulse train (pitch gliding 110-180 Hz) through three formant
// resonators, fricatives from noise through high resonators, and silences.
// Only + and * on doubles (coefficients are literals, noise is an LCG), so the
// samples are identical in every program that includes this, whichever codec2
// tree it links. Used by test/test_vox_record.cpp and test/vox_ref_encode.c.

#include <stdint.h>

#define VOX_SYNTH_RATE 8000
#define VOX_SYNTH_SAMPLES (4 * VOX_SYNTH_RATE)

// 2*r*cos(w) and r*r for formants with a 100 Hz bandwidth (r = 0.9615) at 8 kHz.
#define VS_R2 0.9245
static const double vs_vowel_a[3] = {1.6396, 1.1303, -0.7359};   // 700, 1200, 2500 Hz
static const double vs_vowel_i[3] = {1.8699, -0.4488, -1.3597};  // 300, 2300, 3000 Hz
static const double vs_fric[3]    = {-0.7359, -1.3597, -1.3597}; // 2500, 3000, 3000 Hz

// Segment kinds: 0 silence, 1 /a/, 2 /i/, 3 fricative. Lengths in ms.
static const int vs_seg_kind[] = {0, 1, 3, 2, 0, 1, 2, 3, 1, 0};
static const int vs_seg_ms[]   = {250, 750, 250, 750, 250, 500, 400, 250, 350, 250};

static inline void voxSynth(int16_t* out /* VOX_SYNTH_SAMPLES */) {
  double y1[3] = {0, 0, 0}, y2[3] = {0, 0, 0};
  uint32_t lcg = 12345u;
  double phase = 0;   // glottal phase in samples since the last pulse
  int seg = 0, seg_left = vs_seg_ms[0] * (VOX_SYNTH_RATE / 1000);
  const int nseg = (int)(sizeof vs_seg_kind / sizeof vs_seg_kind[0]);
  for (int n = 0; n < VOX_SYNTH_SAMPLES; n++) {
    while (seg_left == 0 && seg + 1 < nseg) { seg++; seg_left = vs_seg_ms[seg] * (VOX_SYNTH_RATE / 1000); }
    const int kind = seg_left > 0 ? vs_seg_kind[seg] : 0;
    if (seg_left > 0) seg_left--;
    // Pitch period glides from 72.7 (110 Hz) to 44.4 (180 Hz) samples and back.
    const double t = (double)n / VOX_SYNTH_SAMPLES;            // 0..1
    const double tri = t < 0.5 ? 2 * t : 2 - 2 * t;            // 0..1..0
    const double period = 72.7 - 28.3 * tri;
    double x = 0;
    const double* f = vs_vowel_a;
    if (kind == 1 || kind == 2) {
      f = kind == 1 ? vs_vowel_a : vs_vowel_i;
      phase += 1.0;
      if (phase >= period) { phase -= period; x = 2000.0; }   // glottal pulse
    } else if (kind == 3) {
      f = vs_fric;
      lcg = lcg * 1103515245u + 12345u;
      x = ((double)((lcg >> 16) & 0x7FFF) - 16384.0) * 0.05;   // white noise
    }
    double s = 0;
    for (int k = 0; k < 3; k++) {
      const double y = x + f[k] * y1[k] - VS_R2 * y2[k];
      y2[k] = y1[k];
      y1[k] = y;
      s += y;
    }
    s *= 0.6;
    if (s > 30000) s = 30000;
    if (s < -30000) s = -30000;
    out[n] = (int16_t)(s < 0 ? s - 0.5 : s + 0.5);
  }
}
