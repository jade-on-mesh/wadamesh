// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Runs one photo decode (VoxAv1Core) at a time on its own FreeRTOS task, so the
// UI and radio keep running while dav1d works.
//
//  - Core 0 at idle priority: anything else that wants the CPU gets it first,
//    and the core-0 idle task still runs, so the 5 s task watchdog stays fed
//    however long a decode takes.
//  - 64 KB stack from internal RAM for the duration of the decode only. dav1d's
//    self-guided loop filter keeps ~34 KB of scratch on the stack and the host
//    measures ~45 KB peak; this build can't place task stacks in PSRAM.
//  - Boards that idle at 80 MHz are lifted to 240 MHz for the decode.
//
// Poll from the UI loop; LVGL objects must only be touched there.

#include <stddef.h>
#include <stdint.h>

#include "VoxAv1Core.h"

namespace VoxAv1Job {

enum State : uint8_t { kIdle = 0, kRunning, kDone };

struct Stats {
  uint32_t decode_ms = 0;     // dav1d + colour conversion, wall clock
  uint32_t stack_used = 0;    // bytes of the decode task's stack touched
  uint32_t heap_peak = 0;     // decoder heap high-water during this decode
  uint32_t cpu_mhz = 0;       // clock the decode ran at
};

// Starts decoding `payload` (a whole photo payload: [0x01][AV1 OBUs]). The bytes
// are copied. False if a decode is already running or the copy can't be made.
bool start(const uint8_t* payload, size_t len, int max_side = 768);

State state();

// After kDone: the result, the picture (caller owns `img->pixels`, free with
// voxAv1Free) and timing. Returns the job to kIdle.
VoxAv1Result finish(VoxAv1Image* img, Stats* stats);

}  // namespace VoxAv1Job
