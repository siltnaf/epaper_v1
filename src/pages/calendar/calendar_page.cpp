#include "pages/calendar/calendar_page.h"

#include "devices/epd_xingtai/epd_xingtai.h"

#include <Arduino.h>
#include <cstring>
#include <ctime>

namespace {

void pixel(uint8_t *frame, int x, int y) {
    if (x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |= 0x80U >> (x % 8);
}

void rect(uint8_t *frame, int x, int y, int width, int height) {
    for (int i = 0; i < width; ++i) {
        pixel(frame, x + i, y);
        pixel(frame, x + i, y + height - 1);
    }
    for (int i = 0; i < height; ++i) {
        pixel(frame, x, y + i);
        pixel(frame, x + width - 1, y + i);
    }
}

void fillRect(uint8_t *frame, int x, int y, int width, int height) {
    for (int row = 0; row < height; ++row)
        for (int column = 0; column < width; ++column) pixel(frame, x + column, y + row);
}

const uint8_t *glyph(char character) {
    static const uint8_t digits[10][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
        {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E},
        {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E},
    };
    static const uint8_t letters[26][7] = {
        {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
        {0x0F,0x10,0x10,0x10,0x10,0x10,0x0F},{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
        {0x0F,0x10,0x10,0x17,0x11,0x11,0x0F},{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
        {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},{0x01,0x01,0x01,0x01,0x11,0x11,0x0E},
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
        {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0x11,0x19,0x15,0x15,0x13,0x13,0x11},
        {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
        {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
        {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
        {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},{0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
        {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
        {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    };
    static const uint8_t dash[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
    if (character >= '0' && character <= '9') return digits[character - '0'];
    if (character >= 'A' && character <= 'Z') return letters[character - 'A'];
    if (character == '-') return dash;
    return nullptr;
}

void text(uint8_t *frame, int x, int y, const char *value, int scale = 1) {
    for (const char *cursor = value; cursor && *cursor; ++cursor) {
        const uint8_t *bitmap = glyph(*cursor);
        if (bitmap) {
            for (int row = 0; row < 7; ++row)
                for (int column = 0; column < 5; ++column)
                    if (bitmap[row] & (0x10U >> column)) fillRect(frame, x + column * scale, y + row * scale, scale, scale);
        }
        x += 6 * scale;
    }
}

int daysInMonth(int year, int month) {
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return days[month - 1];
}

}

namespace CalendarPage {
void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    time_t now = time(nullptr);
    struct tm current = {};
    localtime_r(&now, &current);
    int year = current.tm_year + 1900;
    int month = current.tm_mon + 1;
    int today = current.tm_mday;
    if (year < 2020) { year = 2026; month = 1; today = 1; }

    char title[16] = {};
    snprintf(title, sizeof(title), "%04d-%02d", year, month);
    text(frame, 78, 14, title, 2);
    const char *weekdays = "SMTWTFS";
    for (int day = 0; day < 7; ++day) {
        const char label[2] = {weekdays[day], '\0'};
        text(frame, 16 + day * 30, 52, label, 1);
    }

    constexpr int left = 8;
    constexpr int top = 68;
    constexpr int cellWidth = 32;
    constexpr int cellHeight = 48;
    for (int row = 0; row < 6; ++row)
        for (int column = 0; column < 7; ++column)
            rect(frame, left + column * cellWidth, top + row * cellHeight, cellWidth, cellHeight);

    struct tm first = current;
    first.tm_year = year - 1900;
    first.tm_mon = month - 1;
    first.tm_mday = 1;
    first.tm_hour = 12;
    mktime(&first);
    const int firstWeekday = first.tm_wday;
    const int count = daysInMonth(year, month);
    for (int day = 1; day <= count; ++day) {
        const int index = firstWeekday + day - 1;
        const int row = index / 7;
        const int column = index % 7;
        const int x = left + column * cellWidth;
        const int y = top + row * cellHeight;
        if (day == today) rect(frame, x + 3, y + 3, cellWidth - 6, cellHeight - 6);
        char number[3] = {};
        snprintf(number, sizeof(number), "%d", day);
        text(frame, x + 12, y + 16, number, 2);
    }
}
}