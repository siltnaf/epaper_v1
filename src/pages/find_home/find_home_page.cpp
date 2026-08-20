#include "pages/find_home/find_home_page.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <TJpg_Decoder.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <cmath>
#include <cstring>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/ml307/ml307.h"
#include "devices/sd_card/sd_card.h"
#include "ui/localization.h"
#include "ui/loading_indicator.h"

namespace {

constexpr uint8_t MAX_WIFI_RESULTS = 8;
constexpr int ACTION_X = 24;
constexpr int ACTION_Y = 328;
constexpr int ACTION_W = 192;
constexpr int ACTION_H = 52;
constexpr int RESET_X = 158;
constexpr int RESET_Y = 40;
constexpr int RESET_W = 72;
constexpr int RESET_H = 28;
constexpr int MAP_X = 8;
constexpr int MAP_Y = 76;
constexpr int MAP_W = 224;
constexpr int MAP_H = 224;
constexpr char MAP_CACHE_PATH[] = "/find-home/map.jpg";

struct WifiResult {
    char bssid[18] = {};
    char ssid[33] = {};
    int32_t rssi = 0;
};

WifiResult wifiResults[MAX_WIFI_RESULTS] = {};
uint8_t wifiCount = 0;
bool homeSet = false;
bool currentMeasured = false;
bool requestPending = false;
char status[48] = "Tap to set home";
char message[96] = {};
char resultStatus[24] = {};
char direction[32] = {};
char directionHint[64] = {};
char cellStatus[32] = {};
float distanceKm = NAN;
float distanceEstM = NAN;
int32_t lastHttpCode = 0;
uint8_t mapBitmap[MAP_W * MAP_H / 8] = {};
bool mapReady = false;
char mapStatus[48] = "Map not loaded";
char serverLocationIp[48] = {};
Preferences findHomePreferences;

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

const char *localized(const char *english, const char *chinese) {
    return UiLocalization::isChinese() ? chinese : english;
}

void saveHomeState() {
    if (!findHomePreferences.begin("find-home", false)) return;
    findHomePreferences.putBool("home-set", homeSet);
    findHomePreferences.end();
}

void loadHomeState() {
    if (!findHomePreferences.begin("find-home", true)) return;
    homeSet = findHomePreferences.getBool("home-set", false);
    findHomePreferences.end();
}

char configuredHost[128] = {};

String deviceMac() {
    const String mac = WiFi.macAddress();
    return mac == "00:00:00:00:00:00" ? String("ESP32-S3") : mac;
}

bool isPublicIp(const IPAddress &address) {
    const uint8_t first = address[0];
    const uint8_t second = address[1];
    if (first == 0 || first == 10 || first == 127 || first >= 224) return false;
    if (first == 100 && second >= 64 && second <= 127) return false;
    if (first == 169 && second == 254) return false;
    if (first == 172 && second >= 16 && second <= 31) return false;
    if (first == 192 && second == 168) return false;
    return true;
}

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

String mapUrl() {
    String base(configuredHost);
    base.trim();
    while (base.endsWith("/")) base.remove(base.length() - 1);
    const int scheme = base.indexOf("://");
    if (scheme < 0) return String();
    const int path = base.indexOf('/', scheme + 3);
    if (path >= 0) base.remove(path);
    String url = base + "/api/find-home/map.jpg?w=224&h=224&z=auto";
    url += "&device_mac=";
    String mac = deviceMac();
    mac.replace(":", "");
    url += mac;
    if (serverLocationIp[0]) {
        url += "&ip=";
        url += serverLocationIp;
    }
    return url;
}

bool mapJpegOutput(int16_t x, int16_t y, uint16_t width, uint16_t height,
                   uint16_t *data) {
    if (!data) return false;
    static constexpr uint8_t BAYER[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6},
        {3, 11, 1, 9}, {15, 7, 13, 5},
    };
    for (uint16_t row = 0; row < height; ++row)
        for (uint16_t column = 0; column < width; ++column) {
            const int targetX = x + column;
            const int targetY = y + row;
            if (targetX < 0 || targetX >= MAP_W || targetY < 0 || targetY >= MAP_H)
                continue;
            const uint16_t rgb = data[static_cast<size_t>(row) * width + column];
            const uint8_t red = static_cast<uint8_t>((((rgb >> 11) & 0x1F) * 255U) / 31U);
            const uint8_t green = static_cast<uint8_t>((((rgb >> 5) & 0x3F) * 255U) / 63U);
            const uint8_t blue = static_cast<uint8_t>(((rgb & 0x1F) * 255U) / 31U);
            const uint16_t luminance = (77U * red + 150U * green + 29U * blue) >> 8;
            const uint8_t threshold = static_cast<uint8_t>(
                BAYER[targetY & 3][targetX & 3] * 16 + 8);
            if (luminance < threshold) {
                const size_t bitIndex = static_cast<size_t>(targetY) * MAP_W + targetX;
                mapBitmap[bitIndex / 8] |= static_cast<uint8_t>(0x80U >> (bitIndex % 8));
            }
        }
    return true;
}

bool decodeMapJpeg() {
    uint16_t width = 0;
    uint16_t height = 0;
    const JRESULT sizeResult = TJpgDec.getFsJpgSize(
        &width, &height, MAP_CACHE_PATH, SD_MMC);
    if (sizeResult != JDR_OK || width != MAP_W || height != MAP_H) {
        Serial.printf("[FIND HOME JPEG] Invalid size result=%d source=%ux%u\n",
                      static_cast<int>(sizeResult), width, height);
        return false;
    }
    std::memset(mapBitmap, 0, sizeof(mapBitmap));
    TJpgDec.setSwapBytes(false);
    TJpgDec.setJpgScale(1);
    TJpgDec.setCallback(mapJpegOutput);
    const JRESULT drawResult = TJpgDec.drawFsJpg(0, 0, MAP_CACHE_PATH, SD_MMC);
    Serial.printf("[FIND HOME JPEG] Decode result=%d source=%ux%u\n",
                  static_cast<int>(drawResult), width, height);
    return drawResult == JDR_OK;
}

bool loadMap() {
    mapReady = false;
    if (!SdCard::isMounted()) {
        copyText(mapStatus, sizeof(mapStatus), "SD card required");
        return false;
    }
    const String url = mapUrl();
    if (url.isEmpty()) {
        copyText(mapStatus, sizeof(mapStatus), "Set content URL first");
        return false;
    }
    copyText(mapStatus, sizeof(mapStatus), "Loading map");
    if (!SdCard::downloadFile(url.c_str(), MAP_CACHE_PATH, 1024)) {
        copyText(mapStatus, sizeof(mapStatus), "Map download failed");
        return false;
    }
    if (!decodeMapJpeg()) {
        copyText(mapStatus, sizeof(mapStatus), "Map JPEG failed");
        return false;
    }
    mapReady = true;
    copyText(mapStatus, sizeof(mapStatus), "Map ready");
    Serial.printf("[FIND HOME] Map ready url=%s cache=%s\n", url.c_str(), MAP_CACHE_PATH);
    return true;
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
    JsonObject footprint = document["footprint"].as<JsonObject>();
    const char *resolvedIp = footprint["ip"] | "";
    if (!resolvedIp[0]) resolvedIp = footprint["client_ip"] | "";
    copyText(serverLocationIp, sizeof(serverLocationIp), resolvedIp);
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
    Serial.printf("[FIND HOME] Server location IP=%s\n",
                  serverLocationIp[0] ? serverLocationIp : "unavailable");
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
    request += "\r\nContent-Type: application/json\r\nX-Device-MAC: ";
    request += deviceMac();
    request += "\r\nX-Network-Transport: cellular\r\nConnection: close\r\nContent-Length: ";
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
    const bool markingHome = !homeSet;
    scanWifi();
    JsonDocument request;
    request["kind"] = markingHome ? "home" : "current";
    request["device_mac"] = deviceMac();
    request["transport"] = WiFi.status() == WL_CONNECTED ? "wifi_sta" : "cellular";
    JsonObject station = request["station"].to<JsonObject>();
    if (WiFi.status() == WL_CONNECTED) {
        const IPAddress localIp = WiFi.localIP();
        station["ip"] = localIp.toString();
        station["gateway"] = WiFi.gatewayIP().toString();
        station["subnet"] = WiFi.subnetMask().toString();
        station["dns"] = WiFi.dnsIP().toString();
        station["ssid"] = WiFi.SSID();
        station["bssid"] = WiFi.BSSIDstr();
        station["rssi"] = WiFi.RSSI();
        station["mac"] = deviceMac();
        // The content server can only GeoIP a public address. Preserve the
        // private STA address under station.ip, and let the server use the
        // HTTP client_ip unless the interface itself owns a public address.
        if (isPublicIp(localIp)) request["ip"] = localIp.toString();
    }
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
                      markingHome ? "home" : "current", static_cast<long>(lastHttpCode),
                      static_cast<unsigned>(wifiCount), static_cast<unsigned>(response.length()));
        if (!succeeded) {
            copyText(status, sizeof(status), url.startsWith("https://")
                ? "4G requires HTTP URL" : "Request failed");
            return false;
        }
        homeSet = true;
        currentMeasured = !markingHome;
        saveHomeState();
        parseResponse(response);
        return true;
    }
    if (!began) {
        copyText(status, sizeof(status), "Invalid endpoint");
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Device-MAC", deviceMac());
    http.addHeader("X-Network-Transport", "wifi_sta");
    http.addHeader("X-Station-IP", WiFi.localIP().toString());
    http.addHeader("Connection", "close");
    lastHttpCode = http.POST(body);
    String response;
    if (lastHttpCode >= 200 && lastHttpCode < 300) response = http.getString();
    http.end();
    Serial.printf("[FIND HOME] POST kind=%s code=%ld wifi=%u bytes=%u station_ip=%s mac=%s\n",
                  markingHome ? "home" : "current", static_cast<long>(lastHttpCode),
                  static_cast<unsigned>(wifiCount), static_cast<unsigned>(response.length()),
                  WiFi.localIP().toString().c_str(), deviceMac().c_str());
    if (lastHttpCode < 200 || lastHttpCode >= 300) {
        copyText(status, sizeof(status), "Request failed");
        return false;
    }
    homeSet = true;
    currentMeasured = !markingHome;
    saveHomeState();
    parseResponse(response);
    return true;
}

void drawText(uint8_t *frame, int y, const char *text, int scale = 1) {
    UiLocalization::drawCentered(frame, y, text ? text : "", scale);
}

void drawTextButton(uint8_t *frame, int x, int y, int width, int height,
                    const char *label) {
    hline(frame, x, y, width);
    hline(frame, x, y + height - 1, width);
    vline(frame, x, y, height);
    vline(frame, x + width - 1, y, height);
    const int labelWidth = UiLocalization::textWidth(label, 1);
    const int labelY = y + (UiLocalization::isChinese() ? 6 : 10);
    UiLocalization::drawText(frame, x + (width - labelWidth) / 2,
                             labelY, label, 1);
}

void drawMap(uint8_t *frame) {
    hline(frame, MAP_X, MAP_Y, MAP_W);
    hline(frame, MAP_X, MAP_Y + MAP_H - 1, MAP_W);
    vline(frame, MAP_X, MAP_Y, MAP_H);
    vline(frame, MAP_X + MAP_W - 1, MAP_Y, MAP_H);
    if (!mapReady) return;
    for (int y = 0; y < MAP_H; ++y)
        for (int x = 0; x < MAP_W; ++x) {
            const size_t bitIndex = static_cast<size_t>(y) * MAP_W + x;
            if (mapBitmap[bitIndex / 8] & (0x80U >> (bitIndex % 8)))
                pixel(frame, MAP_X + x, MAP_Y + y);
        }
}

void resetHome() {
    homeSet = false;
    currentMeasured = false;
    mapReady = false;
    distanceKm = NAN;
    distanceEstM = NAN;
    message[0] = '\0';
    resultStatus[0] = '\0';
    direction[0] = '\0';
    directionHint[0] = '\0';
    cellStatus[0] = '\0';
    serverLocationIp[0] = '\0';
    std::memset(mapBitmap, 0, sizeof(mapBitmap));
    copyText(mapStatus, sizeof(mapStatus), localized("Mark home first", "请先标记家"));
    copyText(status, sizeof(status), localized("Tap Mark Home", "点击标记家"));
    saveHomeState();
}

} // namespace

namespace FindHomePage {

void setContentUrl(const char *url) { copyText(configuredHost, sizeof(configuredHost), url); }

void open() {
    requestPending = false;
    loadHomeState();
    currentMeasured = false;
    copyText(status, sizeof(status), homeSet
        ? localized("Tap Measure Current", "点击测量当前位置")
        : localized("Tap Mark Home", "点击标记家"));
    mapReady = false;
    copyText(mapStatus, sizeof(mapStatus), homeSet
        ? localized("Measure current to show path", "测量当前位置后显示路线")
        : localized("Mark home first", "请先标记家"));
}

bool resetControlAt(int16_t x, int16_t y) { return inRect(x, y, RESET_X, RESET_Y, RESET_W, RESET_H); }

bool actionControlAt(int16_t x, int16_t y) { return inRect(x, y, ACTION_X, ACTION_Y, ACTION_W, ACTION_H); }

bool handleTap(int16_t x, int16_t y) {
    if (resetControlAt(x, y)) {
        if (!requestPending) resetHome();
        return true;
    }
    if (!actionControlAt(x, y) || requestPending) return false;
    requestPending = true;
    const bool succeeded = postFootprint();
    if (succeeded) loadMap();
    requestPending = false;
    if (!succeeded && !message[0]) copyText(message, sizeof(message), status);
    if (succeeded && currentMeasured && !std::isnan(distanceKm)) {
        if (UiLocalization::isChinese())
            snprintf(status, sizeof(status), "当前位置到家：%.1f 公里",
                     static_cast<double>(distanceKm));
        else
            snprintf(status, sizeof(status), "CURRENT TO HOME: %.1f KM",
                     static_cast<double>(distanceKm));
    } else if (succeeded) {
        status[0] = '\0';
    }
    return true;
}

void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    drawMap(frame);
    drawTextButton(frame, RESET_X, RESET_Y, RESET_W, RESET_H,
                   localized("RESET", "重置家"));
    roundedFrame(frame, ACTION_X, ACTION_Y, ACTION_W, ACTION_H);
    const char *label = homeSet
        ? localized("MEASURE CURRENT", "测量当前位置")
        : localized("MARK HOME", "标记家");
    const int labelWidth = UiLocalization::textWidth(label, 1);
    UiLocalization::drawText(frame, ACTION_X + (ACTION_W - labelWidth) / 2,
                             ACTION_Y + (UiLocalization::isChinese() ? 17 : 22), label, 1);
    if (currentMeasured && !std::isnan(distanceKm)) {
        char distance[64] = {};
        if (UiLocalization::isChinese())
            snprintf(distance, sizeof(distance), "当前位置到家：%.1f 公里",
                     static_cast<double>(distanceKm));
        else
            snprintf(distance, sizeof(distance), "CURRENT TO HOME: %.1f KM",
                     static_cast<double>(distanceKm));
        drawText(frame, 307, distance, 1);
    }
}

} // namespace FindHomePage