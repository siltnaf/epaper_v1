#pragma once

#include <stdint.h>

class Es8311;

namespace RadioPage {

void setContentUrl(const char *url);
void setAudio(Es8311 *audio);
void open();
bool rowBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                 int16_t &width, int16_t &height);
bool handleTap(int16_t x, int16_t y);
bool process();
bool isPlaying();
void stop();
void render(uint8_t *frame);

} // namespace RadioPage