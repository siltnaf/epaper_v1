#include "devices/epd_xingtai/epd_xingtai.h"

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
    // The UC8253 vendor driver treats BUSY HIGH as ready. In particular, do not
    // reset the controller for the next frame while the previous waveform is
    // still active; doing so can leave the panel settled with inverted polarity.
    const uint32_t started = millis();
    while (digitalRead(BoardPins::EP_BUSY) != HIGH && millis() - started < timeoutMs) {
        delay(1);
    }
    delay(20);
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

    // The panel natively mirrors transferred rows horizontally. To present the
    // page framebuffer upside down (180 degrees), send its rows bottom-to-top
    // without the usual horizontal pre-mirror: the panel's native mirror then
    // supplies the remaining horizontal part of the rotation.
    static uint8_t rotated[FRAME_BYTES];
    constexpr size_t ROW_BYTES = WIDTH / 8;
    for (uint16_t y = 0; y < HEIGHT; ++y) {
        const size_t destinationRow = static_cast<size_t>(y) * ROW_BYTES;
        const size_t sourceRow = static_cast<size_t>(HEIGHT - 1 - y) * ROW_BYTES;
        memcpy(rotated + destinationRow, buffer + sourceRow, ROW_BYTES);
    }
    data(rotated, FRAME_BYTES);

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

void XingtaiEpd::displayPartial(const uint8_t *oldFrame, const uint8_t *newFrame,
                               uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    if (oldFrame == nullptr || newFrame == nullptr || width == 0 || height == 0 ||
        x >= WIDTH || y >= HEIGHT) {
        return;
    }

    const uint16_t logicalXStart = x & ~0x07U;
    const uint16_t logicalXEnd = min<uint16_t>(WIDTH - 1, (x + width - 1) | 0x07U);
    const uint16_t logicalYEnd = min<uint16_t>(HEIGHT - 1, y + height - 1);
    // Partial-window coordinates address the physical X axis directly. Pixel
    // data still follows the panel's native scan order; mirroring this window
    // itself incorrectly moves right-side updates beside the Home icon.
    const uint16_t controllerXStart = logicalXStart;
    const uint16_t controllerXEnd = logicalXEnd;
    const uint16_t controllerYStart = HEIGHT - 1 - logicalYEnd;
    const uint16_t controllerYEnd = HEIGHT - 1 - y;
    const size_t rowBytes = WIDTH / 8;
    const size_t windowBytes = (logicalXEnd - logicalXStart + 1) / 8;

    reset();
    waitBusy();
    controllerSetup();

    // The UC8253 partial waveform needs both the pixels currently on-screen
    // and their replacements. A reset discards the controller's previous RAM,
    // so seed both planes with the current full image before replacing only the
    // selected window. The visible refresh below is still restricted to the tile.
    command(0x50);
    data(0xC7);

    command(0x10);
    waitBusy();
    for (int logicalY = HEIGHT - 1; logicalY >= 0; --logicalY) {
        data(oldFrame + static_cast<size_t>(logicalY) * rowBytes, rowBytes);
    }

    command(0x13);
    waitBusy();
    for (int logicalY = HEIGHT - 1; logicalY >= 0; --logicalY) {
        data(oldFrame + static_cast<size_t>(logicalY) * rowBytes, rowBytes);
    }

    command(0x91); // partial in
    waitBusy();
    command(0x90); // partial window
    waitBusy();
    data(static_cast<uint8_t>(controllerXStart));
    data(static_cast<uint8_t>(controllerXEnd));
    data(static_cast<uint8_t>(controllerYStart >> 8));
    data(static_cast<uint8_t>(controllerYStart));
    data(static_cast<uint8_t>(controllerYEnd >> 8));
    data(static_cast<uint8_t>(controllerYEnd));
    data(0x01);

    command(0x10); // old image data
    waitBusy();
    for (int logicalY = logicalYEnd; logicalY >= static_cast<int>(y); --logicalY) {
        const uint8_t *row = oldFrame + static_cast<size_t>(logicalY) * rowBytes + logicalXStart / 8;
        data(row, windowBytes);
    }

    command(0x13); // new image data
    waitBusy();
    for (int logicalY = logicalYEnd; logicalY >= static_cast<int>(y); --logicalY) {
        const uint8_t *row = newFrame + static_cast<size_t>(logicalY) * rowBytes + logicalXStart / 8;
        data(row, windowBytes);
    }

    command(0xE0);
    data(0x02);
    command(0xE5);
    data(0x65); // vendor partial-refresh waveform selection
    command(0x92); // partial out
    waitBusy();
    command(0x04); // power on
    waitBusy();
    delay(20);
    command(0x12); // refresh only the configured window
    delay(20);
    waitBusy();
    command(0x02); // power off
    waitBusy();
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
