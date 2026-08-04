#pragma once

#include <stdint.h>

namespace BookPage {

void setContentUrl(const char *url);
void openLibrary();
bool handleTap(int16_t x, int16_t y);
void render(uint8_t *frame);

}