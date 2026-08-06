#include "pages/calendar/calendar_page.h"

#include "devices/epd_xingtai/epd_xingtai.h"
#include "ui/localization.h"

#include <Arduino.h>
#include <cstring>
#include <ctime>

namespace {

int viewedYear = 0;
int viewedMonth = 0;
int currentYear = 0;
int currentMonth = 0;
int currentDay = 0;

void pixel(uint8_t *frame, int x, int y) {
    if (x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |= 0x80U >> (x % 8);
}

void clearPixel(uint8_t *frame, int x, int y) {
    if (x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] &=
        static_cast<uint8_t>(~(0x80U >> (x % 8)));
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

void dateText(uint8_t *frame, int x, int y, const char *value) {
    for (const char *cursor = value; cursor && *cursor; ++cursor) {
        const uint8_t *bitmap = glyph(*cursor);
        if (bitmap) {
            for (int row = 0; row < 7; ++row) {
                const int top = y + row * 3 / 2;
                const int bottom = y + (row + 1) * 3 / 2;
                for (int column = 0; column < 5; ++column) {
                    if ((bitmap[row] & (0x10U >> column)) == 0) continue;
                    const int left = x + column * 3 / 2;
                    const int right = x + (column + 1) * 3 / 2;
                    fillRect(frame, left, top, right - left, bottom - top);
                }
            }
        }
        x += 9;
    }
}

void invertedDateText(uint8_t *frame, int x, int y, const char *value) {
    for (const char *cursor = value; cursor && *cursor; ++cursor) {
        const uint8_t *bitmap = glyph(*cursor);
        if (bitmap) {
            for (int row = 0; row < 7; ++row) {
                const int top = y + row * 3 / 2;
                const int bottom = y + (row + 1) * 3 / 2;
                for (int column = 0; column < 5; ++column) {
                    if ((bitmap[row] & (0x10U >> column)) == 0) continue;
                    const int left = x + column * 3 / 2;
                    const int right = x + (column + 1) * 3 / 2;
                    for (int py = top; py < bottom; ++py)
                        for (int px = left; px < right; ++px) clearPixel(frame, px, py);
                }
            }
        }
        x += 9;
    }
}

int dateTextWidth(const char *value) {
    return value && *value ? static_cast<int>(std::strlen(value)) * 9 - 1 : 0;
}

int daysInMonth(int year, int month) {
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return days[month - 1];
}

int textWidth(const char *value, int scale) {
    return value && *value ? static_cast<int>(std::strlen(value)) * 6 * scale - scale : 0;
}

void chevron(uint8_t *frame, int centerX, int centerY, bool right) {
    for (int offset = -4; offset <= 4; ++offset) {
        const int x = right ? centerX - abs(offset) / 2 : centerX + abs(offset) / 2;
        pixel(frame, x, centerY + offset);
    }
}

void navigationButton(uint8_t *frame, int x, bool right, bool year) {
    rect(frame, x, 36, 28, 28);
    chevron(frame, x + (year ? 10 : 14), 49, right);
    if (year) chevron(frame, x + 17, 49, right);
}

void loadCurrentDate() {
    time_t now = time(nullptr);
    struct tm current = {};
    localtime_r(&now, &current);
    currentYear = current.tm_year + 1900;
    currentMonth = current.tm_mon + 1;
    currentDay = current.tm_mday;
    if (currentYear < 2020) {
        currentYear = 2026;
        currentMonth = 1;
        currentDay = 1;
    }
    if (viewedYear == 0) {
        viewedYear = currentYear;
        viewedMonth = currentMonth;
    }
}

}

namespace CalendarPage {
void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    loadCurrentDate();

    navigationButton(frame, 8, false, true);
    navigationButton(frame, 40, false, false);
    navigationButton(frame, 172, true, false);
    navigationButton(frame, 204, true, true);

    char title[16] = {};
    snprintf(title, sizeof(title), "%04d-%02d", viewedYear, viewedMonth);
    text(frame, (XingtaiEpd::WIDTH - textWidth(title, 2)) / 2, 43, title, 2);
    const char *weekdays = "SMTWTFS";
    const char *chineseWeekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
    for (int day = 0; day < 7; ++day) {
        if (UiLocalization::isChinese()) UiLocalization::drawText(frame, 16 + day * 32, 68, chineseWeekdays[day], 1);
        else {
            const char label[2] = {weekdays[day], '\0'};
            text(frame, 21 + day * 32, 72, label, 1);
        }
    }

    constexpr int left = 8;
    constexpr int top = 86;
    constexpr int cellWidth = 32;
    constexpr int cellHeight = 52;
    for (int row = 0; row < 6; ++row)
        for (int column = 0; column < 7; ++column)
            rect(frame, left + column * cellWidth, top + row * cellHeight, cellWidth, cellHeight);

    struct tm first = {};
    first.tm_year = viewedYear - 1900;
    first.tm_mon = viewedMonth - 1;
    first.tm_mday = 1;
    first.tm_hour = 12;
    mktime(&first);
    const int firstWeekday = first.tm_wday;
    const int count = daysInMonth(viewedYear, viewedMonth);
    for (int day = 1; day <= count; ++day) {
        const int index = firstWeekday + day - 1;
        const int row = index / 7;
        const int column = index % 7;
        const int x = left + column * cellWidth;
        const int y = top + row * cellHeight;
        const bool today = viewedYear == currentYear && viewedMonth == currentMonth &&
                           day == currentDay;
        if (today) fillRect(frame, x + 2, y + 2, cellWidth - 3, cellHeight - 3);
        char number[3] = {};
        snprintf(number, sizeof(number), "%d", day);
        const int numberX = x + (cellWidth - dateTextWidth(number)) / 2;
        const int numberY = y + (cellHeight - 10) / 2;
        if (today) invertedDateText(frame, numberX, numberY, number);
        else dateText(frame, numberX, numberY, number);
    }
}

Action actionAt(int16_t x, int16_t y) {
    if (y < 36 || y >= 64) return Action::None;
    if (x >= 8 && x < 36) return Action::PreviousYear;
    if (x >= 40 && x < 68) return Action::PreviousMonth;
    if (x >= 172 && x < 200) return Action::NextMonth;
    if (x >= 204 && x < 232) return Action::NextYear;
    return Action::None;
}

void navigate(Action action) {
    loadCurrentDate();
    switch (action) {
    case Action::PreviousYear: --viewedYear; break;
    case Action::NextYear: ++viewedYear; break;
    case Action::PreviousMonth:
        if (--viewedMonth < 1) {
            viewedMonth = 12;
            --viewedYear;
        }
        break;
    case Action::NextMonth:
        if (++viewedMonth > 12) {
            viewedMonth = 1;
            ++viewedYear;
        }
        break;
    default: break;
    }
}
}