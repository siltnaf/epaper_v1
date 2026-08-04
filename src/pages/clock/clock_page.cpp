#include "pages/clock/clock_page.h"

#include "devices/epd_xingtai/epd_xingtai.h"
#include "font/basic_font.h"
#include "ui/localization.h"

#include <cmath>
#include <cstring>
#include <ctime>

namespace {

char weatherLocation[32] = "LOCATION WAITING";
char weatherCondition[32] = "WEATHER WAITING";
char weatherTemperature[12] = "--C";
bool weatherAvailable = false;
portMUX_TYPE weatherDataMux = portMUX_INITIALIZER_UNLOCKED;

const uint8_t DIGITS[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E},
    {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E},
};

const uint8_t GLYPH_COLON[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
const uint8_t GLYPH_MINUS[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
const uint8_t GLYPH_PLUS[7] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
const uint8_t GLYPH_SLASH[7] = {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};

void pixel(uint8_t *frame, int x, int y) {
    if (x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |= 0x80U >> (x % 8);
}

void fillRect(uint8_t *frame, int x, int y, int width, int height) {
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) pixel(frame, x + column, y + row);
    }
}

void line(uint8_t *frame, int x0, int y0, int x1, int y1, int thickness = 1) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        const int radius = thickness / 2;
        fillRect(frame, x0 - radius, y0 - radius, thickness, thickness);
        if (x0 == x1 && y0 == y1) break;
        const int twiceError = error * 2;
        if (twiceError >= dy) { error += dy; x0 += sx; }
        if (twiceError <= dx) { error += dx; y0 += sy; }
    }
}

void circle(uint8_t *frame, int centerX, int centerY, int radius) {
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y) {
        pixel(frame, centerX + x, centerY + y); pixel(frame, centerX + y, centerY + x);
        pixel(frame, centerX - y, centerY + x); pixel(frame, centerX - x, centerY + y);
        pixel(frame, centerX - x, centerY - y); pixel(frame, centerX - y, centerY - x);
        pixel(frame, centerX + y, centerY - x); pixel(frame, centerX + x, centerY - y);
        ++y;
        if (error < 0) error += 2 * y + 1;
        else { --x; error += 2 * (y - x) + 1; }
    }
}

const uint8_t *glyph(char character) {
    if (character >= 'A' && character <= 'Z') return BasicFont::english(character);
    if (character >= '0' && character <= '9') return DIGITS[character - '0'];
    if (character == ':') return GLYPH_COLON;
    if (character == '-') return GLYPH_MINUS;
    if (character == '+') return GLYPH_PLUS;
    if (character == '/') return GLYPH_SLASH;
    return nullptr;
}

int textWidth(const char *value, int scale) {
    return value && *value ? static_cast<int>(std::strlen(value)) * 6 * scale - scale : 0;
}

void text(uint8_t *frame, int x, int y, const char *value, int scale = 1) {
    for (const char *cursor = value; cursor && *cursor; ++cursor) {
        const uint8_t *bitmap = glyph(*cursor);
        if (bitmap) {
            for (int row = 0; row < 7; ++row) {
                for (int column = 0; column < 5; ++column) {
                    if (bitmap[row] & (0x10U >> column)) {
                        fillRect(frame, x + column * scale, y + row * scale, scale, scale);
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

void centeredText(uint8_t *frame, int y, const char *value, int scale = 1) {
    text(frame, (XingtaiEpd::WIDTH - textWidth(value, scale)) / 2, y, value, scale);
}

void clippedCenteredText(uint8_t *frame, int y, const char *value, size_t maxCharacters) {
    char clipped[33] = {};
    std::strncpy(clipped, value ? value : "", maxCharacters);
    centeredText(frame, y, clipped, 1);
}

void analogClock(uint8_t *frame, const tm &timeInfo) {
    constexpr float PI_F = 3.14159265358979323846f;
    constexpr int centerX = XingtaiEpd::WIDTH / 2;
    constexpr int centerY = 150;
    constexpr int radius = 82;
    circle(frame, centerX, centerY, radius);
    circle(frame, centerX, centerY, radius - 1);

    for (int index = 0; index < 12; ++index) {
        const float angle = (index * 30.0f - 90.0f) * PI_F / 180.0f;
        const int inner = index % 3 == 0 ? radius - 14 : radius - 9;
        line(frame,
             centerX + static_cast<int>(std::lround(inner * std::cos(angle))),
             centerY + static_cast<int>(std::lround(inner * std::sin(angle))),
             centerX + static_cast<int>(std::lround((radius - 4) * std::cos(angle))),
             centerY + static_cast<int>(std::lround((radius - 4) * std::sin(angle))),
             index % 3 == 0 ? 3 : 1);
    }

    const float hourAngle = ((timeInfo.tm_hour % 12) * 30.0f + timeInfo.tm_min * 0.5f - 90.0f) * PI_F / 180.0f;
    const float minuteAngle = (timeInfo.tm_min * 6.0f - 90.0f) * PI_F / 180.0f;
    line(frame, centerX, centerY,
         centerX + static_cast<int>(std::lround(42 * std::cos(hourAngle))),
         centerY + static_cast<int>(std::lround(42 * std::sin(hourAngle))), 5);
    line(frame, centerX, centerY,
         centerX + static_cast<int>(std::lround(61 * std::cos(minuteAngle))),
         centerY + static_cast<int>(std::lround(61 * std::sin(minuteAngle))), 3);
    fillRect(frame, centerX - 3, centerY - 3, 7, 7);
}

}

namespace ClockPage {

void setWeather(const char *location, const char *condition, const char *temperature,
                bool available) {
    portENTER_CRITICAL(&weatherDataMux);
    std::strncpy(weatherLocation, location ? location : "LOCATION UNKNOWN", sizeof(weatherLocation) - 1);
    std::strncpy(weatherCondition, condition ? condition : "WEATHER UNAVAILABLE", sizeof(weatherCondition) - 1);
    std::strncpy(weatherTemperature, temperature ? temperature : "--C", sizeof(weatherTemperature) - 1);
    weatherLocation[sizeof(weatherLocation) - 1] = '\0';
    weatherCondition[sizeof(weatherCondition) - 1] = '\0';
    weatherTemperature[sizeof(weatherTemperature) - 1] = '\0';
    weatherAvailable = available;
    portEXIT_CRITICAL(&weatherDataMux);
}

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);

    char location[sizeof(weatherLocation)] = {};
    char condition[sizeof(weatherCondition)] = {};
    char temperature[sizeof(weatherTemperature)] = {};
    bool available = false;
    portENTER_CRITICAL(&weatherDataMux);
    std::memcpy(location, weatherLocation, sizeof(location));
    std::memcpy(condition, weatherCondition, sizeof(condition));
    std::memcpy(temperature, weatherTemperature, sizeof(temperature));
    available = weatherAvailable;
    portEXIT_CRITICAL(&weatherDataMux);

    time_t now = time(nullptr);
    tm timeInfo = {};
    localtime_r(&now, &timeInfo);
    const bool timeAvailable = timeInfo.tm_year + 1900 >= 2024;
    if (!timeAvailable) {
        timeInfo.tm_hour = 10;
        timeInfo.tm_min = 10;
    }

    analogClock(frame, timeInfo);

    char timeText[6] = {};
    char dateText[11] = {};
    if (timeAvailable) {
        std::strftime(timeText, sizeof(timeText), "%H:%M", &timeInfo);
        std::strftime(dateText, sizeof(dateText), "%Y-%m-%d", &timeInfo);
    } else {
        std::strcpy(timeText, "--:--");
        std::strcpy(dateText, "TIME SYNC");
    }
    centeredText(frame, 244, timeText, 3);
    if (timeAvailable || !UiLocalization::isChinese()) centeredText(frame, 272, dateText, 1);
    else UiLocalization::drawCentered(frame, 272, "时间同步", 1);

    line(frame, 18, 294, 221, 294);
    clippedCenteredText(frame, 310, location, 30);
    clippedCenteredText(frame, 334, condition, 30);
    centeredText(frame, 358, temperature, 2);
    if (!available) {
        if (UiLocalization::isChinese()) UiLocalization::drawCentered(frame, 386, "正在获取天气", 1);
        else centeredText(frame, 386, "FETCHING WEATHER", 1);
    }
}

}