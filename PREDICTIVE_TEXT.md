# Predictive text

While you type a message, up to three words appear in a slim strip along the
top of the message field, in the small font, the best one first and brightest.
Tap one to put it in, with a space after. They are, as on a phone keyboard:

- **Completions** of the word you're on, from its first letter.
- **Corrections** of a mistyped word: one letter wrong, missing or extra, or
  two swapped ("teh" -> "the", "recieve" -> "receive", "becuase" -> "because").
  A slip onto a neighbouring key counts as the likeliest. A missing apostrophe
  is always fixed ("dont" -> "don't", "im" -> "I'm").
- **The next word**, after a space, before you've typed any of it: "on my"
  offers "way", "thank" offers "you". After a full stop the suggestions are
  capitalised.

What comes first depends on the word before as well as the word itself.
Completions that usually follow the previous word rank higher ("good mor" ->
"morning").

**Space fixes or completes a word that isn't one**, as on a phone. "teh" +
Space gives "the "; "thoguh" + Space gives "though ". A real word is left
alone: "in" stays "in". Of the real words, only the spelling of "i" changes,
to "I". A short word (three letters or fewer) is only corrected for a likely
slip, so "thx" stays "thx".

A Backspace straight after puts back what you typed, without the space. A
space then keeps it as typed, and the word is learned, so it isn't changed
again.

*Settings › Keyboard › Typing › Space completes the word* turns just this off.

## Where the words come from

- **The words you send.** The more often you send a word, and the more recently,
  the higher it ranks. Place names, callsigns and channel words you use show up
  after you've sent them once. Words with digits aren't learned. Up to 400 words.
- **The word pairs you send** ("on my radio"), for the next word: your pairs
  outrank the built-in ones. Up to 400 pairs.
- **A built-in dictionary**: the 10,000 most used English words with how often
  each is used, plus mesh and team words ("repeater", "roger", "checkpoint",
  "MeshCore"...).
  - The dictionary is [wordfreq](https://github.com/rspeer/wordfreq)'s English
    list (Robyn Speer et al., CC-BY-SA 4.0). It is built from subtitles,
    Reddit, Twitter, news and Wikipedia, so it knows chat words ("gonna", "lol",
    "tho").
  - Profanity is left out of suggestions.
  - Apostrophe-less contractions ("dont") are left out so they get corrected.
  - The dictionary takes about 57 KB of flash.
- **A built-in table of what usually comes next** in chat on the mesh.

A capital typed ("Tho") gives capitalised words ("Thornton"). The strip steps
aside for the @-mention and accent pickers. The learned words and pairs are
kept in `/words` on the internal store, written 30 s after a send. Older
firmware reads the words from that file and skips the pairs.

*Settings › Keyboard › Typing › Predictive text* turns it on or off. It isn't
on the T-Display P4, whose app space is full (`CAP_TAK_EXTRAS`).

## Rebuilding the dictionary

```
pip install wordfreq better_profanity
python3 scripts/predict/gen_words.py            # PREDICT_WORDS=15000 for more
```

This writes `src/ui-touch/PredictWords.inc`. The words are sorted, front-coded in
blocks of 32 with an index for binary search, and carry a frequency byte (zipf × 10).

## Code

| Piece | Where |
| --- | --- |
| Dictionary lookup, completions, corrections, next word, ranking, Space's choice, learning, save/load (host-tested) | `src/ui-touch/PredictText.h`, `test/test_predict_text.cpp` |
| The dictionary (generated) | `src/ui-touch/PredictWords.inc`, `scripts/predict/gen_words.py` |
| The strip over the message field, Space (`wordSpaceComplete`), learning on send and on undo, the settings | `wordBoxMaybeShow`, `predictLearn`, `predictToggleCb` in `src/ui-touch/UITask.cpp` |
