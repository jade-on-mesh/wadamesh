// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A zeroed buffer in PSRAM where a `static` array used to sit in internal RAM.
//
// Internal RAM is what Wi-Fi, BLE, TLS and every task stack come from, and the
// T-Deck's image already takes ~240 KB of it statically. UI-only buffers don't
// need it, so instead of
//     static char buf[3200];
// write
//     static auto& buf = psramStatic<char[3200]>();
// `buf` stays an array reference: indexing and sizeof work as before. It is
// allocated, zeroed, on first use (a function-local static) or at start-up (a
// file-scope one, after PSRAM is up), and kept for good, like the static was.
// Falls back to internal RAM where there is no PSRAM.
//
// Only for plain data never touched from an ISR or while the flash cache is
// off (PSRAM is unreachable then).

#include <stdlib.h>
#include <type_traits>

#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

template <typename T>
inline T& psramStatic() {
  static_assert(std::is_trivially_default_constructible<typename std::remove_all_extents<T>::type>::value,
                "psramStatic: plain data only (it is zero-filled, not constructed)");
  void* p = nullptr;
#if defined(ESP32)
  p = heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if (!p) p = calloc(1, sizeof(T));
  if (!p) abort();   // as a static that didn't fit would have failed to link
  return *static_cast<T*>(p);
}

// The same for an object that has a constructor (default member initialisers,
// say): constructed in place, in PSRAM, and never destroyed, like the static.
//     static Ring s_ring;   ->   static Ring& s_ring = psramObject<Ring>();
#include <new>
template <typename T>
inline T& psramObject() {
  static_assert(!std::is_array<T>::value, "psramObject: one object; arrays of plain data: psramStatic");
  void* p = nullptr;
#if defined(ESP32)
  p = heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if (!p) p = calloc(1, sizeof(T));
  if (!p) abort();
  return *new (p) T();
}
