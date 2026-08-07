#pragma once
#include <stdint.h>
namespace MusicPage {
void setContentUrl(const char *url);
void open();
bool startLibraryLoad();
bool takeLibraryLoadCompleted();
bool handleTap(int16_t x, int16_t y);
bool handleSwipe(int16_t deltaX, int16_t deltaY);
bool processAudio();
bool isAudioActive();
void stopAudioFromTouchInterrupt();
void stopAudio();
bool takeDirtyRows(int8_t &firstRow, int8_t &secondRow);
bool advanceMarquee(int16_t &rowTop);
void renderMarquee(uint8_t *destination, const uint8_t *currentFrame);
void render(uint8_t *frame);
}