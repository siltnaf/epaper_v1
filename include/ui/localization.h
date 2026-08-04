#pragma once
#include <Arduino.h>

namespace UiLocalization {
enum class Language : uint8_t { English = 0, Chinese = 1 };
void setLanguage(uint8_t language);
bool isChinese();
int textWidth(const char *utf8, int scale = 1);
void drawText(uint8_t *frame, int x, int y, const char *utf8, int scale = 1);
void drawCentered(uint8_t *frame, int y, const char *utf8, int scale = 1);
}
