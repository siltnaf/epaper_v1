#pragma once

#include <stdint.h>

namespace OfflineContentPage {

enum class Kind : uint8_t { Voice, Music, Poem, Learn };

void open(Kind kind);
bool handleTap(Kind kind, int16_t x, int16_t y);
void render(Kind kind, uint8_t *frame);

}