#include "pages/main/main_page.h"

#include <cstring>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "pages/settings/settings_bitmap.h"
#include "pages/calendar/calendar_bitmap.h"
#include "pages/recording/recording_bitmap.h"
#include "pages/book/book_bitmap.h"
#include "pages/voice/voice_bitmap.h"
#include "pages/calculator/calculator_bitmap.h"
#include "pages/clock/clock_bitmap.h"
#include "pages/music/music_bitmap.h"
#include "pages/poem/poem_bitmap.h"
#include "pages/word/word_bitmap.h"
#include "pages/topbar/topbar_assets.h"
#include "pages/topbar/topbar_bitmap.h"

namespace {

MainPage::NetworkMode networkMode = MainPage::NetworkMode::None;

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
    // A six-pixel radius gives the monochrome waveform enough connected pixels
    // around each bend. The previous sparse four-pixel corner could appear
    // broken after partial refreshes.
    constexpr int radius = 6;
    hline(frame, x + radius, y, width - radius * 2);
    hline(frame, x + radius, y + height - 1, width - radius * 2);
    vline(frame, x, y + radius, height - radius * 2);
    vline(frame, x + width - 1, y + radius, height - radius * 2);

    constexpr uint8_t cornerX[] = {5, 4, 3, 2, 1, 1, 0};
    constexpr uint8_t cornerY[] = {0, 1, 1, 2, 3, 4, 5};
    for (size_t index = 0; index < sizeof(cornerX); ++index) {
        const int dx = cornerX[index];
        const int dy = cornerY[index];
        pixel(frame, x + dx, y + dy);
        pixel(frame, x + width - 1 - dx, y + dy);
        pixel(frame, x + dx, y + height - 1 - dy);
        pixel(frame, x + width - 1 - dx, y + height - 1 - dy);
    }
}

void roundedFrame(uint8_t *frame, int x, int y, int width, int height, bool bold) {
    roundedFrame(frame, x, y, width, height);
    if (!bold) return;

    // Multiple connected rounded outlines create the pressed state. Each inset
    // uses the same continuous corner path, avoiding the square appearance and
    // the sparse corner gaps seen with the earlier outline.
    for (int inset = 0; inset <= 2; ++inset) {
        roundedFrame(frame, x + inset, y + inset,
                     width - inset * 2, height - inset * 2);
    }
}

void statusBar(uint8_t *frame) {
    // Keep the status area clean: no separator/bounding rule is drawn.
    Topbar::drawHome(frame, 4, 2);
    if (networkMode == MainPage::NetworkMode::Wifi) {
        Topbar::drawWifi(frame, 181, 2);
    } else if (networkMode == MainPage::NetworkMode::Cellular4G) {
        Topbar::draw4G(frame, 184, 2);
    }
    Topbar::drawBattery(frame, 209, 2);
}

void icon(uint8_t *frame, int x, int y, const uint8_t *bitmap, bool selected) {
    constexpr int framePadding = 4;
    constexpr int frameSize = 48 + framePadding * 2;
    roundedFrame(frame, x - framePadding, y - framePadding, frameSize, frameSize, selected);

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

void setNetworkMode(NetworkMode mode) {
    networkMode = mode;
}

void render(uint8_t *frame) {
    render(frame, FunctionIcon::None);
}

void render(uint8_t *frame, FunctionIcon selectedIcon) {
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
    icon(frame, x1, y1, SettingsBitmap::DATA, selectedIcon == FunctionIcon::Settings);
    icon(frame, x2, y1, CalendarBitmap::DATA, selectedIcon == FunctionIcon::Calendar);
    icon(frame, x3, y1, CalculatorBitmap::DATA, selectedIcon == FunctionIcon::Calculator);
    icon(frame, x1, y2, ClockBitmap::DATA, selectedIcon == FunctionIcon::Clock);
    icon(frame, x2, y2, BookBitmap::DATA, selectedIcon == FunctionIcon::Book);
    icon(frame, x3, y2, VoiceBitmap::DATA, selectedIcon == FunctionIcon::Voice);
    icon(frame, x1, y3, MusicBitmap::DATA, selectedIcon == FunctionIcon::Music);
    icon(frame, x2, y3, PoemBitmap::DATA, selectedIcon == FunctionIcon::Poem);
    icon(frame, x3, y3, WordBitmap::DATA, selectedIcon == FunctionIcon::Word);
    icon(frame, x1, y3 + iconSize + rowGap, RecordingBitmap::DATA,
         selectedIcon == FunctionIcon::Recording);
}

}