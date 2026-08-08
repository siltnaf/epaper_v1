#include "devices/ml307/ml307.h"

Ml307::Ml307(HardwareSerial &serial, int rxPin, int txPin, int powerPin,
             uint32_t baud)
    : serial_(serial), rxPin_(rxPin), txPin_(txPin), powerPin_(powerPin),
      baud_(baud) {}

void Ml307::begin() {
    if (started_) return;

    pinMode(powerPin_, OUTPUT);
    // GPIO11 is the board's modem power-enable line. The ML307 reference code
    // assumes the module is otherwise powered and only controls this enable.
    digitalWrite(powerPin_, LOW);
    serial_.begin(baud_, SERIAL_8N1, rxPin_, txPin_);
    serial_.setTimeout(200);
    delay(150);
    while (serial_.available() > 0) serial_.read();
    started_ = true;
    Serial.printf("[ML307] UART started baud=%lu RX=GPIO%d TX=GPIO%d PWR=GPIO%d\n",
                  static_cast<unsigned long>(baud_), rxPin_, txPin_, powerPin_);
}

void Ml307::setPowered(bool powered) {
    begin();
    if (powered_ == powered) return;

    digitalWrite(powerPin_, powered ? HIGH : LOW);
    powered_ = powered;
    if (!powered) connected_ = false;
    Serial.printf("[ML307] power=%s\n", powered ? "ON" : "OFF");
}

bool Ml307::probeConnection() {
    if (!powered_) {
        connected_ = false;
        return false;
    }

    String response;
    if (!sendCommand("AT", 2000, response)) {
        connected_ = false;
        Serial.println("[ML307] No AT response. Check ESP RX GPIO44 <- modem TX and ESP TX GPIO43 -> modem RX.");
        return false;
    }

    sendCommand("ATE0", 1000, response);
    if (!sendCommand("AT+CPIN?", 3000, response)) {
        connected_ = false;
        Serial.println("[ML307] SIM query failed");
        return false;
    }
    Serial.printf("[ML307] SIM=%s\n", simIsReady(response) ? "READY" : "NOT READY");
    if (!simIsReady(response)) {
        connected_ = false;
        return false;
    }

    if (sendCommand("AT+CSQ", 3000, response)) {
        Serial.printf("[ML307] signal=%s\n", response.c_str());
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

    connected_ = ready;
    Serial.printf("[ML307] modem=AT OK network=%s\n", ready ? "READY" : "NOT READY");
    return ready;
}

bool Ml307::isPowered() const {
    return powered_;
}

bool Ml307::isConnected() const {
    return connected_;
}

bool Ml307::httpGet(const char *url, String &payload, uint32_t timeoutMs) {
    payload = "";
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

    sendCommand("AT+MHTTPDEL=0", 1000, payload);
    char command[384] = {};
    snprintf(command, sizeof(command), "AT+MHTTPCREATE=\"%s://%s\"", protocol, host);
    if (!sendCommand(command, 5000, payload)) return false;
    if (strcmp(protocol, "https") == 0) {
        sendCommand("AT+MHTTPCFG=\"ssl\",0,1,0", 2000, payload);
    }
    sendCommand("AT+MHTTPCFG=\"encoding\",0,0,0", 2000, payload);

    String hexPath;
    appendHex(hexPath, path, strlen(path));
    sendCommand("AT+MHTTPCFG=\"encoding\",0,1,1", 1000, payload);
    snprintf(command, sizeof(command), "AT+MHTTPREQUEST=0,1,0,\"%s\"", hexPath.c_str());
    if (!sendCommand(command, 10000, payload)) {
        sendCommand("AT+MHTTPDEL=0", 1000, payload);
        return false;
    }

    String pending;
    const uint32_t deadline = millis() + timeoutMs;
    bool finished = false;
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
        Serial.printf("[ML307 URC] %s\n", line.c_str());
        if (line.indexOf("+MHTTPURC: \"content\"") >= 0) {
            String hex;
            if (extractContentHex(line, hex)) {
                for (int i = 0; i + 1 < hex.length(); i += 2) {
                    const int high = hexValue(hex[i]);
                    const int low = hexValue(hex[i + 1]);
                    if (high >= 0 && low >= 0) payload += static_cast<char>((high << 4) | low);
                }
            }
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
            if ((contentLength > 0 && totalLength >= contentLength) ||
                (contentLength == -1 && currentLength == 0)) {
                finished = true;
            }
        } else if (line.indexOf("+MHTTPURC: \"err\"") >= 0) {
            break;
        }
        if (finished) break;
    }
    sendCommand("AT+MHTTPDEL=0", 1500, pending);
    return payload.length() > 0;
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
    response = readResponse(timeoutMs, "OK", "ERROR");

    String monitorResponse = response;
    monitorResponse.replace("\r", "");
    monitorResponse.replace("\n", " ");
    monitorResponse.trim();
    Serial.printf("[ML307] << %s\n", monitorResponse.length() ? monitorResponse.c_str() : "<timeout>");
    return response.indexOf("OK") >= 0 && response.indexOf("ERROR") < 0;
}

bool Ml307::mipCallIsActive(const String &response) {
    return response.indexOf("+MIPCALL:") >= 0 &&
           (response.indexOf(",1") >= 0 || response.indexOf(": 1") >= 0);
}

bool Ml307::simIsReady(const String &response) {
    return response.indexOf("READY") >= 0;
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