#include "pages/word/word_page.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <SD_MMC.h>

#include <cmath>
#include <cstring>

#include "devices/audio/opus_player.h"
#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/ml307/ml307.h"
#include "devices/sd_card/sd_card.h"
#include "font/xiaozhi_font.h"
#include "ui/loading_indicator.h"
#include "ui/localization.h"

namespace {

constexpr uint8_t ITEMS_PER_PAGE = 6;
constexpr int BITMAP_W = 180;
constexpr int BITMAP_H = 120;
constexpr size_t BITMAP_BYTES = (BITMAP_W * BITMAP_H + 1) / 2;
constexpr uint8_t MAX_STROKES = 32;
constexpr uint8_t MAX_POINTS_PER_STROKE = 64;
constexpr uint16_t MAX_OFFLINE_WORDS = 256;
// Cover the complete source character, including its upper and leftmost marks,
// then redraw the character progressively inside the same image area.
constexpr int GLYPH_X = 22;
constexpr int GLYPH_Y = 75;
constexpr int GLYPH_W = 100;
constexpr int GLYPH_H = 100;
constexpr uint8_t ANIMATION_STEPS_PER_STROKE = 1;
constexpr uint32_t MIN_ANIMATION_DURATION_MS = 125;
constexpr uint32_t MAX_ANIMATION_DURATION_MS = 150;
constexpr char WORD_SD_FOLDER[] = "/word";
constexpr int PAGER_TOP = 52;
constexpr int PAGER_HEIGHT = 25;
constexpr int PAGER_BUTTON_W = 34;
constexpr int CARD_X = 10;
constexpr int CARD_Y = 82;
constexpr int CARD_W = 106;
constexpr int CARD_H = 101;
constexpr int CARD_GAP_X = 8;
constexpr int CARD_GAP_Y = 7;
constexpr int DETAIL_CARD_X = 12;
constexpr int DETAIL_CARD_Y = 74;
constexpr int DETAIL_CARD_W = 216;
constexpr int DETAIL_CARD_H = 326;
constexpr int DETAIL_IMAGE_INSET = 6;
constexpr int REPLAY_X = 174;
constexpr int REPLAY_Y = 362;
constexpr int REPLAY_W = 48;
constexpr int REPLAY_H = 30;

struct WordItem {
    int32_t id = 0;
    char word[40] = {};
    char pinyin[60] = {};
    char emoji[16] = {};
    char category[40] = {};
    char bitmapUrl[220] = {};
    bool bitmapLoaded = false;
    bool saved = false;
};

struct StrokePoint {
    int16_t x = 0;
    int16_t y = 0;
};

struct Stroke {
    StrokePoint points[MAX_POINTS_PER_STROKE] = {};
    uint8_t pointCount = 0;
};

WordItem items[ITEMS_PER_PAGE] = {};
uint8_t bitmaps[ITEMS_PER_PAGE][BITMAP_BYTES] = {};
uint8_t itemCount = 0;
int32_t totalItems = 0;
int32_t currentPage = 1;
int8_t selectedIndex = -1;
char contentBaseUrl[128] = "http://";
char selectedVoice[32] = "Jasper";
char statusText[72] = "LOADING WORDS";
Stroke strokes[MAX_STROKES] = {};
uint8_t strokeCount = 0;
uint8_t animationStroke = 0;
uint8_t animationStep = 0;
uint32_t nextAnimationMs = 0;
uint32_t animationFrameMs = 250;
bool animationActive = false;
bool pronunciationPending = false;
bool wordAudioActive = false;
bool replayRefreshRequested = false;
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

bool parseStrokeData(const String &payload);

void pixel(uint8_t *frame, int x, int y) {
    if (x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |= 0x80U >> (x % 8);
}

void clearPixel(uint8_t *frame, int x, int y) {
    if (x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] &=
        static_cast<uint8_t>(~(0x80U >> (x % 8)));
}

void clearRect(uint8_t *frame, int x, int y, int width, int height) {
    for (int row = 0; row < height; ++row)
        for (int column = 0; column < width; ++column)
            clearPixel(frame, x + column, y + row);
}

void grayPixel(uint8_t *frame, int x, int y, uint8_t level) {
    // UC8253 is a binary panel. Stable 4x4 ordered halftoning presents four
    // useful perceived levels without shimmer between partial refreshes.
    static constexpr uint8_t bayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5},
    };
    static constexpr uint8_t coverage[4] = {16, 11, 5, 0};
    if (level < 4 && bayer[y & 3][x & 3] < coverage[level]) pixel(frame, x, y);
}

void line(uint8_t *frame, int x1, int y1, int x2, int y2) {
    const int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    const int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
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

bool inRect(int16_t x, int16_t y, int left, int top, int width, int height) {
    return x >= left && x < left + width && y >= top && y < top + height;
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

bool nextCodepoint(const char *&cursor, uint32_t &codepoint) {
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

void drawChineseGlyph(uint8_t *frame, const XiaozhiFont::Glyph *glyph,
                      int x, int y, int right, int bottom) {
    if (!glyph) return;
    for (uint16_t gy = 0; gy < glyph->height && y + gy < bottom; ++gy) {
        for (uint16_t gx = 0; gx < glyph->width && x + gx < right; ++gx) {
            const uint32_t index = static_cast<uint32_t>(gy) * glyph->width + gx;
            const uint8_t packed = glyph->bitmap[index / 2U];
            const uint8_t alpha = (index & 1U) ? (packed & 0x0FU) : (packed >> 4);
            if (alpha >= 11) pixel(frame, x + gx, y + gy);
        }
    }
}

int textAdvance(uint32_t codepoint) {
    if (codepoint < 0x80) return codepoint == ' ' ? 5 : 7;
    const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint);
    return glyph ? max(1, static_cast<int>(glyph->advance)) + 1 : 18;
}

int utf8Width(const char *text) {
    int width = 0;
    const char *cursor = text;
    uint32_t codepoint = 0;
    while (nextCodepoint(cursor, codepoint)) width += textAdvance(codepoint);
    return width;
}

void drawUtf8(uint8_t *frame, int x, int y, int width, int height, const char *text) {
    const int right = x + width;
    const char *cursor = text;
    uint32_t codepoint = 0;
    while (nextCodepoint(cursor, codepoint) && x < right) {
        const int advance = textAdvance(codepoint);
        if (x + advance > right) break;
        if (codepoint < 0x80) {
            char value[2] = {static_cast<char>(codepoint), 0};
            UiLocalization::drawText(frame, x, y + (height - 7) / 2, value);
        } else if (const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint)) {
            const int top = y + height - 6 - glyph->height - glyph->offsetY;
            drawChineseGlyph(frame, glyph, x + glyph->offsetX, top, right, y + height);
        }
        x += advance;
    }
}

void drawUtf8Centered(uint8_t *frame, int x, int y, int width, int height, const char *text) {
    drawUtf8(frame, x + max(0, (width - utf8Width(text)) / 2), y, width, height, text);
}

void copyJsonString(char *destination, size_t size, JsonObject item,
                    const char *key, const char *fallback = "") {
    const char *value = item[key] | fallback;
    std::strncpy(destination, value ? value : fallback, size - 1);
    destination[size - 1] = '\0';
}

String hostBase() {
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

String apiBase() {
    String base(contentBaseUrl);
    base.trim();
    while (base.endsWith("/")) base.remove(base.length() - 1);
    const int api = base.indexOf("/api/");
    return api >= 0 ? base.substring(0, api) : hostBase();
}

String absoluteUrl(const char *value) {
    String url(value ? value : "");
    url.trim();
    if (url.startsWith("http://") || url.startsWith("https://")) return url;
    return hostBase() + (url.startsWith("/") ? url : "/" + url);
}

bool httpGetText(const String &url, String &payload, bool showLoading = true,
                 uint32_t timeoutMs = 12000) {
    OptionalLoadingScope loading(showLoading);
    if (WiFi.status() != WL_CONNECTED) return cellularModem.httpGet(url.c_str(), payload);
    HTTPClient http;
    http.setConnectTimeout(7000);
    http.setTimeout(min<uint32_t>(timeoutMs, 65000));
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);
    WiFiClient plain;
    WiFiClientSecure secure;
    secure.setInsecure();
    const bool began = url.startsWith("https://") ? http.begin(secure, url) : http.begin(plain, url);
    if (!began) return false;
    http.addHeader("Connection", "close");
    const int code = http.GET();
    if (code >= 200 && code < 300) payload = http.getString();
    http.end();
    Serial.printf("[WORD API] GET code=%d bytes=%u url=%s\n", code, payload.length(), url.c_str());
    return code >= 200 && code < 300;
}

bool httpGetBitmap(const String &url, uint8_t *output) {
    if (!output || WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    http.setConnectTimeout(7000);
    http.setTimeout(12000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);
    WiFiClient plain;
    WiFiClientSecure secure;
    secure.setInsecure();
    const bool began = url.startsWith("https://") ? http.begin(secure, url) : http.begin(plain, url);
    if (!began) return false;
    http.addHeader("Connection", "close");
    const int code = http.GET();
    size_t total = 0;
    if (code >= 200 && code < 300) {
        WiFiClient *stream = http.getStreamPtr();
        uint32_t lastData = millis();
        while (total < BITMAP_BYTES && (http.connected() || stream->available())) {
            const size_t available = stream->available();
            if (available) {
                const size_t requested = min(available, BITMAP_BYTES - total);
                const int received = stream->readBytes(output + total, requested);
                if (received <= 0) break;
                total += received;
                lastData = millis();
            } else {
                if (millis() - lastData > 3000) break;
                delay(2);
            }
        }
    }
    http.end();
    Serial.printf("[WORD BITMAP] code=%d bytes=%u/%u url=%s\n",
                  code, total, BITMAP_BYTES, url.c_str());
    return code >= 200 && code < 300 && total == BITMAP_BYTES;
}

int pageCount() {
    return totalItems > 0 ? (totalItems + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE : 1;
}

String bitmapRequestUrl(const WordItem &item) {
    if (item.bitmapUrl[0]) {
        String url = absoluteUrl(item.bitmapUrl);
        const int bpp = url.indexOf("bpp=");
        if (bpp >= 0) {
            const int value = bpp + 4;
            url.setCharAt(value, '4');
        } else {
            url += url.indexOf('?') < 0 ? "?width=180&height=120&bpp=4" : "&bpp=4";
        }
        return url;
    }
    return apiBase() + "/api/image-learn/" + String(item.id) +
           "/bitmap?width=180&height=120&bpp=4";
}

void wordSdPath(int32_t id, const char *fileName, char *output, size_t size) {
    snprintf(output, size, "%s/%ld/%s", WORD_SD_FOLDER, static_cast<long>(id), fileName);
}

bool isWordSaved(int32_t id) {
    if (!SdCard::isMounted() || id <= 0) return false;
    char metaPath[64] = {};
    char bitmapPath[64] = {};
    char strokesPath[64] = {};
    wordSdPath(id, "meta.json", metaPath, sizeof(metaPath));
    wordSdPath(id, "bitmap.bin", bitmapPath, sizeof(bitmapPath));
    wordSdPath(id, "strokes.json", strokesPath, sizeof(strokesPath));
    return SD_MMC.exists(metaPath) && SD_MMC.exists(bitmapPath) && SD_MMC.exists(strokesPath);
}

bool ensureWordDirectory(int32_t id, char *directory, size_t size) {
    if (!SdCard::isMounted() || id <= 0) return false;
    if (!SD_MMC.exists(WORD_SD_FOLDER) && !SD_MMC.mkdir(WORD_SD_FOLDER)) return false;
    snprintf(directory, size, "%s/%ld", WORD_SD_FOLDER, static_cast<long>(id));
    return SD_MMC.exists(directory) || SD_MMC.mkdir(directory);
}

bool writeWordBitmap(int32_t id, const uint8_t *bitmap) {
    if (!bitmap) return false;
    char path[64] = {};
    wordSdPath(id, "bitmap.bin", path, sizeof(path));
    SD_MMC.remove(path);
    File file = SD_MMC.open(path, FILE_WRITE);
    if (!file) return false;
    const size_t written = file.write(bitmap, BITMAP_BYTES);
    file.flush();
    file.close();
    return written == BITMAP_BYTES;
}

bool writeWordText(int32_t id, const char *fileName, const String &content) {
    char path[64] = {};
    wordSdPath(id, fileName, path, sizeof(path));
    SD_MMC.remove(path);
    File file = SD_MMC.open(path, FILE_WRITE);
    if (!file) return false;
    const size_t written = file.print(content);
    file.flush();
    file.close();
    return written == content.length();
}

bool saveWordToSd(WordItem &item, uint8_t bitmapIndex, const String &strokePayload) {
    if (bitmapIndex >= itemCount || !item.bitmapLoaded || strokePayload.isEmpty()) return false;
    char directory[48] = {};
    if (!ensureWordDirectory(item.id, directory, sizeof(directory))) return false;

    JsonDocument document;
    document["id"] = item.id;
    document["word"] = item.word;
    document["pinyin"] = item.pinyin;
    document["emoji"] = item.emoji;
    document["category"] = item.category;
    String metadata;
    serializeJson(document, metadata);

    // Write the completion marker (metadata) last so interrupted saves are not
    // presented as valid offline cards.
    char metaPath[64] = {};
    wordSdPath(item.id, "meta.json", metaPath, sizeof(metaPath));
    SD_MMC.remove(metaPath);
    if (!writeWordBitmap(item.id, bitmaps[bitmapIndex]) ||
        !writeWordText(item.id, "strokes.json", strokePayload) ||
        !writeWordText(item.id, "meta.json", metadata)) {
        Serial.printf("[WORD SD] Save failed id=%ld\n", static_cast<long>(item.id));
        return false;
    }
    item.saved = true;
    Serial.printf("[WORD SD] Saved id=%ld word=%s\n", static_cast<long>(item.id), item.word);
    return true;
}

bool readWordBitmap(int32_t id, uint8_t *output) {
    char path[64] = {};
    wordSdPath(id, "bitmap.bin", path, sizeof(path));
    File file = SD_MMC.open(path, FILE_READ);
    if (!file || file.size() != BITMAP_BYTES) {
        if (file) file.close();
        return false;
    }
    const size_t read = file.read(output, BITMAP_BYTES);
    file.close();
    return read == BITMAP_BYTES;
}

bool readWordText(int32_t id, const char *fileName, String &content) {
    char path[64] = {};
    wordSdPath(id, fileName, path, sizeof(path));
    File file = SD_MMC.open(path, FILE_READ);
    if (!file) return false;
    content = file.readString();
    file.close();
    return !content.isEmpty();
}

bool loadOfflinePage() {
    itemCount = 0;
    selectedIndex = -1;
    totalItems = 0;
    if (!SdCard::isMounted() || !SD_MMC.exists(WORD_SD_FOLDER)) {
        std::strcpy(statusText, UiLocalization::isChinese() ? "没有离线单词" : "NO OFFLINE WORDS");
        return false;
    }

    int32_t ids[MAX_OFFLINE_WORDS] = {};
    uint16_t idCount = 0;
    File root = SD_MMC.open(WORD_SD_FOLDER);
    if (!root || !root.isDirectory()) return false;
    File entry = root.openNextFile();
    while (entry && idCount < MAX_OFFLINE_WORDS) {
        if (entry.isDirectory()) {
            String name(entry.name());
            const int slash = name.lastIndexOf('/');
            if (slash >= 0) name = name.substring(slash + 1);
            const int32_t id = name.toInt();
            if (id > 0 && isWordSaved(id)) ids[idCount++] = id;
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
    for (uint16_t i = 0; i < idCount; ++i) {
        for (uint16_t j = i + 1; j < idCount; ++j) {
            if (ids[j] < ids[i]) { const int32_t value = ids[i]; ids[i] = ids[j]; ids[j] = value; }
        }
    }

    totalItems = idCount;
    const uint16_t first = static_cast<uint16_t>((currentPage - 1) * ITEMS_PER_PAGE);
    if (first >= idCount && currentPage > 1) {
        currentPage = max<int32_t>(1, (idCount + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE);
        return loadOfflinePage();
    }
    for (uint16_t source = first; source < idCount && itemCount < ITEMS_PER_PAGE; ++source) {
        String metadata;
        if (!readWordText(ids[source], "meta.json", metadata)) continue;
        JsonDocument document;
        if (deserializeJson(document, metadata)) continue;
        WordItem &item = items[itemCount];
        item = WordItem();
        item.id = document["id"] | ids[source];
        std::strncpy(item.word, document["word"] | "", sizeof(item.word) - 1);
        std::strncpy(item.pinyin, document["pinyin"] | "", sizeof(item.pinyin) - 1);
        std::strncpy(item.emoji, document["emoji"] | "", sizeof(item.emoji) - 1);
        std::strncpy(item.category, document["category"] | "", sizeof(item.category) - 1);
        item.bitmapLoaded = readWordBitmap(item.id, bitmaps[itemCount]);
        item.saved = item.bitmapLoaded;
        if (item.bitmapLoaded) ++itemCount;
    }
    snprintf(statusText, sizeof(statusText), "%u OFFLINE", itemCount);
    Serial.printf("[WORD SD] offline page=%ld items=%u total=%u\n",
                  static_cast<long>(currentPage), itemCount, idCount);
    return itemCount > 0;
}

bool loadPage(bool showLoading = true) {
    OptionalLoadingScope loadingIndicator(showLoading);
    itemCount = 0;
    selectedIndex = -1;
    totalItems = 0;
    for (uint8_t i = 0; i < ITEMS_PER_PAGE; ++i) {
        items[i] = WordItem();
        std::memset(bitmaps[i], 0xFF, BITMAP_BYTES);
    }
    if (WiFi.status() != WL_CONNECTED) {
        return loadOfflinePage();
    }
    const String url = apiBase() + "/api/image-learn?page=" + String(currentPage) +
                       "&perPage=" + String(ITEMS_PER_PAGE);
    String payload;
    if (!httpGetText(url, payload, showLoading)) {
        return loadOfflinePage();
    }
    JsonDocument document;
    if (deserializeJson(document, payload)) {
        std::strcpy(statusText, "WORD JSON FAILED");
        return false;
    }
    totalItems = document["total"] | 0;
    JsonArray values = document["items"].as<JsonArray>();
    if (values.isNull()) return false;
    for (JsonObject value : values) {
        if (itemCount >= ITEMS_PER_PAGE) break;
        WordItem &item = items[itemCount];
        item.id = value["id"] | 0;
        copyJsonString(item.word, sizeof(item.word), value, "word", "Untitled");
        copyJsonString(item.pinyin, sizeof(item.pinyin), value, "pinyin");
        copyJsonString(item.emoji, sizeof(item.emoji), value, "emoji");
        copyJsonString(item.category, sizeof(item.category), value, "category");
        copyJsonString(item.bitmapUrl, sizeof(item.bitmapUrl), value, "bitmap_url");
        ++itemCount;
    }
    for (uint8_t i = 0; i < itemCount; ++i) {
        items[i].bitmapLoaded = httpGetBitmap(bitmapRequestUrl(items[i]), bitmaps[i]);
        items[i].saved = isWordSaved(items[i].id);
        if (!items[i].bitmapLoaded && items[i].saved) {
            items[i].bitmapLoaded = readWordBitmap(items[i].id, bitmaps[i]);
        }
    }
    snprintf(statusText, sizeof(statusText), "%u WORDS", itemCount);
    Serial.printf("[WORD] page=%ld items=%u total=%ld\n",
                  static_cast<long>(currentPage), itemCount, static_cast<long>(totalItems));
    return itemCount > 0;
}

void libraryLoadTask(void *) {
    loadPage(false);
    libraryAwaitingContent = false;
    libraryLoadRunning = false;
    libraryLoadCompleted = true;
    vTaskDelete(nullptr);
}

uint8_t bitmapValue(const uint8_t *bitmap, int x, int y) {
    x = constrain(x, 0, BITMAP_W - 1);
    y = constrain(y, 0, BITMAP_H - 1);
    const int index = y * BITMAP_W + x;
    const uint8_t packed = bitmap[index / 2];
    return (index & 1) ? (packed & 0x0F) : (packed >> 4);
}

uint8_t enhancedBitmapValue(const uint8_t *bitmap, int x, int y) {
    const int center = bitmapValue(bitmap, x, y);
    const int neighborAverage =
        (bitmapValue(bitmap, x - 1, y) + bitmapValue(bitmap, x + 1, y) +
         bitmapValue(bitmap, x, y - 1) + bitmapValue(bitmap, x, y + 1)) /
        4;

    // A small unsharp mask followed by a modest midpoint contrast expansion
    // makes soft source images clearer after conversion to four e-paper levels.
    const int sharpened = center * 2 - neighborAverage;
    const int contrasted = 8 + (sharpened - 8) * 5 / 4;
    return static_cast<uint8_t>(constrain(contrasted, 0, 15));
}

void drawBitmapScaled(uint8_t *frame, int x, int y, int width, int height,
                      const uint8_t *bitmap) {
    for (int dy = 0; dy < height; ++dy) {
        const int sy = dy * BITMAP_H / height;
        for (int dx = 0; dx < width; ++dx) {
            const int sx = dx * BITMAP_W / width;
            const uint8_t value = enhancedBitmapValue(bitmap, sx, sy);
            const uint8_t level = value >= 12 ? 3 : value >= 7 ? 2 : value >= 3 ? 1 : 0;
            grayPixel(frame, x + dx, y + dy, level);
        }
    }
}

int segmentLength(const StrokePoint &a, const StrokePoint &b) {
    const int32_t dx = b.x - a.x;
    const int32_t dy = b.y - a.y;
    return static_cast<int>(sqrtf(static_cast<float>(dx * dx + dy * dy)));
}

void thickLine(uint8_t *frame, int x0, int y0, int x1, int y1, int thickness, uint8_t gray) {
    const int radius = thickness / 2;
    const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        for (int oy = -radius; oy <= radius; ++oy) {
            for (int ox = -radius; ox <= radius; ++ox) {
                if (ox * ox + oy * oy <= radius * radius + 1) grayPixel(frame, x0 + ox, y0 + oy, gray);
            }
        }
        if (x0 == x1 && y0 == y1) break;
        const int doubled = error * 2;
        if (doubled >= dy) { error += dy; x0 += sx; }
        if (doubled <= dx) { error += dx; y0 += sy; }
    }
}

void drawStroke(uint8_t *frame, const Stroke &stroke, int percent, uint8_t gray, int thickness) {
    if (stroke.pointCount < 2 || percent <= 0) return;
    int total = 0;
    for (uint8_t i = 1; i < stroke.pointCount; ++i) total += segmentLength(stroke.points[i - 1], stroke.points[i]);
    int remaining = total * min(100, percent) / 100;
    auto mapPoint = [](const StrokePoint &point, int &x, int &y) {
        constexpr int padding = 12;
        x = GLYPH_X + padding + static_cast<int32_t>(point.x) * (GLYPH_W - padding * 2) / 1024;
        y = GLYPH_Y + padding + static_cast<int32_t>(point.y) * (GLYPH_H - padding * 2) / 1024;
    };
    for (uint8_t i = 1; i < stroke.pointCount && remaining > 0; ++i) {
        const int length = max(1, segmentLength(stroke.points[i - 1], stroke.points[i]));
        int x0, y0, x1, y1;
        mapPoint(stroke.points[i - 1], x0, y0);
        mapPoint(stroke.points[i], x1, y1);
        if (remaining < length) {
            x1 = x0 + (x1 - x0) * remaining / length;
            y1 = y0 + (y1 - y0) * remaining / length;
        }
        thickLine(frame, x0, y0, x1, y1, thickness, gray);
        remaining -= length;
    }
}

void renderStrokeAnimation(uint8_t *frame) {
    for (uint8_t i = 0; i < strokeCount; ++i) {
        if (i < animationStroke) drawStroke(frame, strokes[i], 100, 0, 1);
        else if (i == animationStroke)
            drawStroke(frame, strokes[i], animationStep * 100 / ANIMATION_STEPS_PER_STROKE, 0, 1);
    }
}

bool parseStrokeData(const String &payload) {
    JsonDocument document;
    if (deserializeJson(document, payload)) return false;
    JsonArray medians = document["medians"].as<JsonArray>();
    if (medians.isNull()) return false;
    strokeCount = 0;
    for (JsonArray median : medians) {
        if (strokeCount >= MAX_STROKES) break;
        Stroke &stroke = strokes[strokeCount];
        stroke = Stroke();
        for (JsonArray point : median) {
            if (stroke.pointCount >= MAX_POINTS_PER_STROKE || point.size() < 2) break;
            const int x = constrain(point[0].as<int>(), 0, 1024);
            const int y = constrain(1024 - point[1].as<int>(), 0, 1024);
            if (stroke.pointCount > 0 && stroke.points[stroke.pointCount - 1].x == x &&
                stroke.points[stroke.pointCount - 1].y == y) continue;
            stroke.points[stroke.pointCount].x = static_cast<int16_t>(x);
            stroke.points[stroke.pointCount].y = static_cast<int16_t>(y);
            ++stroke.pointCount;
        }
        if (stroke.pointCount >= 2) ++strokeCount;
    }
    return strokeCount > 0;
}

bool loadStrokeData(WordItem &item) {
    UiLoadingIndicator::Scope loadingIndicator;
    strokeCount = 0;
    const char *cursor = item.word;
    uint32_t codepoint = 0;
    if (!nextCodepoint(cursor, codepoint)) return false;
    char first[5] = {};
    const size_t length = static_cast<size_t>(cursor - item.word);
    if (length == 0 || length >= sizeof(first)) return false;
    memcpy(first, item.word, length);
    String encoded;
    for (size_t i = 0; i < length; ++i) {
        char hex[4];
        snprintf(hex, sizeof(hex), "%%%02X", static_cast<uint8_t>(first[i]));
        encoded += hex;
    }
    String payload;
    bool loaded = item.saved && readWordText(item.id, "strokes.json", payload) && parseStrokeData(payload);
    if (!loaded && WiFi.status() == WL_CONNECTED) {
        loaded = httpGetText(apiBase() + "/api/stroke-data/" + encoded, payload) && parseStrokeData(payload);
    }
    if (loaded && !item.saved) {
        const ptrdiff_t index = &item - items;
        if (index >= 0 && index < itemCount) saveWordToSd(item, static_cast<uint8_t>(index), payload);
    }
    Serial.printf("[WORD STROKES] word=%s strokes=%u loaded=%s\n", item.word, strokeCount, loaded ? "yes" : "no");
    return loaded;
}

void startAnimation() {
    OpusPlayer::stop();
    wordAudioActive = false;
    animationStroke = 0;
    animationStep = 0;
    pronunciationPending = false;
    animationActive = strokeCount > 0;
    if (animationActive) {
        const uint8_t complexity = constrain(strokeCount, 1, 20);
        const uint32_t targetDuration = MIN_ANIMATION_DURATION_MS +
            static_cast<uint32_t>(complexity - 1) *
                (MAX_ANIMATION_DURATION_MS - MIN_ANIMATION_DURATION_MS) / 19;
        animationFrameMs = targetDuration /
            (static_cast<uint32_t>(strokeCount) * ANIMATION_STEPS_PER_STROKE);
        if (animationFrameMs == 0) animationFrameMs = 1;
        Serial.printf("[WORD ANIMATION] strokes=%u target=%lu ms frame=%lu ms\n",
                      strokeCount, targetDuration, animationFrameMs);
    }
    nextAnimationMs = millis() + animationFrameMs;
}

void safeVoiceName(char *output, size_t size) {
    size_t count = 0;
    for (size_t i = 0; selectedVoice[i] && count + 1 < size; ++i) {
        const char c = selectedVoice[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') output[count++] = c;
    }
    output[count] = '\0';
    if (count == 0) std::strcpy(output, "Jasper");
}

bool playPronunciation() {
    if (selectedIndex < 0 || selectedIndex >= itemCount || !SdCard::isMounted()) {
        Serial.printf("[WORD PRONUNCIATION] unavailable selected=%d count=%u sd=%s\n",
                      selectedIndex, itemCount, SdCard::isMounted() ? "yes" : "no");
        return false;
    }
    UiLoadingIndicator::Scope loadingIndicator;
    const WordItem &item = items[selectedIndex];
    char voice[32] = {};
    safeVoiceName(voice, sizeof(voice));
    char directory[48] = {};
    char path[96] = {};
    if (!ensureWordDirectory(item.id, directory, sizeof(directory))) {
        Serial.printf("[WORD PRONUNCIATION] could not create directory wordId=%ld\n",
                      static_cast<long>(item.id));
        return false;
    }
    snprintf(path, sizeof(path), "%s/pronunciation_%s.opus", directory, voice);
    if (!SdCard::isValidOggOpus(path)) {
        if (SD_MMC.exists(path)) SD_MMC.remove(path);
        String metadata;
        const String url = apiBase() + "/api/image-learn/" + String(item.id) + "/tts?voice=" + String(voice);
        Serial.printf("[WORD PRONUNCIATION] metadata wordId=%ld voice=%s url=%s\n",
                      static_cast<long>(item.id), selectedVoice, url.c_str());
        if (!httpGetText(url, metadata, false, 180000)) {
            Serial.println("[WORD PRONUNCIATION] metadata request failed");
            return false;
        }
        JsonDocument document;
        const DeserializationError error = deserializeJson(document, metadata);
        if (error) {
            Serial.printf("[WORD PRONUNCIATION] metadata JSON failed: %s\n", error.c_str());
            return false;
        }
        const char *audioUrl = document["esp32_url"] | document["absolute_audio_url"] | document["audio_url"] | "";
        if (!audioUrl[0]) {
            Serial.println("[WORD PRONUNCIATION] metadata contains no audio URL");
            return false;
        }
        const String downloadUrl = absoluteUrl(audioUrl);
        Serial.printf("[WORD PRONUNCIATION] downloading url=%s path=%s\n",
                      downloadUrl.c_str(), path);
        if (!SdCard::downloadFile(downloadUrl.c_str(), path, 1024) ||
            !SdCard::isValidOggOpus(path)) {
            SD_MMC.remove(path);
            Serial.println("[WORD PRONUNCIATION] invalid or incomplete Opus download");
            return false;
        }
    }
    wordAudioActive = OpusPlayer::play(path);
    return wordAudioActive;
}

void renderPager(uint8_t *frame) {
    rect(frame, 4, PAGER_TOP, PAGER_BUTTON_W, PAGER_HEIGHT);
    drawDoubleArrow(frame, 21, PAGER_TOP + PAGER_HEIGHT / 2, false);
    rect(frame, 42, PAGER_TOP, PAGER_BUTTON_W, PAGER_HEIGHT);
    drawArrow(frame, 59, PAGER_TOP + PAGER_HEIGHT / 2, false);
    rect(frame, 164, PAGER_TOP, PAGER_BUTTON_W, PAGER_HEIGHT);
    drawArrow(frame, 181, PAGER_TOP + PAGER_HEIGHT / 2, true);
    rect(frame, 202, PAGER_TOP, PAGER_BUTTON_W, PAGER_HEIGHT);
    drawDoubleArrow(frame, 219, PAGER_TOP + PAGER_HEIGHT / 2, true);
    char pager[20] = {};
    snprintf(pager, sizeof(pager), "%ld/%d", static_cast<long>(currentPage), pageCount());
    UiLocalization::drawCentered(frame, 61, pager);
}

void renderLibrary(uint8_t *frame) {
    if (UiLocalization::isChinese()) {
        drawUtf8Centered(frame, 80, 34, 80, 18, "识字");
    } else {
        UiLocalization::drawCentered(frame, 36, "WORDS", 2);
    }
    renderPager(frame);
    if (itemCount == 0) {
        if (libraryAwaitingContent) return;
        UiLocalization::drawCentered(frame, 210, statusText);
        return;
    }
    for (uint8_t i = 0; i < itemCount; ++i) {
        const int column = i % 2;
        const int row = i / 2;
        const int x = CARD_X + column * (CARD_W + CARD_GAP_X);
        const int y = CARD_Y + row * (CARD_H + CARD_GAP_Y);
        rect(frame, x, y, CARD_W, CARD_H);
        if (items[i].bitmapLoaded) drawBitmapScaled(frame, x + 2, y + 2, CARD_W - 4, CARD_H - 4, bitmaps[i]);
        if (items[i].saved) {
            // A small white-backed tick remains legible over light or dark images.
            clearRect(frame, x + CARD_W - 18, y + 3, 14, 14);
            line(frame, x + CARD_W - 15, y + 10, x + CARD_W - 11, y + 14);
            line(frame, x + CARD_W - 11, y + 14, x + CARD_W - 5, y + 6);
        }
    }
}

void renderDetail(uint8_t *frame) {
    if (selectedIndex < 0 || selectedIndex >= itemCount) return;
    const WordItem &item = items[selectedIndex];
    rect(frame, 10, 40, 58, 26);
    drawUtf8Centered(frame, 12, 41, 54, 23,
                     UiLocalization::isChinese() ? "返回" : "BACK");
    constexpr int headerLeft = 72;
    constexpr int headerRight = 230;
    constexpr int headerGap = 12;
    const int wordWidth = utf8Width(item.word);
    const int pinyinWidth = utf8Width(item.pinyin);
    const int groupWidth = min(headerRight - headerLeft,
                               wordWidth + (pinyinWidth > 0 ? headerGap + pinyinWidth : 0));
    int groupX = headerLeft + (headerRight - headerLeft - groupWidth) / 2;
    drawUtf8(frame, groupX, 38, wordWidth, 30, item.word);
    groupX += wordWidth + (pinyinWidth > 0 ? headerGap : 0);
    if (pinyinWidth > 0) drawUtf8(frame, groupX, 38, headerRight - groupX, 30, item.pinyin);

    // Keep the detail card away from the panel edge so all four border lines
    // remain inside the reliable e-paper refresh area.
    rect(frame, DETAIL_CARD_X, DETAIL_CARD_Y, DETAIL_CARD_W, DETAIL_CARD_H);
    if (item.bitmapLoaded) {
        drawBitmapScaled(frame, DETAIL_CARD_X + DETAIL_IMAGE_INSET,
                         DETAIL_CARD_Y + DETAIL_IMAGE_INSET,
                         DETAIL_CARD_W - DETAIL_IMAGE_INSET * 2,
                         DETAIL_CARD_H - DETAIL_IMAGE_INSET * 2,
                         bitmaps[selectedIndex]);
    }

    // Replace only the small source character in the image's upper-left.
    // Preserve the large reference image in the rest of the card.
    clearRect(frame, GLYPH_X, GLYPH_Y, GLYPH_W, GLYPH_H);
    renderStrokeAnimation(frame);

    // Keep Replay inside the requested bottom-right image corner without
    // reducing the image card's usable dimensions.
    clearRect(frame, REPLAY_X, REPLAY_Y, REPLAY_W, REPLAY_H);
    rect(frame, REPLAY_X, REPLAY_Y, REPLAY_W, REPLAY_H);
    drawUtf8Centered(frame, REPLAY_X + 2, REPLAY_Y + 2, REPLAY_W - 4, REPLAY_H - 4,
                     UiLocalization::isChinese() ? "重播" : "REPLAY");
}

bool navigateLibrary(int direction) {
    const int32_t target = currentPage + direction;
    if (target < 1 || target > pageCount()) return false;
    currentPage = target;
    loadPage();
    return true;
}

bool navigateDetail(bool next) {
    OpusPlayer::stop();
    wordAudioActive = false;
    if (next) {
        if (selectedIndex + 1 < itemCount) { ++selectedIndex; loadStrokeData(items[selectedIndex]); startAnimation(); return true; }
        if (currentPage < pageCount()) {
            ++currentPage;
            loadPage();
            selectedIndex = itemCount > 0 ? 0 : -1;
            if (selectedIndex >= 0) { loadStrokeData(items[selectedIndex]); startAnimation(); }
            return true;
        }
    } else {
        if (selectedIndex > 0) { --selectedIndex; loadStrokeData(items[selectedIndex]); startAnimation(); return true; }
        if (currentPage > 1) {
            --currentPage;
            loadPage();
            selectedIndex = itemCount > 0 ? itemCount - 1 : -1;
            if (selectedIndex >= 0) { loadStrokeData(items[selectedIndex]); startAnimation(); }
            return true;
        }
    }
    return false;
}

}

namespace WordPage {

void setContentUrl(const char *url) {
    std::strncpy(contentBaseUrl, url ? url : "", sizeof(contentBaseUrl) - 1);
    contentBaseUrl[sizeof(contentBaseUrl) - 1] = '\0';
}

void setVoice(const char *voice) {
    std::strncpy(selectedVoice, voice ? voice : "Jasper", sizeof(selectedVoice) - 1);
    selectedVoice[sizeof(selectedVoice) - 1] = '\0';
}

void open() {
    stopAudio();
    currentPage = 1;
    selectedIndex = -1;
    replayRefreshRequested = false;
    std::strcpy(statusText, UiLocalization::isChinese() ? "正在加载单词" : "LOADING WORDS");
    itemCount = 0;
    totalItems = 0;
    libraryAwaitingContent = true;
    libraryLoadCompleted = false;
}

bool startLibraryLoad() {
    if (libraryLoadRunning) return false;
    libraryLoadRunning = true;
    libraryLoadCompleted = false;
    if (xTaskCreate(libraryLoadTask, "word-library", 4096, nullptr, 1, nullptr) != pdPASS) {
        libraryLoadRunning = false;
        libraryAwaitingContent = false;
        std::strcpy(statusText, "WORD TASK FAILED");
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

bool isDetail() { return selectedIndex >= 0; }

bool libraryCardBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                         int16_t &width, int16_t &height) {
    if (selectedIndex >= 0) return false;
    for (uint8_t i = 0; i < itemCount; ++i) {
        const int column = i % 2;
        const int row = i / 2;
        const int cardX = CARD_X + column * (CARD_W + CARD_GAP_X);
        const int cardY = CARD_Y + row * (CARD_H + CARD_GAP_Y);
        if (!inRect(x, y, cardX - 3, cardY - 3, CARD_W + 6, CARD_H + 6)) continue;
        left = cardX;
        top = cardY;
        width = CARD_W;
        height = CARD_H;
        return true;
    }
    return false;
}

bool handleTap(int16_t x, int16_t y) {
    const bool replayTarget = selectedIndex >= 0 &&
        (inRect(x, y, REPLAY_X - 4, REPLAY_Y - 4, REPLAY_W + 8, REPLAY_H + 8) ||
         inRect(x, y, GLYPH_X, GLYPH_Y, GLYPH_W, GLYPH_H));
    if (animationActive) animationActive = false;
    if (wordAudioActive) {
        OpusPlayer::stop();
        wordAudioActive = false;
        if (!replayTarget) return true;
    }
    if (selectedIndex >= 0) {
        if (inRect(x, y, 6, 36, 64, 36)) { selectedIndex = -1; strokeCount = 0; return true; }
        if (replayTarget) {
            startAnimation();
            replayRefreshRequested = true;
            return true;
        }
        return false;
    }
    if (inRect(x, y, 4, PAGER_TOP, PAGER_BUTTON_W, PAGER_HEIGHT) && currentPage > 1) {
        currentPage = 1; loadPage(); return true;
    }
    if (inRect(x, y, 42, PAGER_TOP, PAGER_BUTTON_W, PAGER_HEIGHT)) return navigateLibrary(-1);
    if (inRect(x, y, 164, PAGER_TOP, PAGER_BUTTON_W, PAGER_HEIGHT)) return navigateLibrary(1);
    if (inRect(x, y, 202, PAGER_TOP, PAGER_BUTTON_W, PAGER_HEIGHT) && currentPage < pageCount()) {
        currentPage = pageCount(); loadPage(); return true;
    }
    for (uint8_t i = 0; i < itemCount; ++i) {
        const int column = i % 2;
        const int row = i / 2;
        const int cardX = CARD_X + column * (CARD_W + CARD_GAP_X);
        const int cardY = CARD_Y + row * (CARD_H + CARD_GAP_Y);
        if (inRect(x, y, cardX - 3, cardY - 3, CARD_W + 6, CARD_H + 6)) {
            selectedIndex = i;
            loadStrokeData(items[selectedIndex]);
            startAnimation();
            return true;
        }
    }
    return false;
}

bool handleSwipe(int16_t deltaX, int16_t deltaY) {
    animationActive = false;
    if (wordAudioActive) { OpusPlayer::stop(); wordAudioActive = false; }
    const int16_t delta = abs(deltaX) >= abs(deltaY) ? deltaX : deltaY;
    if (abs(delta) < 35) return false;
    const bool next = delta < 0;
    return selectedIndex >= 0 ? navigateDetail(next) : navigateLibrary(next ? 1 : -1);
}

bool takeReplayRefreshRequest() {
    const bool requested = replayRefreshRequested;
    replayRefreshRequested = false;
    return requested;
}

bool processAnimation() {
    if (wordAudioActive && !OpusPlayer::isPlaying()) wordAudioActive = false;
    if (pronunciationPending) {
        pronunciationPending = false;
        const bool started = playPronunciation();
        Serial.printf("[WORD PRONUNCIATION] word=%s started=%s\n",
                      selectedIndex >= 0 && selectedIndex < itemCount
                          ? items[selectedIndex].word : "",
                      started ? "yes" : "no");
        return false;
    }
    const uint32_t now = millis();
    if (!animationActive || selectedIndex < 0 ||
        static_cast<int32_t>(now - nextAnimationMs) < 0) {
        return false;
    }
    nextAnimationMs = now + animationFrameMs;
    if (++animationStep >= ANIMATION_STEPS_PER_STROKE) {
        animationStep = 0;
        ++animationStroke;
    }
    if (animationStroke >= strokeCount) {
        animationStroke = strokeCount;
        animationStep = ANIMATION_STEPS_PER_STROKE;
        animationActive = false;
        // processAnimation() returns true so the completed character reaches
        // the panel first. Pronunciation starts on the following loop pass.
        pronunciationPending = true;
    }
    return true;
}

bool isAnimating() { return animationActive; }
bool isAudioActive() { return wordAudioActive || OpusPlayer::isPlaying(); }
void stopAudio() {
    animationActive = false;
    pronunciationPending = false;
    if (wordAudioActive) OpusPlayer::stop();
    wordAudioActive = false;
}
void stopFromTouchInterrupt() {
    animationActive = false;
    pronunciationPending = false;
    OpusPlayer::requestStopFromIsr();
    wordAudioActive = false;
}

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    if (selectedIndex >= 0) renderDetail(frame);
    else renderLibrary(frame);
}

}