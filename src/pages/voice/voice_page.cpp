#include "pages/voice/voice_page.h"

#include "devices/epd_xingtai/epd_xingtai.h"

#include <cstring>

namespace VoicePage {
void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
}
}