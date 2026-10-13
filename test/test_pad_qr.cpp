// SPDX-License-Identifier: GPL-3.0-or-later
// Pad QR parts: PadQr.h against golden text that MeshCore Open TAK's test
// (test/services/otp_pad_qr_test.dart) checks its reader and writer against.
//
//   g++ -std=c++17 -Wall -I src test/test_pad_qr.cpp -o /tmp/q && /tmp/q
// With LVGL's qrcodegen at hand it also checks every part really fits its
// version as an alphanumeric code at error correction L:
//   gcc -c -DLV_CONF_SKIP <lvgl>/src/extra/libs/qrcode/qrcodegen.c -o /tmp/qrgen.o
//   g++ -std=c++17 -Wall -I src -DQRGEN=<lvgl>/src/extra/libs/qrcode test/test_pad_qr.cpp /tmp/qrgen.o -o /tmp/q
#include <stdio.h>
#include <string.h>

#include <string>

#include "ui-touch/PadQr.h"

#ifdef QRGEN
#define STR2(x) #x
#define STR(x) STR2(x)
#include STR(QRGEN/qrcodegen.h)
#endif

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b))) { fprintf(stderr, "FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); g_fail++; } } while (0)

int main() {
  // Golden: bytes 00..13 in parts of 8 bytes.
  const char* hex = "000102030405060708090a0b0c0d0e0f10111213";
  const size_t hl = strlen(hex);
  const uint32_t crc = padqr::crc32Hex(hex, hl);
  CHECK(crc == 0x3BDDFFA4u);
  CHECK(padqr::partsFor(20, 8) == 3);
  char out[128];
  CHECK(padqr::frame(hex, hl, crc, 1, 3, 8, out, sizeof out));
  CHECK_STR(out, "OTPQR1:1:3:3BDDFFA4:0001020304050607");
  CHECK(padqr::frame(hex, hl, crc, 2, 3, 8, out, sizeof out));
  CHECK_STR(out, "OTPQR1:2:3:3BDDFFA4:08090A0B0C0D0E0F");
  CHECK(padqr::frame(hex, hl, crc, 3, 3, 8, out, sizeof out));
  CHECK_STR(out, "OTPQR1:3:3:3BDDFFA4:10111213");
  CHECK(!padqr::frame(hex, hl, crc, 4, 3, 8, out, sizeof out));
  CHECK(!padqr::frame(hex, hl, crc, 1, 3, 8, out, 20));   // too small

  int i, n;
  uint32_t c;
  const char* h;
  size_t len;
  CHECK(padqr::parse("OTPQR1:3:3:3BDDFFA4:10111213", &i, &n, &c, &h, &len));
  CHECK(i == 3 && n == 3 && c == 0x3BDDFFA4u && len == 8 && !strcmp(h, "10111213"));
  CHECK(!padqr::parse("OTPQR1:4:3:3BDDFFA4:10111213", &i, &n, &c, &h, &len));
  CHECK(!padqr::parse("OTPQR1:1:3:3BDDFFA4:1011121", &i, &n, &c, &h, &len));
  CHECK(!padqr::parse("OTPQR2:1:3:3BDDFFA4:10111213", &i, &n, &c, &h, &len));
  CHECK(!padqr::parse("0001020304050607", &i, &n, &c, &h, &len));

  // Sizes: the T-Deck's 3-pixel modules in about 196 px give version 11.
  CHECK(padqr::versionFor(196, 3) == 11);
  CHECK(padqr::versionFor(400, 4) == 13);   // capped
  CHECK(padqr::versionFor(50, 3) == 0);
  CHECK(padqr::chunkBytes(11) == 222);
  CHECK(padqr::partsFor(1000, 222) == 5);
  CHECK(padqr::partsFor(8200, 222) == 37);

  // A whole 8200-byte pad round trip at version 11.
  std::string pad;
  for (int k = 0; k < 8200; k++) {
    char b[3];
    snprintf(b, sizeof b, "%02x", (k * 37 + 11) & 0xFF);
    pad += b;
  }
  const uint32_t pc = padqr::crc32Hex(pad.data(), pad.size());
  const size_t chunk = padqr::chunkBytes(11);
  const int parts = padqr::partsFor(8200, chunk);
  std::string back;
  for (int k = 1; k <= parts; k++) {
    char f[700];
    const size_t fl = padqr::frame(pad.data(), pad.size(), pc, k, parts, chunk, f, sizeof f);
    CHECK(fl && fl <= padqr::kAlnumCapL[11]);
    CHECK(padqr::parse(f, &i, &n, &c, &h, &len) && i == k && n == parts && c == pc);
    back.append(h, len);
#ifdef QRGEN
    for (int v = 10; v <= 13; v++) {
      if (k > 1) break;
      char g[700];
      padqr::frame(pad.data(), pad.size(), pc, 1, 999, padqr::chunkBytes(v), g, sizeof g);   // the longest header
      uint8_t tmp[qrcodegen_BUFFER_LEN_MAX], qr[qrcodegen_BUFFER_LEN_MAX];
      CHECK(qrcodegen_encodeText(g, tmp, qr, qrcodegen_Ecc_LOW, v, v, qrcodegen_Mask_AUTO, false));
      CHECK(qrcodegen_getSize(qr) == padqr::modules(v));
    }
#endif
  }
  for (char& ch : pad) if (ch >= 'a' && ch <= 'f') ch = (char)(ch - 'a' + 'A');
  CHECK(back == pad);
  CHECK(padqr::crc32Hex(back.data(), back.size()) == pc);

  if (g_fail) { fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
  printf("pad qr: all checks passed\n");
  return 0;
}
