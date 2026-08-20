#pragma once

#include <stdint.h>

namespace GamePage {
void setContentUrl(const char *url);
void open();
bool returnControlAt(int16_t x, int16_t y);
bool handleTap(int16_t x, int16_t y);
bool takeExitRequest();
void render(uint8_t *frame);
void drawTopbar(uint8_t *frame);
}