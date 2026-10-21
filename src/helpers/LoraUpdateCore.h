// SPDX-License-Identifier: GPL-3.0-or-later
//
// LoRa Update: the portable half. A standalone radio serves `.mota` firmware
// files from its SD card to a repeater that pulls them over LoRa (Mike
// Carper's LoRa OTA, docs in LORA_UPDATE.md). The OTA engine itself is his,
// vendored in helpers/ota/ (MIT); this file is what Wadamesh adds around it:
//
//  * MotaFileSource: an engine MotaSource over a folder of files. It describes
//    each file from its fixed 205-byte head and its 5-byte trailer, so an image
//    of any size is served without holding it in RAM.
//  * Status-line parsing for the repeater's `ota status` / `ota ls` replies.
//
// No Arduino here: the device supplies the file I/O (LoraUpdate.cpp) and the
// host test (test/test_lora_update.cpp) supplies memory-backed files.

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ota/MotaContainer.h"
#include "ota/OtaByteIO.h"
#include "ota/OtaFormat.h"
#include "ota/OtaSource.h"

namespace loraupdate {

using mesh::ota::MotaDesc;
using mesh::ota::MotaManifest;

static constexpr uint8_t kMaxFiles = 8;          // `.mota` files offered at once
static constexpr uint32_t kHeadLen = 8 + mesh::ota::MOTA_MFL;   // MAGIC, total, manifest
static constexpr uint32_t kTrailerLen = 5;
static constexpr const char* kFolder = "/mota";  // on the SD card

/** Random access to a set of files: the folder listing and reads. */
class FileIO {
public:
  virtual ~FileIO() {}
  virtual uint8_t count() = 0;
  virtual const char* name(uint8_t i) = 0;   // file name only, no folder
  virtual uint32_t size(uint8_t i) = 0;
  virtual bool read(uint8_t i, uint32_t off, uint8_t* buf, uint32_t len) = 0;
};

/** One served file, as described from its head. */
struct FileInfo {
  char name[48] = {0};
  MotaDesc desc;
  uint32_t image_size = 0;
  bool is_signed = false;
  bool bootloader = false;
  char hw[33] = {0};                 // the manifest's hardware tag, NUL-terminated
};

/** Describe a `.mota` of `size` bytes from its head and trailer. False when it
 *  is not a container this engine can serve (bad magic, length, trailer, or
 *  manifest). */
inline bool describeHead(const uint8_t head[kHeadLen], const uint8_t trailer[kTrailerLen], uint32_t size,
                         FileInfo& out) {
  out = FileInfo{};
  if (size < kHeadLen + kTrailerLen) return false;
  if (memcmp(head, mesh::ota::MOTA_MAGIC, 4) != 0) return false;
  if (mesh::ota::rd_u32le(head + 4) != size) return false;
  if (memcmp(trailer, mesh::ota::MOTA_TRAILER, kTrailerLen) != 0) return false;
  MotaManifest m;
  if (!mesh::ota::mota_parse_manifest(head + 8, mesh::ota::MOTA_MFL, m)) return false;
  const uint64_t leaves_off = kHeadLen;
  const uint64_t payload_off = leaves_off + 4ull * m.block_count;
  if (payload_off + m.payload_size + kTrailerLen != size) return false;
  MotaDesc& d = out.desc;
  memcpy(d.mid, m.merkle_root, 4);
  d.target_id = m.target_id;
  d.fw_version = m.fw_version;
  d.codec_id = m.codec_id;
  d.flags = m.flags;
  d.block_size_log2 = m.block_size_log2;
  d.source_caps = 0;               // no pre-compressed blocks: the engine sends raw
  d.total_size = size;
  d.leaves_off = (uint32_t)leaves_off;
  d.block_count = m.block_count;
  d.payload_off = (uint32_t)payload_off;
  d.payload_size = m.payload_size;
  out.image_size = m.image_size;
  out.is_signed = m.is_signed();
  out.bootloader = m.is_bootloader();
  memcpy(out.hw, m.hw_id, 32);
  out.hw[32] = 0;
  return true;
}

/** The manifest id as the repeater's CLI writes it (`ota pull <mid8>`). */
inline void midHex(const uint8_t mid[4], char out[9]) {
  snprintf(out, 9, "%02X%02X%02X%02X", mid[0], mid[1], mid[2], mid[3]);
}

/** A firmware version as the repeater prints it (OtaCli ver_str): "v1.2.3",
 *  or "v1.2.3.4" with a prerelease byte. */
inline void versionText(uint32_t v, char* out, size_t cap) {
  const mesh::ota::FwVersion fw = mesh::ota::FwVersion::unpack(v);
  if (fw.prerelease) snprintf(out, cap, "v%u.%u.%u.%u", fw.major, fw.minor, fw.patch, fw.prerelease);
  else snprintf(out, cap, "v%u.%u.%u", fw.major, fw.minor, fw.patch);
}

static inline bool endsWithMota(const char* n) {
  const size_t l = strlen(n);
  if (l < 6) return false;
  const char* e = n + l - 5;
  return (e[0] == '.') && ((e[1] | 32) == 'm') && ((e[2] | 32) == 'o') && ((e[3] | 32) == 't') && ((e[4] | 32) == 'a');
}

/** The engine's view of the folder: every valid `.mota` in it, at most
 *  kMaxFiles, each described once on scan(). Reads go straight to the file. */
class MotaFileSource : public mesh::ota::MotaSource {
public:
  explicit MotaFileSource(FileIO& io) : _io(io) {}

  /** Re-read the folder. Returns how many files can be served. Files that
   *  are not valid containers are counted in skipped(). Two files with the
   *  same content id are offered once. */
  uint8_t scan() {
    _n = 0;
    _skipped = 0;
    const uint8_t n = _io.count();
    for (uint8_t i = 0; i < n; i++) {
      const char* nm = _io.name(i);
      if (!nm || !endsWithMota(nm)) continue;
      const uint32_t sz = _io.size(i);
      uint8_t head[kHeadLen], tr[kTrailerLen];
      FileInfo fi;
      if (sz < kHeadLen + kTrailerLen || !_io.read(i, 0, head, kHeadLen) ||
          !_io.read(i, sz - kTrailerLen, tr, kTrailerLen) || !describeHead(head, tr, sz, fi)) {
        _skipped++;
        continue;
      }
      bool dup = false;
      for (uint8_t k = 0; k < _n; k++) dup |= memcmp(_info[k].desc.mid, fi.desc.mid, 4) == 0;
      if (dup) continue;
      if (_n >= kMaxFiles) { _skipped++; continue; }
      snprintf(fi.name, sizeof fi.name, "%s", nm);
      _info[_n] = fi;
      _file[_n] = i;
      _n++;
    }
    return _n;
  }

  uint8_t skipped() const { return _skipped; }
  const FileInfo* info(uint8_t i) const { return i < _n ? &_info[i] : nullptr; }

  /** Offer only file `i` (0xFF: all of them). The repeater lists what we
   *  offer, so offering just the chosen file keeps its `ota ls` short. */
  void only(uint8_t i) { _only = i; }

  // ---- MotaSource ----
  uint8_t count() override { return _only == 0xFF ? _n : (_only < _n ? 1 : 0); }
  bool describe(uint8_t idx, MotaDesc& out) override {
    const int k = slot(idx);
    if (k < 0) return false;
    out = _info[k].desc;
    return true;
  }
  bool read(uint8_t idx, uint32_t off, uint8_t* buf, uint32_t len) override {
    const int k = slot(idx);
    if (k < 0 || (uint64_t)off + len > _info[k].desc.total_size) return false;
    return _io.read(_file[k], off, buf, len);
  }

private:
  int slot(uint8_t idx) const {
    if (_only == 0xFF) return idx < _n ? idx : -1;
    return (idx == 0 && _only < _n) ? _only : -1;
  }

  FileIO& _io;
  FileInfo _info[kMaxFiles];
  uint8_t _file[kMaxFiles] = {};
  uint8_t _n = 0;
  uint8_t _skipped = 0;
  uint8_t _only = 0xFF;
};

// ---- the repeater's replies ------------------------------------------------------

/** What `ota status` said about a download. */
struct DownloadStatus {
  bool known = false;      // a blocks figure was found
  uint32_t have = 0, total = 0;
  int percent = -1;
  char mid[9] = {0};       // `id=XXXXXXXX`, when given
  bool ready = false;      // "ready to install"
  bool failed = false;     // "download: failed"
};

/** Parse an `ota status` reply the way the app does (lora_ota_screen.dart):
 *  the first "<have>/<total>" with an optional " (<n>%)", `id=<8 hex>`, and
 *  the "ready to install" / "download: failed" phrases. */
inline DownloadStatus parseStatus(const char* t) {
  DownloadStatus s;
  if (!t) return s;
  for (const char* p = t; *p; p++) {
    if (*p < '0' || *p > '9') continue;
    if (p > t && p[-1] >= '0' && p[-1] <= '9') continue;   // start of a number only
    char* e;
    const unsigned long a = strtoul(p, &e, 10);
    if (*e != '/' || e[1] < '0' || e[1] > '9') continue;
    char* e2;
    const unsigned long b = strtoul(e + 1, &e2, 10);
    s.known = true;
    s.have = (uint32_t)a;
    s.total = (uint32_t)b;
    if (e2[0] == ' ' && e2[1] == '(') {
      char* e3;
      const unsigned long pc = strtoul(e2 + 2, &e3, 10);
      if (e3 != e2 + 2 && *e3 == '%' && pc <= 100) s.percent = (int)pc;
    }
    if (s.percent < 0 && b > 0) s.percent = (int)(a * 100ull / b);
    break;
  }
  for (const char* p = t; (p = strstr(p, "id=")) != nullptr; p += 3) {
    if (p > t && ((p[-1] | 32) >= 'a' && (p[-1] | 32) <= 'z')) continue;   // a word boundary
    bool hex = true;
    for (int i = 0; i < 8; i++) {
      const char c = p[3 + i];
      hex &= (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'f');
    }
    const char after = p[11];
    if (!hex || ((after | 32) >= 'a' && (after | 32) <= 'z') || (after >= '0' && after <= '9')) continue;
    for (int i = 0; i < 8; i++) {
      const char c = p[3 + i];
      s.mid[i] = (c >= 'a' && c <= 'f') ? (char)(c - 32) : c;
    }
    s.mid[8] = 0;
    break;
  }
  char low[256];
  size_t n = 0;
  for (; t[n] && n < sizeof low - 1; n++) low[n] = (char)((t[n] >= 'A' && t[n] <= 'Z') ? t[n] + 32 : t[n]);
  low[n] = 0;
  s.ready = strstr(low, "ready to install") != nullptr;
  s.failed = strstr(low, "download: failed") != nullptr;
  return s;
}

/** True when a reply starts with "ok" (any case), after leading spaces. */
inline bool replyOk(const char* t) {
  if (!t) return false;
  while (*t == ' ') t++;
  return (t[0] | 32) == 'o' && (t[1] | 32) == 'k';
}

/** True when a reply is empty or an error ("err...", "(err..."). */
inline bool replyError(const char* t) {
  if (!t) return true;
  while (*t == ' ') t++;
  if (!*t) return true;
  if (*t == '(') t++;
  return (t[0] | 32) == 'e' && (t[1] | 32) == 'r' && (t[2] | 32) == 'r';
}

/** Whether `ota ls` lists content id `mid8` (8 hex, any case). */
inline bool listsMid(const char* reply, const char* mid8) {
  if (!reply || !mid8) return false;
  for (const char* p = reply; *p; p++) {
    bool m = true;
    for (int i = 0; i < 8 && m; i++) m = p[i] && ((p[i] | 32) == (mid8[i] | 32));
    if (m) return true;
  }
  return false;
}

}  // namespace loraupdate
