// SPDX-License-Identifier: GPL-3.0-or-later
// Predictive text with the dictionary loaded from its file, as the T-Display P4
// does (PREDICT_BUILTIN_WORDS 0): dictionaries/words-en.pwd.
#define PREDICT_BUILTIN_WORDS 0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui-touch/PredictText.h"

using namespace predict;

static Suggestions at(const Dict& d, const char* text) {
  size_t wl;
  const size_t a = wordAt(text, strlen(text), &wl);
  Suggestions s;
  d.suggest(text, a, wl, s);
  return s;
}

int main(int argc, char** argv) {
  static Dict d;
  // No dictionary yet: nothing breaks, your own words and the next-word table still work.
  assert(!dict::loaded() && dict::count() == 0);
  assert(dict::find("the", 3) == 0);
  Suggestions s = at(d, "teh");
  assert(s.n == 0 && s.space[0] == 0);   // nothing to correct against
  s = at(d, "thank ");
  assert(s.n >= 1 && !strcmp(s.w[0], "you"));
  d.learnText("Thornton creek trailhead");
  char out[kSuggest][kWordMax];
  assert(d.suggest("tho", 3, out) >= 1 && !strcmp(out[0], "Thornton"));

  // The file.
  const char* path = argc > 1 ? argv[1] : "dictionaries/words-en.pwd";
  FILE* f = fopen(path, "rb");
  assert(f);
  static uint8_t img[200000];
  const size_t n = fread(img, 1, sizeof img, f);
  fclose(f);
  assert(n == words::kFileBytes);
  // A damaged or different file is refused, and changes nothing.
  img[100] ^= 1;
  assert(!dict::loadFile(img, n) && !dict::loaded());
  img[100] ^= 1;
  assert(!dict::loadFile(img, n - 1) && !dict::loaded());
  assert(dict::loadFile(img, n) && dict::loaded() && dict::count() == words::kFileWords);

  // Then it behaves as the compiled-in one (test_predict_text.cpp).
  assert(dict::find("the", 3) > 70);
  char disp[kWordMax];
  assert(dict::find("i", 1, disp) && !strcmp(disp, "I"));
  int seen = 0;
  dict::scanFrom(0, [&](const char*, size_t, uint8_t) { seen++; return true; });
  assert(seen == dict::count());
  s = at(d, "teh");
  assert(!strcmp(s.w[0], "the") && !strcmp(s.space, "the"));
  s = at(d, "I dont");
  assert(!strcmp(s.space, "don't"));
  s = at(d, "on my ");
  assert(!strcmp(s.w[0], "way"));
  assert(d.suggest("tho", 3, out) >= 1 && !strcmp(out[0], "Thornton"));
  printf("predict text (dictionary file): ok\n");
  return 0;
}
