// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Firmware updates for THIS fork (jade-on-mesh/wadamesh-tak), not upstream Wadamesh.
//
// Upstream's About page checks firmware.wadamesh.com for the newest beta_N and
// installs it. A fork device doing that would replace itself with upstream
// firmware and lose the fork's features, so in this fork the same About page
// (badge, "Update available", Install update, previous versions, save to SD)
// follows the fork's own GitHub releases instead:
//
// Versions follow upstream: a fork build on top of Wadamesh beta_93 is
// "wadamesh-tak beta_93.1", then beta_93.2, ... and beta_94.1 after the next
// upstream release. Its tag is tak-beta_93.1 (made by fork-release.yml; not
// beta_*, which would start upstream's own release workflow).
//
//   latest    https://github.com/<repo>/releases/latest  -> 302 .../tag/tak-beta_93.2
//   firmware  https://github.com/<repo>/releases/download/tak-beta_93.2/wadamesh-<board>.bin
//   signature                                       ... /wadamesh-<board>.bin.sig
//
// Every image is signed in CI with the fork's Ed25519 key (repo secret
// FORK_SIGNING_KEY) and checked here against kForkUpdatePubKey before it is
// marked bootable or kept on the SD card. The signed message names the tag and
// the board's file, so a valid image for another board or another release is
// refused too. That check is what makes the download safe: the transport is
// HTTPS without certificate pinning (the device keeps no CA store).
//
// Pure logic, host-tested in test/test_fork_update.cpp.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WADA_FORK_RELEASES
#define WADA_FORK_RELEASES 1
#endif

#define WADA_FORK_REPO "jade-on-mesh/wadamesh-tak"
#define WADA_TAG_PREFIX "tak-"
#define WADA_BRAND "WADAMESH-TAK"
#define WADA_UPDATE_SITE "github.com/" WADA_FORK_REPO "/releases"

extern "C" int ed25519_verify(const unsigned char* signature, const unsigned char* message, size_t message_len,
                              const unsigned char* public_key);

namespace forkupd {

// Public half of the fork's release signing key. The private half lives only in
// the repo's Actions secrets. Rotating it means a release signed with the old key
// that ships the new one, then switching the secret.
static const uint8_t kForkUpdatePubKey[32] = {
    0x2d, 0x0f, 0xd5, 0xe9, 0x81, 0xe9, 0x21, 0x52, 0x76, 0xa6, 0x72, 0xc3, 0x5d, 0xd1, 0x9a, 0x96,
    0x23, 0x44, 0x71, 0x67, 0x49, 0x09, 0x4b, 0x18, 0x52, 0x68, 0x7d, 0xc2, 0x9e, 0x54, 0x6f, 0x94};

constexpr size_t kSigBytes = 64;

// A release as one comparable number: upstream beta N, fork build k -> N*1000+k.
constexpr int kBuildsPerBase = 1000;

// "beta_93.2", "tak-beta_93.2" or a URL ending in .../tag/tak-beta_93.2 -> 93002;
// -1 for anything else (an upstream "beta_93" has no fork build and is -1 too).
inline int releaseCode(const char* s) {
  if (!s) return -1;
  const char* p = strstr(s, "beta_");
  const char* last = nullptr;
  while (p) { last = p; p = strstr(p + 1, "beta_"); }
  if (!last) return -1;
  last += 5;
  auto num = [&](long* out) {
    if (*last < '0' || *last > '9') return false;
    long v = 0;
    while (*last >= '0' && *last <= '9') {
      v = v * 10 + (*last - '0');
      if (v > 100000) return false;
      ++last;
    }
    *out = v;
    return true;
  };
  long n = 0, k = 0;
  if (!num(&n) || *last != '.') return -1;
  ++last;
  if (!num(&k) || k < 1 || k >= kBuildsPerBase) return -1;
  // Only a clean end: "...93.2", "...93.2/", "...93.2?x", not "...93.2abc".
  if (*last && *last != '/' && *last != '?' && *last != '#' && *last != '\r' && *last != '\n') return -1;
  return (int)(n * kBuildsPerBase + k);
}

// 93002 -> "beta_93.2"
inline void formatRelease(int code, char* out, size_t cap) {
  if (code < 0) { snprintf(out, cap, "?"); return; }
  snprintf(out, cap, "beta_%d.%d", code / kBuildsPerBase, code % kBuildsPerBase);
}

// 93002 -> "tak-beta_93.2" (the release tag, and part of the signed message)
inline void tagFor(int code, char* out, size_t cap) {
  snprintf(out, cap, WADA_TAG_PREFIX "beta_%d.%d", code / kBuildsPerBase, code % kBuildsPerBase);
}

// The exact bytes the release workflow signs:
//   "wadamesh-fork-v1\n" tag "\n" file "\n" sha256(image)
inline size_t signedMessage(const char* tag, const char* file, const uint8_t digest[32], uint8_t* out, size_t cap) {
  static const char kHead[] = "wadamesh-fork-v1\n";
  const size_t a = sizeof kHead - 1, t = strlen(tag), f = strlen(file);
  const size_t need = a + t + 1 + f + 1 + 32;
  if (need > cap) return 0;
  uint8_t* o = out;
  memcpy(o, kHead, a); o += a;
  memcpy(o, tag, t); o += t; *o++ = '\n';
  memcpy(o, file, f); o += f; *o++ = '\n';
  memcpy(o, digest, 32);
  return need;
}

inline bool verify(const char* tag, const char* file, const uint8_t digest[32], const uint8_t sig[kSigBytes],
                   const uint8_t* pubkey = kForkUpdatePubKey) {
  uint8_t msg[160];
  const size_t n = signedMessage(tag, file, digest, msg, sizeof msg);
  return n && ed25519_verify(sig, msg, n, pubkey) == 1;
}

}  // namespace forkupd
