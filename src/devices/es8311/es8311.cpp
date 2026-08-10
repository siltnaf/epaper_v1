#include "devices/es8311/es8311.h"

#include <algorithm>

namespace {

// ES8311 register map used by the codec initialization below.
constexpr uint8_t REG_RESET = 0x00;
constexpr uint8_t REG_CLK_MANAGER_01 = 0x01;
constexpr uint8_t REG_CLK_MANAGER_02 = 0x02;
constexpr uint8_t REG_CLK_MANAGER_03 = 0x03;
constexpr uint8_t REG_CLK_MANAGER_04 = 0x04;
constexpr uint8_t REG_CLK_MANAGER_05 = 0x05;
constexpr uint8_t REG_CLK_MANAGER_06 = 0x06;
constexpr uint8_t REG_CLK_MANAGER_07 = 0x07;
constexpr uint8_t REG_CLK_MANAGER_08 = 0x08;
constexpr uint8_t REG_SDP_IN = 0x09;
constexpr uint8_t REG_SDP_OUT = 0x0A;
constexpr uint8_t REG_SYSTEM_0B = 0x0B;
constexpr uint8_t REG_SYSTEM_0C = 0x0C;
constexpr uint8_t REG_SYSTEM_0D = 0x0D;
constexpr uint8_t REG_SYSTEM_0E = 0x0E;
constexpr uint8_t REG_SYSTEM_0F = 0x0F;
constexpr uint8_t REG_SYSTEM_10 = 0x10;
constexpr uint8_t REG_SYSTEM_11 = 0x11;
constexpr uint8_t REG_SYSTEM_12 = 0x12;
constexpr uint8_t REG_SYSTEM_13 = 0x13;
constexpr uint8_t REG_SYSTEM_14 = 0x14;
constexpr uint8_t REG_ADC_15 = 0x15;
constexpr uint8_t REG_ADC_16 = 0x16;
constexpr uint8_t REG_ADC_17 = 0x17;
constexpr uint8_t REG_ADC_18 = 0x18;
constexpr uint8_t REG_ADC_19 = 0x19;
constexpr uint8_t REG_ADC_1A = 0x1A;
constexpr uint8_t REG_ADC_1B = 0x1B;
constexpr uint8_t REG_ADC_1C = 0x1C;
constexpr uint8_t REG_DAC_31 = 0x31;
constexpr uint8_t REG_DAC_VOLUME = 0x32;
constexpr uint8_t REG_DAC_33 = 0x33;
constexpr uint8_t REG_DAC_34 = 0x34;
constexpr uint8_t REG_DAC_35 = 0x35;
constexpr uint8_t REG_DAC_37 = 0x37;

constexpr uint32_t MCLK_MULTIPLE = 256;

} // namespace

Es8311::Es8311(TwoWire &wire, const Pins &pins, uint8_t address, i2s_port_t i2sPort)
    : _wire(wire), _pins(pins), _address(address), _i2sPort(i2sPort) {}

Es8311::~Es8311() {
    end();
}

bool Es8311::begin(uint32_t sampleRate) {
    end();
    _sampleRate = sampleRate;

    if (_pins.chipEnable >= 0) {
        pinMode(_pins.chipEnable, OUTPUT);
        // CE high selects the board's intended 7-bit I2C address, 0x19.
        setChipEnabled(true);
        delay(10);
    }
    if (_pins.powerEnable >= 0) {
        pinMode(_pins.powerEnable, OUTPUT);
        setPowerEnabled(true);
        delay(50);
    }
    pinMode(_pins.paEnable, OUTPUT);
    setSpeakerEnabled(false);

    _online = probe(_address);
    if (!_online) return false;
    if (!configureI2s()) return false;
    if (!configureCodec()) {
        end();
        _online = true;
        return false;
    }

    setSpeakerEnabled(true);
    _initialized = true;
    return true;
}

void Es8311::end() {
    if (_pins.paEnable >= 0) setSpeakerEnabled(false);
    if (_i2sInstalled) {
        i2s_zero_dma_buffer(_i2sPort);
        i2s_driver_uninstall(_i2sPort);
        _i2sInstalled = false;
    }
    _initialized = false;
    if (_pins.powerEnable >= 0) setPowerEnabled(false);
}

bool Es8311::isOnline() const { return _online; }
bool Es8311::isInitialized() const { return _initialized; }
uint8_t Es8311::address() const { return _address; }
uint32_t Es8311::sampleRate() const { return _sampleRate; }

bool Es8311::probe(uint8_t address) {
    _wire.beginTransmission(address);
    return _wire.endTransmission() == 0;
}

bool Es8311::configureI2s() {
    const i2s_config_t config = {
        .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = _sampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 6,
        .dma_buf_len = 240,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = static_cast<int>(_sampleRate * MCLK_MULTIPLE),
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan = I2S_BITS_PER_CHAN_16BIT,
    };

    esp_err_t error = i2s_driver_install(_i2sPort, &config, 0, nullptr);
    if (error != ESP_OK) return false;
    _i2sInstalled = true;

    const i2s_pin_config_t pins = {
        .mck_io_num = _pins.mclk,
        .bck_io_num = _pins.sclk,
        .ws_io_num = _pins.lrclk,
        .data_out_num = _pins.dout,
        .data_in_num = _pins.din,
    };
    error = i2s_set_pin(_i2sPort, &pins);
    if (error != ESP_OK) return false;
    return i2s_zero_dma_buffer(_i2sPort) == ESP_OK;
}

bool Es8311::configureCodec() {
    // Slave-mode, 16-bit standard I2S, MCLK = sample rate * 256. The register
    // sequence below follows the Espressif esp-bsp/esp-adf es8311 driver init
    // (reference project d:/project/epaper_s3/reference6/esp-bsp/components/es8311),
    // the proven full-duplex configuration used on ESP32-S3 boards. In
    // particular 0x12=0x00 powers up the DAC and 0x13=0x10 enables the output
    // to the headphone/line driver; without these the codec stays silent.
    if (!writeRegister(REG_RESET, 0x1F)) return false; // reset digital/csm/clk mgr
    delay(20);
    if (!writeRegister(REG_RESET, 0x00)) return false;
    delay(10);
    if (!writeRegister(REG_RESET, 0x80)) return false; // power-on command

    const RegisterValue sequence[] = {
        {REG_CLK_MANAGER_01, 0x3F}, // enable all codec clocks
        {REG_CLK_MANAGER_02, 0x00}, // MCLK from MCLK pin, 256*fs, pre-divider 1
        {REG_CLK_MANAGER_03, 0x10}, // ADC single-speed mode + OSR
        // Espressif's proven 4.096 MHz MCLK / 16 kHz coefficient uses DAC OSR
        // 0x20 (epaper_test/components/esp_codec_dev/device/es8311/es8311.c).
        {REG_CLK_MANAGER_04, 0x20},
        {REG_CLK_MANAGER_05, 0x00}, // ADC/DAC clock divider
        {REG_CLK_MANAGER_06, 0x03}, // BCLK divider (4.096 MHz -> 512 kHz @16k)
        {REG_CLK_MANAGER_07, 0x00}, // LRCK divider high
        {REG_CLK_MANAGER_08, 0xFF}, // LRCK divider low
        {REG_SDP_IN, 0x0C},         // I2S format, 16-bit
        {REG_SDP_OUT, 0x0C},
        {REG_SYSTEM_0B, 0x00},
        {REG_SYSTEM_0C, 0x00},
        {REG_SYSTEM_0D, 0x01},      // power up analog circuitry
        {REG_SYSTEM_0E, 0x02},      // enable analog PGA + ADC modulator
        {REG_SYSTEM_0F, 0x00},
        {REG_SYSTEM_10, 0x1F},
        {REG_SYSTEM_11, 0x7F},
        {REG_SYSTEM_12, 0x00},      // power up DAC
        {REG_SYSTEM_13, 0x10},      // enable output to HP/line driver
        {REG_SYSTEM_14, 0x1A},      // analog MIC, max PGA gain
        {REG_ADC_15, 0x40},
        {REG_ADC_16, 0x1F},
        {REG_ADC_17, 0xBF},         // ADC volume
        {REG_ADC_18, 0x03},
        {REG_ADC_19, 0x00},
        {REG_ADC_1A, 0x33},
        {REG_ADC_1B, 0x0A},
        {REG_ADC_1C, 0x6A},         // ADC EQ bypass, cancel DC offset in digital domain
        {REG_DAC_31, 0x00},         // DAC unmute
        {REG_DAC_VOLUME, 0xBF},     // DAC volume (overridden by setOutputVolume)
        {REG_DAC_33, 0x10},
        {REG_DAC_34, 0x10},
        {REG_DAC_35, 0x00},
        {REG_DAC_37, 0x08},         // bypass DAC equalizer
    };
    return writeSequence(sequence, sizeof(sequence) / sizeof(sequence[0]));
}

bool Es8311::setOutputVolume(uint8_t percent) {
    percent = std::min<uint8_t>(percent, 100);
    // ES8311 volume uses 0.5 dB steps; 0x00 is mute and 0xFF is maximum.
    const uint8_t value = percent == 0 ? 0 : static_cast<uint8_t>(percent * 255U / 100U);
    return writeRegister(REG_DAC_VOLUME, value);
}

bool Es8311::setMicrophoneGain(uint8_t gainDb) {
    // PGA supports 0..42 dB in 6 dB steps in the low nibble.
    const uint8_t step = std::min<uint8_t>(gainDb / 6, 7);
    return updateRegister(REG_SYSTEM_14, 0x0F, step);
}

void Es8311::setSpeakerEnabled(bool enabled) {
    if (_pins.paEnable < 0) return;
    digitalWrite(_pins.paEnable, enabled ? HIGH : LOW);
}

void Es8311::setChipEnabled(bool enabled) {
    if (_pins.chipEnable < 0) return;
    digitalWrite(_pins.chipEnable, enabled ? HIGH : LOW);
}

void Es8311::setPowerEnabled(bool enabled) {
    if (_pins.powerEnable < 0) return;
    digitalWrite(_pins.powerEnable, enabled ? HIGH : LOW);
}

size_t Es8311::write(const int16_t *samples, size_t sampleCount, uint32_t timeoutMs) {
    if (!_initialized || samples == nullptr || sampleCount == 0) return 0;
    size_t bytesWritten = 0;
    const size_t bytes = sampleCount * sizeof(int16_t);
    if (i2s_write(_i2sPort, samples, bytes, &bytesWritten, pdMS_TO_TICKS(timeoutMs)) != ESP_OK) return 0;
    return bytesWritten / sizeof(int16_t);
}

size_t Es8311::read(int16_t *samples, size_t sampleCount, uint32_t timeoutMs) {
    if (!_initialized || samples == nullptr || sampleCount == 0) return 0;
    size_t bytesRead = 0;
    const size_t bytes = sampleCount * sizeof(int16_t);
    if (i2s_read(_i2sPort, samples, bytes, &bytesRead, pdMS_TO_TICKS(timeoutMs)) != ESP_OK) return 0;
    return bytesRead / sizeof(int16_t);
}

bool Es8311::writeRegister(uint8_t reg, uint8_t value) {
    _wire.beginTransmission(_address);
    _wire.write(reg);
    _wire.write(value);
    return _wire.endTransmission() == 0;
}

bool Es8311::readRegister(uint8_t reg, uint8_t &value) {
    _wire.beginTransmission(_address);
    _wire.write(reg);
    if (_wire.endTransmission(false) != 0) return false;
    if (_wire.requestFrom(_address, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) != 1) return false;
    value = _wire.read();
    return true;
}

bool Es8311::updateRegister(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t current = 0;
    if (!readRegister(reg, current)) return false;
    current = static_cast<uint8_t>((current & ~mask) | (value & mask));
    return writeRegister(reg, current);
}

bool Es8311::writeSequence(const RegisterValue *sequence, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!writeRegister(sequence[i].reg, sequence[i].value)) return false;
    }
    return true;
}
