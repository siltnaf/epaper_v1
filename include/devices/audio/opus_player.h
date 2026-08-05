#pragma once

class Es8311;

namespace OpusPlayer {
void setAudio(Es8311 *audio);
bool play(const char *path);
void requestStopFromIsr();
void stop();
bool loop();
bool isPlaying();
}