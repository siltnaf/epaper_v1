#pragma once

#include <stdint.h>

namespace BookPage {

void setContentUrl(const char *url);
void openLibrary();
bool handleTap(int16_t x, int16_t y);
bool handleSwipe(int16_t deltaX, int16_t deltaY);
void processPendingSave();
void render(uint8_t *frame);

}