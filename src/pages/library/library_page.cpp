#include "pages/library/library_page.h"
#include "pages/library/library_assets.h"
#include "devices/epd_xingtai/epd_xingtai.h"
#include <cstring>
namespace LibraryPage { void render(uint8_t *frame) { std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES); } }