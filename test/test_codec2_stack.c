// Stack used by lib/codec2 to create, encode and decode, for the radio's
// record (VoxMic, 16 KB task) and playback (vox_voice, 16 KB task) stacks.
// Runs each in a thread on a painted stack and reports the deepest byte used.
// Built without sanitizers (they inflate stack frames); see
// test/build_vox_record_test.sh. Limits are for this 64-bit host, which uses
// a little more than the ESP32-S3.
#include <codec2/codec2.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void* codec2_malloc(size_t n) { return malloc(n); }
void* codec2_calloc(size_t a, size_t b) { return calloc(a, b); }
void codec2_free(void* p) { free(p); }

static const int kModes[] = {CODEC2_MODE_3200, CODEC2_MODE_2400, CODEC2_MODE_1600,
                             CODEC2_MODE_1400, CODEC2_MODE_1300, CODEC2_MODE_1200};
static int s_what;   // 0 create, 1 encode, 2 decode

static void* run(void* arg) {
  (void)arg;
  for (unsigned mi = 0; mi < sizeof kModes / sizeof kModes[0]; mi++) {
    struct CODEC2* c = codec2_create(kModes[mi]);
    const int spf = codec2_samples_per_frame(c), bpf = codec2_bytes_per_frame(c);
    short s[320], o[320];
    unsigned char b[16] = {0};
    double ph = 0;
    for (int fr = 0; fr < 50; fr++) {
      for (int i = 0; i < spf; i++) {
        ph += 2 * M_PI * (120 + 40 * sin(fr * 0.05)) / 8000;
        double v = 0;
        for (int h = 1; h < 20; h++) v += sin(h * ph) / h;
        s[i] = (short)(3000 * v);
      }
      if (s_what == 1) codec2_encode(c, b, s);
      if (s_what == 2) {
        for (int k = 0; k < bpf; k++) b[k] = (unsigned char)(fr * 37 + k * 11);
        codec2_decode(c, o, b);
      }
    }
    codec2_destroy(c);
  }
  return NULL;
}

static size_t measure(int what) {
  const size_t sz = 256 * 1024;
  unsigned char* stk = aligned_alloc(4096, sz);
  memset(stk, 0xA5, sz);
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstack(&a, stk, sz);
  s_what = what;
  pthread_t t;
  if (pthread_create(&t, &a, run, NULL)) return (size_t)-1;
  pthread_join(t, NULL);
  size_t i = 0;
  while (i < sz && stk[i] == 0xA5) i++;
  free(stk);
  return sz - i;
}

int main(void) {
  const size_t create = measure(0), enc = measure(1), dec = measure(2);
  printf("codec2 stack on this host: create %zu, encode %zu, decode %zu bytes\n", create, enc, dec);
  int bad = 0;
  if (enc > 11 * 1024) { printf("FAIL: encode needs %zu bytes (VoxMic's task has 16 KB)\n", enc); bad = 1; }
  if (dec > 11 * 1024) { printf("FAIL: decode needs %zu bytes (vox_voice's task has 16 KB)\n", dec); bad = 1; }
  return bad;
}
