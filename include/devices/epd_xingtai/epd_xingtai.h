#pragma once

#include <Arduino.h>
#include <SPI.h>

class XingtaiEpd {
public:
    static constexpr uint16_t WIDTH = 240;
    static constexpr uint16_t HEIGHT = 416;
    static constexpr size_t FRAME_BYTES = WIDTH * HEIGHT / 8;

    explicit XingtaiEpd(SPIClass &spi = SPI);

    void begin();
    void reset();
    void clear(uint8_t value = 0x00);
    void display(const uint8_t *frame);
    void displayPartial(const uint8_t *oldFrame, const uint8_t *newFrame,
                        uint16_t x, uint16_t y, uint16_t width, uint16_t height);
    void drawTestPattern();
    void sleep();

private:
    SPIClass &_spi;

    void command(uint8_t value);
    void data(uint8_t value);
    void data(const uint8_t *buffer, size_t length);
    void waitBusy(uint32_t timeoutMs = 15000);
    void controllerSetup();
    void refresh(const uint8_t *buffer);
};
