#pragma once

#include <stdint.h>

namespace AiPage {

void setLoading(bool loading);
void drawTopbarStatus(uint8_t *frame);
bool controlAt(int16_t x, int16_t y);
bool openAt(int16_t x, int16_t y);
void close();
bool isOpen();
bool animate();
void stopAnimation();
void render(uint8_t *frame);

} // namespace AiPage