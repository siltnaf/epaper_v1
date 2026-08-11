#include "ui/loading_indicator.h"

#include <Arduino.h>

namespace {

UiLoadingIndicator::Handler indicatorHandler = nullptr;
uint8_t indicatorDepth = 0;
bool indicatorDisplayed = false;
bool indicatorDirty = false;
TaskHandle_t indicatorUiTask = nullptr;
portMUX_TYPE indicatorMux = portMUX_INITIALIZER_UNLOCKED;

void applyIndicatorIfUiTask() {
    if (!indicatorHandler || xTaskGetCurrentTaskHandle() != indicatorUiTask) return;
    bool desired = false;
    bool update = false;
    portENTER_CRITICAL(&indicatorMux);
    desired = indicatorDepth > 0;
    update = indicatorDirty || desired != indicatorDisplayed;
    if (update) {
        indicatorDisplayed = desired;
        indicatorDirty = false;
    }
    portEXIT_CRITICAL(&indicatorMux);
    if (update) indicatorHandler(desired);
}

}

namespace UiLoadingIndicator {

void setHandler(Handler handler) {
    indicatorHandler = handler;
    indicatorUiTask = xTaskGetCurrentTaskHandle();
}

void show() {
    portENTER_CRITICAL(&indicatorMux);
    if (indicatorDepth < 0xFF) {
        if (indicatorDepth == 0) indicatorDirty = true;
        ++indicatorDepth;
    }
    portEXIT_CRITICAL(&indicatorMux);
    applyIndicatorIfUiTask();
}

void hide() {
    portENTER_CRITICAL(&indicatorMux);
    if (indicatorDepth > 0) {
        --indicatorDepth;
        if (indicatorDepth == 0) indicatorDirty = true;
    }
    portEXIT_CRITICAL(&indicatorMux);
    applyIndicatorIfUiTask();
}

void service() {
    applyIndicatorIfUiTask();
}

}