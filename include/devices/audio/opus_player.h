#pragma once

#include <stdint.h>

class Es8311;

namespace OpusPlayer {
void setAudio(Es8311 *audio);
bool play(const char *path);
void requestStopFromIsr();
bool acceptsTouchStop(uint32_t interruptTick);
bool pause();
bool resume();
bool isPaused();
void stop();
bool loop();
bool isPlaying();
}