// SPDX-License-Identifier: GPL-3.0-or-later
//
// Recording a voice clip on the radio: VoxCodec2Encoder (lib/codec2, built as
// the firmware builds it) and VoxMicPrep (mic 16 kHz stereo to Codec2's 8 kHz).
//
//  - The encoder's bits for test/vox_synth.h's signal must equal
//    test/data/vox/encode_1200.c2, which MeshCore Open TAK's own Codec2 tree
//    produced (test/vox_ref_encode.c), so the app decodes exactly what the radio
//    meant to send.
//  - Encoded then decoded, speech segments stay loud and silences stay quiet.
//  - VoxMicPrep removes DC, keeps speech-band tones, rejects what would alias,
//    levels quiet and loud input towards its target, never clips, and doesn't
//    pump up silence.
//
// Build: test/build_vox_record_test.sh

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>

#include "ui-touch/VoxVoice.h"
#include "ui-touch/VoxWire.h"
#include "vox_synth.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                     \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
      printf(__VA_ARGS__);                                   \
      printf("\n");                                          \
      g_fail++;                                              \
    }                                                        \
  } while (0)

static std::vector<uint8_t> readFile(const char* path) {
  std::vector<uint8_t> b;
  FILE* f = fopen(path, "rb");
  if (!f) return b;
  uint8_t buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
  fclose(f);
  return b;
}

static std::vector<uint8_t> encodeSynth(int wire_mode, double* ms_per_frame) {
  std::vector<int16_t> pcm(VOX_SYNTH_SAMPLES);
  voxSynth(pcm.data());
  VoxCodec2Encoder enc;
  std::vector<uint8_t> bits;
  if (!enc.begin(wire_mode)) return bits;
  const int spf = enc.samplesPerFrame(), bpf = enc.bytesPerFrame();
  uint8_t frame[16];
  int frames = 0;
  const clock_t t0 = clock();
  for (int i = 0; i + spf <= VOX_SYNTH_SAMPLES; i += spf, frames++) {
    enc.encode(pcm.data() + i, frame);
    bits.insert(bits.end(), frame, frame + bpf);
  }
  if (ms_per_frame) *ms_per_frame = (clock() - t0) * 1000.0 / CLOCKS_PER_SEC / (frames ? frames : 1);
  return bits;
}

static double rms(const int16_t* p, size_t n) {
  double s = 0;
  for (size_t i = 0; i < n; i++) s += (double)p[i] * p[i];
  return n ? sqrt(s / n) : 0;
}

// Power of `f` Hz in `x` (Goertzel), as an RMS amplitude.
static double tone(const int16_t* x, size_t n, double f, double rate) {
  const double w = 2 * M_PI * f / rate, c = 2 * cos(w);
  double s1 = 0, s2 = 0;
  for (size_t i = 0; i < n; i++) {
    const double s = x[i] + c * s1 - s2;
    s2 = s1;
    s1 = s;
  }
  const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
  return sqrt(2 * p) / n;
}

// ---- encoder ----------------------------------------------------------------

static void testEncoderMatchesApp(const char* dir) {
  char path[512];
  snprintf(path, sizeof path, "%s/encode_1200.c2", dir);
  const std::vector<uint8_t> want = readFile(path);
  CHECK(!want.empty(), "missing %s (regenerate with --regen)", path);
  double ms = 0;
  const std::vector<uint8_t> got = encodeSynth(kVoxRecordWireMode, &ms);
  CHECK(got.size() == (size_t)(VOX_SYNTH_SAMPLES / 320) * 6, "1200: %zu bytes", got.size());
  CHECK(got == want, "1200 bits differ from the app's Codec2 (%zu vs %zu bytes)", got.size(), want.size());
  if (got != want && got.size() == want.size()) {
    for (size_t i = 0; i < got.size(); i++)
      if (got[i] != want[i]) { printf("  first difference at byte %zu (frame %zu)\n", i, i / 6); break; }
  }
  printf("encode 1200: %zu bytes, %.3f ms/frame on this host\n", got.size(), ms);

  // The packets a 4 s clip makes, as the app will count them.
  const int per = VoxWire::packetBytes(kVoxRecordWireMode);
  CHECK(per == 156, "packet bytes %d", per);
  CHECK((got.size() + per - 1) / per == 4, "4 s at 1200 should be 4 packets");
}

static void testEncoderModes() {
  VoxCodec2Encoder e;
  CHECK(!e.begin(0), "700C is compiled out, so the radio must not record in it");
  CHECK(!e.begin(32), "Mimi is not a Codec2 mode");
  CHECK(!e.begin(40), "AV1 is not a Codec2 mode");
  for (int m : {1, 2, 3, 4, 5, 6}) {
    CHECK(e.begin(m), "mode %d", m);
    CHECK(e.bytesPerFrame() == VoxWire::bytesPerFrame(m), "mode %d bpf %d", m, e.bytesPerFrame());
    e.end();
  }
}

// Decoding uses Codec2's process-wide random seed, so it runs in a child.
static void testRoundTrip() {
  const std::vector<uint8_t> bits = encodeSynth(kVoxRecordWireMode, nullptr);
  int fd[2];
  assert(pipe(fd) == 0);
  const pid_t pid = fork();
  if (pid == 0) {
    close(fd[0]);
    std::vector<int16_t> out(voxCodec2Samples(kVoxRecordWireMode, bits.size()));
    const long n = voxCodec2Decode(kVoxRecordWireMode, bits.data(), bits.size(), out.data(), out.size());
    double r[2] = {-1, -1};
    if (n == (long)out.size()) {
      // Middle of the first /a/ (250..1000 ms) vs the closing silence (3750..4000 ms),
      // skipping the codec's 1-2 frame delay at each edge.
      r[0] = rms(out.data() + 8000 * 400 / 1000, 8000 * 500 / 1000);
      r[1] = rms(out.data() + 8000 * 3850 / 1000, 8000 * 120 / 1000);
    }
    if (write(fd[1], r, sizeof r) != (ssize_t)sizeof r) _exit(1);
    _exit(0);
  }
  close(fd[1]);
  double r[2] = {-1, -1};
  CHECK(read(fd[0], r, sizeof r) == (ssize_t)sizeof r, "child didn't report");
  close(fd[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  std::vector<int16_t> pcm(VOX_SYNTH_SAMPLES);
  voxSynth(pcm.data());
  const double in_vowel = rms(pcm.data() + 8000 * 400 / 1000, 8000 * 500 / 1000);
  printf("round trip: vowel rms in %.0f out %.0f, silence out %.0f\n", in_vowel, r[0], r[1]);
  CHECK(r[0] > in_vowel * 0.25 && r[0] < in_vowel * 4, "vowel level %.0f vs %.0f", r[0], in_vowel);
  CHECK(r[1] >= 0 && r[1] < r[0] * 0.05, "silence %.0f should be far below speech %.0f", r[1], r[0]);
}

// The waveform the chat draws for a talk: loud where the synthetic speech is,
// flat in its silences, 0..100.
static void testWaveform() {
  const std::vector<uint8_t> bits = encodeSynth(kVoxRecordWireMode, nullptr);
  uint8_t bars[kVoxWaveBars];
  CHECK(voxCodec2Waveform(kVoxRecordWireMode, bits.data(), bits.size(), bars, kVoxWaveBars), "waveform");
  int top = 0;
  for (uint8_t b : bars) {
    CHECK(b <= 100, "bar %u", b);
    if (b > top) top = b;
  }
  CHECK(top == 100, "loudest bar %d", top);
  // 4 s in 40 bars: 100 ms each. The first /a/ runs 250..1000 ms, the clip
  // ends in 250 ms of silence.
  CHECK(bars[6] > 50, "vowel bar %u", bars[6]);
  CHECK(bars[kVoxWaveBars - 1] < bars[6] / 3, "closing silence %u vs vowel %u", bars[kVoxWaveBars - 1], bars[6]);
  printf("waveform:");
  for (uint8_t b : bars) printf(" %u", b);
  printf("\n");
  CHECK(!voxCodec2Waveform(0, bits.data(), bits.size(), bars, kVoxWaveBars), "700C is compiled out");
}

// ---- mic prep ---------------------------------------------------------------

// 16 kHz stereo: `amp` sine at `f` on both channels plus `dc` and `alias_amp` at 6 kHz.
static std::vector<int16_t> stereoTone(double secs, double f, double amp, double dc, double alias_amp,
                                       bool right_silent = false) {
  const size_t n = (size_t)(secs * 16000);
  std::vector<int16_t> s(2 * n);
  for (size_t i = 0; i < n; i++) {
    double v = dc + amp * sin(2 * M_PI * f * i / 16000) + alias_amp * sin(2 * M_PI * 6000 * i / 16000);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    s[2 * i] = (int16_t)lrint(v);
    s[2 * i + 1] = right_silent ? (int16_t)lrint(dc) : (int16_t)lrint(v);
  }
  return s;
}

static std::vector<int16_t> runPrep(VoxMicPrep& p, const std::vector<int16_t>& stereo, size_t chunk = 256) {
  std::vector<int16_t> out(stereo.size() / 2 / 2 + 8);
  size_t n = 0;
  const size_t frames = stereo.size() / 2;
  for (size_t i = 0; i < frames; i += chunk) {
    const size_t k = frames - i < chunk ? frames - i : chunk;
    n += p.process(stereo.data() + 2 * i, k, out.data() + n, out.size() - n);
  }
  out.resize(n);
  return out;
}

static void testPrepDecimates() {
  VoxMicPrep p;
  const auto in = stereoTone(1.0, 1000, 1500, 0, 0);
  const auto out = runPrep(p, in);
  CHECK(out.size() == 8000, "1 s of 16 kHz should give 8000 samples, got %zu", out.size());
  // Chunking must not change anything (state carries across calls, odd counts too).
  VoxMicPrep q;
  const auto out2 = runPrep(q, in, 37);
  CHECK(out == out2, "output depends on how the input is chunked");
  // A mono stream (if the I2S driver delivers one slot) gives the same audio.
  std::vector<int16_t> mono(in.size() / 2);
  for (size_t i = 0; i < mono.size(); i++) mono[i] = in[2 * i];
  VoxMicPrep r;
  std::vector<int16_t> out3(8010);
  size_t n3 = 0;
  for (size_t i = 0; i < mono.size(); i += 512) {
    const size_t k = mono.size() - i < 512 ? mono.size() - i : 512;
    n3 += r.processMono(mono.data() + i, k, out3.data() + n3, out3.size() - n3);
  }
  out3.resize(n3);
  CHECK(out3 == out, "mono input should match the same signal in stereo");
}

static void testPrepRemovesDcAndAliases() {
  VoxMicPrep p;
  // Quiet 1 kHz speech-band tone, a big DC offset, and 6 kHz that would land on
  // 2 kHz if it weren't filtered before decimating.
  const auto out = runPrep(p, stereoTone(2.0, 1000, 1000, 4000, 1000));
  const size_t skip = 8000;   // the second second, after the AGC and DC block settle
  const int16_t* x = out.data() + skip;
  const size_t n = out.size() - skip;
  double mean = 0;
  for (size_t i = 0; i < n; i++) mean += x[i];
  mean /= n;
  const double t1k = tone(x, n, 1000, 8000), t2k = tone(x, n, 2000, 8000);
  printf("prep: dc %.1f, 1 kHz %.0f, 2 kHz alias %.1f (%.1f dB down), gain %.2f\n", mean, t1k, t2k,
         20 * log10(t1k / (t2k > 1e-9 ? t2k : 1e-9)), p.gain());
  CHECK(fabs(mean) < 50, "DC left: %.1f", mean);
  CHECK(t1k > 3000, "1 kHz should be levelled up from 1000, got %.0f", t1k);
  CHECK(t1k / t2k > 100, "6 kHz aliases into 2 kHz only %.1f dB down", 20 * log10(t1k / t2k));
}

static void testPrepAgc() {
  // Quiet talker: a 300-peak tone ends up near the target (13000 peak).
  {
    VoxMicPrep p;
    const auto out = runPrep(p, stereoTone(3.0, 500, 300, 0, 0));
    const double pk = rms(out.data() + 16000, 8000) * sqrt(2.0);
    printf("agc quiet: peak %.0f, gain %.2f\n", pk, p.gain());
    CHECK(pk > 6500 && pk < 20000, "quiet input levelled to %.0f", pk);
  }
  // Loud talker: a near-full-scale tone comes down, and nothing ever clips.
  {
    VoxMicPrep p;
    const auto out = runPrep(p, stereoTone(2.0, 500, 30000, 0, 0));
    int mx = 0;
    for (int16_t v : out) mx = abs(v) > mx ? abs(v) : mx;
    const double pk = rms(out.data() + 8000, 8000) * sqrt(2.0);
    printf("agc loud: settled peak %.0f, max %d, gain %.2f\n", pk, mx, p.gain());
    CHECK(mx <= 32000, "limiter let %d through", mx);
    CHECK(pk > 6500 && pk < 20000, "loud input levelled to %.0f", pk);
  }
  // One dead capsule (right channel silent): still levelled.
  {
    VoxMicPrep p;
    const auto out = runPrep(p, stereoTone(3.0, 500, 600, 0, 0, true));
    const double pk = rms(out.data() + 16000, 8000) * sqrt(2.0);
    CHECK(pk > 6500, "one-capsule input levelled to only %.0f", pk);
  }
}

static void testPrepSilenceNotPumped() {
  VoxMicPrep p;
  const float g0 = p.gain();
  // 3 s of faint noise (peak ~60, below the noise floor).
  std::vector<int16_t> s(2 * 48000);
  uint32_t lcg = 7;
  for (auto& v : s) { lcg = lcg * 1103515245u + 12345u; v = (int16_t)(((int)((lcg >> 16) & 0x7F)) - 64); }
  const auto out = runPrep(p, s);
  printf("silence: gain %.2f -> %.2f, out rms %.1f\n", g0, p.gain(), rms(out.data(), out.size()));
  CHECK(p.gain() <= g0 + 0.01f, "gain rose during silence: %.2f -> %.2f", g0, p.gain());
  CHECK(rms(out.data(), out.size()) < 300, "silence came out at rms %.1f", rms(out.data(), out.size()));
}

static void testPeakMeter() {
  VoxMicPrep p;
  runPrep(p, stereoTone(1.0, 500, 2000, 0, 0));
  const int pk = p.takePeak();
  CHECK(pk > 1000, "meter peak %d", pk);
  CHECK(p.takePeak() == 0, "takePeak should reset");
}

// The full recording path: 16 kHz stereo mic samples of the synthetic speech,
// through VoxMicPrep, encoded. The clip must be a whole number of frames that
// decodes, and the prep must not add latency beyond a frame.
static void testPipeline() {
  std::vector<int16_t> pcm8(VOX_SYNTH_SAMPLES);
  voxSynth(pcm8.data());
  std::vector<int16_t> st(4 * VOX_SYNTH_SAMPLES);   // 16 kHz stereo (linear upsample)
  for (int i = 0; i < VOX_SYNTH_SAMPLES; i++) {
    const int a = pcm8[i], b = i + 1 < VOX_SYNTH_SAMPLES ? pcm8[i + 1] : a;
    const int16_t m = (int16_t)((a + b) / 2);
    st[4 * i + 0] = st[4 * i + 1] = (int16_t)(a / 8);   // a quiet mic
    st[4 * i + 2] = st[4 * i + 3] = (int16_t)(m / 8);
  }
  VoxMicPrep prep;
  VoxCodec2Encoder enc;
  CHECK(enc.begin(kVoxRecordWireMode), "encoder");
  std::vector<int16_t> pending;
  std::vector<uint8_t> bits;
  int16_t buf[512];
  uint8_t fr[16];
  for (size_t i = 0; i < st.size() / 2; i += 256) {
    const size_t k = st.size() / 2 - i < 256 ? st.size() / 2 - i : 256;
    const size_t n = prep.process(st.data() + 2 * i, k, buf, 512);
    pending.insert(pending.end(), buf, buf + n);
    while ((int)pending.size() >= enc.samplesPerFrame()) {
      enc.encode(pending.data(), fr);
      bits.insert(bits.end(), fr, fr + enc.bytesPerFrame());
      pending.erase(pending.begin(), pending.begin() + enc.samplesPerFrame());
    }
  }
  CHECK(bits.size() == (size_t)(VOX_SYNTH_SAMPLES / 320) * 6, "pipeline made %zu bytes", bits.size());
  CHECK(pending.empty(), "%zu samples left over", pending.size());
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char* dir = argc > 1 ? argv[1] : "test/data/vox";
  testEncoderModes();
  testEncoderMatchesApp(dir);
  testRoundTrip();
  testWaveform();
  testPrepDecimates();
  testPrepRemovesDcAndAliases();
  testPrepAgc();
  testPrepSilenceNotPumped();
  testPeakMeter();
  testPipeline();
  if (g_fail) {
    printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  printf("test_vox_record: all passed\n");
  return 0;
}
