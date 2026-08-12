#include "pages/recording/recording_page.h"

#include <Arduino.h>
#include <SD_MMC.h>

#include <cstring>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/es8311/es8311.h"
#include "devices/sd_card/sd_card.h"
#include "ui/localization.h"

namespace {

constexpr char ROOT_FOLDER[] = "/recordings";
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint16_t CHANNELS = 2;
constexpr uint16_t BITS_PER_SAMPLE = 16;
constexpr uint32_t BYTE_RATE = SAMPLE_RATE * CHANNELS * BITS_PER_SAMPLE / 8;
constexpr uint8_t TAG_COUNT = 5;
constexpr uint8_t MAX_FILES = 8;
constexpr int TAG_X = 48;
constexpr int TAG_Y = 92;
constexpr int TAG_W = 144;
constexpr int TAG_H = 38;
constexpr int TAG_GAP = 10;

constexpr const char *TAG_NAMES[TAG_COUNT] = {"Note", "Work", "Idea", "Buy", "Private"};
constexpr const char *TAG_FOLDERS[TAG_COUNT] = {"note", "work", "idea", "buy", "private"};

enum class View : uint8_t { Tags, Recorder, BrowseTags, Files };

View view = View::Tags;
Es8311 *codec = nullptr;
int8_t selectedTag = -1;
char fileNames[MAX_FILES][40] = {};
uint8_t fileCount = 0;
char activePath[96] = {};
File recordingFile;
TaskHandle_t recordingTaskHandle = nullptr;
volatile bool recordingActive = false;
volatile bool recordingPaused = false;
volatile bool recordingStopRequested = false;
volatile uint32_t recordedDataBytes = 0;
uint32_t recordingStartedMs = 0;
uint32_t pauseStartedMs = 0;
uint32_t pausedTotalMs = 0;
uint32_t lastRenderedSecond = UINT32_MAX;
char statusText[48] = "READY";

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

bool inRect(int16_t x, int16_t y, int left, int top, int width, int height) {
    return x >= left && x < left + width && y >= top && y < top + height;
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
    writeLe32(file, 16); writeLe16(file, 1); writeLe16(file, CHANNELS);
    writeLe32(file, SAMPLE_RATE); writeLe32(file, BYTE_RATE);
    writeLe16(file, CHANNELS * BITS_PER_SAMPLE / 8); writeLe16(file, BITS_PER_SAMPLE);
    file.write(reinterpret_cast<const uint8_t *>("data"), 4);
    writeLe32(file, dataBytes);
}

bool ensureFolders(int8_t tag) {
    if (!SdCard::isMounted() || tag < 0 || tag >= TAG_COUNT) return false;
    if (!SD_MMC.exists(ROOT_FOLDER) && !SD_MMC.mkdir(ROOT_FOLDER)) return false;
    char folder[48] = {};
    snprintf(folder, sizeof(folder), "%s/%s", ROOT_FOLDER, TAG_FOLDERS[tag]);
    return SD_MMC.exists(folder) || SD_MMC.mkdir(folder);
}

uint16_t nextFileNumber(int8_t tag) {
    char folder[48] = {};
    snprintf(folder, sizeof(folder), "%s/%s", ROOT_FOLDER, TAG_FOLDERS[tag]);
    File root = SD_MMC.open(folder);
    uint16_t maximum = 0;
    if (root && root.isDirectory()) {
        File entry = root.openNextFile();
        while (entry) {
            const String path(entry.name());
            const int slash = path.lastIndexOf('/');
            const String name = path.substring(slash + 1);
            if (!entry.isDirectory() && name.startsWith("REC_") && name.endsWith(".wav")) {
                maximum = max<uint16_t>(maximum, static_cast<uint16_t>(name.substring(4, 8).toInt()));
            }
            entry.close();
            entry = root.openNextFile();
        }
    }
    if (root) root.close();
    return maximum + 1;
}

uint32_t elapsedSeconds() {
    if (!recordingActive && recordingStartedMs == 0) return 0;
    const uint32_t now = recordingPaused ? pauseStartedMs : millis();
    return (now - recordingStartedMs - pausedTotalMs) / 1000;
}

void recordingTask(void *) {
    int16_t samples[480] = {};
    while (!recordingStopRequested) {
        const size_t count = codec ? codec->read(samples, 480, 100) : 0;
        if (count == 0) continue;
        if (!recordingPaused && recordingFile) {
            recordedDataBytes += recordingFile.write(
                reinterpret_cast<const uint8_t *>(samples), count * sizeof(int16_t));
        }
    }
    if (recordingFile) {
        writeWavHeader(recordingFile, recordedDataBytes);
        recordingFile.flush();
        recordingFile.close();
    }
    recordingActive = false;
    recordingPaused = false;
    recordingTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

bool startRecording() {
    if (recordingActive || !codec || !codec->isInitialized() || !ensureFolders(selectedTag)) {
        std::strcpy(statusText, "RECORDING UNAVAILABLE");
        return false;
    }
    snprintf(activePath, sizeof(activePath), "%s/%s/REC_%04u.wav",
             ROOT_FOLDER, TAG_FOLDERS[selectedTag], nextFileNumber(selectedTag));
    recordingFile = SD_MMC.open(activePath, FILE_WRITE);
    if (!recordingFile) { std::strcpy(statusText, "FILE OPEN FAILED"); return false; }
    writeWavHeader(recordingFile, 0);
    recordedDataBytes = 0;
    recordingStopRequested = false;
    recordingPaused = false;
    recordingStartedMs = millis();
    pauseStartedMs = 0;
    pausedTotalMs = 0;
    recordingActive = true;
    lastRenderedSecond = UINT32_MAX;
    std::strcpy(statusText, "RECORDING");
    codec->setSpeakerEnabled(false);
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
    const uint32_t started = millis();
    while (recordingTaskHandle && millis() - started < 2500) vTaskDelay(pdMS_TO_TICKS(10));
    std::strcpy(statusText, recordingTaskHandle ? "STOP TIMEOUT" : "SAVED");
    Serial.printf("[RECORDING] Stopped path=%s bytes=%lu\n",
                  activePath, static_cast<unsigned long>(recordedDataBytes));
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

void loadFiles(int8_t tag) {
    fileCount = 0;
    if (!ensureFolders(tag)) return;
    char folder[48] = {};
    snprintf(folder, sizeof(folder), "%s/%s", ROOT_FOLDER, TAG_FOLDERS[tag]);
    File root = SD_MMC.open(folder);
    if (!root || !root.isDirectory()) { if (root) root.close(); return; }
    File entry = root.openNextFile();
    while (entry && fileCount < MAX_FILES) {
        const String path(entry.name());
        const int slash = path.lastIndexOf('/');
        const String name = path.substring(slash + 1);
        if (!entry.isDirectory() && name.endsWith(".wav")) {
            std::strncpy(fileNames[fileCount], name.c_str(), sizeof(fileNames[fileCount]) - 1);
            ++fileCount;
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
}

void drawBack(uint8_t *frame) {
    rect(frame, 10, 40, 48, 28);
    line(frame, 40, 47, 25, 54); line(frame, 25, 54, 40, 61);
}

void drawFolder(uint8_t *frame, int x, int y) {
    line(frame, x, y + 6, x + 12, y + 6); line(frame, x + 12, y + 6, x + 16, y + 10);
    rect(frame, x, y + 10, 38, 26);
}

void drawTags(uint8_t *frame, bool browser) {
    UiLocalization::drawCentered(frame, 55, browser ? "CHOOSE TAG FOLDER" : "CHOOSE TAG", 1);
    if (browser) drawBack(frame);
    for (uint8_t i = 0; i < TAG_COUNT; ++i) {
        const int top = TAG_Y + i * (TAG_H + TAG_GAP);
        rect(frame, TAG_X, top, TAG_W, TAG_H);
        UiLocalization::drawCentered(frame, top + 13, TAG_NAMES[i], 1);
    }
}

void drawRecordButton(uint8_t *frame) {
    circle(frame, 120, 354, 39);
    if (recordingActive) rect(frame, 108, 342, 24, 24);
    else circle(frame, 120, 354, 14);
}

void drawRecorder(uint8_t *frame) {
    drawBack(frame);
    UiLocalization::drawCentered(frame, 48, TAG_NAMES[selectedTag], 1);
    const uint32_t seconds = elapsedSeconds();
    char timer[20] = {};
    snprintf(timer, sizeof(timer), "%02lu:%02lu:%02lu",
             static_cast<unsigned long>(seconds / 3600),
             static_cast<unsigned long>((seconds / 60) % 60),
             static_cast<unsigned long>(seconds % 60));
    UiLocalization::drawCentered(frame, 120, timer, 2);
    UiLocalization::drawCentered(frame, 178, statusText, 1);
    rect(frame, 154, 205, 58, 42);
    UiLocalization::drawText(frame, 163, 216, "512", 1);
    UiLocalization::drawText(frame, 164, 230, "kbps", 1);
    line(frame, 12, 278, 228, 278);
    drawFolder(frame, 27, 337);
    drawRecordButton(frame);
    line(frame, 195, 338, 195, 370); line(frame, 208, 338, 208, 370);
}

void drawFiles(uint8_t *frame) {
    drawBack(frame);
    char title[48] = {};
    snprintf(title, sizeof(title), "%s RECORDINGS", TAG_NAMES[selectedTag]);
    UiLocalization::drawCentered(frame, 48, title, 1);
    if (fileCount == 0) { UiLocalization::drawCentered(frame, 190, "NO RECORDINGS", 1); return; }
    for (uint8_t i = 0; i < fileCount; ++i) {
        const int top = 82 + i * 38;
        rect(frame, 18, top, 204, 31);
        UiLocalization::drawText(frame, 30, top + 11, fileNames[i], 1);
    }
}

} // namespace

namespace RecordingPage {

void setAudio(Es8311 *audio) { codec = audio; }

void open() {
    stopRecording();
    view = View::Tags;
    selectedTag = -1;
    recordingStartedMs = 0;
    std::strcpy(statusText, "READY");
}

bool handleTap(int16_t x, int16_t y) {
    if (view == View::Tags || view == View::BrowseTags) {
        if (view == View::BrowseTags && inRect(x, y, 10, 40, 48, 28)) {
            view = View::Recorder; return true;
        }
        for (uint8_t i = 0; i < TAG_COUNT; ++i) {
            const int top = TAG_Y + i * (TAG_H + TAG_GAP);
            if (!inRect(x, y, TAG_X, top, TAG_W, TAG_H)) continue;
            selectedTag = i;
            if (view == View::Tags) {
                view = View::Recorder;
                recordingStartedMs = 0;
                std::strcpy(statusText, "READY");
            } else {
                loadFiles(selectedTag);
                view = View::Files;
            }
            return true;
        }
        return false;
    }
    if (view == View::Files) {
        if (inRect(x, y, 10, 40, 48, 28)) { view = View::BrowseTags; return true; }
        return false;
    }
    if (inRect(x, y, 10, 40, 48, 28)) {
        stopRecording(); view = View::Tags; selectedTag = -1; return true;
    }
    if (inRect(x, y, 12, 320, 68, 75)) {
        stopRecording(); view = View::BrowseTags; return true;
    }
    if (inRect(x, y, 80, 310, 80, 88)) {
        if (recordingActive) stopRecording(); else startRecording();
        return true;
    }
    if (inRect(x, y, 175, 315, 55, 80)) { togglePause(); return true; }
    return false;
}

bool process() {
    if (view != View::Recorder || !recordingActive) return false;
    const uint32_t second = elapsedSeconds();
    if (second == lastRenderedSecond) return false;
    lastRenderedSecond = second;
    return true;
}

bool isRecording() { return recordingActive; }
void stop() { stopRecording(); }

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    switch (view) {
    case View::Tags: drawTags(frame, false); break;
    case View::Recorder: drawRecorder(frame); break;
    case View::BrowseTags: drawTags(frame, true); break;
    case View::Files: drawFiles(frame); break;
    }
}

} // namespace RecordingPage