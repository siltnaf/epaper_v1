#include <Arduino.h>

#include "board_pins.h"
#include "epd_xingtai.h"

XingtaiEpd epaper;

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("ESP32-S3 custom e-paper bring-up");
    Serial.printf("EPD DIN=%d CLK=%d CS=%d DC=%d RST=%d BUSY=%d\n",
                  BoardPins::EP_DIN, BoardPins::EP_CLK, BoardPins::EP_CS,
                  BoardPins::EP_DC, BoardPins::EP_RST, BoardPins::EP_BUSY);

    pinMode(BoardPins::BOOT, INPUT_PULLUP);
    pinMode(BoardPins::WAKE, INPUT_PULLUP);

    epaper.begin();
    epaper.clear(0x00);
    delay(1000);
    epaper.drawTestPattern();
    epaper.sleep();
    Serial.println("EPD test complete");
}

void loop() {
    delay(1000);
}
