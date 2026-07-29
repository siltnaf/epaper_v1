#include <Arduino.h>

#include "board_pins.h"
#include "epd_xingtai.h"

namespace {
constexpr uint8_t WHITE = 0x00;
constexpr uint8_t BLACK = 0xFF;
constexpr uint8_t TEXT_SCALE = 4;

// 5x7 font entries for the letters used by "Asundar".
const uint8_t FONT_A[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
const uint8_t FONT_S[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
const uint8_t FONT_U[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
const uint8_t FONT_N[7] = {0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11};
const uint8_t FONT_D[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
const uint8_t FONT_R[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};

const uint8_t *glyph(char character) {
    switch (character) {
    case 'A': return FONT_A;
    case 's': return FONT_S;
    case 'u': return FONT_U;
    case 'n': return FONT_N;
    case 'd': return FONT_D;
    case 'r': return FONT_R;
    default: return nullptr;
    }
}

void setPixel(uint8_t *frame, int x, int y, bool black) {
    if (x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    const size_t index = static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8;
    const uint8_t mask = static_cast<uint8_t>(0x80U >> (x % 8));
    if (black) frame[index] |= mask;
    else frame[index] &= static_cast<uint8_t>(~mask);
}

void drawCenteredText(uint8_t *frame, const char *text) {
    const int characterWidth = 5 * TEXT_SCALE;
    const int characterGap = TEXT_SCALE;
    const int textWidth = static_cast<int>(strlen(text)) * (characterWidth + characterGap) - characterGap;
    const int textHeight = 7 * TEXT_SCALE;
    const int originX = (XingtaiEpd::WIDTH - textWidth) / 2;
    const int originY = (XingtaiEpd::HEIGHT - textHeight) / 2;

    int cursorX = originX;
    for (const char *character = text; *character != '\0'; ++character) {
        const uint8_t *bitmap = glyph(*character);
        if (bitmap == nullptr) {
            cursorX += characterWidth + characterGap;
            continue;
        }
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((bitmap[row] & (0x10U >> column)) == 0) continue;
                for (int dy = 0; dy < TEXT_SCALE; ++dy) {
                    for (int dx = 0; dx < TEXT_SCALE; ++dx) {
                        setPixel(frame, cursorX + column * TEXT_SCALE + dx,
                                 originY + row * TEXT_SCALE + dy, true);
                    }
                }
            }
        }
        cursorX += characterWidth + characterGap;
    }
}
}

XingtaiEpd epaper;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("ESP32-S3 e-paper portrait test");
    Serial.printf("EPD DIN=%d CLK=%d CS=%d DC=%d RST=%d BUSY=%d\n",
                  BoardPins::EP_DIN, BoardPins::EP_CLK, BoardPins::EP_CS,
                  BoardPins::EP_DC, BoardPins::EP_RST, BoardPins::EP_BUSY);

    static uint8_t frame[XingtaiEpd::FRAME_BYTES];
    memset(frame, WHITE, sizeof(frame));
    drawCenteredText(frame, "Asundar");

    Serial.println("Initializing e-paper...");
    epaper.begin();
    Serial.println("Writing centered Asundar...");
    epaper.display(frame);
    epaper.sleep();
    Serial.println("E-paper refresh complete");
}

void loop() {
    delay(1000);
}
