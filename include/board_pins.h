#pragma once

// ESP32-S3 custom e-paper board pin map.
// Source: ESP32-S3_ePaper_PinMapping.xlsx and schematic.pdf.

#include <Arduino.h>

namespace BoardPins {
// ESP32 controls
constexpr int BOOT = 0;
constexpr int WAKE = 1;

// 3.7-inch Xingtai / UC8253 e-paper interface
constexpr int EP_DIN = 3;   // SPI MOSI
constexpr int EP_CLK = 4;   // SPI SCLK
constexpr int EP_CS = 5;
constexpr int EP_DC = 6;
constexpr int EP_RST = 7;
constexpr int EP_BUSY = 8;

// System
constexpr int BAT_ADC = 9;

// SD card, 4-bit SDMMC wiring
constexpr int SD_PWR = 10;
constexpr int SD_DATA1 = 14;
constexpr int SD_DATA0 = 15;
constexpr int SD_CLK = 16;
constexpr int SD_CMD = 17;
constexpr int SD_DATA3 = 18;
constexpr int SD_DATA2 = 21;

// USB native signals are not configured as GPIO outputs by this project.
constexpr int USB_D_MINUS = 19;
constexpr int USB_D_PLUS = 20;

// Shared I2C bus (FT6336 e-paper touch controller / audio codec)
constexpr int I2C_SCL = 12;
constexpr int I2C_SDA = 13;

// Audio
constexpr int PA_EN = 35;
constexpr int TOUCH_INT = 36;
constexpr int TOUCH_PWR = 37;
constexpr int AUDIO_DOUT = 38;
constexpr int AUDIO_LRCLK = 39;
constexpr int AUDIO_SCLK = 40;
constexpr int AUDIO_DIN = 41;
constexpr int AUDIO_MCLK = 42;

// ML307R modem
constexpr int MODEM_RX = 43; // ESP RX, modem TX
constexpr int MODEM_TX = 44; // ESP TX, modem RX
constexpr int MODEM_PWR = 11;
}
