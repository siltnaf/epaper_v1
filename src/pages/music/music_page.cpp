#include "pages/music/music_page.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <cstring>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/audio/opus_player.h"
#include "devices/ml307/ml307.h"
#include "devices/sd_card/sd_card.h"
#include "font/xiaozhi_font.h"
#include "ui/loading_indicator.h"
#include "ui/localization.h"

namespace {

constexpr uint8_t ITEMS_PER_PAGE = 10;
constexpr int LIST_TOP = 82;
constexpr int ROW_HEIGHT = 30;
constexpr int ROW_GAP = 2;
constexpr int PAGER_TOP = 52;
constexpr int PAGER_HEIGHT = 25;
constexpr int PAGER_BUTTON_WIDTH = 34;
constexpr char MUSIC_SD_FOLDER[] = "/music";
constexpr int MARQUEE_X = 34;
constexpr int MARQUEE_Y_OFFSET = 2;
constexpr int MARQUEE_WIDTH = 167;
constexpr int MARQUEE_HEIGHT = ROW_HEIGHT - 4;
constexpr int MARQUEE_ROW_BYTES = (MARQUEE_WIDTH + 7) / 8;

struct SongItem {
    int32_t id = 0;
    char title[80] = {};
    char filename[80] = {};
    char source[40] = {};
    bool saved = false;
};

SongItem songs[ITEMS_PER_PAGE] = {};
uint8_t songCount = 0;
int32_t songTotal = 0;
int32_t libraryPage = 1;
int8_t selectedIndex = -1;
int8_t activeSongIndex = -1;
char contentBaseUrl[128] = "http://";
char statusText[64] = "OPENING MUSIC";
bool musicPlaying = false;
bool pendingAudioStart = false;
uint8_t marqueeBitmap[MARQUEE_ROW_BYTES * MARQUEE_HEIGHT] = {};
bool marqueeReady = false;
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
uint16_t marqueeOffset = 0;
uint32_t nextMarqueeMs = 0;
int8_t dirtyRowA = -1;
int8_t dirtyRowB = -1;

void stopAudio();

void pixel(uint8_t *frame, int x, int y) {
    if (x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |=
        0x80U >> (x % 8);
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
    constexpr size_t rowBytes = XingtaiEpd::WIDTH / 8;
    for (int pixelY = y; pixelY < y + height; ++pixelY) {
        uint8_t *row = frame + static_cast<size_t>(pixelY) * rowBytes;
        for (int pixelX = x; pixelX < x + width; ++pixelX) {
            row[pixelX / 8] ^= 0x80U >> (pixelX % 8);
        }
    }
}

bool pointInRect(int16_t x, int16_t y, int left, int top, int width, int height) {
    return x >= left && x < left + width && y >= top && y < top + height;
}

void drawArrow(uint8_t *frame, int centerX, int centerY, bool right) {
    const int direction = right ? 1 : -1;
    line(frame, centerX - direction * 5, centerY - 7,
         centerX + direction * 3, centerY);
    line(frame, centerX + direction * 3, centerY,
         centerX - direction * 5, centerY + 7);
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
    if (!frame || x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) {
        return false;
    }
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
            if (framePixel(frame, MARQUEE_X + x, top + MARQUEE_Y_OFFSET + y)) {
                marqueeBitmap[y * MARQUEE_ROW_BYTES + x / 8] |= 0x80U >> (x % 8);
            }
        }
    }
    marqueeReady = true;
    marqueeOffset = 0;
    nextMarqueeMs = millis() + 900;
}

bool marqueePixel(int x, int y) {
    return (marqueeBitmap[y * MARQUEE_ROW_BYTES + x / 8] &
            (0x80U >> (x % 8))) != 0;
}

void markDirtyRow(int8_t index) {
    if (index < 0 || index >= static_cast<int8_t>(songCount) || dirtyRowA == index ||
        dirtyRowB == index) return;
    if (dirtyRowA < 0) dirtyRowA = index;
    else dirtyRowB = index;
}

bool nextUtf8Codepoint(const char *&cursor, uint32_t &codepoint) {
    const uint8_t *source = reinterpret_cast<const uint8_t *>(cursor);
    if (!source || source[0] == 0) return false;
    if (source[0] < 0x80) {
        codepoint = source[0]; cursor += 1; return true;
    }
    if ((source[0] & 0xE0) == 0xC0 && (source[1] & 0xC0) == 0x80) {
        codepoint = ((source[0] & 0x1F) << 6) | (source[1] & 0x3F);
        cursor += 2; return true;
    }
    if ((source[0] & 0xF0) == 0xE0 && (source[1] & 0xC0) == 0x80 &&
        (source[2] & 0xC0) == 0x80) {
        codepoint = ((source[0] & 0x0F) << 12) | ((source[1] & 0x3F) << 6) |
                    (source[2] & 0x3F);
        cursor += 3; return true;
    }
    codepoint = '?'; ++cursor; return true;
}

void drawChineseGlyph(uint8_t *frame, const XiaozhiFont::Glyph *glyph, int x, int y,
                      int clipRight, int clipBottom) {
    if (!glyph) return;
    for (uint16_t glyphY = 0; glyphY < glyph->height && y + glyphY < clipBottom; ++glyphY) {
        for (uint16_t glyphX = 0; glyphX < glyph->width && x + glyphX < clipRight; ++glyphX) {
            const uint32_t alphaIndex = static_cast<uint32_t>(glyphY) * glyph->width + glyphX;
            const uint8_t packed = glyph->bitmap[alphaIndex / 2U];
            const uint8_t alpha = (alphaIndex & 1U) ? (packed & 0x0FU) : (packed >> 4);
            if (alpha >= 11) pixel(frame, x + glyphX, y + glyphY);
        }
    }
}

void drawUtf8Title(uint8_t *frame, int x, int y, int width, int height,
                   const char *value) {
    const int right = x + width;
    const int bottom = y + height;
    const char *cursor = value;
    int drawX = x;
    uint32_t codepoint = 0;
    while (nextUtf8Codepoint(cursor, codepoint) && drawX < right) {
        if (codepoint < 0x80) {
            char text[2] = {static_cast<char>(codepoint), '\0'};
            UiLocalization::drawText(frame, drawX, y + (height - 7) / 2, text, 1);
            drawX += codepoint == ' ' ? 5 : 7;
            continue;
        }
        const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint);
        if (!glyph) { drawX += 17; continue; }
        const int advance = max(1, static_cast<int>(glyph->advance)) + 1;
        if (drawX + advance > right) break;
        constexpr int FONT_LINE_HEIGHT = 25;
        constexpr int FONT_BASELINE = 6;
        const int fontTop = y + (height - FONT_LINE_HEIGHT) / 2;
        const int glyphTop = fontTop + FONT_LINE_HEIGHT - FONT_BASELINE -
                             glyph->height - glyph->offsetY;
        drawChineseGlyph(frame, glyph, drawX + glyph->offsetX, glyphTop, right, bottom);
        drawX += advance;
    }
}

void copyUtf8(char *destination, size_t destinationSize, const char *source,
              const char *fallback) {
    if (!destination || destinationSize == 0) return;
    const char *value = source && source[0] ? source : fallback;
    value = value ? value : "";
    size_t input = 0, output = 0;
    while (value[input] && output + 1 < destinationSize) {
        const uint8_t first = static_cast<uint8_t>(value[input]);
        size_t sequenceLength = 1;
        if ((first & 0xE0U) == 0xC0U) sequenceLength = 2;
        else if ((first & 0xF0U) == 0xE0U) sequenceLength = 3;
        else if ((first & 0xF8U) == 0xF0U) sequenceLength = 4;
        if (output + sequenceLength >= destinationSize) break;
        bool valid = true;
        for (size_t byte = 1; byte < sequenceLength; ++byte) {
            if (!value[input + byte] ||
                (static_cast<uint8_t>(value[input + byte]) & 0xC0U) != 0x80U) {
                valid = false; break;
            }
        }
        if (!valid) break;
        for (size_t byte = 0; byte < sequenceLength; ++byte) {
            destination[output++] = value[input++];
        }
    }
    destination[output] = '\0';
}

String endpointBase() {
    String base(contentBaseUrl);
    base.trim();
    while (base.endsWith("/")) base.remove(base.length() - 1);
    const int query = base.indexOf('?');
    if (query >= 0) base.remove(query);
    const int songsPath = base.indexOf("/api/kid-songs");
    if (songsPath >= 0) return base.substring(0, songsPath) + "/api/kid-songs";
    const int scheme = base.indexOf("://");
    if (scheme >= 0) {
        const int path = base.indexOf('/', scheme + 3);
        if (path >= 0) base.remove(path);
    }
    return base + "/api/kid-songs";
}

bool httpGet(const String &url, String &payload, bool showLoading = true) {
    OptionalLoadingScope loadingIndicator(showLoading);
    Serial.printf("[MUSIC API] request method=GET url=%s wifi_status=%d ip=%s heap=%u\n",
                  url.c_str(), static_cast<int>(WiFi.status()),
                  WiFi.localIP().toString().c_str(), static_cast<unsigned>(ESP.getFreeHeap()));
    if (WiFi.status() != WL_CONNECTED) {
        if (cellularModem.httpGet(url.c_str(), payload)) return true;
        std::strcpy(statusText, UiLocalization::isChinese() ? "网络未连接" : "WIFI NOT CONNECTED");
        return false;
    }

    HTTPClient http;
    http.setConnectTimeout(7000);
    http.setTimeout(20000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    secureClient.setInsecure();
    const bool began = url.startsWith("https://")
        ? http.begin(secureClient, url) : http.begin(plainClient, url);
    if (!began) { std::strcpy(statusText, "INVALID CONTENT URL"); return false; }
    http.addHeader("Connection", "close");
    http.addHeader("User-Agent", "ESP32-ePaper-Music/1.0");
    const int code = http.GET();
    if (code >= 200 && code < 300) payload = http.getString();
    const String error = code < 0 ? http.errorToString(code) : String();
    http.end();
    Serial.printf("[MUSIC API] response code=%d%s%s bytes=%u url=%s\n",
                  code, error.isEmpty() ? "" : " ", error.c_str(), payload.length(), url.c_str());
    if (code < 0) snprintf(statusText, sizeof(statusText), "HTTP ERROR %d", code);
    else if (code < 200 || code >= 300) snprintf(statusText, sizeof(statusText), "SERVER HTTP %d", code);
    return code >= 200 && code < 300;
}

bool isSongSavedOnSd(int32_t songId) {
    if (!SdCard::isMounted() || songId <= 0) return false;
    char metaPath[64] = {};
    snprintf(metaPath, sizeof(metaPath), "%s/%ld/meta.txt", MUSIC_SD_FOLDER,
             static_cast<long>(songId));
    File meta = SD_MMC.open(metaPath, FILE_READ);
    if (!meta) return false;
    meta.readStringUntil('\n');
    meta.readStringUntil('\n');
    String filename = meta.readStringUntil('\n');
    meta.close();
    filename.trim();
    if (filename.isEmpty()) return false;
    char audioPath[144] = {};
    snprintf(audioPath, sizeof(audioPath), "%s/%ld/%s", MUSIC_SD_FOLDER,
             static_cast<long>(songId), filename.c_str());
    return SD_MMC.exists(audioPath);
}

bool cachedSongPath(int32_t songId, char *path, size_t pathSize) {
    if (!path || pathSize == 0 || songId <= 0 || !SdCard::isMounted()) return false;
    snprintf(path, pathSize, "%s/%ld/song.opus", MUSIC_SD_FOLDER,
             static_cast<long>(songId));
    if (SdCard::isValidOggOpus(path)) return true;
    SD_MMC.remove(path);
    return false;
}

bool ensureSongDirectory(int32_t songId, char *directory, size_t directorySize) {
    if (!SdCard::isMounted() || songId <= 0 || !directory || directorySize == 0) return false;
    if (!SD_MMC.exists(MUSIC_SD_FOLDER) && !SD_MMC.mkdir(MUSIC_SD_FOLDER)) return false;
    snprintf(directory, directorySize, "%s/%ld", MUSIC_SD_FOLDER,
             static_cast<long>(songId));
    return SD_MMC.exists(directory) || SD_MMC.mkdir(directory);
}

bool downloadSong(const String &url, const char *path) {
    if (!path || WiFi.status() != WL_CONNECTED || !SdCard::isMounted()) return false;
    UiLoadingIndicator::Scope loadingIndicator;
    if (!SdCard::downloadFile(url.c_str(), path, 1024) || !SdCard::isValidOggOpus(path)) {
        SD_MMC.remove(path);
        Serial.printf("[MUSIC AUDIO] Invalid or incomplete Ogg Opus download path=%s\n", path);
        return false;
    }
    File file = SD_MMC.open(path, FILE_READ);
    const size_t total = file ? file.size() : 0;
    if (file) file.close();
    Serial.printf("[MUSIC AUDIO] download complete bytes=%u path=%s\n", total, path);
    return true;
}

bool ensureSongOpus(uint8_t index, char *path, size_t pathSize) {
    if (index >= songCount) return false;
    SongItem &song = songs[index];
    if (cachedSongPath(song.id, path, pathSize) && isSongSavedOnSd(song.id)) {
        song.saved = true;
        Serial.printf("[MUSIC AUDIO] Using cache id=%ld path=%s\n",
                      static_cast<long>(song.id), path);
        return true;
    }
    if (WiFi.status() != WL_CONNECTED || !SdCard::isMounted()) return false;
    char directory[48] = {};
    if (!ensureSongDirectory(song.id, directory, sizeof(directory))) return false;
    snprintf(path, pathSize, "%s/song.opus", directory);
    const String url = endpointBase() + "/" + String(song.id) + "/opus";
    Serial.printf("[MUSIC AUDIO] Downloading id=%ld url=%s -> %s\n",
                  static_cast<long>(song.id), url.c_str(), path);
    if (!downloadSong(url, path) || !cachedSongPath(song.id, path, pathSize)) return false;

    char metaPath[72] = {};
    snprintf(metaPath, sizeof(metaPath), "%s/meta.txt", directory);
    SD_MMC.remove(metaPath);
    File meta = SD_MMC.open(metaPath, FILE_WRITE);
    if (!meta) {
        Serial.printf("[MUSIC SD] Could not write metadata id=%ld path=%s\n",
                      static_cast<long>(song.id), metaPath);
        SD_MMC.remove(path);
        return false;
    }
    meta.printf("%ld\n%s\nsong.opus\n%s\n%s\n", static_cast<long>(song.id),
                song.title, song.source, url.c_str());
    meta.flush();
    meta.close();
    if (!isSongSavedOnSd(song.id)) {
        Serial.printf("[MUSIC SD] Saved song verification failed id=%ld\n",
                      static_cast<long>(song.id));
        SD_MMC.remove(metaPath);
        SD_MMC.remove(path);
        return false;
    }
    Serial.printf("[MUSIC SD] Saved offline copy id=%ld title=%s path=%s\n",
                  static_cast<long>(song.id), song.title, path);
    song.saved = true;
    return true;
}

bool startSongAudio() {
    if (activeSongIndex < 0 || activeSongIndex >= static_cast<int8_t>(songCount)) return false;
    UiLoadingIndicator::Scope loadingIndicator;
    char path[128] = {};
    if (!ensureSongOpus(static_cast<uint8_t>(activeSongIndex), path, sizeof(path))) return false;
    musicPlaying = OpusPlayer::play(path);
    return musicPlaying;
}

bool loadSavedSongs() {
    if (!SdCard::isMounted()) return false;
    File root = SD_MMC.open(MUSIC_SD_FOLDER);
    if (!root || !root.isDirectory()) return false;
    songCount = 0;
    File entry = root.openNextFile();
    while (entry && songCount < ITEMS_PER_PAGE) {
        if (entry.isDirectory()) {
            String entryName = entry.name();
            const int slash = entryName.lastIndexOf('/');
            const int32_t id = entryName.substring(slash + 1).toInt();
            if (id <= 0) {
                entry.close();
                entry = root.openNextFile();
                continue;
            }
            // SD_MMC can report either "/music/12" or "12" depending on
            // the Arduino core version. Always rebuild the absolute path.
            char directory[64] = {};
            snprintf(directory, sizeof(directory), "%s/%ld", MUSIC_SD_FOLDER,
                     static_cast<long>(id));
            char metaPath[96] = {};
            snprintf(metaPath, sizeof(metaPath), "%s/meta.txt", directory);
            File meta = SD_MMC.open(metaPath, FILE_READ);
            if (meta) {
                meta.readStringUntil('\n');
                const String title = meta.readStringUntil('\n');
                const String filename = meta.readStringUntil('\n');
                const String source = meta.readStringUntil('\n');
                meta.close();
                if (isSongSavedOnSd(id)) {
                    SongItem &song = songs[songCount++];
                    song.id = id;
                    copyUtf8(song.title, sizeof(song.title), title.c_str(), "UNTITLED");
                    copyUtf8(song.filename, sizeof(song.filename), filename.c_str(), "");
                    copyUtf8(song.source, sizeof(song.source), source.c_str(), "");
                    song.saved = true;
                }
            }
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
    songTotal = songCount;
    if (songCount == 0) return false;
    snprintf(statusText, sizeof(statusText), "%u SAVED SONGS", songCount);
    Serial.printf("[MUSIC SD] Offline playlist items=%u\n", songCount);
    return true;
}

bool loadLibrary(bool showLoading = true) {
    OptionalLoadingScope loadingIndicator(showLoading);
    OpusPlayer::stop();
    musicPlaying = false;
    pendingAudioStart = false;
    activeSongIndex = -1;
    marqueeReady = false;
    songCount = 0;
    selectedIndex = -1;
    // With Wi-Fi disabled, do not spend time making a network request. The
    // SD library is the authoritative source for previously played songs.
    if (WiFi.status() != WL_CONNECTED && loadSavedSongs()) return true;

    String payload;
    const String url = endpointBase() + "?page=" + String(libraryPage) +
                       "&perPage=" + String(ITEMS_PER_PAGE);
    if (!httpGet(url, payload, showLoading)) return loadSavedSongs();

    Serial.printf("[MUSIC] Library payload preview: %.180s\n", payload.c_str());
    JsonDocument document;
    const DeserializationError error = deserializeJson(document, payload);
    if (error) {
        snprintf(statusText, sizeof(statusText), "JSON %s", error.c_str());
        return loadSavedSongs();
    }
    songTotal = document["total"] | 0;
    JsonArray items = document["items"].as<JsonArray>();
    if (items.isNull()) { std::strcpy(statusText, "NO ITEMS IN JSON"); return loadSavedSongs(); }
    for (JsonObject item : items) {
        if (songCount >= ITEMS_PER_PAGE) break;
        SongItem &song = songs[songCount];
        song.id = item["id"] | 0;
        char fallback[24] = {};
        snprintf(fallback, sizeof(fallback), "SONG %ld", static_cast<long>(song.id));
        copyUtf8(song.title, sizeof(song.title), item["title"] | "", fallback);
        copyUtf8(song.filename, sizeof(song.filename), item["filename"] | "", "");
        copyUtf8(song.source, sizeof(song.source), item["source"] | "", "");
        song.saved = isSongSavedOnSd(song.id);
        Serial.printf("[MUSIC API] parsed item index=%u id=%ld title=%s source=%s\n",
                      songCount, static_cast<long>(song.id), song.title, song.source);
        if (song.id > 0) ++songCount;
    }
    if (songCount == 0) { std::strcpy(statusText, "NO SONGS FOUND"); return false; }
    snprintf(statusText, sizeof(statusText), "%u SONGS", songCount);
    return true;
}

void libraryLoadTask(void *) {
    loadLibrary(false);
    libraryAwaitingContent = false;
    libraryLoadRunning = false;
    libraryLoadCompleted = true;
    vTaskDelete(nullptr);
}

int libraryPageCount() {
    return songTotal > 0 ? (songTotal + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE : 1;
}

void renderLibrary(uint8_t *frame) {
    if (UiLocalization::isChinese()) drawUtf8Title(frame, 91, 34, 58, 18, "儿歌");
    else UiLocalization::drawCentered(frame, 36, "MUSIC", 2);

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
    UiLocalization::drawCentered(frame, 61, pager);

    if (songCount == 0) {
        if (libraryAwaitingContent) return;
        UiLocalization::drawCentered(frame, 190, statusText);
        UiLocalization::drawCentered(frame, 215,
            UiLocalization::isChinese() ? "检查网络设置" : "CHECK WIFI AND URL");
        return;
    }
    for (uint8_t index = 0; index < songCount; ++index) {
        const int top = LIST_TOP + index * (ROW_HEIGHT + ROW_GAP);
        rect(frame, 12, top, 216, ROW_HEIGHT);
        char number[8] = {};
        snprintf(number, sizeof(number), "%u",
                 static_cast<unsigned>((libraryPage - 1) * ITEMS_PER_PAGE + index + 1));
        UiLocalization::drawText(frame, 18, top + 9, number, 1);
        drawUtf8Title(frame, 34, top + 1, 167, ROW_HEIGHT - 2, songs[index].title);
        if (index == activeSongIndex) {
            drawPlayPause(frame, 215, top + ROW_HEIGHT / 2, true);
            if (!marqueeReady) captureMarquee(frame, top);
        } else if (songs[index].saved) drawCheckmark(frame, 215, top + ROW_HEIGHT / 2);
        else drawArrow(frame, 215, top + ROW_HEIGHT / 2, true);
        if (index == activeSongIndex || index == selectedIndex) {
            invertRect(frame, 12, top, 216, ROW_HEIGHT);
        }
    }
}

bool changePage(int32_t nextPage) {
    if (nextPage < 1 || nextPage > libraryPageCount() || nextPage == libraryPage) return false;
    const int32_t previousPage = libraryPage;
    libraryPage = nextPage;
    if (!loadLibrary()) {
        libraryPage = previousPage;
        loadLibrary();
        return false;
    }
    return true;
}

}

namespace MusicPage {

void setContentUrl(const char *url) {
    std::strncpy(contentBaseUrl, url ? url : "", sizeof(contentBaseUrl) - 1);
    contentBaseUrl[sizeof(contentBaseUrl) - 1] = '\0';
}

void open() {
    OpusPlayer::stop();
    musicPlaying = false;
    pendingAudioStart = false;
    activeSongIndex = -1;
    dirtyRowA = -1;
    dirtyRowB = -1;
    libraryPage = 1;
    selectedIndex = -1;
    std::strcpy(statusText, UiLocalization::isChinese() ? "正在获取歌曲" : "LOADING SONGS");
    songCount = 0;
    songTotal = 0;
    libraryAwaitingContent = true;
    libraryLoadCompleted = false;
}

bool startLibraryLoad() {
    if (libraryLoadRunning) return false;
    libraryLoadRunning = true;
    libraryLoadCompleted = false;
    if (xTaskCreate(libraryLoadTask, "music-library", 8192, nullptr, 1, nullptr) != pdPASS) {
        libraryLoadRunning = false;
        libraryAwaitingContent = false;
        std::strcpy(statusText, "MUSIC TASK FAILED");
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

bool handleTap(int16_t x, int16_t y) {
    if (pointInRect(x, y, 4, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT)) return changePage(1);
    if (pointInRect(x, y, 42, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT)) return changePage(libraryPage - 1);
    if (pointInRect(x, y, 164, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT)) return changePage(libraryPage + 1);
    if (pointInRect(x, y, 202, PAGER_TOP, PAGER_BUTTON_WIDTH, PAGER_HEIGHT)) return changePage(libraryPageCount());
    for (uint8_t index = 0; index < songCount; ++index) {
        const int top = LIST_TOP + index * (ROW_HEIGHT + ROW_GAP);
        if (pointInRect(x, y, 12, top, 216, ROW_HEIGHT)) {
            if (activeSongIndex == static_cast<int8_t>(index)) {
                markDirtyRow(activeSongIndex);
                stopAudio();
                return true;
            }
            const int8_t previousIndex = activeSongIndex;
            stopAudio();
            markDirtyRow(previousIndex);
            selectedIndex = static_cast<int8_t>(index);
            activeSongIndex = static_cast<int8_t>(index);
            markDirtyRow(activeSongIndex);
            marqueeReady = false;
            marqueeOffset = 0;
            pendingAudioStart = true;
            Serial.printf("[MUSIC] Selected index=%u id=%ld title=%s saved=%s\n",
                          index, static_cast<long>(songs[index].id), songs[index].title,
                          songs[index].saved ? "yes" : "no");
            return true;
        }
    }
    return false;
}

bool processAudio() {
    if (pendingAudioStart) {
        pendingAudioStart = false;
        if (!startSongAudio()) {
            markDirtyRow(activeSongIndex);
            activeSongIndex = -1;
            selectedIndex = -1;
            marqueeReady = false;
            return true;
        }
        return false;
    }
    if (!musicPlaying) return false;
    if (OpusPlayer::isPlaying()) return false;
    musicPlaying = false;
    markDirtyRow(activeSongIndex);
    activeSongIndex = -1;
    selectedIndex = -1;
    marqueeReady = false;
    return true;
}

bool isAudioActive() {
    return pendingAudioStart || musicPlaying || OpusPlayer::isPlaying();
}

void stopAudioFromTouchInterrupt() {
    const int8_t interruptedIndex = activeSongIndex;
    markDirtyRow(activeSongIndex);
    pendingAudioStart = false;
    OpusPlayer::stop();
    musicPlaying = false;
    // Keep the interrupted row identity until handleTap() receives coordinates.
    activeSongIndex = interruptedIndex;
    selectedIndex = interruptedIndex;
    marqueeReady = false;
    marqueeOffset = 0;
}

void stopAudio() {
    pendingAudioStart = false;
    OpusPlayer::stop();
    musicPlaying = false;
    activeSongIndex = -1;
    selectedIndex = -1;
    marqueeReady = false;
    marqueeOffset = 0;
}

bool takeDirtyRows(int8_t &firstRow, int8_t &secondRow) {
    firstRow = dirtyRowA;
    secondRow = dirtyRowB;
    dirtyRowA = -1;
    dirtyRowB = -1;
    return firstRow >= 0;
}

bool advanceMarquee(int16_t &rowTop) {
    if (!musicPlaying || activeSongIndex < 0 || !marqueeReady || millis() < nextMarqueeMs) {
        return false;
    }
    marqueeOffset = (marqueeOffset + 12) % MARQUEE_WIDTH;
    nextMarqueeMs = millis() + 900;
    rowTop = LIST_TOP + activeSongIndex * (ROW_HEIGHT + ROW_GAP);
    return true;
}

void renderMarquee(uint8_t *destination, const uint8_t *currentFrame) {
    if (!destination || !currentFrame) return;
    std::memcpy(destination, currentFrame, XingtaiEpd::FRAME_BYTES);
    if (activeSongIndex < 0 || !marqueeReady) return;
    const int top = LIST_TOP + activeSongIndex * (ROW_HEIGHT + ROW_GAP);
    for (int y = top + MARQUEE_Y_OFFSET;
         y < top + MARQUEE_Y_OFFSET + MARQUEE_HEIGHT; ++y) {
        for (int x = MARQUEE_X; x < MARQUEE_X + MARQUEE_WIDTH; ++x) {
            destination[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |=
                0x80U >> (x % 8);
        }
    }
    for (int y = 0; y < MARQUEE_HEIGHT; ++y) {
        for (int x = 0; x < MARQUEE_WIDTH; ++x) {
            const int sourceX = (x + marqueeOffset) % MARQUEE_WIDTH;
            if (marqueePixel(sourceX, y)) {
                const int drawX = MARQUEE_X + x;
                const int drawY = top + MARQUEE_Y_OFFSET + y;
                destination[static_cast<size_t>(drawY) * (XingtaiEpd::WIDTH / 8) + drawX / 8] &=
                    static_cast<uint8_t>(~(0x80U >> (drawX % 8)));
            }
        }
    }
}

bool handleSwipe(int16_t deltaX, int16_t deltaY) {
    constexpr int16_t SWIPE_THRESHOLD = 35;
    const bool horizontal = abs(deltaX) >= abs(deltaY);
    const int16_t primaryDelta = horizontal ? deltaX : deltaY;
    if (abs(primaryDelta) < SWIPE_THRESHOLD) return false;
    const bool changed = changePage(libraryPage + (primaryDelta < 0 ? 1 : -1));
    if (changed) {
        Serial.printf("[MUSIC SWIPE] page=%ld axis=%s\n", static_cast<long>(libraryPage),
                      horizontal ? "horizontal" : "vertical");
    }
    return changed;
}

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    renderLibrary(frame);
}

}