#include "pages/asundar/asundar_page.h"

#include <cstring>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "font/firasans_asundar.h"
#include "pages/asundar/logo_bitmap.h"

namespace {

void setPixel(uint8_t *frame, int x, int y) {
    if (x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    const size_t index = static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8;
    frame[index] |= static_cast<uint8_t>(0x80U >> (x % 8));
}

void drawLogo(uint8_t *frame, int centerX, int topY) {
    const int leftX = centerX - AsundarLogo::WIDTH / 2;
    for (int row = 0; row < AsundarLogo::HEIGHT; ++row) {
        for (int column = 0; column < AsundarLogo::WIDTH; ++column) {
            const uint8_t packed = AsundarLogo::BITMAP[row * AsundarLogo::ROW_BYTES + column / 8];
            if ((packed & (0x80U >> (column % 8))) != 0) setPixel(frame, leftX + column, topY + row);
        }
    }
}

int scaleHalf(int value) {
    return (value >= 0) ? (value + 1) / 2 : value / 2;
}

void drawCenteredText(uint8_t *frame, const char *text, int topY) {
    constexpr int fontHeight = 25;
    constexpr int ascender = 20;
    int textWidth = 0;
    for (const char *character = text; *character != '\0'; ++character) {
        const FiraSansAsundar::Glyph *glyph = FiraSansAsundar::glyph(*character);
        if (glyph != nullptr) textWidth += scaleHalf(glyph->advanceX);
    }

    int cursorX = (XingtaiEpd::WIDTH - textWidth) / 2;
    const int baselineY = topY + ascender;

    for (const char *character = text; *character != '\0'; ++character) {
        const FiraSansAsundar::Glyph *glyph = FiraSansAsundar::glyph(*character);
        if (glyph == nullptr) continue;

        const int rowBytes = (glyph->width + 1) / 2;
        for (int row = 0; row < glyph->height; ++row) {
            const int y0 = baselineY + scaleHalf(row - glyph->top);
            const int y1 = baselineY + scaleHalf(row + 1 - glyph->top);
            for (int column = 0; column < glyph->width; ++column) {
                const uint8_t packed = glyph->bitmap[row * rowBytes + column / 2];
                const uint8_t alpha = (column % 2 == 0) ? (packed & 0x0F) : (packed >> 4);
                if (alpha < 8) continue;

                const int x0 = cursorX + scaleHalf(glyph->left + column);
                const int x1 = cursorX + scaleHalf(glyph->left + column + 1);
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) setPixel(frame, x, y);
                }
            }
        }
        cursorX += scaleHalf(glyph->advanceX);
    }
}

void drawStartupArtwork(uint8_t *frame) {
    constexpr int logoHeight = AsundarLogo::HEIGHT;
    constexpr int gap = 10;
    constexpr int textHeight = 25;
    constexpr int artworkHeight = logoHeight + gap + textHeight;
    const int artworkTop = (XingtaiEpd::HEIGHT - artworkHeight) / 2;
    drawLogo(frame, XingtaiEpd::WIDTH / 2, artworkTop);

    drawCenteredText(frame, "Asundar", artworkTop + logoHeight + gap);
}

}

namespace AsundarPage {

void render(uint8_t *frame) {
    std::memset(frame, WHITE, XingtaiEpd::FRAME_BYTES);
    drawStartupArtwork(frame);
}

}