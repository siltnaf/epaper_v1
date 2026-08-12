#pragma once

#include <stdint.h>

class Es8311;

namespace PoemPage {
void setContentUrl(const char *url);
void setVoice(const char *voice);
void setAudio(Es8311 *audio);
void openLibrary();
bool startLibraryLoad();
bool takeLibraryLoadCompleted();
bool returnControlAt(int16_t x, int16_t y);
bool replayControlAt(int16_t x, int16_t y);
bool handleTap(int16_t x, int16_t y);
bool handleSwipe(int16_t deltaX, int16_t deltaY);
bool isPopupOpen();
bool takePlaybackIconRefreshRequest();
void processPendingSave();
bool processAudio();
bool isAudioActive();
void stopAudioFromTouchInterrupt();
bool takeDirtyRows(int8_t &firstRow, int8_t &secondRow);
bool advanceMarquee(int16_t &rowTop);
void renderMarquee(uint8_t *destination, const uint8_t *currentFrame);
void stopAudio();
void render(uint8_t *frame);
}
