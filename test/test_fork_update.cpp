// Fork firmware updates: the device-side check against a signature made the way
// .github/workflows/fork-release.yml makes them (Python cryptography, Ed25519).
// The vector was signed with the fork's real release key, so this also proves
// kForkUpdatePubKey is that key's public half.
#include <stdio.h>
#include <string>
#include <vector>

#include "ui-touch/ForkUpdateCore.h"
#include "mbedtls_sha256_shim.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::vector<uint8_t> unhex(const std::string& h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)strtoul(h.substr(i, 2).c_str(), nullptr, 16));
  return v;
}

static const char* kSig = "05c26b6cb088faf955c7bc9b68f683c54e55ba9f4f233628e5a8f8823ca9b49b29906c8bec316f1b447948d9e065d8384d8b7d0f299a887d27adb6f878c26609";

int main() {
  // The image: bytes 0..255 repeated 40 times.
  std::vector<uint8_t> img;
  for (int r = 0; r < 40; r++) for (int i = 0; i < 256; i++) img.push_back((uint8_t)i);
  uint8_t d[32];
  sha256(img.data(), img.size(), d);
  auto sig = unhex(kSig);
  CHECK(sig.size() == 64);
  CHECK(forkupd::verify("tak_7", "wadamesh-tdeck.bin", d, sig.data()));
  // Another release, another board, a changed image: all refused.
  CHECK(!forkupd::verify("tak_8", "wadamesh-tdeck.bin", d, sig.data()));
  CHECK(!forkupd::verify("tak_7", "wadamesh-thinknode-m9.bin", d, sig.data()));
  img[100] ^= 1;
  sha256(img.data(), img.size(), d);
  CHECK(!forkupd::verify("tak_7", "wadamesh-tdeck.bin", d, sig.data()));

  CHECK(forkupd::releaseN("tak_12") == 12);
  CHECK(forkupd::releaseN("https://github.com/jade-on-mesh/wadamesh/releases/tag/tak_3") == 3);
  CHECK(forkupd::releaseN("https://github.com/x/releases/tag/tak_3\r\n") == 3);
  CHECK(forkupd::releaseN("beta_93") == -1);
  CHECK(forkupd::releaseN("tak_") == -1);
  CHECK(forkupd::releaseN("tak_4x") == -1);
  CHECK(forkupd::releaseN(nullptr) == -1);
  if (g_fail) { printf("%d check(s) failed\n", g_fail); return 1; }
  printf("fork update checks passed\n");
  return 0;
}
