#pragma once

#include <stdint.h>

class Es8311;

namespace RadioPage {

void setContentUrl(const char *url);
void setAudio(Es8311 *audio);
void open();
bool startLibraryLoad();
bool takeLibraryLoadCompleted();
bool rowBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                 int16_t &width, int16_t &height);
bool handleTap(int16_t x, int16_t y);
bool process();
bool isPlaying();
bool advanceMarquee(int16_t &rowTop);
void renderMarquee(uint8_t *destination, const uint8_t *currentFrame);
void stop();
void render(uint8_t *frame);

} // namespace RadioPage