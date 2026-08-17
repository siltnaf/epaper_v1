#pragma once

#include <stdint.h>

namespace CartoonPage {

enum class RefreshMode : uint8_t { Layout, ListContent, ImageContent };

void setContentUrl(const char *url);
void open();
bool isReader();
bool startLibraryLoad();
bool takeLibraryLoadCompleted();
bool controlBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                     int16_t &width, int16_t &height);
bool rowBoundsAt(int16_t x, int16_t y, int16_t &left, int16_t &top,
                 int16_t &width, int16_t &height);
bool handleTap(int16_t x, int16_t y);
RefreshMode takeRefreshMode();
void render(uint8_t *frame);

} // namespace CartoonPage