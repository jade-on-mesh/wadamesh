#pragma once
// T-Deck radio SPI: RadioLib's Arduino HAL, sending each command in one
// buffered transfer at 8.89 MHz instead of byte by byte at RadioLib's default
// 2 MHz. Transactions, chip select, BUSY polling and status parsing stay
// RadioLib's own.
//
// From keymindCascade (mikecarper/MeshCore d4c826322, measured on a T-Deck,
// 18 Oct 2026): bulk transfers passed 10,000 paired modem/sync-word reads at
// 8 MHz and at 8,888,888 Hz (80 MHz / 9), and failed from 10 MHz up on this
// board's wiring, so 8.89 MHz is the setting. That is a limit of this link,
// not of the SX1262 (16 MHz in its datasheet). Byte-by-byte transfers read
// correctly faster, but each byte is its own SPI operation.
#include <hal/Arduino/ArduinoHal.h>

#ifndef TDECK_RADIO_SPI_HZ
#define TDECK_RADIO_SPI_HZ 8888888
#endif

class TDeckRadioHal : public ArduinoHal {
 public:
  explicit TDeckRadioHal(SPIClass& bus, uint32_t hz = TDECK_RADIO_SPI_HZ)
      : ArduinoHal(bus, SPISettings(hz, MSBFIRST, SPI_MODE0)) {}
  void spiTransfer(uint8_t* out, size_t len, uint8_t* in) override {
    if (len) spi->transferBytes(out, in, len);
  }
};
