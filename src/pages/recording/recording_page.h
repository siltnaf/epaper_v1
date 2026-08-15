#pragma once

#include <stdint.h>

class Es8311;

namespace RecordingPage {
using TimerEventHandler = void (*)();

void setAudio(Es8311 *audio);
void setTimerEventHandler(TimerEventHandler handler);
void open();
bool returnControlAt(int16_t x, int16_t y);
bool headerControlAt(int16_t x, int16_t y);
bool folderControlAt(int16_t x, int16_t y);
bool pauseControlAt(int16_t x, int16_t y);
bool tagItemBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                     int16_t &width, int16_t &height);
bool pagerControlBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                          int16_t &width, int16_t &height);
bool handleTap(int16_t x, int16_t y);
bool takeExitRequest();
bool process();
bool takeTimerEvent();
bool advanceMarquee(int16_t &rowTop);
void renderMarquee(uint8_t *destination, const uint8_t *currentFrame);
bool isRecording();
void stop();
void render(uint8_t *frame);
}