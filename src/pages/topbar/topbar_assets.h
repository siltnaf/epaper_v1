#pragma once
#include <stdint.h>
namespace Topbar {
constexpr uint8_t ICON_W = 24;
constexpr uint8_t ICON_H = 24;
void drawHome(uint8_t *frame, int x, int y);
void drawWifi(uint8_t *frame, int x, int y);
void drawBle(uint8_t *frame, int x, int y);
void draw4G(uint8_t *frame, int x, int y);
void drawBattery(uint8_t *frame, int x, int y);
}
