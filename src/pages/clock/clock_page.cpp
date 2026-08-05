#include "pages/clock/clock_page.h"

#include "devices/epd_xingtai/epd_xingtai.h"
#include "font/basic_font.h"
#include "font/xiaozhi_font.h"
#include "ui/localization.h"

#include <cmath>
#include <cstring>
#include <ctime>

namespace {

char weatherLocation[32] = "LOCATION WAITING";
char weatherCondition[32] = "WEATHER WAITING";
char weatherTemperature[12] = "--C";
char weatherHumidity[12] = "--%";
char weatherWindSpeed[20] = "-- KM/H";
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
const uint8_t GLYPH_PERCENT[7] = {0x19, 0x1A, 0x04, 0x04, 0x0B, 0x13, 0x00};

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
    if (character == '%') return GLYPH_PERCENT;
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

void centeredTextInRegion(uint8_t *frame, int left, int width, int y,
                          const char *value, int scale = 1) {
    text(frame, left + (width - textWidth(value, scale)) / 2, y, value, scale);
}

void clippedCenteredText(uint8_t *frame, int y, const char *value, size_t maxCharacters) {
    char clipped[33] = {};
    std::strncpy(clipped, value ? value : "", maxCharacters);
    centeredText(frame, y, clipped, 1);
}

bool nextUtf8Codepoint(const char *&cursor, uint32_t &codepoint) {
    const uint8_t *source = reinterpret_cast<const uint8_t *>(cursor);
    if (!source || source[0] == 0) return false;
    if (source[0] < 0x80) { codepoint = source[0]; ++cursor; return true; }
    if ((source[0] & 0xE0) == 0xC0 && (source[1] & 0xC0) == 0x80) {
        codepoint = ((source[0] & 0x1F) << 6) | (source[1] & 0x3F);
        cursor += 2;
        return true;
    }
    if ((source[0] & 0xF0) == 0xE0 && (source[1] & 0xC0) == 0x80 &&
        (source[2] & 0xC0) == 0x80) {
        codepoint = ((source[0] & 0x0F) << 12) | ((source[1] & 0x3F) << 6) |
                    (source[2] & 0x3F);
        cursor += 3;
        return true;
    }
    codepoint = '?';
    ++cursor;
    return true;
}

int utf8Advance(uint32_t codepoint, int scale = 1) {
    if (codepoint < 0x80) return 7 * scale;
    const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint);
    return (glyph ? max(1, static_cast<int>(glyph->advance)) + 1 : 18) * scale;
}

int utf8TextWidth(const char *value, int scale = 1) {
    int width = 0;
    const char *cursor = value;
    uint32_t codepoint = 0;
    while (nextUtf8Codepoint(cursor, codepoint)) width += utf8Advance(codepoint, scale);
    return width;
}

void drawChineseGlyph(uint8_t *frame, const XiaozhiFont::Glyph *glyph, int x, int y,
                      int clipRight, int clipBottom, int scale = 1) {
    if (!glyph) return;
    for (uint16_t glyphY = 0; glyphY < glyph->height; ++glyphY) {
        for (uint16_t glyphX = 0; glyphX < glyph->width; ++glyphX) {
            const uint32_t index = static_cast<uint32_t>(glyphY) * glyph->width + glyphX;
            const uint8_t packed = glyph->bitmap[index / 2U];
            const uint8_t alpha = (index & 1U) ? (packed & 0x0FU) : (packed >> 4);
            if (alpha >= 11) {
                const int outputX = x + glyphX * scale;
                const int outputY = y + glyphY * scale;
                for (int sy = 0; sy < scale && outputY + sy < clipBottom; ++sy) {
                    for (int sx = 0; sx < scale && outputX + sx < clipRight; ++sx) {
                        pixel(frame, outputX + sx, outputY + sy);
                    }
                }
            }
        }
    }
}

void utf8CenteredText(uint8_t *frame, int y, const char *value, int scale = 1) {
    int x = max(4, (XingtaiEpd::WIDTH - utf8TextWidth(value, scale)) / 2);
    const char *cursor = value;
    uint32_t codepoint = 0;
    while (nextUtf8Codepoint(cursor, codepoint) && x < XingtaiEpd::WIDTH - 4) {
        const int advance = utf8Advance(codepoint, scale);
        if (codepoint < 0x80) {
            char ascii[2] = {static_cast<char>(codepoint), '\0'};
            text(frame, x, y + 9 * scale, ascii, scale);
        } else {
            const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint);
            if (glyph) {
                constexpr int lineHeight = 25;
                constexpr int baseline = 6;
                const int glyphTop = y + (lineHeight - baseline - glyph->height - glyph->offsetY) * scale;
                drawChineseGlyph(frame, glyph, x + glyph->offsetX * scale, glyphTop,
                                 XingtaiEpd::WIDTH - 4, y + lineHeight * scale, scale);
            }
        }
        x += advance;
    }
}

void utf8CenteredTextInRegion(uint8_t *frame, int left, int width, int y,
                              const char *value, int scale = 1) {
    int x = max(left + 2, left + (width - utf8TextWidth(value, scale)) / 2);
    const char *cursor = value;
    uint32_t codepoint = 0;
    while (nextUtf8Codepoint(cursor, codepoint) && x < left + width - 2) {
        const int advance = utf8Advance(codepoint, scale);
        if (codepoint < 0x80) {
            char ascii[2] = {static_cast<char>(codepoint), '\0'};
            text(frame, x, y + 9 * scale, ascii, scale);
        } else {
            const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint);
            if (glyph) {
                constexpr int lineHeight = 25;
                constexpr int baseline = 6;
                const int glyphTop = y + (lineHeight - baseline - glyph->height - glyph->offsetY) * scale;
                drawChineseGlyph(frame, glyph, x + glyph->offsetX * scale, glyphTop,
                                 left + width - 2, y + lineHeight * scale, scale);
            }
        }
        x += advance;
    }
}

void drawUtf8At(uint8_t *frame, int x, int y, const char *value, int scale = 1) {
    const char *cursor = value;
    uint32_t codepoint = 0;
    while (nextUtf8Codepoint(cursor, codepoint) && x < XingtaiEpd::WIDTH - 4) {
        const int advance = utf8Advance(codepoint, scale);
        if (codepoint < 0x80) {
            char ascii[2] = {static_cast<char>(codepoint), '\0'};
            text(frame, x, y + 9 * scale, ascii, scale);
        } else {
            const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint);
            if (glyph) {
                constexpr int lineHeight = 25;
                constexpr int baseline = 6;
                const int glyphTop = y + (lineHeight - baseline - glyph->height - glyph->offsetY) * scale;
                drawChineseGlyph(frame, glyph, x + glyph->offsetX * scale, glyphTop,
                                 XingtaiEpd::WIDTH - 4, y + lineHeight * scale, scale);
            }
        }
        x += advance;
    }
}

void mixedChineseDateAndWeekday(uint8_t *frame, int y, const tm &timeInfo,
                                const char *weekday) {
    char year[5] = {};
    char month[3] = {};
    char day[3] = {};
    snprintf(year, sizeof(year), "%d", timeInfo.tm_year + 1900);
    snprintf(month, sizeof(month), "%d", timeInfo.tm_mon + 1);
    snprintf(day, sizeof(day), "%d", timeInfo.tm_mday);
    const int totalWidth = textWidth(year, 2) + utf8TextWidth("年") +
                           textWidth(month, 2) + utf8TextWidth("月") +
                           textWidth(day, 2) + utf8TextWidth("日，") +
                           utf8TextWidth(weekday) + 12;
    int x = (XingtaiEpd::WIDTH - totalWidth) / 2;
    text(frame, x, y + 7, year, 2); x += textWidth(year, 2) + 2;
    drawUtf8At(frame, x, y, "年"); x += utf8TextWidth("年") + 2;
    text(frame, x, y + 7, month, 2); x += textWidth(month, 2) + 2;
    drawUtf8At(frame, x, y, "月"); x += utf8TextWidth("月") + 2;
    text(frame, x, y + 7, day, 2); x += textWidth(day, 2) + 2;
    drawUtf8At(frame, x, y, "日，"); x += utf8TextWidth("日，") + 2;
    drawUtf8At(frame, x, y, weekday);
}

void mixedChineseTemperature(uint8_t *frame, int y, const char *english) {
    String value = english ? String(english) : String();
    value.replace("+", "");
    value.replace("C", "");
    value.trim();
    if (value.isEmpty()) value = "--";
    const int numberWidth = textWidth(value.c_str(), 3);
    const int unitWidth = utf8TextWidth("度");
    int x = (XingtaiEpd::WIDTH - numberWidth - unitWidth - 3) / 2;
    text(frame, x, y + 2, value.c_str(), 3);
    drawUtf8At(frame, x + numberWidth + 3, y, "度");
}

void chineseWeatherLocation(const char *english, char *output, size_t outputSize) {
    const String location = english ? String(english) : String();
    if (location.indexOf("SHENZHEN") >= 0) std::strncpy(output, "中国 广东 深圳", outputSize - 1);
    else if (location.indexOf("GUANGZHOU") >= 0) std::strncpy(output, "中国 广东 广州", outputSize - 1);
    else if (location.indexOf("BEIJING") >= 0) std::strncpy(output, "中国 北京", outputSize - 1);
    else if (location.indexOf("SHANGHAI") >= 0) std::strncpy(output, "中国 上海", outputSize - 1);
    else std::strncpy(output, "当前位置", outputSize - 1);
    output[outputSize - 1] = '\0';
}

void chineseWeatherCondition(const char *english, char *output, size_t outputSize) {
    String condition = english ? String(english) : String();
    condition.toUpperCase();
    const char *translated = "天气信息";
    if (condition.indexOf("THUNDER") >= 0) translated = "雷雨";
    else if (condition.indexOf("HEAVY RAIN") >= 0) translated = "大雨";
    else if (condition.indexOf("MODERATE RAIN") >= 0) translated = "中雨";
    else if (condition.indexOf("PATCHY RAIN") >= 0) translated = "局部有雨";
    else if (condition.indexOf("LIGHT RAIN") >= 0 || condition.indexOf("DRIZZLE") >= 0) translated = "小雨";
    else if (condition.indexOf("SNOW") >= 0 || condition.indexOf("SLEET") >= 0) translated = "有雪";
    else if (condition.indexOf("FOG") >= 0 || condition.indexOf("MIST") >= 0) translated = "有雾";
    else if (condition.indexOf("PARTLY CLOUDY") >= 0) translated = "多云";
    else if (condition.indexOf("OVERCAST") >= 0 || condition.indexOf("CLOUDY") >= 0) translated = "阴天";
    else if (condition.indexOf("SUNNY") >= 0 || condition.indexOf("CLEAR") >= 0) translated = "晴天";
    std::strncpy(output, translated, outputSize - 1);
    output[outputSize - 1] = '\0';
}

void analogClock(uint8_t *frame, const tm &timeInfo) {
    constexpr float PI_F = 3.14159265358979323846f;
    constexpr int centerX = XingtaiEpd::WIDTH / 2;
    constexpr int centerY = 126;
    constexpr int radius = 68;
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
         centerX + static_cast<int>(std::lround(35 * std::cos(hourAngle))),
         centerY + static_cast<int>(std::lround(35 * std::sin(hourAngle))), 5);
    line(frame, centerX, centerY,
         centerX + static_cast<int>(std::lround(51 * std::cos(minuteAngle))),
         centerY + static_cast<int>(std::lround(51 * std::sin(minuteAngle))), 3);
    fillRect(frame, centerX - 3, centerY - 3, 7, 7);
}

}

namespace ClockPage {

void setWeather(const char *location, const char *condition, const char *temperature,
                const char *humidity, const char *windSpeed, bool available) {
    portENTER_CRITICAL(&weatherDataMux);
    std::strncpy(weatherLocation, location ? location : "LOCATION UNKNOWN", sizeof(weatherLocation) - 1);
    std::strncpy(weatherCondition, condition ? condition : "WEATHER UNAVAILABLE", sizeof(weatherCondition) - 1);
    std::strncpy(weatherTemperature, temperature ? temperature : "--C", sizeof(weatherTemperature) - 1);
    std::strncpy(weatherHumidity, humidity ? humidity : "--%", sizeof(weatherHumidity) - 1);
    std::strncpy(weatherWindSpeed, windSpeed ? windSpeed : "-- KM/H", sizeof(weatherWindSpeed) - 1);
    weatherLocation[sizeof(weatherLocation) - 1] = '\0';
    weatherCondition[sizeof(weatherCondition) - 1] = '\0';
    weatherTemperature[sizeof(weatherTemperature) - 1] = '\0';
    weatherHumidity[sizeof(weatherHumidity) - 1] = '\0';
    weatherWindSpeed[sizeof(weatherWindSpeed) - 1] = '\0';
    weatherAvailable = available;
    portEXIT_CRITICAL(&weatherDataMux);
}

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);

    char location[sizeof(weatherLocation)] = {};
    char condition[sizeof(weatherCondition)] = {};
    char temperature[sizeof(weatherTemperature)] = {};
    char humidity[sizeof(weatherHumidity)] = {};
    char windSpeed[sizeof(weatherWindSpeed)] = {};
    bool available = false;
    portENTER_CRITICAL(&weatherDataMux);
    std::memcpy(location, weatherLocation, sizeof(location));
    std::memcpy(condition, weatherCondition, sizeof(condition));
    std::memcpy(temperature, weatherTemperature, sizeof(temperature));
    std::memcpy(humidity, weatherHumidity, sizeof(humidity));
    std::memcpy(windSpeed, weatherWindSpeed, sizeof(windSpeed));
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
    char dateText[24] = {};
    char weekdayText[16] = {};
    if (timeAvailable) {
        std::strftime(timeText, sizeof(timeText), "%H:%M", &timeInfo);
        if (UiLocalization::isChinese()) {
            snprintf(dateText, sizeof(dateText), "%d年%d月%d日",
                     timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday);
        } else {
            std::strftime(dateText, sizeof(dateText), "%Y-%m-%d", &timeInfo);
        }
        if (UiLocalization::isChinese()) {
            static const char *weekdays[] = {
                "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
            };
            std::strncpy(weekdayText, weekdays[timeInfo.tm_wday], sizeof(weekdayText) - 1);
        } else {
            static const char *weekdays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
            std::strncpy(weekdayText, weekdays[timeInfo.tm_wday], sizeof(weekdayText) - 1);
        }
    } else {
        std::strcpy(timeText, "--:--");
        std::strcpy(dateText, "TIME SYNC");
    }
    centeredText(frame, 204, timeText, 3);
    if (timeAvailable && UiLocalization::isChinese()) {
        mixedChineseDateAndWeekday(frame, 228, timeInfo, weekdayText);
    } else if (timeAvailable) {
        char dateAndWeekday[32] = {};
        snprintf(dateAndWeekday, sizeof(dateAndWeekday), "%s %s", dateText, weekdayText);
        centeredText(frame, 236, dateAndWeekday, 2);
    } else if (!UiLocalization::isChinese()) {
        centeredText(frame, 242, dateText, 2);
    } else {
        UiLocalization::drawCentered(frame, 254, "时间同步", 1);
    }

    line(frame, 18, 270, 221, 270);
    if (UiLocalization::isChinese()) {
        char chineseLocation[48] = {};
        char chineseCondition[32] = {};
        chineseWeatherLocation(location, chineseLocation, sizeof(chineseLocation));
        chineseWeatherCondition(condition, chineseCondition, sizeof(chineseCondition));
        utf8CenteredText(frame, 271, chineseLocation);
        utf8CenteredText(frame, 296, chineseCondition);
        mixedChineseTemperature(frame, 320, temperature);
        line(frame, 120, 357, 120, 412);
        utf8CenteredTextInRegion(frame, 8, 108, 354, "湿度");
        centeredTextInRegion(frame, 8, 108, 382, humidity, 2);
        utf8CenteredTextInRegion(frame, 124, 108, 354, "风速");
        centeredTextInRegion(frame, 124, 108, 382, windSpeed, 2);
    } else {
        clippedCenteredText(frame, 278, location, 30);
        clippedCenteredText(frame, 302, condition, 30);
        centeredText(frame, 326, temperature, 3);
        line(frame, 120, 357, 120, 412);
        centeredTextInRegion(frame, 8, 108, 363, "HUMIDITY");
        centeredTextInRegion(frame, 8, 108, 386, humidity, 2);
        centeredTextInRegion(frame, 124, 108, 363, "WIND");
        centeredTextInRegion(frame, 124, 108, 386, windSpeed, 2);
    }
    // Leave the weather footer blank while the background request is pending.
    // Successful weather data is rendered normally once it becomes available.
}

}