#pragma once

#include <stdint.h>

namespace ChatPage {
void setContentUrl(const char *url);
void open();
void close();
void service();
bool sendReply(const char *text);
const char *roomName();
const char *deviceMacAddress();
const char *pairingUrl();
bool pairingControlAt(int16_t x, int16_t y);
bool pairingReturnControlAt(int16_t x, int16_t y);
bool recordControlAt(int16_t x, int16_t y);
bool messageControlBoundsAt(int16_t x, int16_t y, int16_t &left,
                            int16_t &top, int16_t &width, int16_t &height);
bool handleTap(int16_t x, int16_t y);
bool process();
void render(uint8_t *frame);
}