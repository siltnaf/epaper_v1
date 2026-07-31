#include "pages/music/music_page.h"
#include "pages/music/music_assets.h"
#include "devices/epd_xingtai/epd_xingtai.h"
#include <cstring>
namespace MusicPage { void render(uint8_t *frame) { std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES); } }