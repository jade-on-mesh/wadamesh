// SPDX-License-Identifier: GPL-3.0-or-later
// OTP Messenger protocol core. See OtpCore.h; function names follow the Lua
// original (OTP_3_RC1.lua) where a reader may want to compare the two.
#include "OtpCore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

namespace otp {

// =============================================================================
// memory
// =============================================================================
static void* (*s_alloc)(size_t) = malloc;
static void (*s_free)(void*) = free;

void setAllocator(void* (*a)(size_t), void (*f)(void*)) {
  s_alloc = a ? a : malloc;
  s_free = f ? f : free;
}
void* xalloc(size_t n) { return s_alloc(n ? n : 1); }
void xfree(void* p) { if (p) s_free(p); }

static char* xstrndup(const char* s, size_t n) {
  char* d = (char*)xalloc(n + 1);
  if (!d) return nullptr;
  memcpy(d, s, n);
  d[n] = 0;
  return d;
}
static char* xstrdup(const char* s) { return xstrndup(s, strlen(s)); }

bool Bytes::reserve(uint32_t want) {
  if (want <= cap) return true;
  uint32_t nc = cap ? cap : 64;
  while (nc < want) nc *= 2;
  uint8_t* np = (uint8_t*)xalloc(nc);
  if (!np) return false;
  if (p && n) memcpy(np, p, n);
  xfree(p);
  p = np;
  cap = nc;
  return true;
}
bool Bytes::append(const void* data, uint32_t len) {
  if (!reserve(n + len + 1)) return false;
  if (len) memcpy(p + n, data, len);
  n += len;
  p[n] = 0;   // always NUL-terminated, handy for text
  return true;
}
void Bytes::release() {
  if (p) {
    memset(p, 0, cap);   // pad bytes: do not leave them in freed memory
    xfree(p);
  }
  p = nullptr;
  n = cap = 0;
}
void Bytes::swap(Bytes& o) {
  uint8_t* tp = p; uint32_t tn = n, tc = cap;
  p = o.p; n = o.n; cap = o.cap;
  o.p = tp; o.n = tn; o.cap = tc;
}

// =============================================================================
// SHA-256 / HMAC (FIPS 180-4, RFC 2104)
// =============================================================================
namespace {
struct Sha256 {
  uint32_t h[8];
  uint8_t buf[64];
  uint64_t len;
  uint32_t fill;
};
const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
void shaBlock(Sha256& s, const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4], f = s.h[5], g = s.h[6], h = s.h[7];
  for (int i = 0; i < 64; i++) {
    const uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
    const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d;
  s.h[4] += e; s.h[5] += f; s.h[6] += g; s.h[7] += h;
}
void shaInit(Sha256& s) {
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(s.h, iv, sizeof iv);
  s.len = 0;
  s.fill = 0;
}
void shaUpdate(Sha256& s, const uint8_t* p, size_t n) {
  s.len += n;
  while (n) {
    const uint32_t take = (uint32_t)((64 - s.fill) < n ? (64 - s.fill) : n);
    memcpy(s.buf + s.fill, p, take);
    s.fill += take; p += take; n -= take;
    if (s.fill == 64) { shaBlock(s, s.buf); s.fill = 0; }
  }
}
void shaFinal(Sha256& s, uint8_t out[32]) {
  const uint64_t bits = s.len * 8;
  const uint8_t one = 0x80, zero = 0;
  shaUpdate(s, &one, 1);
  while (s.fill != 56) shaUpdate(s, &zero, 1);
  uint8_t lb[8];
  for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
  shaUpdate(s, lb, 8);
  for (int i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(s.h[i] >> 24); out[i * 4 + 1] = (uint8_t)(s.h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(s.h[i] >> 8); out[i * 4 + 3] = (uint8_t)s.h[i];
  }
}
}  // namespace

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  Sha256 s;
  shaInit(s);
  shaUpdate(s, data, len);
  shaFinal(s, out);
}

void padFingerprint(const uint8_t* pad, size_t n, uint8_t fp[8]) {
  Sha256 s;
  shaInit(s);
  shaUpdate(s, (const uint8_t*)kPadCheckLabel, strlen(kPadCheckLabel));
  if (n) shaUpdate(s, pad, n);
  uint8_t h[32];
  shaFinal(s, h);
  memcpy(fp, h, 8);
}

size_t padCheckLink(const uint8_t fp[8], uint32_t n, char* out, size_t cap) {
  char hex[17];
  toHex(fp, 8, hex);
  const int r = snprintf(out, cap, "meshcore://otp/check?fp=%s&n=%lu", hex, (unsigned long)n);
  return r > 0 && (size_t)r < cap ? (size_t)r : 0;
}

bool parsePadCheck(const char* s, uint8_t fp[8], uint32_t* n) {
  static const char* const pre = "meshcore://otp/check?";
  for (size_t i = 0; pre[i]; i++)
    if (!s[i] || (s[i] | 0x20) != (pre[i] | 0x20)) return false;
  bool have_fp = false;
  *n = 0;
  for (const char* p = s + strlen(pre); *p;) {
    const char* end = strchr(p, '&');
    if (!end) end = p + strlen(p);
    if (end - p == 3 + 16 && !strncmp(p, "fp=", 3)) {
      Bytes b;
      if (!fromHex(p + 3, 16, b) || b.n != 8) return false;
      memcpy(fp, b.p, 8);
      have_fp = true;
    } else if (end - p > 2 && !strncmp(p, "n=", 2)) {
      *n = (uint32_t)strtoul(p + 2, nullptr, 10);
    }
    p = *end ? end + 1 : end;
  }
  return have_fp;
}

void padCheckCode(const uint8_t fp[8], char out[20]) {
  char hex[17];
  toHex(fp, 8, hex);
  snprintf(out, 20, "%.4s %.4s %.4s %.4s", hex, hex + 4, hex + 8, hex + 12);
}

void hmacSha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len, uint8_t out[32]) {
  uint8_t k[64] = {0};
  if (key_len > 64) sha256(key, key_len, k);
  else if (key_len) memcpy(k, key, key_len);
  uint8_t pad[64];
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  Sha256 s;
  shaInit(s);
  shaUpdate(s, pad, 64);
  shaUpdate(s, msg, msg_len);
  uint8_t inner[32];
  shaFinal(s, inner);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
  shaInit(s);
  shaUpdate(s, pad, 64);
  shaUpdate(s, inner, 32);
  shaFinal(s, out);
}

// =============================================================================
// helpers
// =============================================================================
static const char kHexDigits[] = "0123456789abcdef";
static const char kB36[] = "0123456789abcdefghijklmnopqrstuvwxyz";

void toHex(const uint8_t* b, size_t n, char* out) {
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = kHexDigits[b[i] >> 4];
    out[i * 2 + 1] = kHexDigits[b[i] & 15];
  }
  out[n * 2] = 0;
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool fromHex(const char* hex, size_t len, Bytes& out) {
  out.clear();
  if (len % 2) return false;
  if (!out.reserve((uint32_t)(len / 2) + 1)) return false;
  for (size_t i = 0; i < len; i += 2) {
    const int hi = hexNibble(hex[i]), lo = hexNibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.p[out.n++] = (uint8_t)((hi << 4) | lo);
  }
  out.p[out.n] = 0;
  return true;
}

static bool isHexRun(const char* s, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (hexNibble(s[i]) < 0) return false;
  return true;
}

void toBase36(uint32_t v, char* out) {
  char tmp[8];
  int n = 0;
  if (v == 0) { out[0] = '0'; out[1] = 0; return; }
  while (v) { tmp[n++] = kB36[v % 36]; v /= 36; }
  for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  out[n] = 0;
}

bool fromBase36(const char* s, size_t len, uint32_t* out) {
  if (!len) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    const char* d = strchr(kB36, c);
    if (!d || !c) return false;
    v = v * 36 + (uint64_t)(d - kB36);
    if (v > 0xFFFFFFFFull) return false;
  }
  *out = (uint32_t)v;
  return true;
}

// llg(): "looks like garbage" -- a wrong pad slice decrypts to noise.
bool looksGarbled(const uint8_t* s, size_t n) {
  if (!n) return false;
  int control = 0, space = 0, distinct = 0, max_run = 0, run = 0;
  int last = -1;
  bool seen[256] = {false};
  for (size_t i = 0; i < n; i++) {
    const uint8_t b = s[i];
    if (b < 0x20 && b != 9 && b != 10 && b != 13) control++;
    if (b == 0x20) space++;
    if (!seen[b]) { seen[b] = true; distinct++; }
    if (b == last) run++; else { run = 1; last = b; }
    if (run > max_run) max_run = run;
  }
  if ((double)control / (double)n > 0.3) return true;
  if (max_run >= 5) return false;
  if (n >= 20 && distinct >= 8 && (double)space / (double)n < 0.045) return true;
  return false;
}

// est_max_wire_chars(): 160 bytes of MeshCore text, less "Name: " and change.
static int estMaxWireChars(int name_len) {
  const int v = 160 - (name_len + 2 + 4 + 1);
  return v < 20 ? 20 : v;
}
int maxSingleShotBytes(int name_len) { return estMaxWireChars(name_len) / 2; }
int maxChunkBytes(int name_len) {
  const int v = maxSingleShotBytes(name_len) - 9 - 3;
  return v < 1 ? 1 : v;
}
int maxChannelBytes(int name_len) {
  const int s = maxSingleShotBytes(name_len);
  return s < 65 ? s : 65;
}

static uint64_t fnv64(const void* a, size_t an, const void* b = nullptr, size_t bn = 0) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < an; i++) h = (h ^ ((const uint8_t*)a)[i]) * 1099511628211ull;
  if (b) {
    h = (h ^ '|') * 1099511628211ull;
    for (size_t i = 0; i < bn; i++) h = (h ^ ((const uint8_t*)b)[i]) * 1099511628211ull;
  }
  return h ? h : 1;
}

static void sanitize(const char* id, char* out, size_t cap) {
  size_t j = 0;
  for (size_t i = 0; id[i] && j + 1 < cap; i++) {
    const char c = id[i];
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    out[j++] = ok ? c : '_';
  }
  out[j] = 0;
}

static const char kChunkMarker[] = "\x01MSGCHNK\x02";   // 9 bytes
static const char kAckMarker[] = "\x01MSGACK\x02";      // 8 bytes
static const size_t kChunkMarkerLen = 9, kAckMarkerLen = 8;

static constexpr uint32_t kChunkRetryMs = 5500;
static constexpr int kMaxChunkRetries = 15;      // the Lua app retries forever
static constexpr int kSsendMaxRetries = 5;
static constexpr uint32_t kResyncGraceMs = 12000;
static constexpr uint32_t kResyncBadgeMs = 6000;
static constexpr uint32_t kSyncSpacingMs = 10000;
static constexpr uint32_t kAutoSyncIntervalMs = 300000;
static constexpr uint32_t kSyncBackoffMs = 30000;
static constexpr int kMaxAutoResyncAbs = 500;
static constexpr int kMaxConsecutiveResyncs = 3;
static constexpr uint32_t kParticipantMaxAgeMs = 10UL * 60UL * 1000UL;
static constexpr uint32_t kWaitingLongPollMs = 10UL * 60UL * 1000UL;
static constexpr uint32_t kLogSaveDelayMs = 1500;

inline bool before(uint32_t now, uint32_t t) { return (int32_t)(now - t) < 0; }

// =============================================================================
// inbound reassembly
// =============================================================================
struct Imt {
  uint8_t tid = 0, tot = 0;
  bool completed = false;
  char sender[kMaxNameLen + 1] = {0};
  uint8_t* data[256] = {nullptr};
  uint16_t len[256] = {0};
  uint64_t wire[256] = {0};
  bool have[256] = {false};
};

static Imt* imtNew() {
  void* m = xalloc(sizeof(Imt));
  return m ? new (m) Imt() : nullptr;
}
static void imtFreeChunks(Imt* m) {
  for (int i = 0; i < 256; i++) { xfree(m->data[i]); m->data[i] = nullptr; }
}
static void imtFree(Imt* m) {
  if (!m) return;
  imtFreeChunks(m);
  m->~Imt();
  xfree(m);
}

// =============================================================================
// Core: targets, pads
// =============================================================================
Core::~Core() {
  for (auto& t : targets_) freeTarget(t);
  xfree(psend_.cur_cipher);
  xfree(ssend_.cipher);
  xfree(queued_.cipher);
  xfree(jitter_.text);
  for (auto& a : acks_) xfree(a.cipher);
  for (int i = 0; i < held_n_; i++) xfree(held_[i].text);
}

void Core::freeTarget(Target& t) {
  t.pad.release();
  imtFree(t.imt);
  t.imt = nullptr;
  for (int i = 0; i < t.wq_n; i++) xfree(t.wq[i].cipher);
  t.wq_n = 0;
  freeLog(t);
}

bool Core::isPublicChannel(const char* name) {
  while (*name == '#') name++;
  const char* p = "public";
  for (; *name && *p; name++, p++) {
    char c = *name;
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    if (c != *p) return false;
  }
  return !*name && !*p;
}

Target* Core::findById(const char* id) {
  for (auto& t : targets_)
    if (t.used && strcmp(t.id, id) == 0) return &t;
  return nullptr;
}

int Core::targetCount() const {
  int n = 0;
  for (auto& t : targets_) if (t.used) n++;
  return n;
}
Target* Core::targetAt(int i) {
  for (auto& t : targets_) {
    if (!t.used) continue;
    if (i-- == 0) return &t;
  }
  return nullptr;
}

Target* Core::alloc(Kind k, const char* id) {
  if (Target* t = findById(id)) return t;
  Target* free_slot = nullptr;
  for (auto& t : targets_) if (!t.used) { free_slot = &t; break; }
  if (!free_slot) {
    // Full: drop the least recently touched target without a pad or traffic in flight.
    for (auto& t : targets_) {
      if (&t == open_ || t.pad.n || t.log_n || t.wq_n) continue;
      if (psend_.t == &t || ssend_.t == &t || queued_.t == &t || jitter_.t == &t) continue;
      freeTarget(t);
      t = Target();
      free_slot = &t;
      break;
    }
  }
  if (!free_slot) return nullptr;
  *free_slot = Target();
  free_slot->used = true;
  free_slot->kind = k;
  snprintf(free_slot->id, sizeof free_slot->id, "%s", id);
  return free_slot;
}

Target* Core::dmTarget(const uint8_t pub[32], const char* name) {
  char hex[9];
  toHex(pub, 4, hex);
  char id[16];
  snprintf(id, sizeof id, "dm:%s", hex);
  Target* t = alloc(Kind::Dm, id);
  if (!t) return nullptr;
  memcpy(t->pub, pub, 32);
  t->have_pub = true;
  if (name && *name) snprintf(t->name, sizeof t->name, "%s", name);
  else if (!t->name[0]) snprintf(t->name, sizeof t->name, "%s", hex);
  return t;
}

Target* Core::channelTarget(const char* name) {
  while (*name == '#') name++;
  if (!*name || isPublicChannel(name)) return nullptr;
  char id[48];
  snprintf(id, sizeof id, "ch:%s", name);
  Target* t = alloc(Kind::Channel, id);
  if (!t) return nullptr;
  snprintf(t->name, sizeof t->name, "%s", name);
  return t;
}

bool Core::reversed(const Target& t, bool mine) const {
  if (t.kind == Kind::Channel) return false;
  if (t.role == 'A') return !mine;   // A reads the peer's bytes from the back
  return mine;                       // B sends from the back
}

int Core::available(Target& t) {
  loadPad(t);
  if (t.legacy || t.read_failed) return 0;
  const int64_t used = t.kind == Kind::Channel ? (int64_t)t.ctr : (int64_t)t.ctr_mine + t.ctr_theirs;
  const int64_t v = (int64_t)t.pad.n - used;
  return v > 0 ? (int)v : 0;
}

char Core::autoRole(Target& t) {
  if (t.kind != Kind::Dm || !t.have_pub) return 0;
  uint8_t me[32];
  if (!io_->selfPub(me)) return 0;
  return memcmp(me, t.pub, 32) <= 0 ? 'A' : 'B';
}

void Core::serializeMeta(Target& t, char* out, size_t cap) {
  if (t.kind == Kind::Channel) {
    snprintf(out, cap, "C2|%u|%d", (unsigned)t.ctr, (int)t.pad.n - (int)t.ctr);
  } else {
    snprintf(out, cap, "D2|%c|%u|%u|%d", t.role, (unsigned)t.ctr_mine, (unsigned)t.ctr_theirs,
             (int)t.pad.n - (int)t.ctr_mine - (int)t.ctr_theirs);
  }
}

static bool parseU(const char*& s, uint32_t* v) {
  if (*s < '0' || *s > '9') return false;
  uint64_t x = 0;
  while (*s >= '0' && *s <= '9') { x = x * 10 + (uint64_t)(*s - '0'); s++; if (x > 0xFFFFFFFFu) return false; }
  *v = (uint32_t)x;
  return true;
}

void Core::loadPad(Target& t) {
  if (t.loaded || locked()) return;
  t.loaded = true;
  char sid[64], name[80];
  sanitize(t.id, sid, sizeof sid);
  snprintf(name, sizeof name, "m_%s", sid);
  Bytes meta;
  bool have_meta = io_->readFile(name, meta) && meta.n;
  Bytes metaplain;
  if (have_meta && !decode(meta.p, meta.n, metaplain)) have_meta = false;
  t.ctr = t.ctr_mine = t.ctr_theirs = 0;
  if (have_meta) {
    const char* s = (const char*)metaplain.p;
    uint32_t a = 0, b = 0, c = 0;
    if (t.kind == Kind::Channel) {
      if (!strncmp(s, "C2|", 3)) { s += 3; if (parseU(s, &a)) t.ctr = a; }
      else if (!strncmp(s, "C|", 2)) { s += 2; if (parseU(s, &a)) t.ctr = a; t.legacy = true; }
    } else {
      if (!strncmp(s, "D2|", 3) && (s[3] == 'A' || s[3] == 'B') && s[4] == '|') {
        t.role = s[3];
        s += 5;
        if (parseU(s, &a) && *s++ == '|' && parseU(s, &b)) { t.ctr_mine = a; t.ctr_theirs = b; }
      } else if (!strncmp(s, "D|", 2) && (s[2] == 'A' || s[2] == 'B')) {
        t.role = s[2];
        s += 4;
        if (parseU(s, &a) && *s++ == '|' && parseU(s, &b)) { t.ctr_mine = a; t.ctr_theirs = b; }
        t.legacy = true;
      }
      (void)c;
    }
  } else if (t.kind == Kind::Dm) {
    // current_pad(): a DM pad with no metadata takes its role from the keys.
    const char r = autoRole(t);
    if (r) t.role = r;
  }
  snprintf(name, sizeof name, "p_%s", sid);
  Bytes raw;
  t.pad.release();
  bool read_ok = true;
  if (io_->readFile(name, raw)) {
    if (raw.n && !decode(raw.p, raw.n, t.pad)) read_ok = false;
  }
  if (t.legacy && t.pad.n) {
    io_->toast("This pad is from an older build. Wipe and re-import it before sending.");
  }
  const uint32_t consumed = t.kind == Kind::Channel ? t.ctr : t.ctr_mine + t.ctr_theirs;
  if (!t.legacy && (!read_ok || (t.pad.n == 0 && consumed > 0))) {
    t.read_failed = true;
    char msg[96];
    snprintf(msg, sizeof msg, "Could not read the pad for %s from storage.", t.name);
    io_->toast(msg);
  }
}

bool Core::persistPad(Target& t) {
  char sid[64], name[80], meta[64];
  sanitize(t.id, sid, sizeof sid);
  if (t.hex_dirty) {
    snprintf(name, sizeof name, "p_%s", sid);
    if (t.pad.n == 0) {
      io_->removeFile(name);
    } else {
      Bytes enc;
      if (!encode(t.pad.p, t.pad.n, enc)) return false;
      if (!io_->writeFile(name, enc.p, enc.n)) return false;
    }
    t.hex_dirty = false;
  }
  serializeMeta(t, meta, sizeof meta);
  snprintf(name, sizeof name, "m_%s", sid);
  Bytes enc;
  if (!encode((const uint8_t*)meta, (uint32_t)strlen(meta), enc)) return false;
  return io_->writeFile(name, enc.p, enc.n);
}

void Core::savePad(Target& t) {
  if (persistPad(t)) { t.save_pending = false; t.save_retries = 0; return; }
  t.save_pending = true;
  t.save_last_ms = io_->nowMs();
  io_->toast("Could not save pad state, retrying");
}

void Core::saveTick() {
  const uint32_t now = io_->nowMs();
  for (auto& t : targets_) {
    if (!t.used) continue;
    if (t.save_pending && now - t.save_last_ms >= 1800) {
      t.save_last_ms = now;
      if (persistPad(t)) { t.save_pending = false; t.save_retries = 0; }
      else if (++t.save_retries >= 6) {
        t.save_pending = false;
        io_->toast("Could not save pad state. Check storage, then resend.");
      }
    }
    if (t.log_dirty && now - t.touched_ms >= kLogSaveDelayMs) saveLog(t);
  }
}

bool Core::keySlice(Target& t, uint32_t n, bool mine, uint8_t* out) {
  if (t.legacy || t.read_failed) return false;
  int64_t start;
  if (t.kind == Kind::Channel) {
    start = t.ctr;
  } else {
    const uint32_t consumed = mine ? t.ctr_mine : t.ctr_theirs;
    start = reversed(t, mine) ? (int64_t)t.pad.n - consumed - n : (int64_t)consumed;
  }
  if (start < 0 || start + (int64_t)n > (int64_t)t.pad.n) return false;
  memcpy(out, t.pad.p + start, n);
  return true;
}

bool Core::consume(Target& t, uint32_t n, bool mine, char* err, size_t cap) {
  if (t.legacy) { if (err) snprintf(err, cap, "pad is from an older build - wipe and reimport"); return false; }
  if (t.read_failed) { if (err) snprintf(err, cap, "could not read pad from storage"); return false; }
  if (t.kind == Kind::Channel) t.ctr += n;
  else if (mine) t.ctr_mine += n;
  else t.ctr_theirs += n;
  savePad(t);
  return true;
}

void Core::clearSyncState(Target& t) {
  t.pending_resync = false;
  t.sync_ok = -1;
  t.sync_text[0] = 0;
  t.next_sync_check = 0;
  t.sync_backoff = false;
  t.awaiting_since = 0;
  t.sent_sync_once = false;
  imtFree(t.imt);
  t.imt = nullptr;
  t.resync_streak = 0;
}

bool Core::importHex(Target& t, const char* hex, size_t len, char* err, size_t cap) {
  if (t.kind == Kind::Channel && isPublicChannel(t.name)) {
    snprintf(err, cap, "OTP is not available on the Public channel");
    return false;
  }
  // Whitespace and separators are fine; anything else is not hex.
  Bytes clean;
  clean.reserve((uint32_t)len + 1);
  for (size_t i = 0; i < len; i++) {
    const char c = hex[i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == ':' || c == '-') continue;
    if (hexNibble(c) < 0) { snprintf(err, cap, "That isn't valid hex"); return false; }
    clean.append(&c, 1);
  }
  if (clean.n % 2) { snprintf(err, cap, "Odd number of hex digits: not a whole number of bytes"); return false; }
  if (clean.n / 2 > (uint32_t)kMaxAddBytes) {
    snprintf(err, cap, "%u bytes is too much for one import (max %d)", (unsigned)(clean.n / 2), kMaxAddBytes);
    return false;
  }
  if (t.wq_n) {
    snprintf(err, cap, "%d message(s) still waiting on the current pad. Send or cancel them first.", t.wq_n);
    return false;
  }
  loadPad(t);
  Bytes bytes;
  if (!fromHex((const char*)clean.p, clean.n, bytes)) { snprintf(err, cap, "That isn't valid hex"); return false; }
  t.pad.swap(bytes);
  t.ctr = t.ctr_mine = t.ctr_theirs = 0;
  t.legacy = t.read_failed = false;
  if (t.kind == Kind::Dm) { const char r = autoRole(t); if (r) t.role = r; }
  t.hex_dirty = true;
  savePad(t);
  clearSyncState(t);
  markKeep(t);
  notifyChanged(t);
  return true;
}

bool Core::addBytes(Target& t, const uint8_t* data, size_t len, char* err, size_t cap) {
  if (t.kind == Kind::Channel && isPublicChannel(t.name)) { snprintf(err, cap, "OTP is not available on the Public channel"); return false; }
  if (!len) { snprintf(err, cap, "Nothing to add"); return false; }
  loadPad(t);
  if (available(t) + (int)len > kMaxTotalPadBytes) {
    snprintf(err, cap, "That would take the pad past %d bytes", kMaxTotalPadBytes);
    return false;
  }
  if (!t.pad.append(data, (uint32_t)len)) { snprintf(err, cap, "Out of memory"); return false; }
  t.hex_dirty = true;
  savePad(t);
  markKeep(t);
  notifyChanged(t);
  return true;
}

int Core::generate(Target& t, int n) {
  if (t.kind == Kind::Channel && isPublicChannel(t.name)) return 0;
  loadPad(t);
  const int have = available(t);
  if (have >= kMaxTotalPadBytes) return 0;
  if (have + n > kMaxTotalPadBytes) n = kMaxTotalPadBytes - have;
  if (!t.pad.reserve(t.pad.n + (uint32_t)n + 1)) return 0;
  for (int i = 0; i < n; i++) t.pad.p[t.pad.n++] = io_->randomByte();
  t.pad.p[t.pad.n] = 0;
  t.hex_dirty = true;
  savePad(t);
  markKeep(t);
  notifyChanged(t);
  return n;
}

void Core::wipe(Target& t) {
  loadPad(t);
  t.pad.release();
  t.ctr = t.ctr_mine = t.ctr_theirs = 0;
  t.legacy = t.read_failed = false;
  t.hex_dirty = true;
  savePad(t);
  clearSyncState(t);
  notifyChanged(t);
}

void Core::flipRole(Target& t) {
  if (t.kind != Kind::Dm) return;
  loadPad(t);
  t.role = t.role == 'A' ? 'B' : 'A';
  savePad(t);
  notifyChanged(t);
}

bool Core::padCheck(Target& t, uint8_t fp[8], uint32_t* n) {
  if (locked()) return false;
  loadPad(t);
  if (t.legacy || t.read_failed || !t.pad.n) return false;
  padFingerprint(t.pad.p, t.pad.n, fp);
  *n = t.pad.n;
  return true;
}

bool Core::exportHex(Target& t, Bytes& out) {
  loadPad(t);
  out.clear();
  if (t.legacy || t.read_failed || !t.pad.n) return false;
  // Only a fresh pad can be handed over: once bytes are used the peer would
  // need our counters as well, and a copy of used bytes is a copy of the past.
  if (t.ctr || t.ctr_mine || t.ctr_theirs) return false;
  if (!out.reserve(t.pad.n * 2 + 1)) return false;
  toHex(t.pad.p, t.pad.n, (char*)out.p);
  out.n = t.pad.n * 2;
  return true;
}

// =============================================================================
// chat log
// =============================================================================
void Core::freeLog(Target& t) {
  for (int i = 0; i < t.log_n; i++) xfree(t.log[i].text);
  total_entries_ -= t.log_n;
  if (total_entries_ < 0) total_entries_ = 0;
  xfree(t.log);
  t.log = nullptr;
  t.log_n = t.log_cap = 0;
  t.log_loaded = false;
}

void Core::ensureLog(Target& t) {
  if (!t.log_loaded && !locked()) loadLog(t);
}

Entry* Core::addEntry(Target& t, const char* who, const uint8_t* text, size_t len, State st, bool mine) {
  ensureLog(t);
  if (len > 4000) len = 4000;
  if (t.log_n >= kChatHistoryMax) {
    xfree(t.log[0].text);
    memmove(t.log, t.log + 1, sizeof(Entry) * (t.log_n - 1));
    t.log_n--;
    total_entries_--;
  }
  if (t.log_n >= t.log_cap) {
    const uint16_t nc = t.log_cap ? (uint16_t)(t.log_cap * 2) : 8;
    Entry* nl = (Entry*)xalloc(sizeof(Entry) * nc);
    if (!nl) return nullptr;
    if (t.log_n) memcpy(nl, t.log, sizeof(Entry) * t.log_n);
    xfree(t.log);
    t.log = nl;
    t.log_cap = nc;
  }
  Entry& e = t.log[t.log_n];
  e = Entry();
  e.seq = seq_++;
  e.at_ms = io_->nowMs();
  e.at_epoch = io_->epoch();
  e.mine = mine;
  snprintf(e.who, sizeof e.who, "%s", who ? who : "");
  e.text = xstrndup((const char*)text, len);
  e.len = (uint16_t)len;
  e.state = st;
  t.log_n++;
  total_entries_++;
  t.touched_ms = io_->nowMs();
  t.log_dirty = true;
  markKeep(t);
  pruneTotal(&t);
  return &t.log[t.log_n - 1];
}

void Core::pruneTotal(Target* protect) {
  while (total_entries_ > kMaxTotalChatEntries) {
    Target* oldest = nullptr;
    for (auto& t : targets_) {
      if (!t.used || &t == protect || &t == open_ || !t.log_n) continue;
      if (!oldest || (int32_t)(t.touched_ms - oldest->touched_ms) < 0) oldest = &t;
    }
    if (!oldest) return;
    // Old messages stay on storage; only the in-memory copy is dropped.
    if (oldest->log_dirty) saveLog(*oldest);
    freeLog(*oldest);
  }
}

Entry* Core::entryBySeq(Target& t, uint32_t seq) {
  if (!seq) return nullptr;
  for (int i = t.log_n - 1; i >= 0; i--)
    if (t.log[i].seq == seq) return &t.log[i];
  return nullptr;
}

void Core::setState(Target& t, uint32_t seq, State st) {
  if (Entry* e = entryBySeq(t, seq)) {
    e->state = st;
    t.log_dirty = true;
    t.touched_ms = io_->nowMs();
  }
  notifyChanged(t);
}

void Core::markRead(Target& t) {
  if (t.unread) { t.unread = 0; notifyChanged(t); }
}

// Log file: "OTL1", count u16, then per entry:
//   seq u32, epoch u32, state u8, fail u8, flags u8 (1 mine, 2 retried), rep u8,
//   fp u32, who (u8 len + bytes), fail_who (u8 len + bytes), text (u16 len + bytes)
static void put32(Bytes& b, uint32_t v) { uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; b.append(x, 4); }
static void put16(Bytes& b, uint16_t v) { uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; b.append(x, 2); }
static void put8(Bytes& b, uint8_t v) { b.append(&v, 1); }
struct Reader {
  const uint8_t* p; uint32_t n, i = 0; bool ok = true;
  Reader(const uint8_t* p_, uint32_t n_) : p(p_), n(n_) {}
  uint8_t u8() { if (i + 1 > n) { ok = false; return 0; } return p[i++]; }
  uint16_t u16() { if (i + 2 > n) { ok = false; return 0; } uint16_t v = (uint16_t)(p[i] | (p[i + 1] << 8)); i += 2; return v; }
  uint32_t u32() { if (i + 4 > n) { ok = false; return 0; } uint32_t v = p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24); i += 4; return v; }
  const uint8_t* take(uint32_t k) { if (i + k > n) { ok = false; return nullptr; } const uint8_t* r = p + i; i += k; return r; }
};

void Core::saveLog(Target& t) {
  t.log_dirty = false;
  if (locked()) return;
  char sid[64], name[80];
  sanitize(t.id, sid, sizeof sid);
  snprintf(name, sizeof name, "l_%s", sid);
  if (!t.log_n) { io_->removeFile(name); return; }
  Bytes b;
  b.append("OTL1", 4);
  put16(b, t.log_n);
  for (int i = 0; i < t.log_n; i++) {
    const Entry& e = t.log[i];
    put32(b, e.seq);
    put32(b, e.at_epoch);
    put8(b, (uint8_t)e.state);
    put8(b, (uint8_t)e.fail);
    put8(b, (uint8_t)((e.mine ? 1 : 0) | (e.retried ? 2 : 0)));
    put8(b, e.rep);
    put32(b, e.fp);
    const uint8_t wl = (uint8_t)strlen(e.who);
    put8(b, wl); b.append(e.who, wl);
    const uint8_t fl = (uint8_t)strlen(e.fail_who);
    put8(b, fl); b.append(e.fail_who, fl);
    put16(b, e.len); b.append(e.text ? e.text : "", e.len);
  }
  Bytes enc;
  if (!encode(b.p, b.n, enc) || !io_->writeFile(name, enc.p, enc.n)) t.log_dirty = true;
}

void Core::loadLog(Target& t) {
  t.log_loaded = true;
  char sid[64], name[80];
  sanitize(t.id, sid, sizeof sid);
  snprintf(name, sizeof name, "l_%s", sid);
  Bytes raw, b;
  if (!io_->readFile(name, raw) || !raw.n || !decode(raw.p, raw.n, b)) return;
  if (b.n < 6 || memcmp(b.p, "OTL1", 4) != 0) return;
  Reader r(b.p + 4, b.n - 4);
  const uint16_t count = r.u16();
  // Stored history first, then anything that arrived before the log was read.
  const uint16_t old_n = t.log_n;
  const uint32_t want = (uint32_t)count + old_n;
  Entry* nl = (Entry*)xalloc(sizeof(Entry) * (want ? want : 1));
  if (!nl) return;
  uint16_t n = 0;
  for (int i = 0; i < count && r.ok; i++) {
    Entry e;
    r.u32();   // stored seq: renumbered below
    e.at_epoch = r.u32();
    e.state = (State)r.u8();
    e.fail = (char)r.u8();
    const uint8_t fl = r.u8();
    e.mine = (fl & 1) != 0;
    e.retried = (fl & 2) != 0;
    e.rep = r.u8();
    e.fp = r.u32();
    const uint8_t wl = r.u8();
    const uint8_t* w = r.take(wl);
    if (w) memcpy(e.who, w, wl < kMaxNameLen ? wl : kMaxNameLen);
    const uint8_t fwl = r.u8();
    const uint8_t* fw = r.take(fwl);
    if (fw) memcpy(e.fail_who, fw, fwl < kMaxNameLen ? fwl : kMaxNameLen);
    const uint16_t tl = r.u16();
    const uint8_t* tx = r.take(tl);
    if (!r.ok) break;
    // Anything still in flight when the log was written did not survive the reboot.
    if (e.state == State::Pending || e.state == State::Retrying || e.state == State::Waiting) e.state = State::Lost;
    e.text = xstrndup((const char*)tx, tl);
    e.len = tl;
    e.seq = seq_++;
    e.at_ms = 0;
    nl[n++] = e;
  }
  const uint16_t loaded = n;
  for (int i = 0; i < old_n; i++) nl[n++] = t.log[i];
  xfree(t.log);
  t.log = nl;
  t.log_n = n;
  t.log_cap = (uint16_t)(want ? want : 1);
  total_entries_ += loaded;
  // Keep the newest kChatHistoryMax.
  if (t.log_n > kChatHistoryMax) {
    const int drop = t.log_n - kChatHistoryMax;
    for (int i = 0; i < drop; i++) xfree(t.log[i].text);
    memmove(t.log, t.log + drop, sizeof(Entry) * (size_t)(t.log_n - drop));
    t.log_n = (uint16_t)(t.log_n - drop);
    total_entries_ -= drop;
  }
  t.log_dirty = old_n > 0;
  if (loaded) markKeep(t);
}

// =============================================================================
// at-rest encryption: the Lua app's ENC2 (nonce 8 | tag 16 | xored), binary
// =============================================================================
static void keystream(const uint8_t key[32], const uint8_t* nonce, size_t nonce_len,
                      const uint8_t* in, uint8_t* out, size_t n) {
  uint8_t msg[40];
  uint32_t counter = 0;
  size_t done = 0;
  while (done < n) {
    memcpy(msg, nonce, nonce_len);
    msg[nonce_len] = (uint8_t)(counter >> 24); msg[nonce_len + 1] = (uint8_t)(counter >> 16);
    msg[nonce_len + 2] = (uint8_t)(counter >> 8); msg[nonce_len + 3] = (uint8_t)counter;
    uint8_t block[32];
    hmacSha256(key, 32, msg, nonce_len + 4, block);
    const size_t take = (n - done) < 32 ? (n - done) : 32;
    for (size_t i = 0; i < take; i++) out[done + i] = in[done + i] ^ block[i];
    done += take;
    counter++;
  }
}

bool Core::encode(const uint8_t* in, uint32_t n, Bytes& out) {
  out.clear();
  if (!have_key_ || !n) return out.assign(in, n);
  uint8_t enc_key[32], mac_key[32];
  hmacSha256(key_, 32, (const uint8_t*)"otp3-enc", 8, enc_key);
  hmacSha256(key_, 32, (const uint8_t*)"otp3-mac", 8, mac_key);
  if (!out.reserve(4 + 8 + 16 + n + 1)) return false;
  memcpy(out.p, "ENC2", 4);
  uint8_t* nonce = out.p + 4;
  for (int i = 0; i < 8; i++) nonce[i] = io_->randomByte();
  uint8_t* body = out.p + 4 + 8 + 16;
  keystream(enc_key, nonce, 8, in, body, n);
  // tag = HMAC(mac_key, nonce .. xored)[0:16]
  Bytes mac_in;
  mac_in.append(nonce, 8);
  mac_in.append(body, n);
  uint8_t tag[32];
  hmacSha256(mac_key, 32, mac_in.p, mac_in.n, tag);
  memcpy(out.p + 12, tag, 16);
  out.n = 4 + 8 + 16 + n;
  return true;
}

bool Core::decode(const uint8_t* in, uint32_t n, Bytes& out) {
  out.clear();
  const bool wrapped = n >= 28 && memcmp(in, "ENC2", 4) == 0;
  if (!wrapped) return out.assign(in, n);
  if (!have_key_) return false;
  uint8_t enc_key[32], mac_key[32];
  hmacSha256(key_, 32, (const uint8_t*)"otp3-enc", 8, enc_key);
  hmacSha256(key_, 32, (const uint8_t*)"otp3-mac", 8, mac_key);
  const uint8_t* nonce = in + 4;
  const uint8_t* tag = in + 12;
  const uint8_t* body = in + 28;
  const uint32_t bn = n - 28;
  Bytes mac_in;
  mac_in.append(nonce, 8);
  mac_in.append(body, bn);
  uint8_t expect[32];
  hmacSha256(mac_key, 32, mac_in.p, mac_in.n, expect);
  uint8_t diff = 0;
  for (int i = 0; i < 16; i++) diff |= (uint8_t)(expect[i] ^ tag[i]);
  if (diff) return false;
  if (!out.reserve(bn + 1)) return false;
  keystream(enc_key, nonce, 8, body, out.p, bn);
  out.n = bn;
  out.p[bn] = 0;
  return true;
}

// derive_key(): k = salt; repeat 3001 times k = HMAC(k, passphrase)
bool Core::deriveKey(const char* pass, const char* salt_hex, uint8_t out[32]) {
  Bytes salt;
  if (!fromHex(salt_hex, strlen(salt_hex), salt)) return false;
  uint8_t k[64];
  size_t klen = salt.n < sizeof k ? salt.n : sizeof k;
  memcpy(k, salt.p, klen);
  for (int i = 0; i < 3001; i++) {
    uint8_t nk[32];
    hmacSha256(k, klen, (const uint8_t*)pass, strlen(pass), nk);
    memcpy(k, nk, 32);
    klen = 32;
  }
  memcpy(out, k, 32);
  return true;
}

static void checkTag(const uint8_t key[32], char out[65]) {
  uint8_t d[32];
  hmacSha256(key, 32, (const uint8_t*)"wadamesh-check-v1", 17, d);
  toHex(d, 32, out);
}

// ---- settings + index -------------------------------------------------------
// "settings" (plain): theme, sound, participants, keep, salt, check
// "targets" (ENC2 when locked): one line per target: kind|id|name|pubhex
void Core::saveSettings() {
  char buf[256];
  const int n = snprintf(buf, sizeof buf, "theme=%u\nsound=%d\nparts=%d\nkeep=%d\nsalt=%s\ncheck=%s\n",
                         (unsigned)theme, sound ? 1 : 0, show_participants ? 1 : 0, keep_files ? 1 : 0,
                         prot_salt_, prot_check_);
  io_->writeFile("settings", (const uint8_t*)buf, (uint32_t)n);
  if (locked()) return;
  Bytes idx;
  for (auto& t : targets_) {
    if (!t.used || !t.keep) continue;
    char pub[65] = "";
    if (t.have_pub) toHex(t.pub, 32, pub);
    char line[200];
    const int ln = snprintf(line, sizeof line, "%c|%s|%s|%s\n", t.kind == Kind::Dm ? 'd' : 'c', t.id, t.name, pub);
    idx.append(line, (uint32_t)ln);
  }
  Bytes enc;
  if (encode(idx.p ? idx.p : (const uint8_t*)"", idx.n, enc)) io_->writeFile("targets", enc.p, enc.n);
}

void Core::loadSettings() {
  Bytes b;
  if (io_->readFile("settings", b) && b.n) {
    b.append("", 0);
    char* s = (char*)b.p;
    while (s && *s) {
      char* nl = strchr(s, '\n');
      if (nl) *nl = 0;
      char* eq = strchr(s, '=');
      if (eq) {
        *eq = 0;
        const char* v = eq + 1;
        if (!strcmp(s, "theme")) theme = (uint8_t)(atoi(v) % 6);
        else if (!strcmp(s, "sound")) sound = atoi(v) != 0;
        else if (!strcmp(s, "parts")) show_participants = atoi(v) != 0;
        else if (!strcmp(s, "keep")) keep_files = atoi(v) != 0;
        else if (!strcmp(s, "salt")) snprintf(prot_salt_, sizeof prot_salt_, "%s", v);
        else if (!strcmp(s, "check")) snprintf(prot_check_, sizeof prot_check_, "%s", v);
      }
      s = nl ? nl + 1 : nullptr;
    }
  }
}

static void loadIndexInto(Core& c, const uint8_t* p, uint32_t n) {
  Bytes b;
  b.append(p, n);
  char* s = (char*)b.p;
  while (s && *s) {
    char* nl = strchr(s, '\n');
    if (nl) *nl = 0;
    char* f[4] = {s, nullptr, nullptr, nullptr};
    for (int i = 1; i < 4 && f[i - 1]; i++) {
      char* bar = strchr(f[i - 1], '|');
      if (!bar) break;
      *bar = 0;
      f[i] = bar + 1;
    }
    if (f[3]) {
      if (f[0][0] == 'd' && strlen(f[3]) == 64) {
        Bytes pub;
        if (fromHex(f[3], 64, pub)) { if (Target* t = c.dmTarget(pub.p, f[2])) t->keep = true; }
      } else if (f[0][0] == 'c') {
        if (Target* t = c.channelTarget(f[2])) t->keep = true;
      }
    }
    s = nl ? nl + 1 : nullptr;
  }
}

void Core::loadIndex() {
  Bytes raw, idx;
  if (io_->readFile("targets", raw) && raw.n && decode(raw.p, raw.n, idx)) loadIndexInto(*this, idx.p, idx.n);
}

void Core::begin() {
  loadSettings();
  if (!locked()) loadIndex();
}

void Core::markKeep(Target& t) {
  if (t.keep) return;
  t.keep = true;
  saveSettings();
}

void Core::forget(Target& t) {
  char sid[64], name[80];
  sanitize(t.id, sid, sizeof sid);
  snprintf(name, sizeof name, "p_%s", sid); io_->removeFile(name);
  snprintf(name, sizeof name, "m_%s", sid); io_->removeFile(name);
  snprintf(name, sizeof name, "l_%s", sid); io_->removeFile(name);
  if (psend_.t == &t) psend_.active = false;
  if (ssend_.t == &t) ssend_.active = false;
  if (queued_.t == &t) queued_.active = false;
  if (jitter_.t == &t) jitter_.active = false;
  for (int i = 0; i < nack_n_; i++) if (nacks_[i].t == &t) { nack_n_ = 0; break; }
  if (open_ == &t) open_ = nullptr;
  freeTarget(t);
  t = Target();
  saveSettings();
}

int Core::unreadTotal() {
  int n = 0;
  for (auto& t : targets_) if (t.used) n += t.unread;
  return n;
}

bool Core::unlock(const char* pass) {
  if (!protectionEnabled()) return true;
  uint8_t k[32];
  if (!deriveKey(pass, prot_salt_, k)) return false;
  char tag[65];
  checkTag(k, tag);
  if (strcmp(tag, prot_check_) != 0) return false;
  memcpy(key_, k, 32);
  have_key_ = true;
  loadIndex();
  heldReplay();
  return true;
}

void Core::resaveAll() {
  for (auto& t : targets_) {
    if (!t.used) continue;
    loadPad(t);
    ensureLog(t);
    t.hex_dirty = true;
    persistPad(t);
    saveLog(t);
  }
  saveSettings();
}

bool Core::enableProtection(const char* pass) {
  if (protectionEnabled() || !pass || !*pass) return false;
  // Everything must be in memory (plain) before the key changes how it is stored.
  for (auto& t : targets_) if (t.used) { loadPad(t); ensureLog(t); }
  uint8_t salt[16];
  for (int i = 0; i < 16; i++) salt[i] = io_->randomByte();
  toHex(salt, 16, prot_salt_);
  if (!deriveKey(pass, prot_salt_, key_)) { prot_salt_[0] = 0; return false; }
  checkTag(key_, prot_check_);
  have_key_ = true;
  resaveAll();
  return true;
}

void Core::lockNow() {
  if (!protectionEnabled()) return;
  for (auto& t : targets_) {
    if (!t.used) continue;
    if (t.log_dirty) saveLog(t);
    if (t.save_pending) persistPad(t);
  }
  memset(key_, 0, sizeof key_);
  have_key_ = false;
  for (auto& t : targets_) {
    if (!t.used) continue;
    freeTarget(t);
    t = Target();
  }
  open_ = nullptr;
  psend_.active = ssend_.active = queued_.active = jitter_.active = false;
}

void Core::panicWipe() {
  // Every file this app could have written, whether or not it is loaded.
  for (auto& t : targets_) {
    if (!t.used) continue;
    char sid[64], name[80];
    sanitize(t.id, sid, sizeof sid);
    snprintf(name, sizeof name, "p_%s", sid); io_->removeFile(name);
    snprintf(name, sizeof name, "m_%s", sid); io_->removeFile(name);
    snprintf(name, sizeof name, "l_%s", sid); io_->removeFile(name);
    freeTarget(t);
    t = Target();
  }
  io_->removeAll();
  memset(key_, 0, sizeof key_);
  have_key_ = false;
  prot_salt_[0] = prot_check_[0] = 0;
  open_ = nullptr;
  psend_.active = ssend_.active = queued_.active = jitter_.active = false;
  for (int i = 0; i < held_n_; i++) xfree(held_[i].text);
  held_n_ = 0;
  nack_n_ = 0;
  saveSettings();
}

// =============================================================================
// transmit
// =============================================================================
bool Core::rawTransmit(Target& t, const char* wire, bool with_offset, uint32_t offset, uint32_t* fp,
                       const char* /*dm_override_name*/) {
  if (fp) *fp = 0;
  if (t.kind == Kind::Dm) {
    if (!t.have_pub) return false;
    return io_->sendDm(t.pub, wire);
  }
  if (with_offset) {
    char b36[8];
    toBase36(offset, b36);
    const size_t n = strlen(wire) + strlen(b36) + 4;
    char* buf = (char*)xalloc(n);
    if (!buf) return false;
    snprintf(buf, n, "z2%s|%s", b36, wire);
    const bool ok = io_->sendChannel(t.name, buf, fp);
    xfree(buf);
    return ok;
  }
  return io_->sendChannel(t.name, wire, fp);
}

void Core::sendSyncCheck(Target& t, bool is_reply) {
  loadPad(t);
  char payload[40], a[8], b[8];
  if (t.kind == Kind::Channel) {
    toBase36(t.ctr, a);
    snprintf(payload, sizeof payload, "z3%c%s", is_reply ? 'S' : 's', a);
  } else {
    toBase36(t.ctr_mine, a);
    toBase36(t.ctr_theirs, b);
    snprintf(payload, sizeof payload, "z3%c%c%s:%s", is_reply ? 'P' : 'p', t.role, a, b);
  }
  rawTransmit(t, payload, false, 0, nullptr);
  t.sent_sync_once = true;
}

void Core::requestPrompt(Target& t) {
  t.sync_backoff = false;
  t.awaiting_since = 0;
  t.next_sync_check = 0;
}

void Core::requestSyncCheck(Target& t) {
  if (locked()) return;
  if (!t.save_pending) { t.loaded = false; t.legacy = t.read_failed = false; loadPad(t); }
  requestPrompt(t);
  t.sync_ok = -1;
  snprintf(t.sync_text, sizeof t.sync_text, "checking...");
  sendSyncCheck(t, false);
  notifyChanged(t);
}

void Core::showBadge(Target& t, uint32_t skipped) {
  snprintf(t.badge, sizeof t.badge, "resynced - skipped %u B", (unsigned)skipped);
  t.badge_until = io_->nowMs() + kResyncBadgeMs;
  notifyChanged(t);
}

void Core::handleSync(Target& t, const char* text, const char* sender) {
  const char mode = text[2];
  const char* body = text + 3;
  const bool msg_ch = mode == 's' || mode == 'S';
  const bool msg_dm = mode == 'p' || mode == 'P';
  if (!msg_ch && !msg_dm) return;
  const bool is_reply = mode == 'S' || mode == 'P';
  if (msg_ch != (t.kind == Kind::Channel)) {
    t.sync_ok = 0;
    snprintf(t.sync_text, sizeof t.sync_text, "party mode mismatch");
    notifyChanged(t);
    return;
  }
  loadPad(t);
  t.sync_backoff = false;
  t.awaiting_since = 0;
  if (msg_dm) flushWaiting(t);
  bool ok = false, collision = false;
  int64_t my_recv = 0, their_send = 0;
  uint32_t their_theirs = 0;
  if (msg_ch) {
    uint32_t off;
    if (!fromBase36(body, strlen(body), &off)) return;
    my_recv = t.ctr;
    their_send = off;
    ok = off == t.ctr;
    if (ok) snprintf(t.sync_text, sizeof t.sync_text, "in sync");
    else snprintf(t.sync_text, sizeof t.sync_text, "out of sync (you: %u, %s: %u)", (unsigned)t.ctr,
                  sender && *sender ? sender : "them", (unsigned)off);
  } else {
    const char their_role = body[0];
    if (their_role != 'A' && their_role != 'B') return;
    const char* colon = strchr(body + 1, ':');
    if (!colon) return;
    uint32_t their_mine;
    if (!fromBase36(body + 1, (size_t)(colon - body - 1), &their_mine)) return;
    if (!fromBase36(colon + 1, strlen(colon + 1), &their_theirs)) return;
    my_recv = t.ctr_theirs;
    their_send = their_mine;
    if (their_role == t.role) {
      collision = true;
      snprintf(t.sync_text, sizeof t.sync_text,
               "ROLE COLLISION: both sides resolved role %c - wipe and re-import this pad on one device", t.role);
    } else {
      ok = their_mine == t.ctr_theirs && their_theirs == t.ctr_mine;
      if (ok) snprintf(t.sync_text, sizeof t.sync_text, "in sync");
      else snprintf(t.sync_text, sizeof t.sync_text, "out of sync (you: %u/%u, them: %u/%u)",
                    (unsigned)t.ctr_mine, (unsigned)t.ctr_theirs, (unsigned)their_mine, (unsigned)their_theirs);
    }
  }
  t.sync_ok = ok ? 1 : 0;
  notifyChanged(t);
  if (ok) t.resync_streak = 0;
  if (!collision && msg_dm && their_theirs > t.ctr_mine) {
    const uint32_t d = their_theirs - t.ctr_mine;
    const int avail = available(t);
    const uint32_t lim = (uint32_t)(avail < kMaxAutoResyncAbs ? avail : kMaxAutoResyncAbs);
    if (d <= lim && consume(t, d, true, nullptr, 0)) {
      t.sync_ok = 1;
      snprintf(t.sync_text, sizeof t.sync_text, "auto-resynced send side (skipped %u bytes)", (unsigned)d);
      showBadge(t, d);
      if (is_reply) sendSyncCheck(t, false);
    }
  }
  if (collision) {
    t.pending_resync = false;
  } else {
    const int64_t gap = their_send - my_recv;
    if (gap > 0) {
      const Kind k = msg_ch ? Kind::Channel : Kind::Dm;
      if (!(t.pending_resync && t.pr_peer_ctr == (uint32_t)their_send && t.pr_kind == k)) {
        t.pending_resync = true;
        t.pr_peer_ctr = (uint32_t)their_send;
        t.pr_kind = k;
        t.pr_first_ms = io_->nowMs();
      }
    } else {
      t.pending_resync = false;
    }
  }
  if (!is_reply) {
    const uint32_t now = io_->nowMs();
    if (!(t.sent_sync_once && t.last_sync_sent && now - t.last_sync_sent < kSyncSpacingMs)) {
      sendSyncCheck(t, true);
      t.last_sync_sent = now ? now : 1;
    }
  }
}

void Core::autoSyncTick(Target& t) {
  if (!t.pending_resync) return;
  const uint32_t now = io_->nowMs();
  if (now - t.pr_first_ms < kResyncGraceMs) return;
  loadPad(t);
  const int64_t mine_now = t.pr_kind == Kind::Channel ? t.ctr : t.ctr_theirs;
  const int64_t gap = (int64_t)t.pr_peer_ctr - mine_now;
  if (gap <= 0) { t.pending_resync = false; t.resync_streak = 0; return; }
  if (t.pr_kind == Kind::Dm && t.resync_streak >= kMaxConsecutiveResyncs) {
    t.pending_resync = false;
    t.sync_ok = 0;
    snprintf(t.sync_text, sizeof t.sync_text,
             "repeated resync with no real fix (role %c) - check both sides show complementary A/B roles, or wipe and re-import",
             t.role);
    notifyChanged(t);
    return;
  }
  const int avail = available(t);
  int cap = avail / 2;
  if (cap > kMaxAutoResyncAbs) cap = kMaxAutoResyncAbs;
  if (avail < gap) {
    t.pending_resync = false;
    t.sync_ok = 0;
    snprintf(t.sync_text, sizeof t.sync_text, "out of sync by %d bytes - not enough pad left to auto-fix, wipe and re-import", (int)gap);
    notifyChanged(t);
    return;
  }
  if (gap > cap) {
    t.pending_resync = false;
    t.sync_ok = 0;
    snprintf(t.sync_text, sizeof t.sync_text, "out of sync by %d bytes - too large to auto-fix safely, wipe and re-import", (int)gap);
    notifyChanged(t);
    return;
  }
  char err[80];
  if (!consume(t, (uint32_t)gap, false, err, sizeof err)) {
    t.pending_resync = false;
    t.sync_ok = 0;
    snprintf(t.sync_text, sizeof t.sync_text, "auto-resync failed: %s", err);
    notifyChanged(t);
    return;
  }
  t.pending_resync = false;
  t.resync_streak++;
  t.sync_ok = 1;
  snprintf(t.sync_text, sizeof t.sync_text, "auto-resynced (skipped %d bytes - a message may have been lost)", (int)gap);
  showBadge(t, (uint32_t)gap);
  sendSyncCheck(t, false);
}

void Core::maybeAutoSyncCheck() {
  Target* t = open_;
  if (!t || t->sync_backoff) return;
  if (!hasPad(*t)) return;
  const uint32_t now = io_->nowMs();
  if (t->next_sync_check && before(now, t->next_sync_check)) return;
  if (t->sent_sync_once && t->last_sync_sent && now - t->last_sync_sent < kSyncSpacingMs) return;
  if (psend_.active && psend_.t == t) return;
  if (t->imt && !t->imt->completed) return;
  if (!io_->airtimeClear()) return;
  sendSyncCheck(*t, false);
  t->next_sync_check = now + kAutoSyncIntervalMs;
  t->awaiting_since = now ? now : 1;
  t->last_sync_sent = now ? now : 1;
}

void Core::backoffTick() {
  Target* t = open_;
  if (!t || !t->awaiting_since) return;
  if (io_->nowMs() - t->awaiting_since >= kSyncBackoffMs) {
    t->sync_backoff = true;
    t->awaiting_since = 0;
  }
}

// ---- nacks ----
void Core::queueNack(Target& t, char reason, const char* who) {
  char payload[8];
  snprintf(payload, sizeof payload, "z4%cz", reason);
  for (int i = 0; i < nack_n_; i++)
    if (!strcmp(nacks_[i].payload, payload)) return;
  if (nack_n_ == 2) { nacks_[0] = nacks_[1]; nack_n_ = 1; }
  Nack& q = nacks_[nack_n_++];
  snprintf(q.payload, sizeof q.payload, "%s", payload);
  q.t = &t;
  snprintf(q.who, sizeof q.who, "%s", who ? who : "");
  q.tries = 0;
}

void Core::nackTick() {
  if (!nack_n_) return;
  const uint32_t now = io_->nowMs();
  if (now - last_nack_ms_ < 5500 || !io_->airtimeClear()) return;
  Nack& q = nacks_[0];
  last_nack_ms_ = now;
  const bool ok = rawTransmit(*q.t, q.payload, false, 0, nullptr);
  q.tries++;
  if (ok || q.tries >= 4) {
    if (nack_n_ == 2) nacks_[0] = nacks_[1];
    nack_n_--;
  }
}

void Core::handleNack(Target& t, const char* text, const char* who) {
  const size_t n = strlen(text);
  if (n < 4) return;
  const char reason = text[n - 2];
  ensureLog(t);
  for (int i = t.log_n - 1; i >= 0; i--) {
    Entry& e = t.log[i];
    if (!e.mine) continue;
    if (e.state != State::Sent && e.state != State::Pending && e.state != State::Retrying) continue;
    if (reason == 'C' && !e.retried && !jitter_.active && t.kind == Kind::Channel) {
      e.retried = true;
      e.state = State::Retrying;
      e.fp = 0; e.rep = 0; e.rep_t0 = 0;
      jitter_.active = true;
      jitter_.t = &t;
      xfree(jitter_.text);
      jitter_.text = xstrndup(e.text, e.len);
      jitter_.len = e.len;
      jitter_.entry_seq = e.seq;
      jitter_.fire_at = io_->nowMs() + 2000 + (uint32_t)((io_->randomByte() << 8 | io_->randomByte()) % 4001);
      t.log_dirty = true;
      notifyChanged(t);
      return;
    }
    e.state = State::Failed;
    e.fail = reason;
    snprintf(e.fail_who, sizeof e.fail_who, "%s", who ? who : "");
    t.log_dirty = true;
    if (psend_.active && psend_.t == &t && psend_.entry_seq == e.seq) psend_.active = false;
    if (ssend_.active && ssend_.t == &t && ssend_.entry_seq == e.seq) ssend_.active = false;
    notifyChanged(t);
    return;
  }
}

// ---- chunk acks ----
bool Core::sendAck(Target& t, uint8_t tid, uint8_t idx) {
  char key[64];
  snprintf(key, sizeof key, "%s:%u:%u", t.id, (unsigned)tid, (unsigned)idx);
  for (auto& a : acks_)
    if (a.cipher && !strcmp(a.key, key)) return rawTransmit(t, a.cipher, false, 0, nullptr);
  uint8_t framed[10];
  memcpy(framed, kAckMarker, kAckMarkerLen);
  framed[8] = tid;
  framed[9] = idx;
  if (available(t) < 10) {
    io_->toast("Not enough pad left to acknowledge a message part");
    return false;
  }
  uint8_t k[10];
  if (!keySlice(t, 10, true, k)) return false;
  for (int i = 0; i < 10; i++) framed[i] ^= k[i];
  char hex[21];
  toHex(framed, 10, hex);
  if (!consume(t, 10, true, nullptr, 0)) return false;
  AckCache& slot = acks_[ack_next_];
  ack_next_ = (ack_next_ + 1) % 24;
  xfree(slot.cipher);
  snprintf(slot.key, sizeof slot.key, "%s", key);
  slot.cipher = xstrdup(hex);
  return rawTransmit(t, hex, false, 0, nullptr);
}

void Core::recordParticipant(Target& t, const char* sender, bool has_off, uint32_t off) {
  if (!sender || !*sender) return;
  const uint32_t now = io_->nowMs();
  int slot = -1, oldest = 0;
  for (int i = 0; i < t.parts_n; i++) {
    if (!strcmp(t.parts[i].name, sender)) { slot = i; break; }
    if ((int32_t)(t.parts[i].last_heard_ms - t.parts[oldest].last_heard_ms) < 0) oldest = i;
  }
  if (slot < 0) {
    if (t.parts_n < 8) slot = t.parts_n++;
    else slot = oldest;
    snprintf(t.parts[slot].name, sizeof t.parts[slot].name, "%s", sender);
    t.parts[slot].in_sync = -1;
  }
  t.parts[slot].last_heard_ms = now;
  if (has_off) t.parts[slot].in_sync = off == t.ctr ? 1 : 0;
}

int Core::activeParticipants(Target& t, Participant* out, int max) {
  const uint32_t now = io_->nowMs();
  int n = 0;
  for (int i = 0; i < t.parts_n && n < max; i++)
    if (now - t.parts[i].last_heard_ms <= kParticipantMaxAgeMs) out[n++] = t.parts[i];
  // alphabetical, case-insensitive
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0; j--) {
      char a[kMaxNameLen + 1], b[kMaxNameLen + 1];
      for (int k = 0; k <= kMaxNameLen; k++) {
        a[k] = (char)((out[j - 1].name[k] >= 'A' && out[j - 1].name[k] <= 'Z') ? out[j - 1].name[k] + 32 : out[j - 1].name[k]);
        b[k] = (char)((out[j].name[k] >= 'A' && out[j].name[k] <= 'Z') ? out[j].name[k] + 32 : out[j].name[k]);
      }
      if (strcmp(a, b) > 0) { Participant tmp = out[j]; out[j] = out[j - 1]; out[j - 1] = tmp; }
    }
  return n;
}

// =============================================================================
// receive
// =============================================================================
static bool startsWith(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

// Exactly what the senders put on the air, so ordinary words are never taken
// for control traffic: "z4Cz", "z4Dz", "z4Gz".
static bool isNack(const char* s) {
  return strlen(s) == 4 && s[0] == 'z' && s[1] == '4' && s[3] == 'z' && (s[2] == 'C' || s[2] == 'D' || s[2] == 'G');
}

static bool isB36Run(const char* s, size_t n) {
  if (!n || n > 7) return false;
  for (size_t i = 0; i < n; i++) {
    const char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
  }
  return true;
}

// "z3s<b36>", "z3S<b36>", "z3p<A|B><b36>:<b36>", "z3P..."
static bool isSync(const char* s) {
  if (s[0] != 'z' || s[1] != '3') return false;
  const char m = s[2];
  if (m == 's' || m == 'S') return isB36Run(s + 3, strlen(s + 3));
  if (m != 'p' && m != 'P') return false;
  if (s[3] != 'A' && s[3] != 'B') return false;
  const char* colon = strchr(s + 4, ':');
  return colon && isB36Run(s + 4, (size_t)(colon - s - 4)) && isB36Run(colon + 1, strlen(colon + 1));
}

// The shortest OTP DM is an encrypted ack: 10 bytes, 20 hex digits.
static constexpr size_t kMinDmHex = 20;

static bool isDmCipher(const char* s) {
  const size_t n = strlen(s);
  return n >= kMinDmHex && n % 2 == 0 && isHexRun(s, n);
}

bool Core::isDupWire(uint64_t h) {
  for (int i = 0; i < recent_wire_n_; i++) if (recent_wire_[i] == h) return true;
  return false;
}

bool Core::onDirect(const uint8_t pub[32], const char* sender_name, const char* text) {
  if (!text || !*text) return false;
  if (!isSync(text) && !isNack(text) && !isDmCipher(text)) return false;
  char hex[9];
  toHex(pub, 4, hex);
  char id[16];
  snprintf(id, sizeof id, "dm:%s", hex);
  if (locked()) {
    // Unknown until unlocked whether this contact has a pad: hold OTP-shaped
    // text so nothing is lost, but only DMs that are plainly OTP.
    if (held_n_ < 12) {
      Held& h = held_[held_n_++];
      h.dm = true;
      memcpy(h.pub, pub, 32);
      snprintf(h.a, sizeof h.a, "%s", sender_name ? sender_name : "");
      h.b[0] = 0;
      h.text = xstrdup(text);
      return true;
    }
    return false;
  }
  Target* t = findById(id);
  if (!t) {
    // Only contacts we hold a pad for: anyone else's hex stays an ordinary DM.
    t = dmTarget(pub, sender_name);
    if (!t) return false;
    if (!hasPad(*t)) {
      freeTarget(*t);
      *t = Target();
      return false;
    }
  }
  if (!hasPad(*t)) return false;
  if (sender_name && *sender_name) snprintf(t->name, sizeof t->name, "%s", sender_name);
  const char* who = t->name;
  if (isSync(text)) { handleSync(*t, text, who); return true; }
  if (isNack(text)) { handleNack(*t, text, who); return true; }
  return process(*t, who, text, false, 0);
}

bool Core::onChannel(const char* channel_name, const char* sender, const char* body) {
  if (!body || !*body || !channel_name) return false;
  while (*channel_name == '#') channel_name++;
  if (isPublicChannel(channel_name)) return false;
  // Channel ciphertext always carries its offset: "z2<b36>|<hex>".
  bool framed = false;
  if (startsWith(body, "z2")) {
    const char* bar = strchr(body + 2, '|');
    framed = bar && isB36Run(body + 2, (size_t)(bar - body - 2)) && bar[1] && strlen(bar + 1) % 2 == 0 &&
             isHexRun(bar + 1, strlen(bar + 1));
  }
  if (!framed && !isSync(body) && !isNack(body)) return false;
  if (locked()) {
    if (held_n_ < 12) {
      Held& h = held_[held_n_++];
      h.dm = false;
      snprintf(h.a, sizeof h.a, "%s", channel_name);
      snprintf(h.b, sizeof h.b, "%s", sender ? sender : "");
      h.text = xstrdup(body);
      return true;
    }
    return false;
  }
  char id[48];
  snprintf(id, sizeof id, "ch:%s", channel_name);
  Target* t = findById(id);
  bool created = false;
  if (!t) { t = channelTarget(channel_name); created = true; }
  if (!t) return false;
  if (!hasPad(*t)) {
    if (created && !t->keep) { freeTarget(*t); *t = Target(); }
    return false;
  }
  const char* who = (sender && *sender) ? sender : "Them";
  if (isSync(body)) { handleSync(*t, body, who); return true; }
  if (isNack(body)) { handleNack(*t, body, who); return true; }
  const char* bar = strchr(body + 2, '|');
  uint32_t off = 0;
  if (!fromBase36(body + 2, (size_t)(bar - body - 2), &off)) return true;
  process(*t, who, bar + 1, true, off);
  return true;
}

void Core::heldReplay() {
  const int n = held_n_;
  Held copy[12];
  memcpy(copy, held_, sizeof(Held) * (size_t)n);
  held_n_ = 0;
  for (int i = 0; i < n; i++) {
    if (copy[i].dm) onDirect(copy[i].pub, copy[i].a, copy[i].text);
    else onChannel(copy[i].a, copy[i].b, copy[i].text);
    xfree(copy[i].text);
  }
}

bool Core::process(Target& t, const char* sender, const char* text, bool has_off, uint32_t off) {
  const bool dm = t.kind == Kind::Dm;
  if (!dm) {
    loadPad(t);
    recordParticipant(t, sender, has_off, off);
    if (open_ == &t) notifyChanged(t);
  }
  const size_t n = strlen(text);
  loadPad(t);
  const uint64_t wh = fnv64(text, n);
  // A part we already have, sent again because our ack was lost: ack again.
  if (t.imt) {
    for (int i = 0; i < 256; i++) {
      if (t.imt->wire[i] == wh) { sendAck(t, t.imt->tid, (uint8_t)i); return true; }
    }
  }
  // Seen and processed already (mesh retransmission).
  const uint64_t rkey = fnv64(t.id, strlen(t.id), text, n);
  {
    const uint32_t now = io_->nowMs();
    for (auto& r : rmc_)
      if (r.used && r.key == rkey && now - r.at < 3000) return true;
  }
  if (isDupWire(wh)) return true;
  const uint32_t cbytes = (uint32_t)(n / 2);
  const int avail = available(t);
  if (avail < (int)cbytes) return !dm;   // a DM we cannot cover is not ours to read
  if (avail > kMaxTotalPadBytes) {
    io_->toast("This pad is too large to use safely. Wipe it to keep receiving.");
    return true;
  }
  if (has_off) {
    if (off < t.ctr) {
      addEntry(t, sender, (const uint8_t*)"[channel message lost - collided with another sender]", 53, State::Lost, false);
      notifyChanged(t);
      queueNack(t, 'C', sender);
      requestPrompt(t);
      sendSyncCheck(t, false);
      return true;
    }
    if (off > t.ctr) {
      if (!(t.pending_resync && t.pr_peer_ctr == off && t.pr_kind == Kind::Channel)) {
        t.pending_resync = true;
        t.pr_peer_ctr = off;
        t.pr_kind = Kind::Channel;
        t.pr_first_ms = io_->nowMs();
      }
      return true;
    }
  }
  Bytes cipher;
  if (!fromHex(text, n, cipher)) return !dm;
  Bytes key;
  if (!key.reserve(cbytes + 1) || !keySlice(t, cbytes, false, key.p)) {
    if (dm) { requestPrompt(t); return false; }
    char msg[80];
    snprintf(msg, sizeof msg, "Decrypt error from %s", sender);
    io_->toast(msg);
    queueNack(t, 'D', sender);
    return true;
  }
  for (uint32_t i = 0; i < cbytes; i++) cipher.p[i] ^= key.p[i];
  const uint8_t* plain = cipher.p;
  // A DM is taken only if it decrypts to a frame (a part or an ack). Anything
  // else is a plain DM that happens to be hex, or ciphertext against pad
  // bytes we are out of step with: leave the pad alone, show the text in
  // Chats, and let a sync check repair any drift.
  if (dm) {
    const bool framed = (cbytes >= kAckMarkerLen + 2 && memcmp(plain, kAckMarker, kAckMarkerLen) == 0) ||
                        (cbytes >= kChunkMarkerLen + 4 && memcmp(plain, kChunkMarker, kChunkMarkerLen) == 0);
    if (!framed) { requestPrompt(t); return false; }
  }
  {
    const uint32_t now = io_->nowMs();
    rmc_[rmc_next_] = {rkey, now, true};
    rmc_next_ = (rmc_next_ + 1) % 8;
  }
  if (recent_wire_n_ == 6) { memmove(recent_wire_, recent_wire_ + 1, sizeof(uint64_t) * 5); recent_wire_n_ = 5; }
  recent_wire_[recent_wire_n_++] = wh;
  char err[80];
  if (!consume(t, cbytes, false, err, sizeof err)) {
    io_->toast(err);
    return true;
  }
  if (cbytes >= kAckMarkerLen && memcmp(plain, kAckMarker, kAckMarkerLen) == 0) {
    if (cbytes < kAckMarkerLen + 2) return true;
    const uint8_t tid = plain[8], idx = plain[9];
    if (psend_.active && psend_.t == &t && psend_.tid == tid && psend_.sctr == idx) {
      psend_.sctr++;
      if (psend_.sctr >= psend_.tot) completeSend();
      else { sendNextChunk(); notifyChanged(t); }
    }
    return true;
  }
  if (cbytes >= kChunkMarkerLen && memcmp(plain, kChunkMarker, kChunkMarkerLen) == 0) {
    if (cbytes < kChunkMarkerLen + 3) return true;
    const uint8_t tid = plain[9], idx = plain[10], tot = plain[11];
    const uint8_t* data = plain + 12;
    const uint32_t dlen = cbytes - 12;
    if (!dlen || !tot) return true;
    if (!t.imt || t.imt->tid != tid) {
      imtFree(t.imt);
      t.imt = imtNew();
      if (!t.imt) return true;
      t.imt->tid = tid;
      t.imt->tot = tot;
      snprintf(t.imt->sender, sizeof t.imt->sender, "%s", sender);
    }
    Imt* m = t.imt;
    if (m->completed || m->have[idx]) { sendAck(t, tid, idx); return true; }
    m->data[idx] = (uint8_t*)xalloc(dlen);
    if (!m->data[idx]) return true;
    memcpy(m->data[idx], data, dlen);
    m->len[idx] = (uint16_t)dlen;
    m->have[idx] = true;
    m->wire[idx] = wh;
    int have = 0;
    for (int i = 0; i < m->tot; i++) if (m->have[i]) have++;
    if (!sendAck(t, tid, idx)) io_->toast("Could not acknowledge a message part");
    if (have >= m->tot) {
      if (finishReassembly(t, sender)) queueNack(t, 'G', sender);
    }
    notifyChanged(t);
    return true;
  }
  if (looksGarbled(plain, cbytes)) {
    addEntry(t, sender, (const uint8_t*)"[decrypted garbage - check the pad is shared correctly]", 55, State::Lost, false);
    char msg[96];
    snprintf(msg, sizeof msg, "Garbage from %s: the pads may be out of step", sender);
    io_->toast(msg);
    requestPrompt(t);
    queueNack(t, 'G', sender);
    notifyChanged(t);
    return true;
  }
  Entry* e = addEntry(t, sender, plain, cbytes, State::Received, false);
  if (e) {
    if (open_ != &t) t.unread++;
    io_->incoming(t, *e);
  }
  requestPrompt(t);
  notifyChanged(t);
  return true;
}

// Returns true when the joined message was garbage (the caller nacks it).
bool Core::finishReassembly(Target& t, const char* sender) {
  Imt* m = t.imt;
  Bytes joined;
  for (int i = 0; i < m->tot; i++) joined.append(m->data[i], m->len[i]);
  m->completed = true;
  imtFreeChunks(m);
  if (looksGarbled(joined.p, joined.n)) {
    addEntry(t, sender, (const uint8_t*)"[reassembled garbage - check the pad is shared correctly]", 57, State::Lost, false);
    requestPrompt(t);
    return true;
  }
  Entry* e = addEntry(t, sender, joined.p, joined.n, State::Received, false);
  if (e) {
    if (open_ != &t) t.unread++;
    io_->incoming(t, *e);
  }
  requestPrompt(t);
  return false;
}

// =============================================================================
// send
// =============================================================================
bool Core::busy() const { return psend_.active || ssend_.active || queued_.active || jitter_.active; }

SendResult Core::send(Target& t, const char* text, size_t len, char* err, size_t cap) {
  if (locked()) { snprintf(err, cap, "Locked"); return SendResult::Error; }
  if (!len) { snprintf(err, cap, "Nothing to send"); return SendResult::Error; }
  if (t.kind == Kind::Channel && isPublicChannel(t.name)) {
    snprintf(err, cap, "OTP is not available on the Public channel");
    return SendResult::Public;
  }
  if (psend_.active || ssend_.active || jitter_.active) {
    snprintf(err, cap, "Still sending the previous message");
    return SendResult::Busy;
  }
  if (queued_.active) { snprintf(err, cap, "Still holding a message for clear airtime"); return SendResult::Busy; }
  if (t.kind == Kind::Dm && !t.have_pub) { snprintf(err, cap, "Unknown contact"); return SendResult::NoTarget; }
  if (t.kind == Kind::Dm && t.wq_n >= kWaitingQueueCap) {
    snprintf(err, cap, "%d messages to %s are already waiting for a path", t.wq_n, t.name);
    return SendResult::QueueFull;
  }
  loadPad(t);
  const int name_len = (int)strlen(io_->selfName());
  const int avail = available(t);
  if (avail > kMaxTotalPadBytes) {
    snprintf(err, cap, "Pad is too large to send from safely (%d bytes). Wipe it to continue.", avail);
    return SendResult::Error;
  }
  if (t.kind == Kind::Channel) {
    const int mwc = maxChannelBytes(name_len);
    if ((int)len > mwc) {
      snprintf(err, cap, "Too long for a channel message (max %d bytes). Use a DM for longer messages.", mwc);
      return SendResult::TooLong;
    }
    if (avail < (int)len) {
      snprintf(err, cap, "Not enough pad: need %d, have %d", (int)len, avail);
      return SendResult::NoPad;
    }
    Entry* e = addEntry(t, "", (const uint8_t*)text, len, State::Pending, true);
    jitter_.active = true;
    jitter_.t = &t;
    xfree(jitter_.text);
    jitter_.text = xstrndup(text, len);
    jitter_.len = (uint16_t)len;
    jitter_.entry_seq = e ? e->seq : 0;
    jitter_.fire_at = io_->nowMs() + 600 + (uint32_t)((io_->randomByte() << 8 | io_->randomByte()) % 1601);
    notifyChanged(t);
    return SendResult::Ok;
  }
  const int ccap = maxChunkBytes(name_len);
  const int chunks = (int)((len + (size_t)ccap - 1) / (size_t)ccap);
  if (chunks > 255) {
    snprintf(err, cap, "Message too long even in parts (max %d characters)", 255 * ccap);
    return SendResult::TooLong;
  }
  // The Lua app found out mid-send; tell the user before anything is burned.
  const int need = (int)len + chunks * 12;
  if (avail < need) {
    snprintf(err, cap, "Not enough pad: need %d, have %d", need, avail);
    return SendResult::NoPad;
  }
  psend_.active = true;
  psend_.t = &t;
  psend_.text.assign(text, (uint32_t)len);
  psend_.ccap = ccap;
  psend_.tid = io_->randomByte();
  psend_.tot = (uint8_t)chunks;
  psend_.sctr = 0;
  xfree(psend_.cur_cipher);
  psend_.cur_cipher = nullptr;
  psend_.cur_idx = -1;
  psend_.retries = 0;
  psend_.sent_once = false;
  Entry* e = addEntry(t, "", (const uint8_t*)text, len, State::Pending, true);
  psend_.entry_seq = e ? e->seq : 0;
  notifyChanged(t);
  sendNextChunk();
  return SendResult::Ok;
}

bool Core::sendNextChunk() {
  if (!psend_.active) return false;
  Target& t = *psend_.t;
  const int idx = psend_.sctr;
  loadPad(t);
  if (!psend_.cur_cipher || psend_.cur_idx != idx) {
    const uint32_t off = (uint32_t)idx * (uint32_t)psend_.ccap;
    uint32_t dlen = psend_.text.n - off;
    if (dlen > (uint32_t)psend_.ccap) dlen = (uint32_t)psend_.ccap;
    const uint32_t need = 12 + dlen;
    Bytes framed;
    framed.append(kChunkMarker, (uint32_t)kChunkMarkerLen);
    const uint8_t hdr[3] = {psend_.tid, (uint8_t)idx, psend_.tot};
    framed.append(hdr, 3);
    framed.append(psend_.text.p + off, dlen);
    if (available(t) < (int)need) {
      io_->toast("Not enough pad left mid-send");
      psend_.active = false;
      setState(t, psend_.entry_seq, State::Lost);
      return false;
    }
    Bytes key;
    if (!key.reserve(need + 1) || !keySlice(t, need, true, key.p)) {
      io_->toast("Encrypt error mid-send");
      psend_.active = false;
      setState(t, psend_.entry_seq, State::Lost);
      return false;
    }
    for (uint32_t i = 0; i < need; i++) framed.p[i] ^= key.p[i];
    char err[80];
    if (!consume(t, need, true, err, sizeof err)) {
      io_->toast(err);
      psend_.active = false;
      setState(t, psend_.entry_seq, State::Lost);
      return false;
    }
    xfree(psend_.cur_cipher);
    psend_.cur_cipher = (char*)xalloc(need * 2 + 1);
    if (!psend_.cur_cipher) { psend_.active = false; return false; }
    toHex(framed.p, need, psend_.cur_cipher);
    psend_.cur_idx = idx;
    psend_.retries = 0;
  }
  psend_.sent_once = true;
  psend_.last_sent = io_->nowMs();
  if (!io_->airtimeClear()) return false;
  if (!rawTransmit(t, psend_.cur_cipher, false, 0, nullptr)) io_->toast("Send failed, will retry");
  return true;
}

void Core::psendTick() {
  if (!psend_.active || !psend_.sent_once) return;
  if (io_->nowMs() - psend_.last_sent < kChunkRetryMs) return;
  if (!io_->airtimeClear()) return;
  if (++psend_.retries > kMaxChunkRetries) {
    Target& t = *psend_.t;
    psend_.active = false;
    if (Entry* e = entryBySeq(t, psend_.entry_seq)) { e->state = State::Failed; e->fail = 0; t.log_dirty = true; }
    requestPrompt(t);
    notifyChanged(t);
    return;
  }
  sendNextChunk();
  notifyChanged(*psend_.t);
}

void Core::completeSend() {
  Target& t = *psend_.t;
  psend_.active = false;
  setState(t, psend_.entry_seq, State::Delivered);
  requestPrompt(t);
}

void Core::jitterTick() {
  if (!jitter_.active || before(io_->nowMs(), jitter_.fire_at)) return;
  jitter_.active = false;
  Target& t = *jitter_.t;
  loadPad(t);
  const uint32_t need = jitter_.len;
  if (available(t) < (int)need) { setState(t, jitter_.entry_seq, State::Lost); return; }
  Bytes buf, key;
  buf.append(jitter_.text, need);
  if (!key.reserve(need + 1) || !keySlice(t, need, true, key.p)) { setState(t, jitter_.entry_seq, State::Lost); return; }
  for (uint32_t i = 0; i < need; i++) buf.p[i] ^= key.p[i];
  const uint32_t claim = t.ctr;
  if (!consume(t, need, true, nullptr, 0)) { setState(t, jitter_.entry_seq, State::Lost); return; }
  char* hex = (char*)xalloc(need * 2 + 1);
  if (!hex) return;
  toHex(buf.p, need, hex);
  if (io_->airtimeClear()) {
    uint32_t fp = 0;
    if (rawTransmit(t, hex, true, claim, &fp)) {
      if (Entry* e = entryBySeq(t, jitter_.entry_seq)) { e->state = State::Sent; e->fp = fp; t.log_dirty = true; }
      requestPrompt(t);
      xfree(hex);
      notifyChanged(t);
    } else {
      setState(t, jitter_.entry_seq, State::Retrying);
      xfree(ssend_.cipher);
      ssend_ = Ssend();
      ssend_.active = true; ssend_.t = &t; ssend_.cipher = hex; ssend_.has_offset = true; ssend_.offset = claim;
      ssend_.last_sent = io_->nowMs(); ssend_.entry_seq = jitter_.entry_seq;
      io_->toast("Send failed, will retry");
    }
  } else {
    xfree(queued_.cipher);
    queued_ = Queued();
    queued_.active = true; queued_.t = &t; queued_.cipher = hex; queued_.has_offset = true; queued_.offset = claim;
    queued_.entry_seq = jitter_.entry_seq;
    notifyChanged(t);
  }
}

void Core::flushQueued() {
  if (!queued_.active || !io_->airtimeClear()) return;
  Target& t = *queued_.t;
  uint32_t fp = 0;
  const bool ok = rawTransmit(t, queued_.cipher, queued_.has_offset, queued_.offset, &fp);
  queued_.active = false;
  if (ok) {
    if (Entry* e = entryBySeq(t, queued_.entry_seq)) { e->state = State::Sent; e->fp = fp; t.log_dirty = true; }
    xfree(queued_.cipher);
    queued_.cipher = nullptr;
    requestPrompt(t);
    notifyChanged(t);
    return;
  }
  setState(t, queued_.entry_seq, State::Retrying);
  xfree(ssend_.cipher);
  ssend_ = Ssend();
  ssend_.active = true; ssend_.t = &t; ssend_.cipher = queued_.cipher; ssend_.has_offset = queued_.has_offset;
  ssend_.offset = queued_.offset; ssend_.last_sent = io_->nowMs(); ssend_.entry_seq = queued_.entry_seq;
  queued_.cipher = nullptr;
}

void Core::ssendTick() {
  if (!ssend_.active) return;
  if (io_->nowMs() - ssend_.last_sent < kChunkRetryMs) return;
  if (!io_->airtimeClear()) return;
  Target& t = *ssend_.t;
  if (ssend_.retries >= kSsendMaxRetries) {
    char* c = ssend_.cipher;
    ssend_.cipher = nullptr;
    ssend_.active = false;
    if (!waitingEnqueue(t, c, ssend_.entry_seq)) { xfree(c); setState(t, ssend_.entry_seq, State::Lost); }
    notifyChanged(t);
    return;
  }
  ssend_.retries++;
  uint32_t fp = 0;
  const bool ok = rawTransmit(t, ssend_.cipher, ssend_.has_offset, ssend_.offset, &fp);
  ssend_.last_sent = io_->nowMs();
  if (ok) {
    if (Entry* e = entryBySeq(t, ssend_.entry_seq)) { e->state = State::Sent; e->fp = fp; t.log_dirty = true; }
    xfree(ssend_.cipher);
    ssend_.cipher = nullptr;
    ssend_.active = false;
    requestPrompt(t);
  }
  notifyChanged(t);
}

bool Core::waitingEnqueue(Target& t, char* cipher, uint32_t seq) {
  if (t.wq_n >= kWaitingQueueCap) {
    char msg[96];
    snprintf(msg, sizeof msg, "No path to %s and %d messages are already waiting", t.name, t.wq_n);
    io_->toast(msg);
    return false;
  }
  WaitItem& w = t.wq[t.wq_n++];
  w.cipher = cipher;
  w.entry_seq = seq;
  w.queued_ms = io_->nowMs();
  w.probed = false;
  w.last_probe_ms = 0;
  setState(t, seq, State::Waiting);
  return true;
}

void Core::flushWaiting(Target& t) {
  if (!t.wq_n) return;
  WaitItem& w = t.wq[0];
  const uint32_t now = io_->nowMs();
  if (w.probed && now - w.last_probe_ms < kSyncSpacingMs) return;
  if (!io_->airtimeClear()) return;
  uint32_t fp = 0;
  const bool ok = rawTransmit(t, w.cipher, false, 0, &fp);
  w.last_probe_ms = now;
  w.probed = true;
  if (!ok) return;
  const uint32_t seq = w.entry_seq;
  xfree(w.cipher);
  memmove(t.wq, t.wq + 1, sizeof(WaitItem) * (size_t)(t.wq_n - 1));
  t.wq_n--;
  if (Entry* e = entryBySeq(t, seq)) { e->state = State::Sent; e->fp = fp; t.log_dirty = true; }
  requestPrompt(t);
  notifyChanged(t);
}

bool Core::cancelWaiting(Target& t) {
  if (!t.wq_n) return false;
  for (int i = 0; i < t.wq_n; i++) {
    setState(t, t.wq[i].entry_seq, State::Lost);
    xfree(t.wq[i].cipher);
  }
  t.wq_n = 0;
  return true;
}

void Core::waitingTick() {
  const uint32_t now = io_->nowMs();
  for (auto& t : targets_) {
    if (!t.used || !t.wq_n) continue;
    if (!t.wq[0].probed || now - t.wq[0].last_probe_ms >= kWaitingLongPollMs) flushWaiting(t);
  }
}

void Core::repeatTick() {
  const uint32_t now = io_->nowMs();
  if (now - last_rep_ms_ < 2000 || !open_) return;
  last_rep_ms_ = now;
  Target& t = *open_;
  int seen = 0;
  bool changed = false;
  for (int i = t.log_n - 1; i >= 0 && seen < 3; i--) {
    Entry& e = t.log[i];
    if (!e.mine || !e.fp) continue;
    seen++;
    if (!e.rep_t0) e.rep_t0 = now ? now : 1;
    if (now - e.rep_t0 < 90000) {
      const uint8_t r = io_->repeatsFor(e.fp);
      if (r > e.rep) { e.rep = r; changed = true; t.log_dirty = true; }
    }
  }
  if (changed) notifyChanged(t);
}

void Core::setOpenTarget(Target* t) {
  open_ = t;
  if (t) {
    loadPad(*t);
    ensureLog(*t);
    t->unread = 0;
  }
}

void Core::tick() {
  if (locked()) return;
  const uint32_t now = io_->nowMs();
  psendTick();
  ssendTick();
  jitterTick();
  flushQueued();
  for (auto& t : targets_) if (t.used && t.pending_resync) autoSyncTick(t);
  nackTick();
  repeatTick();
  backoffTick();
  maybeAutoSyncCheck();
  waitingTick();
  saveTick();
  for (auto& t : targets_) {
    if (t.used && t.badge_until && !before(now, t.badge_until)) {
      t.badge_until = 0;
      t.badge[0] = 0;
      notifyChanged(t);
    }
  }
  if (now - last_prune_ms_ > 300000) {
    last_prune_ms_ = now;
    for (auto& t : targets_) {
      if (!t.used) continue;
      int k = 0;
      for (int i = 0; i < t.parts_n; i++)
        if (now - t.parts[i].last_heard_ms <= kParticipantMaxAgeMs) t.parts[k++] = t.parts[i];
      t.parts_n = (uint8_t)k;
    }
  }
}

// =============================================================================
// moving over from the Lua app
// =============================================================================
namespace {
// Value of `key` in the Lua store text ("key=value" lines), or false.
bool savGet(const char* sav, size_t n, const char* key, Bytes& out) {
  const size_t kl = strlen(key);
  size_t i = 0;
  while (i < n) {
    size_t e = i;
    while (e < n && sav[e] != '\n') e++;
    if (e - i > kl && sav[i + kl] == '=' && !memcmp(sav + i, key, kl)) {
      out.assign(sav + i + kl + 1, (uint32_t)(e - i - kl - 1));
      return true;
    }
    i = e + 1;
  }
  return false;
}
// The Lua app's dec_str for "ENC2:<hex>": raw bytes, or false.
bool luaDec(const uint8_t key[32], const char* v, size_t n, Bytes& out) {
  if (n < 5 || memcmp(v, "ENC2:", 5) != 0) return out.assign(v, (uint32_t)n);
  Bytes raw;
  if (!fromHex(v + 5, n - 5, raw) || raw.n < 24) return false;
  uint8_t enc_key[32], mac_key[32];
  hmacSha256(key, 32, (const uint8_t*)"otp3-enc", 8, enc_key);
  hmacSha256(key, 32, (const uint8_t*)"otp3-mac", 8, mac_key);
  const uint8_t* nonce = raw.p;
  const uint8_t* tag = raw.p + 8;
  const uint8_t* body = raw.p + 24;
  const uint32_t bn = raw.n - 24;
  Bytes mac_in;
  mac_in.append(nonce, 8);
  mac_in.append(body, bn);
  uint8_t expect[32];
  hmacSha256(mac_key, 32, mac_in.p, mac_in.n, expect);
  uint8_t diff = 0;
  for (int i = 0; i < 16; i++) diff |= (uint8_t)(expect[i] ^ tag[i]);
  if (diff) return false;
  if (!out.reserve(bn + 1)) return false;
  keystream(enc_key, nonce, 8, body, out.p, bn);
  out.n = bn;
  out.p[bn] = 0;
  return true;
}
}  // namespace

bool Core::luaNeedsPassphrase(const char* sav, size_t n) {
  Bytes salt;
  return savGet(sav, n, "otp3_prot_salt", salt) && salt.n > 0;
}

int Core::importLua(const char* sav, size_t n, const LuaSource& src, const char* pass, char* err, size_t cap) {
  if (locked()) { snprintf(err, cap, "Unlock first"); return -1; }
  uint8_t lkey[32];
  bool have_lkey = false;
  Bytes salt, check;
  if (savGet(sav, n, "otp3_prot_salt", salt) && salt.n) {
    if (!pass || !*pass) { snprintf(err, cap, "The Lua app is passphrase-protected"); return -1; }
    savGet(sav, n, "otp3_prot_check", check);
    if (!deriveKey(pass, (const char*)salt.p, lkey)) { snprintf(err, cap, "Bad salt"); return -1; }
    char tag[65];
    checkTag(lkey, tag);
    if (check.n != 64 || memcmp(check.p, tag, 64) != 0) { snprintf(err, cap, "Wrong passphrase for the Lua app"); return -1; }
    have_lkey = true;
  }
  int moved = 0;
  size_t i = 0;
  while (i < n) {
    size_t e = i;
    while (e < n && sav[e] != '\n') e++;
    const char* line = sav + i;
    const size_t ln = e - i;
    i = e + 1;
    if (ln < 9 || memcmp(line, "padmeta_", 8) != 0) continue;
    const char* eq = (const char*)memchr(line, '=', ln);
    if (!eq) continue;
    char sid[64];
    const size_t sl = (size_t)(eq - line - 8);
    if (sl >= sizeof sid) continue;
    memcpy(sid, line + 8, sl);
    sid[sl] = 0;
    Bytes meta;
    meta.assign(eq + 1, (uint32_t)(ln - (size_t)(eq - line) - 1));
    // Which conversation?
    Target* t = nullptr;
    if (!strncmp(sid, "dm_", 3) && strlen(sid) == 11) {
      uint8_t pub[32];
      char name[kMaxNameLen + 1];
      if (!src.findContact(src.ctx, sid + 3, pub, name, sizeof name)) continue;
      t = dmTarget(pub, name);
    } else if (!strncmp(sid, "ch_", 3)) {
      char name[kMaxNameLen + 1];
      if (!src.findChannel(src.ctx, sid + 3, name, sizeof name)) continue;
      t = channelTarget(name);
    }
    if (!t) continue;
    loadPad(*t);
    if (t->pad.n) continue;   // never overwrite a pad this app already holds
    // The pad: a file in the app's folder, else (older builds) the store.
    char fname[96];
    snprintf(fname, sizeof fname, "otp3pad_%s.hex", sid);
    Bytes stored, hex;
    bool ok = src.readFile(src.ctx, fname, stored) && stored.n;
    if (!ok) {
      char key[96];
      snprintf(key, sizeof key, "otp3pad_%s", sid);
      ok = savGet(sav, n, key, stored) && stored.n;
    }
    if (!ok) continue;
    Bytes plain;
    if (stored.n >= 5 && !memcmp(stored.p, "ENC2:", 5)) {
      if (!have_lkey || !luaDec(lkey, (const char*)stored.p, stored.n, plain)) continue;
      // enc_pad wraps the RAW bytes (dec_pad turns them back into hex)
      t->pad.swap(plain);
    } else {
      if (!fromHex((const char*)stored.p, stored.n, hex)) continue;
      t->pad.swap(hex);
    }
    // Counters exactly as the Lua app left them.
    Bytes mplain;
    if (meta.n >= 5 && !memcmp(meta.p, "ENC2:", 5)) {
      if (!have_lkey || !luaDec(lkey, (const char*)meta.p, meta.n, mplain)) continue;
    } else {
      mplain.assign(meta.p, meta.n);
    }
    const char* s = (const char*)mplain.p;
    uint32_t a = 0, b = 0;
    t->ctr = t->ctr_mine = t->ctr_theirs = 0;
    if (t->kind == Kind::Channel && !strncmp(s, "C2|", 3)) {
      s += 3;
      if (parseU(s, &a)) t->ctr = a;
    } else if (t->kind == Kind::Dm && !strncmp(s, "D2|", 3) && (s[3] == 'A' || s[3] == 'B')) {
      t->role = s[3];
      s += 5;
      if (parseU(s, &a) && *s++ == '|' && parseU(s, &b)) { t->ctr_mine = a; t->ctr_theirs = b; }
    } else {
      t->pad.release();
      continue;   // a legacy pad the Lua app itself refused to use
    }
    t->legacy = t->read_failed = false;
    t->hex_dirty = true;
    savePad(*t);
    markKeep(*t);
    notifyChanged(*t);
    moved++;
  }
  return moved;
}

// =============================================================================
// view helpers
// =============================================================================
const char* Core::statusWord(Target& t, const Entry& e, char* buf, size_t cap) {
  switch (e.state) {
    case State::Waiting: return "Waiting for a path";
    case State::Pending:
      if (psend_.active && psend_.t == &t && psend_.entry_seq == e.seq && psend_.tot > 1) {
        if (psend_.retries) snprintf(buf, cap, "Sending %d/%d (retry %d)", psend_.sctr + 1, psend_.tot, psend_.retries);
        else snprintf(buf, cap, "Sending %d/%d", psend_.sctr + 1, psend_.tot);
        return buf;
      }
      if (psend_.active && psend_.t == &t && psend_.entry_seq == e.seq && psend_.retries) {
        snprintf(buf, cap, "Sending (retry %d)", psend_.retries);
        return buf;
      }
      return "Sending";
    case State::Retrying:
      if (ssend_.active && ssend_.t == &t && ssend_.entry_seq == e.seq) {
        snprintf(buf, cap, "Retrying (%d/%d)", ssend_.retries, kSsendMaxRetries);
        return buf;
      }
      return "Retrying";
    case State::Failed: {
      const char* w = e.fail == 'C' ? "collided" : e.fail == 'D' ? "decrypt error" : e.fail == 'G' ? "garbled" : nullptr;
      if (!w) return "Not delivered";
      if (t.kind == Kind::Channel && e.fail_who[0]) snprintf(buf, cap, "Failed: %s (%.6s)", w, e.fail_who);
      else snprintf(buf, cap, "Failed: %s", w);
      return buf;
    }
    case State::Lost: return e.mine ? "Not sent" : "";
    case State::Delivered: return "Delivered";
    case State::Sent:
      if (e.rep) { snprintf(buf, cap, "Heard x%u", (unsigned)e.rep); return buf; }
      return "Sent";
    default: return "";
  }
}

int Core::sendProgress(Target& t, int* total) {
  if (!psend_.active || psend_.t != &t) return -1;
  if (total) *total = psend_.tot;
  return psend_.sctr;
}

int Core::recvProgress(Target& t, int* total) {
  if (!t.imt || t.imt->completed) return -1;
  int have = 0;
  for (int i = 0; i < t.imt->tot; i++) if (t.imt->have[i]) have++;
  if (total) *total = t.imt->tot;
  return have;
}

}  // namespace otp
