#pragma once

#include <Arduino.h>

namespace AsundarPage {

constexpr uint8_t WHITE = 0x00;

// The 3.7-inch Xingtai panel is 240 x 416 pixels in portrait orientation.
void render(uint8_t *frame);

}