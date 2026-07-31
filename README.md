# ESP32-S3 3.7-inch e-paper bring-up

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

## Project structure

The project is intentionally split into device drivers and page modules:

```text
include/devices/<device>/<device>.h
src/devices/<device>/<device>.cpp
include/pages/<page>/<page>.h
src/pages/<page>/<page>.cpp
src/pages/<page>/assets/       # icons and images used only by that page
```

The current 3.7-inch Xingtai / UC8253 device is in `devices/epd_xingtai`.
The portrait page that draws the Asundar logo above centered `Asundar` is in
`pages/asundar`. `src/main.cpp` only performs application startup and invokes
the selected page.

## Fonts

The `Asundar` startup page uses the Fira Sans glyphs extracted into
`include/font/firasans_asundar.h`. Only the six glyphs required by the startup
word are included, and their original 4-bit antialias data is thresholded to
the panel's 1-bit framebuffer. The old `include/font/basic_font.h` remains
available for future compact UI text but is not used by this page.

## First test

The firmware initializes the 3.7-inch, 240x416 panel in portrait orientation,
renders `Asundar` centered on a white framebuffer, refreshes the display, and
then puts the panel to sleep. The controller sequence and frame format are
adapted from `paper_xingtai/demo/User/e-Paper/EPD_3in7_new.c`.

The vendor reference code uses a conservative fixed 500 ms delay because BUSY
polarity/behavior varies between panel revisions. `XingtaiEpd::waitBusy()`
therefore follows that reference behavior instead of reporting a BUSY timeout.
