#pragma once

#include <stdint.h>

namespace XiaozhiFont {

struct Glyph {
    uint16_t advance = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    int16_t offsetX = 0;
    int16_t offsetY = 0;
    uint16_t bitmapBytes = 0;
    uint8_t bitmap[256] = {};
};

// Checks the SD cache immediately, then starts a low-priority worker which
// waits for Wi-Fi and downloads the official Xiaozhi font only when missing.
void beginBackgroundProvisioning();
bool isAvailable();
bool isProvisioning();
const Glyph *glyph(uint32_t codepoint);
const char *cachePath();

}