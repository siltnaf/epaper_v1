#include "devices/ml307/ml307.h"

namespace {
constexpr char ML307_SERVER[] = "43.133.150.106";
constexpr uint16_t ML307_SERVER_PORT = 8081;
constexpr uint8_t ML307_TCP_STREAM_ID = 0;

class ModemLock {
public:
    explicit ModemLock(SemaphoreHandle_t mutex) : mutex_(mutex) {
        locked_ = mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
    }
    ~ModemLock() {
        if (locked_) xSemaphoreGive(mutex_);
    }
    bool locked() const { return locked_; }

private:
    SemaphoreHandle_t mutex_;
    bool locked_ = false;
};

void logUrcLine(const String &line) {
    constexpr size_t MAX_URC_LOG_CHARS = 160;
    Serial.print("[ML307 URC] ");
    if (line.length() <= MAX_URC_LOG_CHARS) {
        Serial.println(line);
        return;
    }

    Serial.write(reinterpret_cast<const uint8_t *>(line.c_str()), MAX_URC_LOG_CHARS);
    Serial.printf("... (%u chars)\n", static_cast<unsigned>(line.length()));
}
}

Ml307TcpClient::Ml307TcpClient(Ml307 &modem) : modem_(modem) {}

int Ml307TcpClient::connect(IPAddress ip, uint16_t port) {
    char host[16] = {};
    snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return connect(host, port);
}

int Ml307TcpClient::connect(const char *host, uint16_t port) {
    return modem_.openTcpStream(host, port) ? 1 : 0;
}

size_t Ml307TcpClient::write(uint8_t value) { return write(&value, 1); }

size_t Ml307TcpClient::write(const uint8_t *buffer, size_t size) {
    return modem_.writeTcpStream(buffer, size);
}

int Ml307TcpClient::available() { return modem_.availableTcpStream(); }

int Ml307TcpClient::read() {
    uint8_t value = 0;
    return read(&value, 1) == 1 ? value : -1;
}

int Ml307TcpClient::read(uint8_t *buffer, size_t size) {
    return modem_.readTcpStream(buffer, size);
}

int Ml307TcpClient::peek() { return modem_.peekTcpStream(); }

void Ml307TcpClient::flush() {}

void Ml307TcpClient::stop() { modem_.closeTcpStream(); }

uint8_t Ml307TcpClient::connected() { return modem_.tcpStreamConnected() ? 1 : 0; }

Ml307TcpClient::operator bool() { return connected(); }

Ml307::Ml307(HardwareSerial &serial, int rxPin, int txPin, int powerPin,
             uint32_t baud)
    : serial_(serial), rxPin_(rxPin), txPin_(txPin), powerPin_(powerPin),
      baud_(baud) {}

void Ml307::begin() {
    if (started_) return;

    commandMutex_ = xSemaphoreCreateMutex();
    if (!commandMutex_) {
        Serial.println("[ML307] Could not create UART mutex");
        return;
    }

    pinMode(powerPin_, OUTPUT);
    // The standalone ML307 test project documents the module as externally
    // powered and requiring DTR held low. Keep this control line low; treating
    // it as active-high power prevents AT responses on this board.
    digitalWrite(powerPin_, LOW);
    serial_.begin(baud_, SERIAL_8N1, rxPin_, txPin_);
    serial_.setTimeout(200);
    delay(150);
    while (serial_.available() > 0) serial_.read();
    started_ = true;
    Serial.printf("[ML307] UART1 started baud=%lu RX=GPIO%d TX=GPIO%d DTR=GPIO%d LOW\n",
                  static_cast<unsigned long>(baud_), rxPin_, txPin_, powerPin_);
}

void Ml307::setPowered(bool powered) {
    begin();
    if (powered_ == powered) return;

    digitalWrite(powerPin_, LOW);
    powered_ = powered;
    if (powered) {
        poweredAtMs_ = millis();
    } else {
        poweredAtMs_ = 0;
        connected_ = false;
    }
    Serial.printf("[ML307] logical power=%s DTR=LOW\n", powered ? "ON" : "OFF");
}

bool Ml307::probeConnection() {
    begin();
    ModemLock lock(commandMutex_);
    if (!lock.locked()) return false;
    if (!powered_) {
        connected_ = false;
        return false;
    }

    constexpr uint32_t MODEM_BOOT_SETTLE_MS = 5000;
    const uint32_t poweredForMs = millis() - poweredAtMs_;
    if (poweredForMs < MODEM_BOOT_SETTLE_MS) {
        const uint32_t remainingMs = MODEM_BOOT_SETTLE_MS - poweredForMs;
        Serial.printf("[ML307] Waiting %lu ms for modem startup\n",
                      static_cast<unsigned long>(remainingMs));
        vTaskDelay(pdMS_TO_TICKS(remainingMs));
    }

    String response;
    if (!sendCommand("AT", 2000, response)) {
        connected_ = false;
        Serial.println("[ML307] No AT response. Check ESP RX GPIO44 <- modem TX and ESP TX GPIO43 -> modem RX.");
        return false;
    }

    sendCommand("ATE0", 1000, response);
    if (!waitForSimReady()) {
        connected_ = false;
        return false;
    }

    if (sendCommand("AT+CSQ", 3000, response)) {
        Serial.printf("[ML307] signal=%s\n", response.c_str());
    }

    if (!waitForNetworkRegistration()) {
        connected_ = false;
        return false;
    }

    response = "";
    sendCommand("AT+MIPCALL?", 3000, response);
    bool ready = mipCallIsActive(response);
    if (!ready && powered_) {
        sendCommand("AT+MIPCALL=1", 8000, response);
        response = "";
        sendCommand("AT+MIPCALL?", 3000, response);
        ready = mipCallIsActive(response);
    }

    if (!ready) {
        connected_ = false;
        Serial.println("[ML307] modem=AT OK network=NOT READY");
        return false;
    }

    connected_ = ensureTcpConnection();
    Serial.printf("[ML307] modem=AT OK network=READY tcp=%s:%u %s\n",
                  ML307_SERVER, ML307_SERVER_PORT,
                  connected_ ? "CONNECTED" : "NOT CONNECTED");
    return connected_;
}

bool Ml307::ensureTcpConnection() {
    String response;
    if (sendCommand("AT+MIPSTATE=0", 5000, response) &&
        mipStateIsConnected(response)) {
        Serial.printf("[ML307] TCP already connected: %s:%u\n",
                      ML307_SERVER, ML307_SERVER_PORT);
        return true;
    }

    return openTcpConnection(ML307_SERVER, ML307_SERVER_PORT);
}

bool Ml307::openTcpConnection(const char *host, uint16_t port) {
    String response;
    sendCommand("AT+MIPCLOSE=0", 5000, response);

    if (!sendCommand("AT+MIPCFG=\"encoding\",0,1,1", 5000, response)) {
        Serial.println("[ML307] Failed to configure TCP hex encoding");
        return false;
    }

    char command[128] = {};
    snprintf(command, sizeof(command), "AT+MIPOPEN=%u,\"TCP\",\"%s\",%u,,0",
             ML307_TCP_STREAM_ID, host, port);
    if (!sendCommand(command, 5000, response)) {
        Serial.println("[ML307] Failed to request TCP connection");
        return false;
    }

    int connectionId = -1;
    int errorCode = -1;
    if (parseMipOpen(response, connectionId, errorCode)) {
        if (connectionId == ML307_TCP_STREAM_ID && errorCode == 0) {
            Serial.printf("[ML307] TCP connected: %s:%u\n",
                          host, port);
            return true;
        }
        Serial.printf("[ML307] TCP open failed id=%d error=%d\n",
                      connectionId, errorCode);
        return false;
    }

    return waitForTcpOpen(15000);
}

bool Ml307::waitForTcpOpen(uint32_t timeoutMs) {
    String response;
    const uint32_t started = millis();
    while (millis() - started < timeoutMs) {
        while (serial_.available() > 0) {
            response += static_cast<char>(serial_.read());
            if (response.indexOf("ERROR") >= 0) {
                Serial.printf("[ML307] TCP open response: %s\n", response.c_str());
                return false;
            }

            int connectionId = -1;
            int errorCode = -1;
            if (parseMipOpen(response, connectionId, errorCode)) {
                Serial.printf("[ML307] << %s\n", response.c_str());
                if (connectionId == 0 && errorCode == 0) {
                    Serial.printf("[ML307] TCP connected: %s:%u\n",
                                  ML307_SERVER, ML307_SERVER_PORT);
                    return true;
                }
                Serial.printf("[ML307] TCP open failed id=%d error=%d\n",
                              connectionId, errorCode);
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    Serial.println("[ML307] Timed out waiting for +MIPOPEN");
    return false;
}

bool Ml307::waitForSimReady() {
    String response;
    for (int attempt = 1; attempt <= 10 && powered_; ++attempt) {
        const bool commandOk = sendCommand("AT+CPIN?", 3000, response);
        if (simIsReady(response)) {
            Serial.println("[ML307] SIM=READY");
            return true;
        }

        if (response.indexOf("+CME ERROR: 10") >= 0 ||
            response.indexOf("SIM not inserted") >= 0) {
            Serial.println("[ML307] SIM not inserted or not detected");
            return false;
        }

        String status = response;
        status.replace("\r", "");
        status.replace("\n", " ");
        status.trim();
        Serial.printf("[ML307] SIM not ready attempt=%d/10 response=%s%s\n",
                      attempt, status.length() ? status.c_str() : "<timeout>",
                      commandOk ? "" : " (command failed)");
        if (attempt < 10) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

bool Ml307::waitForNetworkRegistration() {
    String response;
    if (!sendCommand("AT+CEREG=2", 3000, response)) {
        Serial.println("[ML307] Could not enable LTE registration reports");
        return false;
    }

    for (int attempt = 1; attempt <= 15 && powered_; ++attempt) {
        if (!sendCommand("AT+CEREG?", 3000, response)) {
            Serial.printf("[ML307] LTE registration query failed attempt=%d/15\n", attempt);
        } else {
            const int state = registrationState(response, "+CEREG:");
            Serial.printf("[ML307] LTE registration=%d (%s) attempt=%d/15\n",
                          state, registrationStateName(state), attempt);
            if (state == 1 || state == 5) {
                sendCommand("AT+COPS?", 3000, response);
                return true;
            }
            if (state == 3) {
                Serial.println("[ML307] Network registration denied by operator");
                return false;
            }
        }
        if (attempt < 15) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    Serial.println("[ML307] Timed out waiting for SIM network registration");
    return false;
}

bool Ml307::isPowered() const {
    return powered_;
}

bool Ml307::isConnected() const {
    return connected_;
}

bool Ml307::openTcpStream(const char *host, uint16_t port) {
    begin();
    if (!host || !host[0] || !connected_ || tcpStreamOpen_) return false;
    if (!commandMutex_ || xSemaphoreTake(commandMutex_, portMAX_DELAY) != pdTRUE) return false;
    tcpStreamLockHeld_ = true;
    if (!tcpRxBuffer_) {
        tcpRxBuffer_ = static_cast<uint8_t *>(malloc(TCP_RX_BUFFER_SIZE));
        if (!tcpRxBuffer_) {
            Serial.printf("[ML307 TCP] receive buffer allocation failed bytes=%u\n",
                          static_cast<unsigned>(TCP_RX_BUFFER_SIZE));
            releaseTcpStreamLock();
            return false;
        }
    }
    resetTcpReceiveBuffer();
    // Do not retain the capacity of a previous multi-kilobyte URC between
    // radio attempts; it fragments the small internal heap needed by MAD.
    tcpPendingLine_ = String();
    if (!openTcpConnection(host, port)) {
        free(tcpRxBuffer_);
        tcpRxBuffer_ = nullptr;
        releaseTcpStreamLock();
        return false;
    }
    snprintf(tcpStreamHost_, sizeof(tcpStreamHost_), "%s", host);
    tcpStreamPort_ = port;
    tcpStreamOpen_ = true;
    Serial.printf("[ML307 TCP] stream connected %s:%u\n", host, port);
    return true;
}

size_t Ml307::writeTcpStream(const uint8_t *buffer, size_t size) {
    if (!tcpStreamOpen_ || !buffer || size == 0) return 0;
    constexpr size_t MAX_SEND_CHUNK = 512;
    size_t sent = 0;
    while (sent < size && tcpStreamOpen_) {
        const size_t chunk = min<size_t>(MAX_SEND_CHUNK, size - sent);
        while (serial_.available() > 0) pumpTcpStream(0);
        String command;
        command.reserve(16 + chunk * 2);
        command += "AT+MIPSEND=";
        command += String(ML307_TCP_STREAM_ID);
        command += ",";
        command += String(static_cast<unsigned>(chunk));
        command += ",";
        appendHex(command, reinterpret_cast<const char *>(buffer + sent), chunk);
        Serial.printf("[ML307 TCP] >> AT+MIPSEND=%u,%u hex=%u\n",
                      ML307_TCP_STREAM_ID, static_cast<unsigned>(chunk),
                      static_cast<unsigned>(chunk * 2));
        String response;
        if (!sendCommand(command.c_str(), 5000, response)) {
            Serial.printf("[ML307 TCP] send failed response=%s\n", response.c_str());
            tcpStreamOpen_ = false;
            break;
        }
        sent += chunk;
    }
    return sent;
}

int Ml307::availableTcpStream() {
    pumpTcpStream(0);
    return static_cast<int>(min<size_t>(tcpRxLength_, INT_MAX));
}

int Ml307::readTcpStream(uint8_t *buffer, size_t size) {
    if (!tcpRxBuffer_ || !buffer || size == 0) return 0;
    const uint32_t started = millis();
    while (tcpStreamOpen_ && tcpRxLength_ == 0 && millis() - started < 2000) {
        pumpTcpStream(50);
        if (tcpRxLength_ == 0) vTaskDelay(pdMS_TO_TICKS(2));
    }
    const size_t count = min<size_t>(size, tcpRxLength_);
    for (size_t i = 0; i < count; ++i) {
        buffer[i] = tcpRxBuffer_[tcpRxRead_];
        tcpRxRead_ = (tcpRxRead_ + 1) % TCP_RX_BUFFER_SIZE;
    }
    tcpRxLength_ -= count;
    return static_cast<int>(count);
}

int Ml307::peekTcpStream() {
    if (availableTcpStream() <= 0) return -1;
    return tcpRxBuffer_[tcpRxRead_];
}

void Ml307::closeTcpStream() {
    if (!tcpStreamOpen_ && !tcpStreamLockHeld_) return;
    tcpStreamOpen_ = false;
    String response;
    sendCommand("AT+MIPCLOSE=0", 5000, response);
    resetTcpReceiveBuffer();
    tcpPendingLine_ = String();
    free(tcpRxBuffer_);
    tcpRxBuffer_ = nullptr;
    releaseTcpStreamLock();
    Serial.println("[ML307 TCP] stream closed");
}

bool Ml307::tcpStreamConnected() {
    pumpTcpStream(0);
    return tcpStreamOpen_;
}

void Ml307::pumpTcpStream(uint32_t timeoutMs) {
    const uint32_t started = millis();
    do {
        while (serial_.available() > 0) {
            const char value = static_cast<char>(serial_.read());
            tcpPendingLine_ += value;
            if (value != '\n') continue;
            String line = tcpPendingLine_;
            tcpPendingLine_ = "";
            line.replace("\r", "");
            line.replace("\n", "");
            line.trim();
            if (line.length()) parseTcpUrcLine(line);
        }
        if (timeoutMs == 0 || tcpRxLength_ > 0 || !tcpStreamOpen_) break;
        vTaskDelay(pdMS_TO_TICKS(2));
    } while (millis() - started < timeoutMs);
}

void Ml307::resetTcpReceiveBuffer() {
    tcpRxRead_ = 0;
    tcpRxWrite_ = 0;
    tcpRxLength_ = 0;
}

bool Ml307::appendTcpReceiveByte(uint8_t value) {
    if (!tcpRxBuffer_ || tcpRxLength_ >= TCP_RX_BUFFER_SIZE) return false;
    tcpRxBuffer_[tcpRxWrite_] = value;
    tcpRxWrite_ = (tcpRxWrite_ + 1) % TCP_RX_BUFFER_SIZE;
    ++tcpRxLength_;
    return true;
}

bool Ml307::parseTcpUrcLine(const String &line) {
    if (line.indexOf("+MIPURC: \"disconn\"") >= 0) {
        tcpStreamOpen_ = false;
        Serial.printf("[ML307 TCP] disconnected: %s\n", line.c_str());
        return true;
    }
    const int marker = line.indexOf("+MIPURC: \"rtcp\"");
    if (marker < 0) return false;
    int firstComma = line.indexOf(',', marker);
    int secondComma = firstComma >= 0 ? line.indexOf(',', firstComma + 1) : -1;
    int thirdComma = secondComma >= 0 ? line.indexOf(',', secondComma + 1) : -1;
    if (firstComma < 0 || secondComma < 0 || thirdComma < 0) return false;
    const int socketId = line.substring(firstComma + 1, secondComma).toInt();
    const int byteCount = line.substring(secondComma + 1, thirdComma).toInt();
    if (socketId != ML307_TCP_STREAM_ID || byteCount <= 0) return false;
    String payload = line.substring(thirdComma + 1);
    payload.trim();
    const bool looksHex = payload.length() >= byteCount * 2;
    int appended = 0;
    if (looksHex) {
        for (int i = 0; i + 1 < payload.length() && appended < byteCount; i += 2) {
            const int high = hexValue(payload[i]);
            const int low = hexValue(payload[i + 1]);
            if (high < 0 || low < 0) break;
            if (!appendTcpReceiveByte(static_cast<uint8_t>((high << 4) | low))) break;
            ++appended;
        }
    }
    if (appended == 0) {
        for (int i = 0; i < payload.length() && appended < byteCount; ++i) {
            if (!appendTcpReceiveByte(static_cast<uint8_t>(payload[i]))) break;
            ++appended;
        }
    }
    if (appended < byteCount) {
        Serial.printf("[ML307 TCP] receive truncated expected=%d appended=%d buffered=%u\n",
                      byteCount, appended, static_cast<unsigned>(tcpRxLength_));
    }
    return appended > 0;
}

void Ml307::releaseTcpStreamLock() {
    if (!tcpStreamLockHeld_) return;
    tcpStreamLockHeld_ = false;
    xSemaphoreGive(commandMutex_);
}

bool Ml307::httpGet(const char *url, String &payload, uint32_t timeoutMs) {
    payload = "";
    return httpGetInternal(url, &payload, nullptr, nullptr, timeoutMs);
}

bool Ml307::httpGet(const char *url, Print &output, size_t &bytesWritten,
                    uint32_t timeoutMs) {
    bytesWritten = 0;
    return httpGetInternal(url, nullptr, &output, &bytesWritten, timeoutMs);
}

bool Ml307::httpGetInternal(const char *url, String *payload, Print *output,
                            size_t *bytesWritten, uint32_t timeoutMs) {
    begin();
    ModemLock lock(commandMutex_);
    if (!lock.locked()) return false;
    if (!url || !connected_) return false;

    const char *schemeEnd = strstr(url, "://");
    if (!schemeEnd) return false;
    const char *hostStart = schemeEnd + 3;
    const char *pathStart = strchr(hostStart, '/');
    char protocol[8] = {};
    char host[128] = {};
    char path[256] = "/";
    const size_t protocolLength = min<size_t>(schemeEnd - url, sizeof(protocol) - 1);
    memcpy(protocol, url, protocolLength);
    protocol[protocolLength] = '\0';
    const size_t hostLength = pathStart ? static_cast<size_t>(pathStart - hostStart) : strlen(hostStart);
    if (hostLength == 0 || hostLength >= sizeof(host)) return false;
    memcpy(host, hostStart, hostLength);
    host[hostLength] = '\0';
    if (pathStart) snprintf(path, sizeof(path), "%s", pathStart);

    String response;
    sendCommand("AT+MHTTPDEL=0", 1000, response);
    char command[384] = {};
    snprintf(command, sizeof(command), "AT+MHTTPCREATE=\"%s://%s\"", protocol, host);
    if (!sendCommand(command, 5000, response)) return false;
    if (strcmp(protocol, "https") == 0) {
        sendCommand("AT+MHTTPCFG=\"ssl\",0,1,0", 2000, response);
    }
    sendCommand("AT+MHTTPCFG=\"encoding\",0,0,0", 2000, response);

    String hexPath;
    appendHex(hexPath, path, strlen(path));
    sendCommand("AT+MHTTPCFG=\"encoding\",0,1,1", 1000, response);
    snprintf(command, sizeof(command), "AT+MHTTPREQUEST=0,1,0,\"%s\"", hexPath.c_str());
    if (!sendCommand(command, 10000, response)) {
        sendCommand("AT+MHTTPDEL=0", 1000, response);
        return false;
    }

    String pending;
    const uint32_t deadline = millis() + timeoutMs;
    bool finished = false;
    bool writeFailed = false;
    while (static_cast<int32_t>(millis() - deadline) < 0) {
        while (serial_.available() > 0) pending += static_cast<char>(serial_.read());
        int newline = pending.indexOf('\n');
        if (newline < 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        String line = pending.substring(0, newline);
        pending.remove(0, newline + 1);
        line.replace("\r", "");
        logUrcLine(line);
        if (line.indexOf("+MHTTPURC: \"content\"") >= 0) {
            String hex;
            if (extractContentHex(line, hex)) {
                for (int i = 0; i + 1 < hex.length(); i += 2) {
                    const int high = hexValue(hex[i]);
                    const int low = hexValue(hex[i + 1]);
                    if (high < 0 || low < 0) continue;
                    const uint8_t value = static_cast<uint8_t>((high << 4) | low);
                    if (payload) *payload += static_cast<char>(value);
                    if (output) {
                        if (output->write(value) != 1) {
                            writeFailed = true;
                            break;
                        }
                        if (bytesWritten) ++*bytesWritten;
                    }
                }
            }
            if (writeFailed) break;
            int commaCount = 0;
            int contentLength = -1;
            int totalLength = -1;
            int currentLength = -1;
            for (int i = 0; i < line.length(); ++i) {
                if (line[i] != ',') continue;
                ++commaCount;
                const int valueStart = i + 1;
                const int valueEnd = line.indexOf(',', valueStart);
                const String value = line.substring(valueStart,
                    valueEnd >= 0 ? valueEnd : line.length());
                if (commaCount == 2) contentLength = value.toInt();
                if (commaCount == 3) totalLength = value.toInt();
                if (commaCount == 4) currentLength = value.toInt();
            }
            // ML307 firmware reports HTTP content in several formats. For the
            // playlist endpoint, the final URC is commonly
            // `content,0,0,<total>,0`; the zero content-length field does not
            // mean the body is empty. A positive total plus currentLength=0 is
            // the modem's end-of-body marker.
            if ((contentLength > 0 && totalLength >= contentLength) ||
                (totalLength > 0 && currentLength == 0)) {
                finished = true;
            }
        } else if (line.indexOf("+MHTTPURC: \"err\"") >= 0) {
            break;
        }
        if (finished) break;
    }
    sendCommand("AT+MHTTPDEL=0", 1500, pending);
    return !writeFailed && finished && ((payload && payload->length() > 0) ||
                        (bytesWritten && *bytesWritten > 0));
}

String Ml307::readResponse(uint32_t timeoutMs, const char *tokenA, const char *tokenB) {
    String response;
    const uint32_t started = millis();
    while (millis() - started < timeoutMs) {
        while (serial_.available() > 0) {
            response += static_cast<char>(serial_.read());
            if ((tokenA && response.indexOf(tokenA) >= 0) ||
                (tokenB && response.indexOf(tokenB) >= 0)) {
                return response;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return response;
}

bool Ml307::sendCommand(const char *command, uint32_t timeoutMs, String &response) {
    while (serial_.available() > 0) serial_.read();
    Serial.printf("[ML307] >> %s\n", command);
    serial_.print(command);
    serial_.print("\r\n");
    serial_.flush();
    response = readResponse(timeoutMs, "OK", "ERROR");

    String monitorResponse = response;
    monitorResponse.replace("\r", "");
    monitorResponse.replace("\n", " ");
    monitorResponse.trim();
    Serial.printf("[ML307] << %s\n", monitorResponse.length() ? monitorResponse.c_str() : "<timeout>");
    return response.indexOf("OK") >= 0 && response.indexOf("ERROR") < 0;
}

bool Ml307::mipCallIsActive(const String &response) {
    const int marker = response.indexOf("+MIPCALL:");
    if (marker < 0) return false;
    const int colon = response.indexOf(':', marker);
    const int firstComma = response.indexOf(',', colon + 1);
    if (colon < 0 || firstComma < 0) return false;
    const int secondComma = response.indexOf(',', firstComma + 1);
    const int lineEnd = response.indexOf('\n', firstComma + 1);
    String state = response.substring(firstComma + 1,
        secondComma >= 0 ? secondComma : lineEnd >= 0 ? lineEnd : response.length());
    state.trim();
    return state == "1";
}

bool Ml307::mipStateIsConnected(const String &response) {
    const int marker = response.indexOf("+MIPSTATE:");
    if (marker < 0) return false;
    const String state = response.substring(marker);
    return state.indexOf("\"TCP\"") >= 0 &&
           state.indexOf(ML307_SERVER) >= 0 &&
           state.indexOf(String(ML307_SERVER_PORT)) >= 0 &&
           state.indexOf("\"CONNECTED\"") >= 0;
}

bool Ml307::parseMipOpen(const String &response, int &connectionId, int &errorCode) {
    const int marker = response.indexOf("+MIPOPEN:");
    if (marker < 0) return false;
    return sscanf(response.c_str() + marker, "+MIPOPEN: %d , %d",
                  &connectionId, &errorCode) == 2;
}

bool Ml307::simIsReady(const String &response) {
    return response.indexOf("+CPIN: READY") >= 0;
}

int Ml307::registrationState(const String &response, const char *prefix) {
    const int marker = response.indexOf(prefix);
    if (marker < 0) return -1;
    const int colon = response.indexOf(':', marker);
    if (colon < 0) return -1;
    const int lineEnd = response.indexOf('\n', colon + 1);
    String fields = response.substring(colon + 1, lineEnd >= 0 ? lineEnd : response.length());
    fields.trim();
    const int comma = fields.indexOf(',');
    if (comma >= 0) {
        fields = fields.substring(comma + 1);
        const int nextComma = fields.indexOf(',');
        if (nextComma >= 0) fields = fields.substring(0, nextComma);
    }
    fields.trim();
    if (fields.length() != 1 || fields[0] < '0' || fields[0] > '9') return -1;
    return fields[0] - '0';
}

const char *Ml307::registrationStateName(int state) {
    switch (state) {
    case 0: return "not registered";
    case 1: return "home network";
    case 2: return "searching";
    case 3: return "denied";
    case 4: return "unknown";
    case 5: return "roaming";
    default: return "invalid response";
    }
}

void Ml307::appendHex(String &out, const char *data, size_t length) {
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < length; ++i) {
        const uint8_t value = static_cast<uint8_t>(data[i]);
        out += digits[value >> 4];
        out += digits[value & 0x0F];
    }
}

int Ml307::hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

bool Ml307::extractContentHex(const String &line, String &hex) {
    const int marker = line.indexOf("\"content\"");
    if (marker < 0) return false;
    int comma = marker;
    for (int count = 0; count < 5; ++count) {
        comma = line.indexOf(',', comma + 1);
        if (comma < 0) return false;
    }
    hex = line.substring(comma + 1);
    hex.trim();
    return hex.length() > 0;
}