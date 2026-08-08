#pragma once

#include <Arduino.h>

class Ml307 {
public:
    Ml307(HardwareSerial &serial, int rxPin, int txPin, int powerPin,
          uint32_t baud = 115200);

    void begin();
    void setPowered(bool powered);
    bool probeConnection();
    bool httpGet(const char *url, String &payload, uint32_t timeoutMs = 20000);

    bool isPowered() const;
    bool isConnected() const;

private:
    String readResponse(uint32_t timeoutMs, const char *tokenA, const char *tokenB);
    bool sendCommand(const char *command, uint32_t timeoutMs, String &response);
    static bool mipCallIsActive(const String &response);
    static bool simIsReady(const String &response);
    static void appendHex(String &out, const char *data, size_t length);
    static int hexValue(char value);
    static bool extractContentHex(const String &line, String &hex);

    HardwareSerial &serial_;
    int rxPin_;
    int txPin_;
    int powerPin_;
    uint32_t baud_;
    volatile bool powered_ = false;
    volatile bool connected_ = false;
    bool started_ = false;
};

extern Ml307 cellularModem;