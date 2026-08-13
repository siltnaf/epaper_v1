#pragma once

#include <stdint.h>

namespace CartoonPage {

void setContentUrl(const char *url);
void open();
bool rowBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                 int16_t &width, int16_t &height);
bool handleTap(int16_t x, int16_t y);
void render(uint8_t *frame);

} // namespace CartoonPage