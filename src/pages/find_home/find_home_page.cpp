#include "pages/find_home/find_home_page.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <cmath>
#include <cstring>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/ml307/ml307.h"
#include "ui/localization.h"
#include "ui/loading_indicator.h"

namespace {

constexpr uint8_t MAX_WIFI_RESULTS = 8;
constexpr int ACTION_X = 24;
constexpr int ACTION_Y = 306;
constexpr int ACTION_W = 192;
constexpr int ACTION_H = 64;

struct WifiResult {
    char bssid[18] = {};
    char ssid[33] = {};
    int32_t rssi = 0;
};

WifiResult wifiResults[MAX_WIFI_RESULTS] = {};
uint8_t wifiCount = 0;
bool homeSet = false;
bool requestPending = false;
bool exitRequested = false;
char status[48] = "Tap to set home";
char message[96] = {};
char resultStatus[24] = {};
char direction[32] = {};
char directionHint[64] = {};
char cellStatus[32] = {};
float distanceKm = NAN;
float distanceEstM = NAN;
int32_t lastHttpCode = 0;

void pixel(uint8_t *frame, int x, int y) {
    if (!frame || x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |= 0x80U >> (x % 8);
}

void hline(uint8_t *frame, int x, int y, int width) {
    for (int i = 0; i < width; ++i) pixel(frame, x + i, y);
}

void vline(uint8_t *frame, int x, int y, int height) {
    for (int i = 0; i < height; ++i) pixel(frame, x, y + i);
}

void roundedFrame(uint8_t *frame, int x, int y, int width, int height) {
    hline(frame, x + 6, y, width - 12);
    hline(frame, x + 6, y + height - 1, width - 12);
    vline(frame, x, y + 6, height - 12);
    vline(frame, x + width - 1, y + 6, height - 12);
    for (int i = 0; i < 6; ++i) {
        pixel(frame, x + 5 - i, y + i);
        pixel(frame, x + width - 6 + i, y + i);
        pixel(frame, x + 5 - i, y + height - 1 - i);
        pixel(frame, x + width - 6 + i, y + height - 1 - i);
    }
}

bool inRect(int16_t x, int16_t y, int left, int top, int width, int height) {
    return x >= left && x < left + width && y >= top && y < top + height;
}

void copyText(char *destination, size_t capacity, const char *source) {
    if (!destination || capacity == 0) return;
    std::strncpy(destination, source ? source : "", capacity - 1);
    destination[capacity - 1] = '\0';
}

char configuredHost[128] = {};

String endpointUrl() {
    String base(configuredHost);
    base.trim();
    while (base.endsWith("/")) base.remove(base.length() - 1);
    const int scheme = base.indexOf("://");
    if (scheme < 0) return String();
    const int path = base.indexOf('/', scheme + 3);
    if (path >= 0) base.remove(path);
    return base + "/api/find-home/footprint";
}

void scanWifi() {
    wifiCount = 0;
    if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
    const int found = WiFi.scanNetworks(false, true);
    if (found <= 0) {
        WiFi.scanDelete();
        return;
    }
    wifiCount = static_cast<uint8_t>(min(found, static_cast<int>(MAX_WIFI_RESULTS)));
    for (uint8_t index = 0; index < wifiCount; ++index) {
        copyText(wifiResults[index].bssid, sizeof(wifiResults[index].bssid), WiFi.BSSIDstr(index).c_str());
        copyText(wifiResults[index].ssid, sizeof(wifiResults[index].ssid), WiFi.SSID(index).c_str());
        wifiResults[index].rssi = WiFi.RSSI(index);
    }
    WiFi.scanDelete();
}

void parseResponse(const String &payload) {
    JsonDocument document;
    const DeserializationError error = deserializeJson(document, payload);
    if (error) {
        copyText(status, sizeof(status), "Invalid server response");
        return;
    }
    JsonObject result = document["result"].as<JsonObject>();
    if (result.isNull()) result = document.as<JsonObject>();
    copyText(resultStatus, sizeof(resultStatus), result["status"] | "");
    copyText(message, sizeof(message), result["message"] | "");
    copyText(direction, sizeof(direction), result["direction_text"] | "");
    copyText(directionHint, sizeof(directionHint), result["direction_hint"] | "");
    copyText(cellStatus, sizeof(cellStatus), result["cell_status"] | "");
    distanceKm = result["distance_km"] | NAN;
    JsonObject wifi = result["wifi"].as<JsonObject>();
    distanceEstM = wifi["dist_est_m"] | NAN;
    if (std::isnan(distanceEstM)) distanceEstM = wifi["distance_est_m"] | NAN;
    if (message[0]) copyText(status, sizeof(status), message);
    else if (resultStatus[0]) copyText(status, sizeof(status), resultStatus);
    else copyText(status, sizeof(status), "No result");
}

bool postViaCellular(const String &url, const String &body, String &responseBody,
                     int32_t &responseCode) {
    if (!url.startsWith("http://")) return false;
    const int hostStart = 7;
    const int pathStart = url.indexOf('/', hostStart);
    String authority = pathStart >= 0 ? url.substring(hostStart, pathStart)
                                      : url.substring(hostStart);
    const String path = pathStart >= 0 ? url.substring(pathStart) : "/";
    uint16_t port = 80;
    const int colon = authority.lastIndexOf(':');
    if (colon > 0) {
        port = static_cast<uint16_t>(authority.substring(colon + 1).toInt());
        authority.remove(colon);
    }
    if (authority.isEmpty() || port == 0) return false;

    Ml307TcpClient client(cellularModem);
    if (!client.connect(authority.c_str(), port)) return false;
    String request;
    request.reserve(body.length() + 256);
    request += "POST ";
    request += path;
    request += " HTTP/1.1\r\nHost: ";
    request += authority;
    request += "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ";
    request += String(body.length());
    request += "\r\n\r\n";
    request += body;
    if (client.write(reinterpret_cast<const uint8_t *>(request.c_str()), request.length()) !=
        request.length()) {
        client.stop();
        return false;
    }

    String response;
    const uint32_t started = millis();
    uint32_t lastData = started;
    while (millis() - started < 20000) {
        while (client.available() > 0) {
            const int value = client.read();
            if (value < 0) break;
            response += static_cast<char>(value);
            lastData = millis();
        }
        if (!client.connected() && client.available() == 0) break;
        if (millis() - lastData > 5000) break;
        delay(2);
    }
    client.stop();
    const int firstSpace = response.indexOf(' ');
    responseCode = firstSpace >= 0 ? response.substring(firstSpace + 1).toInt() : 0;
    const int bodyStart = response.indexOf("\r\n\r\n");
    if (bodyStart >= 0) responseBody = response.substring(bodyStart + 4);
    return responseCode >= 200 && responseCode < 300 && !responseBody.isEmpty();
}

bool postFootprint() {
    if (WiFi.status() != WL_CONNECTED && !cellularModem.isConnected()) {
        copyText(status, sizeof(status), "Network unavailable");
        return false;
    }
    scanWifi();
    JsonDocument request;
    request["kind"] = homeSet ? "current" : "home";
    JsonArray wifi = request["wifi"].to<JsonArray>();
    for (uint8_t index = 0; index < wifiCount; ++index) {
        JsonObject item = wifi.add<JsonObject>();
        item["bssid"] = wifiResults[index].bssid;
        if (wifiResults[index].ssid[0]) item["ssid"] = wifiResults[index].ssid;
        item["rssi"] = wifiResults[index].rssi;
    }
    String body;
    serializeJson(request, body);
    String url = endpointUrl();
    if (url.isEmpty()) {
        copyText(status, sizeof(status), "Set content URL first");
        return false;
    }

    UiLoadingIndicator::Scope loading;
    HTTPClient http;
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    http.setConnectTimeout(7000);
    http.setTimeout(20000);
    http.setReuse(false);
    bool began = false;
    if (WiFi.status() == WL_CONNECTED) {
        began = url.startsWith("https://")
            ? (secureClient.setInsecure(), http.begin(secureClient, url))
            : http.begin(plainClient, url);
    } else {
        String response;
        const bool succeeded = postViaCellular(url, body, response, lastHttpCode);
        Serial.printf("[FIND HOME] 4G POST kind=%s code=%ld wifi=%u bytes=%u\n",
                      homeSet ? "current" : "home", static_cast<long>(lastHttpCode),
                      static_cast<unsigned>(wifiCount), static_cast<unsigned>(response.length()));
        if (!succeeded) {
            copyText(status, sizeof(status), url.startsWith("https://")
                ? "4G requires HTTP URL" : "Request failed");
            return false;
        }
        homeSet = true;
        parseResponse(response);
        return true;
    }
    if (!began) {
        copyText(status, sizeof(status), "Invalid endpoint");
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Connection", "close");
    lastHttpCode = http.POST(body);
    String response;
    if (lastHttpCode >= 200 && lastHttpCode < 300) response = http.getString();
    http.end();
    Serial.printf("[FIND HOME] POST kind=%s code=%ld wifi=%u bytes=%u\n",
                  homeSet ? "current" : "home", static_cast<long>(lastHttpCode),
                  static_cast<unsigned>(wifiCount), static_cast<unsigned>(response.length()));
    if (lastHttpCode < 200 || lastHttpCode >= 300) {
        copyText(status, sizeof(status), "Request failed");
        return false;
    }
    homeSet = true;
    parseResponse(response);
    return true;
}

void drawText(uint8_t *frame, int y, const char *text, int scale = 1) {
    UiLocalization::drawCentered(frame, y, text ? text : "", scale);
}

void drawActionIcon(uint8_t *frame) {
    const int centerX = 120;
    const int centerY = ACTION_Y + ACTION_H / 2;
    for (int radius = 20; radius <= 23; ++radius) {
        for (int step = 0; step < 360; step += 10) {
            const float angle = step * 3.1415926f / 180.0f;
            pixel(frame, centerX + static_cast<int>(cosf(angle) * radius),
                  centerY + static_cast<int>(sinf(angle) * radius));
        }
    }
    for (int row = -3; row <= 3; ++row) {
        for (int column = -3; column <= 3; ++column) {
            if (row * row + column * column <= 9) pixel(frame, centerX + column, centerY + row);
        }
    }
}

void drawResult(uint8_t *frame) {
    if (message[0]) drawText(frame, 122, message, 1);
    else drawText(frame, 122, status, 1);
    if (!std::isnan(distanceKm) && direction[0]) {
        char line[64] = {};
        snprintf(line, sizeof(line), "%.1f km %s", static_cast<double>(distanceKm), direction);
        drawText(frame, 154, line, 1);
    } else if (!std::isnan(distanceEstM)) {
        char line[64] = {};
        snprintf(line, sizeof(line), "WiFi %.0f m", static_cast<double>(distanceEstM));
        drawText(frame, 154, line, 1);
    } else if (direction[0]) {
        drawText(frame, 154, direction, 1);
    } else if (directionHint[0]) {
        drawText(frame, 154, directionHint, 1);
    }
    if (cellStatus[0]) drawText(frame, 186, cellStatus, 1);
    else if (resultStatus[0]) drawText(frame, 186, resultStatus, 1);
}

} // namespace

namespace FindHomePage {

void setContentUrl(const char *url) { copyText(configuredHost, sizeof(configuredHost), url); }

void open() {
    requestPending = false;
    exitRequested = false;
    copyText(status, sizeof(status), homeSet ? "Tap to measure here" : "Tap to set home");
}

bool returnControlAt(int16_t x, int16_t y) { return inRect(x, y, 10, 40, 48, 28); }

bool actionControlAt(int16_t x, int16_t y) { return inRect(x, y, ACTION_X, ACTION_Y, ACTION_W, ACTION_H); }

bool handleTap(int16_t x, int16_t y) {
    if (returnControlAt(x, y)) {
        exitRequested = true;
        return true;
    }
    if (!actionControlAt(x, y) || requestPending) return false;
    requestPending = true;
    const bool succeeded = postFootprint();
    requestPending = false;
    if (!succeeded && !message[0]) copyText(message, sizeof(message), status);
    return true;
}

bool takeExitRequest() {
    const bool requested = exitRequested;
    exitRequested = false;
    return requested;
}

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    UiLocalization::drawText(frame, 10, 10, "FIND HOME", 2);
    UiLocalization::drawText(frame, 10, 42, "Find home", 1);
    drawResult(frame);
    roundedFrame(frame, ACTION_X, ACTION_Y, ACTION_W, ACTION_H);
    drawActionIcon(frame);
    const char *label = homeSet ? "MEASURE CURRENT" : "SET HOME";
    drawText(frame, 388, label, 1);
    // Home/back glyph.
    hline(frame, 18, 52, 24);
    vline(frame, 18, 52, 12);
    vline(frame, 41, 52, 12);
    drawText(frame, 260, status, 1);
}

} // namespace FindHomePage