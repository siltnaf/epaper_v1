#include "pages/cartoon/cartoon_page.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <TJpg_Decoder.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <cstring>
#include <memory>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "font/xiaozhi_font.h"
#include "ui/loading_indicator.h"
#include "ui/localization.h"

namespace {

constexpr uint8_t ROWS_PER_PAGE = 10;
constexpr uint8_t MAX_CARTOONS = 30;
constexpr int LIST_TOP = 82;
constexpr int ROW_HEIGHT = 30;
constexpr int ROW_GAP = 2;
constexpr int PAGER_TOP = 52;
constexpr int BUTTON_WIDTH = 34;
constexpr int BUTTON_HEIGHT = 25;

struct ListItem {
    char id[64] = {};
    char title[80] = {};
};

enum class View : uint8_t { Cartoons, Chapters, Reader };

View view = View::Cartoons;
char contentBaseUrl[128] = "http://";
ListItem cartoons[MAX_CARTOONS] = {};
ListItem chapterRows[ROWS_PER_PAGE] = {};
uint8_t cartoonCount = 0;
uint8_t chapterRowCount = 0;
uint16_t cartoonPage = 1;
uint16_t chapterPage = 1;
uint16_t chapterTotal = 0;
uint16_t readerPage = 1;
uint16_t readerPageTotal = 1;
char selectedSlug[64] = {};
char selectedComicTitle[80] = {};
char selectedChapterId[32] = {};
char selectedChapterTitle[80] = {};
char statusText[64] = {};
String chapterPayload;
uint8_t *jpegData = nullptr;
size_t jpegSize = 0;
bool imageReady = false;
uint8_t *decodeFrame = nullptr;

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

bool httpGetText(const String &url, String &payload, uint32_t timeout = 20000) {
    UiLoadingIndicator::Scope loading;
    if (WiFi.status() != WL_CONNECTED) {
        copyText(statusText, sizeof(statusText), "WIFI NOT CONNECTED");
        return false;
    }
    HTTPClient http;
    WiFiClient client;
    http.setConnectTimeout(7000);
    http.setTimeout(timeout);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) return false;
    http.addHeader("Connection", "close");
    const int code = http.GET();
    if (code >= 200 && code < 300) payload = http.getString();
    http.end();
    Serial.printf("[CARTOON API] GET code=%d bytes=%u url=%s\n",
                  code, payload.length(), url.c_str());
    return code >= 200 && code < 300;
}

bool loadCartoons() {
    String payload;
    if (!httpGetText(apiBase() + "?limit=" + String(MAX_CARTOONS), payload)) return false;
    JsonDocument document;
    if (deserializeJson(document, payload)) {
        copyText(statusText, sizeof(statusText), "CARTOON JSON FAILED");
        return false;
    }
    cartoonCount = 0;
    for (JsonObject item : document["cartoons"].as<JsonArray>()) {
        if (cartoonCount >= MAX_CARTOONS) break;
        copyText(cartoons[cartoonCount].id, sizeof(cartoons[cartoonCount].id), item["id"] | "");
        copyText(cartoons[cartoonCount].title, sizeof(cartoons[cartoonCount].title),
                 item["name"] | "", "UNTITLED");
        if (cartoons[cartoonCount].id[0]) ++cartoonCount;
    }
    copyText(statusText, sizeof(statusText), cartoonCount ? "" : "NO CARTOONS");
    return cartoonCount > 0;
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
            if (chapterRows[chapterRowCount].id[0]) ++chapterRowCount;
        }
        ++chapterTotal;
        position = objectEnd + 1;
    }
}

bool loadChapters(const ListItem &comic) {
    chapterPayload = "";
    if (!httpGetText(apiBase() + "/" + comic.id, chapterPayload, 30000)) return false;
    copyText(selectedSlug, sizeof(selectedSlug), comic.id);
    copyText(selectedComicTitle, sizeof(selectedComicTitle), comic.title);
    chapterPage = 1;
    loadChapterRows();
    view = View::Chapters;
    return chapterRowCount > 0;
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

bool downloadJpeg(const String &url) {
    UiLoadingIndicator::Scope loading;
    imageReady = false;
    if (jpegData) { free(jpegData); jpegData = nullptr; jpegSize = 0; }
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    WiFiClient client;
    http.setConnectTimeout(7000);
    http.setTimeout(30000);
    if (!http.begin(client, url)) return false;
    http.addHeader("Connection", "close");
    const int code = http.GET();
    const int contentLength = http.getSize();
    if (code < 200 || code >= 300 || contentLength <= 0 || contentLength > 90000) {
        http.end();
        return false;
    }
    jpegData = static_cast<uint8_t *>(malloc(static_cast<size_t>(contentLength)));
    if (!jpegData) { http.end(); return false; }
    WiFiClient *stream = http.getStreamPtr();
    jpegSize = stream->readBytes(jpegData, static_cast<size_t>(contentLength));
    http.end();
    Serial.printf("[CARTOON JPEG] code=%d bytes=%u/%d url=%s\n",
                  code, jpegSize, contentLength, url.c_str());
    return jpegSize == static_cast<size_t>(contentLength);
}

bool loadReaderPage() {
    const String url = apiBase() + "/" + selectedSlug + "/chapter/" +
                       selectedChapterId + "/page/" + String(readerPage);
    if (!downloadJpeg(url)) {
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
    readerPageTotal = 1;
    String manifest;
    if (httpGetText(apiBase() + "/" + selectedSlug + "/chapter/" + selectedChapterId,
                    manifest, 30000)) {
        JsonDocument document;
        if (!deserializeJson(document, manifest)) readerPageTotal = document["page_count"] | 1;
    }
    chapterPayload = "";
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

void renderList(uint8_t *frame, const ListItem *items, uint8_t count, uint16_t page, uint16_t pages) {
    renderPager(frame, page, pages);
    for (uint8_t index = 0; index < count; ++index) {
        const int top = LIST_TOP + index * (ROW_HEIGHT + ROW_GAP);
        rect(frame, 12, top, 216, ROW_HEIGHT);
        char number[8] = {};
        snprintf(number, sizeof(number), "%u", (page - 1) * ROWS_PER_PAGE + index + 1);
        UiLocalization::drawText(frame, 18, top + 9, number);
        drawTitle(frame, 34, top + 1, 171, ROW_HEIGHT - 2, items[index].title);
        drawArrow(frame, 215, top + ROW_HEIGHT / 2, true);
    }
}

void renderReader(uint8_t *frame) {
    rect(frame, 8, 38, 50, 26);
    UiLocalization::drawText(frame, 15, 47, "BACK");
    drawTitle(frame, 66, 39, 164, 24, selectedChapterTitle);
    line(frame, 8, 72, 231, 72);
    if (imageReady && jpegData && jpegSize) {
        uint16_t sourceWidth = 0, sourceHeight = 0;
        TJpgDec.getJpgSize(&sourceWidth, &sourceHeight, jpegData, jpegSize);
        const uint8_t scale = sourceWidth > 240 || sourceHeight > 300 ? 2 : 1;
        const int imageWidth = sourceWidth / scale;
        const int imageHeight = sourceHeight / scale;
        const int drawX = max(0, (XingtaiEpd::WIDTH - imageWidth) / 2);
        const int drawY = 76 + max(0, (300 - imageHeight) / 2);
        decodeFrame = frame;
        TJpgDec.setSwapBytes(false);
        TJpgDec.setJpgScale(scale);
        TJpgDec.setCallback(jpegOutput);
        TJpgDec.drawJpg(drawX, drawY, jpegData, jpegSize);
        decodeFrame = nullptr;
    } else {
        UiLocalization::drawCentered(frame, 210, statusText);
    }
    rect(frame, 8, 386, 34, 24);
    drawArrow(frame, 25, 398, false);
    rect(frame, 198, 386, 34, 24);
    drawArrow(frame, 215, 398, true);
    char counter[20] = {};
    snprintf(counter, sizeof(counter), "%u/%u", readerPage, readerPageTotal);
    UiLocalization::drawCentered(frame, 394, counter);
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
    chapterPayload = "";
    imageReady = false;
    if (jpegData) { free(jpegData); jpegData = nullptr; jpegSize = 0; }
    loadCartoons();
}

bool rowBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                 int16_t &width, int16_t &height) {
    if (view == View::Reader) return false;
    const uint8_t visibleRows = view == View::Cartoons
        ? min<uint16_t>(ROWS_PER_PAGE,
                        cartoonCount > (cartoonPage - 1) * ROWS_PER_PAGE
                            ? cartoonCount - (cartoonPage - 1) * ROWS_PER_PAGE : 0)
        : chapterRowCount;
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
    if (view == View::Reader) {
        if (inRect(x, y, 0, 32, 70, 42)) {
            view = View::Chapters;
            imageReady = false;
            if (jpegData) { free(jpegData); jpegData = nullptr; jpegSize = 0; }
            if (!httpGetText(apiBase() + "/" + selectedSlug, chapterPayload, 30000)) {
                copyText(statusText, sizeof(statusText), "CHAPTER LOAD FAILED");
            }
            loadChapterRows();
            return true;
        }
        if (inRect(x, y, 0, 374, 60, 42) && readerPage > 1) {
            --readerPage;
            loadReaderPage();
            return true;
        }
        if (inRect(x, y, 180, 374, 60, 42) && readerPage < readerPageTotal) {
            ++readerPage;
            loadReaderPage();
            return true;
        }
        return false;
    }

    if (view == View::Chapters && inRect(x, y, 4, 34, 34, 42)) {
        view = View::Cartoons;
        chapterPayload = "";
        return true;
    }

    uint16_t &page = view == View::Cartoons ? cartoonPage : chapterPage;
    const uint16_t pages = view == View::Cartoons ? pageCount(cartoonCount) : pageCount(chapterTotal);
    if (inRect(x, y, 4, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT) && page > 1) {
        page = 1;
        if (view == View::Chapters) loadChapterRows();
        return true;
    }
    if (inRect(x, y, 42, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT) && page > 1) {
        --page;
        if (view == View::Chapters) loadChapterRows();
        return true;
    }
    if (inRect(x, y, 164, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT) && page < pages) {
        ++page;
        if (view == View::Chapters) loadChapterRows();
        return true;
    }
    if (inRect(x, y, 202, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT) && page < pages) {
        page = pages;
        if (view == View::Chapters) loadChapterRows();
        return true;
    }
    for (uint8_t index = 0; index < ROWS_PER_PAGE; ++index) {
        const int top = LIST_TOP + index * (ROW_HEIGHT + ROW_GAP);
        if (!inRect(x, y, 12, top, 216, ROW_HEIGHT)) continue;
        if (view == View::Cartoons) {
            const uint16_t item = (cartoonPage - 1) * ROWS_PER_PAGE + index;
            return item < cartoonCount && loadChapters(cartoons[item]);
        }
        return index < chapterRowCount && openChapter(chapterRows[index]);
    }
    return false;
}

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    if (view == View::Reader) {
        renderReader(frame);
        return;
    }
    if (view == View::Chapters) {
        rect(frame, 8, 38, 28, 26);
        drawArrow(frame, 22, 51, false);
        drawTitle(frame, 82, 39, 148, 24, selectedComicTitle);
        renderList(frame, chapterRows, chapterRowCount, chapterPage, pageCount(chapterTotal));
        return;
    }
    if (UiLocalization::isChinese()) drawTitle(frame, 91, 34, 58, 18, "漫画");
    else UiLocalization::drawCentered(frame, 36, "CARTOON", 2);
    const uint16_t first = (cartoonPage - 1) * ROWS_PER_PAGE;
    const uint8_t visible = first < cartoonCount
        ? min<uint16_t>(ROWS_PER_PAGE, cartoonCount - first) : 0;
    renderList(frame, cartoons + first, visible, cartoonPage, pageCount(cartoonCount));
    if (!cartoonCount) UiLocalization::drawCentered(frame, 200, statusText);
}

} // namespace CartoonPage