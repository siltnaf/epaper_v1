#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"
#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/ft6336/ft6336.h"
#include "devices/sd_card/sd_card.h"
#include "pages/asundar/asundar_page.h"
#include "pages/main/main_page.h"

XingtaiEpd epaper;
Ft6336 touch;

namespace {

}

void setup() {
    Serial.begin(115200);
    // Give the native USB CDC serial monitor time to attach after reset.
    // The timeout keeps the device booting even when no monitor is connected.
    const uint32_t serialWaitStart = millis();
    while (!Serial && millis() - serialWaitStart < 5000) {
        delay(10);
    }
    delay(100);
    Serial.println();
    Serial.println("========================================");
    Serial.println("[BOOT] ESP32-S3 e-paper startup");
    Serial.printf("[BOOT] Serial ready: %lu baud\n", 115200UL);
    Serial.println("[BOOT] Firmware: 3.7-inch e-paper portrait test");
    Serial.printf("[BOOT] EPD pins: DIN=%d CLK=%d CS=%d DC=%d RST=%d BUSY=%d\n",
                  BoardPins::EP_DIN, BoardPins::EP_CLK, BoardPins::EP_CS,
                  BoardPins::EP_DC, BoardPins::EP_RST, BoardPins::EP_BUSY);
    Serial.println("[BOOT] Checking SD card...");
    SdCard::begin();
    Serial.println("[BOOT] Initializing touch controller...");
    touch.begin();
    Serial.println("[BOOT] Touch controller initialization complete");

    static uint8_t frame[XingtaiEpd::FRAME_BYTES];
    Serial.println("[BOOT] Rendering Asundar splash frame...");
    AsundarPage::render(frame);

    Serial.println("[BOOT] Initializing e-paper controller...");
    epaper.begin();
    Serial.println("[BOOT] Refreshing Asundar splash...");
    epaper.display(frame);
    Serial.println("[BOOT] Asundar splash refresh complete");

    // Keep the splash-to-home transition direct: do not clear the panel or
    // insert a delay. The controller still needs its mandatory refresh to
    // replace the splash pixels with the home pixels.
    Serial.println("[BOOT] Rendering main page frame...");
    MainPage::render(frame);
    Serial.println("[BOOT] Refreshing main page...");
    epaper.display(frame);
    Serial.println("[BOOT] Putting e-paper controller to sleep...");
    epaper.sleep();
    Serial.println("[BOOT] Main page refresh complete");
    Serial.println("[BOOT] Startup sequence complete; entering touch monitoring");
    Serial.println("========================================");
}

void loop() {
    SdCard::processSerialCommand();
    touch.update();
    delay(5);
}
