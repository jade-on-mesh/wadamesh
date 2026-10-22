# Team on Home

Three Home widgets for standalones on a team, and a **Team** preset that lays
them out with battery, unread, the clock, mentions and GPS (*Settings › Display
› Home › Start over from a preset › Team*, or hold Home and add them one by
one; they're under *Mesh*).

| Widget | Shows | Tap |
| --- | --- | --- |
| **Nearest teammate** | Who, how far and which way ("420 m NE"), when last heard, their battery and status; red with SOS | Their sheet (Hound, Map, answer an alert) |
| **Latest alert** | A teammate's alert while one is on (red), else the newest watched report (#icewatch…) in its colour, with distance and age | The alarm, or the report's sheet |
| **Mesh health** | A score each minute, 0–100, with the last half hour as a curve: up to 50 for how much was heard (5 a packet) and up to 50 for how well (the last packet's SNR, −15 to +10 dB). Good from 60, fair from 30 | Signal & traffic |

Each comes small (one cell: a figure and a word) or wide (two cells: title,
detail, and the curve for mesh health).

Not on the ThinkNode M9 or the T-Display P4, whose app space is full
(`CAP_TAK_EXTRAS`). Their kinds are numbered from 200 in the saved layout so
they never clash with upstream Wadamesh's.

## Code

`src/ui-touch/UITask.cpp`: `hwMate*`, `hwAlert*`, `hwMeshHealth*`, `kHwPresetTeam`.
