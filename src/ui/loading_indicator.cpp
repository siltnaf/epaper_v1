#include "ui/loading_indicator.h"

#include <Arduino.h>

namespace {

UiLoadingIndicator::Handler indicatorHandler = nullptr;
uint8_t indicatorDepth = 0;

}

namespace UiLoadingIndicator {

void setHandler(Handler handler) {
    indicatorHandler = handler;
}

void show() {
    if (indicatorDepth < 0xFF) ++indicatorDepth;
    if (indicatorDepth == 1 && indicatorHandler) indicatorHandler(true);
}

void hide() {
    if (indicatorDepth == 0) return;
    --indicatorDepth;
    if (indicatorDepth == 0 && indicatorHandler) indicatorHandler(false);
}

}