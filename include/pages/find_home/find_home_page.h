#pragma once

#include <stdint.h>

namespace FindHomePage {

void setContentUrl(const char *url);
void open();
bool resetControlAt(int16_t x, int16_t y);
bool actionControlAt(int16_t x, int16_t y);
bool handleTap(int16_t x, int16_t y);
void render(uint8_t *frame);

}