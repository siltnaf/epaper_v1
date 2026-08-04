#include "pages/recording/recording_page.h"

#include "devices/epd_xingtai/epd_xingtai.h"

#include <cstring>

namespace RecordingPage {
void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
}
}