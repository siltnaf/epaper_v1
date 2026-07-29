#include "epd_xingtai.h"

#include "board_pins.h"

namespace {
constexpr uint32_t EPD_SPI_HZ = 4000000;
}

XingtaiEpd::XingtaiEpd(SPIClass &spi) : _spi(spi) {}

void XingtaiEpd::begin() {
    pinMode(BoardPins::EP_CS, OUTPUT);
    pinMode(BoardPins::EP_DC, OUTPUT);
    pinMode(BoardPins::EP_RST, OUTPUT);
    pinMode(BoardPins::EP_BUSY, INPUT);

    digitalWrite(BoardPins::EP_CS, HIGH);
    digitalWrite(BoardPins::EP_DC, HIGH);
    digitalWrite(BoardPins::EP_RST, HIGH);

    // MISO is unused by this panel. CS is controlled explicitly because the
    // Xingtai command/data framing toggles DC and CS for every byte.
    _spi.begin(BoardPins::EP_CLK, -1, BoardPins::EP_DIN, BoardPins::EP_CS);
}

void XingtaiEpd::reset() {
    digitalWrite(BoardPins::EP_RST, HIGH);
    delay(300);
    digitalWrite(BoardPins::EP_RST, LOW);
    delay(3);
    digitalWrite(BoardPins::EP_RST, HIGH);
    delay(300);
}

void XingtaiEpd::command(uint8_t value) {
    _spi.beginTransaction(SPISettings(EPD_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(BoardPins::EP_DC, LOW);
    digitalWrite(BoardPins::EP_CS, LOW);
    _spi.transfer(value);
    digitalWrite(BoardPins::EP_CS, HIGH);
    _spi.endTransaction();
}

void XingtaiEpd::data(uint8_t value) {
    _spi.beginTransaction(SPISettings(EPD_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(BoardPins::EP_DC, HIGH);
    digitalWrite(BoardPins::EP_CS, LOW);
    _spi.transfer(value);
    digitalWrite(BoardPins::EP_CS, HIGH);
    _spi.endTransaction();
}

void XingtaiEpd::data(const uint8_t *buffer, size_t length) {
    _spi.beginTransaction(SPISettings(EPD_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(BoardPins::EP_DC, HIGH);
    digitalWrite(BoardPins::EP_CS, LOW);
    _spi.transferBytes(const_cast<uint8_t *>(buffer), nullptr, length);
    digitalWrite(BoardPins::EP_CS, HIGH);
    _spi.endTransaction();
}

void XingtaiEpd::waitBusy(uint32_t timeoutMs) {
    // The reference Xingtai firmware uses a conservative fixed delay. The
    // schematic exposes BUSY, so use it when active and retain a timeout for
    // panel/firmware revisions whose BUSY polarity differs.
    const uint32_t start = millis();
    while (digitalRead(BoardPins::EP_BUSY) == HIGH) {
        if (millis() - start >= timeoutMs) {
            Serial.println("[EPD] BUSY timeout; continuing");
            break;
        }
        delay(10);
    }
}

void XingtaiEpd::controllerSetup() {
    // UC8253 setup taken from paper_xingtai/demo/User/e-Paper/EPD_3in7_new.c.
    command(0x00); // panel setting
    data(0xD7);
    data(0x0E);
    command(0x50); // VCOM/data interval
    data(0x47);
}

void XingtaiEpd::refresh(const uint8_t *buffer) {
    command(0x13); // write new image data
    waitBusy();
    data(buffer, FRAME_BYTES);

    command(0x04); // power on
    waitBusy();
    delay(20);
    command(0x12); // display refresh
    delay(20);
    waitBusy();
    command(0x02); // power off
    waitBusy();
}

void XingtaiEpd::clear(uint8_t value) {
    static uint8_t frame[FRAME_BYTES];
    memset(frame, value, sizeof(frame));
    display(frame);
}

void XingtaiEpd::display(const uint8_t *frame) {
    reset();
    waitBusy();
    controllerSetup();
    refresh(frame);
}

void XingtaiEpd::drawTestPattern() {
    static uint8_t frame[FRAME_BYTES];
    // The Xingtai reference treats 0x00 as white and 0xFF as black.
    memset(frame, 0x00, sizeof(frame));
    for (uint16_t y = 0; y < HEIGHT; ++y) {
        const uint8_t value = y < HEIGHT / 2 ? 0x00 : 0xFF;
        for (uint16_t xByte = 0; xByte < WIDTH / 8; ++xByte) {
            frame[y * (WIDTH / 8) + xByte] = value;
        }
    }
    reset();
    waitBusy();
    controllerSetup();
    refresh(frame);
}

void XingtaiEpd::sleep() {
    command(0x07);
    data(0xA5);
}
