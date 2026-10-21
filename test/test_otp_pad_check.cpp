// SPDX-License-Identifier: GPL-3.0-or-later
// Checking a pad by sound (OtpCore: padFingerprint, the check link). The
// vectors are shared with MeshCore Open TAK (test/otp/otp_pad_check_test.dart):
// both must give the same code for the same pad.
#include <stdio.h>
#include <string.h>

#include "ui-touch/OtpCore.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void hexOf(const uint8_t fp[8], char out[17]) { otp::toHex(fp, 8, out); }

int main() {
  uint8_t pad[4096], fp[8];
  char hex[17];
  for (int i = 0; i < 256; i++) pad[i] = (uint8_t)i;
  otp::padFingerprint(pad, 256, fp);
  hexOf(fp, hex);
  CHECK(!strcmp(hex, "73fcdbde9589d5a0"));
  for (int i = 0; i < 4096; i++) pad[i] = (uint8_t)((i * 7 + 3) & 255);
  otp::padFingerprint(pad, 4096, fp);
  hexOf(fp, hex);
  CHECK(!strcmp(hex, "a027f73305d4958f"));

  // One byte different, or one byte fewer: a different code.
  uint8_t fp2[8];
  pad[1000] ^= 1;
  otp::padFingerprint(pad, 4096, fp2);
  CHECK(memcmp(fp, fp2, 8) != 0);
  pad[1000] ^= 1;
  otp::padFingerprint(pad, 4095, fp2);
  CHECK(memcmp(fp, fp2, 8) != 0);

  // The link and the code to read out.
  char link[80], code[20];
  CHECK(otp::padCheckLink(fp, 4096, link, sizeof link));
  CHECK(!strcmp(link, "meshcore://otp/check?fp=a027f73305d4958f&n=4096"));
  uint8_t back[8];
  uint32_t n = 0;
  CHECK(otp::parsePadCheck(link, back, &n) && n == 4096 && !memcmp(back, fp, 8));
  CHECK(otp::parsePadCheck("MESHCORE://otp/check?n=12&fp=A027F73305D4958F", back, &n) && n == 12 && !memcmp(back, fp, 8));
  CHECK(!otp::parsePadCheck("meshcore://otp/check?fp=a027", back, &n));
  CHECK(!otp::parsePadCheck("meshcore://channel/add?fp=a027f73305d4958f", back, &n));
  CHECK(!otp::parsePadCheck("meshcore://otp/check?fp=zz27f73305d4958f&n=1", back, &n));
  CHECK(!otp::padCheckLink(fp, 4096, link, 20));
  otp::padCheckCode(fp, code);
  CHECK(!strcmp(code, "a027 f733 05d4 958f"));

  if (g_fail) { fprintf(stderr, "%d failure(s)\n", g_fail); return 1; }
  printf("otp pad check: all passed\n");
  return 0;
}
