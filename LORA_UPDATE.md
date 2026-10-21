# LoRa Update

LoRa Update lets a standalone radio update a repeater's firmware over LoRa. You
don't need a phone, Wi-Fi or a cable. The standalone serves a `.mota` firmware
file from its SD card, and the repeater pulls it, checks every block and installs
it.

It uses Mike Carper's LoRa OTA, the same system as **Repeater Management > LoRa
OTA** in MeshCore Open TAK. The engine is his, vendored in `src/helpers/ota`
(MIT, see the README there). The steps follow his app.

## What you need

- A repeater running Mike Carper's MeshCore firmware (keymindCascade) with LoRa
  OTA, which answers `ota status`. Stock repeaters don't have it.
- An admin login to that repeater on this standalone.
- The repeater in **direct range** of the standalone. During the update both
  move to a temporary radio that other repeaters don't relay.
- A standalone with an SX1262 radio: T-Deck, T-LoRa Pager (SX1262) or ThinkNode
  M9. The LR1121 Pager can't run a temporary radio.
- A `.mota` file built for the repeater's board, in the `/mota` folder of the
  standalone's SD card. You can build one with Mike's `motatool`, or download it
  with MeshCore Open TAK's LoRa OTA screen and copy it to the card. Up to eight
  files are offered.

## Updating a repeater

1. Log in to the repeater (**Contacts**, the repeater, **Admin**).
2. Tap the **arrow** button in the admin console's header, next to the MediaMesh
   antenna.
3. Choose the firmware file. Each row shows the file name, the version and its
   8-character id, which the repeater also prints.
4. Check the **temporary radio**. The defaults are Mike's: 909.950 MHz, 250 kHz,
   SF5, CR5, for 120 minutes. Pick a quiet frequency you're allowed to use, and
   give the window enough minutes for the whole download. A large image on a busy
   channel can take over an hour.
5. Tap **Start update** and confirm. Then:
   - **Safety check.** The repeater, then the standalone, switch to the
     temporary radio for three minutes, and the repeater must answer there. If it
     doesn't, both return to normal by themselves when the three minutes end.
   - **Switching.** The window is extended to the minutes you chose.
   - **Offering.** The standalone announces the file until the repeater lists it.
   - **Downloading.** The repeater pulls and verifies each block. The bar and
     **Blocks** show its progress, asked every 15 seconds.
   - **Ready to install.** The download is complete and checked.
6. Tap **Install and reboot** and confirm. The repeater flashes the new firmware
   and reboots, and the standalone returns to its normal radio. A minute later,
   check the repeater's version from the admin console (`ver`). If the install
   reply was lost, check the version before trying again.

**Stop** works at any point. It puts the repeater back with `normalradio`, then
the standalone. You can close the page or the admin console during an update and
it keeps going. Reopen it from the same repeater to watch it.

## Notes

- While the update runs, the standalone and the repeater are off the normal mesh,
  so messages wait until the window ends.
- The standalone only answers OTA packets during a session, from a direct
  neighbour, and never relays them.
- The repeater checks the file itself: every block against the signed Merkle
  root, then the whole image. A damaged or wrong file fails the download instead
  of being installed.
- Files that aren't valid `.mota` containers are left out, and the page says how
  many.
- Relayed updates (a repeater several hops away) and bootloader packages aren't
  supported on standalones yet. Use MeshCore Open TAK for those.
