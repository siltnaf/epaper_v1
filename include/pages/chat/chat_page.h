#pragma once

#include <stdint.h>

namespace ChatPage {
void setContentUrl(const char *url);
void open();
bool returnControlAt(int16_t x, int16_t y);
bool recordControlAt(int16_t x, int16_t y);
bool refreshControlAt(int16_t x, int16_t y);
bool handleTap(int16_t x, int16_t y);
bool process();
bool takeExitRequest();
void render(uint8_t *frame);
}