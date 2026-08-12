#pragma once

#include <stdint.h>

class Es8311;

namespace RecordingPage {
void setAudio(Es8311 *audio);
void open();
bool handleTap(int16_t x, int16_t y);
bool process();
bool isRecording();
void stop();
void render(uint8_t *frame);
}