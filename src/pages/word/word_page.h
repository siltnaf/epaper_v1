#pragma once

#include <stdint.h>

namespace WordPage {

void setContentUrl(const char *url);
void setVoice(const char *voice);
void open();
bool startLibraryLoad();
bool takeLibraryLoadCompleted();
bool isDetail();
bool libraryCardBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                         int16_t &width, int16_t &height);
bool handleTap(int16_t x, int16_t y);
bool handleSwipe(int16_t deltaX, int16_t deltaY);
bool takeReplayRefreshRequest();
bool processAnimation();
bool isAnimating();
bool isAudioActive();
void stopAudio();
void stopFromTouchInterrupt();
void render(uint8_t *frame);

}