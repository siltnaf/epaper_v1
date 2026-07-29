# ESP32-S3 e-paper bring-up

This is a clean PlatformIO/Arduino starting point for the custom ESP32-S3
board in `schematic.pdf`. The old `d:/project/epaper_s3` application is not
copied wholesale because it contains unrelated TTS, database, content and
board-specific application code.

## Build and upload

From this directory:

```text
pio run
pio run -t upload
pio device monitor
```

The environment uses the generic `esp32-s3-devkitc-1` board definition. If
your module has a different flash/PSRAM configuration, update `platformio.ini`
after the basic GPIO/SPI test is working.

## E-paper pins

| Signal | ESP32-S3 GPIO |
|---|---:|
| DIN / MOSI | 3 |
| CLK / SCLK | 4 |
| CS | 5 |
| DC | 6 |
| RST | 7 |
| BUSY | 8 |

The remaining assignments are centralized in `include/board_pins.h`, including
I2C (12/13), SD, battery ADC, touch, audio and ML307R modem signals.

## First test

`src/main.cpp` clears the panel, draws a two-part black/white test pattern,
then puts the panel to sleep. The controller sequence and 240x416 frame format
are adapted from `paper_xingtai/demo/User/e-Paper/EPD_3in7_new.c`.

The reference code uses a conservative BUSY delay and the BUSY polarity can
vary between panel revisions. The implementation uses GPIO8 with a timeout;
if the panel never refreshes, check the panel's BUSY polarity and adjust
`XingtaiEpd::waitBusy()`.
