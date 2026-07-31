#include "pages/settings/settings_page.h"
#include "pages/settings/settings_assets.h"
#include "devices/epd_xingtai/epd_xingtai.h"
#include <cstring>
namespace SettingsPage { void render(uint8_t *frame) { std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES); } }