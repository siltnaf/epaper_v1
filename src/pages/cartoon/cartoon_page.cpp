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
#include "devices/sd_card/sd_card.h"
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
constexpr char CARTOON_SD_FOLDER[] = "/cartoon";
constexpr char READER_JPEG_PATH[] = "/cartoon/reader.jpg";
constexpr char READER_JPEG_PART_PATH[] = "/cartoon/reader.jpg.part";

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
    payload = static_cast<const char *>(nullptr);
    for (uint8_t attempt = 1; attempt <= 2; ++attempt) {
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
        if (code >= 200 && code < 300) payload = http.getString();
        const String error = code < 0 ? http.errorToString(code) : String();
        http.end();
        Serial.printf("[CARTOON API] GET attempt=%u/2 code=%d%s%s bytes=%u "
                      "heap=%u largest=%u url=%s\n",
                      attempt, code, error.isEmpty() ? "" : " ", error.c_str(),
                      payload.length(), static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(ESP.getMaxAllocHeap()), url.c_str());
        if (code >= 200 && code < 300) return true;
        if (code >= 0 || attempt == 2) break;
        delay(350);
    }
    copyText(statusText, sizeof(statusText), "CARTOON NETWORK ERROR");
    return false;
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
    copyText(selectedSlug, sizeof(selectedSlug), comic.id);
    copyText(selectedComicTitle, sizeof(selectedComicTitle), comic.title);
    chapterPage = 1;
    chapterRowCount = 0;
    chapterTotal = 0;
    view = View::Chapters;
    copyText(statusText, sizeof(statusText), "LOADING CHAPTERS");
    chapterPayload = static_cast<const char *>(nullptr);
    if (!httpGetText(apiBase() + "/" + comic.id, chapterPayload, 30000)) {
        copyText(statusText, sizeof(statusText), "CHAPTER LOAD FAILED");
        Serial.printf("[CARTOON] Chapter page opened without data comic=%s\n", comic.id);
        return true;
    }
    loadChapterRows();
    copyText(statusText, sizeof(statusText), chapterRowCount ? "" : "NO CHAPTERS");
    Serial.printf("[CARTOON] Chapter page comic=%s total=%u visible=%u payload=%u\n",
                  comic.id, chapterTotal, chapterRowCount, chapterPayload.length());
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
            while ((sourceWidth / outputScale > 240 || sourceHeight / outputScale > 300) &&
                   outputScale < 8) outputScale <<= 1;
            const int outputWidth = max<int>(1, sourceWidth / outputScale);
            const int outputHeight = max<int>(1, sourceHeight / outputScale);
            const int drawX = max(0, (XingtaiEpd::WIDTH - outputWidth) / 2);
            const int drawY = 76 + max(0, (300 - outputHeight) / 2);
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

bool downloadJpeg(const String &url) {
    UiLoadingIndicator::Scope loading;
    imageReady = false;
    jpegSize = 0;
    if (WiFi.status() != WL_CONNECTED || !ensureCartoonDirectory()) {
        copyText(statusText, sizeof(statusText), "SD OR WIFI NOT READY");
        return false;
    }
    SD_MMC.remove(READER_JPEG_PART_PATH);
    SD_MMC.remove(READER_JPEG_PATH);
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
            File output = SD_MMC.open(READER_JPEG_PART_PATH, FILE_WRITE);
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
            if (SD_MMC.rename(READER_JPEG_PART_PATH, READER_JPEG_PATH)) {
                Serial.printf("[CARTOON SD] Saved image path=%s bytes=%u\n",
                              READER_JPEG_PATH, static_cast<unsigned>(jpegSize));
                return true;
            }
            Serial.println("[CARTOON JPEG] Could not finalize SD image file");
        }
        SD_MMC.remove(READER_JPEG_PART_PATH);
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
    // Release the retained chapter-list allocation before the manifest/JPEG
    // buffers compete for the constrained internal heap.
    chapterPayload = static_cast<const char *>(nullptr);
    readerPageTotal = fetchReaderPageCount(
        apiBase() + "/" + selectedSlug + "/chapter/" + selectedChapterId);
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
    if (imageReady && jpegSize && SD_MMC.exists(READER_JPEG_PATH)) {
        uint16_t sourceWidth = 0, sourceHeight = 0;
        const JRESULT sizeResult = TJpgDec.getFsJpgSize(
            &sourceWidth, &sourceHeight, READER_JPEG_PATH, SD_MMC);
        if (sizeResult == JDR_OK) {
            const uint8_t scale = sourceWidth > 240 || sourceHeight > 300 ? 2 : 1;
            const int imageWidth = sourceWidth / scale;
            const int imageHeight = sourceHeight / scale;
            const int drawX = max(0, (XingtaiEpd::WIDTH - imageWidth) / 2);
            const int drawY = 76 + max(0, (300 - imageHeight) / 2);
            decodeFrame = frame;
            TJpgDec.setSwapBytes(false);
            TJpgDec.setJpgScale(scale);
            TJpgDec.setCallback(jpegOutput);
            const JRESULT drawResult = TJpgDec.drawFsJpg(
                drawX, drawY, READER_JPEG_PATH, SD_MMC);
            decodeFrame = nullptr;
            Serial.printf("[CARTOON JPEG] Decode baseline size_result=%d draw_result=%d "
                          "source=%ux%u scale=%u file=%u\n",
                          static_cast<int>(sizeResult), static_cast<int>(drawResult),
                          sourceWidth, sourceHeight, scale, static_cast<unsigned>(jpegSize));
        } else if (sizeResult == JDR_FMT3) {
            uint8_t previewScale = 1;
            const bool previewReady = drawProgressiveGrayscalePreview(
                frame, READER_JPEG_PATH, sourceWidth, sourceHeight, previewScale);
            Serial.printf("[CARTOON JPEG] Decode progressive_dc result=%s source=%ux%u "
                          "scale=%u file=%u\n",
                          previewReady ? "ok" : "failed", sourceWidth, sourceHeight,
                          previewScale, static_cast<unsigned>(jpegSize));
            if (!previewReady)
                UiLocalization::drawCentered(frame, 210, "UNSUPPORTED JPEG");
        } else {
            Serial.printf("[CARTOON JPEG] Decode failed size_result=%d file=%u\n",
                          static_cast<int>(sizeResult), static_cast<unsigned>(jpegSize));
            UiLocalization::drawCentered(frame, 210, "JPEG DECODE FAILED");
        }
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
    jpegSize = 0;
    if (ensureCartoonDirectory()) {
        SD_MMC.remove(READER_JPEG_PART_PATH);
        SD_MMC.remove(READER_JPEG_PATH);
    }
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
            jpegSize = 0;
            SD_MMC.remove(READER_JPEG_PART_PATH);
            SD_MMC.remove(READER_JPEG_PATH);
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
        if (!chapterRowCount) UiLocalization::drawCentered(frame, 200, statusText);
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