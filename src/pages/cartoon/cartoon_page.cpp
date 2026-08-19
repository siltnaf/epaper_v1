#include "pages/cartoon/cartoon_page.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <TJpg_Decoder.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <cstring>
#include <memory>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/ml307/ml307.h"
#include "devices/sd_card/sd_card.h"
#include "font/xiaozhi_font.h"
#include "memory_budget.h"
#include "pages/playlist_cache.h"
#include "ui/loading_indicator.h"
#include "ui/localization.h"

namespace {

constexpr uint8_t ROWS_PER_PAGE = 10;
constexpr int LIST_TOP = 82;
constexpr int ROW_HEIGHT = 30;
constexpr int ROW_GAP = 2;
constexpr int PAGER_TOP = 52;
constexpr int BUTTON_WIDTH = 34;
constexpr int BUTTON_HEIGHT = 25;
// Version the cache when server-side image preprocessing changes so stale
// progressive or non-dithered JPEGs are not reused indefinitely.
constexpr char CARTOON_SD_FOLDER[] = "/cartoon-v4";
constexpr int READER_TOP = 32;
constexpr int READER_HEIGHT = XingtaiEpd::HEIGHT - READER_TOP;
constexpr int READER_BACK_HEIGHT = 48;
constexpr size_t MAX_CARTOON_LIST_BYTES = 64 * 1024;

struct ListItem {
    char id[64] = {};
    char title[80] = {};
    bool saved = false;
};

enum class View : uint8_t { Cartoons, Chapters, Reader };

View view = View::Cartoons;
char contentBaseUrl[128] = "http://";
ListItem cartoons[ROWS_PER_PAGE] = {};
ListItem chapterRows[ROWS_PER_PAGE] = {};
uint8_t cartoonCount = 0;
uint8_t chapterRowCount = 0;
uint16_t cartoonPage = 1;
uint16_t cartoonTotal = 0;
uint16_t cartoonOffset = 0;
bool cartoonHasMore = false;
uint16_t chapterPage = 1;
uint16_t chapterTotal = 0;
uint16_t chapterOffset = 0;
bool chapterHasMore = false;
uint16_t readerPage = 1;
uint16_t readerPageTotal = 1;
char selectedSlug[64] = {};
char selectedComicTitle[80] = {};
char selectedChapterId[32] = {};
char selectedChapterTitle[80] = {};
char statusText[64] = {};
String chapterPayload;
size_t jpegSize = 0;
bool imageReady = false;
uint8_t *decodeFrame = nullptr;
char readerJpegPath[192] = {};
CartoonPage::RefreshMode refreshMode = CartoonPage::RefreshMode::Layout;
volatile bool libraryLoadRunning = false;
volatile bool libraryLoadCompleted = false;

bool loadChapterPayloadFromSd(const char *slug, String &payload);
bool saveChapterPayloadToSd(const char *slug, const String &payload);
bool cartoonHasCache(const char *slug);
bool chapterHasCache(const char *slug, const char *chapterId);
bool ensureCartoonDirectory();
bool ensureCacheDirectory(const char *slug, const char *chapterId = nullptr);
bool loadChapterRowsFromSd(const char *slug);
bool loadSavedCartoonsFromSd();
bool loadSavedChapterPageFromSd(const char *slug, int32_t requestedOffset);
bool parseChapterObject(const String &object, uint16_t firstVisible);
void saveCartoonTitleToSd(const char *slug, const char *title);
void saveChapterTitleToSd(const char *slug, const char *chapterId, const char *title);
void safePathComponent(const char *value, char *output, size_t outputSize);

void pixel(uint8_t *frame, int x, int y) {
    if (!frame || x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |= 0x80U >> (x % 8);
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

bool inRect(int16_t x, int16_t y, int left, int top, int width, int height) {
    return x >= left && x < left + width && y >= top && y < top + height;
}

void copyText(char *destination, size_t size, const char *source, const char *fallback = "") {
    if (!destination || size == 0) return;
    const char *value = source && source[0] ? source : fallback;
    std::strncpy(destination, value ? value : "", size - 1);
    destination[size - 1] = '\0';
}

String apiBase() {
    String base(contentBaseUrl);
    base.trim();
    while (base.endsWith("/")) base.remove(base.length() - 1);
    return base + "/api/cartoons";
}

void drawTitle(uint8_t *frame, int x, int y, int width, int height, const char *text) {
    const int right = x + width;
    const int bottom = y + height;
    const char *cursor = text ? text : "";
    int drawX = x;
    while (*cursor && drawX < right) {
        const uint8_t first = static_cast<uint8_t>(*cursor);
        uint32_t codepoint = first;
        int bytes = 1;
        if ((first & 0xE0U) == 0xC0U) {
            codepoint = ((first & 0x1FU) << 6) | (static_cast<uint8_t>(cursor[1]) & 0x3FU);
            bytes = 2;
        } else if ((first & 0xF0U) == 0xE0U) {
            codepoint = ((first & 0x0FU) << 12) |
                        ((static_cast<uint8_t>(cursor[1]) & 0x3FU) << 6) |
                        (static_cast<uint8_t>(cursor[2]) & 0x3FU);
            bytes = 3;
        }
        if (codepoint < 0x80) {
            char ascii[2] = {static_cast<char>(codepoint), '\0'};
            UiLocalization::drawText(frame, drawX, y + (height - 7) / 2, ascii, 1);
            drawX += codepoint == ' ' ? 5 : 7;
        } else {
            const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint);
            const int advance = glyph ? max(1, static_cast<int>(glyph->advance)) + 1 : 18;
            if (drawX + advance > right) break;
            if (glyph) {
                constexpr int baseline = 6;
                const int glyphTop = y + (height - 25) / 2 + 25 - baseline -
                                     glyph->height - glyph->offsetY;
                for (uint16_t gy = 0; gy < glyph->height && glyphTop + gy < bottom; ++gy) {
                    for (uint16_t gx = 0; gx < glyph->width && drawX + glyph->offsetX + gx < right; ++gx) {
                        const uint32_t index = static_cast<uint32_t>(gy) * glyph->width + gx;
                        const uint8_t packed = glyph->bitmap[index / 2U];
                        const uint8_t alpha = (index & 1U) ? packed & 0x0FU : packed >> 4;
                        if (alpha >= 11) pixel(frame, drawX + glyph->offsetX + gx, glyphTop + gy);
                    }
                }
            }
            drawX += advance;
        }
        cursor += bytes;
    }
}

void drawCenteredTitle(uint8_t *frame, int x, int y, int width, int height,
                       const char *text) {
    const char *cursor = text ? text : "";
    int textWidth = 0;
    while (*cursor && textWidth < width) {
        const uint8_t first = static_cast<uint8_t>(*cursor);
        uint32_t codepoint = first;
        int bytes = 1;
        if ((first & 0xE0U) == 0xC0U) {
            codepoint = ((first & 0x1FU) << 6) |
                        (static_cast<uint8_t>(cursor[1]) & 0x3FU);
            bytes = 2;
        } else if ((first & 0xF0U) == 0xE0U) {
            codepoint = ((first & 0x0FU) << 12) |
                        ((static_cast<uint8_t>(cursor[1]) & 0x3FU) << 6) |
                        (static_cast<uint8_t>(cursor[2]) & 0x3FU);
            bytes = 3;
        }
        const XiaozhiFont::Glyph *glyph = codepoint < 0x80
            ? nullptr : XiaozhiFont::glyph(codepoint);
        const int advance = codepoint < 0x80
            ? (codepoint == ' ' ? 5 : 7)
            : (glyph ? max(1, static_cast<int>(glyph->advance)) + 1 : 18);
        if (textWidth + advance > width) break;
        textWidth += advance;
        cursor += bytes;
    }
    drawTitle(frame, x + max(0, (width - textWidth) / 2), y,
              width - max(0, (width - textWidth) / 2), height, text);
}

bool httpGetText(const String &url, String &payload, uint32_t timeout = 20000) {
    UiLoadingIndicator::Scope loading;
    if (WiFi.status() != WL_CONNECTED) {
        if (cellularModem.httpGet(url.c_str(), payload, timeout)) return true;
        copyText(statusText, sizeof(statusText), "NETWORK NOT CONNECTED");
        return false;
    }
    for (uint8_t attempt = 1; attempt <= 2; ++attempt) {
        payload = String();
        HTTPClient http;
        WiFiClient client;
        http.setConnectTimeout(10000);
        http.setTimeout(min<uint32_t>(timeout, 65000));
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.setReuse(false);
        client.setTimeout((timeout + 999) / 1000);
        if (!http.begin(client, url)) {
            copyText(statusText, sizeof(statusText), "INVALID CARTOON URL");
            return false;
        }
        http.addHeader("Connection", "close");
        http.addHeader("User-Agent", "ESP32-ePaper-Cartoon/1.0");
        const int code = http.GET();
        const int contentLength = http.getSize();
        size_t receivedBytes = 0;
        if (code >= 200 && code < 300) {
            WiFiClient *stream = http.getStreamPtr();
            const bool lengthKnown = contentLength >= 0;
            const size_t expectedBytes = lengthKnown
                ? static_cast<size_t>(contentLength) : 0;
            if (lengthKnown && expectedBytes <= MAX_CARTOON_LIST_BYTES) {
                payload.reserve(expectedBytes);
            }
            uint8_t buffer[1024] = {};
            const uint32_t started = millis();
            uint32_t lastDataMs = started;
            while (millis() - started < timeout &&
                   receivedBytes < MAX_CARTOON_LIST_BYTES &&
                   (!lengthKnown || receivedBytes < expectedBytes)) {
                const int available = stream->available();
                if (available <= 0) {
                    // HTTPClient may report the socket closed while lwIP still
                    // has the final response bytes buffered. Drain for a short
                    // idle period before treating the response as truncated.
                    if (millis() - lastDataMs >= min<uint32_t>(timeout, 1000)) break;
                    delay(2);
                    continue;
                }
                size_t requested = min<size_t>(sizeof(buffer),
                    static_cast<size_t>(available));
                if (lengthKnown) {
                    requested = min<size_t>(requested, expectedBytes - receivedBytes);
                }
                const int count = stream->read(buffer, requested);
                if (count <= 0) {
                    delay(2);
                    continue;
                }
                payload.concat(reinterpret_cast<const char *>(buffer), count);
                receivedBytes += static_cast<size_t>(count);
                lastDataMs = millis();
            }
            const bool complete = lengthKnown
                ? receivedBytes == expectedBytes
                : receivedBytes > 0;
            if (!complete) payload = String();
        }
        const String error = code < 0 ? http.errorToString(code) : String();
        http.end();
        Serial.printf("[CARTOON API] GET attempt=%u/2 code=%d%s%s bytes=%u "
                      "content_length=%d heap=%u largest=%u url=%s\n",
                      attempt, code, error.isEmpty() ? "" : " ", error.c_str(),
                       static_cast<unsigned>(receivedBytes), contentLength,
                       static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(ESP.getMaxAllocHeap()), url.c_str());
        if (code >= 200 && code < 300 && !payload.isEmpty()) return true;
        if (code >= 0 || attempt == 2) break;
        delay(350);
    }
    copyText(statusText, sizeof(statusText), "CARTOON NETWORK ERROR");
    return false;
}

// The chapter endpoint currently returns the complete chapter array even when
// limit/offset are supplied.  That response can exceed the largest available
// heap block, so do not use HTTPClient::getString() here.  Keep only one JSON
// object at a time and retain the requested page of rows.
bool httpGetChapterStream(const String &url, uint16_t firstVisible, uint32_t timeout) {
    UiLoadingIndicator::Scope loading;
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    WiFiClient client;
    http.setConnectTimeout(10000);
    http.setTimeout(min<uint32_t>(timeout, 65000));
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);
    client.setTimeout((timeout + 999) / 1000);
    if (!http.begin(client, url)) {
        copyText(statusText, sizeof(statusText), "INVALID CARTOON URL");
        return false;
    }
    http.addHeader("Connection", "close");
    http.addHeader("User-Agent", "ESP32-ePaper-Cartoon/1.0");
    const int code = http.GET();
    const int contentLength = http.getSize();
    bool parsed = false;
    chapterRowCount = 0;
    chapterTotal = 0;
    if (code >= 200 && code < 300) {
        WiFiClient *stream = http.getStreamPtr();
        String marker;
        String object;
        bool chaptersKeyFound = false;
        bool inArray = false;
        bool inObject = false;
        bool inString = false;
        bool escaped = false;
        uint16_t depth = 0;
        const uint32_t started = millis();
        while (millis() - started < timeout) {
            if (stream->available() <= 0) {
                if (!stream->connected()) break;
                delay(2);
                continue;
            }
            const char character = static_cast<char>(stream->read());
            if (!inArray) {
                if (!chaptersKeyFound) {
                    marker += character;
                    if (marker.length() > 12) marker.remove(0, marker.length() - 12);
                    chaptersKeyFound = marker.endsWith("\"chapters\"");
                } else if (character == '[') {
                    inArray = true;
                }
                continue;
            }
            if (!inObject) {
                if (character == ']') {
                    parsed = true;
                    break;
                }
                if (character == '{') {
                    inObject = true;
                    depth = 1;
                    object = "{";
                    inString = false;
                    escaped = false;
                }
                continue;
            }
            object += character;
            if (inString) {
                if (escaped) escaped = false;
                else if (character == '\\') escaped = true;
                else if (character == '"') inString = false;
            } else if (character == '"') {
                inString = true;
            } else if (character == '{') {
                ++depth;
            } else if (character == '}' && depth > 0 && --depth == 0) {
                parseChapterObject(object, firstVisible);
                inObject = false;
                object = "";
                // The current server ignores limit/offset and continues with
                // the complete chapter array. Stop after this page instead of
                // reading the remaining potentially very large response.
                if (chapterTotal >= firstVisible + ROWS_PER_PAGE) {
                    parsed = true;
                    break;
                }
            }
        }
        if (parsed) {
            chapterOffset = firstVisible;
            chapterHasMore = chapterRowCount == ROWS_PER_PAGE;
            chapterTotal = firstVisible + chapterRowCount + (chapterHasMore ? 1 : 0);
            chapterPage = max<uint16_t>(1, firstVisible / ROWS_PER_PAGE + 1);
        }
    }
    const String error = code < 0 ? http.errorToString(code) : String();
    http.end();
    Serial.printf("[CARTOON API] stream code=%d%s%s content_length=%d chapters=%u visible=%u "
                  "heap=%u largest=%u url=%s\n",
                  code, error.isEmpty() ? "" : " ", error.c_str(), contentLength,
                  chapterTotal, chapterRowCount, static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMaxAllocHeap()), url.c_str());
    return parsed && chapterRowCount > 0;
}

bool loadCartoons(int32_t requestedOffset, bool forceRemote = false,
                  bool cacheOnly = false) {
    String payload;
    const String endpoint = apiBase();
    char cacheSlot[24] = {};
    snprintf(cacheSlot, sizeof(cacheSlot), "offset-%ld", static_cast<long>(requestedOffset));
    bool loaded = !forceRemote && PlaylistCache::load(
        CARTOON_SD_FOLDER, endpoint, cacheSlot, payload);
    bool fetchedRemote = false;
    const String url = endpoint + "?limit=" + String(ROWS_PER_PAGE) +
                       "&offset=" + String(requestedOffset);
    if (!cacheOnly && (forceRemote || !loaded)) {
        loaded = httpGetText(url, payload);
        fetchedRemote = loaded;
    }
    if (!loaded && forceRemote) loaded = PlaylistCache::load(
        CARTOON_SD_FOLDER, endpoint, cacheSlot, payload);
    if (!loaded) return loadSavedCartoonsFromSd();
    JsonDocument document;
    if (deserializeJson(document, payload)) {
        if (fetchedRemote) return loadCartoons(requestedOffset, false, true);
        copyText(statusText, sizeof(statusText), "CARTOON JSON FAILED");
        return false;
    }
    if (fetchedRemote) PlaylistCache::save(
        CARTOON_SD_FOLDER, endpoint, cacheSlot, payload);
    const uint16_t responseTotal = document["total"] | 0;
    const uint16_t responseOffset = document["offset"] |
        static_cast<uint16_t>(requestedOffset < 0 ? 0 : requestedOffset);
    cartoonCount = 0;
    cartoonTotal = max<uint16_t>(responseTotal, responseOffset);
    cartoonOffset = responseOffset;
    cartoonHasMore = document["has_more"] | false;
    const bool offline = WiFi.status() != WL_CONNECTED;
    for (JsonObject item : document["cartoons"].as<JsonArray>()) {
        if (cartoonCount >= ROWS_PER_PAGE) break;
        copyText(cartoons[cartoonCount].id, sizeof(cartoons[cartoonCount].id), item["id"] | "");
        copyText(cartoons[cartoonCount].title, sizeof(cartoons[cartoonCount].title),
                 item["name"] | "", "UNTITLED");
        cartoons[cartoonCount].saved = cartoonHasCache(cartoons[cartoonCount].id);
        if (offline && !cartoons[cartoonCount].saved) continue;
        if (cartoons[cartoonCount].saved) {
            saveCartoonTitleToSd(cartoons[cartoonCount].id, cartoons[cartoonCount].title);
        }
        if (cartoons[cartoonCount].id[0]) ++cartoonCount;
    }
    if (cartoonTotal < cartoonOffset + cartoonCount) {
        cartoonTotal = cartoonOffset + cartoonCount;
    }
    if (offline) {
        cartoonTotal = cartoonCount;
        cartoonOffset = 0;
        cartoonHasMore = false;
        if (cartoonCount == 0) return loadSavedCartoonsFromSd();
    }
    cartoonPage = max<uint16_t>(
        1, (cartoonOffset + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE + 1);
    copyText(statusText, sizeof(statusText), cartoonCount ? "" : "NO CARTOONS");
    return cartoonCount > 0 || cartoonTotal == 0;
}

void libraryLoadTask(void *) {
    loadCartoons(0, true);
    libraryLoadRunning = false;
    libraryLoadCompleted = true;
    vTaskDelete(nullptr);
}

String jsonStringAt(const String &json, int objectStart, const char *key, int objectEnd) {
    const String token = String('"') + key + '"';
    int position = json.indexOf(token, objectStart);
    if (position < 0 || position >= objectEnd) return String();
    position = json.indexOf(':', position + token.length());
    if (position < 0 || position >= objectEnd) return String();
    position = json.indexOf('"', position + 1);
    if (position < 0 || position >= objectEnd) return String();
    String result;
    for (++position; position < objectEnd; ++position) {
        const char value = json[position];
        if (value == '"') break;
        if (value == '\\' && position + 1 < objectEnd) {
            const char escaped = json[++position];
            result += escaped == 'n' ? '\n' : escaped;
        } else {
            result += value;
        }
    }
    return result;
}

void loadChapterRows() {
    chapterRowCount = 0;
    chapterTotal = 0;
    const int arrayStart = chapterPayload.indexOf("\"chapters\"");
    int position = arrayStart >= 0 ? chapterPayload.indexOf('[', arrayStart) + 1 : -1;
    const uint16_t firstVisible = (chapterPage - 1) * ROWS_PER_PAGE;
    while (position > 0) {
        const int objectStart = chapterPayload.indexOf('{', position);
        const int arrayEnd = chapterPayload.indexOf(']', position);
        if (objectStart < 0 || (arrayEnd >= 0 && objectStart > arrayEnd)) break;
        const int objectEnd = chapterPayload.indexOf('}', objectStart);
        if (objectEnd < 0) break;
        if (chapterTotal >= firstVisible && chapterRowCount < ROWS_PER_PAGE) {
            const String id = jsonStringAt(chapterPayload, objectStart, "id", objectEnd);
            const String title = jsonStringAt(chapterPayload, objectStart, "title", objectEnd);
            copyText(chapterRows[chapterRowCount].id, sizeof(chapterRows[chapterRowCount].id), id.c_str());
            copyText(chapterRows[chapterRowCount].title, sizeof(chapterRows[chapterRowCount].title),
                     title.c_str(), "UNTITLED");
            chapterRows[chapterRowCount].saved =
                chapterHasCache(selectedSlug, chapterRows[chapterRowCount].id);
            if (chapterRows[chapterRowCount].id[0]) ++chapterRowCount;
        }
        ++chapterTotal;
        position = objectEnd + 1;
    }
}

void chapterListPath(const char *slug, char *path, size_t pathSize) {
    char safeSlug[64] = {};
    safePathComponent(slug, safeSlug, sizeof(safeSlug));
    snprintf(path, pathSize, "%s/%s/chapters.json", CARTOON_SD_FOLDER, safeSlug);
}

bool parseChapterObject(const String &object, uint16_t firstVisible) {
    if (object.indexOf("\"chapters\"") >= 0) return false;
    const String id = jsonStringAt(object, 0, "id", object.length());
    const String title = jsonStringAt(object, 0, "title", object.length());
    if (id.isEmpty()) return false;
    if (chapterTotal >= firstVisible && chapterRowCount < ROWS_PER_PAGE) {
        copyText(chapterRows[chapterRowCount].id, sizeof(chapterRows[chapterRowCount].id), id.c_str());
        copyText(chapterRows[chapterRowCount].title, sizeof(chapterRows[chapterRowCount].title),
                 title.c_str(), "UNTITLED");
        chapterRows[chapterRowCount].saved =
            chapterHasCache(selectedSlug, chapterRows[chapterRowCount].id);
        ++chapterRowCount;
    }
    ++chapterTotal;
    return true;
}

bool loadChapterRowsFromSd(const char *slug) {
    char path[128] = {};
    chapterListPath(slug, path, sizeof(path));
    File file = SD_MMC.open(path, FILE_READ);
    if (!file || file.size() == 0) {
        if (file) file.close();
        return false;
    }
    chapterRowCount = 0;
    chapterTotal = 0;
    const uint16_t firstVisible = (chapterPage - 1) * ROWS_PER_PAGE;
    String object;
    String marker;
    bool chaptersKeyFound = false;
    bool inArray = false;
    bool inObject = false;
    uint16_t depth = 0;
    while (file.available()) {
        const char character = static_cast<char>(file.read());
        if (!inArray) {
            if (!chaptersKeyFound) {
                marker += character;
                if (marker.length() > 12) marker.remove(0, marker.length() - 12);
                chaptersKeyFound = marker.endsWith("\"chapters\"");
            } else if (character == '[') {
                inArray = true;
            }
            continue;
        }
        if (!inObject) {
            if (character == ']') break;
            if (character == '{') {
                inObject = true;
                depth = 1;
                object = "{";
            }
            continue;
        }
        object += character;
        if (character == '{') ++depth;
        else if (character == '}' && depth > 0 && --depth == 0) {
            parseChapterObject(object, firstVisible);
            inObject = false;
            object = "";
        }
    }
    file.close();
    chapterOffset = firstVisible;
    chapterHasMore = firstVisible + chapterRowCount < chapterTotal;
    chapterPage = max<uint16_t>(1, firstVisible / ROWS_PER_PAGE + 1);
    return chapterTotal > 0;
}

bool loadChapterPageFromApi(const char *slug, int32_t requestedOffset) {
    String payload;
    const String endpoint = apiBase() + "/" + slug;
    char cacheSlot[24] = {};
    snprintf(cacheSlot, sizeof(cacheSlot), "offset-%ld", static_cast<long>(requestedOffset));
    bool loaded = PlaylistCache::load(
        CARTOON_SD_FOLDER, endpoint, cacheSlot, payload);
    const String url = endpoint + "?limit=" + String(ROWS_PER_PAGE) +
                       "&offset=" + String(requestedOffset);
    if (!loaded) {
        const uint16_t firstVisible = static_cast<uint16_t>(
            requestedOffset < 0 ? 0 : requestedOffset);
        if (WiFi.status() == WL_CONNECTED &&
            httpGetChapterStream(url, firstVisible, 30000)) {
            copyText(statusText, sizeof(statusText), "");
            return true;
        }
        if (WiFi.status() != WL_CONNECTED) {
            loaded = httpGetText(url, payload, 30000);
            if (loaded) PlaylistCache::save(
                CARTOON_SD_FOLDER, endpoint, cacheSlot, payload);
        }
    }
    if (!loaded) {
        if (requestedOffset >= 0) {
            chapterPage = static_cast<uint16_t>(requestedOffset / ROWS_PER_PAGE + 1);
            if (loadChapterRowsFromSd(slug)) return true;
        }
        return loadSavedChapterPageFromSd(slug, requestedOffset);
    }

    JsonDocument document;
    if (deserializeJson(document, payload)) {
        copyText(statusText, sizeof(statusText), "CHAPTER JSON FAILED");
        return false;
    }
    const uint16_t responseTotal = document["total"] | 0;
    const uint16_t responseOffset = document["offset"] |
        static_cast<uint16_t>(requestedOffset < 0 ? 0 : requestedOffset);
    chapterRowCount = 0;
    chapterTotal = max<uint16_t>(responseTotal, responseOffset);
    chapterOffset = responseOffset;
    chapterHasMore = document["has_more"] | false;
    for (JsonObject item : document["chapters"].as<JsonArray>()) {
        if (chapterRowCount >= ROWS_PER_PAGE) break;
        copyText(chapterRows[chapterRowCount].id, sizeof(chapterRows[chapterRowCount].id),
                 item["id"] | "");
        copyText(chapterRows[chapterRowCount].title, sizeof(chapterRows[chapterRowCount].title),
                 item["title"] | "", "UNTITLED");
        chapterRows[chapterRowCount].saved =
            chapterHasCache(selectedSlug, chapterRows[chapterRowCount].id);
        if (chapterRows[chapterRowCount].saved) {
            saveChapterTitleToSd(slug, chapterRows[chapterRowCount].id,
                                 chapterRows[chapterRowCount].title);
        }
        if (chapterRows[chapterRowCount].id[0]) ++chapterRowCount;
    }
    if (chapterTotal < chapterOffset + chapterRowCount) {
        chapterTotal = chapterOffset + chapterRowCount;
    }
    chapterPage = max<uint16_t>(
        1, (chapterOffset + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE + 1);
    copyText(statusText, sizeof(statusText), chapterRowCount ? "" : "NO CHAPTERS");
    return chapterRowCount > 0 || chapterTotal == 0;
}

bool loadChapters(const ListItem &comic) {
    copyText(selectedSlug, sizeof(selectedSlug), comic.id);
    copyText(selectedComicTitle, sizeof(selectedComicTitle), comic.title);
    chapterPage = 1;
    chapterRowCount = 0;
    chapterTotal = 0;
    chapterOffset = 0;
    chapterHasMore = false;
    view = View::Chapters;
    copyText(statusText, sizeof(statusText), "LOADING CHAPTERS");
    chapterPayload = "";
    if (!loadChapterPageFromApi(comic.id, 0) && !loadChapterRowsFromSd(comic.id)) {
        copyText(statusText, sizeof(statusText), "CHAPTER LOAD FAILED");
        Serial.printf("[CARTOON] Chapter page opened without data comic=%s\n", comic.id);
        return true;
    }
    copyText(statusText, sizeof(statusText), chapterRowCount ? "" : "NO CHAPTERS");
    Serial.printf("[CARTOON] Chapter page comic=%s total=%u visible=%u payload=%u\n",
                  comic.id, chapterTotal, chapterRowCount, 0U);
    return true;
}

bool jpegOutput(int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t *data) {
    if (!decodeFrame || !data) return false;
    for (uint16_t row = 0; row < height; ++row) {
        for (uint16_t column = 0; column < width; ++column) {
            const uint16_t rgb = data[static_cast<size_t>(row) * width + column];
            const uint16_t red = (rgb >> 11) & 0x1F;
            const uint16_t green = (rgb >> 5) & 0x3F;
            const uint16_t blue = rgb & 0x1F;
            const uint16_t luminance = red * 299 + green * 293 + blue * 114;
            if (luminance < 15000) pixel(decodeFrame, x + column, y + row);
        }
    }
    return true;
}

struct JpegDcTable {
    uint8_t counts[16] = {};
    uint8_t symbols[16] = {};
    uint8_t symbolCount = 0;
    bool valid = false;
};

class JpegBitReader {
public:
    explicit JpegBitReader(File &file) : file_(file) {}

    bool readBits(uint8_t count, uint16_t &value) {
        value = 0;
        while (count--) {
            if (bitsRemaining_ == 0 && !readEntropyByte(currentByte_)) return false;
            value = static_cast<uint16_t>((value << 1) |
                                          ((currentByte_ >> --bitsRemaining_) & 1U));
        }
        return true;
    }

private:
    bool readEntropyByte(uint8_t &value) {
        int next = file_.read();
        if (next < 0) return false;
        if (next != 0xFF) {
            value = static_cast<uint8_t>(next);
            bitsRemaining_ = 8;
            return true;
        }
        do {
            next = file_.read();
        } while (next == 0xFF);
        if (next != 0x00) return false;
        value = 0xFF;
        bitsRemaining_ = 8;
        return true;
    }

    File &file_;
    uint8_t currentByte_ = 0;
    uint8_t bitsRemaining_ = 0;
};

bool readFileBytes(File &file, uint8_t *buffer, size_t size) {
    return buffer && file.read(buffer, size) == size;
}

bool skipFileBytes(File &file, size_t size) {
    return file.seek(file.position() + size);
}

bool readJpegMarker(File &file, uint8_t &marker) {
    int value = 0;
    do {
        value = file.read();
        if (value < 0) return false;
    } while (value != 0xFF);
    do {
        value = file.read();
        if (value < 0) return false;
    } while (value == 0xFF);
    marker = static_cast<uint8_t>(value);
    return marker != 0x00;
}

bool decodeHuffmanSymbol(JpegBitReader &bits, const JpegDcTable &table, uint8_t &symbol) {
    uint16_t code = 0;
    uint16_t firstCode = 0;
    uint8_t symbolIndex = 0;
    for (uint8_t length = 1; length <= 16; ++length) {
        uint16_t bit = 0;
        if (!bits.readBits(1, bit)) return false;
        code = static_cast<uint16_t>((code << 1) | bit);
        const uint8_t count = table.counts[length - 1];
        if (code >= firstCode && code < firstCode + count) {
            const uint8_t index = static_cast<uint8_t>(symbolIndex + code - firstCode);
            if (index >= table.symbolCount) return false;
            symbol = table.symbols[index];
            return true;
        }
        symbolIndex = static_cast<uint8_t>(symbolIndex + count);
        firstCode = static_cast<uint16_t>((firstCode + count) << 1);
    }
    return false;
}

void drawDitheredBlock(uint8_t *frame, int left, int top, int right, int bottom,
                       uint8_t grayscale) {
    static constexpr uint8_t BAYER_4X4[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}
    };
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const uint8_t threshold = static_cast<uint8_t>(BAYER_4X4[y & 3][x & 3] * 16 + 8);
            if (grayscale < threshold) pixel(frame, x, y);
        }
    }
}

bool drawProgressiveGrayscalePreview(uint8_t *frame, const char *path,
                                     uint16_t &sourceWidth, uint16_t &sourceHeight,
                                     uint8_t &outputScale) {
    File file = SD_MMC.open(path, FILE_READ);
    if (!file) return false;
    uint8_t soi[2] = {};
    if (!readFileBytes(file, soi, sizeof(soi)) || soi[0] != 0xFF || soi[1] != 0xD8) {
        file.close();
        return false;
    }

    uint16_t quantDc[4] = {};
    JpegDcTable dcTables[4];
    uint8_t componentId = 0;
    uint8_t quantTableId = 0;
    bool progressiveGrayscale = false;

    while (file.available()) {
        uint8_t marker = 0;
        if (!readJpegMarker(file, marker) || marker == 0xD9) break;
        uint8_t lengthBytes[2] = {};
        if (!readFileBytes(file, lengthBytes, sizeof(lengthBytes))) break;
        const uint16_t segmentLength = static_cast<uint16_t>(lengthBytes[0] << 8 | lengthBytes[1]);
        if (segmentLength < 2) break;
        size_t remaining = segmentLength - 2;

        if (marker == 0xDB) {
            while (remaining > 0) {
                const int info = file.read();
                if (info < 0) break;
                --remaining;
                const uint8_t precision = static_cast<uint8_t>(info >> 4);
                const uint8_t tableId = static_cast<uint8_t>(info & 0x0F);
                const size_t tableBytes = precision ? 128 : 64;
                if (remaining < tableBytes) { remaining = 0; break; }
                uint16_t firstValue = 0;
                if (precision) {
                    const int high = file.read(), low = file.read();
                    if (high < 0 || low < 0) { remaining = 0; break; }
                    firstValue = static_cast<uint16_t>(high << 8 | low);
                    skipFileBytes(file, tableBytes - 2);
                } else {
                    const int value = file.read();
                    if (value < 0) { remaining = 0; break; }
                    firstValue = static_cast<uint16_t>(value);
                    skipFileBytes(file, tableBytes - 1);
                }
                if (tableId < 4) quantDc[tableId] = firstValue;
                remaining -= tableBytes;
            }
        } else if (marker == 0xC2) {
            uint8_t header[32] = {};
            if (remaining > sizeof(header) || !readFileBytes(file, header, remaining)) break;
            if (remaining >= 9 && header[0] == 8 && header[5] == 1) {
                sourceHeight = static_cast<uint16_t>(header[1] << 8 | header[2]);
                sourceWidth = static_cast<uint16_t>(header[3] << 8 | header[4]);
                componentId = header[6];
                quantTableId = header[8];
                progressiveGrayscale = header[7] == 0x11 && quantTableId < 4;
            }
        } else if (marker == 0xC4) {
            while (remaining >= 17) {
                const int info = file.read();
                if (info < 0) { remaining = 0; break; }
                --remaining;
                uint8_t counts[16] = {};
                if (!readFileBytes(file, counts, sizeof(counts))) { remaining = 0; break; }
                remaining -= sizeof(counts);
                uint16_t total = 0;
                for (uint8_t count : counts) total += count;
                if (total > remaining) { remaining = 0; break; }
                const uint8_t tableClass = static_cast<uint8_t>(info >> 4);
                const uint8_t tableId = static_cast<uint8_t>(info & 0x0F);
                if (tableClass == 0 && tableId < 4 && total <= sizeof(dcTables[tableId].symbols)) {
                    memcpy(dcTables[tableId].counts, counts, sizeof(counts));
                    dcTables[tableId].symbolCount = static_cast<uint8_t>(total);
                    dcTables[tableId].valid = readFileBytes(
                        file, dcTables[tableId].symbols, total);
                } else {
                    skipFileBytes(file, total);
                }
                remaining -= total;
            }
            if (remaining) skipFileBytes(file, remaining);
        } else if (marker == 0xDA) {
            uint8_t scan[16] = {};
            if (remaining > sizeof(scan) || !readFileBytes(file, scan, remaining)) break;
            if (!progressiveGrayscale || remaining < 6 || scan[0] != 1 ||
                scan[1] != componentId || scan[3] != 0 || scan[4] != 0 ||
                (scan[5] >> 4) != 0) break;
            const uint8_t dcTableId = static_cast<uint8_t>(scan[2] >> 4);
            const uint8_t successiveLow = static_cast<uint8_t>(scan[5] & 0x0F);
            if (dcTableId >= 4 || !dcTables[dcTableId].valid ||
                !quantDc[quantTableId]) break;

            outputScale = 1;
            while ((sourceWidth / outputScale > XingtaiEpd::WIDTH ||
                    sourceHeight / outputScale > READER_HEIGHT) &&
                   outputScale < 8) outputScale <<= 1;
            const int outputWidth = max<int>(1, sourceWidth / outputScale);
            const int outputHeight = max<int>(1, sourceHeight / outputScale);
            const int drawX = max(0, (XingtaiEpd::WIDTH - outputWidth) / 2);
            const int drawY = READER_TOP + max(0, (READER_HEIGHT - outputHeight) / 2);
            const uint16_t blockColumns = (sourceWidth + 7) / 8;
            const uint16_t blockRows = (sourceHeight + 7) / 8;
            JpegBitReader bits(file);
            int32_t dc = 0;
            for (uint16_t by = 0; by < blockRows; ++by) {
                for (uint16_t bx = 0; bx < blockColumns; ++bx) {
                    uint8_t category = 0;
                    if (!decodeHuffmanSymbol(bits, dcTables[dcTableId], category) || category > 11) {
                        file.close();
                        return false;
                    }
                    uint16_t encoded = 0;
                    if (category && !bits.readBits(category, encoded)) {
                        file.close();
                        return false;
                    }
                    int32_t difference = encoded;
                    if (category && encoded < (1U << (category - 1)))
                        difference -= (1U << category) - 1;
                    dc += difference;
                    const int32_t coefficient = dc << successiveLow;
                    const int32_t level = 128 + coefficient * quantDc[quantTableId] / 8;
                    const uint8_t grayscale = static_cast<uint8_t>(constrain(level, 0, 255));
                    const int left = drawX + (bx * 8) / outputScale;
                    const int right = drawX + min<int>(sourceWidth, (bx + 1) * 8) / outputScale;
                    const int top = drawY + (by * 8) / outputScale;
                    const int bottom = drawY + min<int>(sourceHeight, (by + 1) * 8) / outputScale;
                    drawDitheredBlock(frame, left, top, right, bottom, grayscale);
                }
            }
            file.close();
            return true;
        } else {
            skipFileBytes(file, remaining);
        }
    }
    file.close();
    return false;
}

bool ensureCartoonDirectory() {
    if (!SdCard::isMounted()) return false;
    if (SD_MMC.exists(CARTOON_SD_FOLDER)) return true;
    const bool created = SD_MMC.mkdir(CARTOON_SD_FOLDER);
    Serial.printf("[CARTOON SD] Create directory path=%s result=%s\n",
                  CARTOON_SD_FOLDER, created ? "yes" : "no");
    return created;
}

void safePathComponent(const char *value, char *output, size_t outputSize) {
    if (!output || outputSize == 0) return;
    size_t written = 0;
    for (size_t index = 0; value && value[index] && written + 1 < outputSize; ++index) {
        const char character = value[index];
        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_') {
            output[written++] = character;
        } else {
            output[written++] = '_';
        }
    }
    output[written] = '\0';
}

bool ensureCacheDirectory(const char *slug, const char *chapterId) {
    if (!ensureCartoonDirectory()) return false;
    char safeSlug[64] = {};
    safePathComponent(slug, safeSlug, sizeof(safeSlug));
    if (!safeSlug[0]) return false;
    char comicDirectory[96] = {};
    snprintf(comicDirectory, sizeof(comicDirectory), "%s/%s", CARTOON_SD_FOLDER, safeSlug);
    if (!SD_MMC.exists(comicDirectory) && !SD_MMC.mkdir(comicDirectory)) return false;
    if (!chapterId) return true;
    char safeChapter[48] = {};
    safePathComponent(chapterId, safeChapter, sizeof(safeChapter));
    if (!safeChapter[0]) return false;
    char chapterDirectory[160] = {};
    snprintf(chapterDirectory, sizeof(chapterDirectory), "%s/%s", comicDirectory, safeChapter);
    return SD_MMC.exists(chapterDirectory) || SD_MMC.mkdir(chapterDirectory);
}

void chapterCachePath(const char *slug, const char *chapterId, uint16_t page,
                      char *path, size_t pathSize) {
    char safeSlug[64] = {};
    char safeChapter[48] = {};
    safePathComponent(slug, safeSlug, sizeof(safeSlug));
    safePathComponent(chapterId, safeChapter, sizeof(safeChapter));
    snprintf(path, pathSize, "%s/%s/%s/page-%u.jpg", CARTOON_SD_FOLDER,
             safeSlug, safeChapter, page);
}

bool validCachedJpeg(const char *path, size_t *size = nullptr) {
    if (!SdCard::isMounted() || !path || !SD_MMC.exists(path)) return false;
    File file = SD_MMC.open(path, FILE_READ);
    uint8_t header[2] = {};
    const size_t bytes = file ? file.read(header, sizeof(header)) : 0;
    const size_t fileSize = file ? file.size() : 0;
    if (file) file.close();
    const bool valid = bytes == sizeof(header) && fileSize >= 128 &&
                       header[0] == 0xFF && header[1] == 0xD8;
    if (valid && size) *size = fileSize;
    return valid;
}

bool chapterHasCache(const char *slug, const char *chapterId) {
    char path[192] = {};
    chapterCachePath(slug, chapterId, 1, path, sizeof(path));
    return validCachedJpeg(path);
}

bool cartoonHasCache(const char *slug) {
    if (!SdCard::isMounted()) return false;
    char safeSlug[64] = {};
    safePathComponent(slug, safeSlug, sizeof(safeSlug));
    char comicDirectory[96] = {};
    snprintf(comicDirectory, sizeof(comicDirectory), "%s/%s", CARTOON_SD_FOLDER, safeSlug);
    File directory = SD_MMC.open(comicDirectory);
    if (!directory || !directory.isDirectory()) return false;
    File entry = directory.openNextFile();
    while (entry) {
        const bool isDirectory = entry.isDirectory();
        const String entryPath = entry.path();
        entry.close();
        if (isDirectory) {
            const String firstPage = entryPath + "/page-1.jpg";
            if (validCachedJpeg(firstPage.c_str())) {
                directory.close();
                return true;
            }
        }
        entry = directory.openNextFile();
    }
    directory.close();
    return false;
}

String pathLeaf(const String &path) {
    const int slash = path.lastIndexOf('/');
    return slash >= 0 ? path.substring(slash + 1) : path;
}

void saveTextMetadata(const char *path, const char *value) {
    if (!path || !value || !value[0] || SD_MMC.exists(path)) return;
    File file = SD_MMC.open(path, FILE_WRITE);
    if (file) {
        file.println(value);
        file.close();
    }
}

void saveCartoonTitleToSd(const char *slug, const char *title) {
    if (!ensureCacheDirectory(slug)) return;
    char safeSlug[64] = {};
    safePathComponent(slug, safeSlug, sizeof(safeSlug));
    char path[128] = {};
    snprintf(path, sizeof(path), "%s/%s/title.txt", CARTOON_SD_FOLDER, safeSlug);
    saveTextMetadata(path, title);
}

void saveChapterTitleToSd(const char *slug, const char *chapterId, const char *title) {
    if (!ensureCacheDirectory(slug, chapterId)) return;
    char safeSlug[64] = {};
    char safeChapter[48] = {};
    safePathComponent(slug, safeSlug, sizeof(safeSlug));
    safePathComponent(chapterId, safeChapter, sizeof(safeChapter));
    char path[176] = {};
    snprintf(path, sizeof(path), "%s/%s/%s/title.txt", CARTOON_SD_FOLDER,
             safeSlug, safeChapter);
    saveTextMetadata(path, title);
}

void readTitleMetadata(const char *path, const char *fallback,
                       char *title, size_t titleSize) {
    String value;
    File file = SD_MMC.open(path, FILE_READ);
    if (file) {
        value = file.readStringUntil('\n');
        value.trim();
        file.close();
    }
    copyText(title, titleSize, value.c_str(), fallback);
}

bool loadSavedCartoonsFromSd() {
    if (!SdCard::isMounted()) return false;
    File root = SD_MMC.open(CARTOON_SD_FOLDER);
    if (!root || !root.isDirectory()) return false;
    cartoonCount = 0;
    cartoonTotal = 0;
    cartoonOffset = 0;
    cartoonHasMore = false;
    File entry = root.openNextFile();
    while (entry && cartoonCount < ROWS_PER_PAGE) {
        const bool directory = entry.isDirectory();
        const String slug = pathLeaf(entry.path());
        entry.close();
        if (directory && !slug.isEmpty() && cartoonHasCache(slug.c_str())) {
            ListItem &cartoon = cartoons[cartoonCount++];
            copyText(cartoon.id, sizeof(cartoon.id), slug.c_str());
            char titlePath[128] = {};
            snprintf(titlePath, sizeof(titlePath), "%s/%s/title.txt",
                     CARTOON_SD_FOLDER, slug.c_str());
            readTitleMetadata(titlePath, slug.c_str(), cartoon.title, sizeof(cartoon.title));
            cartoon.saved = true;
        }
        entry = root.openNextFile();
    }
    root.close();
    cartoonTotal = cartoonCount;
    cartoonPage = 1;
    copyText(statusText, sizeof(statusText), cartoonCount ? "" : "NO SAVED CARTOONS");
    Serial.printf("[CARTOON SD] Offline cartoons=%u\n", cartoonCount);
    return cartoonCount > 0;
}

uint16_t savedChapterCount(const char *slug) {
    char safeSlug[64] = {};
    safePathComponent(slug, safeSlug, sizeof(safeSlug));
    char comicPath[96] = {};
    snprintf(comicPath, sizeof(comicPath), "%s/%s", CARTOON_SD_FOLDER, safeSlug);
    File root = SD_MMC.open(comicPath);
    if (!root || !root.isDirectory()) return 0;
    uint16_t count = 0;
    File entry = root.openNextFile();
    while (entry) {
        const bool directory = entry.isDirectory();
        const String chapterId = pathLeaf(entry.path());
        entry.close();
        if (directory && chapterHasCache(slug, chapterId.c_str())) ++count;
        entry = root.openNextFile();
    }
    root.close();
    return count;
}

bool loadSavedChapterPageFromSd(const char *slug, int32_t requestedOffset) {
    if (!SdCard::isMounted()) return false;
    const uint16_t total = savedChapterCount(slug);
    if (total == 0) return false;
    const uint16_t start = requestedOffset < 0
        ? (total > ROWS_PER_PAGE ? total - ROWS_PER_PAGE : 0)
        : min<uint16_t>(requestedOffset, total);
    char safeSlug[64] = {};
    safePathComponent(slug, safeSlug, sizeof(safeSlug));
    char comicPath[96] = {};
    snprintf(comicPath, sizeof(comicPath), "%s/%s", CARTOON_SD_FOLDER, safeSlug);
    File root = SD_MMC.open(comicPath);
    if (!root || !root.isDirectory()) return false;
    chapterRowCount = 0;
    uint16_t savedIndex = 0;
    File entry = root.openNextFile();
    while (entry && chapterRowCount < ROWS_PER_PAGE) {
        const bool directory = entry.isDirectory();
        const String chapterId = pathLeaf(entry.path());
        entry.close();
        if (directory && chapterHasCache(slug, chapterId.c_str())) {
            if (savedIndex >= start) {
                ListItem &chapter = chapterRows[chapterRowCount++];
                copyText(chapter.id, sizeof(chapter.id), chapterId.c_str());
                char titlePath[176] = {};
                snprintf(titlePath, sizeof(titlePath), "%s/%s/%s/title.txt",
                         CARTOON_SD_FOLDER, safeSlug, chapterId.c_str());
                readTitleMetadata(titlePath, chapterId.c_str(), chapter.title,
                                  sizeof(chapter.title));
                chapter.saved = true;
            }
            ++savedIndex;
        }
        entry = root.openNextFile();
    }
    root.close();
    chapterTotal = total;
    chapterOffset = start;
    chapterHasMore = start + chapterRowCount < total;
    chapterPage = max<uint16_t>(1, start / ROWS_PER_PAGE + 1);
    copyText(statusText, sizeof(statusText), chapterRowCount ? "" : "NO SAVED CHAPTERS");
    Serial.printf("[CARTOON SD] Offline chapters comic=%s offset=%u visible=%u total=%u\n",
                  slug, start, chapterRowCount, chapterTotal);
    return chapterRowCount > 0;
}

bool saveChapterPayloadToSd(const char *slug, const String &payload) {
    if (payload.isEmpty() || !ensureCacheDirectory(slug)) return false;
    char safeSlug[64] = {};
    safePathComponent(slug, safeSlug, sizeof(safeSlug));
    char path[128] = {};
    char temporary[136] = {};
    snprintf(path, sizeof(path), "%s/%s/chapters.json", CARTOON_SD_FOLDER, safeSlug);
    snprintf(temporary, sizeof(temporary), "%s.part", path);
    SD_MMC.remove(temporary);
    File file = SD_MMC.open(temporary, FILE_WRITE);
    if (!file) return false;
    const size_t written = file.print(payload);
    file.flush();
    file.close();
    if (written != payload.length()) {
        SD_MMC.remove(temporary);
        return false;
    }
    SD_MMC.remove(path);
    return SD_MMC.rename(temporary, path);
}

bool loadChapterPayloadFromSd(const char *slug, String &payload) {
    if (!SdCard::isMounted()) return false;
    char safeSlug[64] = {};
    safePathComponent(slug, safeSlug, sizeof(safeSlug));
    char path[128] = {};
    snprintf(path, sizeof(path), "%s/%s/chapters.json", CARTOON_SD_FOLDER, safeSlug);
    File file = SD_MMC.open(path, FILE_READ);
    if (!file || file.size() == 0) return false;
    payload = file.readString();
    file.close();
    return !payload.isEmpty();
}

bool downloadJpeg(const String &url, const char *destinationPath) {
    UiLoadingIndicator::Scope loading;
    imageReady = false;
    jpegSize = 0;
    if (!MemoryBudget::canAllocate(1024)) {
        copyText(statusText, sizeof(statusText), "LOW MEMORY, RETRY IMAGE");
        MemoryBudget::log("jpeg-skip");
        return false;
    }
    if (WiFi.status() != WL_CONNECTED ||
        !ensureCacheDirectory(selectedSlug, selectedChapterId)) {
        copyText(statusText, sizeof(statusText), "SD OR WIFI NOT READY");
        return false;
    }
    const String temporaryPath = String(destinationPath) + ".part";
    SD_MMC.remove(temporaryPath);
    for (uint8_t attempt = 1; attempt <= 2; ++attempt) {
        HTTPClient http;
        WiFiClient client;
        http.setConnectTimeout(10000);
        http.setTimeout(60000);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.setReuse(false);
        client.setTimeout(60);
        if (!http.begin(client, url)) {
            copyText(statusText, sizeof(statusText), "INVALID IMAGE URL");
            return false;
        }
        http.addHeader("Connection", "close");
        http.addHeader("User-Agent", "ESP32-ePaper-Cartoon/1.0");
        const int code = http.GET();
        const int contentLength = http.getSize();
        const String error = code < 0 ? http.errorToString(code) : String();
        bool complete = false;
        if (code >= 200 && code < 300 && contentLength > 0 && contentLength <= 90000) {
            File output = SD_MMC.open(temporaryPath, FILE_WRITE);
            if (output) {
                WiFiClient *stream = http.getStreamPtr();
                uint8_t buffer[1024] = {};
                uint32_t lastDataMs = millis();
                const uint32_t startedMs = lastDataMs;
                while (jpegSize < static_cast<size_t>(contentLength) &&
                       millis() - startedMs < 90000) {
                    const int available = stream->available();
                    if (available > 0) {
                        const size_t requested = min<size_t>(
                            min<size_t>(static_cast<size_t>(available), sizeof(buffer)),
                            static_cast<size_t>(contentLength) - jpegSize);
                        const int received = stream->readBytes(buffer, requested);
                        if (received <= 0 ||
                            output.write(buffer, static_cast<size_t>(received)) !=
                                static_cast<size_t>(received)) break;
                        jpegSize += static_cast<size_t>(received);
                        lastDataMs = millis();
                    } else {
                        // A response with "Connection: close" can report the
                        // socket closed before lwIP exposes the buffered body.
                        // Content-Length is authoritative; wait for buffered
                        // bytes until the idle timeout instead of exiting at 0.
                        if (millis() - lastDataMs >= 20000) break;
                        delay(2);
                    }
                }
                output.flush();
                output.close();
                complete = jpegSize == static_cast<size_t>(contentLength);
            }
        }
        Serial.printf("[CARTOON JPEG] attempt=%u/2 code=%d%s%s bytes=%u/%d "
                      "heap=%u largest=%u url=%s\n",
                      attempt, code, error.isEmpty() ? "" : " ", error.c_str(),
                      jpegSize, contentLength, static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(ESP.getMaxAllocHeap()), url.c_str());
        http.end();
        if (complete) {
            SD_MMC.remove(destinationPath);
            if (SD_MMC.rename(temporaryPath, destinationPath)) {
                Serial.printf("[CARTOON SD] Saved image path=%s bytes=%u\n",
                              destinationPath, static_cast<unsigned>(jpegSize));
                return true;
            }
            Serial.println("[CARTOON JPEG] Could not finalize SD image file");
        }
        SD_MMC.remove(temporaryPath);
        jpegSize = 0;
        if (code >= 0 && code != 408 && code != 429 && code < 500) break;
        if (attempt < 2) delay(350);
    }
    return false;
}

uint16_t fetchReaderPageCount(const String &url) {
    if (WiFi.status() != WL_CONNECTED) return 1;
    for (uint8_t attempt = 1; attempt <= 2; ++attempt) {
        HTTPClient http;
        WiFiClient client;
        http.setConnectTimeout(10000);
        http.setTimeout(45000);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.setReuse(false);
        client.setTimeout(45);
        if (!http.begin(client, url)) return 1;
        http.addHeader("Connection", "close");
        http.addHeader("User-Agent", "ESP32-ePaper-Cartoon/1.0");
        const int code = http.GET();
        uint16_t pageTotal = 1;
        DeserializationError jsonError = DeserializationError::Ok;
        if (code >= 200 && code < 300) {
            JsonDocument filter;
            filter["page_count"] = true;
            JsonDocument document;
            jsonError = deserializeJson(document, *http.getStreamPtr(),
                                        DeserializationOption::Filter(filter));
            if (!jsonError) pageTotal = max<uint16_t>(1, document["page_count"] | 1);
        }
        const String error = code < 0 ? http.errorToString(code) : String();
        http.end();
        Serial.printf("[CARTOON MANIFEST] attempt=%u/2 code=%d%s%s json=%s pages=%u "
                      "heap=%u largest=%u url=%s\n",
                      attempt, code, error.isEmpty() ? "" : " ", error.c_str(),
                      jsonError.c_str(), pageTotal, static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(ESP.getMaxAllocHeap()), url.c_str());
        if (code >= 200 && code < 300 && !jsonError) return pageTotal;
        if (code >= 0 || attempt == 2) break;
        delay(350);
    }
    return 1;
}

bool loadReaderPage() {
    chapterCachePath(selectedSlug, selectedChapterId, readerPage,
                     readerJpegPath, sizeof(readerJpegPath));
    if (validCachedJpeg(readerJpegPath, &jpegSize)) {
        imageReady = true;
        Serial.printf("[CARTOON SD] Loaded cached image path=%s bytes=%u\n",
                      readerJpegPath, static_cast<unsigned>(jpegSize));
        return true;
    }
    const String url = apiBase() + "/" + selectedSlug + "/chapter/" +
                       selectedChapterId + "/page/" + String(readerPage);
    if (!downloadJpeg(url, readerJpegPath)) {
        copyText(statusText, sizeof(statusText), "IMAGE DOWNLOAD FAILED");
        return false;
    }
    imageReady = true;
    return true;
}

bool openChapter(const ListItem &chapter) {
    copyText(selectedChapterId, sizeof(selectedChapterId), chapter.id);
    copyText(selectedChapterTitle, sizeof(selectedChapterTitle), chapter.title);
    readerPage = 1;
    // Release the retained chapter-list allocation before the manifest/JPEG
    // buffers compete for the constrained internal heap.
    chapterPayload = static_cast<const char *>(nullptr);
    char safeSlug[64] = {};
    char safeChapter[48] = {};
    safePathComponent(selectedSlug, safeSlug, sizeof(safeSlug));
    safePathComponent(selectedChapterId, safeChapter, sizeof(safeChapter));
    char pageCountPath[176] = {};
    snprintf(pageCountPath, sizeof(pageCountPath), "%s/%s/%s/page-count.txt",
             CARTOON_SD_FOLDER, safeSlug, safeChapter);
    File pageCountFile = SD_MMC.open(pageCountPath, FILE_READ);
    if (pageCountFile) {
        readerPageTotal = max<uint16_t>(1, pageCountFile.parseInt());
        pageCountFile.close();
    } else {
        readerPageTotal = fetchReaderPageCount(
            apiBase() + "/" + selectedSlug + "/chapter/" + selectedChapterId);
        if (ensureCacheDirectory(selectedSlug, selectedChapterId)) {
            pageCountFile = SD_MMC.open(pageCountPath, FILE_WRITE);
            if (pageCountFile) {
                pageCountFile.print(readerPageTotal);
                pageCountFile.close();
            }
        }
    }
    view = View::Reader;
    return loadReaderPage();
}

uint16_t pageCount(uint16_t total) {
    return max<uint16_t>(1, (total + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
}

void renderPager(uint8_t *frame, uint16_t page, uint16_t pages) {
    rect(frame, 4, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT);
    drawDoubleArrow(frame, 21, PAGER_TOP + BUTTON_HEIGHT / 2, false);
    rect(frame, 42, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT);
    drawArrow(frame, 59, PAGER_TOP + BUTTON_HEIGHT / 2, false);
    rect(frame, 164, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT);
    drawArrow(frame, 181, PAGER_TOP + BUTTON_HEIGHT / 2, true);
    rect(frame, 202, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT);
    drawDoubleArrow(frame, 219, PAGER_TOP + BUTTON_HEIGHT / 2, true);
    char counter[20] = {};
    snprintf(counter, sizeof(counter), "%u/%u", page, pages);
    UiLocalization::drawCentered(frame, 61, counter);
}

void renderList(uint8_t *frame, const ListItem *items, uint8_t count,
                uint16_t offset, uint16_t page, uint16_t pages) {
    renderPager(frame, page, pages);
    for (uint8_t index = 0; index < count; ++index) {
        const int top = LIST_TOP + index * (ROW_HEIGHT + ROW_GAP);
        rect(frame, 12, top, 216, ROW_HEIGHT);
        char number[8] = {};
        snprintf(number, sizeof(number), "%u", offset + index + 1);
        UiLocalization::drawText(frame, 18, top + 9, number);
        drawTitle(frame, 34, top + 1, 171, ROW_HEIGHT - 2, items[index].title);
        if (items[index].saved) drawCheckmark(frame, 215, top + ROW_HEIGHT / 2);
        else drawArrow(frame, 215, top + ROW_HEIGHT / 2, true);
    }
}

void renderReader(uint8_t *frame) {
    if (imageReady && jpegSize && SD_MMC.exists(readerJpegPath) &&
        MemoryBudget::canAllocate(4 * 1024)) {
        uint16_t sourceWidth = 0, sourceHeight = 0;
        const JRESULT sizeResult = TJpgDec.getFsJpgSize(
            &sourceWidth, &sourceHeight, readerJpegPath, SD_MMC);
        if (sizeResult == JDR_OK) {
            uint8_t scale = 1;
            while ((sourceWidth / scale > XingtaiEpd::WIDTH ||
                    sourceHeight / scale > READER_HEIGHT) && scale < 8) {
                scale <<= 1;
            }
            const int imageWidth = sourceWidth / scale;
            const int imageHeight = sourceHeight / scale;
            const int drawX = max(0, (XingtaiEpd::WIDTH - imageWidth) / 2);
            const int drawY = READER_TOP + max(0, (READER_HEIGHT - imageHeight) / 2);
            decodeFrame = frame;
            TJpgDec.setSwapBytes(false);
            TJpgDec.setJpgScale(scale);
            TJpgDec.setCallback(jpegOutput);
            const JRESULT drawResult = TJpgDec.drawFsJpg(
                drawX, drawY, readerJpegPath, SD_MMC);
            decodeFrame = nullptr;
            Serial.printf("[CARTOON JPEG] Decode baseline size_result=%d draw_result=%d "
                          "source=%ux%u scale=%u file=%u\n",
                          static_cast<int>(sizeResult), static_cast<int>(drawResult),
                          sourceWidth, sourceHeight, scale, static_cast<unsigned>(jpegSize));
        } else if (sizeResult == JDR_FMT3) {
            uint8_t previewScale = 1;
            const bool previewReady = drawProgressiveGrayscalePreview(
                frame, readerJpegPath, sourceWidth, sourceHeight, previewScale);
            Serial.printf("[CARTOON JPEG] Decode progressive_dc result=%s source=%ux%u "
                          "scale=%u file=%u\n",
                          previewReady ? "ok" : "failed", sourceWidth, sourceHeight,
                          previewScale, static_cast<unsigned>(jpegSize));
            if (!previewReady)
                UiLocalization::drawCentered(frame, 204, "UNSUPPORTED JPEG");
        } else {
            Serial.printf("[CARTOON JPEG] Decode failed size_result=%d file=%u\n",
                          static_cast<int>(sizeResult), static_cast<unsigned>(jpegSize));
            UiLocalization::drawCentered(frame, 204, "JPEG DECODE FAILED");
        }
    } else {
        UiLocalization::drawCentered(frame, 204, statusText);
    }
}

} // namespace

namespace CartoonPage {

void setContentUrl(const char *url) {
    copyText(contentBaseUrl, sizeof(contentBaseUrl), url, "http://");
    Serial.printf("[CARTOON API] Content URL=%s endpoint=%s\n",
                  contentBaseUrl, apiBase().c_str());
}

void open() {
    view = View::Cartoons;
    cartoonPage = 1;
    cartoonOffset = 0;
    cartoonHasMore = false;
    chapterPayload = "";
    imageReady = false;
    jpegSize = 0;
    readerJpegPath[0] = '\0';
    ensureCartoonDirectory();
    cartoonCount = 0;
    cartoonTotal = 0;
    libraryLoadCompleted = false;
    copyText(statusText, sizeof(statusText), "LOADING CARTOONS");
}

bool isReader() { return view == View::Reader; }

bool startLibraryLoad() {
    if (libraryLoadRunning) return false;
    libraryLoadRunning = true;
    libraryLoadCompleted = false;
    if (xTaskCreate(libraryLoadTask, "cartoon-library", 6144,
                    nullptr, 1, nullptr) != pdPASS) {
        libraryLoadRunning = false;
        libraryLoadCompleted = true;
        copyText(statusText, sizeof(statusText), "CARTOON TASK FAILED");
        return false;
    }
    return true;
}

bool takeLibraryLoadCompleted() {
    if (!libraryLoadCompleted) return false;
    libraryLoadCompleted = false;
    return true;
}

bool controlBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                     int16_t &width, int16_t &height) {
    if (view == View::Reader) {
        if (inRect(x, y, 0, READER_TOP, XingtaiEpd::WIDTH, READER_BACK_HEIGHT)) {
            left = 0; top = READER_TOP; width = XingtaiEpd::WIDTH;
            height = READER_BACK_HEIGHT;
            return true;
        }
        if (readerPage > 1 && inRect(x, y, 0, READER_TOP + READER_BACK_HEIGHT,
                                     XingtaiEpd::WIDTH / 2,
                                     READER_HEIGHT - READER_BACK_HEIGHT)) {
            left = 0; top = READER_TOP + READER_BACK_HEIGHT;
            width = XingtaiEpd::WIDTH / 2;
            height = READER_HEIGHT - READER_BACK_HEIGHT;
            return true;
        }
        if (readerPage < readerPageTotal &&
            inRect(x, y, XingtaiEpd::WIDTH / 2, READER_TOP + READER_BACK_HEIGHT,
                   XingtaiEpd::WIDTH / 2,
                   READER_HEIGHT - READER_BACK_HEIGHT)) {
            left = XingtaiEpd::WIDTH / 2;
            top = READER_TOP + READER_BACK_HEIGHT;
            width = XingtaiEpd::WIDTH / 2;
            height = READER_HEIGHT - READER_BACK_HEIGHT;
            return true;
        }
        return false;
    }

    if (view == View::Chapters && inRect(x, y, 4, 34, 58, 18)) {
        left = 8; top = 34; width = 50; height = 17;
        return true;
    }

    const uint16_t offset = view == View::Cartoons ? cartoonOffset : chapterOffset;
    const bool hasMore = view == View::Cartoons ? cartoonHasMore : chapterHasMore;
    if (offset > 0 && inRect(x, y, 4, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT)) {
        left = 4; top = PAGER_TOP; width = BUTTON_WIDTH; height = BUTTON_HEIGHT;
        return true;
    }
    if (offset > 0 && inRect(x, y, 42, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT)) {
        left = 42; top = PAGER_TOP; width = BUTTON_WIDTH; height = BUTTON_HEIGHT;
        return true;
    }
    if (hasMore && inRect(x, y, 164, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT)) {
        left = 164; top = PAGER_TOP; width = BUTTON_WIDTH; height = BUTTON_HEIGHT;
        return true;
    }
    if (hasMore && inRect(x, y, 202, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT)) {
        left = 202; top = PAGER_TOP; width = BUTTON_WIDTH; height = BUTTON_HEIGHT;
        return true;
    }
    return false;
}

bool rowBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                 int16_t &width, int16_t &height) {
    if (view == View::Reader) return false;
    const uint8_t visibleRows = view == View::Cartoons ? cartoonCount : chapterRowCount;
    for (uint8_t index = 0; index < visibleRows; ++index) {
        const int rowTop = LIST_TOP + index * (ROW_HEIGHT + ROW_GAP);
        if (!inRect(x, y, 12, rowTop, 216, ROW_HEIGHT)) continue;
        left = 12;
        top = rowTop;
        width = 216;
        height = ROW_HEIGHT;
        return true;
    }
    return false;
}

bool handleTap(int16_t x, int16_t y) {
    refreshMode = RefreshMode::Layout;
    if (view == View::Reader) {
        if (inRect(x, y, 0, READER_TOP, XingtaiEpd::WIDTH, READER_BACK_HEIGHT)) {
            view = View::Chapters;
            imageReady = false;
            jpegSize = 0;
            readerJpegPath[0] = '\0';
            chapterPayload = "";
            if (!loadChapterPageFromApi(selectedSlug, chapterOffset) &&
                !loadChapterRowsFromSd(selectedSlug)) {
                copyText(statusText, sizeof(statusText), "CHAPTER LOAD FAILED");
            }
            for (ListItem &cartoon : cartoons) {
                if (std::strcmp(cartoon.id, selectedSlug) == 0) {
                    cartoon.saved = cartoonHasCache(cartoon.id);
                    break;
                }
            }
            return true;
        }
        if (inRect(x, y, 0, READER_TOP + READER_BACK_HEIGHT,
                   XingtaiEpd::WIDTH / 2, READER_HEIGHT - READER_BACK_HEIGHT) &&
            readerPage > 1) {
            --readerPage;
            loadReaderPage();
            refreshMode = RefreshMode::ImageContent;
            return true;
        }
        if (inRect(x, y, XingtaiEpd::WIDTH / 2, READER_TOP + READER_BACK_HEIGHT,
                   XingtaiEpd::WIDTH / 2,
                   READER_HEIGHT - READER_BACK_HEIGHT) &&
            readerPage < readerPageTotal) {
            ++readerPage;
            loadReaderPage();
            refreshMode = RefreshMode::ImageContent;
            return true;
        }
        return false;
    }

    if (view == View::Chapters && inRect(x, y, 4, 34, 58, 18)) {
        for (ListItem &cartoon : cartoons) {
            if (std::strcmp(cartoon.id, selectedSlug) == 0) {
                cartoon.saved = cartoonHasCache(cartoon.id);
                break;
            }
        }
        view = View::Cartoons;
        chapterPayload = "";
        return true;
    }

    const uint16_t offset = view == View::Cartoons ? cartoonOffset : chapterOffset;
    const uint8_t rowCount = view == View::Cartoons ? cartoonCount : chapterRowCount;
    const bool hasMore = view == View::Cartoons ? cartoonHasMore : chapterHasMore;
    if (inRect(x, y, 4, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT) && offset > 0) {
        const bool loaded = view == View::Cartoons
            ? loadCartoons(0) : loadChapterPageFromApi(selectedSlug, 0);
        if (loaded) refreshMode = RefreshMode::ListContent;
        return loaded;
    }
    if (inRect(x, y, 42, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT) && offset > 0) {
        const int32_t previousOffset = max<int32_t>(0, offset - ROWS_PER_PAGE);
        const bool loaded = view == View::Cartoons
            ? loadCartoons(previousOffset)
            : loadChapterPageFromApi(selectedSlug, previousOffset);
        if (loaded) refreshMode = RefreshMode::ListContent;
        return loaded;
    }
    if (inRect(x, y, 164, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT) && hasMore) {
        const int32_t nextOffset = offset + rowCount;
        const bool loaded = view == View::Cartoons
            ? loadCartoons(nextOffset)
            : loadChapterPageFromApi(selectedSlug, nextOffset);
        if (loaded) refreshMode = RefreshMode::ListContent;
        return loaded;
    }
    if (inRect(x, y, 202, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT) && hasMore) {
        const bool loaded = view == View::Cartoons
            ? loadCartoons(-1) : loadChapterPageFromApi(selectedSlug, -1);
        if (loaded) refreshMode = RefreshMode::ListContent;
        return loaded;
    }
    for (uint8_t index = 0; index < ROWS_PER_PAGE; ++index) {
        const int top = LIST_TOP + index * (ROW_HEIGHT + ROW_GAP);
        if (!inRect(x, y, 12, top, 216, ROW_HEIGHT)) continue;
        if (view == View::Cartoons) {
            return index < cartoonCount && loadChapters(cartoons[index]);
        }
        return index < chapterRowCount && openChapter(chapterRows[index]);
    }
    return false;
}

RefreshMode takeRefreshMode() {
    const RefreshMode mode = refreshMode;
    refreshMode = RefreshMode::Layout;
    return mode;
}

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    if (view == View::Reader) {
        renderReader(frame);
        return;
    }
    if (view == View::Chapters) {
        rect(frame, 8, 34, 50, 17);
        if (UiLocalization::isChinese()) drawTitle(frame, 12, 34, 42, 17, "返回");
        else UiLocalization::drawText(frame, 14, 39, "RETURN");
        drawCenteredTitle(frame, 0, 34, XingtaiEpd::WIDTH, 18, selectedComicTitle);
        renderList(frame, chapterRows, chapterRowCount, chapterOffset,
                   chapterPage, pageCount(chapterTotal));
        if (!chapterRowCount) UiLocalization::drawCentered(frame, 200, statusText);
        return;
    }
    if (UiLocalization::isChinese()) drawTitle(frame, 91, 34, 58, 18, "漫画");
    else UiLocalization::drawCentered(frame, 36, "CARTOON", 2);
    renderList(frame, cartoons, cartoonCount, cartoonOffset,
               cartoonPage, pageCount(cartoonTotal));
    if (!cartoonCount) UiLocalization::drawCentered(frame, 200, statusText);
}

} // namespace CartoonPage