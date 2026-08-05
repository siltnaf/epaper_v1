#pragma once
#include <stdint.h>

namespace ClockPage {

void setWeather(const char *location, const char *condition, const char *temperature,
                const char *humidity, const char *windSpeed, bool available);
void render(uint8_t *frame);

}