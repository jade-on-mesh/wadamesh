// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Predictive text (PREDICTIVE_TEXT.md): word suggestions over the composer as
// you type, from a small built-in list of common words and the words you send.
// Words you use rank first, the more often and the more recently the higher.
//
// Pure logic, header-only, host-tested in test/test_predict_text.cpp.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace predict {

// Common English words, most frequent first, and words heard on the mesh.
static const char* const kBase[] = {
  "the", "to", "and", "you", "that", "it", "for", "is", "on", "in", "have", "with", "this", "be", "are",
  "not", "just", "was", "but", "what", "they", "can", "get", "all", "know", "like", "out", "here", "there",
  "about", "going", "good", "now", "back", "see", "will", "right", "think", "when", "from", "how", "yes",
  "today", "where", "time", "people", "thanks", "thank", "sure", "okay", "please", "need", "want", "would",
  "should", "could", "really", "still", "something", "anyone", "everyone", "someone", "nothing", "anything",
  "around", "because", "before", "after", "again", "always", "never", "maybe", "already", "almost",
  "tonight", "tomorrow", "yesterday", "morning", "afternoon", "evening", "night", "weekend", "minutes",
  "hours", "later", "soon", "early", "late", "coming", "leaving", "heading", "arrived", "waiting", "meeting",
  "location", "position", "north", "south", "east", "west", "street", "road", "highway", "bridge", "park",
  "parking", "station", "downtown", "hospital", "school", "store", "building", "corner", "near", "nearby",
  "behind", "between", "across", "inside", "outside", "home", "house", "office", "friend", "friends",
  "family", "everything", "everybody", "message", "messages", "received", "receive", "reply", "replied",
  "copy", "roger", "affirmative", "negative", "standby", "over", "check", "testing", "test", "signal",
  "repeater", "repeaters", "channel", "channels", "mesh", "network", "radio", "antenna", "battery", "solar",
  "power", "range", "coverage", "contact", "contacts", "advert", "node", "nodes", "frequency", "MeshCore",
  "Wadamesh", "photo", "picture", "voice", "team", "teammate", "status", "update", "updated", "report",
  "reports", "watch", "seen", "spotted", "vehicle", "vehicles", "agents", "police", "checkpoint", "drone",
  "drones", "safe", "help", "emergency", "medical", "water", "food", "supplies", "weather", "rain", "wind",
  "snow", "fire", "smoke", "traffic", "accident", "closed", "open", "working", "broken", "fixed", "sorry",
  "welcome", "hello", "morning", "great", "awesome", "nice", "cool", "perfect", "probably", "definitely",
  "actually", "exactly", "different", "important", "interesting", "together", "without", "through",
  "during", "another", "other", "every", "first", "last", "next", "little", "long", "small", "big", "same",
  "which", "while", "until", "since", "though", "these", "those", "their", "them", "then", "than", "said",
  "make", "made", "take", "took", "come", "came", "give", "look", "looking", "find", "found", "tell",
  "told", "call", "called", "try", "trying", "keep", "help", "send", "sent", "sending", "bring", "meet",
};
constexpr int kBaseN = (int)(sizeof kBase / sizeof kBase[0]);
constexpr size_t kWordMax = 24;
constexpr int kLearnMax = 200;
constexpr int kSuggest = 3;

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
// Letters (any non-ASCII byte counts: accented letters), digits and an apostrophe.
inline bool wordChar(char c) {
  const uint8_t b = (uint8_t)c;
  return b >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '\'';
}
inline bool startsNoCase(const char* w, const char* p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (!w[i] || lower(w[i]) != lower(p[i])) return false;
  return true;
}
inline bool eqNoCase(const char* a, const char* b) {
  for (; *a && *b; a++, b++)
    if (lower(*a) != lower(*b)) return false;
  return !*a && !*b;
}

// The word being typed: the run of word characters ending at `caret`
// (a byte offset into text). Returns its start; *len its length.
inline size_t wordAt(const char* text, size_t caret, size_t* len) {
  size_t s = caret;
  while (s > 0 && wordChar(text[s - 1])) s--;
  *len = caret - s;
  return s;
}

struct Learned {
  char w[kWordMax];
  uint16_t n;      // times sent
  uint32_t last;   // a counter: higher is more recent
};

class Dict {
 public:
  Learned e[kLearnMax];
  int count = 0;
  uint32_t clock = 0;

  void learnWord(const char* w, size_t len) {
    if (len < 3 || len >= kWordMax) return;   // short words are quick to type anyway
    char word[kWordMax];
    memcpy(word, w, len);
    word[len] = 0;
    for (size_t i = 0; i < len; i++)
      if (word[i] >= '0' && word[i] <= '9') return;   // not numbers or codes
    clock++;
    for (int i = 0; i < count; i++)
      if (eqNoCase(e[i].w, word)) { if (e[i].n < 60000) e[i].n++; e[i].last = clock; return; }
    int slot = count;
    if (count == kLearnMax) {   // full: the least used, then the oldest, goes
      slot = 0;
      for (int i = 1; i < count; i++)
        if (e[i].n < e[slot].n || (e[i].n == e[slot].n && e[i].last < e[slot].last)) slot = i;
    } else {
      count++;
    }
    memcpy(e[slot].w, word, len + 1);
    e[slot].n = 1;
    e[slot].last = clock;
  }

  // Every word of a sent message.
  void learnText(const char* t) {
    if (!t) return;
    while (*t) {
      while (*t && !wordChar(*t)) t++;
      const char* s = t;
      while (*t && wordChar(*t)) t++;
      if (t > s && *s != '@' && s[0] != '\'') learnWord(s, (size_t)(t - s));
    }
  }

  // Up to kSuggest completions of `prefix` (at least 2 characters), best first,
  // never the prefix itself. A capitalised prefix capitalises them.
  int suggest(const char* prefix, size_t plen, char out[][kWordMax], int cap = kSuggest) const {
    if (plen < 2 || plen >= kWordMax) return 0;
    struct C { const char* w; uint32_t score; };
    C c[16];
    int n = 0;
    auto add = [&](const char* w, uint32_t score) {
      if (!startsNoCase(w, prefix, plen) || strlen(w) <= plen) return;
      for (int i = 0; i < n; i++)
        if (eqNoCase(c[i].w, w)) { if (score > c[i].score) c[i].score = score; return; }
      if (n < 16) c[n++] = {w, score};
      else {
        int lo = 0;
        for (int i = 1; i < n; i++) if (c[i].score < c[lo].score) lo = i;
        if (score > c[lo].score) c[lo] = {w, score};
      }
    };
    for (int i = 0; i < count; i++) {
      // Use counts most; recency breaks ties and lifts this week's words.
      const uint32_t age = clock - e[i].last;
      add(e[i].w, 1000000u + (uint32_t)e[i].n * 1000u + (age < 1000 ? 999 - age : 0));
    }
    for (int i = 0; i < kBaseN; i++) add(kBase[i], (uint32_t)(kBaseN - i));
    for (int i = 1; i < n; i++)
      for (int j = i; j > 0 && c[j].score > c[j - 1].score; j--) { C t = c[j]; c[j] = c[j - 1]; c[j - 1] = t; }
    const bool cap1 = prefix[0] >= 'A' && prefix[0] <= 'Z';
    int k = 0;
    for (int i = 0; i < n && k < cap; i++) {
      strncpy(out[k], c[i].w, kWordMax - 1);
      out[k][kWordMax - 1] = 0;
      if (cap1 && out[k][0] >= 'a' && out[k][0] <= 'z') out[k][0] = (char)(out[k][0] - 'a' + 'A');
      k++;
    }
    return k;
  }

  // "word count\n" lines (counts only; recency starts over after a restart).
  size_t save(char* out, size_t cap) const {
    size_t o = 0;
    for (int i = 0; i < count; i++) {
      const size_t l = strlen(e[i].w);
      if (o + l + 8 >= cap) break;
      memcpy(out + o, e[i].w, l);
      o += l;
      out[o++] = ' ';
      unsigned v = e[i].n;
      char num[6];
      int d = 0;
      do { num[d++] = (char)('0' + v % 10); v /= 10; } while (v && d < 5);
      while (d) out[o++] = num[--d];
      out[o++] = '\n';
    }
    if (o < cap) out[o] = 0;
    return o;
  }
  void load(const char* s, size_t len) {
    count = 0;
    clock = 0;
    size_t i = 0;
    while (i < len && count < kLearnMax) {
      size_t a = i;
      while (i < len && s[i] != ' ' && s[i] != '\n') i++;
      const size_t wl = i - a;
      unsigned v = 0;
      if (i < len && s[i] == ' ') {
        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') v = v * 10 + (unsigned)(s[i++] - '0');
      }
      while (i < len && s[i] != '\n') i++;
      i++;
      if (wl < 3 || wl >= kWordMax || !v) continue;
      memcpy(e[count].w, s + a, wl);
      e[count].w[wl] = 0;
      e[count].n = (uint16_t)(v > 60000 ? 60000 : v);
      e[count].last = 0;
      count++;
    }
  }
};

}  // namespace predict
