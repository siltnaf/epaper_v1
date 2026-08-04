#include "pages/book/book_page.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "ui/localization.h"

#include <cstring>

namespace {

constexpr uint8_t ITEMS_PER_PAGE = 5;
constexpr int LIST_TOP = 105;
constexpr int ROW_HEIGHT = 54;
constexpr int ROW_GAP = 7;
constexpr int READER_COLUMNS = 36;
constexpr int READER_LINES = 27;
constexpr int READER_CHARS_PER_PAGE = READER_COLUMNS * READER_LINES;

struct BookItem {
    int32_t id = 0;
    char title[64] = {};
};

enum class View : uint8_t { Library, Reader };

BookItem books[ITEMS_PER_PAGE] = {};
uint8_t bookCount = 0;
int32_t bookTotal = 0;
int32_t libraryPage = 1;
View view = View::Library;
char contentBaseUrl[128] = "http://";
char statusText[64] = "OPENING LIBRARY";
char selectedTitle[64] = {};
String selectedContent;
int readerPage = 0;

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

bool httpGet(const String &url, String &payload) {
    Serial.printf("[BOOK API] request method=GET url=%s wifi_status=%d ip=%s gateway=%s dns=%s heap=%u\n",
                  url.c_str(), static_cast<int>(WiFi.status()),
                  WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
                  WiFi.dnsIP().toString().c_str(), static_cast<unsigned>(ESP.getFreeHeap()));
    if (WiFi.status() != WL_CONNECTED) {
        std::strcpy(statusText, UiLocalization::isChinese() ? "网络未连接" : "WIFI NOT CONNECTED");
        Serial.println("[BOOK] WiFi is not connected");
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

bool loadLibrary() {
    bookCount = 0;
    if (std::strlen(contentBaseUrl) <= 7) {
        std::strcpy(statusText, "SET CONTENT URL FIRST");
        return false;
    }
    String payload;
    const String url = endpointBase() + "?page=" + String(libraryPage) + "&perPage=" + String(ITEMS_PER_PAGE);
    if (!httpGet(url, payload)) {
        return false;
    }
    Serial.printf("[BOOK] Library payload preview: %.180s\n", payload.c_str());
    bookTotal = jsonInteger(payload, "total", 0, payload.length(), 0);
    const int itemsKey = findKey(payload, "items");
    int cursor = itemsKey < 0 ? -1 : payload.indexOf('[', itemsKey);
    while (cursor >= 0 && bookCount < ITEMS_PER_PAGE) {
        const int objectStart = payload.indexOf('{', cursor);
        const int arrayEnd = payload.indexOf(']', cursor);
        if (objectStart < 0 || (arrayEnd >= 0 && objectStart > arrayEnd)) break;
        const int objectEnd = matchingBrace(payload, objectStart);
        if (objectEnd < 0) break;
        BookItem &book = books[bookCount];
        book.id = jsonInteger(payload, "id", objectStart, objectEnd, 0);
        const String title = jsonString(payload, "title", objectStart, objectEnd);
        char fallback[24] = {};
        snprintf(fallback, sizeof(fallback), "BOOK %ld", static_cast<long>(book.id));
        asciiCopy(book.title, sizeof(book.title), title, fallback);
        if (book.id > 0) ++bookCount;
        cursor = objectEnd;
    }
    if (bookCount == 0) {
        std::strcpy(statusText, "NO BOOKS FOUND");
        return false;
    }
    snprintf(statusText, sizeof(statusText), "%u BOOKS", bookCount);
    return true;
}

bool loadBook(const BookItem &book) {
    String payload;
    if (!httpGet(endpointBase() + "/" + String(book.id), payload)) {
        std::strcpy(statusText, "BOOK LOAD FAILED");
        return false;
    }
    const int end = payload.length();
    String title = jsonString(payload, "title", 0, end);
    if (title.isEmpty()) title = book.title;
    asciiCopy(selectedTitle, sizeof(selectedTitle), title, book.title);
    selectedContent = normalizeContent(jsonString(payload, "content", 0, end));
    if (selectedContent.isEmpty()) {
        std::strcpy(statusText, "BOOK HAS NO CONTENT");
        return false;
    }
    readerPage = 0;
    view = View::Reader;
    return true;
}

int libraryPageCount() {
    return bookTotal > 0 ? (bookTotal + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE : 1;
}

int readerPageCount() {
    return selectedContent.length() > 0
        ? (selectedContent.length() + READER_CHARS_PER_PAGE - 1) / READER_CHARS_PER_PAGE : 1;
}

void renderLibrary(uint8_t *frame) {
    if (UiLocalization::isChinese()) drawCentered(frame, 40, "中文内容", 1);
    else drawCentered(frame, 40, "BOOK LIBRARY", 2);
    rect(frame, 12, 70, 34, 26);
    drawArrow(frame, 29, 83, false);
    rect(frame, 194, 70, 34, 26);
    drawArrow(frame, 211, 83, true);
    char pager[24] = {};
    if (UiLocalization::isChinese()) {
        snprintf(pager, sizeof(pager), "%ld/%d", static_cast<long>(libraryPage), libraryPageCount());
    } else {
        snprintf(pager, sizeof(pager), "PAGE %ld OF %d", static_cast<long>(libraryPage), libraryPageCount());
    }
    drawCentered(frame, 79, pager);

    if (bookCount == 0) {
        drawCentered(frame, 190, statusText);
        drawCentered(frame, 215, UiLocalization::isChinese() ? "网络设置" : "CHECK WIFI AND URL");
        return;
    }
    for (uint8_t index = 0; index < bookCount; ++index) {
        const int top = LIST_TOP + index * (ROW_HEIGHT + ROW_GAP);
        rect(frame, 12, top, 216, ROW_HEIGHT);
        char number[8] = {};
        snprintf(number, sizeof(number), "%u", static_cast<unsigned>((libraryPage - 1) * ITEMS_PER_PAGE + index + 1));
        UiLocalization::drawText(frame, 20, top + 10, number, 2);
        char title[31] = {};
        std::strncpy(title, books[index].title, sizeof(title) - 1);
        UiLocalization::drawText(frame, 50, top + 9, title, 1);
        drawArrow(frame, 215, top + ROW_HEIGHT / 2, true);
    }
}

void renderReader(uint8_t *frame) {
    rect(frame, 8, 38, 50, 26);
    UiLocalization::drawText(frame, UiLocalization::isChinese() ? 16 : 15, 47,
                             UiLocalization::isChinese() ? "返回" : "BACK");
    char title[27] = {};
    std::strncpy(title, selectedTitle, sizeof(title) - 1);
    UiLocalization::drawText(frame, 66, 47, title);
    line(frame, 8, 74, 231, 74);

    const int start = readerPage * READER_CHARS_PER_PAGE;
    int x = 12;
    int y = 86;
    int column = 0;
    int row = 0;
    for (int index = start; index < static_cast<int>(selectedContent.length()) && row < READER_LINES; ++index) {
        const char character = selectedContent[index];
        if (character == ' ' && column == 0) continue;
        char glyph[2] = {character, '\0'};
        UiLocalization::drawText(frame, x, y, glyph);
        x += 6;
        if (++column >= READER_COLUMNS) {
            column = 0;
            ++row;
            x = 12;
            y += 11;
        }
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
    libraryPage = 1;
    std::strcpy(statusText, UiLocalization::isChinese() ? "正在获取内容" : "LOADING BOOKS");
    loadLibrary();
}

bool handleTap(int16_t x, int16_t y) {
    if (view == View::Library) {
        if (pointInRect(x, y, 12, 70, 34, 26) && libraryPage > 1) {
            --libraryPage;
            loadLibrary();
            return true;
        }
        if (pointInRect(x, y, 194, 70, 34, 26) && libraryPage < libraryPageCount()) {
            ++libraryPage;
            loadLibrary();
            return true;
        }
        for (uint8_t index = 0; index < bookCount; ++index) {
            const int top = LIST_TOP + index * (ROW_HEIGHT + ROW_GAP);
            if (pointInRect(x, y, 12, top, 216, ROW_HEIGHT)) {
                loadBook(books[index]);
                return true;
            }
        }
        return false;
    }

    if (pointInRect(x, y, 8, 38, 50, 26)) {
        view = View::Library;
        return true;
    }
    if (pointInRect(x, y, 8, 386, 34, 24) && readerPage > 0) {
        --readerPage;
        return true;
    }
    if (pointInRect(x, y, 198, 386, 34, 24) && readerPage + 1 < readerPageCount()) {
        ++readerPage;
        return true;
    }
    return false;
}

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    if (view == View::Library) renderLibrary(frame);
    else renderReader(frame);
}

}