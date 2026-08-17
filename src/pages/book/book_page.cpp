#include "pages/book/book_page.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/ml307/ml307.h"
#include "devices/sd_card/sd_card.h"
#include "font/xiaozhi_font.h"
#include "pages/playlist_cache.h"
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
constexpr char BOOK_SD_FOLDER[] = "/book";
constexpr int READER_CONTENT_X = 10;
constexpr int READER_CONTENT_Y = 82;
constexpr int READER_CONTENT_WIDTH = 220;
constexpr int READER_CONTENT_BOTTOM = 378;
constexpr int READER_LINE_HEIGHT = 25;
constexpr int READER_LINES_PER_PAGE =
    (READER_CONTENT_BOTTOM - READER_CONTENT_Y) / READER_LINE_HEIGHT;
constexpr uint16_t MAX_READER_PAGES = 128;

struct BookItem {
    int32_t id = 0;
    char title[64] = {};
    bool saved = false;
};

enum class View : uint8_t { Library, Reader };

BookItem books[ITEMS_PER_PAGE] = {};
uint8_t bookCount = 0;
int32_t bookTotal = 0;
int32_t libraryPage = 1;
View view = View::Library;
char contentBaseUrl[128] = "http://";
char statusText[64] = "OPENING LIBRARY";
int32_t selectedBookId = 0;
int8_t selectedLibraryIndex = -1;
char selectedTitle[64] = {};
char selectedAuthor[64] = {};
char selectedCategory[64] = {};
String selectedContent;
int readerPage = 0;
uint32_t readerPageOffsets[MAX_READER_PAGES + 1] = {};
uint16_t readerPageTotal = 1;
bool pendingSave = false;
int8_t pendingBookOpenIndex = -1;
BookPage::ReaderControl readerControlPress = BookPage::ReaderControl::None;
bool pendingReaderBack = false;
bool readerContentRefreshRequested = false;
bool libraryContentRefreshRequested = false;
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

void invertRect(uint8_t *frame, int x, int y, int width, int height) {
    if (!frame || width <= 0 || height <= 0) return;
    const int left = max(0, x);
    const int top = max(0, y);
    const int right = min<int>(XingtaiEpd::WIDTH, x + width);
    const int bottom = min<int>(XingtaiEpd::HEIGHT, y + height);
    constexpr size_t rowBytes = XingtaiEpd::WIDTH / 8;
    for (int pixelY = top; pixelY < bottom; ++pixelY) {
        uint8_t *row = frame + static_cast<size_t>(pixelY) * rowBytes;
        for (int pixelX = left; pixelX < right; ++pixelX) {
            row[pixelX / 8] ^= 0x80U >> (pixelX % 8);
        }
    }
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
    Serial.printf("[BOOK] Reader pagination bytes=%u pages=%u lines_per_page=%d\n",
                  selectedContent.length(), readerPageTotal, READER_LINES_PER_PAGE);
}

bool ensureBookDirectory(int32_t bookId, char *directory, size_t directorySize) {
    if (!SdCard::isMounted() || bookId <= 0 || !directory || directorySize == 0) return false;
    if (!SD_MMC.exists(BOOK_SD_FOLDER) && !SD_MMC.mkdir(BOOK_SD_FOLDER)) return false;
    snprintf(directory, directorySize, "%s/%ld", BOOK_SD_FOLDER, static_cast<long>(bookId));
    return SD_MMC.exists(directory) || SD_MMC.mkdir(directory);
}

bool isBookSavedOnSd(int32_t bookId) {
    if (!SdCard::isMounted() || bookId <= 0) return false;
    char metaPath[64] = {};
    char contentPath[64] = {};
    snprintf(metaPath, sizeof(metaPath), "%s/%ld/meta.txt",
             BOOK_SD_FOLDER, static_cast<long>(bookId));
    snprintf(contentPath, sizeof(contentPath), "%s/%ld/content.txt",
             BOOK_SD_FOLDER, static_cast<long>(bookId));
    if (!SD_MMC.exists(metaPath) || !SD_MMC.exists(contentPath)) return false;
    File contentFile = SD_MMC.open(contentPath, FILE_READ);
    const bool valid = contentFile && contentFile.size() > 0;
    if (contentFile) contentFile.close();
    return valid;
}

bool saveBookToSd(int32_t bookId, const char *title, const char *author,
                  const char *category, const String &content) {
    char directory[40] = {};
    if (!ensureBookDirectory(bookId, directory, sizeof(directory))) {
        Serial.printf("[BOOK SD] Could not create cache folder for id=%ld\n", static_cast<long>(bookId));
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
        Serial.printf("[BOOK SD] Short write id=%ld expected=%u written=%u\n",
                      static_cast<long>(bookId), content.length(), written);
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
    metaFile.printf("%ld\n%s\n%s\n%s\n", static_cast<long>(bookId),
                    title ? title : "", author ? author : "", category ? category : "");
    metaFile.flush();
    metaFile.close();
    Serial.printf("[BOOK SD] Saved id=%ld bytes=%u folder=%s\n",
                  static_cast<long>(bookId), content.length(), directory);
    return true;
}

bool loadBookFromSd(const BookItem &book) {
    if (!SdCard::isMounted() || book.id <= 0) return false;
    const uint32_t loadStarted = millis();
    char directory[40] = {};
    snprintf(directory, sizeof(directory), "%s/%ld", BOOK_SD_FOLDER, static_cast<long>(book.id));
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
    const String category = metaFile.readStringUntil('\n');
    metaFile.close();
    copyUtf8(selectedTitle, sizeof(selectedTitle), title.c_str(), book.title);
    copyUtf8(selectedAuthor, sizeof(selectedAuthor), author.c_str(), "");
    copyUtf8(selectedCategory, sizeof(selectedCategory), category.c_str(), "");
    const uint32_t contentReadStarted = millis();
    selectedContent = contentFile.readString();
    contentFile.close();
    const uint32_t contentReadMs = millis() - contentReadStarted;
    if (selectedContent.isEmpty()) return false;
    selectedBookId = book.id;
    readerPage = 0;
    const uint32_t paginationStarted = millis();
    updateReaderPagination();
    const uint32_t paginationMs = millis() - paginationStarted;
    view = View::Reader;
    Serial.printf("[BOOK SD] Loaded id=%ld bytes=%u read=%lums paginate=%lums total=%lums from %s\n",
                  static_cast<long>(book.id), selectedContent.length(),
                  static_cast<unsigned long>(contentReadMs),
                  static_cast<unsigned long>(paginationMs),
                  static_cast<unsigned long>(millis() - loadStarted), directory);
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
    const int books = base.indexOf("/api/books");
    if (books >= 0) return base.substring(0, books) + "/api/books";
    const int scheme = base.indexOf("://");
    if (scheme >= 0) {
        const int path = base.indexOf('/', scheme + 3);
        if (path >= 0) base.remove(path);
    }
    return base + "/api/books";
}

bool httpGet(const String &url, String &payload, bool showLoading = true) {
    OptionalLoadingScope loadingIndicator(showLoading);
    Serial.printf("[BOOK API] request method=GET url=%s wifi_status=%d ip=%s gateway=%s dns=%s heap=%u\n",
                  url.c_str(), static_cast<int>(WiFi.status()),
                  WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
                  WiFi.dnsIP().toString().c_str(), static_cast<unsigned>(ESP.getFreeHeap()));
    if (WiFi.status() != WL_CONNECTED) {
        if (cellularModem.httpGet(url.c_str(), payload)) return true;
        std::strcpy(statusText, UiLocalization::isChinese() ? "网络未连接" : "NETWORK NOT CONNECTED");
        Serial.println("[BOOK] WiFi and ML307 are not connected");
        return false;
    }

    HTTPClient http;
    http.setConnectTimeout(7000);
    http.setTimeout(20000);
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
        Serial.printf("[BOOK] Invalid URL: %s\n", url.c_str());
        return false;
    }
    http.addHeader("Connection", "close");
    http.addHeader("User-Agent", "ESP32-ePaper-Book/1.0");
    const int code = http.GET();
    if (code >= 200 && code < 300) payload = http.getString();
    const String error = code < 0 ? http.errorToString(code) : String();
    http.end();
    Serial.printf("[BOOK API] response code=%d%s%s bytes=%u url=%s\n", code,
                  error.isEmpty() ? "" : " ", error.c_str(), payload.length(), url.c_str());
    if (code < 0) {
        if (UiLocalization::isChinese()) std::strcpy(statusText, "网络错误");
        else snprintf(statusText, sizeof(statusText), "HTTP ERROR %d", code);
    }
    else if (code < 200 || code >= 300) snprintf(statusText, sizeof(statusText), "SERVER HTTP %d", code);
    return code >= 200 && code < 300;
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

bool loadLibrary(bool showLoading = true, bool forceRemote = false,
                 bool cacheOnly = false) {
    OptionalLoadingScope loadingIndicator(showLoading);
    bookCount = 0;
    String payload;
    bool remoteLoaded = false;
    bool fetchedRemote = false;
    char cacheSlot[20] = {};
    snprintf(cacheSlot, sizeof(cacheSlot), "page-%ld", static_cast<long>(libraryPage));
    const String endpoint = endpointBase();
    if (!forceRemote) remoteLoaded = PlaylistCache::load(
        BOOK_SD_FOLDER, endpoint, cacheSlot, payload);
    if (!cacheOnly && std::strlen(contentBaseUrl) > 7) {
        const String url = endpoint + "?page=" + String(libraryPage) +
                           "&perPage=" + String(ITEMS_PER_PAGE);
        if (forceRemote || !remoteLoaded) {
            remoteLoaded = httpGet(url, payload, showLoading);
            fetchedRemote = remoteLoaded;
        }
    }
    if (!remoteLoaded && forceRemote) remoteLoaded = PlaylistCache::load(
        BOOK_SD_FOLDER, endpoint, cacheSlot, payload);
    if (!remoteLoaded) {
        // The remote library is optional once books have been cached. Build a
        // local page directly from /book/<id>/meta.txt + content.txt.
        if (!SdCard::isMounted()) return false;
        File root = SD_MMC.open(BOOK_SD_FOLDER);
        if (!root || !root.isDirectory()) return false;
        const int32_t firstItem = (libraryPage - 1) * ITEMS_PER_PAGE;
        int32_t validIndex = 0;
        bookTotal = 0;
        File entry = root.openNextFile();
        while (entry) {
            if (entry.isDirectory()) {
                String directory = entry.name();
                const int slash = directory.lastIndexOf('/');
                const int32_t id = directory.substring(slash + 1).toInt();
                if (isBookSavedOnSd(id)) {
                    if (validIndex >= firstItem && bookCount < ITEMS_PER_PAGE) {
                        BookItem &book = books[bookCount];
                        book.id = id;
                        book.saved = true;
                        char metaPath[64] = {};
                        snprintf(metaPath, sizeof(metaPath), "%s/%ld/meta.txt",
                                 BOOK_SD_FOLDER, static_cast<long>(id));
                        File meta = SD_MMC.open(metaPath, FILE_READ);
                        if (meta) {
                            meta.readStringUntil('\n');
                            const String title = meta.readStringUntil('\n');
                            meta.close();
                            char fallback[24] = {};
                            snprintf(fallback, sizeof(fallback), "BOOK %ld", static_cast<long>(id));
                            copyUtf8(book.title, sizeof(book.title), title.c_str(), fallback);
                            ++bookCount;
                        }
                    }
                    ++validIndex;
                    ++bookTotal;
                }
            }
            entry.close();
            entry = root.openNextFile();
        }
        root.close();
        if (bookCount > 0) {
            snprintf(statusText, sizeof(statusText), "%u SAVED BOOKS", bookCount);
            Serial.printf("[BOOK SD] Offline library page=%ld items=%u total=%ld\n",
                          static_cast<long>(libraryPage), bookCount,
                          static_cast<long>(bookTotal));
            return true;
        }
        std::strcpy(statusText, "NO ONLINE OR SAVED BOOKS");
        return false;
    }
    Serial.printf("[BOOK] Library payload preview: %.180s\n", payload.c_str());
    JsonDocument document;
    const DeserializationError error = deserializeJson(document, payload);
    if (error) {
        if (fetchedRemote) return loadLibrary(showLoading, false, true);
        snprintf(statusText, sizeof(statusText), "JSON %s", error.c_str());
        Serial.printf("[BOOK API] JSON parse failed: %s\n", error.c_str());
        return false;
    }
    bookTotal = document["total"] | 0;
    JsonArray items = document["items"].as<JsonArray>();
    if (items.isNull()) {
        if (fetchedRemote) return loadLibrary(showLoading, false, true);
        std::strcpy(statusText, "NO ITEMS IN JSON");
        Serial.println("[BOOK API] Parsed JSON has no items array");
        return false;
    }
    if (fetchedRemote) PlaylistCache::save(
        BOOK_SD_FOLDER, endpoint, cacheSlot, payload);
    for (JsonObject item : items) {
        if (bookCount >= ITEMS_PER_PAGE) break;
        BookItem &book = books[bookCount];
        book.id = item["id"] | 0;
        char fallback[24] = {};
        snprintf(fallback, sizeof(fallback), "BOOK %ld", static_cast<long>(book.id));
        copyUtf8(book.title, sizeof(book.title), item["title"] | "", fallback);
        book.saved = isBookSavedOnSd(book.id);
        Serial.printf("[BOOK API] parsed item index=%u id=%ld title=%s\n",
                      bookCount, static_cast<long>(book.id), book.title);
        if (book.id > 0) ++bookCount;
    }
    if (bookCount == 0) {
        std::strcpy(statusText, "NO BOOKS FOUND");
        return false;
    }
    snprintf(statusText, sizeof(statusText), "%u BOOKS", bookCount);
    return true;
}

void libraryLoadTask(void *) {
    // The main UI owns the persistent topbar LOADING state for the opening
    // request, so the worker must not invoke the display-backed indicator.
    loadLibrary(false, true);
    libraryAwaitingContent = false;
    libraryLoadRunning = false;
    libraryLoadCompleted = true;
    vTaskDelete(nullptr);
}

bool loadBook(const BookItem &book) {
    UiLoadingIndicator::Scope loadingIndicator;
    // A saved/local book must never fall through to the network. The list marks
    // it from the same /book/<id> SD cache used by loadBookFromSd().
    if (book.saved) {
        if (loadBookFromSd(book)) return true;
        std::strcpy(statusText, UiLocalization::isChinese()
                                      ? "本地书籍读取失败"
                                      : "LOCAL BOOK READ FAILED");
        Serial.printf("[BOOK SD] Refusing web fallback for saved id=%ld\n",
                      static_cast<long>(book.id));
        return false;
    }

    String payload;
    bool loaded = false;
    const String url = endpointBase() + "/" + String(book.id);
    for (uint8_t attempt = 1; attempt <= 3 && !loaded; ++attempt) {
        Serial.printf("[BOOK] Detail fetch id=%ld attempt=%u/3\n",
                      static_cast<long>(book.id), attempt);
        loaded = httpGet(url, payload);
        if (!loaded && attempt < 3) delay(800);
    }
    if (!loaded) {
        std::strcpy(statusText, "BOOK LOAD FAILED");
        return false;
    }
    JsonDocument document;
    const DeserializationError error = deserializeJson(document, payload);
    if (error) {
        Serial.printf("[BOOK API] Detail JSON parse failed: %s\n", error.c_str());
        std::strcpy(statusText, "BOOK JSON FAILED");
        return false;
    }
    selectedBookId = document["id"] | book.id;
    copyUtf8(selectedTitle, sizeof(selectedTitle), document["title"] | "", book.title);
    copyUtf8(selectedAuthor, sizeof(selectedAuthor), document["author"] | "", "");
    copyUtf8(selectedCategory, sizeof(selectedCategory), document["category"] | "", "");
    selectedContent = document["content"] | "";
    if (selectedContent.isEmpty()) {
        std::strcpy(statusText, "BOOK HAS NO CONTENT");
        return false;
    }
    readerPage = 0;
    updateReaderPagination();
    view = View::Reader;
    // Save is deferred until the caller has refreshed the reader display.
    pendingSave = true;
    return true;
}

int libraryPageCount() {
    return bookTotal > 0 ? (bookTotal + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE : 1;
}

int readerPageCount() {
    return readerPageTotal;
}

void renderLibrary(uint8_t *frame) {
    if (UiLocalization::isChinese()) drawUtf8Title(frame, 91, 34, 58, 18, "书单");
    else drawCentered(frame, 36, "BOOKLIST", 2);

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

    if (bookCount == 0) {
        if (libraryAwaitingContent) return;
        drawCentered(frame, 190, statusText);
        drawCentered(frame, 215, UiLocalization::isChinese() ? "网络设置" : "CHECK WIFI AND URL");
        return;
    }
    for (uint8_t index = 0; index < bookCount; ++index) {
        const int top = LIST_TOP + index * (ROW_HEIGHT + ROW_GAP);
        rect(frame, 12, top, 216, ROW_HEIGHT);
        char number[8] = {};
        snprintf(number, sizeof(number), "%u", static_cast<unsigned>((libraryPage - 1) * ITEMS_PER_PAGE + index + 1));
        UiLocalization::drawText(frame, 18, top + 9, number, 1);
        drawUtf8Title(frame, 34, top + 1, 171, ROW_HEIGHT - 2, books[index].title);
        if (books[index].saved) drawCheckmark(frame, 215, top + ROW_HEIGHT / 2);
        else drawArrow(frame, 215, top + ROW_HEIGHT / 2, true);
        if (index == selectedLibraryIndex) invertRect(frame, 12, top, 216, ROW_HEIGHT);
    }
}

void renderReader(uint8_t *frame) {
    rect(frame, 8, 38, 50, 26);
    UiLocalization::drawText(frame, UiLocalization::isChinese() ? 16 : 15, 47,
                             UiLocalization::isChinese() ? "返回" : "BACK");
    drawUtf8Title(frame, 66, 39, 164, 24, selectedTitle);
    line(frame, 8, 74, 231, 74);

    const uint32_t start = readerPageOffsets[readerPage];
    const uint32_t end = readerPageOffsets[readerPage + 1];
    const char *base = selectedContent.c_str();
    const char *cursor = base + start;
    int x = READER_CONTENT_X;
    int lineIndex = 0;
    while (*cursor != '\0' && static_cast<uint32_t>(cursor - base) < end &&
           lineIndex < READER_LINES_PER_PAGE) {
        uint32_t codepoint = 0;
        if (!nextUtf8Codepoint(cursor, codepoint)) break;
        if (codepoint == '\r') continue;
        if (codepoint == '\n') {
            x = READER_CONTENT_X;
            ++lineIndex;
            continue;
        }
        const int advance = codepointAdvance(codepoint);
        if (x > READER_CONTENT_X && x + advance > READER_CONTENT_X + READER_CONTENT_WIDTH) {
            x = READER_CONTENT_X;
            if (++lineIndex >= READER_LINES_PER_PAGE) break;
        }
        const int lineTop = READER_CONTENT_Y + lineIndex * READER_LINE_HEIGHT;
        if (codepoint < 0x80) {
            char text[2] = {static_cast<char>(codepoint), '\0'};
            UiLocalization::drawText(frame, x, lineTop + 9, text, 1);
        } else {
            const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint);
            if (glyph) {
                constexpr int FONT_BASELINE = 6;
                const int glyphTop = lineTop + READER_LINE_HEIGHT - FONT_BASELINE -
                                     glyph->height - glyph->offsetY;
                drawChineseGlyph(frame, glyph, x + glyph->offsetX, glyphTop,
                                 READER_CONTENT_X + READER_CONTENT_WIDTH,
                                 lineTop + READER_LINE_HEIGHT);
            }
        }
        x += advance;
    }
    rect(frame, 8, 386, 34, 24);
    drawArrow(frame, 25, 398, false);
    rect(frame, 198, 386, 34, 24);
    drawArrow(frame, 215, 398, true);
    char pager[20] = {};
    if (UiLocalization::isChinese()) snprintf(pager, sizeof(pager), "%d/%d", readerPage + 1, readerPageCount());
    else snprintf(pager, sizeof(pager), "%d OF %d", readerPage + 1, readerPageCount());
    drawCentered(frame, 394, pager);
}

}

namespace BookPage {

void setContentUrl(const char *url) {
    std::strncpy(contentBaseUrl, url ? url : "", sizeof(contentBaseUrl) - 1);
    contentBaseUrl[sizeof(contentBaseUrl) - 1] = '\0';
}

void openLibrary() {
    view = View::Library;
    readerContentRefreshRequested = false;
    libraryContentRefreshRequested = false;
    libraryPage = 1;
    selectedLibraryIndex = -1;
    std::strcpy(statusText, UiLocalization::isChinese() ? "正在获取内容" : "LOADING BOOKS");
    bookCount = 0;
    bookTotal = 0;
    libraryLoadCompleted = false;
    libraryAwaitingContent = true;
}

bool startLibraryLoad() {
    if (libraryLoadRunning) return false;
    libraryLoadRunning = true;
    libraryLoadCompleted = false;
    if (xTaskCreate(libraryLoadTask, "book-library", 4096, nullptr, 1, nullptr) != pdPASS) {
        libraryLoadRunning = false;
        libraryAwaitingContent = false;
        std::strcpy(statusText, "BOOK TASK FAILED");
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
    if (!pendingSave || view != View::Reader || selectedBookId <= 0) return;
    pendingSave = false;
    if (saveBookToSd(selectedBookId, selectedTitle, selectedAuthor,
                     selectedCategory, selectedContent)) {
        for (BookItem &book : books) {
            if (book.id == selectedBookId) {
                book.saved = true;
                break;
            }
        }
    } else {
        Serial.printf("[BOOK SD] Displayed id=%ld, but SD save failed\n",
                      static_cast<long>(selectedBookId));
    }
}

bool isReader() {
    return view == View::Reader;
}

bool handleTap(int16_t x, int16_t y) {
    if (view == View::Library) {
        if (pointInRect(x, y, 4, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT) &&
            libraryPage > 1) {
            selectedLibraryIndex = -1;
            libraryPage = 1;
            loadLibrary();
            libraryContentRefreshRequested = true;
            return true;
        }
        if (pointInRect(x, y, 42, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT) &&
            libraryPage > 1) {
            selectedLibraryIndex = -1;
            --libraryPage;
            loadLibrary();
            libraryContentRefreshRequested = true;
            return true;
        }
        if (pointInRect(x, y, 164, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT) &&
            libraryPage < libraryPageCount()) {
            selectedLibraryIndex = -1;
            ++libraryPage;
            loadLibrary();
            libraryContentRefreshRequested = true;
            return true;
        }
        if (pointInRect(x, y, 202, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT) &&
            libraryPage < libraryPageCount()) {
            selectedLibraryIndex = -1;
            libraryPage = libraryPageCount();
            loadLibrary();
            libraryContentRefreshRequested = true;
            return true;
        }
        for (uint8_t index = 0; index < bookCount; ++index) {
            const int top = LIST_TOP + index * (ROW_HEIGHT + ROW_GAP);
            if (pointInRect(x, y, 12, top, 216, ROW_HEIGHT)) {
                selectedLibraryIndex = static_cast<int8_t>(index);
                pendingBookOpenIndex = static_cast<int8_t>(index);
                libraryContentRefreshRequested = true;
                return true;
            }
        }
        return false;
    }

    // Keep the visible Return button unchanged while making its touch target
    // easier to hit around the top-left edge of the reader.
    if (pointInRect(x, y, 0, 32, 70, 40)) {
        readerControlPress = ReaderControl::Back;
        pendingReaderBack = true;
        return true;
    }
    // Use a larger invisible target around the small bottom-edge button. The
    // visible 34x24 frame is retained, but taps near it are easier to register.
    if (pointInRect(x, y, 0, 374, 60, 42)) {
        readerControlPress = ReaderControl::Previous;
        if (readerPage > 0) {
            --readerPage;
            readerContentRefreshRequested = true;
            Serial.printf("[BOOK] Reader previous page=%d/%d\n",
                          readerPage + 1, readerPageCount());
        } else {
            Serial.printf("[BOOK] Reader already at first page 1/%d\n",
                          readerPageCount());
        }
        return true;
    }
    if (pointInRect(x, y, 180, 374, 60, 42)) {
        readerControlPress = ReaderControl::Next;
        if (readerPage + 1 < readerPageCount()) {
            ++readerPage;
            readerContentRefreshRequested = true;
            Serial.printf("[BOOK] Reader next page=%d/%d\n",
                          readerPage + 1, readerPageCount());
        } else {
            Serial.printf("[BOOK] Reader already at last page %d/%d\n",
                          readerPage + 1, readerPageCount());
        }
        return true;
    }
    return false;
}

ReaderControl takeReaderControlPress() {
    const ReaderControl pressed = readerControlPress;
    readerControlPress = ReaderControl::None;
    return pressed;
}

bool processPendingReaderBack() {
    if (!pendingReaderBack) return false;
    pendingReaderBack = false;
    view = View::Library;
    selectedLibraryIndex = -1;
    pendingBookOpenIndex = -1;
    readerControlPress = ReaderControl::None;
    readerContentRefreshRequested = false;
    libraryContentRefreshRequested = false;
    selectedContent = "";
    readerPage = 0;
    Serial.printf("[BOOK] Return to booklist page=%ld items=%u\n",
                  static_cast<long>(libraryPage), bookCount);
    return true;
}

void renderReaderControlPressed(uint8_t *frame, ReaderControl control) {
    renderReader(frame);
    switch (control) {
    case ReaderControl::Back:
        invertRect(frame, 8, 38, 50, 26);
        break;
    case ReaderControl::Previous:
        invertRect(frame, 8, 386, 34, 24);
        break;
    case ReaderControl::Next:
        invertRect(frame, 198, 386, 34, 24);
        break;
    default:
        break;
    }
}

bool handleSwipe(int16_t deltaX, int16_t deltaY) {
    constexpr int16_t SWIPE_THRESHOLD = 35;
    const bool horizontal = abs(deltaX) >= abs(deltaY);
    const int16_t primaryDelta = horizontal ? deltaX : deltaY;
    if (abs(primaryDelta) < SWIPE_THRESHOLD) return false;

    // Natural page movement: sweeping content left/up advances; sweeping it
    // right/down goes back. Both axes are supported for one-handed use.
    const bool next = primaryDelta < 0;
    if (view == View::Library) {
        if (next && libraryPage < libraryPageCount()) {
            selectedLibraryIndex = -1;
            ++libraryPage;
            loadLibrary();
            libraryContentRefreshRequested = true;
            Serial.printf("[BOOK SWIPE] Library next page=%ld axis=%s\n",
                          static_cast<long>(libraryPage), horizontal ? "horizontal" : "vertical");
            return true;
        }
        if (!next && libraryPage > 1) {
            selectedLibraryIndex = -1;
            --libraryPage;
            loadLibrary();
            libraryContentRefreshRequested = true;
            Serial.printf("[BOOK SWIPE] Library previous page=%ld axis=%s\n",
                          static_cast<long>(libraryPage), horizontal ? "horizontal" : "vertical");
            return true;
        }
        return false;
    }

    if (next && readerPage + 1 < readerPageCount()) {
        ++readerPage;
        readerContentRefreshRequested = true;
        Serial.printf("[BOOK SWIPE] Reader next page=%d/%d axis=%s\n",
                      readerPage + 1, readerPageCount(), horizontal ? "horizontal" : "vertical");
        return true;
    }
    if (!next && readerPage > 0) {
        --readerPage;
        readerContentRefreshRequested = true;
        Serial.printf("[BOOK SWIPE] Reader previous page=%d/%d axis=%s\n",
                      readerPage + 1, readerPageCount(), horizontal ? "horizontal" : "vertical");
        return true;
    }
    return false;
}

bool takeReaderContentRefreshRequest() {
    const bool requested = readerContentRefreshRequested;
    readerContentRefreshRequested = false;
    return requested;
}

bool takeLibraryContentRefreshRequest() {
    const bool requested = libraryContentRefreshRequested;
    libraryContentRefreshRequested = false;
    return requested;
}

bool pendingBookOpenRow(int16_t &top) {
    if (pendingBookOpenIndex < 0 || pendingBookOpenIndex >= bookCount) return false;
    top = static_cast<int16_t>(LIST_TOP + pendingBookOpenIndex * (ROW_HEIGHT + ROW_GAP));
    return true;
}

bool pendingBookOpenIsLocal() {
    return pendingBookOpenIndex >= 0 && pendingBookOpenIndex < bookCount &&
           books[static_cast<uint8_t>(pendingBookOpenIndex)].saved;
}

bool preparePendingBookOpen() {
    if (pendingBookOpenIndex < 0 || pendingBookOpenIndex >= bookCount) return false;

    const BookItem &book = books[static_cast<uint8_t>(pendingBookOpenIndex)];
    selectedBookId = book.id;
    copyUtf8(selectedTitle, sizeof(selectedTitle), book.title, "BOOK");
    selectedAuthor[0] = '\0';
    selectedCategory[0] = '\0';
    selectedContent = "";
    readerPage = 0;
    readerPageTotal = 1;
    readerPageOffsets[0] = 0;
    readerPageOffsets[1] = 0;
    pendingSave = false;
    pendingReaderBack = false;
    readerControlPress = ReaderControl::None;
    readerContentRefreshRequested = false;
    view = View::Reader;
    return true;
}

bool processPendingBookOpen() {
    if (pendingBookOpenIndex < 0 || pendingBookOpenIndex >= bookCount) return false;

    const uint8_t index = static_cast<uint8_t>(pendingBookOpenIndex);
    pendingBookOpenIndex = -1;
    return loadBook(books[index]);
}

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    if (view == View::Library) renderLibrary(frame);
    else renderReader(frame);
}

}