// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoxMic.h"

#include "VoxVoice.h"

#if defined(HAS_TDECK_GT911) && VOX_CODEC2_AVAILABLE
#define VOX_MIC_TDECK 1
#else
#define VOX_MIC_TDECK 0
#endif

#if VOX_MIC_TDECK

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

namespace {

using VoxMic::State;
using VoxMic::kIdle;
using VoxMic::kRecording;
using VoxMic::kReady;
using VoxMic::kFailed;

// ---- ES7210 ------------------------------------------------------------------
// Register sequence from Espressif's ES7210 driver (ESP-ADF, MIT licence,
// Copyright (c) 2021 Espressif Systems (Shanghai) Co., Ltd.), as LilyGo ships
// it for the T-Deck (lib/es7210 in Xinyuan-LilyGO/T-Deck): slave mode, I2S
// format, 16-bit, 16 kHz from a 4.096 MHz MCLK (256 fs), all four ADCs powered
// with ADC1/2 on SDOUT1, the one data line the T-Deck wires (GPIO14).

constexpr uint8_t kEs7210Addr = 0x40;
constexpr int kPinMclk = 48, kPinBck = 47, kPinWs = 21, kPinDin = 14;
constexpr i2s_port_t kMicPort = I2S_NUM_1;   // I2S_NUM_0 is the speaker

// PGA gain for the mic inputs, in the ES7210's 3 dB steps (9 = 27 dB). The
// digital AGC in VoxMicPrep adds up to another 27 dB on top, so this only has
// to lift a normal voice clear of the ADC's noise floor without clipping a
// shout. Hardware-tune here if clips come out noisy (raise) or distorted (lower).
constexpr uint8_t kMicGain = 9;

bool wr(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(kEs7210Addr);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

int rd(uint8_t reg) {
  Wire.beginTransmission(kEs7210Addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((int)kEs7210Addr, 1) != 1) return -1;
  return Wire.read();
}

bool upd(uint8_t reg, uint8_t mask, uint8_t v) {
  const int cur = rd(reg);
  if (cur < 0) return false;
  return wr(reg, (uint8_t)((cur & ~mask) | (mask & v)));
}

// All four inputs on with their PGAs (es7210_mic_select with MIC1..MIC4).
bool micSelect() {
  bool ok = true;
  for (int i = 0; i < 4; i++) ok &= upd((uint8_t)(0x43 + i), 0x10, 0x00);
  ok &= wr(0x4B, 0xFF);
  ok &= wr(0x4C, 0xFF);
  ok &= upd(0x01, 0x0B, 0x00);
  ok &= wr(0x4B, 0x00);
  ok &= upd(0x01, 0x15, 0x00);
  ok &= wr(0x4C, 0x00);
  for (int i = 0; i < 4; i++) ok &= upd((uint8_t)(0x43 + i), 0x10, 0x10);
  return ok;
}

bool es7210Start() {
  bool ok = true;
  ok &= wr(0x00, 0xFF);   // reset
  ok &= wr(0x00, 0x41);
  ok &= wr(0x01, 0x1F);   // clocks off while configuring
  ok &= wr(0x09, 0x30);   // chip state cycle
  ok &= wr(0x0A, 0x30);   // power-on state cycle
  ok &= wr(0x40, 0xC3);   // analog: VDDA 3.3 V, VMID 5k start
  ok &= wr(0x41, 0x70);   // MIC1/2 bias 2.87 V
  ok &= wr(0x42, 0x70);   // MIC3/4 bias 2.87 V
  // 16 kHz from MCLK 4.096 MHz: adc_div 1, doubler on, DLL on, OSR 0x20, LRCK div 256.
  ok &= wr(0x02, 0xC1);
  ok &= wr(0x07, 0x20);
  ok &= wr(0x04, 0x01);
  ok &= wr(0x05, 0x00);
  // Format: 16-bit, I2S; ADC1/2 -> SDOUT1, ADC3/4 -> SDOUT2 (not TDM).
  const int sdp = rd(0x11);
  ok &= sdp >= 0 && wr(0x11, (uint8_t)(((sdp & 0x1F) | 0x60) & 0xFC));
  ok &= wr(0x12, 0x00);
  ok &= micSelect();
  for (int i = 0; i < 4; i++) ok &= upd((uint8_t)(0x43 + i), 0x0F, kMicGain);
  // Start: clocks on, power up, mics on, then select again (as es7210_start does).
  const int clk = rd(0x01);
  ok &= clk >= 0 && wr(0x01, (uint8_t)clk);
  ok &= wr(0x06, 0x00);
  for (int i = 0; i < 4; i++) ok &= wr((uint8_t)(0x47 + i), 0x00);
  ok &= micSelect();
  return ok;
}

void es7210Stop() {
  for (int i = 0; i < 4; i++) wr((uint8_t)(0x47 + i), 0xFF);
  wr(0x4B, 0xFF);
  wr(0x4C, 0xFF);
  wr(0x01, 0x7F);
  wr(0x06, 0x07);
}

bool i2sStart() {
  // LilyGo's tested T-Deck mic configuration (examples/Microphone), but with
  // both slots kept (MIC1 left, MIC2 right) rather than left copied to both.
  // captureTask measures what actually arrives and copes with mono too.
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = VoxMicPrep::kInputRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 128;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = 0;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;
  cfg.chan_mask = (i2s_channel_t)(I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1);
  if (i2s_driver_install(kMicPort, &cfg, 0, nullptr) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = kPinMclk;
  pins.bck_io_num = kPinBck;
  pins.ws_io_num = kPinWs;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = kPinDin;
  if (i2s_set_pin(kMicPort, &pins) != ESP_OK) {
    i2s_driver_uninstall(kMicPort);
    return false;
  }
  i2s_zero_dma_buffer(kMicPort);
  return true;
}

// ---- capture task --------------------------------------------------------------

constexpr size_t kMaxBits = (size_t)kVoxMaxRecordSeconds * 1000 / 40 * 8;   // 20 s at up to 8 B / 40 ms
constexpr uint32_t kSettleMs = 150;   // the ADC's power-up thump (also used to measure the stream)

volatile State s_state = kIdle;
volatile bool s_stop = false;
volatile bool s_cancel = false;
volatile bool s_task_running = false;
volatile uint32_t s_frames = 0;       // codec frames captured
volatile int s_level = 0;
const char* s_error = "";
uint8_t* s_bits = nullptr;
size_t s_len = 0;
bool s_codec_on = false;

// The capture task's stack, from internal RAM (FreeRTOS here won't take a
// PSRAM stack). Codec2's deepest encoder path measures ~15 KB (analyse_one_frame
// -> nlp -> FFT, -fstack-usage) and codec2_create ~10 KB; 40 KB was twice that.
constexpr uint32_t kTaskStack = 28 * 1024;   // ~18-20 KB used on Xtensa; the rest is margin
// Plus the I2S driver: 8 DMA buffers of 128 stereo frames (4 KB) and its state.
constexpr size_t kDriverBytes = 8 * 1024;
// What has to stay free for everything else while recording (Wi-Fi, BLE, LVGL).
constexpr size_t kHeadroom = 8 * 1024;
char s_mem_err[64];

void captureTask(void*) {
  VoxMicPrep prep;
  VoxCodec2Encoder enc;
  bool ok = enc.begin(kVoxRecordWireMode);
  if (!ok) s_error = "Codec2 couldn't start";
  const int spf = ok ? enc.samplesPerFrame() : 320;
  const int bpf = ok ? enc.bytesPerFrame() : 6;
  const size_t max_frames = ok ? (size_t)kVoxMaxRecordSeconds * 8000 / (size_t)spf : 0;
  static int16_t rx[2 * 256];         // 256 stereo frames = 16 ms
  static int16_t pcm8[2 * 256 / 2 + 2];   // 8 kHz out of one read, stereo or mono
  static int16_t frame[320];
  int have = 0;
  // The first kSettleMs are thrown away (the ADC's power-up thump). They also
  // tell us what the driver delivers: 16 kHz stereo is 64000 B/s, mono 32000.
  bool settling = true, mono = false;
  size_t settle_bytes = 0;
  int64_t settle_t0 = 0;
  // Drain what the DMA buffered before this task ran, so the measurement only
  // sees samples arriving in real time.
  for (int i = 0; ok && i < 64; i++) {
    size_t got = 0;
    if (i2s_read(kMicPort, rx, sizeof rx, &got, 0) != ESP_OK || got == 0) break;
  }
  while (ok && !s_stop && !s_cancel) {
    size_t got = 0;
    if (i2s_read(kMicPort, rx, sizeof rx, &got, pdMS_TO_TICKS(200)) != ESP_OK || got == 0) {
      s_error = "The microphone stopped";
      ok = false;
      break;
    }
    if (settling) {
      const int64_t t = esp_timer_get_time();
      if (!settle_t0) { settle_t0 = t; continue; }   // the first read may be stale DMA
      settle_bytes += got;
      const int64_t us = t - settle_t0;
      if (us < (int64_t)kSettleMs * 1000) continue;
      const int64_t bps = (int64_t)settle_bytes * 1000000 / us;
      mono = bps < 48000;
      settling = false;
      Serial.printf("[VOX] mic stream %lld B/s: %s\n", (long long)bps, mono ? "mono" : "stereo");
      continue;
    }
    const size_t n = mono ? prep.processMono(rx, got / sizeof(int16_t), pcm8, sizeof pcm8 / sizeof pcm8[0])
                          : prep.process(rx, got / (2 * sizeof(int16_t)), pcm8, sizeof pcm8 / sizeof pcm8[0]);
    for (size_t i = 0; i < n; i++) {
      frame[have++] = pcm8[i];
      if (have == spf) {
        have = 0;
        if (s_frames < max_frames && (s_frames + 1) * (size_t)bpf <= kMaxBits) {
          enc.encode(frame, s_bits + (size_t)s_frames * (size_t)bpf);
          s_frames = s_frames + 1;
        }
      }
    }
    const int pk = prep.takePeak();
    const int lv = pk >= 32000 ? 100 : pk * 100 / 32000;
    if (lv > s_level) s_level = lv;
    if (s_frames >= max_frames) break;   // 20 s: stop as if released
  }
  s_len = (size_t)s_frames * (size_t)bpf;
  enc.end();
  // How close the stack came to its end, for tuning kTaskStack.
  Serial.printf("[VOX] mic task stack: %u bytes unused of %u\n",
                (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)kTaskStack);
  s_state = s_cancel ? kIdle : ok ? kReady : kFailed;
  s_task_running = false;
  vTaskDelete(nullptr);
}

void shutdownHardware() {
  i2s_driver_uninstall(kMicPort);
  if (s_codec_on) es7210Stop();
  s_codec_on = false;
}

void dropClip() {
  if (s_bits) heap_caps_free(s_bits);
  s_bits = nullptr;
  s_len = 0;
  s_frames = 0;
}

// The capture task ends on its own (stop, cancel, 20 s, error); the hardware is
// released here, on the loop task, once it has.
void reap() {
  static bool hw_up = false;
  if (s_task_running) { hw_up = true; return; }
  if (hw_up) {
    shutdownHardware();
    hw_up = false;
  }
}

}  // namespace

namespace VoxMic {

bool available() { return true; }

bool start(const char** err) {
  reap();
  if (s_task_running || s_state == kRecording) { *err = "Already recording"; return false; }
  dropClip();
  s_error = "";
  // The capture task's stack and the I2S DMA buffers both come from internal
  // RAM; refuse cleanly rather than let the driver abort(). The stack needs one
  // contiguous block, so check the largest block as well as the total. (This
  // used to demand 72 KB free in total, more than a T-Deck with Wi-Fi, BLE
  // and the UI up usually has, so recording was refused on a healthy device.)
  const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t big_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (big_int < kTaskStack + 1024 || free_int < kTaskStack + kDriverBytes + kHeadroom) {
    snprintf(s_mem_err, sizeof s_mem_err, "Not enough memory to record (%uK free, %uK block)",
             (unsigned)(free_int / 1024), (unsigned)(big_int / 1024));
    Serial.printf("[VOX] %s\n", s_mem_err);
    *err = s_mem_err;
    return false;
  }
  s_bits = (uint8_t*)heap_caps_malloc(kMaxBits, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_bits) s_bits = (uint8_t*)heap_caps_malloc(kMaxBits, MALLOC_CAP_8BIT);
  if (!s_bits) { *err = "Not enough memory to record"; return false; }
  if (!es7210Start()) {
    es7210Stop();
    dropClip();
    *err = "Microphone not found";
    return false;
  }
  s_codec_on = true;
  if (!i2sStart()) {
    es7210Stop();
    s_codec_on = false;
    dropClip();
    *err = "Microphone busy";
    return false;
  }
  s_stop = s_cancel = false;
  s_level = 0;
  s_state = kRecording;
  s_task_running = true;
  if (xTaskCreate(captureTask, "vox_mic", kTaskStack, nullptr, 3, nullptr) != pdPASS) {
    s_task_running = false;
    s_state = kIdle;
    shutdownHardware();
    dropClip();
    *err = "Not enough memory to record";
    return false;
  }
  reap();   // note the hardware is up
  Serial.println("[VOX] recording");
  return true;
}

void stop() { s_stop = true; }

void cancel() {
  s_cancel = true;
  if (!s_task_running && s_state != kRecording) {
    dropClip();
    s_state = kIdle;
  }
}

State state() {
  reap();
  if (s_state == kIdle && s_bits && !s_task_running) dropClip();   // a cancelled recording
  return s_state;
}

const char* error() { return s_error; }

uint32_t elapsedMs() { return s_frames * 40u; }

int level() {
  const int v = s_level;
  s_level = v * 3 / 4;
  return v;
}

bool take(uint8_t* mode, uint8_t** bits, size_t* len, uint32_t* ms, uint32_t min_ms) {
  reap();
  if (s_state != kReady || !s_bits) return false;
  s_state = kIdle;
  const uint32_t dur = s_frames * 40u;
  Serial.printf("[VOX] recorded %lu ms, %u bytes\n", (unsigned long)dur, (unsigned)s_len);
  if (dur < min_ms || s_len == 0) {
    dropClip();
    return false;
  }
  *mode = (uint8_t)kVoxRecordWireMode;
  *bits = s_bits;
  *len = s_len;
  *ms = dur;
  s_bits = nullptr;   // the caller frees it
  s_len = 0;
  s_frames = 0;
  return true;
}

}  // namespace VoxMic

#else  // no microphone on this board

namespace VoxMic {
bool available() { return false; }
bool start(const char** err) { *err = "This radio has no microphone"; return false; }
void stop() {}
void cancel() {}
State state() { return kIdle; }
const char* error() { return ""; }
uint32_t elapsedMs() { return 0; }
int level() { return 0; }
bool take(uint8_t*, uint8_t**, size_t*, uint32_t*, uint32_t) { return false; }
}  // namespace VoxMic

#endif
