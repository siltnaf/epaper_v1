#include "pages/settings/settings_page.h"

#include "devices/epd_xingtai/epd_xingtai.h"
#include "font/basic_font.h"
#include "ui/localization.h"

#include <cstring>

namespace {

SettingsPage::State state = {true, true, false, 60, 0};
bool sdMounted = false;
bool audioTestActive = false;

enum class View : uint8_t { Settings, Scanning, Networks, Password, Sd, ContentUrl, Voices };
enum class Keyboard : uint8_t { Lowercase, Uppercase, Symbols };

View view = View::Settings;
Keyboard keyboard = Keyboard::Lowercase;
char networkNames[6][33] = {};
uint8_t networkCount = 0;
char selectedSsid[33] = {};
char enteredPassword[64] = {};
char enteredUrl[128] = {};
char sdNames[8][33] = {};
bool sdDirectories[8] = {};
uint8_t sdCount = 0;
bool formatPending = false;
constexpr const char *VOICES[] = {"Bella", "Jasper", "Luna", "Bruno", "Rosie", "Hugo", "Kiki", "Leo"};
constexpr uint8_t VOICE_COUNT = sizeof(VOICES) / sizeof(VOICES[0]);
uint8_t selectedVoice = 1;

const char LOWERCASE_KEYS[3][11] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
const char UPPERCASE_KEYS[3][11] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
const char SYMBOL_KEYS[3][11] = {"1234567890", "!@#$%^&*()", "/\\_-+=?.:"};

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

const uint8_t LOWERCASE[26][7] = {
    {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}, // a
    {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E}, // b
    {0x00, 0x00, 0x0E, 0x10, 0x10, 0x11, 0x0E}, // c
    {0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F}, // d
    {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}, // e
    {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08}, // f
    {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E}, // g
    {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11}, // h
    {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}, // i
    {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C}, // j
    {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}, // k
    {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, // l
    {0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15}, // m
    {0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11}, // n
    {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}, // o
    {0x00, 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10}, // p
    {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x01}, // q
    {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}, // r
    {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E}, // s
    {0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06}, // t
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D}, // u
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04}, // v
    {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}, // w
    {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}, // x
    {0x00, 0x11, 0x11, 0x11, 0x0F, 0x01, 0x0E}, // y
    {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F}, // z
};

const uint8_t UPPERCASE[26][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // A
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, // B
    {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F}, // C
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, // D
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, // E
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, // F
    {0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F}, // G
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // H
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}, // I
    {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E}, // J
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, // K
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, // L
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, // M
    {0x11, 0x19, 0x15, 0x15, 0x13, 0x13, 0x11}, // N
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // O
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, // P
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, // Q
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, // R
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, // S
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, // T
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // U
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, // V
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, // W
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, // X
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, // Y
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, // Z
};

const uint8_t GLYPH_AT[7] = {0x0E, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0E};
const uint8_t GLYPH_HASH[7] = {0x0A, 0x1F, 0x0A, 0x0A, 0x1F, 0x0A, 0x00};
const uint8_t GLYPH_DOLLAR[7] = {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04};
const uint8_t GLYPH_PERCENT[7] = {0x19, 0x1A, 0x04, 0x08, 0x0B, 0x13, 0x00};
const uint8_t GLYPH_AMPERSAND[7] = {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D};
const uint8_t GLYPH_STAR[7] = {0x00, 0x15, 0x0E, 0x1F, 0x0E, 0x15, 0x00};
const uint8_t GLYPH_PLUS[7] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
const uint8_t GLYPH_MINUS[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
const uint8_t GLYPH_UNDERSCORE[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F};
const uint8_t GLYPH_SLASH[7] = {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};
const uint8_t GLYPH_BACKSLASH[7] = {0x10, 0x08, 0x08, 0x04, 0x02, 0x02, 0x01};
const uint8_t GLYPH_QUESTION[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
const uint8_t GLYPH_PERIOD[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
const uint8_t GLYPH_EXCLAMATION[7] = {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04};
const uint8_t GLYPH_CARET[7] = {0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00};
const uint8_t GLYPH_EQUALS[7] = {0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00, 0x00};
const uint8_t GLYPH_LEFT_PAREN[7] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02};
const uint8_t GLYPH_RIGHT_PAREN[7] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08};
const uint8_t GLYPH_COLON[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};

const uint8_t *glyphFor(char character) {
    if (character >= 'a' && character <= 'z') return LOWERCASE[character - 'a'];
    if (character >= '0' && character <= '9') return DIGITS[character - '0'];
    if (character >= 'A' && character <= 'Z') return UPPERCASE[character - 'A'];
    switch (character) {
    case '@': return GLYPH_AT;
    case '#': return GLYPH_HASH;
    case '$': return GLYPH_DOLLAR;
    case '%': return GLYPH_PERCENT;
    case '&': return GLYPH_AMPERSAND;
    case '*': return GLYPH_STAR;
    case '+': return GLYPH_PLUS;
    case '-': return GLYPH_MINUS;
    case '_': return GLYPH_UNDERSCORE;
    case '/': return GLYPH_SLASH;
    case '\\': return GLYPH_BACKSLASH;
    case '?': return GLYPH_QUESTION;
    case '.': return GLYPH_PERIOD;
    case '!': return GLYPH_EXCLAMATION;
    case '^': return GLYPH_CARET;
    case '=': return GLYPH_EQUALS;
    case '(': return GLYPH_LEFT_PAREN;
    case ')': return GLYPH_RIGHT_PAREN;
    case ':': return GLYPH_COLON;
    default: return nullptr;
    }
}

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

void rect(uint8_t *frame, int x, int y, int width, int height) {
    hline(frame, x, y, width);
    hline(frame, x, y + height - 1, width);
    vline(frame, x, y, height);
    vline(frame, x + width - 1, y, height);
}

void fillRect(uint8_t *frame, int x, int y, int width, int height) {
    for (int row = 0; row < height; ++row) hline(frame, x, y + row, width);
}

int textWidth(const char *text, int scale) {
    if (text && static_cast<uint8_t>(*text) >= 0x80) return UiLocalization::textWidth(text, scale);
    return text && *text ? static_cast<int>(std::strlen(text)) * (BasicFont::ENGLISH_WIDTH + 1) * scale - scale : 0;
}

void text(uint8_t *frame, int x, int y, const char *value, int scale = 2) {
    if (value && static_cast<uint8_t>(*value) >= 0x80) {
        UiLocalization::drawText(frame, x, y, value, scale);
        return;
    }
    for (const char *cursor = value; cursor && *cursor; ++cursor) {
        if (*cursor == ' ') {
            x += (BasicFont::ENGLISH_WIDTH + 1) * scale;
            continue;
        }
        const uint8_t *glyph = glyphFor(*cursor);
        if (glyph) {
            for (int row = 0; row < BasicFont::ENGLISH_HEIGHT; ++row) {
                for (int column = 0; column < BasicFont::ENGLISH_WIDTH; ++column) {
                    if ((glyph[row] & (0x10U >> column)) == 0) continue;
                    fillRect(frame, x + column * scale, y + row * scale, scale, scale);
                }
            }
        }
        x += (BasicFont::ENGLISH_WIDTH + 1) * scale;
    }
}

void centeredText(uint8_t *frame, int y, const char *value, int scale = 2) {
    text(frame, (XingtaiEpd::WIDTH - textWidth(value, scale)) / 2, y, value, scale);
}

void toggle(uint8_t *frame, int x, int y, bool enabled) {
    constexpr int width = 52;
    constexpr int height = 26;
    rect(frame, x, y, width, height);
    rect(frame, x + 1, y + 1, width - 2, height - 2);
    const int knobX = enabled ? x + width - 22 : x + 4;
    fillRect(frame, knobX, y + 4, 18, height - 8);
    text(frame, x - 38, y + 7,
         UiLocalization::isChinese() ? (enabled ? "开" : "关") : (enabled ? "ON" : "OFF"), 1);
}

bool inRect(int16_t x, int16_t y, int left, int top, int width, int height) {
    return x >= left && x < left + width && y >= top && y < top + height;
}

const char (*currentKeys())[11];

void clippedText(uint8_t *frame, int x, int y, const char *value, int maxCharacters, int scale = 1) {
    char clipped[34] = {};
    if (!value) return;
    const size_t limit = maxCharacters > 0
        ? min(static_cast<size_t>(maxCharacters), sizeof(clipped) - 1)
        : 0;
    std::strncpy(clipped, value, limit);
    text(frame, x, y, clipped, scale);
}

void button(uint8_t *frame, int x, int y, int width, int height, const char *label,
            bool bold = false) {
    rect(frame, x, y, width, height);
    if (bold) {
        // A second inset outline thickens the frame so the running test stands
        // out on the monochrome panel, matching the active/pressed look used by
        // the home icons.
        rect(frame, x + 1, y + 1, width - 2, height - 2);
    }
    const int labelX = x + (width - textWidth(label, 1)) / 2;
    const int labelY = y + (height - BasicFont::ENGLISH_HEIGHT) / 2;
    if (bold) text(frame, labelX + 1, labelY, label, 1);  // offset pass fakes bold
    text(frame, labelX, labelY, label, 1);
}

void renderSettings(uint8_t *frame) {
    const bool cn = UiLocalization::isChinese();
    centeredText(frame, 40, cn ? "设置" : "SETTINGS", 1);

    constexpr int rowHeight = 32;
    const auto rowY = [](int index) { return 58 + index * 36; };

    rect(frame, 16, rowY(0), 208, rowHeight);
    text(frame, 28, rowY(0) + 9, cn ? "内容网址" : "CONTENT URL", cn ? 1 : 2);

    rect(frame, 16, rowY(1), 208, rowHeight);
    text(frame, 28, rowY(1) + 9, cn ? "无线网络" : "WIFI", cn ? 1 : 2);
    toggle(frame, 156, rowY(1) + 3, state.wifiEnabled);

    rect(frame, 16, rowY(2), 208, rowHeight);
    text(frame, 28, rowY(2) + 9, "4G", 2);
    toggle(frame, 156, rowY(2) + 3, state.cellularEnabled);

    rect(frame, 16, rowY(3), 208, rowHeight);
    text(frame, 28, rowY(3) + 9, cn ? "存储卡" : "SD CARD", cn ? 1 : 2);
    text(frame, 154, rowY(3) + 10, cn ? (sdMounted ? "就绪" : "未找到") : (sdMounted ? "READY" : "NONE"), 1);

    rect(frame, 16, rowY(4), 208, rowHeight);
    text(frame, 28, rowY(4) + 9, cn ? "语言" : "LANGUAGE", cn ? 1 : 2);
    text(frame, 166, rowY(4) + 10, cn ? "中文" : "EN", 1);

    rect(frame, 16, rowY(5), 208, rowHeight);
    text(frame, 28, rowY(5) + 9, cn ? "语音" : "TTS VOICE", cn ? 1 : 2);
    text(frame, 220 - textWidth(VOICES[selectedVoice], 2), rowY(5) + 9,
         VOICES[selectedVoice], 2);

    rect(frame, 16, rowY(6), 208, rowHeight);
    text(frame, 28, rowY(6) + 9, cn ? "音量" : "AUDIO", cn ? 1 : 2);
    button(frame, 132, rowY(6) + 2, 28, 28, "-");
    button(frame, 188, rowY(6) + 2, 28, 28, "+");

    char volume[5] = {};
    const uint8_t value = state.volumePercent;
    if (value == 100) std::memcpy(volume, "100", 4);
    else if (value >= 10) {
        volume[0] = static_cast<char>('0' + value / 10);
        volume[1] = static_cast<char>('0' + value % 10);
    } else volume[0] = static_cast<char>('0' + value);
    text(frame, 181 - textWidth(volume, 1), rowY(6) + 12, volume, 1);

    rect(frame, 16, rowY(7), 208, rowHeight);
    text(frame, 28, rowY(7) + 9, cn ? "声音测试" : "AUDIO TEST", cn ? 1 : 2);
    button(frame, 164, rowY(7) + 2, 52, 28, cn ? "测试" : "TEST", audioTestActive);

    rect(frame, 16, rowY(8), 208, rowHeight);
    text(frame, 28, rowY(8) + 9, cn ? "缓存" : "CACHE", cn ? 1 : 2);
    toggle(frame, 156, rowY(8) + 3, state.playlistCacheEnabled);
}

void renderVoices(uint8_t *frame) {
    const bool cn = UiLocalization::isChinese();
    centeredText(frame, 34, cn ? "语音" : "TTS VOICE", 1);
    text(frame, 22, 56, cn ? "选择语音" : "SELECT VOICE", 1);
    for (uint8_t index = 0; index < VOICE_COUNT; ++index) {
        const int column = index % 2;
        const int row = index / 2;
        const int x = 14 + column * 108;
        const int y = 78 + row * 42;
        rect(frame, x, y, 100, 34);
        if (index == selectedVoice) {
            rect(frame, x + 3, y + 3, 94, 28);
            fillRect(frame, x + 8, y + 12, 8, 8);
        }
        text(frame, x + (100 - textWidth(VOICES[index], 2)) / 2,
             y + (34 - BasicFont::ENGLISH_HEIGHT * 2) / 2, VOICES[index], 2);
    }
    button(frame, 14, 260, 100, 40, cn ? "返回" : "BACK");
}

void renderNetworks(uint8_t *frame) {
    const bool cn = UiLocalization::isChinese();
    centeredText(frame, 48, cn ? "选择无线网络" : "SELECT WIFI", cn ? 1 : 2);
    if (networkCount == 0) {
        centeredText(frame, 150, cn ? "未找到网络" : "NO NETWORKS", cn ? 1 : 2);
        button(frame, 70, 210, 100, 42, cn ? "重试" : "RETRY");
        return;
    }
    for (uint8_t index = 0; index < networkCount; ++index) {
        const int y = 82 + index * 48;
        rect(frame, 14, y, 212, 40);
        clippedText(frame, 24, y + 16, networkNames[index], 30, 1);
    }
}

void renderSd(uint8_t *frame) {
    const bool cn = UiLocalization::isChinese();
    centeredText(frame, 42, cn ? "存储卡" : "SD CARD", cn ? 1 : 2);
    if (sdCount == 0) centeredText(frame, 92, cn ? "空" : "EMPTY", cn ? 1 : 2);
    for (uint8_t i = 0; i < sdCount; ++i) {
        const int y = 74 + i * 30;
        clippedText(frame, 20, y, sdNames[i], 38, 1);
    }
    if (formatPending) {
        centeredText(frame, 300, cn ? "擦除存储卡" : "ERASE SD?", cn ? 1 : 2);
        button(frame, 18, 340, 96, 42, cn ? "取消" : "CANCEL");
        button(frame, 126, 340, 96, 42, cn ? "确认" : "CONFIRM");
    } else {
        button(frame, 18, 340, 96, 42, cn ? "返回" : "BACK");
        button(frame, 126, 340, 96, 42, cn ? "格式化" : "FORMAT");
    }
}

const char (*currentKeys())[11] {
    if (keyboard == Keyboard::Uppercase) return UPPERCASE_KEYS;
    if (keyboard == Keyboard::Symbols) return SYMBOL_KEYS;
    return LOWERCASE_KEYS;
}

void renderPassword(uint8_t *frame) {
    const bool cn = UiLocalization::isChinese();
    centeredText(frame, 42, cn ? "无线网络密码" : "WIFI PASSWORD", cn ? 1 : 2);
    clippedText(frame, 12, 68, selectedSsid, 34, 1);
    rect(frame, 10, 88, 220, 38);
    const size_t length = std::strlen(enteredPassword);
    constexpr size_t visiblePasswordCharacters = 17;
    const char *visiblePassword = length > visiblePasswordCharacters
        ? enteredPassword + length - visiblePasswordCharacters : enteredPassword;
    clippedText(frame, 18, 100, visiblePassword, visiblePasswordCharacters, 2);

    const char (*keys)[11] = currentKeys();
    for (int row = 0; row < 3; ++row) {
        const int count = static_cast<int>(std::strlen(keys[row]));
        const int keyWidth = 23;
        const int startX = (XingtaiEpd::WIDTH - count * keyWidth) / 2;
        const int y = 142 + row * 48;
        for (int column = 0; column < count; ++column) {
            rect(frame, startX + column * keyWidth, y, keyWidth, 40);
            char label[2] = {keys[row][column], '\0'};
            text(frame, startX + column * keyWidth + (keyWidth - textWidth(label, 2)) / 2,
                 y + (40 - BasicFont::ENGLISH_HEIGHT * 2) / 2, label, 2);
        }
    }

    button(frame, 8, 288, 54, 38, keyboard == Keyboard::Lowercase ? "ABC" :
           keyboard == Keyboard::Uppercase ? "123" : "abc");
    button(frame, 66, 288, 108, 38, cn ? "空格" : "SPACE");
    button(frame, 178, 288, 54, 38, cn ? "删除" : "DEL");
    button(frame, 8, 342, 108, 44, cn ? "连接" : "CONNECT");
    button(frame, 124, 342, 108, 44, cn ? "取消" : "CANCEL");
}

void renderContentUrl(uint8_t *frame) {
    const bool cn = UiLocalization::isChinese();
    centeredText(frame, 42, cn ? "内容网址" : "CONTENT URL", cn ? 1 : 2);
    rect(frame, 10, 68, 220, 58);
    const size_t length = std::strlen(enteredUrl);
    constexpr size_t charactersPerLine = 17;
    constexpr size_t visibleCharacters = charactersPerLine * 2;
    const char *visible = length > visibleCharacters ? enteredUrl + length - visibleCharacters : enteredUrl;
    clippedText(frame, 18, 78, visible, charactersPerLine, 2);
    if (std::strlen(visible) > charactersPerLine) {
        clippedText(frame, 18, 98, visible + charactersPerLine, charactersPerLine, 2);
    }

    const char (*keys)[11] = currentKeys();
    for (int row = 0; row < 3; ++row) {
        const int count = static_cast<int>(std::strlen(keys[row]));
        const int keyWidth = 23;
        const int startX = (XingtaiEpd::WIDTH - count * keyWidth) / 2;
        const int y = 142 + row * 48;
        for (int column = 0; column < count; ++column) {
            rect(frame, startX + column * keyWidth, y, keyWidth, 40);
            char label[2] = {keys[row][column], '\0'};
            text(frame, startX + column * keyWidth + (keyWidth - textWidth(label, 2)) / 2,
                 y + (40 - BasicFont::ENGLISH_HEIGHT * 2) / 2, label, 2);
        }
    }
    button(frame, 8, 288, 54, 38, keyboard == Keyboard::Lowercase ? "ABC" :
           keyboard == Keyboard::Uppercase ? "123" : "abc");
    button(frame, 66, 288, 108, 38, cn ? "空格" : "SPACE");
    button(frame, 178, 288, 54, 38, cn ? "删除" : "DEL");
    button(frame, 8, 342, 72, 44, cn ? "保存" : "SAVE");
    button(frame, 84, 342, 72, 44, cn ? "清除" : "CLEAR");
    button(frame, 160, 342, 72, 44, cn ? "取消" : "CANCEL");
}

}

namespace SettingsPage {

void setState(const State &newState) {
    state = newState;
}

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    if (view == View::Scanning) centeredText(frame, 170, UiLocalization::isChinese() ? "正在扫描无线网络" : "SCANNING WIFI", UiLocalization::isChinese() ? 1 : 2);
    else if (view == View::Networks) renderNetworks(frame);
    else if (view == View::Password) renderPassword(frame);
    else if (view == View::Sd) renderSd(frame);
    else if (view == View::ContentUrl) renderContentUrl(frame);
    else if (view == View::Voices) renderVoices(frame);
    else renderSettings(frame);
}

Action actionAt(int16_t x, int16_t y) {
    if (view == View::ContentUrl) {
        if (inRect(x, y, 8, 342, 72, 44)) return Action::UrlSave;
        if (inRect(x, y, 84, 342, 72, 44)) return Action::UrlClear;
        if (inRect(x, y, 160, 342, 72, 44)) return Action::UrlCancel;
        if (inRect(x, y, 8, 288, 54, 38)) return Action::UrlChangeKeyboard;
        if (inRect(x, y, 66, 288, 108, 38)) return Action::UrlSpace;
        if (inRect(x, y, 178, 288, 54, 38)) return Action::UrlBackspace;
        return keyAt(x, y) != '\0' ? Action::UrlKey : Action::None;
    }
    if (view == View::Sd) {
        if (inRect(x, y, 18, 340, 96, 42)) return Action::SdBack;
        if (inRect(x, y, 126, 340, 96, 42)) return formatPending ? Action::SdConfirmFormat : Action::SdRequestFormat;
        return Action::None;
    }
    if (view == View::Networks) {
        if (networkCount == 0 && inRect(x, y, 70, 210, 100, 42)) return Action::RetryWifiScan;
        return networkIndexAt(x, y) < networkCount ? Action::SelectWifiNetwork : Action::None;
    }
    if (view == View::Password) {
        if (inRect(x, y, 8, 342, 108, 44)) return Action::WifiConnect;
        if (inRect(x, y, 124, 342, 108, 44)) return Action::WifiCancel;
        if (inRect(x, y, 8, 288, 54, 38)) return Action::WifiChangeKeyboard;
        if (inRect(x, y, 66, 288, 108, 38)) return Action::WifiSpace;
        if (inRect(x, y, 178, 288, 54, 38)) return Action::WifiBackspace;
        return keyAt(x, y) != '\0' ? Action::WifiKey : Action::None;
    }
    if (view == View::Voices) {
        if (inRect(x, y, 14, 260, 100, 40)) return Action::VoiceBack;
        return voiceIndexAt(x, y) < VOICE_COUNT ? Action::SelectVoice : Action::None;
    }
    if (view != View::Settings) return Action::None;
    constexpr int rowHeight = 32;
    const auto rowY = [](int index) { return 58 + index * 36; };
    if (inRect(x, y, 16, rowY(0), 208, rowHeight)) return Action::OpenContentUrl;
    if (inRect(x, y, 16, rowY(1), 125, rowHeight)) return Action::OpenWifiSetup;
    if (inRect(x, y, 141, rowY(1), 83, rowHeight)) return Action::ToggleWifi;
    if (inRect(x, y, 16, rowY(2), 208, rowHeight)) return Action::ToggleCellular;
    if (inRect(x, y, 16, rowY(3), 208, rowHeight)) return Action::RefreshSdCard;
    if (inRect(x, y, 16, rowY(4), 208, rowHeight)) return Action::CycleLanguage;
    if (inRect(x, y, 16, rowY(5), 208, rowHeight)) return Action::OpenVoiceSelection;
    if (inRect(x, y, 116, rowY(6), 56, rowHeight)) return Action::VolumeDown;
    if (inRect(x, y, 172, rowY(6), 52, rowHeight)) return Action::VolumeUp;
    if (inRect(x, y, 16, rowY(7), 208, rowHeight)) return Action::TestAudio;
    if (inRect(x, y, 16, rowY(8), 208, rowHeight)) return Action::TogglePlaylistCache;
    return Action::None;
}

void showSettings() { view = View::Settings; }

void setVoice(const char *voice) {
    for (uint8_t index = 0; index < VOICE_COUNT; ++index) {
        if (voice && std::strcmp(voice, VOICES[index]) == 0) {
            selectedVoice = index;
            return;
        }
    }
    selectedVoice = 1;
}

const char *voiceName() { return VOICES[selectedVoice]; }

const char *voiceNameAt(uint8_t index) { return index < VOICE_COUNT ? VOICES[index] : nullptr; }

void showVoiceSelection() { view = View::Voices; }

uint8_t voiceIndexAt(int16_t x, int16_t y) {
    for (uint8_t index = 0; index < VOICE_COUNT; ++index) {
        const int column = index % 2;
        const int row = index / 2;
        if (inRect(x, y, 14 + column * 108, 78 + row * 42, 100, 34)) return index;
    }
    return 0xFF;
}

void setSdMounted(bool mounted) { sdMounted = mounted; }

void setAudioTestActive(bool active) { audioTestActive = active; }

void showSdPage(const char names[][33], const bool directories[], uint8_t count) {
    sdCount = count > 8 ? 8 : count;
    for (uint8_t i = 0; i < sdCount; ++i) {
        std::strncpy(sdNames[i], names[i], 32);
        sdNames[i][32] = '\0';
        sdDirectories[i] = directories[i];
    }
    formatPending = false;
    view = View::Sd;
}

void setFormatPending(bool pending) { formatPending = pending; }

void showWifiScanning() { view = View::Scanning; }

void showWifiNetworks(const char networks[][33], uint8_t count) {
    networkCount = count > 6 ? 6 : count;
    for (uint8_t index = 0; index < networkCount; ++index) {
        std::strncpy(networkNames[index], networks[index], sizeof(networkNames[index]) - 1);
        networkNames[index][sizeof(networkNames[index]) - 1] = '\0';
    }
    keyboard = Keyboard::Lowercase;
    view = View::Networks;
}

void showWifiPassword(const char *ssid, const char *password) {
    std::strncpy(selectedSsid, ssid ? ssid : "", sizeof(selectedSsid) - 1);
    selectedSsid[sizeof(selectedSsid) - 1] = '\0';
    std::strncpy(enteredPassword, password ? password : "", sizeof(enteredPassword) - 1);
    enteredPassword[sizeof(enteredPassword) - 1] = '\0';
    view = View::Password;
}

void showContentUrl(const char *url) {
    std::strncpy(enteredUrl, url ? url : "", sizeof(enteredUrl) - 1);
    enteredUrl[sizeof(enteredUrl) - 1] = '\0';
    view = View::ContentUrl;
}

void cycleKeyboard() {
    if (keyboard == Keyboard::Lowercase) keyboard = Keyboard::Uppercase;
    else if (keyboard == Keyboard::Uppercase) keyboard = Keyboard::Symbols;
    else keyboard = Keyboard::Lowercase;
}

uint8_t networkIndexAt(int16_t x, int16_t y) {
    if (x < 14 || x >= 226 || y < 82) return 0xFF;
    const int relativeY = y - 82;
    if (relativeY % 48 >= 40) return 0xFF;
    const uint8_t index = static_cast<uint8_t>(relativeY / 48);
    return index < networkCount ? index : 0xFF;
}

char keyAt(int16_t x, int16_t y) {
    if (y < 142 || y >= 278) return '\0';
    const int row = (y - 142) / 48;
    if ((y - 142) % 48 >= 40 || row < 0 || row >= 3) return '\0';
    const char (*keys)[11] = currentKeys();
    const int count = static_cast<int>(std::strlen(keys[row]));
    const int startX = (XingtaiEpd::WIDTH - count * 23) / 2;
    if (x < startX || x >= startX + count * 23) return '\0';
    return keys[row][(x - startX) / 23];
}

}