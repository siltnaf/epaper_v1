#pragma once

#include <stdint.h>

namespace SdCard {

void begin();
bool isMounted();
bool downloadFile(const char *url, const char *path, uint32_t minimumBytes,
                  const char *bearerToken = nullptr);
bool isValidOggOpus(const char *path, uint32_t minimumBytes = 1024);
uint8_t listRoot(char names[][33], bool directories[], uint8_t capacity);
bool format();
void printInfo();
void processSerialCommand();

}