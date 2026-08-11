#pragma once

#include <stdint.h>

namespace BookPage {

enum class ReaderControl : uint8_t { None, Back, Previous, Next };

void setContentUrl(const char *url);
void openLibrary();
bool startLibraryLoad();
bool takeLibraryLoadCompleted();
bool isReader();
bool handleTap(int16_t x, int16_t y);
bool handleSwipe(int16_t deltaX, int16_t deltaY);
ReaderControl takeReaderControlPress();
bool processPendingReaderBack();
void renderReaderControlPressed(uint8_t *frame, ReaderControl control);
bool takeReaderContentRefreshRequest();
bool takeLibraryContentRefreshRequest();
bool pendingBookOpenRow(int16_t &top);
bool pendingBookOpenIsLocal();
bool preparePendingBookOpen();
bool processPendingBookOpen();
void processPendingSave();
void render(uint8_t *frame);

}