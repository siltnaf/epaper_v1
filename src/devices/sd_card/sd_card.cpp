#include "devices/sd_card/sd_card.h"

#include <Arduino.h>
#include <SD_MMC.h>

#include <cstring>

#include "board_pins.h"

namespace SdCard {

bool isMounted() {
    return SD_MMC.cardType() != CARD_NONE;
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