#include "devices/sd_card/sd_card.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <freertos/semphr.h>

#include <cstring>

#include "board_pins.h"
#include "devices/ml307/ml307.h"
#include "ui/loading_indicator.h"

namespace SdCard {

namespace {
SemaphoreHandle_t sdMutex = nullptr;
}

Lock::Lock() {
    if (!sdMutex) sdMutex = xSemaphoreCreateRecursiveMutex();
    acquired_ = sdMutex && xSemaphoreTakeRecursive(sdMutex, portMAX_DELAY) == pdTRUE;
}

Lock::~Lock() {
    if (acquired_ && sdMutex) xSemaphoreGiveRecursive(sdMutex);
}

bool Lock::acquired() const { return acquired_; }

bool isMounted() {
    return SD_MMC.cardType() != CARD_NONE;
}

bool isValidOggOpus(const char *path, uint32_t minimumBytes) {
    Lock lock;
    if (!lock.acquired()) return false;
    if (!path || !isMounted() || !SD_MMC.exists(path)) return false;
    File file = SD_MMC.open(path, FILE_READ);
    uint8_t header[128] = {};
    const size_t bytes = file ? file.read(header, sizeof(header)) : 0;
    const uint32_t size = file ? static_cast<uint32_t>(file.size()) : 0;
    if (file) file.close();

    // Validate the first complete Ogg BOS page, not merely the leading "OggS".
    // The old manual HTTP copy could drop bytes 14..22 while preserving OggS
    // and OpusHead, allowing a structurally corrupt cache to pass a 4-byte test.
    if (size < minimumBytes || bytes < 47 || std::memcmp(header, "OggS", 4) != 0 ||
        header[4] != 0 || (header[5] & 0x02) == 0) {
        return false;
    }
    const uint8_t segmentCount = header[26];
    if (segmentCount == 0 || static_cast<size_t>(27 + segmentCount + 8) > bytes) return false;
    const size_t packetOffset = 27 + segmentCount;
    return std::memcmp(header + packetOffset, "OpusHead", 8) == 0;
}

bool downloadFile(const char *url, const char *path, uint32_t minimumBytes,
                  const char *bearerToken) {
    Lock lock;
    if (!lock.acquired()) return false;
    if (!url || !path || !isMounted() ||
        (WiFi.status() != WL_CONNECTED && !cellularModem.isConnected())) return false;
    UiLoadingIndicator::Scope loadingIndicator;

    const String destination(path);
    const int slash = destination.lastIndexOf('/');
    if (slash > 0) {
        const String directory = destination.substring(0, slash);
        if (!SD_MMC.exists(directory) && !SD_MMC.mkdir(directory)) {
            Serial.printf("[SD] Could not create directory %s\n", directory.c_str());
            return false;
        }
    }

    const String temporary = destination + ".part";
    SD_MMC.remove(temporary);
    File output = SD_MMC.open(temporary, FILE_WRITE);
    if (!output) {
        Serial.printf("[SD] Could not open %s for writing\n", temporary.c_str());
        return false;
    }

    if (WiFi.status() != WL_CONNECTED) {
        constexpr uint8_t MAX_4G_ATTEMPTS = 2;
        size_t written = 0;
        bool downloaded = false;
        for (uint8_t attempt = 1; attempt <= MAX_4G_ATTEMPTS; ++attempt) {
            if (attempt > 1) {
                output.close();
                SD_MMC.remove(temporary);
                output = SD_MMC.open(temporary, FILE_WRITE);
                if (!output) break;
            }
            written = 0;
            Serial.printf("[SD] Downloading through 4G attempt=%u/%u: %s\n",
                          attempt, MAX_4G_ATTEMPTS, url);
            downloaded = cellularModem.httpGet(url, output, written);
            output.flush();
            if (downloaded && written >= minimumBytes) break;
            Serial.printf("[SD] 4G attempt failed bytes=%u\n",
                          static_cast<unsigned>(written));
        }
        output.close();
        if (!downloaded || written < minimumBytes) {
            Serial.printf("[SD] 4G download failed bytes=%u minimum=%lu\n",
                          static_cast<unsigned>(written),
                          static_cast<unsigned long>(minimumBytes));
            SD_MMC.remove(temporary);
            return false;
        }
        SD_MMC.remove(destination);
        if (!SD_MMC.rename(temporary, destination)) {
            SD_MMC.remove(temporary);
            return false;
        }
        Serial.printf("[SD] 4G download complete: %u bytes at %s\n",
                      static_cast<unsigned>(written), destination.c_str());
        return true;
    }

    constexpr uint8_t MAX_WIFI_ATTEMPTS = 3;
    uint32_t actualBytes = 0;
    bool downloadedSuccessfully = false;
    for (uint8_t attempt = 1; attempt <= MAX_WIFI_ATTEMPTS; ++attempt) {
        if (attempt > 1) {
            output.close();
            SD_MMC.remove(temporary);
            output = SD_MMC.open(temporary, FILE_WRITE);
            if (!output) break;
            delay(350);
        }

        HTTPClient http;
        http.setConnectTimeout(10000);
        // HTTPClient stores this as uint16_t; use the valid maximum for
        // individual socket reads and recreate the connection on each retry.
        http.setTimeout(65000);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.setReuse(false);
        http.setUserAgent("ESP32-ePaper-Asset/1.0");
        WiFiClient plainClient;
        WiFiClientSecure secureClient;
        secureClient.setInsecure();
        const bool began = String(url).startsWith("https://")
            ? http.begin(secureClient, url) : http.begin(plainClient, url);
        if (!began) {
            output.close();
            SD_MMC.remove(temporary);
            Serial.printf("[SD] Invalid download URL %s\n", url);
            return false;
        }
        if (bearerToken && bearerToken[0]) {
            String authorization = "Bearer ";
            authorization += bearerToken;
            http.addHeader("Authorization", authorization);
        }

        Serial.printf("[SD] Downloading attempt=%u/%u %s to %s\n",
                      attempt, MAX_WIFI_ATTEMPTS, url, destination.c_str());
        const uint32_t transferStartedMs = millis();
        const int responseCode = http.GET();
        int written = -1;
        int expectedBytes = -1;
        bool transferComplete = false;
        if (responseCode >= 200 && responseCode < 300) {
            expectedBytes = http.getSize();
            // HTTPClient owns the socket receive loop. Its stream copier handles
            // partial Wi-Fi packets correctly.
            written = http.writeToStream(&output);
            transferComplete = written >= static_cast<int>(minimumBytes) &&
                               (expectedBytes < 0 || written == expectedBytes);
        }
        output.flush();
        output.close();
        http.end();

        File downloaded = SD_MMC.open(temporary, FILE_READ);
        actualBytes = downloaded ? static_cast<uint32_t>(downloaded.size()) : 0;
        if (downloaded) downloaded.close();
        downloadedSuccessfully = responseCode >= 200 && responseCode < 300 &&
                                 transferComplete && actualBytes >= minimumBytes;
        if (downloadedSuccessfully) break;

        Serial.printf("[SD] Download attempt=%u/%u failed http=%d written=%d expected=%d "
                      "size=%lu elapsed=%lums\n",
                      attempt, MAX_WIFI_ATTEMPTS, responseCode, written, expectedBytes,
                      static_cast<unsigned long>(actualBytes),
                      static_cast<unsigned long>(millis() - transferStartedMs));
    }
    if (!downloadedSuccessfully) {
        SD_MMC.remove(temporary);
        return false;
    }

    SD_MMC.remove(destination);
    if (!SD_MMC.rename(temporary, destination)) {
        Serial.printf("[SD] Could not rename %s to %s\n", temporary.c_str(), destination.c_str());
        SD_MMC.remove(temporary);
        return false;
    }
    Serial.printf("[SD] Download complete: %lu bytes at %s\n",
                  static_cast<unsigned long>(actualBytes), destination.c_str());
    return true;
}

uint8_t listRoot(char names[][33], bool directories[], uint8_t capacity) {
    if (!isMounted() || names == nullptr || directories == nullptr) return 0;
    File root = SD_MMC.open("/");
    if (!root || !root.isDirectory()) return 0;
    uint8_t count = 0;
    File entry = root.openNextFile();
    while (entry && count < capacity) {
        std::strncpy(names[count], entry.name(), 32);
        names[count][32] = '\0';
        directories[count] = entry.isDirectory();
        ++count;
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
    return count;
}

bool clearDirectory(File directory) {
    File entry = directory.openNextFile();
    while (entry) {
        const String path = entry.name();
        const bool isDirectory = entry.isDirectory();
        entry.close();
        if (isDirectory) {
            File child = SD_MMC.open(path);
            const bool cleared = child && clearDirectory(child);
            if (child) child.close();
            if (!cleared || !SD_MMC.rmdir(path)) return false;
        } else if (!SD_MMC.remove(path)) {
            return false;
        }
        entry = directory.openNextFile();
    }
    return true;
}

bool format() {
    if (!isMounted()) return false;
    File root = SD_MMC.open("/");
    if (!root || !root.isDirectory()) return false;
    const bool cleared = clearDirectory(root);
    root.close();
    return cleared;
}

void printInfo() {
    if (SD_MMC.cardType() == CARD_NONE) {
        Serial.println("[SD] status=not present");
        return;
    }

    const uint64_t cardSizeMb = SD_MMC.cardSize() / (1024ULL * 1024ULL);
    const uint64_t totalMb = SD_MMC.totalBytes() / (1024ULL * 1024ULL);
    const uint64_t usedMb = SD_MMC.usedBytes() / (1024ULL * 1024ULL);
    const uint64_t freeMb = totalMb > usedMb ? totalMb - usedMb : 0;
    Serial.printf("[SD] status=mounted type=%u card_size=%llu MiB filesystem=%llu MiB used=%llu MiB free=%llu MiB\n",
                  static_cast<unsigned>(SD_MMC.cardType()), cardSizeMb,
                  totalMb, usedMb, freeMb);
    Serial.printf("[SD] pins=CLK:%d CMD:%d D0:%d D1:%d D2:%d D3:%d PWR:%d mode=4-bit\n",
                  BoardPins::SD_CLK, BoardPins::SD_CMD, BoardPins::SD_DATA0,
                  BoardPins::SD_DATA1, BoardPins::SD_DATA2, BoardPins::SD_DATA3,
                  BoardPins::SD_PWR);
}

void begin() {
    pinMode(BoardPins::SD_PWR, OUTPUT);
    digitalWrite(BoardPins::SD_PWR, HIGH);
    delay(50);

    SD_MMC.setPins(BoardPins::SD_CLK, BoardPins::SD_CMD,
                   BoardPins::SD_DATA0, BoardPins::SD_DATA1,
                   BoardPins::SD_DATA2, BoardPins::SD_DATA3);

    if (!SD_MMC.begin("/sdcard", false, true)) {
        Serial.println("[BOOT] SD card not detected or mount failed");
        return;
    }

    printInfo();
    Serial.println("[SD] Commands: SD_INFO or FORMAT_FAT32");
}

void processSerialCommand() {
    static String command;
    while (Serial.available() > 0) {
        const char character = static_cast<char>(Serial.read());
        if (character == '\n' || character == '\r') {
            command.trim();
            if (command == "FORMAT_FAT32") {
                Serial.println("[SD] FAT32 formatting is not supported by the Arduino SD_MMC API");
                Serial.println("[SD] Remove the card and format it with a PC or dedicated formatter");
            } else if (command == "SD_INFO") {
                printInfo();
            } else if (command.length() > 0) {
                Serial.printf("[SD] Unknown command: %s\n", command.c_str());
            }
            command = "";
        } else if (command.length() < 32) {
            command += character;
        }
    }
}

}