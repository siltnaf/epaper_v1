#pragma once

#include <stdint.h>

namespace CalculatorPage {

bool handleTouchStart(int16_t x, int16_t y);
bool takePressedKeyBounds(int16_t &x, int16_t &y, int16_t &width, int16_t &height);
void renderPressedKey(uint8_t *frame);
void render(uint8_t *frame);

}