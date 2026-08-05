#include "pages/learn/learn_page.h"

#include "pages/offline/offline_content_page.h"

namespace LearnPage {
void open() { OfflineContentPage::open(OfflineContentPage::Kind::Learn); }
bool handleTap(int16_t x, int16_t y) { return OfflineContentPage::handleTap(OfflineContentPage::Kind::Learn, x, y); }
void render(uint8_t *frame) { OfflineContentPage::render(OfflineContentPage::Kind::Learn, frame); }
}