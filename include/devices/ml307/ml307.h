#pragma once

#include <Arduino.h>
#include <Client.h>
#include <freertos/semphr.h>

class Ml307;

class Ml307TcpClient final : public Client {
public:
    explicit Ml307TcpClient(Ml307 &modem);

    int connect(IPAddress ip, uint16_t port) override;
    int connect(const char *host, uint16_t port) override;
    size_t write(uint8_t value) override;
    size_t write(const uint8_t *buffer, size_t size) override;
    int available() override;
    int read() override;
    int read(uint8_t *buffer, size_t size) override;
    int peek() override;
    void flush() override;
    void stop() override;
    uint8_t connected() override;
    operator bool() override;

private:
    Ml307 &modem_;
};

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
    friend class Ml307TcpClient;

    String readResponse(uint32_t timeoutMs, const char *tokenA, const char *tokenB);
    bool sendCommand(const char *command, uint32_t timeoutMs, String &response);
    bool openTcpStream(const char *host, uint16_t port);
    size_t writeTcpStream(const uint8_t *buffer, size_t size);
    int availableTcpStream();
    int readTcpStream(uint8_t *buffer, size_t size);
    int peekTcpStream();
    void closeTcpStream();
    bool tcpStreamConnected();
    void pumpTcpStream(uint32_t timeoutMs = 0);
    void resetTcpReceiveBuffer();
    bool appendTcpReceiveByte(uint8_t value);
    bool parseTcpUrcLine(const String &line);
    void releaseTcpStreamLock();
    bool waitForSimReady();
    bool waitForNetworkRegistration();
    bool ensureTcpConnection();
    bool openTcpConnection(const char *host, uint16_t port);
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
    bool tcpStreamOpen_ = false;
    bool tcpStreamLockHeld_ = false;
    char tcpStreamHost_[96] = {};
    uint16_t tcpStreamPort_ = 0;
    // ML307 can deliver a whole encoded TCP URC in one line. Radio responses
    // observed so far are below 8.3 KiB decoded; 12 KiB preserves them while
    // leaving enough contiguous internal RAM for the MP3 decoder workspace.
    static constexpr size_t TCP_RX_BUFFER_SIZE = 12 * 1024;
    uint8_t *tcpRxBuffer_ = nullptr;
    size_t tcpRxRead_ = 0;
    size_t tcpRxWrite_ = 0;
    size_t tcpRxLength_ = 0;
    String tcpPendingLine_;
};

extern Ml307 cellularModem;