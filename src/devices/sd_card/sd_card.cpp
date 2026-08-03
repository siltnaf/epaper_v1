#include "devices/sd_card/sd_card.h"

#include <Arduino.h>
#include <SD_MMC.h>

#include "board_pins.h"

namespace SdCard {

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