// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoxAv1Job.h"

#ifdef ESP_PLATFORM

#include <Arduino.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "VoxWire.h"

namespace VoxAv1Job {
namespace {

constexpr uint32_t kStackBytes = 24 * 1024;

std::atomic<uint8_t> g_state{kIdle};
uint8_t* g_payload = nullptr;
size_t g_len = 0;
int g_max_side = 768;
VoxAv1Result g_result = kVoxAv1DecodeFailed;
VoxAv1Image g_img;
Stats g_stats;

void decodeTask(void*) {
  const uint32_t mhz_before = getCpuFrequencyMhz();
  const bool boosted = mhz_before < 240 && setCpuFrequencyMhz(240);

  const int64_t t0 = esp_timer_get_time();
  voxAv1HeapResetPeak();
  const size_t heap_before = voxAv1HeapInUse();

  size_t obu_len = 0;
  const uint8_t* obus = VoxWire::imageObus(g_payload, g_len, &obu_len);
  g_result = obus ? voxAv1Decode(obus, obu_len, kVoxAv1Rgb565, g_max_side, &g_img) : kVoxAv1BadInput;

  g_stats.decode_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
  g_stats.heap_peak = (uint32_t)(voxAv1HeapPeak() - heap_before);
  g_stats.cpu_mhz = getCpuFrequencyMhz();
  // ESP-IDF reports the high-water mark in bytes.
  g_stats.stack_used = kStackBytes - (uint32_t)uxTaskGetStackHighWaterMark(nullptr);

  // Only undo our own boost: if something else changed the clock meanwhile, leave it.
  if (boosted && getCpuFrequencyMhz() == 240) setCpuFrequencyMhz(mhz_before);

  voxAv1Free(g_payload);
  g_payload = nullptr;
  g_state.store(kDone);
  vTaskDelete(nullptr);
}

}  // namespace

bool start(const uint8_t* payload, size_t len, int max_side) {
  if (!payload || len < 2) return false;
  uint8_t expected = kIdle;
  if (!g_state.compare_exchange_strong(expected, kRunning)) return false;

  g_payload = (uint8_t*)voxAv1Alloc(len);
  if (!g_payload) { g_state.store(kIdle); return false; }
  memcpy(g_payload, payload, len);
  g_len = len;
  g_max_side = max_side;
  g_img = VoxAv1Image{};
  g_stats = Stats{};
  g_result = kVoxAv1DecodeFailed;
  g_stats.stack_size = kStackBytes;
  g_stats.internal_largest =
      (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g_stats.psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  if (xTaskCreatePinnedToCore(decodeTask, "vox_av1", kStackBytes, nullptr, tskIDLE_PRIORITY,
                              nullptr, 0) != pdPASS) {
    // Not enough internal RAM for the decode stack right now.
    voxAv1Free(g_payload);
    g_payload = nullptr;
    g_result = kVoxAv1NoStack;
    g_state.store(kDone);
  }
  return true;
}

State state() { return (State)g_state.load(); }

VoxAv1Result finish(VoxAv1Image* img, Stats* stats) {
  if (g_state.load() != kDone) return kVoxAv1DecodeFailed;
  if (img) *img = g_img;
  else voxAv1Free(g_img.pixels);
  if (stats) *stats = g_stats;
  const VoxAv1Result r = g_result;
  g_img = VoxAv1Image{};
  g_state.store(kIdle);
  return r;
}

}  // namespace VoxAv1Job

#endif  // ESP_PLATFORM
