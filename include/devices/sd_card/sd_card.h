#pragma once

#include <stdint.h>

namespace SdCard {

void begin();
bool isMounted();
uint8_t listRoot(char names[][33], bool directories[], uint8_t capacity);
bool format();
void printInfo();
void processSerialCommand();

}