#include "pages/calculator/calculator_page.h"

#include "devices/epd_xingtai/epd_xingtai.h"

#include <cstring>

namespace CalculatorPage {
void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
}
}