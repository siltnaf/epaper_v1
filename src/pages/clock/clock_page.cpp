#include "pages/clock/clock_page.h"
#include "pages/clock/clock_assets.h"
#include "devices/epd_xingtai/epd_xingtai.h"
#include <cstring>
namespace ClockPage { void render(uint8_t *frame) { std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES); } }