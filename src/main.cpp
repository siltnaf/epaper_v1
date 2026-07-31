#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvFT6X36.hpp>

#include "board_pins.h"
#include "devices/epd_xingtai/epd_xingtai.h"
#include "pages/asundar/asundar_page.h"
#include "pages/main/main_page.h"

XingtaiEpd epaper;

namespace {

constexpr uint8_t TOUCH_ADDR_ALT = 0x14;
TouchDrvFT6X36 touch;
bool touchOnline = false;
uint32_t lastTouchPoll = 0;
uint32_t lastI2cScan = 0;

uint8_t findTouchAddress() {
    for (const uint8_t candidate : {TOUCH_ADDR_ALT, static_cast<uint8_t>(0x5D), static_cast<uint8_t>(0x38)}) {
        Wire.beginTransmission(candidate);
        if (Wire.endTransmission() == 0) return candidate;
    }
    return 0;
}

bool readTouchPoint(int16_t &x, int16_t &y, uint8_t &count) {
    if (!touchOnline) return false;
    count = touch.getPoint(&x, &y, 1);
    return count != 0;
}

void setupTouchDiagnostic() {
    pinMode(BoardPins::TOUCH_PWR, OUTPUT);
    digitalWrite(BoardPins::TOUCH_PWR, HIGH);
    // Match the working reference firmware: wake the touch controller before
    // starting the shared I2C bus.
    pinMode(BoardPins::TOUCH_INT, OUTPUT);
    digitalWrite(BoardPins::TOUCH_INT, HIGH);
    Wire.begin(BoardPins::I2C_SDA, BoardPins::I2C_SCL);
    Wire.setTimeOut(25);
    const uint8_t touchAddress = findTouchAddress();
    touch.setPins(-1, BoardPins::TOUCH_INT);
    if (touchAddress != 0 && touch.begin(Wire, touchAddress, BoardPins::I2C_SDA, BoardPins::I2C_SCL)) {
        touch.setMaxCoordinates(XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT);
        touch.setSwapXY(true);
        touch.setMirrorXY(false, true);
        touchOnline = true;
    }
    Serial.printf("Touch diagnostic: SDA=%d SCL=%d PWR=%d INT=%d address=0x%02X online=%s\n",
                  BoardPins::I2C_SDA, BoardPins::I2C_SCL, BoardPins::TOUCH_PWR,
                  BoardPins::TOUCH_INT, touchAddress, touchOnline ? "yes" : "no");
}

void printI2cStatus() {
    Serial.printf("I2C scan SDA=%d SCL=%d INT=%d: ",
                  digitalRead(BoardPins::I2C_SDA), digitalRead(BoardPins::I2C_SCL),
                  digitalRead(BoardPins::TOUCH_INT));
    bool found = false;
    for (uint8_t address = 1; address < 0x7F; ++address) {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() == 0) {
            Serial.printf("0x%02X ", address);
            found = true;
        }
    }
    if (!found) Serial.print("none");
    Serial.println();
}

}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("ESP32-S3 3.7-inch e-paper portrait test");
    Serial.printf("EPD DIN=%d CLK=%d CS=%d DC=%d RST=%d BUSY=%d\n",
                  BoardPins::EP_DIN, BoardPins::EP_CLK, BoardPins::EP_CS,
                  BoardPins::EP_DC, BoardPins::EP_RST, BoardPins::EP_BUSY);
    setupTouchDiagnostic();

    static uint8_t frame[XingtaiEpd::FRAME_BYTES];
    AsundarPage::render(frame);

    Serial.println("Initializing e-paper...");
    epaper.begin();
    Serial.println("Writing Asundar splash...");
    epaper.display(frame);

    // Keep the splash-to-home transition direct: do not clear the panel or
    // insert a delay. The controller still needs its mandatory refresh to
    // replace the splash pixels with the home pixels.
    MainPage::render(frame);
    Serial.println("Writing main page...");
    epaper.display(frame);
    epaper.sleep();
    Serial.println("Main page refresh complete");
}

void loop() {
    if (millis() - lastI2cScan >= 2000) {
        lastI2cScan = millis();
        printI2cStatus();
    }
    if (millis() - lastTouchPoll >= 50) {
        lastTouchPoll = millis();
        int16_t x = 0;
        int16_t y = 0;
        uint8_t count = 0;
        if (readTouchPoint(x, y, count) && count > 0) {
            Serial.printf("TOUCH x=%d y=%d points=%u\n", x, y, count);
        }
    }
    delay(5);
}
