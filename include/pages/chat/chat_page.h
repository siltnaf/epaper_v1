#pragma once

#include <stdint.h>

namespace ChatPage {
void setContentUrl(const char *url);
void open();
void service();
bool sendReply(const char *text);
bool authenticateFromSettings(bool registerDevice);
bool isAuthenticated();
const char *roomName();
const char *deviceMacAddress();
const char *pairingUrl();
const char *authStatus();
bool recordControlAt(int16_t x, int16_t y);
bool messageControlBoundsAt(int16_t x, int16_t y, int16_t &left,
                            int16_t &top, int16_t &width, int16_t &height);
bool handleTap(int16_t x, int16_t y);
bool process();
void render(uint8_t *frame);
}