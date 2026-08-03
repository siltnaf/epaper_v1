#pragma once

#include <Arduino.h>

class Ft6336 {
public:
    void begin();
    void update();
    bool isOnline() const;

private:
    static constexpr uint8_t ADDRESS = 0x38;
    static constexpr uint8_t FRAME_LENGTH = 7;
    static constexpr uint8_t REG_DEVICE_MODE = 0x00;
    static constexpr uint8_t REG_TOUCH_STATUS = 0x02;
    static constexpr uint8_t REG_INTERRUPT_MODE = 0xA4;

    static void IRAM_ATTR onInterrupt();
    bool readRegister(uint8_t reg, uint8_t *buffer, size_t length);
    bool writeRegister(uint8_t reg, uint8_t value);
    uint8_t readPoint(int16_t &rawX, int16_t &rawY, uint8_t &event, uint8_t &id);
    void scanBus();
    void probeControllerId(uint8_t address);

    static Ft6336 *_instance;
    bool _online = false;
    volatile bool _interruptPending = false;
    uint32_t _nextPoll = 0;
    uint32_t _pollUntil = 0;
};