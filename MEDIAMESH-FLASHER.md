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
- `.github/workflows/flasher-pages.yml` - publishes the page to GitHub Pages with a copy of
  the newest Wadamesh TAK images and all Spectra-Cascadia images, via the gh-pages branch.
- `proxy/cors-proxy-worker.js` - Cloudflare Worker that relays release downloads with CORS
  headers. Needed for Keymind Cascade (too many images to copy to Pages). Put its URL in
  `CONFIG.proxy`. Without it, the page asks people to download the file and drop it in.

## Why the relay
GitHub release download links send no CORS headers, so browsers block a page from reading
them. The page itself, the catalog (raw.githubusercontent.com) and the GitHub API are fine.

## Putting it on WordPress (cascadiamesh.org)
Paste `wordpress/mediamesh-flasher-block.html` into a **Custom HTML** block. The whole page is
inside it, so nothing else needs hosting for the page itself; the downloads still come from the
Pages copy (Wadamesh TAK, Spectra-Cascadia) or the relay (Keymind Cascade). Re-paste it when
`flasher/index.html` changes.

Or, once Pages is on, a plain iframe that always shows the latest page:
`<iframe src="https://jade-on-mesh.github.io/wadamesh-tak/" allow="serial" style="width:100%;height:2400px;border:0"></iframe>`

Open a tab directly with `#cascade` or `#spectra`, or a Wadamesh board with `#tdeck`, `#m9`, `#pager`, `#p4`.
