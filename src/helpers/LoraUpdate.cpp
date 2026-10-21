// SPDX-License-Identifier: GPL-3.0-or-later
// LoRa Update: SD file access and the engine's lifetime. See LoraUpdate.h.

#include "LoraUpdate.h"

#if defined(ESP32)

#include <Arduino.h>
#include <FS.h>
#include <esp_heap_caps.h>

#include <new>

#include "ota/OtaManager.h"

namespace loraupdate {
namespace {

// ---- the /mota folder on the card -------------------------------------------

class SdFileIO : public FileIO {
public:
  static constexpr uint8_t kMaxEntries = 16;   // files looked at per scan

  void list(fs::FS* fs) {
    close();
    _fs = fs;
    _n = 0;
    if (!fs) return;
    File dir = fs->open(kFolder);
    if (!dir || !dir.isDirectory()) return;
    for (File f = dir.openNextFile(); f && _n < kMaxEntries; f = dir.openNextFile()) {
      if (f.isDirectory()) continue;
      const char* full = f.name();
      const char* base = strrchr(full, '/');
      base = base ? base + 1 : full;
      if (!base[0] || base[0] == '.') continue;   // macOS ._ files and the like
      snprintf(_name[_n], sizeof _name[_n], "%s", base);
      _size[_n] = (uint32_t)f.size();
      _n++;
    }
  }
  void close() {
    if (_open) _f.close();
    _open = false;
    _open_idx = 0xFF;
  }

  uint8_t count() override { return _n; }
  const char* name(uint8_t i) override { return i < _n ? _name[i] : nullptr; }
  uint32_t size(uint8_t i) override { return i < _n ? _size[i] : 0; }
  bool read(uint8_t i, uint32_t off, uint8_t* buf, uint32_t len) override {
    if (!_fs || i >= _n) return false;
    if (!_open || _open_idx != i) {   // a session reads one file: keep it open
      close();
      char path[80];
      snprintf(path, sizeof path, "%s/%s", kFolder, _name[i]);
      _f = _fs->open(path, "r");
      if (!_f) return false;
      _open = true;
      _open_idx = i;
    }
    if (!_f.seek(off)) { close(); return false; }
    const size_t got = _f.read(buf, len);
    if (got != len) { close(); return false; }
    return true;
  }

private:
  fs::FS* _fs = nullptr;
  char _name[kMaxEntries][48] = {};
  uint32_t _size[kMaxEntries] = {};
  uint8_t _n = 0;
  File _f;
  bool _open = false;
  uint8_t _open_idx = 0xFF;
};

SdFileIO s_io;
MotaFileSource s_src(s_io);
mesh::ota::OtaManager* s_mgr = nullptr;   // PSRAM, while a session is open
uint8_t s_self[4] = {};
SendFn s_send = nullptr;

bool engineSend(void*, const uint8_t* msg, uint16_t len, bool) {
  return s_send && s_send(msg, len);
}

}  // namespace

void begin(const uint8_t self_pub[32], SendFn send) {
  memcpy(s_self, self_pub, 4);
  s_send = send;
}

uint8_t scan(fs::FS* sd) {
  if (s_mgr) return fileCount();   // serving: a rescan would renumber the open file
  s_io.list(sd);
  return s_src.scan();
}

uint8_t fileCount() {
  uint8_t n = 0;
  while (s_src.info(n)) n++;
  return n;
}
const FileInfo* file(uint8_t i) { return s_src.info(i); }
uint8_t skipped() { return s_src.skipped(); }

bool start(uint8_t i) {
  stop();
  if (!s_src.info(i)) return false;
  void* mem = heap_caps_malloc(sizeof(mesh::ota::OtaManager), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!mem) mem = malloc(sizeof(mesh::ota::OtaManager));
  if (!mem) return false;
  s_mgr = new (mem) mesh::ota::OtaManager();
  s_mgr->begin(0, engineSend, nullptr);   // target 0: we serve, never fetch
  s_mgr->set_seeder_id(s_self);
  s_mgr->set_max_hops(1);                 // the repeater is a neighbour
  s_mgr->set_clock(millis());
  s_src.only(i);
  if (!s_mgr->add_source(&s_src)) {
    stop();
    return false;
  }
  s_mgr->announce();
  return true;
}

void announce() {
  if (s_mgr) s_mgr->announce();
}

void stop() {
  if (s_mgr) {
    s_mgr->clear_sources();
    s_mgr->~OtaManager();
    heap_caps_free(s_mgr);
    s_mgr = nullptr;
  }
  s_src.only(0xFF);
  s_io.close();
}

bool active() { return s_mgr != nullptr; }

void onPacket(const uint8_t* payload, uint16_t len, uint8_t hops) {
  if (!s_mgr || !payload || len == 0 || hops > 1) return;
  s_mgr->set_clock(millis());
  s_mgr->note_rx_path_hops(hops);
  s_mgr->on_message(payload, len);
}

void loop() {
  if (!s_mgr) return;
  s_mgr->set_clock(millis());
  s_mgr->loop();
  s_mgr->serviceEgress();
}

uint32_t packetsSent() { return s_mgr ? s_mgr->packetsSent() : 0; }

}  // namespace loraupdate

#else   // not ESP32: nothing to serve from

namespace loraupdate {
void begin(const uint8_t*, SendFn) {}
uint8_t scan(fs::FS*) { return 0; }
uint8_t fileCount() { return 0; }
const FileInfo* file(uint8_t) { return nullptr; }
uint8_t skipped() { return 0; }
bool start(uint8_t) { return false; }
void announce() {}
void stop() {}
bool active() { return false; }
void onPacket(const uint8_t*, uint16_t, uint8_t) {}
void loop() {}
uint32_t packetsSent() { return 0; }
}  // namespace loraupdate

#endif
