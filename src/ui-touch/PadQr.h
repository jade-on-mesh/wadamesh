// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A one-time pad as a series of QR codes, so MeshCore Open TAK can scan a pad
// straight off this screen (OTP Pad > Scan QR). A pad of a few KB is far too
// dense for one QR code on a small screen, so it is cut into parts:
//
//   OTPQR1:<i>:<n>:<CRC>:<HEX>
//
//   i     part number, 1..n
//   n     number of parts
//   CRC   CRC-32 (IEEE, as zlib) of the whole pad's bytes, 8 hex digits: ties
//         the parts to one pad and checks the reassembled result
//   HEX   this part's slice of the pad as uppercase hex (an even length; the
//         parts in order give the whole pad)
//
// Everything is digits, uppercase letters and ':', so QR alphanumeric mode
// carries it (5.5 bits a character rather than 8). The app reads the same
// format (lib/services/otp_pad_qr.dart) and still takes a plain hex QR, the
// older single-code form, as one part. Pure logic, host-tested in
// test/test_pad_qr.cpp.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace padqr {

constexpr const char* kPrefix = "OTPQR1";
// "OTPQR1:999:999:XXXXXXXX:"
constexpr size_t kHeaderMax = 24;

// Alphanumeric characters a QR code holds at error correction L, by version.
static const uint16_t kAlnumCapL[21] = {0,   25,  47,  77,  114, 154, 195, 224, 279,  335, 395,
                                        468, 535, 619, 667, 758, 854, 938, 1046, 1153, 1249};

inline uint32_t crc32Update(uint32_t crc, uint8_t b) {
  crc ^= b;
  for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  return crc;
}

inline int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// CRC-32 of the bytes a hex string spells (its length must be even).
inline uint32_t crc32Hex(const char* hex, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i + 1 < len; i += 2) crc = crc32Update(crc, (uint8_t)(hexVal(hex[i]) << 4 | hexVal(hex[i + 1])));
  return ~crc;
}

inline int modules(int version) { return 17 + 4 * version; }

// The largest version (up to max_version) whose symbol, with a 2-module quiet
// zone on each side, fits `px` at `scale` pixels a module; 0 if none does.
inline int versionFor(int px, int scale, int max_version = 13) {
  if (max_version > 20) max_version = 20;
  for (int v = max_version; v >= 1; v--)
    if ((modules(v) + 4) * scale <= px) return v;
  return 0;
}

// Pad bytes per part at a version.
inline size_t chunkBytes(int version) {
  if (version < 1 || version > 20 || kAlnumCapL[version] <= kHeaderMax) return 0;
  return (kAlnumCapL[version] - kHeaderMax) / 2;
}

inline int partsFor(size_t pad_bytes, size_t chunk) {
  if (!chunk || !pad_bytes) return 0;
  return (int)((pad_bytes + chunk - 1) / chunk);
}

// Part i (1-based) of n, from the pad's hex. Returns the text's length (0 if
// it doesn't fit `cap`).
inline size_t frame(const char* hex, size_t hex_len, uint32_t crc, int i, int n, size_t chunk, char* out, size_t cap) {
  if (i < 1 || i > n || !chunk) return 0;
  const size_t from = (size_t)(i - 1) * chunk * 2;
  if (from >= hex_len) return 0;
  size_t take = chunk * 2;
  if (from + take > hex_len) take = hex_len - from;
  const int h = snprintf(out, cap, "%s:%d:%d:%08X:", kPrefix, i, n, (unsigned)crc);
  if (h < 0 || (size_t)h + take + 1 > cap) return 0;
  for (size_t k = 0; k < take; k++) {
    char c = hex[from + k];
    if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
    out[h + k] = c;
  }
  out[h + take] = 0;
  return (size_t)h + take;
}

// Reads a part back: its number, the count, the CRC and where its hex is.
inline bool parse(const char* s, int* i, int* n, uint32_t* crc, const char** hex, size_t* hex_len) {
  const size_t pl = strlen(kPrefix);
  if (strncmp(s, kPrefix, pl) || s[pl] != ':') return false;
  const char* p = s + pl + 1;
  auto num = [&](long* v, char end) {
    long x = 0;
    int d = 0;
    while (*p >= '0' && *p <= '9' && d < 4) { x = x * 10 + (*p - '0'); p++; d++; }
    if (!d || *p != end) return false;
    p++;
    *v = x;
    return true;
  };
  long a, b;
  if (!num(&a, ':') || !num(&b, ':') || a < 1 || b < 1 || a > b) return false;
  uint32_t c = 0;
  for (int k = 0; k < 8; k++) {
    const int v = hexVal(p[k]);
    if (v < 0) return false;
    c = c << 4 | (uint32_t)v;
  }
  if (p[8] != ':') return false;
  p += 9;
  const size_t len = strlen(p);
  if (!len || len % 2) return false;
  for (size_t k = 0; k < len; k++)
    if (hexVal(p[k]) < 0) return false;
  *i = (int)a;
  *n = (int)b;
  *crc = c;
  *hex = p;
  *hex_len = len;
  return true;
}

}  // namespace padqr
