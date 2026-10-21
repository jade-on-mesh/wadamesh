// Host stand-in for the core's Utils.h: just the SHA-256 the OTA engine uses
// (Multihash.h), backed by a real implementation so merkle checks are meaningful.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "SHA256.h"

#ifndef MAX_PACKET_PAYLOAD
#define MAX_PACKET_PAYLOAD 184   // the core's MeshCore.h (Utils.h includes it on device)
#endif

namespace mesh {
class Utils {
public:
  static void sha256(uint8_t* hash, size_t hash_len, const uint8_t* msg, int msg_len) {
    SHA256 s; s.update(msg, msg_len); s.finalize(hash, hash_len);
  }
  static void sha256(uint8_t* hash, size_t hash_len, const uint8_t* a, int a_len, const uint8_t* b, int b_len) {
    SHA256 s; s.update(a, a_len); s.update(b, b_len); s.finalize(hash, hash_len);
  }
};
}  // namespace mesh
