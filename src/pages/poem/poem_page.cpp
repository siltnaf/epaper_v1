#include "pages/poem/poem_page.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "devices/audio/opus_player.h"
#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/es8311/es8311.h"
#include "devices/sd_card/sd_card.h"
#include "font/xiaozhi_font.h"
#include "ui/loading_indicator.h"
#include "ui/localization.h"

#include <cstring>

namespace {

constexpr uint8_t ITEMS_PER_PAGE = 10;
constexpr int LIST_TOP = 82;
constexpr int ROW_HEIGHT = 30;
constexpr int ROW_GAP = 2;
constexpr int PAGER_TOP = 52;
constexpr int PAGER_HEIGHT = 25;
constexpr int PAGER_BUTTON_WIDTH = 34;
constexpr char POEM_SD_FOLDER[] = "/poems";
constexpr int READER_CONTENT_X = 16;
constexpr int READER_CONTENT_Y = 108;
constexpr int READER_CONTENT_WIDTH = 208;
constexpr int READER_CONTENT_BOTTOM = 358;
constexpr int READER_LINE_HEIGHT = 25;
constexpr int READER_LINES_PER_PAGE =
    (READER_CONTENT_BOTTOM - READER_CONTENT_Y) / READER_LINE_HEIGHT;
constexpr uint16_t MAX_READER_PAGES = 128;
constexpr int MARQUEE_X = 34;
constexpr int MARQUEE_WIDTH = 171;
constexpr int MARQUEE_HEIGHT = ROW_HEIGHT - 2;
constexpr int MARQUEE_ROW_BYTES = (MARQUEE_WIDTH + 7) / 8;
constexpr int POPUP_X = 12;
constexpr int POPUP_Y = LIST_TOP;
constexpr int POPUP_W = 216;
constexpr int POPUP_H = ROW_HEIGHT * ITEMS_PER_PAGE + ROW_GAP * (ITEMS_PER_PAGE - 1);
constexpr int POPUP_CONTENT_X = 22;
constexpr int POPUP_CONTENT_Y = POPUP_Y + 54;
constexpr int POPUP_CONTENT_W = 196;
constexpr int POPUP_CONTENT_BOTTOM = POPUP_Y + POPUP_H - 14;

struct PoemItem {
    int32_t id = 0;
    char title[64] = {};
    bool saved = false;
};

enum class View : uint8_t { Library, Player };

PoemItem stories[ITEMS_PER_PAGE] = {};
uint8_t poemCount = 0;
int32_t poemTotal = 0;
int32_t libraryPage = 1;
View view = View::Library;
char contentBaseUrl[128] = "http://";
char selectedVoice[32] = "Jasper";
char statusText[64] = "OPENING LIBRARY";
char audioStatus[48] = "点击播放";
int32_t selectedPoemId = 0;
char selectedTitle[64] = {};
char selectedAuthor[64] = {};
char selectedDynasty[64] = {};
String selectedContent;
int readerPage = 0;
uint32_t readerPageOffsets[MAX_READER_PAGES + 1] = {};
uint16_t readerPageTotal = 1;
bool pendingSave = false;
bool poemPlaying = false;
bool pendingAudioStart = false;
int8_t activePoemIndex = -1;
uint8_t marqueeBitmap[MARQUEE_ROW_BYTES * MARQUEE_HEIGHT] = {};
bool marqueeReady = false;
bool poemPopupOpen = false;
uint16_t marqueeOffset = 0;
uint32_t nextMarqueeMs = 0;
int8_t dirtyRowA = -1;
int8_t dirtyRowB = -1;
volatile bool libraryLoadRunning = false;
volatile bool libraryLoadCompleted = false;
volatile bool libraryAwaitingContent = false;

struct OptionalLoadingScope {
    explicit OptionalLoadingScope(bool enabled) : enabled(enabled) {
        if (enabled) UiLoadingIndicator::show();
    }
    ~OptionalLoadingScope() {
        if (enabled) UiLoadingIndicator::hide();
    }
    bool enabled;
};

bool cachedOpusPath(int32_t poemId, char *path, size_t pathSize);

void pixel(uint8_t *frame, int x, int y) {
    if (x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |= 0x80U >> (x % 8);
}

void line(uint8_t *frame, int x1, int y1, int x2, int y2) {
    const int dx = abs(x2 - x1);
    const int sx = x1 < x2 ? 1 : -1;
    const int dy = -abs(y2 - y1);
    const int sy = y1 < y2 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        pixel(frame, x1, y1);
        if (x1 == x2 && y1 == y2) break;
        const int doubled = error * 2;
        if (doubled >= dy) { error += dy; x1 += sx; }
        if (doubled <= dx) { error += dx; y1 += sy; }
    }
}

void rect(uint8_t *frame, int x, int y, int width, int height) {
    line(frame, x, y, x + width - 1, y);
    line(frame, x, y + height - 1, x + width - 1, y + height - 1);
    line(frame, x, y, x, y + height - 1);
    line(frame, x + width - 1, y, x + width - 1, y + height - 1);
}

void boldRect(uint8_t *frame, int x, int y, int width, int height) {
    rect(frame, x, y, width, height);
    if (width > 2 && height > 2) rect(frame, x + 1, y + 1, width - 2, height - 2);
}

bool pointInRect(int16_t x, int16_t y, int left, int top, int width, int height) {
    return x >= left && x < left + width && y >= top && y < top + height;
}

void drawCentered(uint8_t *frame, int y, const char *text, int scale = 1) {
    UiLocalization::drawCentered(frame, y, text, scale);
}

void drawArrow(uint8_t *frame, int centerX, int centerY, bool right) {
    const int direction = right ? 1 : -1;
    line(frame, centerX - direction * 5, centerY - 7, centerX + direction * 3, centerY);
    line(frame, centerX + direction * 3, centerY, centerX - direction * 5, centerY + 7);
}

void drawDoubleArrow(uint8_t *frame, int centerX, int centerY, bool right) {
    drawArrow(frame, centerX - (right ? 4 : -4), centerY, right);
    drawArrow(frame, centerX + (right ? 4 : -4), centerY, right);
}

void drawCheckmark(uint8_t *frame, int centerX, int centerY) {
    line(frame, centerX - 7, centerY, centerX - 2, centerY + 6);
    line(frame, centerX - 2, centerY + 6, centerX + 8, centerY - 7);
}

void drawPlayPause(uint8_t *frame, int centerX, int centerY, bool pause) {
    if (pause) {
        rect(frame, centerX - 7, centerY - 8, 5, 17);
        rect(frame, centerX + 3, centerY - 8, 5, 17);
        return;
    }
    line(frame, centerX - 6, centerY - 9, centerX - 6, centerY + 9);
    line(frame, centerX - 6, centerY - 9, centerX + 8, centerY);
    line(frame, centerX + 8, centerY, centerX - 6, centerY + 9);
}

bool framePixel(const uint8_t *frame, int x, int y) {
    if (!frame || x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return false;
    return (frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] &
            (0x80U >> (x % 8))) != 0;
}

void clearFrameArea(uint8_t *frame, int x, int y, int width, int height) {
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const int drawX = x + column;
            const int drawY = y + row;
            frame[static_cast<size_t>(drawY) * (XingtaiEpd::WIDTH / 8) + drawX / 8] &=
                static_cast<uint8_t>(~(0x80U >> (drawX % 8)));
        }
    }
}

void captureMarquee(const uint8_t *frame, int top) {
    std::memset(marqueeBitmap, 0x00, sizeof(marqueeBitmap));
    for (int y = 0; y < MARQUEE_HEIGHT; ++y) {
        for (int x = 0; x < MARQUEE_WIDTH; ++x) {
            if (framePixel(frame, MARQUEE_X + x, top + 1 + y)) {
                marqueeBitmap[y * MARQUEE_ROW_BYTES + x / 8] |= 0x80U >> (x % 8);
            }
        }
    }
    marqueeReady = true;
    marqueeOffset = 0;
    nextMarqueeMs = millis() + 900;
}

bool marqueePixel(int x, int y) {
    return (marqueeBitmap[y * MARQUEE_ROW_BYTES + x / 8] & (0x80U >> (x % 8))) != 0;
}

void markDirtyRow(int8_t index) {
    if (index < 0 || index >= static_cast<int8_t>(poemCount) || dirtyRowA == index ||
        dirtyRowB == index) return;
    if (dirtyRowA < 0) dirtyRowA = index;
    else dirtyRowB = index;
}

bool nextUtf8Codepoint(const char *&cursor, uint32_t &codepoint) {
    const uint8_t *source = reinterpret_cast<const uint8_t *>(cursor);
    if (!source || source[0] == 0) return false;
    if (source[0] < 0x80) {
        codepoint = source[0];
        cursor += 1;
        return true;
    }
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
    if ((source[0] & 0xF8) == 0xF0 && (source[1] & 0xC0) == 0x80 &&
        (source[2] & 0xC0) == 0x80 && (source[3] & 0xC0) == 0x80) {
        codepoint = ((source[0] & 0x07) << 18) | ((source[1] & 0x3F) << 12) |
                    ((source[2] & 0x3F) << 6) | (source[3] & 0x3F);
        cursor += 4;
        return true;
    }
    codepoint = '?';
    cursor += 1;
    return true;
}

void drawChineseGlyph(uint8_t *frame, const XiaozhiFont::Glyph *glyph, int x, int y,
                      int clipRight, int clipBottom) {
    if (!glyph) return;
    for (uint16_t glyphY = 0; glyphY < glyph->height && y + glyphY < clipBottom; ++glyphY) {
        for (uint16_t glyphX = 0; glyphX < glyph->width && x + glyphX < clipRight; ++glyphX) {
            const uint32_t alphaIndex = static_cast<uint32_t>(glyphY) * glyph->width + glyphX;
            const uint8_t packed = glyph->bitmap[alphaIndex / 2U];
            const uint8_t alpha = (alphaIndex & 1U) ? (packed & 0x0FU) : (packed >> 4);
            // Use only the strongest half of PuHui's 4-bit coverage values.
            // This removes the antialiased edge pixels that looked too bold on
            // the monochrome e-paper waveform while keeping the glyph geometry.
            if (alpha >= 11) pixel(frame, x + glyphX, y + glyphY);
        }
    }
}

void drawUtf8Title(uint8_t *frame, int x, int y, int width, int height, const char *value) {
    const int right = x + width;
    const int bottom = y + height;
    const char *cursor = value;
    uint32_t codepoint = 0;
    int drawX = x;
    while (nextUtf8Codepoint(cursor, codepoint) && drawX < right) {
        if (codepoint < 0x80) {
            char text[2] = {static_cast<char>(codepoint), '\0'};
            UiLocalization::drawText(frame, drawX, y + (height - 7) / 2, text, 1);
            drawX += 7;
            continue;
        }
        const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint);
        if (!glyph) {
            drawX += 17;
            continue;
        }
        const int advance = max(1, static_cast<int>(glyph->advance)) + 1;
        if (drawX + advance > right) break;
        constexpr int FONT_LINE_HEIGHT = 25;
        constexpr int FONT_BASELINE = 6;
        const int fontTop = y + (height - FONT_LINE_HEIGHT) / 2;
        const int glyphTop = fontTop + FONT_LINE_HEIGHT - FONT_BASELINE -
                             glyph->height - glyph->offsetY;
        drawChineseGlyph(frame, glyph, drawX + glyph->offsetX,
                         glyphTop, right, bottom);
        drawX += advance;
    }
}

void copyUtf8(char *destination, size_t destinationSize, const char *source, const char *fallback) {
    if (!destination || destinationSize == 0) return;
    const char *value = source && source[0] ? source : fallback;
    value = value ? value : "";
    size_t input = 0;
    size_t output = 0;
    while (value[input] != '\0' && output + 1 < destinationSize) {
        const uint8_t first = static_cast<uint8_t>(value[input]);
        size_t sequenceLength = 1;
        if ((first & 0xE0U) == 0xC0U) sequenceLength = 2;
        else if ((first & 0xF0U) == 0xE0U) sequenceLength = 3;
        else if ((first & 0xF8U) == 0xF0U) sequenceLength = 4;
        bool complete = true;
        for (size_t byte = 1; byte < sequenceLength; ++byte) {
            if (value[input + byte] == '\0' ||
                (static_cast<uint8_t>(value[input + byte]) & 0xC0U) != 0x80U) {
                complete = false;
                break;
            }
        }
        if (!complete || output + sequenceLength >= destinationSize) break;
        for (size_t byte = 0; byte < sequenceLength; ++byte) {
            destination[output++] = value[input++];
        }
    }
    destination[output] = '\0';
}

int codepointAdvance(uint32_t codepoint) {
    if (codepoint == '\t') return 28;
    if (codepoint < 0x80) return codepoint == ' ' ? 5 : 7;
    const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint);
    return glyph ? max(1, static_cast<int>(glyph->advance)) + 1 : 18;
}

void updateReaderPagination() {
    readerPageTotal = 1;
    readerPageOffsets[0] = 0;
    const char *base = selectedContent.c_str();
    const char *cursor = base;
    int line = 0;
    int lineWidth = 0;
    while (*cursor != '\0') {
        const char *codepointStart = cursor;
        uint32_t codepoint = 0;
        if (!nextUtf8Codepoint(cursor, codepoint)) break;
        if (codepoint == '\r') continue;
        if (codepoint == '\n') {
            lineWidth = 0;
            if (++line >= READER_LINES_PER_PAGE && readerPageTotal < MAX_READER_PAGES) {
                readerPageOffsets[readerPageTotal++] = static_cast<uint32_t>(cursor - base);
                line = 0;
            }
            continue;
        }

        const int advance = codepointAdvance(codepoint);
        if (lineWidth > 0 && lineWidth + advance > READER_CONTENT_WIDTH) {
            lineWidth = 0;
            if (++line >= READER_LINES_PER_PAGE && readerPageTotal < MAX_READER_PAGES) {
                readerPageOffsets[readerPageTotal++] =
                    static_cast<uint32_t>(codepointStart - base);
                line = 0;
            }
        }
        lineWidth += advance;
    }
    readerPageOffsets[readerPageTotal] = selectedContent.length();
    if (readerPage < 0) readerPage = 0;
    if (readerPage >= readerPageTotal) readerPage = readerPageTotal - 1;
    Serial.printf("[POEM] Reader pagination bytes=%u pages=%u lines_per_page=%d\n",
                  selectedContent.length(), readerPageTotal, READER_LINES_PER_PAGE);
}

bool ensurePoemDirectory(int32_t poemId, char *directory, size_t directorySize) {
    if (!SdCard::isMounted() || poemId <= 0 || !directory || directorySize == 0) return false;
    if (!SD_MMC.exists(POEM_SD_FOLDER) && !SD_MMC.mkdir(POEM_SD_FOLDER)) return false;
    snprintf(directory, directorySize, "%s/%ld", POEM_SD_FOLDER, static_cast<long>(poemId));
    return SD_MMC.exists(directory) || SD_MMC.mkdir(directory);
}

bool isPoemSavedOnSd(int32_t poemId) {
    if (!SdCard::isMounted() || poemId <= 0) return false;
    char audioPath[96] = {};
    return cachedOpusPath(poemId, audioPath, sizeof(audioPath));
}

void savePoemMetadata(int32_t poemId, const char *title) {
    char directory[40] = {};
    if (!ensurePoemDirectory(poemId, directory, sizeof(directory))) return;
    char metaPath[64] = {};
    snprintf(metaPath, sizeof(metaPath), "%s/meta.txt", directory);
    SD_MMC.remove(metaPath);
    File meta = SD_MMC.open(metaPath, FILE_WRITE);
    if (!meta) return;
    meta.printf("%ld\n%s\n\n\n", static_cast<long>(poemId), title ? title : "");
    meta.close();
}

bool savePoemToSd(int32_t poemId, const char *title, const char *author,
                  const char *dynasty, const String &content) {
    char directory[40] = {};
    if (!ensurePoemDirectory(poemId, directory, sizeof(directory))) {
        Serial.printf("[POEM SD] Could not create cache folder for id=%ld\n", static_cast<long>(poemId));
        return false;
    }

    char contentPath[64] = {};
    char temporaryPath[72] = {};
    snprintf(contentPath, sizeof(contentPath), "%s/content.txt", directory);
    snprintf(temporaryPath, sizeof(temporaryPath), "%s/content.part", directory);
    SD_MMC.remove(temporaryPath);
    File contentFile = SD_MMC.open(temporaryPath, FILE_WRITE);
    if (!contentFile) return false;
    const size_t written = contentFile.print(content);
    contentFile.flush();
    contentFile.close();
    if (written != content.length()) {
        SD_MMC.remove(temporaryPath);
        Serial.printf("[POEM SD] Short write id=%ld expected=%u written=%u\n",
                      static_cast<long>(poemId), content.length(), written);
        return false;
    }
    SD_MMC.remove(contentPath);
    if (!SD_MMC.rename(temporaryPath, contentPath)) {
        SD_MMC.remove(temporaryPath);
        return false;
    }

    char metaPath[64] = {};
    snprintf(metaPath, sizeof(metaPath), "%s/meta.txt", directory);
    SD_MMC.remove(metaPath);
    File metaFile = SD_MMC.open(metaPath, FILE_WRITE);
    if (!metaFile) return false;
    metaFile.printf("%ld\n%s\n%s\n%s\n", static_cast<long>(poemId),
                    title ? title : "", author ? author : "", dynasty ? dynasty : "");
    metaFile.flush();
    metaFile.close();
    Serial.printf("[POEM SD] Saved id=%ld bytes=%u folder=%s\n",
                  static_cast<long>(poemId), content.length(), directory);
    return true;
}

bool loadPoemFromSd(const PoemItem &poem) {
    if (!SdCard::isMounted() || poem.id <= 0) return false;
    char directory[40] = {};
    snprintf(directory, sizeof(directory), "%s/%ld", POEM_SD_FOLDER, static_cast<long>(poem.id));
    char metaPath[64] = {};
    char contentPath[64] = {};
    snprintf(metaPath, sizeof(metaPath), "%s/meta.txt", directory);
    snprintf(contentPath, sizeof(contentPath), "%s/content.txt", directory);
    File metaFile = SD_MMC.open(metaPath, FILE_READ);
    File contentFile = SD_MMC.open(contentPath, FILE_READ);
    if (!metaFile || !contentFile) {
        if (metaFile) metaFile.close();
        if (contentFile) contentFile.close();
        return false;
    }
    metaFile.readStringUntil('\n');
    const String title = metaFile.readStringUntil('\n');
    const String author = metaFile.readStringUntil('\n');
    const String dynasty = metaFile.readStringUntil('\n');
    metaFile.close();
    copyUtf8(selectedTitle, sizeof(selectedTitle), title.c_str(), poem.title);
    copyUtf8(selectedAuthor, sizeof(selectedAuthor), author.c_str(), "");
    copyUtf8(selectedDynasty, sizeof(selectedDynasty), dynasty.c_str(), "");
    selectedContent = contentFile.readString();
    contentFile.close();
    if (selectedContent.isEmpty()) return false;
    selectedPoemId = poem.id;
    readerPage = 0;
    updateReaderPagination();
    view = View::Player;
    Serial.printf("[POEM SD] Loaded id=%ld bytes=%u from %s\n",
                  static_cast<long>(poem.id), selectedContent.length(), directory);
    return true;
}

void asciiCopy(char *destination, size_t size, const String &source, const char *fallback) {
    if (!destination || size == 0) return;
    size_t output = 0;
    for (size_t index = 0; index < source.length() && output + 1 < size;) {
        const uint8_t value = static_cast<uint8_t>(source[index]);
        if (value < 0x80) {
            char character = static_cast<char>(value);
            if (character == '\r' || character == '\n' || character == '\t') character = ' ';
            if (character >= 32 && character <= 126) destination[output++] = character;
            ++index;
        } else {
            const size_t sequence = (value & 0xE0) == 0xC0 ? 2 : (value & 0xF0) == 0xE0 ? 3 : 4;
            index += sequence;
            destination[output++] = '?';
        }
    }
    while (output > 0 && destination[output - 1] == ' ') --output;
    destination[output] = '\0';
    if (output == 0 && fallback) {
        std::strncpy(destination, fallback, size - 1);
        destination[size - 1] = '\0';
    }
}

String normalizeContent(const String &source) {
    String result;
    result.reserve(source.length());
    bool previousSpace = true;
    for (size_t index = 0; index < source.length();) {
        const uint8_t value = static_cast<uint8_t>(source[index]);
        char output = 0;
        if (value < 0x80) {
            output = static_cast<char>(value);
            ++index;
        } else {
            const size_t sequence = (value & 0xE0) == 0xC0 ? 2 : (value & 0xF0) == 0xE0 ? 3 : 4;
            index += sequence;
            output = '?';
        }
        if (output == '\r') continue;
        if (output == '\n' || output == '\t') output = ' ';
        if (output < 32 || output > 126) continue;
        if (output == ' ' && previousSpace) continue;
        result += output;
        previousSpace = output == ' ';
        if (result.length() >= 12000) break;
    }
    result.trim();
    return result;
}

String endpointBase() {
    String base(contentBaseUrl);
    base.trim();
    while (base.endsWith("/")) base.remove(base.length() - 1);
    const int query = base.indexOf('?');
    if (query >= 0) base.remove(query);
    const int stories = base.indexOf("/api/poems");
    if (stories >= 0) return base.substring(0, stories) + "/api/poems";
    const int scheme = base.indexOf("://");
    if (scheme >= 0) {
        const int path = base.indexOf('/', scheme + 3);
        if (path >= 0) base.remove(path);
    }
    return base + "/api/poems";
}

bool httpGet(const String &url, String &payload, uint32_t timeoutMs = 20000,
             bool showLoading = true) {
    OptionalLoadingScope loadingIndicator(showLoading);
    Serial.printf("[POEM API] request method=GET url=%s wifi_status=%d ip=%s gateway=%s dns=%s heap=%u\n",
                  url.c_str(), static_cast<int>(WiFi.status()),
                  WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
                  WiFi.dnsIP().toString().c_str(), static_cast<unsigned>(ESP.getFreeHeap()));
    if (WiFi.status() != WL_CONNECTED) {
        std::strcpy(statusText, UiLocalization::isChinese() ? "网络未连接" : "WIFI NOT CONNECTED");
        Serial.println("[POEM] WiFi is not connected");
        return false;
    }

    HTTPClient http;
    http.setConnectTimeout(7000);
    http.setTimeout(timeoutMs);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    plainClient.setTimeout(20);
    secureClient.setTimeout(20);
    bool began = false;
    if (url.startsWith("https://")) {
        secureClient.setInsecure();
        began = http.begin(secureClient, url);
    } else {
        began = http.begin(plainClient, url);
    }
    if (!began) {
        std::strcpy(statusText, "INVALID CONTENT URL");
        Serial.printf("[POEM] Invalid URL: %s\n", url.c_str());
        return false;
    }
    http.addHeader("Connection", "close");
    http.addHeader("User-Agent", "ESP32-ePaper-Poem/1.0");
    const int code = http.GET();
    if (code >= 200 && code < 300) payload = http.getString();
    const String error = code < 0 ? http.errorToString(code) : String();
    http.end();
    Serial.printf("[POEM API] response code=%d%s%s bytes=%u url=%s\n", code,
                  error.isEmpty() ? "" : " ", error.c_str(), payload.length(), url.c_str());
    if (code < 0) {
        if (UiLocalization::isChinese()) std::strcpy(statusText, "网络错误");
        else snprintf(statusText, sizeof(statusText), "HTTP ERROR %d", code);
    }
    else if (code < 200 || code >= 300) snprintf(statusText, sizeof(statusText), "SERVER HTTP %d", code);
    return code >= 200 && code < 300;
}

String contentHostBase() {
    String base(contentBaseUrl);
    base.trim();
    while (base.endsWith("/")) base.remove(base.length() - 1);
    const int query = base.indexOf('?');
    if (query >= 0) base.remove(query);
    const int scheme = base.indexOf("://");
    if (scheme >= 0) {
        const int path = base.indexOf('/', scheme + 3);
        if (path >= 0) base.remove(path);
    }
    return base;
}

String absoluteAudioUrl(const char *value) {
    String url(value ? value : "");
    url.trim();
    if (url.startsWith("http://") || url.startsWith("https://")) return url;
    const String host = contentHostBase();
    if (url.startsWith("/")) return host + url;
    return host + "/" + url;
}

void safeVoiceName(char *output, size_t outputSize) {
    if (!output || outputSize == 0) return;
    size_t length = 0;
    for (size_t index = 0; selectedVoice[index] && length + 1 < outputSize; ++index) {
        const char character = selectedVoice[index];
        output[length++] = isalnum(static_cast<unsigned char>(character)) ? character : '_';
    }
    if (length == 0 && outputSize > 1) output[length++] = 'v';
    output[length] = '\0';
}

bool cachedOpusPath(int32_t poemId, char *path, size_t pathSize) {
    if (!path || pathSize == 0 || poemId <= 0 || !SdCard::isMounted()) return false;
    char voice[32] = {};
    safeVoiceName(voice, sizeof(voice));
    snprintf(path, pathSize, "%s/%ld/tts_%s.opus", POEM_SD_FOLDER,
             static_cast<long>(poemId), voice);
    if (!SD_MMC.exists(path)) return false;
    File file = SD_MMC.open(path, FILE_READ);
    uint8_t header[4] = {};
    const size_t bytes = file ? file.read(header, sizeof(header)) : 0;
    const size_t size = file ? file.size() : 0;
    if (file) file.close();
    if (bytes == sizeof(header) && size >= 1024 && std::memcmp(header, "OggS", 4) == 0) {
        return true;
    }
    SD_MMC.remove(path);
    return false;
}

bool downloadFile(const String &url, const char *path) {
    if (!path || !SdCard::isMounted() || WiFi.status() != WL_CONNECTED) return false;
    UiLoadingIndicator::Scope loadingIndicator;
    HTTPClient http;
    http.setConnectTimeout(10000);
    // Arduino HTTPClient stores this timeout as uint16_t. Keep the socket read
    // timeout at its valid maximum; the outer transfer loop still allows the
    // complete TTS download to run for as long as data continues arriving.
    http.setTimeout(65000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    secureClient.setInsecure();
    const bool began = url.startsWith("https://")
        ? http.begin(secureClient, url)
        : http.begin(plainClient, url);
    if (!began) return false;
    http.addHeader("Connection", "close");
    const int code = http.GET();
    if (code < 200 || code >= 300) {
        Serial.printf("[POEM TTS] Opus HTTP %d url=%s\n", code, url.c_str());
        http.end();
        return false;
    }

    char temporaryPath[112] = {};
    snprintf(temporaryPath, sizeof(temporaryPath), "%s.part", path);
    SD_MMC.remove(temporaryPath);
    File output = SD_MMC.open(temporaryPath, FILE_WRITE);
    if (!output) {
        http.end();
        return false;
    }
    WiFiClient *stream = http.getStreamPtr();
    uint8_t buffer[2048] = {};
    size_t total = 0;
    uint32_t lastDataMs = millis();
    while (http.connected() || stream->available()) {
        const size_t available = stream->available();
        if (available > 0) {
            const size_t requested = min(available, sizeof(buffer));
            const int received = stream->readBytes(buffer, requested);
            if (received <= 0 || output.write(buffer, received) != static_cast<size_t>(received)) {
                output.close();
                http.end();
                SD_MMC.remove(temporaryPath);
                return false;
            }
            total += received;
            lastDataMs = millis();
        } else {
            if (millis() - lastDataMs > 15000) break;
            delay(2);
        }
    }
    output.flush();
    output.close();
    http.end();
    if (total < 1024) {
        SD_MMC.remove(temporaryPath);
        return false;
    }
    SD_MMC.remove(path);
    if (!SD_MMC.rename(temporaryPath, path)) {
        SD_MMC.remove(temporaryPath);
        return false;
    }
    Serial.printf("[POEM TTS] Opus download complete bytes=%u path=%s\n", total, path);
    return true;
}

bool ensurePoemOpus(char *path, size_t pathSize) {
    if (cachedOpusPath(selectedPoemId, path, pathSize)) {
        Serial.printf("[POEM TTS] Using voice cache poemId=%ld voice=%s path=%s\n",
                      static_cast<long>(selectedPoemId), selectedVoice, path);
        return true;
    }
    if (WiFi.status() != WL_CONNECTED || !SdCard::isMounted()) {
        std::strcpy(audioStatus, UiLocalization::isChinese() ? "没有音频" : "NO AUDIO");
        return false;
    }
    char directory[40] = {};
    if (!ensurePoemDirectory(selectedPoemId, directory, sizeof(directory))) return false;

    String metadataUrl = endpointBase() + "/" + String(selectedPoemId) +
                         "/tts?voice=" + String(selectedVoice);
    Serial.printf("[POEM TTS] Metadata request poemId=%ld voice=%s url=%s\n",
                  static_cast<long>(selectedPoemId), selectedVoice, metadataUrl.c_str());
    std::strcpy(audioStatus, UiLocalization::isChinese() ? "正在生成语音" : "GENERATING AUDIO");
    String payload;
    if (!httpGet(metadataUrl, payload, 180000)) return false;
    JsonDocument document;
    const DeserializationError error = deserializeJson(document, payload);
    if (error) {
        Serial.printf("[POEM TTS] Metadata JSON failed: %s\n", error.c_str());
        return false;
    }
    const char *audioUrl = document["esp32_url"] |
                           document["absolute_audio_url"] |
                           document["audio_url"] | "";
    if (!audioUrl[0]) return false;
    const String downloadUrl = absoluteAudioUrl(audioUrl);
    char voice[32] = {};
    safeVoiceName(voice, sizeof(voice));
    snprintf(path, pathSize, "%s/tts_%s.opus", directory, voice);
    Serial.printf("[POEM TTS] Downloading voice=%s url=%s -> %s\n",
                  selectedVoice, downloadUrl.c_str(), path);
    if (!downloadFile(downloadUrl, path) || !cachedOpusPath(selectedPoemId, path, pathSize)) {
        return false;
    }
    return true;
}

bool startPoemAudio() {
    char path[96] = {};
    if (!ensurePoemOpus(path, sizeof(path))) {
        poemPlaying = false;
        std::strcpy(audioStatus, UiLocalization::isChinese() ? "语音失败" : "AUDIO FAILED");
        return false;
    }
    poemPlaying = OpusPlayer::play(path);
    std::strcpy(audioStatus, poemPlaying
        ? (UiLocalization::isChinese() ? "正在播放" : "PLAYING")
        : (UiLocalization::isChinese() ? "播放失败" : "PLAY FAILED"));
    return poemPlaying;
}

int findKey(const String &json, const char *key, int start = 0) {
    const String token = String('"') + key + '"';
    int position = json.indexOf(token, start);
    if (position < 0) return -1;
    position = json.indexOf(':', position + token.length());
    return position < 0 ? -1 : position + 1;
}

int32_t jsonInteger(const String &json, const char *key, int start, int end, int32_t fallback) {
    int position = findKey(json, key, start);
    if (position < 0 || position >= end) return fallback;
    while (position < end && isspace(static_cast<unsigned char>(json[position]))) ++position;
    return json.substring(position, end).toInt();
}

String jsonString(const String &json, const char *key, int start, int end) {
    int position = findKey(json, key, start);
    if (position < 0 || position >= end) return String();
    while (position < end && isspace(static_cast<unsigned char>(json[position]))) ++position;
    if (position >= end || json[position] != '"') return String();
    String result;
    bool escaped = false;
    for (++position; position < end; ++position) {
        const char character = json[position];
        if (escaped) {
            if (character == 'n' || character == 'r' || character == 't') result += ' ';
            else if (character == 'u') {
                result += '?';
                position += 4;
            } else result += character;
            escaped = false;
        } else if (character == '\\') escaped = true;
        else if (character == '"') break;
        else result += character;
    }
    return result;
}

int matchingBrace(const String &json, int start) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (int index = start; index < static_cast<int>(json.length()); ++index) {
        const char character = json[index];
        if (inString) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') inString = false;
            continue;
        }
        if (character == '"') inString = true;
        else if (character == '{') ++depth;
        else if (character == '}' && --depth == 0) return index + 1;
    }
    return -1;
}

bool loadLibrary(bool showLoading = true) {
    poemCount = 0;
    String payload;
    bool remoteLoaded = false;
    if (std::strlen(contentBaseUrl) > 7) {
        const String url = endpointBase() + "?page=" + String(libraryPage) +
                           "&perPage=" + String(ITEMS_PER_PAGE);
        remoteLoaded = httpGet(url, payload, 20000, showLoading);
    }
    if (!remoteLoaded) {
        // The remote library is optional once stories have been cached. Build a
        // local page directly from /voice/<id>/meta.txt + content.txt.
        if (!SdCard::isMounted()) return false;
        File root = SD_MMC.open(POEM_SD_FOLDER);
        if (!root || !root.isDirectory()) return false;
        const int32_t firstItem = (libraryPage - 1) * ITEMS_PER_PAGE;
        int32_t validIndex = 0;
        poemTotal = 0;
        File entry = root.openNextFile();
        while (entry) {
            if (entry.isDirectory()) {
                String directory = entry.name();
                const int slash = directory.lastIndexOf('/');
                const int32_t id = directory.substring(slash + 1).toInt();
                if (isPoemSavedOnSd(id)) {
                    if (validIndex >= firstItem && poemCount < ITEMS_PER_PAGE) {
                        PoemItem &poem = stories[poemCount];
                        poem.id = id;
                        poem.saved = true;
                        char metaPath[64] = {};
                        snprintf(metaPath, sizeof(metaPath), "%s/%ld/meta.txt",
                                 POEM_SD_FOLDER, static_cast<long>(id));
                        File meta = SD_MMC.open(metaPath, FILE_READ);
                        if (meta) {
                            meta.readStringUntil('\n');
                            const String title = meta.readStringUntil('\n');
                            meta.close();
                            char fallback[24] = {};
                            snprintf(fallback, sizeof(fallback), "POEM %ld", static_cast<long>(id));
                            copyUtf8(poem.title, sizeof(poem.title), title.c_str(), fallback);
                            ++poemCount;
                        }
                    }
                    ++validIndex;
                    ++poemTotal;
                }
            }
            entry.close();
            entry = root.openNextFile();
        }
        root.close();
        if (poemCount > 0) {
            snprintf(statusText, sizeof(statusText), "%u SAVED STORIES", poemCount);
            Serial.printf("[POEM SD] Offline library page=%ld items=%u total=%ld\n",
                          static_cast<long>(libraryPage), poemCount,
                          static_cast<long>(poemTotal));
            return true;
        }
        std::strcpy(statusText, "NO ONLINE OR SAVED STORIES");
        return false;
    }
    Serial.printf("[POEM] Library payload preview: %.180s\n", payload.c_str());
    JsonDocument document;
    const DeserializationError error = deserializeJson(document, payload);
    if (error) {
        snprintf(statusText, sizeof(statusText), "JSON %s", error.c_str());
        Serial.printf("[POEM API] JSON parse failed: %s\n", error.c_str());
        return false;
    }
    poemTotal = document["total"] | 0;
    JsonArray items = document["items"].as<JsonArray>();
    if (items.isNull()) {
        std::strcpy(statusText, "NO ITEMS IN JSON");
        Serial.println("[POEM API] Parsed JSON has no items array");
        return false;
    }
    for (JsonObject item : items) {
        if (poemCount >= ITEMS_PER_PAGE) break;
        PoemItem &poem = stories[poemCount];
        poem.id = item["id"] | 0;
        char fallback[24] = {};
        snprintf(fallback, sizeof(fallback), "POEM %ld", static_cast<long>(poem.id));
        copyUtf8(poem.title, sizeof(poem.title), item["title"] | "", fallback);
        poem.saved = isPoemSavedOnSd(poem.id);
        Serial.printf("[POEM API] parsed item index=%u id=%ld title=%s\n",
                      poemCount, static_cast<long>(poem.id), poem.title);
        if (poem.id > 0) ++poemCount;
    }
    if (poemCount == 0) {
        std::strcpy(statusText, "NO STORIES FOUND");
        return false;
    }
    snprintf(statusText, sizeof(statusText), "%u STORIES", poemCount);
    return true;
}

void libraryLoadTask(void *) {
    loadLibrary(false);
    libraryAwaitingContent = false;
    libraryLoadRunning = false;
    libraryLoadCompleted = true;
    vTaskDelete(nullptr);
}

bool loadPoem(const PoemItem &poem) {
    if (loadPoemFromSd(poem)) return true;

    String payload;
    bool loaded = false;
    const String url = endpointBase() + "/" + String(poem.id);
    for (uint8_t attempt = 1; attempt <= 3 && !loaded; ++attempt) {
        Serial.printf("[POEM] Detail fetch id=%ld attempt=%u/3\n",
                      static_cast<long>(poem.id), attempt);
        loaded = httpGet(url, payload);
        if (!loaded && attempt < 3) delay(800);
    }
    if (!loaded) {
        std::strcpy(statusText, "POEM LOAD FAILED");
        return false;
    }
    JsonDocument document;
    const DeserializationError error = deserializeJson(document, payload);
    if (error) {
        Serial.printf("[POEM API] Detail JSON parse failed: %s\n", error.c_str());
        std::strcpy(statusText, "POEM JSON FAILED");
        return false;
    }
    selectedPoemId = document["id"] | poem.id;
    copyUtf8(selectedTitle, sizeof(selectedTitle), document["title"] | "", poem.title);
    copyUtf8(selectedAuthor, sizeof(selectedAuthor), document["author"] | "", "");
    copyUtf8(selectedDynasty, sizeof(selectedDynasty), document["dynasty"] | "", "");
    selectedContent = document["content"] | "";
    if (selectedContent.isEmpty()) {
        std::strcpy(statusText, "POEM HAS NO CONTENT");
        return false;
    }
    readerPage = 0;
    updateReaderPagination();
    view = View::Player;
    // Save is deferred until the caller has refreshed the reader display.
    pendingSave = true;
    return true;
}

int libraryPageCount() {
    return poemTotal > 0 ? (poemTotal + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE : 1;
}

int readerPageCount() {
    return readerPageTotal;
}

void renderLibrary(uint8_t *frame) {
    if (UiLocalization::isChinese()) drawUtf8Title(frame, 91, 34, 58, 18, "诗词");
    else drawCentered(frame, 36, "POEMS", 2);

    rect(frame, 4, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT);
    drawDoubleArrow(frame, 21, PAGER_TOP + PAGER_HEIGHT / 2, false);
    rect(frame, 42, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT);
    drawArrow(frame, 59, PAGER_TOP + PAGER_HEIGHT / 2, false);
    rect(frame, 164, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT);
    drawArrow(frame, 181, PAGER_TOP + PAGER_HEIGHT / 2, true);
    rect(frame, 202, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT);
    drawDoubleArrow(frame, 219, PAGER_TOP + PAGER_HEIGHT / 2, true);
    char pager[24] = {};
    if (UiLocalization::isChinese()) {
        snprintf(pager, sizeof(pager), "%ld/%d", static_cast<long>(libraryPage), libraryPageCount());
    } else {
        snprintf(pager, sizeof(pager), "PAGE %ld OF %d", static_cast<long>(libraryPage), libraryPageCount());
    }
    drawCentered(frame, 61, pager);

    if (poemCount == 0) {
        if (libraryAwaitingContent) return;
        drawCentered(frame, 190, statusText);
        drawCentered(frame, 215, UiLocalization::isChinese() ? "检查网络设置" : "CHECK WIFI AND URL");
        return;
    }
    for (uint8_t index = 0; index < poemCount; ++index) {
        const int top = LIST_TOP + index * (ROW_HEIGHT + ROW_GAP);
        if (index == activePoemIndex) boldRect(frame, 12, top, 216, ROW_HEIGHT);
        else rect(frame, 12, top, 216, ROW_HEIGHT);
        char number[8] = {};
        snprintf(number, sizeof(number), "%u", static_cast<unsigned>((libraryPage - 1) * ITEMS_PER_PAGE + index + 1));
        UiLocalization::drawText(frame, 18, top + 9, number, 1);
        drawUtf8Title(frame, 34, top + 1, 171, ROW_HEIGHT - 2, stories[index].title);
        if (index == activePoemIndex) {
            drawPlayPause(frame, 215, top + ROW_HEIGHT / 2, true);
            if (!marqueeReady) captureMarquee(frame, top);
        } else if (stories[index].saved) drawCheckmark(frame, 215, top + ROW_HEIGHT / 2);
        else drawArrow(frame, 215, top + ROW_HEIGHT / 2, true);
    }
}

void renderPoemPopup(uint8_t *frame) {
    // Keep the playlist underneath and cover only its first five rows. This
    // makes the reader feel like a window rather than a page transition.
    for (int y = POPUP_Y; y < POPUP_Y + POPUP_H; ++y) {
        for (int x = POPUP_X; x < POPUP_X + POPUP_W; ++x) {
            frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] &=
                static_cast<uint8_t>(~(0x80U >> (x % 8)));
        }
    }
    boldRect(frame, POPUP_X, POPUP_Y, POPUP_W, POPUP_H);
    rect(frame, POPUP_X + 7, POPUP_Y + 7, 42, 24);
    const char *backLabel = UiLocalization::isChinese() ? "返回" : "BACK";
    const int backWidth = UiLocalization::textWidth(backLabel, 1);
    UiLocalization::drawText(frame, POPUP_X + 7 + (42 - backWidth) / 2,
                             POPUP_Y + 15, backLabel);
    drawUtf8Title(frame, POPUP_X + 56, POPUP_Y + 5, 112, 28, selectedTitle);
    drawPlayPause(frame, POPUP_X + POPUP_W - 18, POPUP_Y + 19, poemPlaying);

    char byline[132] = {};
    if (selectedDynasty[0] && selectedAuthor[0]) {
        snprintf(byline, sizeof(byline), "%s · %s", selectedDynasty, selectedAuthor);
    } else {
        snprintf(byline, sizeof(byline), "%s%s", selectedDynasty, selectedAuthor);
    }
    drawUtf8Title(frame, POPUP_CONTENT_X, POPUP_Y + 34, POPUP_CONTENT_W, 18, byline);
    line(frame, POPUP_X + 7, POPUP_Y + 51, POPUP_X + POPUP_W - 8, POPUP_Y + 51);

    const char *base = selectedContent.c_str();
    const char *cursor = base + readerPageOffsets[readerPage];
    const char *end = base + readerPageOffsets[readerPage + 1];
    int drawY = POPUP_CONTENT_Y;
    int drawX = POPUP_CONTENT_X;
    while (cursor < end && drawY + READER_LINE_HEIGHT <= POPUP_CONTENT_BOTTOM) {
        const char *codepointStart = cursor;
        uint32_t codepoint = 0;
        if (!nextUtf8Codepoint(cursor, codepoint)) break;
        if (codepoint == '\r') continue;
        if (codepoint == '\n') {
            drawX = POPUP_CONTENT_X;
            drawY += READER_LINE_HEIGHT;
            continue;
        }
        const int advance = codepoint < 0x80 ? 6 : codepointAdvance(codepoint);
        if (drawX > POPUP_CONTENT_X && drawX + advance > POPUP_CONTENT_X + POPUP_CONTENT_W) {
            cursor = codepointStart;
            drawX = POPUP_CONTENT_X;
            drawY += READER_LINE_HEIGHT;
            continue;
        }
        if (codepoint < 0x80) {
            char text[2] = {static_cast<char>(codepoint), '\0'};
            UiLocalization::drawText(frame, drawX, drawY + 7, text, 1);
        } else if (const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint)) {
            constexpr int baseline = 6;
            const int glyphTop = drawY + READER_LINE_HEIGHT - baseline -
                                 glyph->height - glyph->offsetY;
            drawChineseGlyph(frame, glyph, drawX + glyph->offsetX, glyphTop,
                             POPUP_CONTENT_X + POPUP_CONTENT_W, POPUP_CONTENT_BOTTOM);
        }
        drawX += advance;
    }

    if (readerPageCount() > 1) {
        char pager[16] = {};
        snprintf(pager, sizeof(pager), "%d/%d", readerPage + 1, readerPageCount());
        drawCentered(frame, POPUP_Y + POPUP_H - 8, pager);
    }
}

void renderPlayer(uint8_t *frame) {
    constexpr int popupX = 8;
    constexpr int popupY = 38;
    constexpr int popupW = 224;
    constexpr int popupH = 368;
    boldRect(frame, popupX, popupY, popupW, popupH);

    rect(frame, 14, 44, 48, 26);
    UiLocalization::drawText(frame, UiLocalization::isChinese() ? 20 : 19, 53,
                             UiLocalization::isChinese() ? "返回" : "BACK");
    drawUtf8Title(frame, 68, 43, 154, 28, selectedTitle);

    char byline[132] = {};
    if (selectedDynasty[0] && selectedAuthor[0]) {
        snprintf(byline, sizeof(byline), "%s · %s", selectedDynasty, selectedAuthor);
    } else {
        snprintf(byline, sizeof(byline), "%s%s", selectedDynasty, selectedAuthor);
    }
    drawUtf8Title(frame, 16, 74, 208, 24, byline);
    line(frame, 14, 102, 225, 102);

    const char *base = selectedContent.c_str();
    const char *cursor = base + readerPageOffsets[readerPage];
    const char *end = base + readerPageOffsets[readerPage + 1];
    int drawY = READER_CONTENT_Y;
    int drawX = READER_CONTENT_X;
    while (cursor < end && drawY + READER_LINE_HEIGHT <= READER_CONTENT_BOTTOM) {
        const char *codepointStart = cursor;
        uint32_t codepoint = 0;
        if (!nextUtf8Codepoint(cursor, codepoint)) break;
        if (codepoint == '\r') continue;
        if (codepoint == '\n') {
            drawX = READER_CONTENT_X;
            drawY += READER_LINE_HEIGHT;
            continue;
        }
        const int advance = codepointAdvance(codepoint);
        if (drawX > READER_CONTENT_X &&
            drawX + advance > READER_CONTENT_X + READER_CONTENT_WIDTH) {
            cursor = codepointStart;
            drawX = READER_CONTENT_X;
            drawY += READER_LINE_HEIGHT;
            continue;
        }
        if (codepoint < 0x80) {
            char text[2] = {static_cast<char>(codepoint), '\0'};
            UiLocalization::drawText(frame, drawX, drawY + 8, text, 1);
        } else {
            const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint);
            if (glyph) {
                constexpr int baseline = 6;
                const int glyphTop = drawY + READER_LINE_HEIGHT - baseline -
                                     glyph->height - glyph->offsetY;
                drawChineseGlyph(frame, glyph, drawX + glyph->offsetX, glyphTop,
                                 READER_CONTENT_X + READER_CONTENT_WIDTH,
                                 READER_CONTENT_BOTTOM);
            }
        }
        drawX += advance;
    }

    rect(frame, 14, 370, 44, 26);
    drawArrow(frame, 36, 383, false);
    rect(frame, 182, 370, 44, 26);
    drawArrow(frame, 204, 383, true);
    char pager[20] = {};
    snprintf(pager, sizeof(pager), "%d/%d", readerPage + 1, readerPageCount());
    drawCentered(frame, 380, pager);

    if (poemPlaying) drawPlayPause(frame, 218, 56, true);
}

}

namespace PoemPage {

void setContentUrl(const char *url) {
    std::strncpy(contentBaseUrl, url ? url : "", sizeof(contentBaseUrl) - 1);
    contentBaseUrl[sizeof(contentBaseUrl) - 1] = '\0';
}

void setVoice(const char *voice) {
    copyUtf8(selectedVoice, sizeof(selectedVoice), voice, "Jasper");
}

void setAudio(Es8311 *audio) { OpusPlayer::setAudio(audio); }

void openLibrary() {
    stopAudio();
    dirtyRowA = -1;
    dirtyRowB = -1;
    view = View::Library;
    poemPopupOpen = false;
    libraryPage = 1;
    std::strcpy(statusText, UiLocalization::isChinese() ? "正在获取诗词" : "LOADING POEMS");
    poemCount = 0;
    poemTotal = 0;
    libraryAwaitingContent = true;
    libraryLoadCompleted = false;
}

bool startLibraryLoad() {
    if (libraryLoadRunning) return false;
    libraryLoadRunning = true;
    libraryLoadCompleted = false;
    if (xTaskCreate(libraryLoadTask, "poem-library", 8192, nullptr, 1, nullptr) != pdPASS) {
        libraryLoadRunning = false;
        libraryAwaitingContent = false;
        std::strcpy(statusText, "POEM TASK FAILED");
        libraryLoadCompleted = true;
        return false;
    }
    return true;
}

bool takeLibraryLoadCompleted() {
    if (!libraryLoadCompleted) return false;
    libraryLoadCompleted = false;
    return true;
}

void processPendingSave() {
    if (!pendingSave) return;
    pendingSave = false;
    savePoemToSd(selectedPoemId, selectedTitle, selectedAuthor,
                 selectedDynasty, selectedContent);
}

bool handleTap(int16_t x, int16_t y) {
    if (poemPopupOpen) {
        if (pointInRect(x, y, POPUP_X + 3, POPUP_Y + 3, 50, 32)) {
            stopAudio();
            poemPopupOpen = false;
            selectedContent = "";
            readerPage = 0;
            return true;
        }
        if (pointInRect(x, y, POPUP_X + POPUP_W - 34, POPUP_Y + 5, 28, 28)) {
            pendingAudioStart = true;
            return true;
        }
        if (pointInRect(x, y, 14, 370, 44, 26) && readerPage > 0) {
            --readerPage;
            return true;
        }
        if (pointInRect(x, y, 182, 370, 44, 26) &&
            readerPage + 1 < readerPageCount()) {
            ++readerPage;
            return true;
        }
        return false;
    }

    if (view == View::Library) {
        if (pointInRect(x, y, 4, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT) &&
            libraryPage > 1) {
            stopAudio();
            libraryPage = 1;
            loadLibrary();
            return true;
        }
        if (pointInRect(x, y, 42, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT) &&
            libraryPage > 1) {
            stopAudio();
            --libraryPage;
            loadLibrary();
            return true;
        }
        if (pointInRect(x, y, 164, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT) &&
            libraryPage < libraryPageCount()) {
            stopAudio();
            ++libraryPage;
            loadLibrary();
            return true;
        }
        if (pointInRect(x, y, 202, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT) &&
            libraryPage < libraryPageCount()) {
            stopAudio();
            libraryPage = libraryPageCount();
            loadLibrary();
            return true;
        }
        for (uint8_t index = 0; index < poemCount; ++index) {
            const int top = LIST_TOP + index * (ROW_HEIGHT + ROW_GAP);
            if (pointInRect(x, y, 12, top, 216, ROW_HEIGHT)) {
                stopAudio();
                std::strcpy(audioStatus, UiLocalization::isChinese() ? "正在准备语音" : "PREPARING AUDIO");
                if (!loadPoem(stories[index])) return true;
                view = View::Library;
                poemPopupOpen = true;
                activePoemIndex = -1;
                marqueeReady = false;
                marqueeOffset = 0;
                // The main loop starts audio only after this reader popup has
                // been physically refreshed, avoiding SD/font/audio races.
                pendingAudioStart = false;
                return true;
            }
        }
        return false;
    }

    return false;
}

bool isPopupOpen() {
    return poemPopupOpen;
}

bool processAudio() {
    if (pendingAudioStart) {
        pendingAudioStart = false;
        if (!startPoemAudio()) {
            activePoemIndex = -1;
            marqueeReady = false;
            return true;
        }
        return false;
    }
    if (!poemPlaying) return false;
    if (OpusPlayer::loop()) return false;
    poemPlaying = false;
    activePoemIndex = -1;
    marqueeReady = false;
    std::strcpy(audioStatus, UiLocalization::isChinese() ? "播放结束" : "FINISHED");
    return true;
}

bool isAudioActive() {
    return pendingAudioStart || poemPlaying || OpusPlayer::isPlaying();
}

void stopAudioFromTouchInterrupt() {
    stopAudio();
    std::strcpy(audioStatus, UiLocalization::isChinese() ? "已停止" : "STOPPED");
}

bool takeDirtyRows(int8_t &firstRow, int8_t &secondRow) {
    firstRow = dirtyRowA;
    secondRow = dirtyRowB;
    dirtyRowA = -1;
    dirtyRowB = -1;
    return firstRow >= 0;
}

bool advanceMarquee(int16_t &rowTop) {
    if (!poemPlaying || activePoemIndex < 0 || !marqueeReady || millis() < nextMarqueeMs) {
        return false;
    }
    marqueeOffset = (marqueeOffset + 12) % MARQUEE_WIDTH;
    nextMarqueeMs = millis() + 900;
    rowTop = LIST_TOP + activePoemIndex * (ROW_HEIGHT + ROW_GAP);
    return true;
}

void renderMarquee(uint8_t *destination, const uint8_t *currentFrame) {
    if (!destination || !currentFrame) return;
    std::memcpy(destination, currentFrame, XingtaiEpd::FRAME_BYTES);
    if (activePoemIndex < 0 || !marqueeReady) return;
    const int top = LIST_TOP + activePoemIndex * (ROW_HEIGHT + ROW_GAP);
    clearFrameArea(destination, MARQUEE_X, top + 1, MARQUEE_WIDTH, MARQUEE_HEIGHT);
    for (int y = 0; y < MARQUEE_HEIGHT; ++y) {
        for (int x = 0; x < MARQUEE_WIDTH; ++x) {
            const int sourceX = (x + marqueeOffset) % MARQUEE_WIDTH;
            if (marqueePixel(sourceX, y)) pixel(destination, MARQUEE_X + x, top + 1 + y);
        }
    }
}

void stopAudio() {
    pendingAudioStart = false;
    OpusPlayer::stop();
    poemPlaying = false;
    activePoemIndex = -1;
    marqueeReady = false;
    marqueeOffset = 0;
}

bool handleSwipe(int16_t deltaX, int16_t deltaY) {
    constexpr int16_t SWIPE_THRESHOLD = 35;
    const bool horizontal = abs(deltaX) >= abs(deltaY);
    const int16_t primaryDelta = horizontal ? deltaX : deltaY;
    if (abs(primaryDelta) < SWIPE_THRESHOLD) return false;

    // Natural page movement: sweeping content left/up advances; sweeping it
    // right/down goes back. Both axes are supported for one-handed use.
    const bool next = primaryDelta < 0;
    if (poemPopupOpen) {
        if (next && readerPage + 1 < readerPageCount()) {
            ++readerPage;
            return true;
        }
        if (!next && readerPage > 0) {
            --readerPage;
            return true;
        }
        return false;
    }
    if (view == View::Library) {
        if (next && libraryPage < libraryPageCount()) {
            ++libraryPage;
            loadLibrary();
            Serial.printf("[POEM SWIPE] Library next page=%ld axis=%s\n",
                          static_cast<long>(libraryPage), horizontal ? "horizontal" : "vertical");
            return true;
        }
        if (!next && libraryPage > 1) {
            --libraryPage;
            loadLibrary();
            Serial.printf("[POEM SWIPE] Library previous page=%ld axis=%s\n",
                          static_cast<long>(libraryPage), horizontal ? "horizontal" : "vertical");
            return true;
        }
        return false;
    }
    return false;
}

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    if (view == View::Library) {
        renderLibrary(frame);
        if (poemPopupOpen) renderPoemPopup(frame);
    }
    else renderPlayer(frame);
}

}
