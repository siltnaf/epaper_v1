#include "pages/radio/radio_page.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourceHTTPStream.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_heap_caps.h>

#include <cstring>

#include "board_pins.h"
#include "devices/audio/opus_player.h"
#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/es8311/es8311.h"
#include "font/xiaozhi_font.h"
#include "ui/loading_indicator.h"
#include "ui/localization.h"

namespace {

constexpr uint8_t ITEMS_PER_PAGE = 10;
constexpr uint8_t MAX_STATIONS = 20;
constexpr uint16_t STREAM_BUFFER_BYTES = 2048;
constexpr size_t MP3_DECODER_WORKSPACE_BYTES = AudioGeneratorMP3::preAllocSize();
// libmad and the HTTP client have deep call chains. A 4 KiB worker stack can
// overflow into adjacent heap allocations as soon as MP3 decoding begins.
constexpr uint16_t PLAYBACK_TASK_STACK_BYTES = 12288;
constexpr int LIST_TOP = 82;
constexpr int ROW_HEIGHT = 30;
constexpr int ROW_GAP = 2;
constexpr int PAGER_TOP = 52;
constexpr int BUTTON_WIDTH = 34;
constexpr int BUTTON_HEIGHT = 25;

struct Station {
    char name[80] = {};
    char codec[12] = {};
    char streamPath[320] = {};
};

class RadioAudioOutput final : public AudioOutput {
public:
    explicit RadioAudioOutput(Es8311 *audio) : audio_(audio) {}

    bool begin() override {
        active_ = audio_ && audio_->isInitialized();
        phase_ = 0;
        bufferedSamples_ = 0;
        if (active_) audio_->setSpeakerEnabled(true);
        return active_;
    }

    bool SetRate(int hz) override {
        sourceRate_ = hz;
        return hz >= 8000 && hz <= 96000 && AudioOutput::SetRate(hz);
    }

    bool SetBitsPerSample(int bits) override {
        return (bits == 8 || bits == 16) && AudioOutput::SetBitsPerSample(bits);
    }

    bool SetChannels(int channels) override {
        return (channels == 1 || channels == 2) && AudioOutput::SetChannels(channels);
    }

    void resetQuota() { sourceFramesRemaining_ = 1024; }

    bool ConsumeSample(int16_t sample[2]) override {
        if (!active_ || sourceRate_ <= 0 || sourceFramesRemaining_ == 0) return false;
        --sourceFramesRemaining_;
        MakeSampleStereo16(sample);
        phase_ += Es8311::DEFAULT_SAMPLE_RATE;
        while (phase_ >= static_cast<uint32_t>(sourceRate_)) {
            phase_ -= static_cast<uint32_t>(sourceRate_);
            pcm_[bufferedSamples_++] = Amplify(sample[LEFTCHANNEL]);
            pcm_[bufferedSamples_++] = Amplify(sample[RIGHTCHANNEL]);
            if (bufferedSamples_ >= PCM_CAPACITY && !flushBuffer()) return false;
        }
        return true;
    }

    bool stop() override {
        flush();
        active_ = false;
        return true;
    }

    void flush() override { flushBuffer(); }

private:
    bool flushBuffer() {
        if (bufferedSamples_ == 0) return true;
        const bool written = audio_ &&
            audio_->write(pcm_, bufferedSamples_, 1000) == bufferedSamples_;
        bufferedSamples_ = 0;
        return written;
    }

    static constexpr uint16_t PCM_CAPACITY = 512;
    Es8311 *audio_ = nullptr;
    int16_t pcm_[PCM_CAPACITY] = {};
    uint16_t bufferedSamples_ = 0;
    uint16_t sourceFramesRemaining_ = 0;
    uint32_t phase_ = 0;
    int sourceRate_ = 0;
    bool active_ = false;
};

Station stations[MAX_STATIONS] = {};
uint8_t stationCount = 0;
uint8_t libraryPage = 1;
int8_t selectedIndex = -1;
int8_t activeIndex = -1;
int8_t pendingIndex = -1;
char contentBaseUrl[128] = "http://";
char locatedCity[48] = {};
char statusText[64] = {};
Es8311 *codec = nullptr;

TaskHandle_t playbackTaskHandle = nullptr;
StaticTask_t playbackTaskBuffer = {};
StackType_t playbackTaskStack[PLAYBACK_TASK_STACK_BYTES / sizeof(StackType_t)] = {};
volatile bool stopRequested = false;
volatile bool playbackActive = false;
volatile bool playbackEnded = false;
char requestedUrl[384] = {};
// Keep the fixed libmad workspace out of the fragmented runtime heap. It is
// shared only by the radio playback task and is released logically on cleanup.
alignas(8) uint8_t decoderWorkspace[MP3_DECODER_WORKSPACE_BYTES] = {};

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

void invertRect(uint8_t *frame, int x, int y, int width, int height) {
    constexpr size_t rowBytes = XingtaiEpd::WIDTH / 8;
    for (int py = y; py < y + height; ++py) {
        uint8_t *row = frame + static_cast<size_t>(py) * rowBytes;
        for (int px = x; px < x + width; ++px) row[px / 8] ^= 0x80U >> (px % 8);
    }
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

void drawPlay(uint8_t *frame, int centerX, int centerY) {
    line(frame, centerX - 5, centerY - 7, centerX - 5, centerY + 7);
    line(frame, centerX - 5, centerY - 7, centerX + 6, centerY);
    line(frame, centerX + 6, centerY, centerX - 5, centerY + 7);
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

bool nextCodepoint(const char *&cursor, uint32_t &codepoint) {
    const uint8_t *source = reinterpret_cast<const uint8_t *>(cursor);
    if (!source || !source[0]) return false;
    if (source[0] < 0x80) { codepoint = source[0]; ++cursor; return true; }
    if ((source[0] & 0xE0) == 0xC0) {
        codepoint = ((source[0] & 0x1F) << 6) | (source[1] & 0x3F); cursor += 2; return true;
    }
    if ((source[0] & 0xF0) == 0xE0) {
        codepoint = ((source[0] & 0x0F) << 12) | ((source[1] & 0x3F) << 6) |
                    (source[2] & 0x3F); cursor += 3; return true;
    }
    codepoint = '?'; ++cursor; return true;
}

void drawTitle(uint8_t *frame, int x, int y, int width, int height, const char *value) {
    const int right = x + width;
    const int bottom = y + height;
    const char *cursor = value ? value : "";
    int drawX = x;
    uint32_t codepoint = 0;
    while (nextCodepoint(cursor, codepoint) && drawX < right) {
        if (codepoint < 0x80) {
            char text[2] = {static_cast<char>(codepoint), '\0'};
            UiLocalization::drawText(frame, drawX, y + (height - 7) / 2, text);
            drawX += codepoint == ' ' ? 5 : 7;
            continue;
        }
        const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint);
        if (!glyph) { drawX += 17; continue; }
        const int advance = max(1, static_cast<int>(glyph->advance)) + 1;
        if (drawX + advance > right) break;
        const int glyphTop = y + (height - 25) / 2 + 19 - glyph->height - glyph->offsetY;
        for (uint16_t gy = 0; gy < glyph->height && glyphTop + gy < bottom; ++gy) {
            for (uint16_t gx = 0; gx < glyph->width && drawX + glyph->offsetX + gx < right; ++gx) {
                const uint32_t index = static_cast<uint32_t>(gy) * glyph->width + gx;
                const uint8_t packed = glyph->bitmap[index / 2U];
                const uint8_t alpha = (index & 1U) ? packed & 0x0FU : packed >> 4;
                if (alpha >= 11) pixel(frame, drawX + glyph->offsetX + gx, glyphTop + gy);
            }
        }
        drawX += advance;
    }
}

String apiBase() {
    String base(contentBaseUrl);
    base.trim();
    while (base.endsWith("/")) base.remove(base.length() - 1);
    return base + "/api/radio";
}

String absoluteStreamUrl(const char *path) {
    String value(path ? path : "");
    if (value.startsWith("http://")) return value;
    String base(contentBaseUrl);
    base.trim();
    while (base.endsWith("/")) base.remove(base.length() - 1);
    return base + (value.startsWith("/") ? value : "/" + value);
}

uint8_t pageCount() {
    return max<uint8_t>(1, (stationCount + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE);
}

bool loadStations() {
    UiLoadingIndicator::Scope loading;
    if (WiFi.status() != WL_CONNECTED) {
        copyText(statusText, sizeof(statusText), UiLocalization::isChinese() ? "网络未连接" : "WIFI NOT CONNECTED");
        return false;
    }
    HTTPClient http;
    WiFiClient client;
    const String url = apiBase() + "/list";
    http.setConnectTimeout(7000);
    http.setTimeout(20000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) return false;
    http.addHeader("Connection", "close");
    const int code = http.GET();
    const int contentLength = http.getSize();
    Serial.printf("[RADIO API] GET code=%d content_length=%d heap=%u largest=%u url=%s\n",
                  code, contentLength, static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                  url.c_str());
    if (code < 200 || code >= 300) {
        http.end();
        return false;
    }

    // Parse directly from the socket. HTTPClient::getString() needs a contiguous
    // allocation for the complete ~10 KiB response, which is unreliable once
    // Wi-Fi has fragmented the runtime heap. Filtering also discards fields the
    // radio UI never uses before ArduinoJson stores them.
    JsonDocument filter;
    filter["city"] = true;
    const char *arrayKeys[] = {"radio", "stations", "items"};
    for (const char *key : arrayKeys) {
        filter[key][0]["name"] = true;
        filter[key][0]["codec"] = true;
        filter[key][0]["esp32_url"] = true;
        filter[key][0]["stream_url"] = true;
        filter[key][0]["url"] = true;
    }
    JsonDocument document;
    const DeserializationError error = deserializeJson(
        document, *http.getStreamPtr(), DeserializationOption::Filter(filter));
    http.end();
    if (error) {
        snprintf(statusText, sizeof(statusText), "JSON %s", error.c_str());
        Serial.printf("[RADIO API] Stream JSON parse failed: %s heap=%u largest=%u\n",
                      error.c_str(), static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        return false;
    }
    copyText(locatedCity, sizeof(locatedCity), document["city"] | "");
    stationCount = 0;
    JsonArray items = document["radio"].as<JsonArray>();
    if (items.isNull()) items = document["stations"].as<JsonArray>();
    if (items.isNull()) items = document["items"].as<JsonArray>();
    if (items.isNull()) {
        copyText(statusText, sizeof(statusText), "NO RADIO ARRAY");
        Serial.println("[RADIO API] Parsed JSON has no radio, stations, or items array");
        return false;
    }
    for (JsonObject item : items) {
        if (stationCount >= MAX_STATIONS) break;
        Station &station = stations[stationCount];
        copyText(station.name, sizeof(station.name), item["name"] | "", "UNTITLED");
        copyText(station.codec, sizeof(station.codec), item["codec"] | "MP3");
        copyText(station.streamPath, sizeof(station.streamPath), item["esp32_url"] | "");
        if (!station.streamPath[0]) {
            copyText(station.streamPath, sizeof(station.streamPath), item["stream_url"] | "");
        }
        if (!station.streamPath[0]) {
            copyText(station.streamPath, sizeof(station.streamPath), item["url"] | "");
        }
        Serial.printf("[RADIO API] item=%u name=%s codec=%s stream=%s\n",
                      stationCount, station.name, station.codec, station.streamPath);
        if (station.streamPath[0]) ++stationCount;
    }
    if (!stationCount) copyText(statusText, sizeof(statusText), "NO RADIO STATIONS");
    return stationCount > 0;
}

void playbackTask(void *) {
    pinMode(BoardPins::PA_EN, OUTPUT);
    digitalWrite(BoardPins::PA_EN, HIGH);
    if (codec) codec->setSpeakerEnabled(true);
    vTaskDelay(pdMS_TO_TICKS(60));

    auto logMemory = [](const char *stage) {
        Serial.printf("[RADIO] %s heap=%u largest=%u stack_free=%u\n", stage,
                      static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    };

    logMemory("before allocations");
    AudioFileSourceHTTPStream *source = new AudioFileSourceHTTPStream();
    AudioFileSourceBuffer *buffer = nullptr;
    AudioGeneratorMP3 *decoder = decoderWorkspace
        ? new AudioGeneratorMP3(decoderWorkspace, AudioGeneratorMP3::preAllocSize())
        : nullptr;
    RadioAudioOutput *output = new RadioAudioOutput(codec);
    const bool objectsReady = source && decoder && output;
    logMemory(objectsReady ? "objects allocated" : "object allocation failed");
    const bool httpOpen = objectsReady && source->open(requestedUrl);
    logMemory(httpOpen ? "HTTP stream opened" : "HTTP stream open failed");
    bool bufferReady = false;
    bool decoderReady = false;
    bool started = httpOpen;
    if (started) {
        buffer = new AudioFileSourceBuffer(source, STREAM_BUFFER_BYTES);
        bufferReady = buffer != nullptr;
        logMemory(bufferReady ? "stream buffer allocated" : "stream buffer allocation failed");
        decoderReady = bufferReady && decoder->begin(buffer, output);
        started = decoderReady;
        logMemory(decoderReady ? "MP3 decoder started" : "MP3 decoder begin failed");
        Serial.printf("[RADIO] Heap integrity after decoder begin=%s\n",
                      heap_caps_check_integrity_all(false) ? "ok" : "corrupt");
    }
    Serial.printf("[RADIO] Stream %s objects=%s http=%s buffer=%s decoder=%s url=%s\n",
                  started ? "started" : "failed", objectsReady ? "ok" : "failed",
                  httpOpen ? "ok" : "failed", bufferReady ? "ok" : "failed",
                  decoderReady ? "ok" : "failed", requestedUrl);
    while (started && !stopRequested && decoder->isRunning()) {
        output->resetQuota();
        if (!decoder->loop()) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (decoder && decoder->isRunning()) decoder->stop();
    if (buffer) buffer->close();
    else if (source) source->close();
    delete decoder;
    delete buffer;
    delete source;
    delete output;
    digitalWrite(BoardPins::PA_EN, LOW);
    logMemory("after cleanup");
    Serial.printf("[RADIO] Heap integrity after cleanup=%s\n",
                  heap_caps_check_integrity_all(false) ? "ok" : "corrupt");
    playbackActive = false;
    playbackEnded = !stopRequested;
    playbackTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

bool startStream(uint8_t index) {
    if (!codec || !codec->isInitialized() || index >= stationCount ||
        playbackTaskHandle || playbackActive) return false;
    OpusPlayer::stop();
    const String url = absoluteStreamUrl(stations[index].streamPath);
    copyText(requestedUrl, sizeof(requestedUrl), url.c_str());
    Serial.printf("[RADIO] Decoder workspace bytes=%u allocation=static heap=%u largest=%u\n",
                  static_cast<unsigned>(MP3_DECODER_WORKSPACE_BYTES),
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    stopRequested = false;
    playbackEnded = false;
    playbackActive = true;
    playbackTaskHandle = xTaskCreateStaticPinnedToCore(
        playbackTask, "radio-playback", PLAYBACK_TASK_STACK_BYTES, nullptr, 2,
        playbackTaskStack, &playbackTaskBuffer, 0);
    if (!playbackTaskHandle) {
        Serial.printf("[RADIO] Playback task creation failed stack=%u heap=%u largest=%u\n",
                      static_cast<unsigned>(PLAYBACK_TASK_STACK_BYTES),
                      static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        playbackActive = false;
        playbackTaskHandle = nullptr;
        return false;
    }
    return true;
}

} // namespace

namespace RadioPage {

void setContentUrl(const char *url) { copyText(contentBaseUrl, sizeof(contentBaseUrl), url, "http://"); }

void setAudio(Es8311 *audio) {
    if (codec != audio) stop();
    codec = audio;
}

void open() {
    stop();
    libraryPage = 1;
    selectedIndex = -1;
    activeIndex = -1;
    pendingIndex = -1;
    stationCount = 0;
    locatedCity[0] = '\0';
    statusText[0] = '\0';
    loadStations();
}

bool rowBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                 int16_t &width, int16_t &height) {
    const uint8_t first = (libraryPage - 1) * ITEMS_PER_PAGE;
    const uint8_t visible = first < stationCount
        ? min<uint8_t>(ITEMS_PER_PAGE, stationCount - first) : 0;
    for (uint8_t index = 0; index < visible; ++index) {
        const int rowTop = LIST_TOP + index * (ROW_HEIGHT + ROW_GAP);
        if (!inRect(x, y, 12, rowTop, 216, ROW_HEIGHT)) continue;
        left = 12; top = rowTop; width = 216; height = ROW_HEIGHT;
        return true;
    }
    return false;
}

bool handleTap(int16_t x, int16_t y) {
    uint8_t nextPage = libraryPage;
    if (inRect(x, y, 4, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT)) nextPage = 1;
    else if (inRect(x, y, 42, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT) && libraryPage > 1) nextPage--;
    else if (inRect(x, y, 164, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT) && libraryPage < pageCount()) nextPage++;
    else if (inRect(x, y, 202, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT)) nextPage = pageCount();
    if (nextPage != libraryPage) { libraryPage = nextPage; return true; }

    const uint8_t first = (libraryPage - 1) * ITEMS_PER_PAGE;
    for (uint8_t row = 0; row < ITEMS_PER_PAGE; ++row) {
        const int top = LIST_TOP + row * (ROW_HEIGHT + ROW_GAP);
        if (!inRect(x, y, 12, top, 216, ROW_HEIGHT)) continue;
        const uint8_t index = first + row;
        if (index >= stationCount) return false;
        if (activeIndex == static_cast<int8_t>(index) && playbackActive) {
            stop();
            return true;
        }
        stop();
        selectedIndex = index;
        pendingIndex = index;
        return true;
    }
    return false;
}

bool process() {
    if (pendingIndex >= 0) {
        const int8_t index = pendingIndex;
        pendingIndex = -1;
        if (startStream(static_cast<uint8_t>(index))) {
            activeIndex = index;
            selectedIndex = -1;
        } else {
            selectedIndex = -1;
            activeIndex = -1;
        }
        return true;
    }
    if (playbackEnded) {
        playbackEnded = false;
        activeIndex = -1;
        selectedIndex = -1;
        return true;
    }
    return false;
}

bool isPlaying() { return playbackActive || pendingIndex >= 0; }

void stop() {
    pendingIndex = -1;
    if (playbackTaskHandle || playbackActive) {
        stopRequested = true;
        const uint32_t started = millis();
        // A stream read can remain inside WiFiClient until its socket timeout.
        // Wait beyond that interval so switching stations cannot create two MP3
        // tasks writing to the shared ES8311 I2S port at the same time.
        while (playbackTaskHandle && millis() - started < 7000) vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!playbackTaskHandle) playbackActive = false;
    activeIndex = -1;
    selectedIndex = -1;
}

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    if (UiLocalization::isChinese()) drawTitle(frame, 91, 34, 58, 18, "收音机");
    else UiLocalization::drawCentered(frame, 36, "RADIO", 2);
    rect(frame, 4, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT);
    drawDoubleArrow(frame, 21, PAGER_TOP + BUTTON_HEIGHT / 2, false);
    rect(frame, 42, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT);
    drawArrow(frame, 59, PAGER_TOP + BUTTON_HEIGHT / 2, false);
    rect(frame, 164, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT);
    drawArrow(frame, 181, PAGER_TOP + BUTTON_HEIGHT / 2, true);
    rect(frame, 202, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT);
    drawDoubleArrow(frame, 219, PAGER_TOP + BUTTON_HEIGHT / 2, true);
    char pager[24] = {};
    snprintf(pager, sizeof(pager), "%u/%u", libraryPage, pageCount());
    if (locatedCity[0]) drawTitle(frame, 79, 52, 82, 13, locatedCity);
    UiLocalization::drawCentered(frame, 67, pager);
    if (!stationCount) {
        UiLocalization::drawCentered(frame, 190, statusText);
        return;
    }
    const uint8_t first = (libraryPage - 1) * ITEMS_PER_PAGE;
    const uint8_t visible = min<uint8_t>(ITEMS_PER_PAGE, stationCount - first);
    for (uint8_t row = 0; row < visible; ++row) {
        const uint8_t index = first + row;
        const int top = LIST_TOP + row * (ROW_HEIGHT + ROW_GAP);
        rect(frame, 12, top, 216, ROW_HEIGHT);
        char number[8] = {};
        snprintf(number, sizeof(number), "%u", index + 1);
        UiLocalization::drawText(frame, 18, top + 9, number);
        drawTitle(frame, 34, top + 1, 140, ROW_HEIGHT - 2, stations[index].name);
        UiLocalization::drawText(frame, 177, top + 9, stations[index].codec);
        if (index == activeIndex && playbackActive) drawPlay(frame, 215, top + ROW_HEIGHT / 2);
        else drawArrow(frame, 215, top + ROW_HEIGHT / 2, true);
        if (index == activeIndex || index == selectedIndex) invertRect(frame, 12, top, 216, ROW_HEIGHT);
    }
}

} // namespace RadioPage