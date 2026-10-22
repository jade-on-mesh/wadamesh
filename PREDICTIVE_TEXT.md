# Predictive text

While you type a message, up to three words that finish the one you're on
appear over the composer once it's two letters long; tap one to put it in,
with a space after. They come from:

- **the words you send**, first: the more often you send a word (and the more
  recently), the higher it ranks. Place names, callsigns and channel words you
  use show up after you've sent them once. Words under three letters and
  anything with digits aren't learned.
- **a built-in list** of about 300 common English words and mesh/team words
  ("repeater", "channel", "roger", "checkpoint"...).

A capitalised start ("Tho") gives capitalised words ("Thornton"). The box steps
aside for the @-mention and accent pickers. The learned words (up to 200) are
kept in `/words` on the internal store, written 30 s after a send.

*Settings › Keyboard › Typing › Word suggestions* turns it off. Not on the
ThinkNode M9 or the T-Display P4, whose app space is full (`CAP_TAK_EXTRAS`).

## Code

| Piece | Where |
| --- | --- |
| Word list, learning, ranking, save/load (host-tested) | `src/ui-touch/PredictText.h`, `test/test_predict_text.cpp` |
| The box over the composer, learning on send, the setting | `wordBoxMaybeShow`, `predictLearn`, `predictToggleCb` in `src/ui-touch/UITask.cpp` |
