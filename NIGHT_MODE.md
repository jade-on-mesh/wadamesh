# Red light (night vision)

**Settings > Theme > Red light** shows everything in dim red, to keep your eyes used to the dark: the menus, the map tiles and icons, photos, the lock screen, all of it. It turns on and off straight away, with no restart, and stays as you left it.

The quickest way there in the dark: open the control centre (tap the status bar) and **hold the Theme tile**. Its page opens with Red light at the top.

Turn the screen brightness down too (control centre) for the darkest result: red light keeps colours from hurting night vision, brightness decides how much light there is.

## How it works

Each pixel is turned into its brightness (0.2126 R + 0.7152 G + 0.0722 B) in red alone on its way to the screen (`redLightPixels`, in the display flush in `src/ui-touch/UITask.cpp`). MeshCore Open TAK's **Red light (night vision)** setting is the same filter, so a standalone and the app look alike at night. Not on the e-paper T-Deck Pro.
