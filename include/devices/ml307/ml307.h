#pragma once

#include <Arduino.h>
#include <freertos/semphr.h>

class Ml307 {
public:
    Ml307(HardwareSerial &serial, int rxPin, int txPin, int powerPin,
          uint32_t baud = 115200);

    void begin();
    void setPowered(bool powered);
    bool probeConnection();
    bool httpGet(const char *url, String &payload, uint32_t timeoutMs = 20000);
    bool httpGet(const char *url, Print &output, size_t &bytesWritten,
                 uint32_t timeoutMs = 180000);

    bool isPowered() const;
    bool isConnected() const;

private:
    String readResponse(uint32_t timeoutMs, const char *tokenA, const char *tokenB);
    bool sendCommand(const char *command, uint32_t timeoutMs, String &response);
    bool waitForSimReady();
    bool waitForNetworkRegistration();
    bool ensureTcpConnection();
    bool waitForTcpOpen(uint32_t timeoutMs);
    static bool mipCallIsActive(const String &response);
    static bool mipStateIsConnected(const String &response);
    static bool parseMipOpen(const String &response, int &connectionId, int &errorCode);
    static bool simIsReady(const String &response);
    static int registrationState(const String &response, const char *prefix);
    static const char *registrationStateName(int state);
    static void appendHex(String &out, const char *data, size_t length);
    static int hexValue(char value);
    static bool extractContentHex(const String &line, String &hex);
    bool httpGetInternal(const char *url, String *payload, Print *output,
                         size_t *bytesWritten, uint32_t timeoutMs);

    HardwareSerial &serial_;
    int rxPin_;
    int txPin_;
    int powerPin_;
    uint32_t baud_;
    volatile bool powered_ = false;
    volatile bool connected_ = false;
    bool started_ = false;
    uint32_t poweredAtMs_ = 0;
    SemaphoreHandle_t commandMutex_ = nullptr;
};

extern Ml307 cellularModem;