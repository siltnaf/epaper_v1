#include "pages/calculator/calculator_page.h"

#include "devices/epd_xingtai/epd_xingtai.h"
#include "ui/localization.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct Button {
    const char *label;
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
};

constexpr Button BUTTONS[] = {
    {"C", 8, 116, 52, 48}, {"DEL", 64, 116, 52, 48}, {"/", 120, 116, 52, 48}, {"*", 176, 116, 56, 48},
    {"7", 8, 168, 52, 48}, {"8", 64, 168, 52, 48}, {"9", 120, 168, 52, 48}, {"-", 176, 168, 56, 48},
    {"4", 8, 220, 52, 48}, {"5", 64, 220, 52, 48}, {"6", 120, 220, 52, 48}, {"+", 176, 220, 56, 48},
    {"1", 8, 272, 52, 48}, {"2", 64, 272, 52, 48}, {"3", 120, 272, 52, 48}, {"=", 176, 272, 56, 100},
    {"0", 8, 324, 108, 48}, {".", 120, 324, 52, 48},
};

char displayValue[24] = "0";
char expression[48] = "0";
double storedValue = 0.0;
char pendingOperator = 0;
bool newInput = true;

void pixel(uint8_t *frame, int x, int y) {
    if (x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |= 0x80U >> (x % 8);
}

void hline(uint8_t *frame, int x, int y, int width) {
    for (int column = 0; column < width; ++column) pixel(frame, x + column, y);
}

void vline(uint8_t *frame, int x, int y, int height) {
    for (int row = 0; row < height; ++row) pixel(frame, x, y + row);
}

void rect(uint8_t *frame, int x, int y, int width, int height) {
    hline(frame, x, y, width);
    hline(frame, x, y + height - 1, width);
    vline(frame, x, y, height);
    vline(frame, x + width - 1, y, height);
}

void fillRect(uint8_t *frame, int x, int y, int width, int height) {
    for (int row = 0; row < height; ++row) hline(frame, x, y + row, width);
}

const uint8_t *glyph(char character) {
    static const uint8_t digits[10][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
        {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E},
        {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},{0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E},
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
    static const uint8_t plus[7] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00};
    static const uint8_t minus[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
    static const uint8_t multiply[7] = {0x00,0x11,0x0A,0x04,0x0A,0x11,0x00};
    static const uint8_t divide[7] = {0x01,0x02,0x02,0x04,0x08,0x08,0x10};
    static const uint8_t equal[7] = {0x00,0x1F,0x00,0x1F,0x00,0x00,0x00};
    static const uint8_t period[7] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C};
    if (character >= '0' && character <= '9') return digits[character - '0'];
    if (character >= 'A' && character <= 'Z') return letters[character - 'A'];
    if (character == '+') return plus;
    if (character == '-') return minus;
    if (character == '*') return multiply;
    if (character == '/') return divide;
    if (character == '=') return equal;
    if (character == '.') return period;
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

bool inRect(int16_t x, int16_t y, const Button &button) {
    return x >= button.x && x < button.x + button.width &&
           y >= button.y && y < button.y + button.height;
}

void reset() {
    std::strcpy(displayValue, "0");
    std::strcpy(expression, "0");
    storedValue = 0.0;
    pendingOperator = 0;
    newInput = true;
}

double apply(double left, double right, char operation) {
    switch (operation) {
    case '+': return left + right;
    case '-': return left - right;
    case '*': return left * right;
    case '/': return right == 0.0 ? NAN : left / right;
    default: return right;
    }
}

void setResult(double value) {
    if (!std::isfinite(value)) {
        std::strcpy(displayValue, "ERROR");
        std::strcpy(expression, "ERROR");
    } else {
        std::snprintf(displayValue, sizeof(displayValue), "%.10g", value);
        std::strncpy(expression, displayValue, sizeof(expression) - 1);
        expression[sizeof(expression) - 1] = '\0';
    }
    newInput = true;
}

void appendExpression(const char *value) {
    if (std::strcmp(expression, "0") == 0 || std::strcmp(expression, "ERROR") == 0) {
        std::strncpy(expression, value, sizeof(expression) - 1);
        expression[sizeof(expression) - 1] = '\0';
    } else if (std::strlen(expression) + std::strlen(value) < sizeof(expression)) {
        std::strcat(expression, value);
    }
}

void handleButton(const char *label) {
    if (std::strcmp(label, "C") == 0) {
        reset();
        return;
    }
    if (std::strcmp(label, "DEL") == 0) {
        size_t length = std::strlen(displayValue);
        if (!newInput && length > 1) displayValue[length - 1] = '\0';
        else {
            std::strcpy(displayValue, "0");
            newInput = true;
        }
        length = std::strlen(expression);
        if (length > 1) expression[length - 1] = '\0';
        else std::strcpy(expression, "0");
        return;
    }
    if (std::strcmp(label, "=") == 0) {
        if (pendingOperator) {
            setResult(apply(storedValue, std::atof(displayValue), pendingOperator));
            pendingOperator = 0;
        }
        return;
    }
    if (label[1] == '\0' && std::strchr("+-*/", label[0])) {
        double value = std::atof(displayValue);
        if (pendingOperator && !newInput) value = apply(storedValue, value, pendingOperator);
        storedValue = value;
        pendingOperator = label[0];
        size_t length = std::strlen(expression);
        if (length > 0 && std::strchr("+-*/", expression[length - 1])) expression[length - 1] = label[0];
        else appendExpression(label);
        newInput = true;
        return;
    }
    if (std::strcmp(label, ".") == 0) {
        if (newInput) {
            std::strcpy(displayValue, "0.");
            appendExpression("0.");
            newInput = false;
        } else if (!std::strchr(displayValue, '.')) {
            std::strcat(displayValue, ".");
            appendExpression(".");
        }
        return;
    }
    if (label[1] == '\0' && label[0] >= '0' && label[0] <= '9') {
        if (newInput || std::strcmp(displayValue, "0") == 0 || std::strcmp(displayValue, "ERROR") == 0) {
            std::strncpy(displayValue, label, sizeof(displayValue) - 1);
            displayValue[sizeof(displayValue) - 1] = '\0';
            if (newInput && !pendingOperator) std::strcpy(expression, "0");
            newInput = false;
        } else if (std::strlen(displayValue) < sizeof(displayValue) - 1) {
            std::strcat(displayValue, label);
        }
        appendExpression(label);
    }
}

}

namespace CalculatorPage {

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);

    rect(frame, 8, 38, 224, 68);
    rect(frame, 11, 41, 218, 62);
    const char *visible = expression;
    constexpr int maxCharacters = 17;
    const int length = static_cast<int>(std::strlen(expression));
    if (length > maxCharacters) visible = expression + length - maxCharacters;
    if (UiLocalization::isChinese() && std::strcmp(visible, "ERROR") == 0) {
        UiLocalization::drawText(frame, 218 - UiLocalization::textWidth("错误", 1), 62, "错误", 1);
    } else {
        text(frame, 218 - textWidth(visible, 2), 64, visible, 2);
    }

    for (const Button &button : BUTTONS) {
        rect(frame, button.x, button.y, button.width, button.height);
        rect(frame, button.x + 2, button.y + 2, button.width - 4, button.height - 4);
        const bool chineseDelete = UiLocalization::isChinese() && std::strcmp(button.label, "DEL") == 0;
        const char *label = chineseDelete ? "删除" : button.label;
        const int scale = std::strcmp(button.label, "DEL") == 0 ? 1 : 2;
        if (chineseDelete) UiLocalization::drawText(frame,
             button.x + (button.width - UiLocalization::textWidth(label, scale)) / 2,
             button.y + (button.height - 16 * scale) / 2, label, scale);
        else text(frame,
             button.x + (button.width - textWidth(label, scale)) / 2,
             button.y + (button.height - 7 * scale) / 2,
             label, scale);
    }
}

bool handleTap(int16_t x, int16_t y) {
    for (const Button &button : BUTTONS) {
        if (!inRect(x, y, button)) continue;
        handleButton(button.label);
        return true;
    }
    return false;
}

}