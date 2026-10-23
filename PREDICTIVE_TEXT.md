# Predictive text

While you type a message, up to three words that finish the one you're on
appear in a slim strip along the top of the message field once it's two
letters long, in the small font, the best match first and brightest; tap one to
put it in, with a space after.

**Space completes the word**, as on a phone: typing a space straight after a
word the strip has a longer match for puts in the best one ("tho" + Space ->
"though "). A Backspace straight after puts back what you typed, without the
space, and a space then keeps it as typed. *Settings › Keyboard › Typing ›
Space completes the word* turns just this off. They come from:

- **the words you send**, first: the more often you send a word (and the more
  recently), the higher it ranks. Place names, callsigns and channel words you
  use show up after you've sent them once. Words under three letters and
  anything with digits aren't learned.
- **a built-in list** of about 300 common English words and mesh/team words
  ("repeater", "channel", "roger", "checkpoint"...).

A capitalised start ("Tho") gives capitalised words ("Thornton"). The strip steps
aside for the @-mention and accent pickers. The learned words (up to 200) are
kept in `/words` on the internal store, written 30 s after a send.

*Settings › Keyboard › Typing › Predictive text* turns it on or off. Not on the
T-Display P4, whose app space is full (`CAP_TAK_EXTRAS`).

## Code

| Piece | Where |
| --- | --- |
| Word list, learning, ranking, save/load (host-tested) | `src/ui-touch/PredictText.h`, `test/test_predict_text.cpp` |
| The strip over the message field, Space completing (`wordSpaceComplete`), learning on send, the settings | `wordBoxMaybeShow`, `predictLearn`, `predictToggleCb` in `src/ui-touch/UITask.cpp` |
