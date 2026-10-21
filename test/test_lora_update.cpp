// Host test for LoRa Update: src/helpers/LoraUpdateCore.h around the vendored
// OTA engine (src/helpers/ota, Mike Carper, MIT). A standalone's SD folder is
// served to a fetching OtaManager (what the repeater runs) and the fetched bytes
// must equal the file. The `.mota` vector is Mike's (test/ota_shim/mota_vectors.h).
//   g++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -I test/ota_shim -I src
//       test/test_lora_update.cpp src/helpers/ota/OtaManager.cpp src/helpers/ota/OtaProtocol.cpp
//       src/helpers/ota/MotaContainer.cpp src/helpers/ota/MerkleTree.cpp -o /tmp/lora_update
//   (one line; .github/workflows/build.yml runs it)
#include <stdio.h>
#include <string.h>

#include <vector>

#include "helpers/LoraUpdateCore.h"
#include "helpers/ota/OtaManager.h"
#include "helpers/ota/OtaStore.h"
#include "mota_vectors.h"

using namespace mesh::ota;

static int g_fail = 0;
#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      g_fail++;                                                     \
    }                                                               \
  } while (0)

// ---- an SD folder in memory ---------------------------------------------------

struct MemFile { const char* name; std::vector<uint8_t> bytes; };

class MemIO : public loraupdate::FileIO {
public:
  std::vector<MemFile> files;
  uint32_t reads = 0;
  uint8_t count() override { return (uint8_t)files.size(); }
  const char* name(uint8_t i) override { return files[i].name; }
  uint32_t size(uint8_t i) override { return (uint32_t)files[i].bytes.size(); }
  bool read(uint8_t i, uint32_t off, uint8_t* buf, uint32_t len) override {
    reads++;
    if ((uint64_t)off + len > files[i].bytes.size()) return false;
    memcpy(buf, files[i].bytes.data() + off, len);
    return true;
  }
};

static std::vector<uint8_t> vec() { return std::vector<uint8_t>(MOTA_VEC, MOTA_VEC + MOTA_VEC_LEN); }

// ---- two engines joined by a lossless link (Mike's sim pump) ----------------------

struct Msg { OtaManager* dest; std::vector<uint8_t> bytes; };
static std::vector<Msg> g_q;
static uint32_t g_clk = 1;
struct SendTo { OtaManager* dest; };
static bool simSend(void* ctx, const uint8_t* msg, uint16_t len, bool) {
  g_q.push_back({((SendTo*)ctx)->dest, std::vector<uint8_t>(msg, msg + len)});
  return true;
}
static void pump(OtaManager& client, OtaManager& server) {
  int idle = 0;
  for (int guard = 0; guard < 200000; guard++) {
    server.serviceEgress();
    client.serviceEgress();
    if (!g_q.empty()) {
      Msg m = std::move(g_q.front());
      g_q.erase(g_q.begin());
      m.dest->on_message(m.bytes.data(), (uint16_t)m.bytes.size());
      idle = 0;
    } else {
      g_clk += 5000;
      client.set_clock(g_clk);
      client.loop();
      server.set_clock(g_clk);
      server.loop();
      server.serviceEgress();
      client.serviceEgress();
      if (!g_q.empty()) { idle = 0; continue; }
      if (++idle >= 2) break;
    }
  }
}

// ---- tests ------------------------------------------------------------------------

static void testDescribeHead() {
  loraupdate::FileInfo fi;
  CHECK(loraupdate::describeHead(MOTA_VEC, MOTA_VEC + MOTA_VEC_LEN - 5, MOTA_VEC_LEN, fi));
  CHECK(memcmp(fi.desc.mid, EXP_MERKLE_ROOT, 4) == 0);
  CHECK(fi.desc.target_id == EXP_TARGET_ID);
  CHECK(fi.desc.fw_version == EXP_FW_VERSION);
  CHECK(fi.desc.block_count == EXP_BLOCK_COUNT);
  CHECK(fi.desc.block_size_log2 == EXP_BLOCK_SIZE_LOG2);
  CHECK(fi.desc.payload_size == EXP_PAYLOAD_SIZE);
  CHECK(fi.image_size == EXP_IMAGE_SIZE);
  CHECK(strcmp(fi.hw, "TESTHW") == 0);
  // The streamed description must agree with the engine's whole-buffer parse.
  MotaManifest m;
  CHECK(mota_parse(MOTA_VEC, MOTA_VEC_LEN, m));
  CHECK(fi.desc.leaves_off == (uint32_t)(m.leaves - MOTA_VEC));
  CHECK(fi.desc.payload_off == (uint32_t)(m.payload - MOTA_VEC));
  CHECK(fi.desc.total_size == MOTA_VEC_LEN);

  char mid[9], ver[16];
  loraupdate::midHex(fi.desc.mid, mid);
  CHECK(strcmp(mid, "F215B438") == 0);
  loraupdate::versionText(fi.desc.fw_version, ver, sizeof ver);
  CHECK(strcmp(ver, "v1.16.0") == 0);

  // Wrong total, wrong magic, wrong trailer, truncated: all refused.
  std::vector<uint8_t> b = vec();
  CHECK(!loraupdate::describeHead(b.data(), b.data() + b.size() - 5, (uint32_t)b.size() - 1, fi));
  b[0] = 'x';
  CHECK(!loraupdate::describeHead(b.data(), b.data() + b.size() - 5, (uint32_t)b.size(), fi));
  b = vec();
  b[b.size() - 1] = 'x';
  CHECK(!loraupdate::describeHead(b.data(), b.data() + b.size() - 5, (uint32_t)b.size(), fi));
  CHECK(!loraupdate::describeHead(b.data(), b.data(), 100, fi));
}

static void testScan() {
  MemIO io;
  std::vector<uint8_t> bad = vec();
  bad[4] ^= 1;   // total size no longer matches
  io.files.push_back({"notes.txt", {1, 2, 3}});
  io.files.push_back({"broken.mota", bad});
  io.files.push_back({"Heltec_v3.MOTA", vec()});
  io.files.push_back({"copy.mota", vec()});          // same content: offered once
  loraupdate::MotaFileSource src(io);
  CHECK(src.scan() == 1);
  CHECK(src.skipped() == 1);
  CHECK(strcmp(src.info(0)->name, "Heltec_v3.MOTA") == 0);
  CHECK(src.count() == 1);
  src.only(5);
  CHECK(src.count() == 0);
  src.only(0);
  CHECK(src.count() == 1);
  MotaDesc d;
  CHECK(src.describe(0, d) && memcmp(d.mid, EXP_MERKLE_ROOT, 4) == 0);
  CHECK(!src.describe(1, d));
  uint8_t buf[8];
  CHECK(!src.read(0, MOTA_VEC_LEN - 4, buf, 8));    // past the end
}

// A standalone serving its folder; a repeater (fetching engine) discovers the
// offer, pulls it by id, and ends up with the file byte for byte.
static void testServeToRepeater() {
  g_q.clear();
  MemIO io;
  io.files.push_back({"fw.mota", vec()});
  loraupdate::MotaFileSource src(io);
  CHECK(src.scan() == 1);
  src.only(0);

  OtaManager standalone, repeater;
  OtaStoreRam<8192> store;
  SendTo toRepeater{&repeater}, toStandalone{&standalone};
  standalone.begin(0, simSend, &toRepeater);
  uint8_t id[4] = {0x5A, 0x11, 0x22, 0x33};
  standalone.set_seeder_id(id);
  repeater.begin(EXP_TARGET_ID, simSend, &toStandalone);
  repeater.set_fetch_store(&store);
  CHECK(standalone.add_source(&src));
  CHECK(standalone.servedCount() == 1);

  standalone.announce();    // what the repeater's `ota ls` comes to list
  pump(repeater, standalone);
  repeater.queryAll();
  pump(repeater, standalone);
  CHECK(repeater.catalogCount() == 1);

  CHECK(repeater.pull(EXP_MERKLE_ROOT, EXP_TARGET_ID) == OtaManager::PULL_STARTED);   // `ota pull <mid> flash`
  pump(repeater, standalone);
  CHECK(repeater.fetchState() == OtaManager::COMPLETE);
  CHECK(repeater.blocksHave() == EXP_BLOCK_COUNT);
  CHECK(store.staged_size() == MOTA_VEC_LEN);
  CHECK(memcmp(store.data(), MOTA_VEC, MOTA_VEC_LEN) == 0);
  CHECK(io.reads > 0);

  // Stopping: no sources, nothing offered.
  CHECK(standalone.remove_source(&src));
  CHECK(standalone.servedCount() == 0);
}

static void testParseStatus() {
  using loraupdate::parseStatus;
  // OtaCli.cpp's own formats.
  auto s = parseStatus("OTA | fw v1.17.1 | download: fetching 12/600 (2%) id=f215b438 40s | hops=3");
  CHECK(s.known && s.have == 12 && s.total == 600 && s.percent == 2);
  CHECK(strcmp(s.mid, "F215B438") == 0);
  CHECK(!s.ready && !s.failed);
  s = parseStatus("download: ready to install 600/600 (100%) id=F215B438 900s");
  CHECK(s.ready && s.percent == 100);
  s = parseStatus("download: failed (manifest timeout) 0/0 id=F215B438");
  CHECK(s.failed && s.known && s.total == 0 && s.percent == -1);
  s = parseStatus("no download");
  CHECK(!s.known && !s.ready && s.mid[0] == 0);
  s = parseStatus("x 7/10 grid=12345678");   // "grid=" is not "id="
  CHECK(s.known && s.percent == 70 && s.mid[0] == 0);

  CHECK(loraupdate::replyOk("  OK - tempradio 30 min"));
  CHECK(!loraupdate::replyOk("ERR busy"));
  CHECK(loraupdate::replyError("ERR busy"));
  CHECK(loraupdate::replyError("(err) nope"));
  CHECK(loraupdate::replyError("   "));
  CHECK(!loraupdate::replyError("OTA | fw v1.17.1"));
  CHECK(loraupdate::listsMid("catalog: 1 F215B438 v1.16.0", "f215b438"));
  CHECK(!loraupdate::listsMid("catalog: 0", "F215B438"));
}

int main() {
  testDescribeHead();
  testScan();
  testServeToRepeater();
  testParseStatus();
  if (g_fail) {
    printf("%d failure(s)\n", g_fail);
    return 1;
  }
  printf("lora update: all passed\n");
  return 0;
}
