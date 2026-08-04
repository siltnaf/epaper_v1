#pragma once
#include <stdint.h>

namespace ClockPage {

void setWeather(const char *location, const char *condition, const char *temperature,
                bool available);
void render(uint8_t *frame);

}