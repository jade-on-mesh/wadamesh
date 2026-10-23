# MediaMesh Flasher

One page, three tabs:
- **Wadamesh TAK** (standalone radios): T-Deck, ThinkNode M9, T-LoRa Pager, T-Display P4.
- **Keymind Cascade** (companions and repeaters): read live from Keymind's Mesh America
  provider catalog (mikecarper/MeshCore, keymindCascade branch), so new boards and
  releases show up without editing the page.
- **Spectra-Cascadia** (DX-LR30 repeaters): Standard (.bin, 64 KB) or XL (.hex, 128 KB), US or EU,
  from jade-on-mesh/spectra-cascadia releases.

ESP32-family boards flash over Web Serial (esptool-js, merged image at 0x0, chip checked
before writing). nRF52 boards get UF2 drag-and-drop instructions plus the .uf2 / DFU .zip.
The DX-LR30 flashes through the STM32 ROM bootloader over its CH340 (57600 baud, 8E1, after
BOOT0 + RESET): only the pages the image covers are erased, so the settings pages at
0x0800F800-0x0800FFFF (name, identity, passwords) survive, and every block is read back and
compared before the node is started. XL asks for confirmation that the board passed
`tools/flash128_test`, and stops with a clear message if the chip refuses the upper 64 KB.

## Files
- `flasher/index.html` - the page. Settings are in `CONFIG` at the top of the script.
- `.github/workflows/flasher-pages.yml` - publishes the page to GitHub Pages (gh-pages branch) with copies of
  the newest Wadamesh TAK images, all Spectra-Cascadia images and Keymind Cascade's ESP32
  companion and repeater USB images, so every tab downloads with one click. Runs after each
  Wadamesh TAK release, daily, and by hand (Actions > flasher-pages > Run workflow).
- `proxy/cors-proxy-worker.js` - optional Cloudflare Worker relay, only needed if a file isn't on
  the Pages copy (for example a Keymind build published since the last run). Put its URL in
  `CONFIG.proxy`.

## Why the copies
GitHub release download links send no CORS headers, so browsers block a page from reading
them. The page itself, the catalog (raw.githubusercontent.com) and the GitHub API are fine.

## Putting it on WordPress (cascadiamesh.org)
Paste `wordpress/mediamesh-flasher-block.html` into a **Custom HTML** block. It shows the live
page from GitHub Pages in a frame that sizes itself, so later changes appear without re-pasting.
Set `THEME` in the block to "light", "dark" or "auto".

Open a tab directly with `#cascade` or `#spectra`, or a Wadamesh board with `#tdeck`, `#m9`, `#pager`, `#p4`.
