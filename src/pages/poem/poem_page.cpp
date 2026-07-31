#include "pages/poem/poem_page.h"

#include "devices/epd_xingtai/epd_xingtai.h"

#include <cstring>

namespace PoemPage {
void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
}
}