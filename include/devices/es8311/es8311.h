#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>

class Es8311 {
public:
    // CE pin pulled high on this board selects the 0x19 7-bit I2C address.
    static constexpr uint8_t DEFAULT_ADDRESS = 0x19;
    static constexpr uint32_t DEFAULT_SAMPLE_RATE = 16000;

    struct Pins {
        int mclk;
        int sclk;
        int lrclk;
        int dout;
        int din;
        int chipEnable;
        int paEnable;
        int powerEnable;
    };

    Es8311(TwoWire &wire, const Pins &pins, uint8_t address = DEFAULT_ADDRESS,
           i2s_port_t i2sPort = I2S_NUM_0);
    ~Es8311();

    bool begin(uint32_t sampleRate = DEFAULT_SAMPLE_RATE);
    void end();

    bool isOnline() const;
    bool isInitialized() const;
    uint8_t address() const;
    uint32_t sampleRate() const;

    bool setOutputVolume(uint8_t percent);
    bool setMicrophoneGain(uint8_t gainDb);
    bool prepareRecording(uint8_t gainDb = 30);
    bool preparePlayback();
    void setSpeakerEnabled(bool enabled);
    void setChipEnabled(bool enabled);
    void setPowerEnabled(bool enabled);

    size_t write(const int16_t *samples, size_t sampleCount, uint32_t timeoutMs = 1000);
    size_t read(int16_t *samples, size_t sampleCount, uint32_t timeoutMs = 1000);

    bool readRegister(uint8_t reg, uint8_t &value);

private:
    struct RegisterValue {
        uint8_t reg;
        uint8_t value;
    };

    bool probe(uint8_t address);
    bool configureI2s();
    bool configureCodec();
    bool writeRegister(uint8_t reg, uint8_t value);
    bool updateRegister(uint8_t reg, uint8_t mask, uint8_t value);
    bool writeSequence(const RegisterValue *sequence, size_t count);

    TwoWire &_wire;
    Pins _pins;
    uint8_t _address;
    i2s_port_t _i2sPort;
    uint32_t _sampleRate = 0;
    bool _online = false;
    bool _initialized = false;
    bool _i2sInstalled = false;
};
