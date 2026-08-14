#include "pages/recording/recording_page.h"

#include <Arduino.h>
#include <SD_MMC.h>

#include <cstring>
#include <ctime>

#include "board_pins.h"
#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/audio/opus_player.h"
#include "devices/es8311/es8311.h"
#include "devices/sd_card/sd_card.h"
#include "ui/localization.h"

namespace {

constexpr char ROOT_FOLDER[] = "/recordings";
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint16_t RECORDING_CHANNELS = 1;
constexpr uint16_t I2S_CHANNELS = 2;
constexpr uint16_t BITS_PER_SAMPLE = 16;
constexpr uint32_t BYTE_RATE = SAMPLE_RATE * RECORDING_CHANNELS * BITS_PER_SAMPLE / 8;
constexpr uint8_t TAG_COUNT = 6;
constexpr uint8_t MAX_FILES = 8;
constexpr int HEADER_X = 62;
constexpr int HEADER_Y = 40;
constexpr int HEADER_W = 136;
constexpr int HEADER_H = 30;
constexpr int DROPDOWN_X = 12;
constexpr int DROPDOWN_Y = 76;
constexpr int DROPDOWN_W = 216;
constexpr int DROPDOWN_H = 146;
constexpr int DROPDOWN_ITEM_W = 96;
constexpr int DROPDOWN_ITEM_H = 38;
constexpr int DROPDOWN_COLUMN_GAP = 8;
constexpr int DROPDOWN_ROW_GAP = 10;
constexpr int FILE_LIST_TOP = 108;
constexpr int FILE_ROW_HEIGHT = 30;
constexpr int FILE_ROW_GAP = 2;
constexpr int FILE_MARQUEE_X = 43;
constexpr int FILE_MARQUEE_WIDTH = 125;
constexpr int FILE_MARQUEE_HEIGHT = FILE_ROW_HEIGHT - 2;
constexpr int FILE_MARQUEE_ROW_BYTES = (FILE_MARQUEE_WIDTH + 7) / 8;
constexpr int FILE_PAGER_TOP = 76;
constexpr int FILE_PAGER_HEIGHT = 25;
constexpr int FILE_PAGER_BUTTON_WIDTH = 34;

constexpr const char *TAG_NAMES[TAG_COUNT] = {"Note", "Work", "Idea", "Buy", "Private", "Meeting"};
constexpr const char *TAG_NAMES_CN[TAG_COUNT] = {"笔记", "工作", "想法", "购买", "私人", "会议"};
constexpr const char *TAG_FOLDERS[TAG_COUNT] = {"note", "work", "idea", "buy", "private", "meeting"};

enum class View : uint8_t { Recorder, TagDropdown, Files, FileTagDropdown };

View view = View::Recorder;
Es8311 *codec = nullptr;
int8_t selectedTag = 0;
char fileNames[MAX_FILES][40] = {};
uint8_t fileCount = 0;
uint16_t fileTotal = 0;
uint16_t filePage = 1;
char activePath[96] = {};
File recordingFile;
TaskHandle_t recordingTaskHandle = nullptr;
volatile bool recordingActive = false;
volatile bool recordingPaused = false;
volatile bool recordingStopRequested = false;
volatile bool recordingCompleted = false;
volatile uint32_t recordedDataBytes = 0;
uint32_t recordingStartedMs = 0;
uint32_t pauseStartedMs = 0;
uint32_t pausedTotalMs = 0;
uint32_t lastRenderedSecond = UINT32_MAX;
char statusText[48] = "READY";
TaskHandle_t playbackTaskHandle = nullptr;
volatile bool playbackActive = false;
volatile bool playbackPaused = false;
volatile bool playbackStopRequested = false;
volatile bool playbackCompleted = false;
bool playbackPending = false;
int8_t activeFileIndex = -1;
char playbackPath[96] = {};
int16_t playbackInput[256] = {};
int16_t playbackOutput[512] = {};
uint8_t marqueeBitmap[FILE_MARQUEE_ROW_BYTES * FILE_MARQUEE_HEIGHT] = {};
bool marqueeReady = false;
uint16_t marqueeOffset = 0;
uint32_t nextMarqueeMs = 0;
bool exitRequested = false;

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
    line(frame, centerX - direction * 5, centerY - 7,
         centerX + direction * 3, centerY);
    line(frame, centerX + direction * 3, centerY,
         centerX - direction * 5, centerY + 7);
}

void drawDoubleArrow(uint8_t *frame, int centerX, int centerY, bool right) {
    drawArrow(frame, centerX - (right ? 4 : -4), centerY, right);
    drawArrow(frame, centerX + (right ? 4 : -4), centerY, right);
}

void hline(uint8_t *frame, int x, int y, int width) {
    for (int offset = 0; offset < width; ++offset) pixel(frame, x + offset, y);
}

void vline(uint8_t *frame, int x, int y, int height) {
    for (int offset = 0; offset < height; ++offset) pixel(frame, x, y + offset);
}

void roundedFrame(uint8_t *frame, int x, int y, int width, int height) {
    constexpr int radius = 6;
    hline(frame, x + radius, y, width - radius * 2);
    hline(frame, x + radius, y + height - 1, width - radius * 2);
    vline(frame, x, y + radius, height - radius * 2);
    vline(frame, x + width - 1, y + radius, height - radius * 2);
    constexpr uint8_t cornerX[] = {5, 4, 3, 2, 1, 1, 0};
    constexpr uint8_t cornerY[] = {0, 1, 1, 2, 3, 4, 5};
    for (size_t index = 0; index < sizeof(cornerX); ++index) {
        const int dx = cornerX[index];
        const int dy = cornerY[index];
        pixel(frame, x + dx, y + dy);
        pixel(frame, x + width - 1 - dx, y + dy);
        pixel(frame, x + dx, y + height - 1 - dy);
        pixel(frame, x + width - 1 - dx, y + height - 1 - dy);
    }
}

void circle(uint8_t *frame, int centerX, int centerY, int radius) {
    int x = radius, y = 0, error = 0;
    while (x >= y) {
        pixel(frame, centerX + x, centerY + y); pixel(frame, centerX + y, centerY + x);
        pixel(frame, centerX - y, centerY + x); pixel(frame, centerX - x, centerY + y);
        pixel(frame, centerX - x, centerY - y); pixel(frame, centerX - y, centerY - x);
        pixel(frame, centerX + y, centerY - x); pixel(frame, centerX + x, centerY - y);
        if (error <= 0) error += 2 * ++y + 1;
        if (error > 0) error -= 2 * --x + 1;
    }
}

void fillCircle(uint8_t *frame, int centerX, int centerY, int radius) {
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius) pixel(frame, centerX + x, centerY + y);
        }
    }
}

bool inRect(int16_t x, int16_t y, int left, int top, int width, int height) {
    return x >= left && x < left + width && y >= top && y < top + height;
}

bool framePixel(const uint8_t *frame, int x, int y) {
    if (!frame || x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) {
        return false;
    }
    return (frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] &
            (0x80U >> (x % 8))) != 0;
}

void captureMarquee(const uint8_t *frame, int top) {
    std::memset(marqueeBitmap, 0x00, sizeof(marqueeBitmap));
    for (int y = 0; y < FILE_MARQUEE_HEIGHT; ++y) {
        for (int x = 0; x < FILE_MARQUEE_WIDTH; ++x) {
            if (framePixel(frame, FILE_MARQUEE_X + x, top + 1 + y)) {
                marqueeBitmap[y * FILE_MARQUEE_ROW_BYTES + x / 8] |=
                    0x80U >> (x % 8);
            }
        }
    }
    marqueeReady = true;
    marqueeOffset = 0;
    nextMarqueeMs = millis() + 900;
}

bool marqueePixel(int x, int y) {
    return (marqueeBitmap[y * FILE_MARQUEE_ROW_BYTES + x / 8] &
            (0x80U >> (x % 8))) != 0;
}

const char *tagLabel(uint8_t tag) {
    if (tag >= TAG_COUNT) return "";
    return UiLocalization::isChinese() ? TAG_NAMES_CN[tag] : TAG_NAMES[tag];
}

const char *localizedStatus() {
    if (!UiLocalization::isChinese()) return statusText;
    if (std::strcmp(statusText, "READY") == 0) return "就绪";
    if (std::strcmp(statusText, "RECORDING") == 0) return "录音中";
    if (std::strcmp(statusText, "PAUSED") == 0) return "已暂停";
    if (std::strcmp(statusText, "SAVING") == 0) return "保存中";
    if (std::strcmp(statusText, "SAVED") == 0) return "已保存";
    if (std::strcmp(statusText, "RECORDING UNAVAILABLE") == 0) return "录音不可用";
    if (std::strcmp(statusText, "FILE OPEN FAILED") == 0) return "文件打开失败";
    if (std::strcmp(statusText, "RECORD TASK FAILED") == 0) return "录音任务失败";
    if (std::strcmp(statusText, "STOP TIMEOUT") == 0) return "停止超时";
    return statusText;
}

void writeLe16(File &file, uint16_t value) {
    const uint8_t bytes[2] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
    file.write(bytes, sizeof(bytes));
}

void writeLe32(File &file, uint32_t value) {
    const uint8_t bytes[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                              static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
    file.write(bytes, sizeof(bytes));
}

void writeWavHeader(File &file, uint32_t dataBytes) {
    file.seek(0);
    file.write(reinterpret_cast<const uint8_t *>("RIFF"), 4);
    writeLe32(file, 36 + dataBytes);
    file.write(reinterpret_cast<const uint8_t *>("WAVEfmt "), 8);
    writeLe32(file, 16); writeLe16(file, 1); writeLe16(file, RECORDING_CHANNELS);
    writeLe32(file, SAMPLE_RATE); writeLe32(file, BYTE_RATE);
    writeLe16(file, RECORDING_CHANNELS * BITS_PER_SAMPLE / 8);
    writeLe16(file, BITS_PER_SAMPLE);
    file.write(reinterpret_cast<const uint8_t *>("data"), 4);
    writeLe32(file, dataBytes);
}

uint16_t readLe16(const uint8_t *bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           (static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t readLe32(const uint8_t *bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

bool seekWavData(File &file, uint32_t &dataBytes, uint16_t &channels) {
    uint8_t header[12] = {};
    if (file.read(header, sizeof(header)) != sizeof(header) ||
        std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) {
        return false;
    }
    while (file.available()) {
        uint8_t chunk[8] = {};
        if (file.read(chunk, sizeof(chunk)) != sizeof(chunk)) return false;
        const uint32_t size = readLe32(chunk + 4);
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t format[16] = {};
            if (size < sizeof(format) || file.read(format, sizeof(format)) != sizeof(format)) return false;
            channels = readLe16(format + 2);
            if (readLe16(format) != 1 || (channels != 1 && channels != 2) ||
                readLe32(format + 4) != SAMPLE_RATE || readLe16(format + 14) != BITS_PER_SAMPLE) {
                return false;
            }
            if (size > sizeof(format) && !file.seek(file.position() + size - sizeof(format))) return false;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            dataBytes = size;
            return true;
        } else if (!file.seek(file.position() + size + (size & 1U))) {
            return false;
        }
    }
    return false;
}

void playbackTask(void *) {
    File file = SD_MMC.open(playbackPath, FILE_READ);
    uint32_t remaining = 0;
    uint16_t channels = 0;
    bool valid = file && seekWavData(file, remaining, channels);
    if (valid && codec) {
        pinMode(BoardPins::PA_EN, OUTPUT);
        digitalWrite(BoardPins::PA_EN, HIGH);
        codec->setSpeakerEnabled(true);
        vTaskDelay(pdMS_TO_TICKS(60));
        Serial.printf("[RECORDING PLAYBACK] Started path=%s bytes=%lu channels=%u PA=%d\n",
                      playbackPath, static_cast<unsigned long>(remaining), channels,
                      digitalRead(BoardPins::PA_EN));
        uint16_t playbackPeak = 0;
        while (!playbackStopRequested && remaining > 0) {
            if (playbackPaused) {
                codec->setSpeakerEnabled(false);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            codec->setSpeakerEnabled(true);
            const size_t wanted = min<size_t>(sizeof(playbackInput), remaining);
            const size_t bytes = file.read(reinterpret_cast<uint8_t *>(playbackInput), wanted);
            if (bytes == 0) break;
            remaining -= bytes;
            const size_t inputSamples = bytes / sizeof(int16_t);
            const size_t frames = inputSamples / channels;
            for (size_t frame = 0; frame < frames; ++frame) {
                int16_t sample = playbackInput[frame * channels];
                if (channels == 2) {
                    const int16_t other = playbackInput[frame * channels + 1];
                    if (abs(static_cast<int32_t>(other)) > abs(static_cast<int32_t>(sample))) {
                        sample = other;
                    }
                }
                playbackPeak = max<uint16_t>(playbackPeak,
                    static_cast<uint16_t>(min<int32_t>(32767, abs(static_cast<int32_t>(sample)))));
                playbackOutput[frame * I2S_CHANNELS] = sample;
                playbackOutput[frame * I2S_CHANNELS + 1] = sample;
            }
            const size_t outputSamples = frames * I2S_CHANNELS;
            if (codec->write(playbackOutput, outputSamples, 1000) != outputSamples) break;
        }
        Serial.printf("[RECORDING PLAYBACK] Peak=%u stack_free=%u\n", playbackPeak,
                      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    } else {
        Serial.printf("[RECORDING PLAYBACK] Invalid WAV path=%s\n", playbackPath);
    }
    if (file) file.close();
    if (codec) codec->setSpeakerEnabled(false);
    digitalWrite(BoardPins::PA_EN, LOW);
    playbackActive = false;
    playbackPaused = false;
    playbackStopRequested = false;
    playbackCompleted = true;
    playbackTaskHandle = nullptr;
    Serial.println("[RECORDING PLAYBACK] Finished");
    vTaskDelete(nullptr);
}

void stopPlayback() {
    playbackPending = false;
    if (playbackTaskHandle || playbackActive) {
        playbackStopRequested = true;
        playbackPaused = false;
        const uint32_t started = millis();
        while (playbackTaskHandle && millis() - started < 2000) vTaskDelay(pdMS_TO_TICKS(5));
    }
    playbackActive = false;
    playbackPaused = false;
    playbackStopRequested = false;
    playbackCompleted = false;
    activeFileIndex = -1;
    marqueeReady = false;
    marqueeOffset = 0;
}

bool startPlayback() {
    if (activeFileIndex < 0 || activeFileIndex >= static_cast<int8_t>(fileCount) ||
        selectedTag < 0 || selectedTag >= TAG_COUNT || !codec || !codec->isInitialized()) {
        return false;
    }
    OpusPlayer::stop();
    snprintf(playbackPath, sizeof(playbackPath), "%s/%s/%s", ROOT_FOLDER,
             TAG_FOLDERS[selectedTag], fileNames[activeFileIndex]);
    if (!SD_MMC.exists(playbackPath)) return false;
    playbackStopRequested = false;
    playbackPaused = false;
    playbackCompleted = false;
    playbackActive = true;
    marqueeReady = false;
    marqueeOffset = 0;
    if (xTaskCreatePinnedToCore(playbackTask, "wav-playback", 8192, nullptr, 2,
                                &playbackTaskHandle, 0) != pdPASS) {
        playbackActive = false;
        playbackTaskHandle = nullptr;
        return false;
    }
    return true;
}

bool ensureFolders(int8_t tag) {
    if (!SdCard::isMounted() || tag < 0 || tag >= TAG_COUNT) return false;
    if (!SD_MMC.exists(ROOT_FOLDER) && !SD_MMC.mkdir(ROOT_FOLDER)) return false;
    char folder[48] = {};
    snprintf(folder, sizeof(folder), "%s/%s", ROOT_FOLDER, TAG_FOLDERS[tag]);
    return SD_MMC.exists(folder) || SD_MMC.mkdir(folder);
}

void buildRecordingPath(int8_t tag, char *path, size_t pathSize) {
    if (!path || pathSize == 0 || tag < 0 || tag >= TAG_COUNT) return;
    time_t now = time(nullptr);
    tm local = {};
    localtime_r(&now, &local);
    char date[9] = {};
    char timestamp[7] = {};
    if (local.tm_year + 1900 >= 2024) {
        std::strftime(date, sizeof(date), "%Y%m%d", &local);
        std::strftime(timestamp, sizeof(timestamp), "%H%M%S", &local);
    } else {
        std::strcpy(date, "nosync");
        snprintf(timestamp, sizeof(timestamp), "%06lu",
                 static_cast<unsigned long>((millis() / 1000) % 1000000UL));
    }

    snprintf(path, pathSize, "%s/%s/%s_%s_%s.wav", ROOT_FOLDER,
             TAG_FOLDERS[tag], TAG_FOLDERS[tag], date, timestamp);
    if (!SD_MMC.exists(path)) return;
    for (uint8_t suffix = 2; suffix < 100; ++suffix) {
        snprintf(path, pathSize, "%s/%s/%s_%s_%s_%u.wav", ROOT_FOLDER,
                 TAG_FOLDERS[tag], TAG_FOLDERS[tag], date, timestamp, suffix);
        if (!SD_MMC.exists(path)) return;
    }
}

uint32_t elapsedSeconds() {
    if (!recordingActive && recordingStartedMs == 0) return 0;
    const uint32_t now = recordingPaused ? pauseStartedMs : millis();
    return (now - recordingStartedMs - pausedTotalMs) / 1000;
}

void recordingTask(void *) {
    int16_t i2sSamples[480] = {};
    int16_t monoSamples[240] = {};
    uint16_t recordingPeak = 0;
    uint32_t emptyReads = 0;
    while (!recordingStopRequested) {
        const size_t count = codec ? codec->read(i2sSamples, 480, 100) : 0;
        if (count == 0) {
            ++emptyReads;
            continue;
        }
        if (!recordingPaused && recordingFile) {
            const size_t frames = count / I2S_CHANNELS;
            for (size_t frame = 0; frame < frames; ++frame) {
                int16_t sample = i2sSamples[frame * I2S_CHANNELS];
                const int16_t other = i2sSamples[frame * I2S_CHANNELS + 1];
                if (abs(static_cast<int32_t>(other)) > abs(static_cast<int32_t>(sample))) {
                    sample = other;
                }
                monoSamples[frame] = sample;
                recordingPeak = max<uint16_t>(recordingPeak,
                    static_cast<uint16_t>(min<int32_t>(32767, abs(static_cast<int32_t>(sample)))));
            }
            recordedDataBytes += recordingFile.write(
                reinterpret_cast<const uint8_t *>(monoSamples), frames * sizeof(int16_t));
        }
    }
    if (recordingFile) {
        writeWavHeader(recordingFile, recordedDataBytes);
        recordingFile.flush();
        recordingFile.close();
    }
    recordingActive = false;
    recordingPaused = false;
    recordingStopRequested = false;
    recordingCompleted = true;
    Serial.printf("[RECORDING] Capture peak=%u empty_reads=%lu\n", recordingPeak,
                  static_cast<unsigned long>(emptyReads));
    recordingTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

bool startRecording() {
    stopPlayback();
    if (recordingActive || recordingTaskHandle || !codec || !codec->isInitialized() ||
        !ensureFolders(selectedTag)) {
        std::strcpy(statusText, "RECORDING UNAVAILABLE");
        return false;
    }
    buildRecordingPath(selectedTag, activePath, sizeof(activePath));
    recordingFile = SD_MMC.open(activePath, FILE_WRITE);
    if (!recordingFile) { std::strcpy(statusText, "FILE OPEN FAILED"); return false; }
    writeWavHeader(recordingFile, 0);
    recordedDataBytes = 0;
    recordingStopRequested = false;
    recordingCompleted = false;
    recordingPaused = false;
    recordingStartedMs = millis();
    pauseStartedMs = 0;
    pausedTotalMs = 0;
    recordingActive = true;
    lastRenderedSecond = UINT32_MAX;
    std::strcpy(statusText, "RECORDING");
    if (!codec->prepareRecording(30)) {
        recordingActive = false;
        recordingFile.close();
        SD_MMC.remove(activePath);
        std::strcpy(statusText, "RECORDING UNAVAILABLE");
        Serial.println("[RECORDING] ES8311 ADC preparation failed");
        return false;
    }
    uint8_t microphoneMode = 0;
    uint8_t microphoneGain = 0;
    uint8_t adcVolume = 0;
    codec->readRegister(0x14, microphoneMode);
    codec->readRegister(0x16, microphoneGain);
    codec->readRegister(0x17, adcVolume);
    Serial.printf("[RECORDING] ES8311 ADC reg14=0x%02X reg16=0x%02X reg17=0x%02X\n",
                  microphoneMode, microphoneGain, adcVolume);
    if (xTaskCreatePinnedToCore(recordingTask, "wav-recording", 4096, nullptr, 2,
                                &recordingTaskHandle, 0) != pdPASS) {
        recordingActive = false;
        recordingFile.close();
        SD_MMC.remove(activePath);
        std::strcpy(statusText, "RECORD TASK FAILED");
        return false;
    }
    Serial.printf("[RECORDING] Started tag=%s path=%s\n", TAG_NAMES[selectedTag], activePath);
    return true;
}

void stopRecording() {
    if (!recordingActive && !recordingTaskHandle) return;
    recordingStopRequested = true;
    recordingPaused = false;
    std::strcpy(statusText, "SAVING");
    Serial.printf("[RECORDING] Stop requested path=%s\n", activePath);
}

void togglePause() {
    if (!recordingActive) return;
    if (!recordingPaused) {
        recordingPaused = true;
        pauseStartedMs = millis();
        std::strcpy(statusText, "PAUSED");
    } else {
        pausedTotalMs += millis() - pauseStartedMs;
        recordingPaused = false;
        std::strcpy(statusText, "RECORDING");
    }
}

uint16_t filePageCount() {
    return fileTotal > 0 ? (fileTotal + MAX_FILES - 1) / MAX_FILES : 1;
}

void loadFiles(int8_t tag) {
    fileCount = 0;
    fileTotal = 0;
    std::memset(fileNames, 0, sizeof(fileNames));
    if (!ensureFolders(tag)) return;
    char folder[48] = {};
    snprintf(folder, sizeof(folder), "%s/%s", ROOT_FOLDER, TAG_FOLDERS[tag]);
    File root = SD_MMC.open(folder);
    if (!root || !root.isDirectory()) { if (root) root.close(); return; }
    File entry = root.openNextFile();
    while (entry) {
        const String path(entry.name());
        const int slash = path.lastIndexOf('/');
        const String name = path.substring(slash + 1);
        if (!entry.isDirectory() && name.endsWith(".wav")) ++fileTotal;
        entry.close();
        entry = root.openNextFile();
    }
    root.close();

    if (filePage > filePageCount()) filePage = filePageCount();
    const uint16_t firstItem = (filePage - 1) * MAX_FILES;
    uint16_t validIndex = 0;
    root = SD_MMC.open(folder);
    if (!root || !root.isDirectory()) { if (root) root.close(); return; }
    entry = root.openNextFile();
    while (entry && fileCount < MAX_FILES) {
        const String path(entry.name());
        const int slash = path.lastIndexOf('/');
        const String name = path.substring(slash + 1);
        if (!entry.isDirectory() && name.endsWith(".wav")) {
            if (validIndex >= firstItem) {
                std::strncpy(fileNames[fileCount], name.c_str(),
                             sizeof(fileNames[fileCount]) - 1);
                ++fileCount;
            }
            ++validIndex;
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
    Serial.printf("[RECORDING FILES] tag=%s page=%u/%u items=%u total=%u\n",
                  TAG_NAMES[tag], filePage, filePageCount(), fileCount, fileTotal);
}

void drawBack(uint8_t *frame) {
    rect(frame, 10, 40, 48, 28);
    const char *label = UiLocalization::isChinese() ? "返回" : "BACK";
    const int width = UiLocalization::textWidth(label, 1);
    UiLocalization::drawText(frame, 10 + (48 - width) / 2, 49, label, 1);
}

void drawFolder(uint8_t *frame, int x, int y) {
    // One-pixel contour based on folder.svg's tab, rear panel, and sloped front.
    line(frame, x + 3, y + 8, x + 14, y + 8);
    line(frame, x + 14, y + 8, x + 17, y + 12);
    line(frame, x + 17, y + 12, x + 40, y + 12);
    line(frame, x + 40, y + 12, x + 40, y + 20);
    line(frame, x + 3, y + 8, x + 3, y + 39);
    line(frame, x + 8, y + 20, x + 45, y + 20);
    line(frame, x + 45, y + 20, x + 40, y + 41);
    line(frame, x + 40, y + 41, x + 4, y + 41);
    line(frame, x + 4, y + 41, x + 8, y + 20);
}

void drawTagHeader(uint8_t *frame, bool fileTitle = false) {
    rect(frame, HEADER_X, HEADER_Y, HEADER_W, HEADER_H);
    char title[48] = {};
    const char *label = tagLabel(selectedTag);
    if (fileTitle) {
        if (UiLocalization::isChinese()) snprintf(title, sizeof(title), "%s录音", label);
        else snprintf(title, sizeof(title), "%s RECORDINGS", label);
        label = title;
    }
    const int labelWidth = UiLocalization::textWidth(label, 1);
    UiLocalization::drawText(frame, HEADER_X + (HEADER_W - labelWidth) / 2 - 5,
                             HEADER_Y + 10, label, 1);
    const int centerX = HEADER_X + HEADER_W - 13;
    const int centerY = HEADER_Y + HEADER_H / 2;
    line(frame, centerX - 5, centerY - 3, centerX, centerY + 3);
    line(frame, centerX, centerY + 3, centerX + 5, centerY - 3);
}

void drawTagDropdown(uint8_t *frame) {
    drawBack(frame);
    drawTagHeader(frame, view == View::FileTagDropdown);
    rect(frame, DROPDOWN_X, DROPDOWN_Y, DROPDOWN_W, DROPDOWN_H);
    for (uint8_t i = 0; i < TAG_COUNT; ++i) {
        const int column = i % 2;
        const int row = i / 2;
        const int left = DROPDOWN_X + 8 +
                         column * (DROPDOWN_ITEM_W + DROPDOWN_COLUMN_GAP);
        const int top = DROPDOWN_Y + 8 + row * (DROPDOWN_ITEM_H + DROPDOWN_ROW_GAP);
        rect(frame, left, top, DROPDOWN_ITEM_W, DROPDOWN_ITEM_H);
        const char *label = tagLabel(i);
        const int width = UiLocalization::textWidth(label, 1);
        UiLocalization::drawText(frame, left + (DROPDOWN_ITEM_W - width) / 2,
                                 top + 13, label, 1);
    }
}

void drawPlayPause(uint8_t *frame, int centerX, int centerY, bool pause) {
    if (pause) {
        rect(frame, centerX - 18, centerY - 21, 12, 43);
        rect(frame, centerX + 7, centerY - 21, 12, 43);
        return;
    }
    line(frame, centerX - 15, centerY - 22, centerX - 15, centerY + 22);
    line(frame, centerX - 15, centerY - 22, centerX + 20, centerY);
    line(frame, centerX + 20, centerY, centerX - 15, centerY + 22);
}

void drawRowPlayPause(uint8_t *frame, int centerX, int centerY, bool pause) {
    if (pause) {
        rect(frame, centerX - 7, centerY - 8, 5, 17);
        rect(frame, centerX + 3, centerY - 8, 5, 17);
        return;
    }
    line(frame, centerX - 6, centerY - 9, centerX - 6, centerY + 9);
    line(frame, centerX - 6, centerY - 9, centerX + 8, centerY);
    line(frame, centerX + 8, centerY, centerX - 6, centerY + 9);
}

void drawTrash(uint8_t *frame, int centerX, int centerY) {
    rect(frame, centerX - 6, centerY - 5, 13, 15);
    line(frame, centerX - 8, centerY - 8, centerX + 8, centerY - 8);
    line(frame, centerX - 3, centerY - 11, centerX + 3, centerY - 11);
    line(frame, centerX - 3, centerY - 11, centerX - 3, centerY - 9);
    line(frame, centerX + 3, centerY - 11, centerX + 3, centerY - 9);
    line(frame, centerX - 2, centerY - 2, centerX - 2, centerY + 7);
    line(frame, centerX + 2, centerY - 2, centerX + 2, centerY + 7);
}

void drawRecordButton(uint8_t *frame) {
    circle(frame, 120, 361, 39);
    if (recordingActive) fillCircle(frame, 120, 361, 14);
    else rect(frame, 108, 349, 24, 24);
}

void drawRecorder(uint8_t *frame) {
    drawBack(frame);
    drawTagHeader(frame);
    const uint32_t seconds = elapsedSeconds();
    char timer[20] = {};
    snprintf(timer, sizeof(timer), "%02lu:%02lu:%02lu",
             static_cast<unsigned long>(seconds / 3600),
             static_cast<unsigned long>((seconds / 60) % 60),
             static_cast<unsigned long>(seconds % 60));
    UiLocalization::drawCentered(frame, 120, timer, 4);
    UiLocalization::drawCentered(frame, 178, localizedStatus(), 1);
    line(frame, 12, 278, 228, 278);
    roundedFrame(frame, 12, 333, 56, 56);
    drawFolder(frame, 16, 337);
    drawRecordButton(frame);
    roundedFrame(frame, 172, 333, 56, 56);
    drawPlayPause(frame, 200, 361, !recordingPaused);
}

void drawFiles(uint8_t *frame) {
    drawBack(frame);
    drawTagHeader(frame, true);
    rect(frame, 4, FILE_PAGER_TOP, FILE_PAGER_BUTTON_WIDTH, FILE_PAGER_HEIGHT);
    drawDoubleArrow(frame, 21, FILE_PAGER_TOP + FILE_PAGER_HEIGHT / 2, false);
    rect(frame, 42, FILE_PAGER_TOP, FILE_PAGER_BUTTON_WIDTH, FILE_PAGER_HEIGHT);
    drawArrow(frame, 59, FILE_PAGER_TOP + FILE_PAGER_HEIGHT / 2, false);
    rect(frame, 164, FILE_PAGER_TOP, FILE_PAGER_BUTTON_WIDTH, FILE_PAGER_HEIGHT);
    drawArrow(frame, 181, FILE_PAGER_TOP + FILE_PAGER_HEIGHT / 2, true);
    rect(frame, 202, FILE_PAGER_TOP, FILE_PAGER_BUTTON_WIDTH, FILE_PAGER_HEIGHT);
    drawDoubleArrow(frame, 219, FILE_PAGER_TOP + FILE_PAGER_HEIGHT / 2, true);
    char pager[24] = {};
    if (UiLocalization::isChinese()) {
        snprintf(pager, sizeof(pager), "%u/%u", filePage, filePageCount());
    } else {
        snprintf(pager, sizeof(pager), "PAGE %u OF %u", filePage, filePageCount());
    }
    UiLocalization::drawCentered(frame, FILE_PAGER_TOP + 9, pager, 1);
    if (fileCount == 0) {
        UiLocalization::drawCentered(frame, 190,
            UiLocalization::isChinese() ? "没有录音" : "NO RECORDINGS", 1);
        return;
    }
    for (uint8_t i = 0; i < fileCount; ++i) {
        const int top = FILE_LIST_TOP + i * (FILE_ROW_HEIGHT + FILE_ROW_GAP);
        rect(frame, 12, top, 186, FILE_ROW_HEIGHT);
        char number[5] = {};
        snprintf(number, sizeof(number), "%u.",
                 static_cast<unsigned>((filePage - 1) * MAX_FILES + i + 1));
        UiLocalization::drawText(frame, 18, top + 9, number, 1);
        char displayName[24] = {};
        std::strncpy(displayName, fileNames[i], sizeof(displayName) - 1);
        UiLocalization::drawText(frame, FILE_MARQUEE_X, top + 9, displayName, 1);
        drawTrash(frame, 218, top + 15);
        if (i == activeFileIndex) {
            drawRowPlayPause(frame, 181, top + 15, !playbackPaused);
            if (!marqueeReady) captureMarquee(frame, top);
            for (int y = top; y < top + FILE_ROW_HEIGHT; ++y) {
                for (int x = 12; x < 198; ++x) {
                    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] ^=
                        0x80U >> (x % 8);
                }
            }
        }
    }
}

} // namespace

namespace RecordingPage {

void setAudio(Es8311 *audio) { codec = audio; }

void open() {
    stopRecording();
    stopPlayback();
    view = View::Recorder;
    if (selectedTag < 0 || selectedTag >= TAG_COUNT) selectedTag = 0;
    filePage = 1;
    fileTotal = 0;
    exitRequested = false;
    recordingStartedMs = 0;
    std::strcpy(statusText, "READY");
}

bool returnControlAt(int16_t x, int16_t y) {
    return inRect(x, y, 10, 40, 48, 28);
}

bool headerControlAt(int16_t x, int16_t y) {
    return (view == View::Recorder || view == View::TagDropdown ||
            view == View::Files || view == View::FileTagDropdown) &&
           inRect(x, y, HEADER_X, HEADER_Y, HEADER_W, HEADER_H);
}

bool folderControlAt(int16_t x, int16_t y) {
    return view == View::Recorder && inRect(x, y, 12, 333, 56, 56);
}

bool pauseControlAt(int16_t x, int16_t y) {
    return view == View::Recorder && inRect(x, y, 172, 333, 56, 56);
}

bool tagItemBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                     int16_t &width, int16_t &height) {
    if (view == View::TagDropdown || view == View::FileTagDropdown) {
        for (uint8_t i = 0; i < TAG_COUNT; ++i) {
            const int column = i % 2;
            const int row = i / 2;
            const int itemLeft = DROPDOWN_X + 8 +
                                 column * (DROPDOWN_ITEM_W + DROPDOWN_COLUMN_GAP);
            const int itemTop = DROPDOWN_Y + 8 +
                                row * (DROPDOWN_ITEM_H + DROPDOWN_ROW_GAP);
            if (!inRect(x, y, itemLeft, itemTop, DROPDOWN_ITEM_W, DROPDOWN_ITEM_H)) {
                continue;
            }
            left = itemLeft;
            top = itemTop;
            width = DROPDOWN_ITEM_W;
            height = DROPDOWN_ITEM_H;
            return true;
        }
        return false;
    }
    return false;
}

bool pagerControlBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                          int16_t &width, int16_t &height) {
    if (view != View::Files || y < FILE_PAGER_TOP ||
        y >= FILE_PAGER_TOP + FILE_PAGER_HEIGHT) return false;
    constexpr int lefts[] = {4, 42, 164, 202};
    for (uint8_t index = 0; index < 4; ++index) {
        const bool enabled = index < 2 ? filePage > 1 : filePage < filePageCount();
        if (enabled && x >= lefts[index] && x < lefts[index] + FILE_PAGER_BUTTON_WIDTH) {
            left = lefts[index];
            top = FILE_PAGER_TOP;
            width = FILE_PAGER_BUTTON_WIDTH;
            height = FILE_PAGER_HEIGHT;
            return true;
        }
    }
    return false;
}

bool handleTap(int16_t x, int16_t y) {
    if (view == View::TagDropdown || view == View::FileTagDropdown) {
        const bool fileDropdown = view == View::FileTagDropdown;
        if (inRect(x, y, 10, 40, 48, 28) ||
            inRect(x, y, HEADER_X, HEADER_Y, HEADER_W, HEADER_H)) {
            view = fileDropdown ? View::Files : View::Recorder;
            return true;
        }
        for (uint8_t i = 0; i < TAG_COUNT; ++i) {
            const int column = i % 2;
            const int row = i / 2;
            const int left = DROPDOWN_X + 8 +
                             column * (DROPDOWN_ITEM_W + DROPDOWN_COLUMN_GAP);
            const int top = DROPDOWN_Y + 8 +
                            row * (DROPDOWN_ITEM_H + DROPDOWN_ROW_GAP);
            if (!inRect(x, y, left, top, DROPDOWN_ITEM_W, DROPDOWN_ITEM_H)) continue;
            selectedTag = i;
            if (fileDropdown) {
                stopPlayback();
                filePage = 1;
                loadFiles(selectedTag);
                view = View::Files;
            } else {
                recordingStartedMs = 0;
                std::strcpy(statusText, "READY");
                view = View::Recorder;
            }
            return true;
        }
        return false;
    }
    if (view == View::Files) {
        if (inRect(x, y, 10, 40, 48, 28)) {
            stopPlayback();
            view = View::Recorder;
            return true;
        }
        if (inRect(x, y, HEADER_X, HEADER_Y, HEADER_W, HEADER_H)) {
            stopPlayback();
            view = View::FileTagDropdown;
            return true;
        }
        if (inRect(x, y, 4, FILE_PAGER_TOP, FILE_PAGER_BUTTON_WIDTH,
                   FILE_PAGER_HEIGHT) && filePage > 1) {
            stopPlayback();
            filePage = 1;
            loadFiles(selectedTag);
            return true;
        }
        if (inRect(x, y, 42, FILE_PAGER_TOP, FILE_PAGER_BUTTON_WIDTH,
                   FILE_PAGER_HEIGHT) && filePage > 1) {
            stopPlayback();
            --filePage;
            loadFiles(selectedTag);
            return true;
        }
        if (inRect(x, y, 164, FILE_PAGER_TOP, FILE_PAGER_BUTTON_WIDTH,
                   FILE_PAGER_HEIGHT) && filePage < filePageCount()) {
            stopPlayback();
            ++filePage;
            loadFiles(selectedTag);
            return true;
        }
        if (inRect(x, y, 202, FILE_PAGER_TOP, FILE_PAGER_BUTTON_WIDTH,
                   FILE_PAGER_HEIGHT) && filePage < filePageCount()) {
            stopPlayback();
            filePage = filePageCount();
            loadFiles(selectedTag);
            return true;
        }
        for (uint8_t i = 0; i < fileCount; ++i) {
            const int top = FILE_LIST_TOP + i * (FILE_ROW_HEIGHT + FILE_ROW_GAP);
            if (inRect(x, y, 204, top, 28, FILE_ROW_HEIGHT)) {
                stopPlayback();
                char path[96] = {};
                snprintf(path, sizeof(path), "%s/%s/%s", ROOT_FOLDER,
                         TAG_FOLDERS[selectedTag], fileNames[i]);
                const bool removed = SD_MMC.remove(path);
                Serial.printf("[RECORDING] Delete path=%s result=%s\n",
                              path, removed ? "ok" : "failed");
                loadFiles(selectedTag);
                return true;
            }
            if (!inRect(x, y, 12, top, 186, FILE_ROW_HEIGHT)) continue;
            if (activeFileIndex == static_cast<int8_t>(i) && playbackActive) {
                playbackPaused = !playbackPaused;
            } else {
                stopPlayback();
                activeFileIndex = static_cast<int8_t>(i);
                playbackPending = true;
            }
            return true;
        }
        return false;
    }
    if (inRect(x, y, 10, 40, 48, 28)) {
        stopRecording();
        exitRequested = true;
        return true;
    }
    if (inRect(x, y, HEADER_X, HEADER_Y, HEADER_W, HEADER_H)) {
        stopRecording();
        view = View::TagDropdown;
        return true;
    }
    if (inRect(x, y, 12, 333, 56, 56)) {
        stopRecording();
        filePage = 1;
        loadFiles(selectedTag);
        view = View::Files;
        return true;
    }
    if (inRect(x, y, 80, 320, 80, 81)) {
        if (recordingActive) stopRecording(); else startRecording();
        return true;
    }
    if (inRect(x, y, 172, 333, 56, 56)) { togglePause(); return true; }
    return false;
}

bool takeExitRequest() {
    const bool requested = exitRequested;
    exitRequested = false;
    return requested;
}

bool process() {
    if (recordingCompleted) {
        recordingCompleted = false;
        std::strcpy(statusText, "SAVED");
        Serial.printf("[RECORDING] Saved path=%s bytes=%lu\n",
                      activePath, static_cast<unsigned long>(recordedDataBytes));
        return true;
    }
    if (playbackPending) {
        playbackPending = false;
        if (!startPlayback()) activeFileIndex = -1;
        return true;
    }
    if (playbackCompleted) {
        playbackCompleted = false;
        activeFileIndex = -1;
        marqueeReady = false;
        marqueeOffset = 0;
        return true;
    }
    if (view == View::Recorder && recordingActive) {
        const uint32_t second = elapsedSeconds();
        if (second != lastRenderedSecond) {
            lastRenderedSecond = second;
            return true;
        }
    }
    return false;
}

bool advanceMarquee(int16_t &rowTop) {
    if (view != View::Files || !playbackActive || playbackPaused ||
        activeFileIndex < 0 || !marqueeReady || millis() < nextMarqueeMs) {
        return false;
    }
    marqueeOffset = (marqueeOffset + 12) % FILE_MARQUEE_WIDTH;
    nextMarqueeMs = millis() + 900;
    rowTop = FILE_LIST_TOP + activeFileIndex * (FILE_ROW_HEIGHT + FILE_ROW_GAP);
    return true;
}

void renderMarquee(uint8_t *destination, const uint8_t *currentFrame) {
    if (!destination || !currentFrame) return;
    std::memcpy(destination, currentFrame, XingtaiEpd::FRAME_BYTES);
    if (activeFileIndex < 0 || !marqueeReady) return;
    const int top = FILE_LIST_TOP + activeFileIndex * (FILE_ROW_HEIGHT + FILE_ROW_GAP);
    for (int y = 0; y < FILE_MARQUEE_HEIGHT; ++y) {
        for (int x = 0; x < FILE_MARQUEE_WIDTH; ++x) {
            const int drawX = FILE_MARQUEE_X + x;
            const int drawY = top + 1 + y;
            destination[static_cast<size_t>(drawY) * (XingtaiEpd::WIDTH / 8) + drawX / 8] |=
                0x80U >> (drawX % 8);
        }
    }
    for (int y = 0; y < FILE_MARQUEE_HEIGHT; ++y) {
        for (int x = 0; x < FILE_MARQUEE_WIDTH; ++x) {
            const int sourceX = (x + marqueeOffset) % FILE_MARQUEE_WIDTH;
            if (!marqueePixel(sourceX, y)) continue;
            const int drawX = FILE_MARQUEE_X + x;
            const int drawY = top + 1 + y;
            destination[static_cast<size_t>(drawY) * (XingtaiEpd::WIDTH / 8) + drawX / 8] &=
                static_cast<uint8_t>(~(0x80U >> (drawX % 8)));
        }
    }
}

bool isRecording() { return recordingActive; }
void stop() { stopRecording(); stopPlayback(); }

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    switch (view) {
    case View::Recorder: drawRecorder(frame); break;
    case View::TagDropdown: drawTagDropdown(frame); break;
    case View::Files: drawFiles(frame); break;
    case View::FileTagDropdown: drawTagDropdown(frame); break;
    }
}

} // namespace RecordingPage