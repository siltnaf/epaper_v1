#pragma once

#include <stdint.h>

namespace CalendarPage {

enum class Action : uint8_t {
    None,
    PreviousYear,
    PreviousMonth,
    NextMonth,
    NextYear,
};

void render(uint8_t *frame);
Action actionAt(int16_t x, int16_t y);
void navigate(Action action);

}