# Vendored OTA engine

These files are Mike Carper's portable LoRa OTA engine, copied unchanged from
[mikecarper/meshcore](https://github.com/mikecarper/meshcore) at commit
`efad0076816f7ba2293c68f56f8db18ac17344b9` (`src/helpers/ota/`). They are MIT
licensed, like MeshCore (Copyright (c) 2025 Scott Powell / rippleradios.com and
contributors). The protocol is described in his `docs/ota_protocol.md`.

Wadamesh uses the engine only to *serve* `.mota` files from the SD card to a
repeater during a LoRa Update (`../LoraUpdate.cpp`, `../LoraUpdateCore.h`,
`../../ui-touch/LoraUpdate.inc`, `LORA_UPDATE.md`). To update these files, copy
the same list from a newer commit and run `test/test_lora_update.cpp`.
