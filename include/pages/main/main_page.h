#pragma once

#include <stdint.h>

namespace MainPage {

enum class FunctionIcon : uint8_t {
    None,
    Settings,
    Calendar,
    Calculator,
    Clock,
    Book,
    Voice,
    Music,
    Poem,
    Word,
    Recording,
    Cartoon,
    Radio,
};

enum class NetworkMode : uint8_t {
    None,
    Wifi,
    Cellular4G,
};

void render(uint8_t *frame);
void render(uint8_t *frame, FunctionIcon selectedIcon);
void setNetworkMode(NetworkMode mode);

}