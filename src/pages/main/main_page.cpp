#include "pages/main/main_page.h"

#include <cstring>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "pages/settings/settings_bitmap.h"
#include "pages/book/book_bitmap.h"
#include "pages/voice/voice_bitmap.h"
#include "pages/calculator/calculator_bitmap.h"
#include "pages/clock/clock_bitmap.h"
#include "pages/music/music_bitmap.h"
#include "pages/poem/poem_bitmap.h"
#include "pages/learn/learn_bitmap.h"
#include "pages/topbar/topbar_assets.h"
#include "pages/topbar/topbar_bitmap.h"

namespace {

void pixel(uint8_t *frame, int x, int y) {
    if (x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |= 0x80U >> (x % 8);
}

void hline(uint8_t *frame, int x, int y, int width) {
    for (int i = 0; i < width; ++i) pixel(frame, x + i, y);
}

void vline(uint8_t *frame, int x, int y, int height) {
    for (int i = 0; i < height; ++i) pixel(frame, x, y + i);
}

void roundedFrame(uint8_t *frame, int x, int y, int width, int height) {
    // A compact 4-pixel corner radius keeps the frame crisp on the monochrome
    // panel while matching the rounded app-tile treatment in the reference.
    constexpr int radius = 4;
    hline(frame, x + radius, y, width - radius * 2);
    hline(frame, x + radius, y + height - 1, width - radius * 2);
    vline(frame, x, y + radius, height - radius * 2);
    vline(frame, x + width - 1, y + radius, height - radius * 2);

    pixel(frame, x + 2, y + 1);
    pixel(frame, x + 1, y + 2);
    pixel(frame, x + width - 3, y + 1);
    pixel(frame, x + width - 2, y + 2);
    pixel(frame, x + 1, y + height - 3);
    pixel(frame, x + 2, y + height - 2);
    pixel(frame, x + width - 2, y + height - 3);
    pixel(frame, x + width - 3, y + height - 2);
}

void statusBar(uint8_t *frame) {
    // Keep the status area clean: no separator/bounding rule is drawn.
    Topbar::drawHome(frame, 4, 2);
    // Wi-Fi belongs immediately to the left of the battery indicator.
    Topbar::drawWifi(frame, 181, 2);
    Topbar::drawBattery(frame, 209, 2);
}

void icon(uint8_t *frame, int x, int y, const uint8_t *bitmap) {
    constexpr int framePadding = 4;
    constexpr int frameSize = 48 + framePadding * 2;
    roundedFrame(frame, x - framePadding, y - framePadding, frameSize, frameSize);

    // DATA is the exact 48x48 MSB-first raster generated from the page SVG.
    for (int row = 0; row < 48; ++row) {
        for (int bit = 0; bit < 8; ++bit) {
            const uint8_t packed = bitmap[row * 6 + bit / 8];
            if ((packed & (0x80U >> (bit % 8))) != 0) {
                pixel(frame, x + bit, y + row);
            }
        }
        for (int byte = 1; byte < 6; ++byte) {
            const uint8_t packed = bitmap[row * 6 + byte];
            for (int bit = 0; bit < 8; ++bit) {
                if ((packed & (0x80U >> bit)) != 0) pixel(frame, x + byte * 8 + bit, y + row);
            }
        }
    }
}

}

namespace MainPage {

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    statusBar(frame);
    constexpr int iconSize = 48;
    constexpr int columnGap = 32;
    constexpr int rowGap = 22;
    constexpr int startX = (XingtaiEpd::WIDTH - iconSize * 3 - columnGap * 2) / 2;
    // Start immediately below the 28-pixel status bar.
    // Leave a touch-safe gap below the 28-pixel top bar.
    constexpr int startY = 56;
    const int x1 = startX;
    const int x2 = x1 + iconSize + columnGap;
    const int x3 = x2 + iconSize + columnGap;
    const int y1 = startY;
    const int y2 = y1 + iconSize + rowGap;
    const int y3 = y2 + iconSize + rowGap;
    icon(frame, x1, y1, SettingsBitmap::DATA);
    icon(frame, x2, y1, CalculatorBitmap::DATA);
    icon(frame, x3, y1, ClockBitmap::DATA);
    icon(frame, x1, y2, BookBitmap::DATA);
    icon(frame, x2, y2, VoiceBitmap::DATA);
    icon(frame, x3, y2, MusicBitmap::DATA);
    icon(frame, x1, y3, PoemBitmap::DATA);
    icon(frame, x2, y3, LearnBitmap::DATA);
}

}