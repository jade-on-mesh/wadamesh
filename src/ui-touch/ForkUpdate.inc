// SPDX-License-Identifier: GPL-3.0-or-later
// Fork firmware updates on the device: the network half of ForkUpdateCore.h.
// Included by UITask.cpp (so WiFiClientSecure is the P4's C6 socket there, as
// for the Reader), and only where the About page's OTA code is compiled.
//
// HTTPS needs more stack than the shared tile/version-check worker has (8 KB,
// and a TLS handshake overflowed it before), so each job runs on its own 16 KB
// task like the Reader's fetches, and the worker just waits for it.

#include <esp_heap_caps.h>
#include <mbedtls/md.h>
#include <mbedtls/platform.h>

#include "ForkUpdateCore.h"

namespace forkupd_dev {

enum class Kind : uint8_t { Latest, Slot, Sd };

struct Job {
  Kind kind;
  int n;                    // release to fetch (Slot, Sd)
  const char* path;         // SD destination (Sd)
  volatile int* pct;
  char* msg;
  size_t msg_cap;
  int result;               // Latest: N or -1; Slot/Sd: 1 ok, 0 failed
  SemaphoreHandle_t done;
};

// mbedTLS takes its memory from INTERNAL RAM in these builds
// (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC: Arduino's prebuilt esp32s3 SDK, and IDF's
// default on the P4): two 16 KB record buffers plus the handshake, ~40 KB per
// connection. With BLE, Wi-Fi and the UI up, that much internal RAM is often not
// left in one piece, and the handshake fails, which HTTPClient reports as -1
// ("can't connect"). That was "Update failed: signature: can't connect to
// github.com (-1)": the update check got through earlier, while there was
// still room, and the download a while later did not. These boards all have
// PSRAM, so send mbedTLS there (internal RAM only if PSRAM is exhausted). It
// covers every TLS client on the device (the Reader too), and is installed
// once, before the first fork update request.
static void* tlsCalloc(size_t n, size_t size) {
  void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static void tlsFree(void* p) { heap_caps_free(p); }   // whichever heap it came from

static void tlsUsePsram() {
  static bool done = false;
  if (done) return;
  done = true;
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
}

// Internal RAM free and its largest block, in KB, for an error message.
static void ramNote(char* out, size_t cap) {
  snprintf(out, cap, "RAM %uK/%uK", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

static void say(Job& j, const char* m) {
  if (j.msg && j.msg_cap) snprintf(j.msg, j.msg_cap, "%s", m);
}

static void setup(HTTPClient& h, bool follow) {
  h.setReuse(false);
  h.setConnectTimeout(10000);
  h.setTimeout(20000);
  h.setUserAgent("wadamesh-fork");
  h.setFollowRedirects(follow ? HTTPC_STRICT_FOLLOW_REDIRECTS : HTTPC_DISABLE_FOLLOW_REDIRECTS);
}

// Newest release: github.com answers /releases/latest with a redirect to its tag.
static int latest() {
  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient h;
  setup(h, false);
  const char* keys[] = {"Location"};
  h.collectHeaders(keys, 1);
  if (!h.begin(c, "https://github.com/" WADA_FORK_REPO "/releases/latest")) return -1;
  const int code = h.GET();
  const String loc = h.header("Location");
  h.end();
  if (code != 301 && code != 302) return -1;
  return forkupd::releaseCode(loc.c_str());   // -1 too when there are no releases yet
}

static void assetUrl(char* out, size_t cap, const char* tag, const char* file) {
  snprintf(out, cap, "https://github.com/" WADA_FORK_REPO "/releases/download/%s/%s", tag, file);
}

// One HTTPS GET that may be redirected (a release file on github.com is a 302
// to release-assets.githubusercontent.com, with a long signed URL). Each hop
// gets a fresh TLS client of its own (and the old one's memory back first).
// Up to 5 hops, and two retries of a hop that couldn't connect.
struct Get {
  WiFiClientSecure* c = nullptr;
  HTTPClient* h = nullptr;
  int code = 0;
  void close() {
    if (h) { h->end(); delete h; h = nullptr; }
    if (c) { c->stop(); delete c; c = nullptr; }
  }
  ~Get() { close(); }
};

static void hostOf(const String& url, char* out, size_t cap) {
  int a = url.indexOf("://");
  a = a < 0 ? 0 : a + 3;
  int b = url.indexOf('/', a);
  if (b < 0) b = url.length();
  snprintf(out, cap, "%s", url.substring(a, b).c_str());
}

// True with g.code 200 and the body ready to read; else a message in j.
static bool getFollowing(Job& j, const char* first, Get& g, const char* what) {
  String url = first;
  for (int hop = 0; hop < 5; hop++) {
    for (int attempt = 0; attempt < 3; attempt++) {
      g.close();
      g.c = new (std::nothrow) WiFiClientSecure();
      g.h = new (std::nothrow) HTTPClient();
      if (!g.c || !g.h) { say(j, "out of memory"); return false; }
      g.c->setInsecure();
      setup(*g.h, false);
      const char* keys[] = {"Location"};
      g.h->collectHeaders(keys, 1);
      if (!g.h->begin(*g.c, url)) { g.code = -1; break; }
      g.code = g.h->GET();
      if (g.code > 0) break;
      vTaskDelay(pdMS_TO_TICKS(1500));   // could not connect: try again
    }
    if (g.code == 200) return true;
    if (g.code == 301 || g.code == 302 || g.code == 303 || g.code == 307 || g.code == 308) {
      const String next = g.h->header("Location");
      if (next.length() < 8) break;
      url = next;
      continue;
    }
    char host[64], m[112];
    hostOf(url, host, sizeof host);
    if (g.code == 404) snprintf(m, sizeof m, "%s not in the release", what);
    else if (g.code < 0) {
      char ram[32];
      ramNote(ram, sizeof ram);
      snprintf(m, sizeof m, "%s: can't connect to %s (%d, %s)", what, host, g.code, ram);
    }
    else snprintf(m, sizeof m, "%s: HTTP %d from %s", what, g.code, host);
    say(j, m);
    return false;
  }
  char m[64];
  snprintf(m, sizeof m, "%s: too many redirects", what);
  say(j, m);
  return false;
}

static bool fetchSig(Job& j, const char* tag, const char* file, uint8_t sig[forkupd::kSigBytes]) {
  char name[64], url[192];
  snprintf(name, sizeof name, "%s.sig", file);
  assetUrl(url, sizeof url, tag, name);
  Get g;
  if (!getFollowing(j, url, g, "signature")) return false;
  auto* st = g.h->getStreamPtr();
  size_t got = 0;
  const uint32_t t0 = millis();
  while (st && got < forkupd::kSigBytes && millis() - t0 < 15000) {
    const int a = st->available();
    if (a <= 0) {
      if (!st->connected()) break;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    const int r = st->read(sig + got, forkupd::kSigBytes - got);
    if (r > 0) got += (size_t)r;
  }
  g.close();
  if (got != forkupd::kSigBytes) { say(j, "bad signature file"); return false; }
  return true;
}

// Streams the image into the spare OTA slot or a file on the SD card, hashing as
// it goes; it is kept only if the hash carries the release key's signature.
static bool fetchImage(Job& j) {
  char tag[32], file[48], url[192];
  forkupd::tagFor(j.n, tag, sizeof tag);
  snprintf(file, sizeof file, "wadamesh-%s.bin", WADA_BOARD_ID);
  uint8_t sig[forkupd::kSigBytes];
  if (!fetchSig(j, tag, file, sig)) return false;

  assetUrl(url, sizeof url, tag, file);
  Get g;
  if (!getFollowing(j, url, g, "firmware")) return false;
  HTTPClient& h = *g.h;
  const int len = h.getSize();
  if (len <= 0) { say(j, "bad length"); return false; }

#if CAP_SD
  File f;
#endif
  if (j.kind == Kind::Slot) {
#if CAP_OTA
    if (!Update.begin((size_t)len)) {
      char m[64];
      // The T-Deck's app slots grew (partitions_tdeck_touch_big_app.csv): a radio
      // still on the old table can only take the new image over USB.
      const esp_partition_t* spare = esp_ota_get_next_update_partition(NULL);
      if (spare && (size_t)len > spare->size)
        snprintf(m, sizeof m, "too big for this radio's slots: install once over USB");
      else
        snprintf(m, sizeof m, "won't fit: %s", Update.errorString());
      say(j, m);
      h.end();
      return false;
    }
#else
    say(j, "no update slot");
    h.end();
    return false;
#endif
  } else {
#if CAP_SD
    SD.remove(j.path);
    f = SD.open(j.path, FILE_WRITE);
    if (!f) { say(j, "SD open failed"); h.end(); return false; }
#else
    say(j, "no SD card");
    h.end();
    return false;
#endif
  }

  mbedtls_md_context_t md;
  mbedtls_md_init(&md);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  bool ok = info && mbedtls_md_setup(&md, info, 0) == 0 && mbedtls_md_starts(&md) == 0;

  // On this task's (internal) stack: flash and SD both want DMA-capable memory.
  uint8_t buf[1024];
  auto* st = h.getStreamPtr();   // NetworkClient* on arduino 3.x
  size_t done = 0;
  uint32_t last_data = millis();
  while (ok && st && done < (size_t)len) {
    const int a = st->available();
    if (a <= 0) {
      if (!st->connected() || millis() - last_data > 20000) break;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    const size_t want = (size_t)len - done < sizeof buf ? (size_t)len - done : sizeof buf;
    const int r = st->read(buf, want < (size_t)a ? want : (size_t)a);
    if (r <= 0) continue;
    last_data = millis();
    if (mbedtls_md_update(&md, buf, (size_t)r) != 0) { ok = false; break; }
    size_t w = 0;
#if CAP_OTA
    if (j.kind == Kind::Slot) w = Update.write(buf, (size_t)r);
#endif
#if CAP_SD
    if (j.kind == Kind::Sd) w = f.write(buf, (size_t)r);
#endif
    if (w != (size_t)r) { say(j, j.kind == Kind::Sd ? "SD write failed" : "flash write failed"); ok = false; break; }
    done += (size_t)r;
    if (j.pct) *j.pct = (int)((uint64_t)done * 100 / (uint64_t)len);
  }
  h.end();
  uint8_t digest[32];
  if (ok && mbedtls_md_finish(&md, digest) != 0) ok = false;
  mbedtls_md_free(&md);
  if (ok && done != (size_t)len) {
    char m[40];
    snprintf(m, sizeof m, "short read %u/%d", (unsigned)done, len);
    say(j, m);
    ok = false;
  }
  if (ok && !forkupd::verify(tag, file, digest, sig)) {
    say(j, "signature check failed");
    ok = false;
  }

  if (j.kind == Kind::Slot) {
#if CAP_OTA
    if (!ok) { Update.abort(); return false; }
    if (!Update.end(true)) {   // finalize, verify the image, mark the slot bootable
      char m[64];
      snprintf(m, sizeof m, "verify: %s", Update.errorString());
      say(j, m);
      return false;
    }
#endif
  } else {
#if CAP_SD
    f.close();
    if (!ok) { SD.remove(j.path); return false; }
#endif
  }
  if (j.pct) *j.pct = 100;
  return true;
}

static void jobBody(Job& j) {
  if (j.kind == Kind::Latest) j.result = latest();
  else j.result = fetchImage(j) ? 1 : 0;
}

static void taskFn(void* arg) {
  Job& j = *static_cast<Job*>(arg);
  jobBody(j);
  xSemaphoreGive(j.done);
  vTaskDelete(nullptr);
}

// On the shared audio worker (VoxAudioTask): its 16 KB internal stack is held
// from boot, so the update doesn't need a second free 16 KB block. At the
// worker's priority (3) a long download would starve the UI, so drop to 1.
static void workerFn(void* arg) {
  Job& j = *static_cast<Job*>(arg);
  const UBaseType_t prio = uxTaskPriorityGet(nullptr);
  vTaskPrioritySet(nullptr, 1);
  jobBody(j);
  vTaskPrioritySet(nullptr, prio);
  xSemaphoreGive(j.done);
}

// Runs `j` on the audio worker when it is idle, else on its own 16 KB task
// (core 0, beside Wi-Fi), and waits for it.
static int run(Job& j) {
  tlsUsePsram();
  j.done = xSemaphoreCreateBinary();
  if (!j.done) { say(j, "out of memory"); return j.kind == Kind::Latest ? -1 : 0; }
  if (VoxAudioTask::run(workerFn, &j)) {
    xSemaphoreTake(j.done, portMAX_DELAY);
    vSemaphoreDelete(j.done);
    return j.result;
  }
  if (xTaskCreatePinnedToCore(taskFn, "fork_upd", 16384, &j, 1, nullptr, 0) != pdPASS) {
    vSemaphoreDelete(j.done);
    // Its 16 KB stack must be one piece of internal RAM (TLS can't run on a PSRAM stack).
    char m[64], ram[32];
    ramNote(ram, sizeof ram);
    snprintf(m, sizeof m, "out of memory (%s, need 16K)", ram);
    say(j, m);
    return j.kind == Kind::Latest ? -1 : 0;
  }
  xSemaphoreTake(j.done, portMAX_DELAY);
  vSemaphoreDelete(j.done);
  return j.result;
}

}  // namespace forkupd_dev

// ---- what the About page's worker calls in this fork ------------------------------
static int forkLatestRelease() {
  forkupd_dev::Job j{forkupd_dev::Kind::Latest, 0, nullptr, nullptr, nullptr, 0, -1, nullptr};
  return forkupd_dev::run(j);
}

static bool forkInstallToSlot(int n, volatile int* pct, char* msg, size_t cap) {
  forkupd_dev::Job j{forkupd_dev::Kind::Slot, n, nullptr, pct, msg, cap, 0, nullptr};
  return forkupd_dev::run(j) == 1;
}

static bool forkSaveToSd(int n, const char* path, volatile int* pct, char* msg, size_t cap) {
  forkupd_dev::Job j{forkupd_dev::Kind::Sd, n, path, pct, msg, cap, 0, nullptr};
  return forkupd_dev::run(j) == 1;
}
