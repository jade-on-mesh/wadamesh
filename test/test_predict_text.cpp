// SPDX-License-Identifier: GPL-3.0-or-later
// Predictive text (src/ui-touch/PredictText.h).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui-touch/PredictText.h"

using namespace predict;

int main() {
  static Dict d;
  char out[kSuggest][kWordMax];
  // Built-in words, most common first.
  int n = d.suggest("th", 2, out);
  assert(n == 3 && !strcmp(out[0], "the") && !strcmp(out[1], "that"));
  assert(d.suggest("t", 1, out) == 0);   // two letters at least
  assert(d.suggest("zzq", 3, out) == 0);
  // Capitalised.
  n = d.suggest("Rep", 3, out);
  assert(n >= 1 && !strcmp(out[0], "Reply"));
  // Your words come first, the more you send them.
  d.learnText("Meet at Thornton Creek trailhead, thornton creek again");
  d.learnText("Thornton is closed");
  n = d.suggest("tho", 3, out);
  assert(n >= 1 && !strcmp(out[0], "Thornton"));   // as first written, 3 times
  n = d.suggest("tr", 2, out);
  assert(!strcmp(out[0], "trailhead"));
  // Short words, numbers and the prefix itself aren't suggested.
  d.learnText("ok 12345 abc123");
  assert(d.suggest("12", 2, out) == 0);
  assert(d.suggest("the", 3, out) >= 1 && strcmp(out[0], "the") != 0);
  // The word at the caret.
  size_t len;
  const char* t = "heading to thorn";
  size_t s = wordAt(t, strlen(t), &len);
  assert(s == 11 && len == 5);
  s = wordAt("hi ", 3, &len);
  assert(len == 0);
  // Save and load keep the counts.
  char buf[4096];
  d.save(buf, sizeof buf);
  static Dict e;
  e.load(buf, strlen(buf));
  assert(e.count == d.count);
  n = e.suggest("tho", 3, out);
  assert(!strcmp(out[0], "Thornton"));
  // Full: the least used goes.
  for (int i = 0; i < kLearnMax + 20; i++) {
    char w[16];
    snprintf(w, sizeof w, "word%c%c", 'a' + i % 26, 'a' + i / 26);
    d.learnText(w);
  }
  assert(d.count == kLearnMax);
  n = d.suggest("tho", 3, out);
  assert(!strcmp(out[0], "Thornton"));   // used three times: kept
  printf("predict text: ok\n");
  return 0;
}
