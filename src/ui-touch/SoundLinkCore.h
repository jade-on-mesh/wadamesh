// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Sound link: the parts with no hardware in them, so they are tested on the
// host (test/test_sound_link.cpp). The radio side is SoundLinkUI.inc; the
// user's side is SOUND_LINK.md.
//
// What goes over the air is ggwave (lib/ggwave), one message of up to 140
// bytes at a time, in one of two shapes:
//
//   Wavest's messages (github.com/bennjordan/Wavest), so the radio, MeshCore
//   Open TAK and Wavest's web page all talk to each other:
//     U:<callsign>|<text>              plain
//     E:<callsign>|AES:<base64>        AES-256-GCM: 12-byte IV, ciphertext,
//                                      16-byte tag; key = SHA-256(passkey)
//     E:<callsign>|XOR:<base64>        Wavest's fallback, read only
//
//   share links, the same text as the QR codes:
//     meshcore://channel/add?name=<name>&secret=<32 hex>
//     meshcore://contact/add?name=<name>&public_key=<64 hex>&type=<1..4>
//
// Wavest's key is an unsalted hash of the passkey: enough to keep a chat
// between friends from bystanders, never a replacement for the mesh's own
// encryption.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// SoundLinkUI.inc: MyMesh hands it every channel text message, for the bridge.
void soundLinkOnChannelText(int channel_idx, const char* text);

namespace soundlink {

// ggwave's limit for one message.
constexpr size_t kMaxBytes = 140;
// The bridge's tag on what it posts, so it never plays its own posts back.
constexpr const char* kBridgeTag = "[Wavest] ";

// ---- base64 ------------------------------------------------------------------------------

inline size_t b64Len(size_t n) { return (n + 2) / 3 * 4; }

// Writes the encoding and a NUL; false when `cap` is too small.
inline bool b64Encode(const uint8_t* in, size_t n, char* out, size_t cap) {
  static const char* const k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (cap < b64Len(n) + 1) return false;
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) |
                       (i + 2 < n ? in[i + 2] : 0);
    out[o++] = k[v >> 18 & 63];
    out[o++] = k[v >> 12 & 63];
    out[o++] = i + 1 < n ? k[v >> 6 & 63] : '=';
    out[o++] = i + 2 < n ? k[v & 63] : '=';
  }
  out[o] = 0;
  return true;
}

inline int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

// Decodes `n` characters (padding optional); false on anything that isn't base64.
inline bool b64Decode(const char* in, size_t n, uint8_t* out, size_t cap, size_t* out_len) {
  while (n && in[n - 1] == '=') n--;
  if (n % 4 == 1) return false;
  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < n; i++) {
    const int v = b64Value(in[i]);
    if (v < 0) return false;
    acc = acc << 6 | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= cap) return false;
      out[o++] = (uint8_t)(acc >> bits);
    }
  }
  *out_len = o;
  return true;
}

// ---- text ----------------------------------------------------------------------------------

// Copies `src` without cutting a UTF-8 character: at most `max` bytes, then a NUL.
inline size_t copyUtf8(char* out, size_t cap, const char* src, size_t max) {
  if (!cap) return 0;
  size_t n = strlen(src);
  if (n > max) n = max;
  if (n > cap - 1) n = cap - 1;
  if (n < strlen(src))   // cut: step back off a continuation byte
    while (n && ((uint8_t)src[n] & 0xC0) == 0x80) n--;
  memcpy(out, src, n);
  out[n] = 0;
  return n;
}

// A callsign as Wavest sends it: no '|' or ':', no space at either end, 24 bytes at most.
inline void callsign(const char* in, char* out, size_t cap) {
  if (!cap) return;
  char tmp[64];
  size_t o = 0;
  for (const char* p = in ? in : ""; *p && o + 1 < sizeof tmp; p++)
    if (*p != '|' && *p != ':') tmp[o++] = *p;
  tmp[o] = 0;
  const char* s = tmp;
  while (*s == ' ') s++;
  size_t n = strlen(s);
  while (n && s[n - 1] == ' ') n--;
  char trimmed[64];
  memcpy(trimmed, s, n);
  trimmed[n] = 0;
  copyUtf8(out, cap, trimmed, 24);
}

// "U:<callsign>|<text>", the text cut (at a character) to fit one message.
// Returns its length.
inline size_t buildPlain(const char* cs, const char* text, char* out, size_t cap) {
  char c[32];
  callsign(cs, c, sizeof c);
  const size_t lim = cap - 1 < kMaxBytes ? cap - 1 : kMaxBytes;
  int n = 0;
  const char* const parts[3] = {"U:", c, "|"};
  for (const char* p : parts) {
    const size_t l = strlen(p);
    if ((size_t)n + l > lim) return 0;
    memcpy(out + n, p, l);
    n += (int)l;
  }
  n += (int)copyUtf8(out + n, cap - (size_t)n, text ? text : "", lim - (size_t)n);
  return (size_t)n;
}

// How much plaintext fits in one encrypted message from `cs`.
inline size_t aesMaxText(const char* cs) {
  char c[32];
  callsign(cs, c, sizeof c);
  const size_t head = 2 + strlen(c) + 1 + 4;   // E:<cs>|AES:
  if (head >= kMaxBytes) return 0;
  const size_t sealed = (kMaxBytes - head) / 4 * 3;   // bytes whose base64 fits
  return sealed > 28 ? sealed - 28 : 0;
}

// "E:<callsign>|AES:<base64(sealed)>", sealed = IV, ciphertext, tag. 0 when it won't fit.
inline size_t buildAes(const char* cs, const uint8_t* sealed, size_t sealed_len, char* out, size_t cap) {
  char c[32];
  callsign(cs, c, sizeof c);
  const int n = snprintf(out, cap, "E:%s|AES:", c);
  if (n < 0 || (size_t)n + b64Len(sealed_len) > kMaxBytes) return 0;
  if (!b64Encode(sealed, sealed_len, out + n, cap - (size_t)n)) return 0;
  return (size_t)n + b64Len(sealed_len);
}

// ---- reading what was heard ----------------------------------------------------------------

enum class Kind : uint8_t { Raw, Plain, Aes, Xor };

struct Heard {
  Kind kind = Kind::Raw;
  char sender[32] = "";
  const char* body = nullptr;   // into the text parse() was given: the message,
                                // or the base64 after AES:/XOR:
};

inline Heard parse(const char* s) {
  Heard h;
  h.body = s;
  const bool u = !strncmp(s, "U:", 2), e = !strncmp(s, "E:", 2);
  const char* bar = (u || e) ? strchr(s + 2, '|') : nullptr;
  if (!bar) return h;
  const size_t n = (size_t)(bar - (s + 2));
  char cs[64];
  memcpy(cs, s + 2, n < sizeof cs - 1 ? n : sizeof cs - 1);
  cs[n < sizeof cs - 1 ? n : sizeof cs - 1] = 0;
  copyUtf8(h.sender, sizeof h.sender, cs, sizeof h.sender - 1);
  h.body = bar + 1;
  if (u) { h.kind = Kind::Plain; return h; }
  if (!strncmp(h.body, "XOR:", 4)) { h.kind = Kind::Xor; h.body += 4; return h; }
  h.kind = Kind::Aes;
  if (!strncmp(h.body, "AES:", 4)) h.body += 4;
  return h;
}

// An AES body's parts. `ct` gets the ciphertext (as long as the plaintext).
inline bool aesSplit(const char* b64, uint8_t iv[12], uint8_t* ct, size_t ct_cap, size_t* ct_len,
                     uint8_t tag[16]) {
  uint8_t all[kMaxBytes];
  size_t n = 0;
  if (!b64Decode(b64, strlen(b64), all, sizeof all, &n) || n <= 12 + 16 || n - 28 > ct_cap) return false;
  memcpy(iv, all, 12);
  memcpy(ct, all + 12, n - 28);
  memcpy(tag, all + n - 16, 16);
  *ct_len = n - 28;
  return true;
}

// Wavest's XOR fallback, opened with `passkey` (byte-wise: right for ASCII text).
inline bool xorOpen(const char* b64, const char* passkey, char* out, size_t cap) {
  const size_t kl = passkey ? strlen(passkey) : 0;
  size_t n = 0;
  if (!kl || !cap || !b64Decode(b64, strlen(b64), (uint8_t*)out, cap - 1, &n)) return false;
  for (size_t i = 0; i < n; i++) out[i] = (char)((uint8_t)out[i] ^ (uint8_t)passkey[i % kl]);
  out[n] = 0;
  return true;
}

// ---- share links ---------------------------------------------------------------------------

inline int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline bool hexBytes(const char* s, size_t n_chars, uint8_t* out) {
  if (n_chars % 2) return false;
  for (size_t i = 0; i < n_chars; i += 2) {
    const int a = hexValue(s[i]), b = hexValue(s[i + 1]);
    if (a < 0 || b < 0) return false;
    out[i / 2] = (uint8_t)(a << 4 | b);
  }
  return true;
}

// The value of `key` in a link's query, URL-decoded ('+' and %20 are spaces).
inline bool queryValue(const char* link, const char* key, char* out, size_t cap) {
  const char* q = strchr(link, '?');
  if (!q || !cap) return false;
  const size_t kl = strlen(key);
  for (const char* p = q + 1; *p;) {
    const char* end = strchr(p, '&');
    if (!end) end = p + strlen(p);
    if ((size_t)(end - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
      size_t o = 0;
      for (const char* v = p + kl + 1; v < end && o + 1 < cap; v++) {
        if (*v == '+') out[o++] = ' ';
        else if (*v == '%' && v + 2 < end && hexValue(v[1]) >= 0 && hexValue(v[2]) >= 0) {
          out[o++] = (char)(hexValue(v[1]) << 4 | hexValue(v[2]));
          v += 2;
        } else out[o++] = *v;
      }
      out[o] = 0;
      return true;
    }
    p = *end ? end + 1 : end;
  }
  return false;
}

inline bool linkIs(const char* s, const char* what) {   // what: "channel/add", "contact/add"
  static const char* const scheme = "meshcore://";
  for (size_t i = 0; scheme[i]; i++)
    if (!s[i] || (s[i] | 0x20) != scheme[i]) return false;
  s += strlen(scheme);
  const size_t n = strlen(what);
  for (size_t i = 0; i < n; i++)
    if (!s[i] || (s[i] | 0x20) != what[i]) return false;
  return s[n] == '?' || (s[n] == '/' && s[n + 1] == '?');
}

inline void trimInPlace(char* s) {
  char* p = s;
  while (*p == ' ') p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  size_t n = strlen(s);
  while (n && s[n - 1] == ' ') s[--n] = 0;
}

inline bool parseChannelLink(const char* s, char* name, size_t cap, uint8_t secret[16]) {
  char hex[80];
  if (!linkIs(s, "channel/add") || !queryValue(s, "secret", hex, sizeof hex)) return false;
  trimInPlace(hex);
  if (strlen(hex) != 32 || !hexBytes(hex, 32, secret)) return false;
  if (!queryValue(s, "name", name, cap)) name[0] = 0;
  trimInPlace(name);
  return true;
}

inline bool parseContactLink(const char* s, char* name, size_t cap, uint8_t pub[32], uint8_t* type) {
  char hex[80];
  if (!linkIs(s, "contact/add") || !queryValue(s, "public_key", hex, sizeof hex)) return false;
  trimInPlace(hex);
  if (strlen(hex) != 64 || !hexBytes(hex, 64, pub)) return false;
  if (!queryValue(s, "name", name, cap)) name[0] = 0;
  trimInPlace(name);
  char t[8];
  *type = 1;
  if (queryValue(s, "type", t, sizeof t) && t[0] >= '1' && t[0] <= '4' && !t[1]) *type = (uint8_t)(t[0] - '0');
  return true;
}

inline size_t urlEncode(const char* s, char* out, size_t cap) {
  static const char* const k = "0123456789ABCDEF";
  size_t o = 0;
  for (; *s; s++) {
    const unsigned char c = (unsigned char)*s;
    const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       c == '-' || c == '_' || c == '.' || c == '~';
    if (o + (plain ? 1 : 3) >= cap) break;
    if (plain) out[o++] = (char)c;
    else { out[o++] = '%'; out[o++] = k[c >> 4]; out[o++] = k[c & 15]; }
  }
  if (cap) out[o] = 0;
  return o;
}

inline void hexLower(const uint8_t* in, size_t n, char* out) {
  static const char* const k = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[2 * i] = k[in[i] >> 4]; out[2 * i + 1] = k[in[i] & 15]; }
  out[2 * n] = 0;
}

inline size_t buildChannelLink(const char* name, const uint8_t secret[16], char* out, size_t cap) {
  char n[100], h[33];
  urlEncode(name, n, sizeof n);
  hexLower(secret, 16, h);
  const int r = snprintf(out, cap, "meshcore://channel/add?name=%s&secret=%s", n, h);
  return r > 0 && (size_t)r < cap ? (size_t)r : 0;
}

inline size_t buildContactLink(const char* name, const uint8_t pub[32], char* out, size_t cap) {
  char n[100], h[65];
  urlEncode(name, n, sizeof n);
  hexLower(pub, 32, h);
  const int r = snprintf(out, cap, "meshcore://contact/add?name=%s&public_key=%s&type=1", n, h);
  return r > 0 && (size_t)r < cap ? (size_t)r : 0;
}

// ---- resampling ----------------------------------------------------------------------------
// The mic and the P4's speaker run at 16 kHz, ggwave at 24 kHz inside. ggwave's
// own resampler costs ~80x its decoder (desktop measurement), too much for an
// ESP32-S3 listening in real time, so the radios convert with this instead: a
// polyphase windowed-sinc filter, 32 taps per output sample, flat to ~6.7 kHz
// (the audible protocols reach 6.4 kHz). Works for any ratio up to 8:8.
class Resampler {
 public:
  static constexpr int kTaps = 32;   // per phase
  static constexpr int kMaxL = 8;

  bool begin(int in_rate, int out_rate) {
    int a = in_rate, b = out_rate;
    while (b) { const int t = a % b; a = b; b = t; }
    if (a <= 0) return false;
    L_ = out_rate / a;
    M_ = in_rate / a;
    if (L_ < 1 || L_ > kMaxL || M_ < 1 || M_ > kMaxL) return false;
    const int n = L_ * kTaps;
    const double fs = (double)in_rate * L_;
    const double fc = 0.47 * (in_rate < out_rate ? in_rate : out_rate) / fs;   // of the upsampled rate
    double sum = 0;
    for (int i = 0; i < n; i++) {
      const double x = i - (n - 1) / 2.0;
      const double sinc = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
      const double w = 0.54 - 0.46 * cos(2 * M_PI * i / (n - 1));   // Hamming
      h_[i] = (float)(sinc * w);
      sum += h_[i];
    }
    for (int i = 0; i < n; i++) h_[i] = (float)(h_[i] * L_ / sum);
    reset();
    return true;
  }
  void reset() {
    memset(hist_, 0, sizeof hist_);
    pos_ = 0;
    count_ = 0;
    next_ = 0;
  }
  // Takes `n` samples; writes up to `cap` and returns how many. Give it room
  // for n * out / in + 2.
  size_t process(const int16_t* in, size_t n, int16_t* out, size_t cap) {
    size_t o = 0;
    for (size_t k = 0; k < n; k++) {
      pos_ = (pos_ + 1) % kTaps;
      hist_[pos_] = in[k];
      count_++;
      while (next_ < count_ * (uint32_t)L_ && o < cap) {
        const int p = (int)(next_ - (count_ - 1) * (uint32_t)L_);   // phase, 0..L-1
        float acc = 0;
        int idx = pos_;
        for (int t = 0; t < kTaps; t++) {
          acc += h_[p + t * L_] * hist_[idx];
          idx = idx ? idx - 1 : kTaps - 1;
        }
        const int v = (int)lrintf(acc);
        out[o++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        next_ += (uint32_t)M_;
      }
      if (count_ >= 0x01000000u) {   // keep the counters small
        const uint32_t drop = count_ - 1;
        count_ -= drop;
        next_ -= drop * (uint32_t)L_;
      }
    }
    return o;
  }

 private:
  int L_ = 1, M_ = 1;
  float h_[kMaxL * kTaps] = {};
  float hist_[kTaps] = {};
  int pos_ = 0;
  uint32_t count_ = 0, next_ = 0;
};

// ---- the bridge ----------------------------------------------------------------------------

// A channel message as it arrives, "<sender>: <text>", split. No sender: "".
inline const char* splitSender(const char* msg, char* sender, size_t cap) {
  const char* sep = strstr(msg, ": ");
  if (!sep || (size_t)(sep - msg) >= cap || sep == msg) { if (cap) sender[0] = 0; return msg; }
  memcpy(sender, msg, (size_t)(sep - msg));
  sender[sep - msg] = 0;
  return sep + 2;
}

// Whether a channel message is one the bridge posted (never played back).
inline bool isBridgePost(const char* text) { return !strncmp(text, kBridgeTag, strlen(kBridgeTag)); }

}  // namespace soundlink
