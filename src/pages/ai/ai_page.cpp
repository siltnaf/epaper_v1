#include "pages/ai/ai_page.h"

#include <cstring>
#include <cmath>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "font/basic_font.h"
#include "ui/localization.h"

namespace {

bool loading = false;
bool aiOpen = false;
bool eyesAnimating = false;
int8_t gazeX = 0;
int8_t gazeY = 0;
uint32_t nextGazeMs = 0;
uint8_t gazeIndex = 0;
constexpr int8_t GAZE_X[] = {-10, 0, 10, 6, -6, 0};
constexpr int8_t GAZE_Y[] = {0, -5, 0, 5, 4, 0};
// The AI page runs a single 5 Hz tick: it advances the eye gaze animation and
// lets the main loop service the FT6336 touch panel on the same 200 ms cadence.
constexpr uint32_t GAZE_INTERVAL_MS = 200;

void pixel(uint8_t *frame, int x, int y) {
    if (!frame || x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |= 0x80U >> (x % 8);
}

void clearPixel(uint8_t *frame, int x, int y) {
    if (!frame || x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] &=
        static_cast<uint8_t>(~(0x80U >> (x % 8)));
}

void ellipse(uint8_t *frame, int centerX, int centerY, int radiusX, int radiusY,
             bool filled) {
    for (int y = -radiusY; y <= radiusY; ++y) {
        for (int x = -radiusX; x <= radiusX; ++x) {
            const float dx = static_cast<float>(x) / radiusX;
            const float dy = static_cast<float>(y) / radiusY;
            const float distance = dx * dx + dy * dy;
            if (filled ? distance <= 1.0f : distance >= 0.98f && distance <= 1.02f) {
                pixel(frame, centerX + x, centerY + y);
            }
        }
    }
}

void drawIdleAi(uint8_t *frame) {
    constexpr int sourceWidth = 22;
    constexpr int sourceHeight = 14;
    constexpr int targetWidth = 18;
    constexpr int targetHeight = 11;
    bool source[sourceHeight][sourceWidth] = {};
    for (int character = 0; character < 2; ++character) {
        const uint8_t *glyph = BasicFont::english(character == 0 ? 'A' : 'I');
        if (!glyph) continue;
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((glyph[row] & (0x10U >> column)) == 0) continue;
                for (int yy = 0; yy < 2; ++yy) {
                    for (int xx = 0; xx < 2; ++xx) {
                        source[row * 2 + yy][character * 12 + column * 2 + xx] = true;
                    }
                }
            }
        }
    }
    constexpr int left = 56;
    // Center the shorter AI glyph inside Home's 24-pixel top-bar slot.
    constexpr int top = 8;
    for (int row = 0; row < targetHeight; ++row) {
        for (int column = 0; column < targetWidth; ++column) {
            if (source[(row * sourceHeight) / targetHeight]
                       [(column * sourceWidth) / targetWidth]) {
                pixel(frame, left + column, top + row);
            }
        }
    }
}

void clearEyeFrame(uint8_t *frame) {
    if (!frame) return;
    constexpr size_t topBarBytes = static_cast<size_t>(32) *
                                   (XingtaiEpd::WIDTH / 8);
    // The AI page owns the complete area below the fixed top bar. Reset it
    // before every eye frame so old gaze positions cannot remain in memory.
    std::memset(frame + topBarBytes, 0x00,
                XingtaiEpd::FRAME_BYTES - topBarBytes);
}

} // namespace

namespace AiPage {

void setLoading(bool value) {
    loading = value;
}

void drawTopbarStatus(uint8_t *frame) {
    // Home ends at x=51, the fixed AI label occupies x=56..73, and the network
    // icon starts at x=181. Keep the loading label in its own band.
    constexpr int left = 52;
    constexpr int right = 181;
    for (int y = 0; y < 32; ++y) {
        for (int x = left; x < right; ++x) {
            clearPixel(frame, x, y);
        }
    }
    drawIdleAi(frame);
    if (loading) {
        const char *label = UiLocalization::isChinese() ? "下载中" : "DOWNLOADING";
        UiLocalization::drawText(frame, 84, 12, label, 1);
    }
}

bool controlAt(int16_t x, int16_t y) {
    // Include the full finger target around the visible AI glyph. Keep a gap
    // from Home's x=0..40 target while covering taps reported around x=83.
    return x >= 48 && x < 96 && y >= 0 && y < 32;
}

bool openAt(int16_t x, int16_t y) {
    if (!controlAt(x, y)) return false;
    aiOpen = true;
    // The eyes were never moving because the animation flag stayed false. Start
    // the gaze loop when the overlay opens and schedule the first move one tick
    // out so the initial render has time to settle.
    eyesAnimating = true;
    gazeIndex = 0;
    gazeX = 0;
    gazeY = 0;
    nextGazeMs = millis() + GAZE_INTERVAL_MS;
    return true;
}

void close() {
    aiOpen = false;
    eyesAnimating = false;
}

bool isOpen() { return aiOpen; }

bool animate() {
    if (!aiOpen || !eyesAnimating ||
        static_cast<int32_t>(millis() - nextGazeMs) < 0) return false;
    gazeIndex = static_cast<uint8_t>((gazeIndex + 1) % (sizeof(GAZE_X) / sizeof(GAZE_X[0])));
    gazeX = GAZE_X[gazeIndex];
    gazeY = GAZE_Y[gazeIndex];
    nextGazeMs = millis() + GAZE_INTERVAL_MS;
    return true;
}

void stopAnimation() {
    eyesAnimating = false;
}

void render(uint8_t *frame) {
    clearEyeFrame(frame);
    for (int16_t eyeX : {72, 168}) {
        // The proportions and layered iris are based on eyes.svg. The white
        // interior is cleared from the black page before drawing the outline.
        ellipse(frame, eyeX, 190, 45, 92, false);
        for (int y = -87; y <= 87; ++y) {
            for (int x = -40; x <= 40; ++x) {
                const float dx = static_cast<float>(x) / 45.0f;
                const float dy = static_cast<float>(y) / 92.0f;
                if (dx * dx + dy * dy <= 0.94f)
                    clearPixel(frame, eyeX + x, 190 + y);
            }
        }
        ellipse(frame, eyeX + gazeX, 190 + gazeY, 27, 31, true);
        ellipse(frame, eyeX + gazeX - 7, 190 + gazeY - 9, 5, 7, false);
    }
}

} // namespace AiPage