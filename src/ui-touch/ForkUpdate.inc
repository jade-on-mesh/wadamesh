// SPDX-License-Identifier: GPL-3.0-or-later
// Fork firmware updates on the device: the network half of ForkUpdateCore.h.
// Included by UITask.cpp (so WiFiClientSecure is the P4's C6 socket there, as
// for the Reader), and only where the About page's OTA code is compiled.
//
// HTTPS needs more stack than the shared tile/version-check worker has (8 KB,
// and a TLS handshake overflowed it before), so each job runs on its own 16 KB
// task like the Reader's fetches, and the worker just waits for it.

#include <mbedtls/md.h>

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
  return forkupd::releaseN(loc.c_str());   // -1 too when there are no releases yet
}

static void assetUrl(char* out, size_t cap, const char* tag, const char* file) {
  snprintf(out, cap, "https://github.com/" WADA_FORK_REPO "/releases/download/%s/%s", tag, file);
}

static bool fetchSig(Job& j, const char* tag, const char* file, uint8_t sig[forkupd::kSigBytes]) {
  char name[64], url[192];
  snprintf(name, sizeof name, "%s.sig", file);
  assetUrl(url, sizeof url, tag, name);
  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient h;
  setup(h, true);
  if (!h.begin(c, url)) { say(j, "connect failed"); return false; }
  const int code = h.GET();
  if (code != 200) {
    char m[40];
    snprintf(m, sizeof m, code == 404 ? "no signed image" : "signature HTTP %d", code);
    say(j, m);
    h.end();
    return false;
  }
  auto* st = h.getStreamPtr();   // NetworkClient* on arduino 3.x
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
  h.end();
  if (got != forkupd::kSigBytes) { say(j, "bad signature file"); return false; }
  return true;
}

// Streams the image into the spare OTA slot or a file on the SD card, hashing as
// it goes; it is kept only if the hash carries the release key's signature.
static bool fetchImage(Job& j) {
  char tag[16], file[48], url[192];
  snprintf(tag, sizeof tag, WADA_RELEASE_PREFIX "%d", j.n);
  snprintf(file, sizeof file, "wadamesh-%s.bin", WADA_BOARD_ID);
  uint8_t sig[forkupd::kSigBytes];
  if (!fetchSig(j, tag, file, sig)) return false;

  assetUrl(url, sizeof url, tag, file);
  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient h;
  setup(h, true);
  if (!h.begin(c, url)) { say(j, "connect failed"); return false; }
  const int code = h.GET();
  if (code != 200) {
    char m[24];
    snprintf(m, sizeof m, "HTTP %d", code);
    say(j, m);
    h.end();
    return false;
  }
  const int len = h.getSize();
  if (len <= 0) { say(j, "bad length"); h.end(); return false; }

#if CAP_SD
  File f;
#endif
  if (j.kind == Kind::Slot) {
#if CAP_OTA
    if (!Update.begin((size_t)len)) {
      char m[64];
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

static void taskFn(void* arg) {
  Job& j = *static_cast<Job*>(arg);
  if (j.kind == Kind::Latest) j.result = latest();
  else j.result = fetchImage(j) ? 1 : 0;
  xSemaphoreGive(j.done);
  vTaskDelete(nullptr);
}

// Runs `j` on its own 16 KB task (core 0, beside Wi-Fi) and waits for it.
static int run(Job& j) {
  j.done = xSemaphoreCreateBinary();
  if (!j.done) { say(j, "out of memory"); return j.kind == Kind::Latest ? -1 : 0; }
  if (xTaskCreatePinnedToCore(taskFn, "fork_upd", 16384, &j, 1, nullptr, 0) != pdPASS) {
    vSemaphoreDelete(j.done);
    say(j, "out of memory");
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
