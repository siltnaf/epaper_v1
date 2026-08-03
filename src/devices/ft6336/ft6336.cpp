#include "devices/ft6336/ft6336.h"

#include <Wire.h>

#include "board_pins.h"
#include "devices/epd_xingtai/epd_xingtai.h"

Ft6336 *Ft6336::_instance = nullptr;

void IRAM_ATTR Ft6336::onInterrupt() {
    if (_instance != nullptr) {
        _instance->_interruptPending = true;
    }
}

void Ft6336::begin() {
    _instance = this;
    pinMode(BoardPins::TOUCH_PWR, OUTPUT);
    digitalWrite(BoardPins::TOUCH_PWR, LOW);
    delay(20);
    digitalWrite(BoardPins::TOUCH_PWR, HIGH);
    delay(200);

    pinMode(BoardPins::TOUCH_INT, OUTPUT);
    digitalWrite(BoardPins::TOUCH_INT, HIGH);
    Wire.begin(BoardPins::I2C_SDA, BoardPins::I2C_SCL);
    Wire.setClock(100000);
    Wire.setTimeOut(25);

    Serial.printf("[TOUCH] power=%d INT=%d SDA=%d SCL=%d\n",
                  digitalRead(BoardPins::TOUCH_PWR), digitalRead(BoardPins::TOUCH_INT),
                  BoardPins::I2C_SDA, BoardPins::I2C_SCL);
    scanBus();
    Wire.beginTransmission(ADDRESS);
    const bool addressOnline = Wire.endTransmission() == 0;
    if (addressOnline) {
        writeRegister(REG_INTERRUPT_MODE, 0x00);
        pinMode(BoardPins::TOUCH_INT, INPUT);
        attachInterrupt(digitalPinToInterrupt(BoardPins::TOUCH_INT), onInterrupt, FALLING);
        _online = true;
    }

    Serial.printf("Touch diagnostic: SDA=%d SCL=%d PWR=%d INT=%d controller=FT6336 address=0x%02X online=%s\n",
                  BoardPins::I2C_SDA, BoardPins::I2C_SCL, BoardPins::TOUCH_PWR,
                  BoardPins::TOUCH_INT, addressOnline ? ADDRESS : 0,
                  _online ? "yes" : "no");
    Serial.println(_online ? "[TOUCH] Ready; touch the panel to print coordinates"
                           : "[TOUCH] Controller unavailable; coordinate reporting disabled");
}

void Ft6336::scanBus() {
    uint8_t found = 0;
    Serial.println("[TOUCH] I2C scan start");
    for (uint8_t address = 1; address < 127; ++address) {
        Wire.beginTransmission(address);
        const uint8_t status = Wire.endTransmission();
        if (status == 0) {
            Serial.printf("[TOUCH] I2C device found: 0x%02X\n", address);
            ++found;
            probeControllerId(address);
        }
        delay(1);
    }
    Serial.printf("[TOUCH] I2C scan complete: %u device(s)\n", found);
}

void Ft6336::probeControllerId(uint8_t address) {
    // FT6x36-family identification registers: vendor, chip, firmware and library.
    constexpr uint8_t registers[] = {0xA3, 0xA6, 0xA8, 0xA1};
    uint8_t values[sizeof(registers)] = {};
    uint8_t successfulReads = 0;
    for (size_t i = 0; i < sizeof(registers); ++i) {
        Wire.beginTransmission(address);
        Wire.write(registers[i]);
        const uint8_t writeStatus = Wire.endTransmission(false);
        if (writeStatus == 0 && Wire.requestFrom(static_cast<int>(address), 1, true) == 1) {
            values[i] = Wire.read();
            ++successfulReads;
        } else {
            values[i] = 0xFF;
        }
    }
    Serial.printf("[TOUCH] ID probe addr=0x%02X reads=%u vendor(0xA3)=0x%02X chip(0xA6)=0x%02X fw(0xA8)=0x%02X lib(0xA1)=0x%02X\n",
                  address, successfulReads, values[0], values[1], values[2], values[3]);
}

bool Ft6336::readRegister(uint8_t reg, uint8_t *buffer, size_t length) {
    uint8_t lastWriteStatus = 0;
    uint8_t lastReceived = 0;
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        Wire.beginTransmission(ADDRESS);
        Wire.write(reg);
        // Keep the register address phase open for the following read.
        // The FT6x36 family expects a repeated-start transaction here;
        // sending STOP first can cause an I2C timeout on this board.
        lastWriteStatus = Wire.endTransmission(false);
        if (lastWriteStatus == 0) {
            lastReceived = Wire.requestFrom(ADDRESS, length, true);
            if (lastReceived == length) {
                for (size_t i = 0; i < length; ++i) buffer[i] = Wire.read();
                return true;
            }
        }
        delay(2);
    }
    Serial.printf("FT6336 read reg 0x%02X failed: write=%u received=%u/%u INT=%d\n",
                  reg, lastWriteStatus, lastReceived, static_cast<unsigned>(length),
                  digitalRead(BoardPins::TOUCH_INT));
    return false;
}

bool Ft6336::writeRegister(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(ADDRESS);
    Wire.write(reg);
    Wire.write(value);
    const uint8_t status = Wire.endTransmission();
    if (status != 0) {
        Serial.printf("FT6336 write reg 0x%02X=0x%02X failed: %u\n", reg, value, status);
        return false;
    }
    return true;
}

uint8_t Ft6336::readPoint(int16_t &rawX, int16_t &rawY, uint8_t &event, uint8_t &id) {
    uint8_t frame[FRAME_LENGTH] = {};
    if (!readRegister(REG_DEVICE_MODE, frame, sizeof(frame))) return 0;
    const uint8_t count = frame[REG_TOUCH_STATUS] & 0x0F;
    if (count == 0 || count == 0x0F) return 0;
    rawX = ((frame[3] & 0x0F) << 8) | frame[4];
    rawY = ((frame[5] & 0x0F) << 8) | frame[6];
    event = (frame[3] >> 6) & 0x03;
    id = (frame[5] >> 4) & 0x0F;
    return count;
}

void Ft6336::update() {
    if (!_online) return;
    if (_interruptPending) {
        noInterrupts();
        _interruptPending = false;
        interrupts();
        _nextPoll = millis();
        _pollUntil = _nextPoll + 300;
    }
    if (millis() >= _pollUntil) return;
    if (digitalRead(BoardPins::TOUCH_INT) != LOW &&
            static_cast<int32_t>(millis() - _nextPoll) < 0) return;

    _nextPoll = millis() + 20;
    int16_t rawX = 0;
    int16_t rawY = 0;
    uint8_t event = 0;
    uint8_t id = 0;
    const uint8_t count = readPoint(rawX, rawY, event, id);
    if (count > 0) {
        const int16_t x = rawY;
        const int16_t y = XingtaiEpd::HEIGHT - 1 - rawX;
        Serial.printf("[TOUCH] coordinate x=%d y=%d raw_x=%d raw_y=%d points=%u event=%u id=%u\n",
                      x, y, rawX, rawY, count, event, id);
    }
}

bool Ft6336::isOnline() const {
    return _online;
}