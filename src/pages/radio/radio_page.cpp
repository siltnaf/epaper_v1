#include "pages/radio/radio_page.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSource.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_heap_caps.h>

#include <cctype>
#include <cstring>
#include <cstdlib>
#include <strings.h>

#include "board_pins.h"
#include "devices/audio/opus_player.h"
#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/es8311/es8311.h"
#include "devices/ml307/ml307.h"
#include "font/xiaozhi_font.h"
#include "pages/playlist_cache.h"
#include "ui/loading_indicator.h"
#include "ui/localization.h"

namespace {

constexpr uint8_t ITEMS_PER_PAGE = 10;
// The decoder occupies about 29 KiB of the no-PSRAM ESP32-S3's internal heap.
// Keep the live stream entirely in RAM, leaving enough contiguous heap for the
// decoder, network stack, and the decoder's short-read adapter.
constexpr size_t STREAM_RING_BUFFER_BYTES = 12 * 1024;
constexpr size_t STREAM_START_BYTES = 8 * 1024;
constexpr uint16_t STREAM_BUFFER_BYTES = 2 * 1024;
constexpr uint32_t STREAM_PREFETCH_TIMEOUT_MS = 30000;
constexpr uint32_t STREAM_REFILL_TIMEOUT_MS = 15000;
constexpr uint16_t PLAYBACK_TASK_STACK_BYTES = 4608;
// ESP-IDF's task APIs use bytes for stack depth. The producer keeps its large
// network buffer off-stack so 3 KiB is sufficient for its shallow fetch loop.
constexpr uint16_t PRODUCER_TASK_STACK_BYTES = 3 * 1024;
static_assert(PRODUCER_TASK_STACK_BYTES % sizeof(StackType_t) == 0,
              "Producer stack byte count must contain whole StackType_t words");
constexpr int LIST_TOP = 82;
constexpr int ROW_HEIGHT = 30;
constexpr int ROW_GAP = 2;
constexpr int PAGER_TOP = 52;
constexpr int BUTTON_WIDTH = 34;
constexpr int BUTTON_HEIGHT = 25;
constexpr int MARQUEE_X = 40;
constexpr int MARQUEE_WIDTH = 164;
constexpr int MARQUEE_HEIGHT = ROW_HEIGHT - 2;

struct Station {
    char name[80] = {};
    char streamPath[256] = {};
};

StackType_t producerTaskStack[PRODUCER_TASK_STACK_BYTES / sizeof(StackType_t)] = {};
StaticTask_t producerTaskBuffer = {};

class RadioHttpSource final : public AudioFileSource {
public:
    bool open(const char *url) override {
        close();
        if (!url || std::strncmp(url, "http://", 7) != 0) return false;
        const char *authority = url + 7;
        const char *path = std::strchr(authority, '/');
        const size_t authorityLength = path
            ? static_cast<size_t>(path - authority) : std::strlen(authority);
        if (authorityLength == 0 || authorityLength >= sizeof(host_)) return false;
        std::memcpy(host_, authority, authorityLength);
        host_[authorityLength] = '\0';
        char *portSeparator = std::strrchr(host_, ':');
        if (portSeparator) {
            *portSeparator++ = '\0';
            port_ = static_cast<uint16_t>(std::atoi(portSeparator));
            if (port_ == 0) port_ = 80;
        }
        std::strncpy(path_, path ? path : "/", sizeof(path_) - 1);
        path_[sizeof(path_) - 1] = '\0';
        client_.setTimeout(2);
        if (!client_.connect(host_, port_)) return false;
        client_.printf("GET %s HTTP/1.1\r\nHost: %s\r\n"
                       "User-Agent: ESP32-ePaper-Radio/1.0\r\n"
                       "Accept: audio/mpeg\r\nConnection: close\r\n\r\n",
                       path_, host_);
        char line[160] = {};
        if (!readLine(line, sizeof(line), 5000)) return close(), false;
        int status = 0;
        if (std::sscanf(line, "HTTP/%*s %d", &status) != 1 || status != 200) {
            Serial.printf("[RADIO HTTP] status line=%s\n", line);
            return close(), false;
        }
        while (readLine(line, sizeof(line), 5000)) {
            if (!line[0]) break;
            if (startsWithIgnoreCase(line, "Transfer-Encoding:") &&
                containsIgnoreCase(line, "chunked")) chunked_ = true;
            if (startsWithIgnoreCase(line, "Content-Length:")) {
                contentLength_ = std::strtol(line + 15, nullptr, 10);
            }
        }
        opened_ = true;
        Serial.printf("[RADIO HTTP] connected host=%s port=%u chunked=%s length=%ld\n",
                      host_, port_, chunked_ ? "yes" : "no",
                      static_cast<long>(contentLength_));
        return true;
    }

    uint32_t read(void *data, uint32_t len) override {
        if (!opened_ || !data || len == 0) return 0;
        uint8_t *output = static_cast<uint8_t *>(data);
        uint32_t total = 0;
        const uint32_t started = millis();
        while (total < len && millis() - started < 5000) {
            if (chunked_ && chunkRemaining_ == 0) {
                if (chunkNeedsCrlf_ && !consumeCrlf()) break;
                char line[32] = {};
                do {
                    if (!readLine(line, sizeof(line), 5000)) {
                        readFailure_ = "chunk header timeout";
                        break;
                    }
                } while (!line[0]);
                if (!line[0]) break;
                char *end = nullptr;
                const unsigned long chunkSize = std::strtoul(line, &end, 16);
                while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
                if (end == line || (end && *end && *end != ';')) {
                    Serial.printf("[RADIO HTTP] invalid chunk header='%s' pos=%lu\n",
                                  line, static_cast<unsigned long>(position_));
                    readFailure_ = "invalid chunk header";
                    break;
                }
                chunkRemaining_ = static_cast<uint32_t>(chunkSize);
                if (chunkRemaining_ == 0) {
                    Serial.printf("[RADIO HTTP] terminal chunk pos=%lu\n",
                                  static_cast<unsigned long>(position_));
                    readFailure_ = "chunked stream ended";
                    opened_ = false;
                    break;
                }
                chunkNeedsCrlf_ = true;
            }
            const int available = client_.available();
            if (available <= 0) {
                if (!client_.connected()) {
                    readFailure_ = "socket closed";
                    opened_ = false;
                    break;
                }
                vTaskDelay(1);
                continue;
            }
            uint32_t requested = min<uint32_t>(len - total, static_cast<uint32_t>(available));
            if (chunked_) requested = min<uint32_t>(requested, chunkRemaining_);
            const int received = client_.read(output + total, requested);
            if (received <= 0) {
                readFailure_ = "socket read failed";
                break;
            }
            readFailure_ = "";
            total += received;
            position_ += received;
            if (chunked_) chunkRemaining_ -= received;
        }
        if (total == 0 && opened_ && millis() - started >= 5000) {
            readFailure_ = "read timeout";
        }
        return total;
    }

    uint32_t readNonBlock(void *data, uint32_t len) override {
        if (!opened_ || client_.available() <= 0) return 0;
        return read(data, len);
    }
    bool close() override { client_.stop(); reset(); return true; }
    bool isOpen() override { return opened_ && (client_.connected() || client_.available()); }
    uint32_t getSize() override { return contentLength_ > 0 ? contentLength_ : 0; }
    uint32_t getPos() override { return position_; }
    const char *readFailure() const { return readFailure_; }

private:
    void reset() {
        opened_ = false;
        chunked_ = false;
        chunkNeedsCrlf_ = false;
        chunkRemaining_ = 0;
        contentLength_ = -1;
        position_ = 0;
        readFailure_ = "";
        port_ = 80;
        host_[0] = '\0';
        path_[0] = '\0';
    }
    bool readLine(char *line, size_t size, uint32_t timeoutMs) {
        if (!line || size == 0) return false;
        size_t used = 0;
        const uint32_t started = millis();
        while (millis() - started < timeoutMs) {
            if (client_.available() <= 0) {
                if (!client_.connected()) break;
                vTaskDelay(1);
                continue;
            }
            const int value = client_.read();
            if (value < 0) continue;
            if (value == '\n') { line[used] = '\0'; return true; }
            if (value != '\r' && used + 1 < size) line[used++] = static_cast<char>(value);
        }
        line[used] = '\0';
        return false;
    }
    bool consumeCrlf() {
        chunkNeedsCrlf_ = false;
        const int first = readByte(2000);
        if (first == '\n') return true;
        const int second = first == '\r' ? readByte(2000) : -1;
        if (first == '\r' && second == '\n') return true;
        Serial.printf("[RADIO HTTP] invalid chunk boundary first=%d second=%d pos=%lu\n",
                      first, second, static_cast<unsigned long>(position_));
        readFailure_ = "invalid chunk boundary";
        return false;
    }
    int readByte(uint32_t timeoutMs) {
        const uint32_t started = millis();
        while (millis() - started < timeoutMs) {
            if (client_.available() > 0) return client_.read();
            if (!client_.connected()) return -1;
            vTaskDelay(1);
        }
        return -1;
    }
    static bool startsWithIgnoreCase(const char *value, const char *prefix) {
        while (*prefix) {
            if (std::tolower(static_cast<unsigned char>(*value++)) !=
                std::tolower(static_cast<unsigned char>(*prefix++))) return false;
        }
        return true;
    }
    static bool containsIgnoreCase(const char *value, const char *needle) {
        const size_t needleLength = std::strlen(needle);
        for (; *value; ++value) {
            if (strncasecmp(value, needle, needleLength) == 0) return true;
        }
        return false;
    }

    WiFiClient client_;
    char host_[96] = {};
    char path_[384] = {};
    uint16_t port_ = 80;
    bool opened_ = false;
    bool chunked_ = false;
    bool chunkNeedsCrlf_ = false;
    uint32_t chunkRemaining_ = 0;
    int32_t contentLength_ = -1;
    uint32_t position_ = 0;
    const char *readFailure_ = "";
};

class RadioPrefetchSource final : public AudioFileSource {
public:
    explicit RadioPrefetchSource(AudioFileSource *network) : network_(network) {}

    bool prefetch(const volatile bool *cancelRequested) {
        cancelRequested_ = cancelRequested;
        if (!network_ || !network_->isOpen() || SD_MMC.cardType() == CARD_NONE) return false;
        if (!SD_MMC.exists(RADIO_SD_FOLDER) && !SD_MMC.mkdir(RADIO_SD_FOLDER)) return false;
        SD_MMC.remove(STREAM_PREFETCH_PATH);
        writer_ = SD_MMC.open(STREAM_PREFETCH_PATH, FILE_WRITE);
        if (!writer_) return false;
        uint8_t chunk[512] = {};
        const uint32_t started = millis();
        while (prefetchedBytes_ < STREAM_PREFETCH_BYTES &&
               millis() - started < STREAM_PREFETCH_TIMEOUT_MS &&
               !(cancelRequested && *cancelRequested)) {
            const size_t requested = min<size_t>(sizeof(chunk),
                STREAM_PREFETCH_BYTES - prefetchedBytes_);
            const uint32_t received = network_->read(chunk, requested);
            if (received == 0) {
                if (!network_->isOpen()) break;
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            if (writer_.write(chunk, received) != received) break;
            prefetchedBytes_ += received;
            producedBytes_ = prefetchedBytes_;
            vTaskDelay(1);
        }
        writer_.flush();
        writtenBytes_ = prefetchedBytes_;
        const bool cancelled = cancelRequested && *cancelRequested;
        Serial.printf("[RADIO PREFETCH] bytes=%u target=%u elapsed=%lums result=%s\n",
                      static_cast<unsigned>(producedBytes_),
                      static_cast<unsigned>(STREAM_PREFETCH_BYTES),
                      static_cast<unsigned long>(millis() - started),
                      cancelled ? "cancelled" :
                      producedBytes_ >= STREAM_PREFETCH_BYTES ? "ok" : "partial");
        if (cancelled || producedBytes_ < STREAM_PREFETCH_BYTES) {
            Serial.printf("[RADIO PREFETCH] rejected partial cache bytes=%u target=%u "
                          "http_reason=%s\n",
                          static_cast<unsigned>(producedBytes_),
                          static_cast<unsigned>(STREAM_PREFETCH_BYTES),
                          network_ ? static_cast<RadioHttpSource *>(network_)->readFailure()
                                   : "none");
            writer_.close();
            SD_MMC.remove(STREAM_PREFETCH_PATH);
            return false;
        }
        producerRunning_ = true;
        producerTaskHandle_ = xTaskCreateStaticPinnedToCore(
            producerTaskThunk, "radio-fetch", PRODUCER_TASK_STACK_BYTES, this, 3,
            producerTaskStack, &producerTaskBuffer, 1);
        if (!producerTaskHandle_) {
            producerRunning_ = false;
            return false;
        }
        return true;
    }

    uint32_t read(void *data, uint32_t len) override {
        if (!data || len == 0) return 0;
        const uint32_t started = millis();
        uint32_t total = 0;
        while (total < len && !(cancelRequested_ && *cancelRequested_)) {
            const uint32_t received = readAvailable(
                static_cast<uint8_t *>(data) + total, len - total);
            if (received > 0) {
                total += received;
                continue;
            }
            if (!producerRunning_ || millis() - started >= STREAM_REFILL_TIMEOUT_MS) break;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (total < len && producerRunning_) {
            Serial.printf("[RADIO PREFETCH] refill timeout requested=%u received=%u "
                          "lead=%u http_reason=%s\n",
                          static_cast<unsigned>(len), static_cast<unsigned>(total),
                          static_cast<unsigned>(leadBytes()),
                          network_ ? static_cast<RadioHttpSource *>(network_)->readFailure()
                                   : "none");
        }
        return total;
    }

    uint32_t readNonBlock(void *data, uint32_t len) override {
        if (!data || len == 0) return 0;
        return readAvailable(data, len);
    }

    bool close() override {
        producerRunning_ = false;
        const uint32_t started = millis();
        // RadioHttpSource::read can remain in its socket loop for two seconds.
        // Do not destroy this source until the producer has left that call and
        // stopped using this object and its File handles.
        while (producerTaskHandle_ && millis() - started < 3000) vTaskDelay(1);
        if (producerTaskHandle_) {
            Serial.println("[RADIO PREFETCH] forcing producer shutdown after timeout");
            vTaskDelete(producerTaskHandle_);
            producerTaskHandle_ = nullptr;
        }
        if (reader_) reader_.close();
        if (writer_) writer_.close();
        const bool closed = network_ ? network_->close() : true;
        SD_MMC.remove(STREAM_PREFETCH_PATH);
        return closed;
    }

    bool isOpen() override {
        return producedBytes_ > consumedBytes_ || producerRunning_;
    }

    uint32_t getSize() override { return 0; }
    uint32_t getPos() override { return consumedBytes_; }
    bool loop() override { return true; }

    size_t leadBytes() const {
        return producedBytes_ > consumedBytes_ ? producedBytes_ - consumedBytes_ : 0;
    }

private:
    static void producerTaskThunk(void *context) {
        static_cast<RadioPrefetchSource *>(context)->producerLoop();
    }

    uint32_t readAvailable(void *data, uint32_t len) {
        const size_t available = producedBytes_ > consumedBytes_
            ? producedBytes_ - consumedBytes_ : 0;
        if (available == 0) return 0;
        if (!reader_) {
            reader_ = SD_MMC.open(STREAM_PREFETCH_PATH, FILE_READ);
            if (!reader_ || !reader_.seek(consumedBytes_)) return 0;
        } else if (reader_.position() != consumedBytes_ && !reader_.seek(consumedBytes_)) {
            reader_.close();
            return 0;
        }
        const uint32_t requested = min<uint32_t>(len, available);
        const uint32_t received = reader_.read(static_cast<uint8_t *>(data), requested);
        if (received == 0) {
            // The VFS handle can retain the file size from when it was opened.
            // Reopen after the producer publishes appended bytes.
            reader_.close();
            return 0;
        }
        consumedBytes_ += received;
        return received;
    }

    void producerLoop() {
        uint8_t chunk[512] = {};
        uint32_t lastProgressLogMs = millis();
        while (producerRunning_ && !(cancelRequested_ && *cancelRequested_)) {
            if (writtenBytes_ > consumedBytes_ &&
                writtenBytes_ - consumedBytes_ >= STREAM_PREFETCH_BYTES) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            const uint32_t received = network_->read(chunk, sizeof(chunk));
            if (received == 0) {
                if (!network_->isOpen()) {
                    Serial.printf("[RADIO PREFETCH] producer HTTP closed bytes=%u reason=%s\n",
                                  static_cast<unsigned>(writtenBytes_),
                                  static_cast<RadioHttpSource *>(network_)->readFailure());
                    break;
                }
                if (millis() - lastProgressLogMs >= 5000) {
                    Serial.printf("[RADIO PREFETCH] producer waiting lead=%u written=%u "
                                  "published=%u reason=%s\n",
                                  static_cast<unsigned>(writtenBytes_ > consumedBytes_
                                      ? writtenBytes_ - consumedBytes_ : 0),
                                  static_cast<unsigned>(writtenBytes_),
                                  static_cast<unsigned>(producedBytes_),
                                  static_cast<RadioHttpSource *>(network_)->readFailure());
                    lastProgressLogMs = millis();
                }
                vTaskDelay(1);
                continue;
            }
            if (writer_.write(chunk, received) != received) {
                Serial.printf("[RADIO PREFETCH] producer SD write failed bytes=%u\n",
                              static_cast<unsigned>(writtenBytes_));
                break;
            }
            writtenBytes_ += received;
            lastProgressLogMs = millis();
            // Publish in SD-sized batches. Frequent flushes make the producer
            // fall behind even when network throughput exceeds playback rate.
            if (writtenBytes_ - producedBytes_ >= STREAM_PUBLISH_BYTES) {
                writer_.flush();
                producedBytes_ = writtenBytes_;
            }
            taskYIELD();
        }
        writer_.flush();
        producedBytes_ = writtenBytes_;
        producerRunning_ = false;
        producerTaskHandle_ = nullptr;
        vTaskDelete(nullptr);
    }

    AudioFileSource *network_ = nullptr;
    File reader_;
    File writer_;
    const volatile bool *cancelRequested_ = nullptr;
    TaskHandle_t producerTaskHandle_ = nullptr;
    volatile bool producerRunning_ = false;
    size_t prefetchedBytes_ = 0;
    size_t writtenBytes_ = 0;
    volatile size_t producedBytes_ = 0;
    volatile size_t consumedBytes_ = 0;
};

class RadioAudioOutput final : public AudioOutput {
public:
    explicit RadioAudioOutput(Es8311 *audio) : audio_(audio) {}

    bool begin() override {
        active_ = audio_ && audio_->isInitialized();
        phase_ = 0;
        bufferedSamples_ = 0;
        writeCount_ = 0;
        peak_ = 0;
        loggedWriteFailure_ = false;
        if (active_) audio_->setSpeakerEnabled(true);
        return active_;
    }

    bool SetRate(int hz) override {
        sourceRate_ = hz;
        Serial.printf("[RADIO AUDIO] source rate=%d output rate=%u\n",
                      hz, static_cast<unsigned>(Es8311::DEFAULT_SAMPLE_RATE));
        return hz >= 8000 && hz <= 96000 && AudioOutput::SetRate(hz);
    }

    bool SetBitsPerSample(int bits) override {
        return (bits == 8 || bits == 16) && AudioOutput::SetBitsPerSample(bits);
    }

    bool SetChannels(int channels) override {
        Serial.printf("[RADIO AUDIO] channels=%d\n", channels);
        return (channels == 1 || channels == 2) && AudioOutput::SetChannels(channels);
    }

    // Keep decoder bursts short so the HTTP source and Wi-Fi tasks are serviced
    // frequently instead of alternating between long decode and refill stalls.
    void resetQuota() { sourceFramesRemaining_ = 384; }

    bool ConsumeSample(int16_t sample[2]) override {
        if (!active_ || sourceFramesRemaining_ == 0) return false;
        // AudioGeneratorMP3 probes the output with one sample before decoding
        // its first frame and calling SetRate(). Accept that bootstrap sample;
        // rejecting it leaves the decoder permanently stalled at rate zero.
        if (sourceRate_ <= 0) return true;
        --sourceFramesRemaining_;
        MakeSampleStereo16(sample);
        phase_ += Es8311::DEFAULT_SAMPLE_RATE;
        while (phase_ >= static_cast<uint32_t>(sourceRate_)) {
            phase_ -= static_cast<uint32_t>(sourceRate_);
            const int16_t left = Amplify(sample[LEFTCHANNEL]);
            const int16_t right = Amplify(sample[RIGHTCHANNEL]);
            peak_ = max<uint16_t>(peak_, max<uint16_t>(abs(left), abs(right)));
            pcm_[bufferedSamples_++] = left;
            pcm_[bufferedSamples_++] = right;
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
        ++writeCount_;
        if (writeCount_ == 1 || !written) {
            Serial.printf("[RADIO AUDIO] write=%lu samples=%u peak=%u result=%s\n",
                          static_cast<unsigned long>(writeCount_), bufferedSamples_, peak_,
                          written ? "ok" : "failed");
        }
        if (!written) loggedWriteFailure_ = true;
        bufferedSamples_ = 0;
        return written;
    }

    static constexpr uint16_t PCM_CAPACITY = 256;
    Es8311 *audio_ = nullptr;
    int16_t pcm_[PCM_CAPACITY] = {};
    uint16_t bufferedSamples_ = 0;
    uint16_t sourceFramesRemaining_ = 0;
    uint32_t phase_ = 0;
    int sourceRate_ = 0;
    bool active_ = false;
    uint32_t writeCount_ = 0;
    uint16_t peak_ = 0;
    bool loggedWriteFailure_ = false;
};

Station stations[ITEMS_PER_PAGE] = {};
uint8_t stationCount = 0;
uint16_t stationTotal = 0;
uint16_t stationOffset = 0;
bool stationHasMore = false;
int8_t selectedIndex = -1;
int8_t activeIndex = -1;
int8_t pendingIndex = -1;
char contentBaseUrl[128] = "http://";
char statusText[64] = {};
Es8311 *codec = nullptr;

TaskHandle_t playbackTaskHandle = nullptr;
volatile bool stopRequested = false;
volatile bool playbackActive = false;
volatile bool playbackEnded = false;
char requestedUrl[384] = {};
void *decoderWorkspace = nullptr;
void *streamBufferWorkspace = nullptr;
uint16_t marqueeOffset = 0;
uint32_t nextMarqueeMs = 0;
bool marqueeReady = false;
uint8_t marqueeBitmap[MARQUEE_HEIGHT][(MARQUEE_WIDTH + 7) / 8] = {};
volatile bool libraryLoadRunning = false;
volatile bool libraryLoadCompleted = false;

void pixel(uint8_t *frame, int x, int y) {
    if (!frame || x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |= 0x80U >> (x % 8);
}

bool marqueePixel(int x, int y) {
    if (x < 0 || x >= MARQUEE_WIDTH || y < 0 || y >= MARQUEE_HEIGHT) return false;
    return marqueeBitmap[y][x / 8] & (0x80U >> (x % 8));
}

bool framePixel(const uint8_t *frame, int x, int y) {
    if (!frame || x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) {
        return false;
    }
    return frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] &
           (0x80U >> (x % 8));
}

void captureMarquee(const uint8_t *frame, int top) {
    std::memset(marqueeBitmap, 0x00, sizeof(marqueeBitmap));
    for (int y = 0; y < MARQUEE_HEIGHT; ++y) {
        for (int x = 0; x < MARQUEE_WIDTH; ++x) {
            if (framePixel(frame, MARQUEE_X + x, top + 1 + y)) {
                marqueeBitmap[y][x / 8] |= 0x80U >> (x % 8);
            }
        }
    }
    marqueeReady = true;
    marqueeOffset = 0;
    nextMarqueeMs = millis() + 900;
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

int titleWidth(const char *value) {
    const char *cursor = value ? value : "";
    int width = 0;
    uint32_t codepoint = 0;
    while (nextCodepoint(cursor, codepoint)) {
        if (codepoint < 0x80) {
            width += codepoint == ' ' ? 5 : 7;
        } else {
            const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint);
            width += glyph ? max(1, static_cast<int>(glyph->advance)) + 1 : 17;
        }
    }
    return width;
}

void drawCenteredTitle(uint8_t *frame, int y, int height, const char *value) {
    const int width = min<int>(XingtaiEpd::WIDTH, titleWidth(value));
    const int x = max(0, (XingtaiEpd::WIDTH - width) / 2);
    drawTitle(frame, x, y, XingtaiEpd::WIDTH - x, height, value);
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

uint16_t pageCount() {
    return stationTotal > 0
        ? (stationTotal + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE : 1;
}

bool loadStations(uint16_t requestedOffset = 0) {
    UiLoadingIndicator::Scope loading;
    const String endpoint = apiBase() + "/list";
    const String url = endpoint + "?limit=" + String(ITEMS_PER_PAGE) +
                       "&offset=" + String(requestedOffset);
    String payload;
    char cacheSlot[24] = {};
    snprintf(cacheSlot, sizeof(cacheSlot), "offset-%u", requestedOffset);
    bool fetchedRemote = false;
    if (WiFi.status() != WL_CONNECTED) {
        if (!cellularModem.httpGet(url.c_str(), payload)) {
            if (!PlaylistCache::load(RADIO_SD_FOLDER, endpoint, cacheSlot, payload)) {
                copyText(statusText, sizeof(statusText), UiLocalization::isChinese()
                    ? "网络未连接" : "NETWORK NOT CONNECTED");
                return false;
            }
        }
    } else {
        int code = 0;
        for (uint8_t attempt = 1; attempt <= 2 && payload.isEmpty(); ++attempt) {
            HTTPClient http;
            WiFiClient client;
            http.setConnectTimeout(7000);
            http.setTimeout(12000);
            http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            http.setReuse(false);
            if (!http.begin(client, url)) break;
            http.addHeader("Connection", "close");
            code = http.GET();
            if (code >= 200 && code < 300) payload = http.getString();
            const String error = code < 0 ? http.errorToString(code) : String();
            http.end();
            Serial.printf("[RADIO API] GET attempt=%u/2 code=%d%s%s bytes=%u url=%s\n",
                          attempt, code, error.isEmpty() ? "" : " ", error.c_str(),
                          payload.length(), url.c_str());
            if (!payload.isEmpty()) {
                fetchedRemote = true;
                break;
            }
            if (attempt < 2) delay(500);
        }
        if (payload.isEmpty() &&
            !PlaylistCache::load(RADIO_SD_FOLDER, endpoint, cacheSlot, payload)) {
            copyText(statusText, sizeof(statusText), code == HTTPC_ERROR_READ_TIMEOUT
                ? "RADIO API TIMEOUT" : "RADIO API FAILED");
            return false;
        }
    }
    JsonDocument document;
    if (deserializeJson(document, payload)) {
        if (fetchedRemote &&
            PlaylistCache::load(RADIO_SD_FOLDER, endpoint, cacheSlot, payload) &&
            !deserializeJson(document, payload)) {
            fetchedRemote = false;
        } else {
            copyText(statusText, sizeof(statusText), "RADIO JSON FAILED");
            return false;
        }
    }
    if (fetchedRemote) PlaylistCache::save(RADIO_SD_FOLDER, endpoint, cacheSlot, payload);
    const uint16_t responseOffset = document["offset"] | requestedOffset;
    const uint16_t responseTotal = document["total"] | 0;
    stationCount = 0;
    for (JsonObject item : document["radio"].as<JsonArray>()) {
        if (stationCount >= ITEMS_PER_PAGE) break;
        Station &station = stations[stationCount];
        station = Station();
        copyText(station.name, sizeof(station.name), item["name"] | "", "UNTITLED");
        copyText(station.streamPath, sizeof(station.streamPath), item["esp32_url"] | "");
        if (station.streamPath[0]) ++stationCount;
    }
    stationOffset = responseOffset;
    stationTotal = max<uint16_t>(responseTotal, stationOffset + stationCount);
    stationHasMore = document["has_more"] | (stationOffset + stationCount < stationTotal);
    statusText[0] = '\0';
    if (!stationCount) copyText(statusText, sizeof(statusText), "NO RADIO STATIONS");
    Serial.printf("[RADIO API] page offset=%u count=%u total=%u has_more=%s\n",
                  stationOffset, stationCount, stationTotal,
                  stationHasMore ? "true" : "false");
    return stationCount > 0;
}

void libraryLoadTask(void *) {
    loadStations(0);
    libraryLoadRunning = false;
    libraryLoadCompleted = true;
    vTaskDelete(nullptr);
}

void radioBufferStatus(void *, int code, const char *message) {
    if (code != AudioFileSourceBuffer::STATUS_UNDERFLOW) return;
    Serial.printf("[RADIO BUFFER] underflow %s\n", message ? message : "");
}

void playbackTask(void *) {
    pinMode(BoardPins::PA_EN, OUTPUT);
    digitalWrite(BoardPins::PA_EN, HIGH);
    const bool playbackReady = codec && codec->preparePlayback();
    if (codec) codec->setSpeakerEnabled(true);
    Serial.printf("[RADIO AUDIO] codec playback restore=%s PA=%d\n",
                  playbackReady ? "ok" : "failed", digitalRead(BoardPins::PA_EN));
    vTaskDelay(pdMS_TO_TICKS(60));

    auto logMemory = [](const char *stage) {
        Serial.printf("[RADIO] %s heap=%u largest=%u stack_free=%u\n", stage,
                      static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    };

    logMemory("before allocations");
    constexpr size_t decoderBytes = AudioGeneratorMP3::preAllocSize();
    decoderWorkspace = heap_caps_malloc(decoderBytes, MALLOC_CAP_8BIT);
    AudioGeneratorMP3 *decoder = nullptr;
    Serial.printf("[RADIO] Decoder workspace bytes=%u allocation=%s heap=%u largest=%u\n",
                  static_cast<unsigned>(decoderBytes), decoderWorkspace ? "ok" : "failed",
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    RadioHttpSource *source = new RadioHttpSource();
    RadioPrefetchSource *prefetchSource = source ? new RadioPrefetchSource(source) : nullptr;
    AudioFileSourceBuffer *buffer = nullptr;
    RadioAudioOutput *output = nullptr;
    const bool prefetchObjectsReady = decoderWorkspace && source && prefetchSource;
    logMemory(prefetchObjectsReady ? "prefetch objects allocated" : "prefetch allocation failed");
    const bool httpOpen = prefetchObjectsReady && source->open(requestedUrl);
    logMemory(httpOpen ? "HTTP stream opened" : "HTTP stream open failed");
    bool prefetchReady = false;
    if (httpOpen) {
        prefetchReady = prefetchSource->prefetch(&stopRequested);
    }
    if (prefetchReady) {
        // The large decoder block is already reserved. Delay playback-only
        // allocations until SD has the full lead so lwIP has maximum heap while
        // receiving the bursty HLS-to-MP3 proxy stream.
        decoder = new AudioGeneratorMP3(decoderWorkspace, decoderBytes);
        output = new RadioAudioOutput(codec);
        streamBufferWorkspace = heap_caps_malloc(STREAM_BUFFER_BYTES, MALLOC_CAP_8BIT);
        buffer = streamBufferWorkspace
            ? new AudioFileSourceBuffer(prefetchSource, streamBufferWorkspace,
                                        STREAM_BUFFER_BYTES)
            : nullptr;
        if (buffer) buffer->RegisterStatusCB(radioBufferStatus, nullptr);
        Serial.printf("[RADIO BUFFER] playback allocations decoder=%s output=%s reserve=%s "
                      "buffer=%s bytes=%u heap=%u largest=%u\n",
                      decoder ? "ok" : "failed", output ? "ok" : "failed",
                      streamBufferWorkspace ? "ok" : "failed", buffer ? "ok" : "failed",
                      static_cast<unsigned>(STREAM_BUFFER_BYTES),
                      static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    }
    const bool bufferReady = buffer != nullptr && streamBufferWorkspace != nullptr;
    bool decoderReady = false;
    const bool objectsReady = decoder && source && prefetchSource && output;
    bool started = httpOpen && prefetchReady && objectsReady && bufferReady;
    if (started) {
        logMemory(bufferReady ? "stream buffer allocated" : "stream buffer allocation failed");
        decoderReady = bufferReady && decoder->begin(buffer, output);
        started = decoderReady;
        logMemory(decoderReady ? "MP3 decoder started" : "MP3 decoder begin failed");
    }
    Serial.printf("[RADIO] Stream %s objects=%s http=%s prefetch=%s buffer=%s decoder=%s url=%s\n",
                  started ? "started" : "failed", objectsReady ? "ok" : "failed",
                  httpOpen ? "ok" : "failed", prefetchReady ? "ok" : "failed",
                  bufferReady ? "ok" : "failed",
                  decoderReady ? "ok" : "failed", requestedUrl);
    uint32_t nextBufferLogMs = millis() + 5000;
    while (started && !stopRequested && decoder->isRunning()) {
        output->resetQuota();
        if (!decoder->loop()) break;
        if (buffer && millis() >= nextBufferLogMs) {
            Serial.printf("[RADIO BUFFER] ram=%u/%u sd_lead=%u/%u heap=%u largest=%u\n",
                          static_cast<unsigned>(buffer->getFillLevel()),
                          static_cast<unsigned>(STREAM_BUFFER_BYTES),
                          static_cast<unsigned>(prefetchSource ? prefetchSource->leadBytes() : 0),
                          static_cast<unsigned>(STREAM_PREFETCH_BYTES),
                          static_cast<unsigned>(ESP.getFreeHeap()),
                          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
            nextBufferLogMs = millis() + 5000;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (decoder && decoder->isRunning()) decoder->stop();
    if (buffer) buffer->close();
    else if (prefetchSource) prefetchSource->close();
    else if (source) source->close();
    delete decoder;
    delete buffer;
    delete prefetchSource;
    delete source;
    delete output;
    free(streamBufferWorkspace);
    streamBufferWorkspace = nullptr;
    free(decoderWorkspace);
    decoderWorkspace = nullptr;
    streamBufferWorkspace = nullptr;
    digitalWrite(BoardPins::PA_EN, LOW);
    playbackActive = false;
    playbackEnded = !stopRequested;
    playbackTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

bool startStream(uint8_t index) {
    if (!codec || !codec->isInitialized() || index >= stationCount ||
        playbackTaskHandle || playbackActive) {
        Serial.printf("[RADIO] Start rejected codec=%s initialized=%s index=%u count=%u "
                      "task=%p active=%s stop=%s\n",
                      codec ? "yes" : "no",
                      codec && codec->isInitialized() ? "yes" : "no",
                      index, stationCount, playbackTaskHandle,
                      playbackActive ? "yes" : "no",
                      stopRequested ? "yes" : "no");
        return false;
    }
    OpusPlayer::stop();
    const String url = absoluteStreamUrl(stations[index].streamPath);
    copyText(requestedUrl, sizeof(requestedUrl), url.c_str());
    decoderWorkspace = nullptr;
    stopRequested = false;
    playbackEnded = false;
    playbackActive = true;
    if (xTaskCreatePinnedToCore(playbackTask, "radio-playback",
                                PLAYBACK_TASK_STACK_BYTES, nullptr, 2,
                                &playbackTaskHandle, 0) != pdPASS) {
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
    stationTotal = 0;
    stationOffset = 0;
    stationHasMore = false;
    selectedIndex = -1;
    activeIndex = -1;
    pendingIndex = -1;
    stationCount = 0;
    copyText(statusText, sizeof(statusText), UiLocalization::isChinese()
        ? "正在获取电台" : "LOADING RADIO");
    libraryLoadCompleted = false;
}

bool startLibraryLoad() {
    if (libraryLoadRunning) return false;
    libraryLoadRunning = true;
    libraryLoadCompleted = false;
    if (xTaskCreate(libraryLoadTask, "radio-library", 4096,
                    nullptr, 1, nullptr) != pdPASS) {
        libraryLoadRunning = false;
        copyText(statusText, sizeof(statusText), "RADIO TASK FAILED");
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

bool rowBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                 int16_t &width, int16_t &height) {
    for (uint8_t index = 0; index < stationCount; ++index) {
        const int rowTop = LIST_TOP + index * (ROW_HEIGHT + ROW_GAP);
        if (!inRect(x, y, 12, rowTop, 216, ROW_HEIGHT)) continue;
        left = 12; top = rowTop; width = 216; height = ROW_HEIGHT;
        return true;
    }
    return false;
}

bool handleTap(int16_t x, int16_t y) {
    uint16_t nextOffset = stationOffset;
    if (inRect(x, y, 4, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT)) {
        nextOffset = 0;
    } else if (inRect(x, y, 42, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT) &&
               stationOffset >= ITEMS_PER_PAGE) {
        nextOffset = stationOffset - ITEMS_PER_PAGE;
    } else if (inRect(x, y, 164, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT) &&
               stationHasMore) {
        nextOffset = stationOffset + ITEMS_PER_PAGE;
    } else if (inRect(x, y, 202, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT) &&
               stationTotal > 0) {
        nextOffset = ((stationTotal - 1) / ITEMS_PER_PAGE) * ITEMS_PER_PAGE;
    }
    if (nextOffset != stationOffset) {
        stop();
        selectedIndex = -1;
        activeIndex = -1;
        pendingIndex = -1;
        const bool loaded = loadStations(nextOffset);
        Serial.printf("[RADIO API] navigate offset=%u result=%s\n",
                      nextOffset, loaded ? "ok" : "failed");
        return true;
    }

    for (uint8_t row = 0; row < ITEMS_PER_PAGE; ++row) {
        const int top = LIST_TOP + row * (ROW_HEIGHT + ROW_GAP);
        if (!inRect(x, y, 12, top, 216, ROW_HEIGHT)) continue;
        const uint8_t index = row;
        if (index >= stationCount) return false;
        if (activeIndex == static_cast<int8_t>(index) && playbackActive) {
            Serial.printf("[RADIO TOUCH] stop active index=%u name=%s\n",
                          index, stations[index].name);
            stop();
            return true;
        }
        stop();
        selectedIndex = index;
        pendingIndex = index;
        Serial.printf("[RADIO TOUCH] selected index=%u name=%s url=%s\n",
                      index, stations[index].name, stations[index].streamPath);
        return true;
    }
    return false;
}

bool process() {
    if (pendingIndex >= 0) {
        const int8_t index = pendingIndex;
        pendingIndex = -1;
        const bool started = startStream(static_cast<uint8_t>(index));
        Serial.printf("[RADIO] deferred start index=%d result=%s\n",
                      index, started ? "ok" : "failed");
        if (started) {
            activeIndex = index;
            selectedIndex = -1;
            marqueeOffset = 0;
        } else {
            selectedIndex = -1;
            activeIndex = -1;
        }
        // The tap refresh already rendered the pending row as active and
        // captured its SD-backed title glyphs. Avoid a second full-page render
        // while the radio producer is writing the stream cache on SD.
        return !started;
    }
    if (playbackEnded) {
        playbackEnded = false;
        activeIndex = -1;
        selectedIndex = -1;
        marqueeReady = false;
        return true;
    }
    return false;
}

bool isPlaying() { return playbackActive || pendingIndex >= 0; }

bool advanceMarquee(int16_t &rowTop) {
    if (!playbackActive || activeIndex < 0 || !marqueeReady || millis() < nextMarqueeMs) {
        return false;
    }
    marqueeOffset = (marqueeOffset + 12) % MARQUEE_WIDTH;
    nextMarqueeMs = millis() + 900;
    rowTop = LIST_TOP + activeIndex * (ROW_HEIGHT + ROW_GAP);
    return true;
}

void renderMarquee(uint8_t *destination, const uint8_t *currentFrame) {
    if (!destination || !currentFrame) return;
    std::memcpy(destination, currentFrame, XingtaiEpd::FRAME_BYTES);
    if (activeIndex < 0 || !marqueeReady) return;
    const int top = LIST_TOP + activeIndex * (ROW_HEIGHT + ROW_GAP);
    for (int y = top + 1; y < top + 1 + MARQUEE_HEIGHT; ++y) {
        for (int x = MARQUEE_X; x < MARQUEE_X + MARQUEE_WIDTH; ++x) {
            destination[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |=
                0x80U >> (x % 8);
        }
    }
    for (int y = 0; y < MARQUEE_HEIGHT; ++y) {
        for (int x = 0; x < MARQUEE_WIDTH; ++x) {
            if (!marqueePixel((x + marqueeOffset) % MARQUEE_WIDTH, y)) continue;
            const int drawX = MARQUEE_X + x;
            const int drawY = top + 1 + y;
            destination[static_cast<size_t>(drawY) * (XingtaiEpd::WIDTH / 8) + drawX / 8] &=
                static_cast<uint8_t>(~(0x80U >> (drawX % 8)));
        }
    }
}

void stop() {
    pendingIndex = -1;
    if (playbackTaskHandle || playbackActive) {
        stopRequested = true;
        const uint32_t started = millis();
        // Prefetch observes stopRequested between each 1 KiB chunk. Retain a
        // ceiling beyond the HTTP read timeout for a chunk already in progress.
        while (playbackTaskHandle && millis() - started < 13000) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        Serial.printf("[RADIO] Stop cleanup elapsed=%lums task=%p active=%s\n",
                      static_cast<unsigned long>(millis() - started),
                      playbackTaskHandle, playbackActive ? "yes" : "no");
    }
    if (!playbackTaskHandle) playbackActive = false;
    activeIndex = -1;
    selectedIndex = -1;
    marqueeReady = false;
    marqueeOffset = 0;
}

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    drawCenteredTitle(frame, 34, 18, UiLocalization::isChinese() ? "收音机" : "RADIO");
    rect(frame, 4, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT);
    drawDoubleArrow(frame, 21, PAGER_TOP + BUTTON_HEIGHT / 2, false);
    rect(frame, 42, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT);
    drawArrow(frame, 59, PAGER_TOP + BUTTON_HEIGHT / 2, false);
    rect(frame, 164, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT);
    drawArrow(frame, 181, PAGER_TOP + BUTTON_HEIGHT / 2, true);
    rect(frame, 202, PAGER_TOP, BUTTON_WIDTH, BUTTON_HEIGHT);
    drawDoubleArrow(frame, 219, PAGER_TOP + BUTTON_HEIGHT / 2, true);
    char pager[24] = {};
    const uint16_t currentPage = stationOffset / ITEMS_PER_PAGE + 1;
    snprintf(pager, sizeof(pager), "%u/%u", currentPage, pageCount());
    UiLocalization::drawCentered(frame, 67, pager);
    if (!stationCount) {
        UiLocalization::drawCentered(frame, 190, statusText);
        return;
    }
    for (uint8_t row = 0; row < stationCount; ++row) {
        const uint8_t index = row;
        const int top = LIST_TOP + row * (ROW_HEIGHT + ROW_GAP);
        rect(frame, 12, top, 216, ROW_HEIGHT);
        char number[8] = {};
        snprintf(number, sizeof(number), "%u", stationOffset + index + 1);
        UiLocalization::drawText(frame, 18, top + 9, number);
        drawTitle(frame, MARQUEE_X, top + 1, MARQUEE_WIDTH,
                  ROW_HEIGHT - 2, stations[index].name);
        const bool activeOrPending =
            (index == activeIndex && playbackActive) || index == pendingIndex;
        if (activeOrPending) drawPlay(frame, 215, top + ROW_HEIGHT / 2);
        else drawArrow(frame, 215, top + ROW_HEIGHT / 2, true);
        // Capture the normal black title glyphs before the active row is
        // inverted. The marquee renderer uses this mask to cut white letters
        // into a solid black strip during each scrolling partial refresh.
        if (activeOrPending && !marqueeReady) captureMarquee(frame, top);
        if (index == activeIndex || index == selectedIndex) invertRect(frame, 12, top, 216, ROW_HEIGHT);
    }
}

} // namespace RadioPage