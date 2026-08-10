# Xiaozhi Chinese font cache

The Booklist page uses the official Xiaozhi **PuHui 20 px / 4 bpp** font without
embedding its 1.24 MB bitmap in the application firmware. On the first Booklist
open, the firmware downloads Xiaozhi's release asset from:

`https://github.com/78/xiaozhi-fonts/releases/download/assets/none-font_puhui_common_20_4-none.bin`

At startup, a low-priority background task checks the `/fonts` folder and the
cached file `/fonts/xiaozhi-puhui-20.assets.bin`. A valid cache is opened
immediately. If it is missing, the task waits for Wi-Fi, downloads it, and saves
the successful Xiaozhi URL in ESP32 NVS/flash under the `xiaozhi-font` namespace.
The remembered URL is tried first on future downloads, with the built-in release
URL as a fallback. Booklist does not block while this happens. Later opens use
random-access glyph reads from SD and do not require network access. Removing
the cached file causes the background task to download it again on the next boot.

Boot initializes networking before mounting/checking the font cache. Wi-Fi is
the preferred transport whenever its setting is enabled: the saved network gets
a 15-second priority window and a successful Wi-Fi connection keeps the ML307R
powered down. If Wi-Fi is switched off, 4G power is applied immediately; if
Wi-Fi has no saved credentials or times out, the modem is powered as fallback.
While neither transport is connected, firmware explicitly retries the saved
Wi-Fi network every 15 seconds and probes ML307 registration/PDP state with
`AT+MIPCALL?` (requesting it with `AT+MIPCALL=1` when needed). Polling pauses as
soon as either Wi-Fi or a real 4G PDP context is connected, and resumes if that
active connection drops. The ML307 probe runs in a low-priority task so touch
and e-paper UI processing are not blocked by AT-command timeouts.
At present, `epaper_v1` has a complete Wi-Fi TCP/IP client and ML307R
power/registration/PDP-state management, but application HTTP requests are not
yet routed through the modem's AT HTTP commands. Font and Book HTTP downloads
therefore still use Wi-Fi sockets. True 4G downloading requires adding that
AT-based HTTP transport rather than treating PDP readiness alone as an ESP32
socket interface.

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

## FT6336 touch screen

The FocalTech driver from `FT6X36-master` is integrated under
`include/devices/ft6336` and `src/devices/ft6336`. The board powers the touch
controller through GPIO37, uses the shared I2C bus on SDA GPIO13/SCL GPIO12,
and receives active-low touch interrupts on GPIO36. The driver validates the
FT6206/FT6236/FT6336 chip ID, configures trigger interrupt mode, and processes
the queued interrupt events from the Arduino `loop()` context (I2C is never
performed inside the ISR).

Raw touch coordinates are rotated into the e-paper's 240x416 portrait coordinate
space in `src/main.cpp`. A tap on a home-page icon opens its page; a tap on the
upper-left Home touch target of a content page returns to the main page. Page refreshes
are deferred until after the touch callback returns so the callback remains
short and the e-paper refresh does not run in the driver's event dispatch.

## ES8311 audio codec

The ES8311 codec driver is implemented locally in
`include/devices/es8311/es8311.h` and `src/devices/es8311/es8311.cpp`. It uses
the same SDA GPIO13/SCL GPIO12 I2C bus as the FT6336. The ES8311's CE pin
selects its 7-bit I2C address (CE low → `0x18`, CE high → `0x19`). With R23
removed, wire the ES8311-side/lower R23 pad (`AUDIO_CE`) to GPIO34; firmware
drives GPIO34 high before the first codec probe and uses address `0x19` only.
Do not connect GPIO34 to the upper R23 pad, because that pad is `CODEC3.3V`.
The ESP32-S3 is the I2S master at 16 kHz/16-bit with MCLK GPIO42, BCLK GPIO40,
LRCLK GPIO39, ESP32 playback TX GPIO41 (`AUDIO_DIN`/ES8311 `DSDIN`), ESP32
recording RX GPIO38 (`AUDIO_DOUT`/ES8311 `ASDOUT`), and amplifier enable
GPIO35. Startup scans the shared bus before initializing either device and
prints all responding addresses to the serial monitor.

The application also releases and reinitializes the shared I2C bus after the
long e-paper startup refresh. If a peripheral holds SDA low after an interrupted
transaction, up to nine SCL recovery pulses are generated before `Wire` is
restarted.

The schematic also shows that ES8311 power comes from regulator U8, whose
enable net is `CODEC_PWR`. That net has a 100 kΩ pull-down but is not connected
to an ESP32 GPIO in the supplied schematic or pin spreadsheet. The driver
supports an optional `Pins::powerEnable` GPIO, but on the current PCB
`CODEC_PWR` must first be electrically tied high (or routed to a GPIO) before
the codec can acknowledge address `0x19`.

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
