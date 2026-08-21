#include "pages/chat/chat_page.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SD_MMC.h>
#include <esp_mac.h>

#include <cstring>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/ml307/ml307.h"
#include "devices/sd_card/sd_card.h"
#include "pages/recording/recording_page.h"
#include "ui/loading_indicator.h"
#include "ui/localization.h"

namespace {
constexpr char CHAT_API_URL[] = "http://100.96.7.15:3001";
constexpr char CHAT_PAIR_URL[] = "https://ultraman.tail34ff26.ts.net:8443";
constexpr char DEFAULT_CHAT_ROOM[] = "客厅";
constexpr char DEFAULT_CHAT_DEVICE_NAME[] = "客厅屏";
constexpr uint32_t CHAT_PRESENCE_INTERVAL_MS = 30000;
constexpr uint32_t CHAT_POLL_INTERVAL_MS = 5000;
constexpr uint32_t CHAT_AUTH_RETRY_MS = 30000;
constexpr uint32_t CHAT_TASK_STACK_BYTES = 6 * 1024;
char contentUrl[128] = {};
char chatToken[768] = {};
char chatSecret[64] = {};
char chatRoom[48] = {};
char chatDeviceName[48] = {};
char chatMac[18] = {};
char chatPairingUrl[192] = {};
bool chatAuthenticated = false;
bool chatCredentialsLoaded = false;
bool chatUiDirty = false;
uint32_t nextPresenceMs = 0;
uint32_t nextPollMs = 0;
TaskHandle_t chatTaskHandle = nullptr;
StaticTask_t chatTaskBuffer;
StackType_t chatTaskStack[CHAT_TASK_STACK_BYTES / sizeof(StackType_t)] = {};
Preferences chatPreferences;
char status[96] = "Tap the microphone to record";
char authStatusText[64] = "Not connected";
char pendingPath[96] = {};
bool uploadPending = false;
uint32_t messageId = 0;
constexpr char CHAT_WAV_PATH[] = "/chat/latest.wav";
constexpr uint8_t MAX_VISIBLE_MESSAGES = 4;
constexpr int MESSAGE_X = 8;
constexpr int MESSAGE_Y = 40;
constexpr int MESSAGE_W = 224;
constexpr int MESSAGE_H = 294;
constexpr int RECORD_X = 12;
constexpr int RECORD_Y = 348;
constexpr int RECORD_W = 216;
constexpr int RECORD_H = 52;

struct ChatMessage {
    uint32_t id = 0;
    bool deviceMessage = false;
    bool audio = false;
    char sender[24] = {};
    char text[72] = {};
    char time[6] = {};
};

ChatMessage messages[MAX_VISIBLE_MESSAGES] = {};
uint8_t messageCount = 0;
uint32_t playingMessageId = 0;

void loadMessages();
bool playMessage(uint32_t id);

void copyDisplayText(char *destination, size_t capacity, const char *source,
                     size_t maxBytes) {
    if (!destination || capacity == 0) return;
    const size_t limit = min(capacity - 1, maxBytes);
    size_t length = min(source ? std::strlen(source) : 0U, limit);
    while (length > 0 && source &&
           (static_cast<uint8_t>(source[length]) & 0xC0U) == 0x80U) --length;
    if (length > 0 && source) std::memcpy(destination, source, length);
    destination[length] = '\0';
}

void copyText(char *dst, size_t size, const char *src) {
    if (!dst || size == 0) return;
    std::strncpy(dst, src ? src : "", size - 1);
    dst[size - 1] = '\0';
}

String endpoint(const char *suffix) {
    String base(contentUrl);
    base.trim();
    while (base.endsWith("/")) base.remove(base.length() - 1);
    const int scheme = base.indexOf("://");
    if (scheme >= 0) {
        const int path = base.indexOf('/', scheme + 3);
        if (path >= 0) base.remove(path);
    }
    return base + suffix;
}

String chatEndpoint(const char *suffix) {
    return String(CHAT_API_URL) + (suffix ? suffix : "");
}

String deviceMac() {
    uint8_t bytes[6] = {};
    if (esp_read_mac(bytes, ESP_MAC_WIFI_STA) != ESP_OK)
        return WiFi.macAddress();
    char mac[18] = {};
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return String(mac);
}

void loadChatCredentials() {
    if (chatCredentialsLoaded) return;
    chatCredentialsLoaded = true;
    copyText(chatRoom, sizeof(chatRoom), DEFAULT_CHAT_ROOM);
    copyText(chatDeviceName, sizeof(chatDeviceName), DEFAULT_CHAT_DEVICE_NAME);
    if (chatPreferences.begin("chat-auth", true)) {
        copyText(chatToken, sizeof(chatToken), chatPreferences.getString("token", "").c_str());
        copyText(chatSecret, sizeof(chatSecret), chatPreferences.getString("secret", "").c_str());
        copyText(chatRoom, sizeof(chatRoom),
                 chatPreferences.getString("room", DEFAULT_CHAT_ROOM).c_str());
        copyText(chatDeviceName, sizeof(chatDeviceName),
                 chatPreferences.getString("name", DEFAULT_CHAT_DEVICE_NAME).c_str());
        chatPreferences.end();
    }
    if (!chatSecret[0]) {
        snprintf(chatSecret, sizeof(chatSecret), "%08lX%08lX%08lX%08lX",
                 static_cast<unsigned long>(esp_random()),
                 static_cast<unsigned long>(esp_random()),
                 static_cast<unsigned long>(esp_random()),
                 static_cast<unsigned long>(esp_random()));
        if (chatPreferences.begin("chat-auth", false)) {
            chatPreferences.putString("secret", chatSecret);
            chatPreferences.putString("room", chatRoom);
            chatPreferences.putString("name", chatDeviceName);
            chatPreferences.end();
        }
    }
}

void saveChatToken(const char *token) {
    copyText(chatToken, sizeof(chatToken), token);
    if (chatPreferences.begin("chat-auth", false)) {
        chatPreferences.putString("token", chatToken);
        chatPreferences.end();
    }
}

void addAuthHeader(HTTPClient &http) {
    if (chatToken[0]) {
        String header = "Bearer ";
        header += chatToken;
        http.addHeader("Authorization", header);
    }
}

bool parseAuthResponse(const String &payload) {
    JsonDocument json;
    if (deserializeJson(json, payload)) return false;
    const char *token = json["token"] | json["access_token"] |
                        json["data"]["token"] | "";
    if (!token[0]) return false;
    saveChatToken(token);
    chatAuthenticated = true;
    return true;
}

bool cellularJsonRequest(const char *method, const String &url, const String &body,
                         String &responseBody, int &responseCode) {
    if (!url.startsWith("http://") || !cellularModem.isConnected()) return false;
    constexpr int schemeLength = 7;
    const int pathStart = url.indexOf('/', schemeLength);
    String authority = pathStart >= 0 ? url.substring(schemeLength, pathStart)
                                      : url.substring(schemeLength);
    const String path = pathStart >= 0 ? url.substring(pathStart) : "/";
    uint16_t port = 80;
    const int colon = authority.lastIndexOf(':');
    if (colon > 0) {
        port = static_cast<uint16_t>(authority.substring(colon + 1).toInt());
        authority.remove(colon);
    }
    Ml307TcpClient client(cellularModem);
    if (authority.isEmpty() || port == 0 || !client.connect(authority.c_str(), port)) return false;
    String request;
    request.reserve(384 + body.length());
    request += method;
    request += ' ';
    request += path;
    request += " HTTP/1.1\r\nHost: ";
    request += authority;
    request += "\r\nContent-Type: application/json\r\nConnection: close\r\n";
    if (chatToken[0] && url.indexOf("/api/auth/") < 0) {
        request += "Authorization: Bearer ";
        request += chatToken;
        request += "\r\n";
    }
    request += "Content-Length: ";
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
    while (millis() - started < 30000) {
        while (client.available() > 0) {
            const int value = client.read();
            if (value < 0) break;
            response += static_cast<char>(value);
            lastData = millis();
        }
        if (!client.connected() && client.available() == 0) break;
        if (millis() - lastData > 10000) break;
        delay(2);
    }
    client.stop();
    const int firstSpace = response.indexOf(' ');
    responseCode = firstSpace >= 0 ? response.substring(firstSpace + 1).toInt() : 0;
    const int bodyStart = response.indexOf("\r\n\r\n");
    responseBody = bodyStart >= 0 ? response.substring(bodyStart + 4) : String();
    return responseCode > 0;
}

bool chatJsonRequest(const char *method, const char *path, const String &body,
                     String &response, int &code) {
    HTTPClient http;
    WiFiClientSecure secure;
    secure.setInsecure();
    http.setConnectTimeout(7000);
    http.setTimeout(15000);
    const String url = chatEndpoint(path);
    if (WiFi.status() != WL_CONNECTED)
        return cellularJsonRequest(method, url, body, response, code);
    if (!http.begin(secure, url)) return false;
    http.addHeader("Content-Type", "application/json");
    if (!String(path).startsWith("/api/auth/")) addAuthHeader(http);
    code = method && std::strcmp(method, "GET") == 0
        ? http.GET() : http.sendRequest(method, body);
    response = code > 0 ? http.getString() : String();
    http.end();
    return code > 0;
}

bool authenticateDevice(bool registerDevice) {
    JsonDocument body;
    body["mac"] = deviceMac();
    body["secret"] = chatSecret;
    body["name"] = chatDeviceName;
    body["room"] = chatRoom;
    String request;
    serializeJson(body, request);
    String response;
    int code = 0;
    const char *path = registerDevice ? "/api/auth/device/register" : "/api/auth/device/login";
    if (chatJsonRequest("POST", path, request, response, code) &&
        code >= 200 && code < 300 && parseAuthResponse(response)) {
        Serial.printf("[CHAT AUTH] device %s ok room=%s\n",
                      registerDevice ? "registered" : "login", chatRoom);
        copyText(authStatusText, sizeof(authStatusText), "Connected");
        return true;
    }
    if (registerDevice && code == 409 &&
        chatJsonRequest("POST", "/api/auth/device/login", request, response, code) &&
        code >= 200 && code < 300 && parseAuthResponse(response)) {
        Serial.printf("[CHAT AUTH] device login after register conflict room=%s\n", chatRoom);
        copyText(authStatusText, sizeof(authStatusText), "Connected");
        return true;
    }
    chatAuthenticated = false;
    Serial.printf("[CHAT AUTH] device %s failed code=%d\n",
                  registerDevice ? "register" : "login", code);
    copyText(authStatusText, sizeof(authStatusText), "Login failed");
    return false;
}

void pixel(uint8_t *f, int x, int y) {
    if (x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    f[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |= 0x80U >> (x % 8);
}
void rect(uint8_t *f, int x, int y, int w, int h) {
    for (int i = 0; i < w; ++i) { pixel(f, x + i, y); pixel(f, x + i, y + h - 1); }
    for (int i = 0; i < h; ++i) { pixel(f, x, y + i); pixel(f, x + w - 1, y + i); }
}
bool inRect(int16_t x, int16_t y, int l, int t, int w, int h) {
    return x >= l && x < l + w && y >= t && y < t + h;
}
bool uploadWifi(const String &url, File &file, size_t length, String &response) {
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    secure.setInsecure();
    http.setConnectTimeout(7000);
    http.setTimeout(65000);
    const bool began = url.startsWith("https://") ? http.begin(secure, url) : http.begin(plain, url);
    if (!began) return false;
    http.addHeader("Content-Type", "audio/wav");
    addAuthHeader(http);
    http.addHeader("Connection", "close");
    const int code = http.sendRequest("POST", &file, length);
    if (code >= 200 && code < 300) response = http.getString();
    http.end();
    return code >= 200 && code < 300;
}

bool uploadCellular(const String &url, File &file, size_t length, String &responseBody) {
    if (!url.startsWith("http://") || !cellularModem.isConnected()) return false;
    constexpr int schemeLength = 7;
    const int pathStart = url.indexOf('/', schemeLength);
    String authority = pathStart >= 0 ? url.substring(schemeLength, pathStart)
                                      : url.substring(schemeLength);
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
    String header;
    header.reserve(256);
    header += "POST ";
    header += path;
    header += " HTTP/1.1\r\nHost: ";
    header += authority;
    header += "\r\nContent-Type: audio/wav\r\nAuthorization: Bearer ";
    header += chatToken;
    header += "\r\nConnection: close\r\nContent-Length: ";
    header += String(length);
    header += "\r\n\r\n";
    if (client.write(reinterpret_cast<const uint8_t *>(header.c_str()), header.length()) !=
        header.length()) {
        client.stop();
        return false;
    }

    uint8_t buffer[1024] = {};
    size_t sent = 0;
    while (sent < length) {
        const size_t bytes = file.read(buffer, min(sizeof(buffer), length - sent));
        if (bytes == 0 || client.write(buffer, bytes) != bytes) {
            client.stop();
            return false;
        }
        sent += bytes;
        delay(1);
    }

    String response;
    const uint32_t started = millis();
    uint32_t lastData = started;
    while (millis() - started < 65000) {
        while (client.available() > 0) {
            const int value = client.read();
            if (value < 0) break;
            response += static_cast<char>(value);
            lastData = millis();
        }
        if (!client.connected() && client.available() == 0) break;
        if (millis() - lastData > 10000) break;
        delay(2);
    }
    client.stop();
    const int firstSpace = response.indexOf(' ');
    const int code = firstSpace >= 0 ? response.substring(firstSpace + 1).toInt() : 0;
    const int bodyStart = response.indexOf("\r\n\r\n");
    if (bodyStart >= 0) responseBody = response.substring(bodyStart + 4);
    Serial.printf("[CHAT] 4G upload code=%d sent=%u/%u response=%u\n",
                  code, static_cast<unsigned>(sent), static_cast<unsigned>(length),
                  static_cast<unsigned>(responseBody.length()));
    return code >= 200 && code < 300;
}

bool uploadFile() {
    File file = SD_MMC.open(pendingPath, FILE_READ);
    if (!file) { copyText(status, sizeof(status), "WAV file unavailable"); return false; }
    const size_t length = file.size();
    String response;
    const String url = chatEndpoint("/api/chat/messages");
    bool ok = false;
    if (WiFi.status() == WL_CONNECTED) ok = uploadWifi(url, file, length, response);
    else ok = uploadCellular(url, file, length, response);
    file.close();
    if (!ok) { copyText(status, sizeof(status), "Chat upload failed"); return false; }
    JsonDocument json;
    if (deserializeJson(json, response) || !(json["ok"] | false)) {
        copyText(status, sizeof(status), "Invalid upload response");
        return false;
    }
    messageId = json["id"] | 0;
    if (messageId == 0) {
        copyText(status, sizeof(status), "Upload ID missing");
        return false;
    }
    snprintf(status, sizeof(status), "WAV sent (%lu bytes)", static_cast<unsigned long>(length));
    Serial.printf("[CHAT] uploaded bytes=%u id=%lu status=%s fmt=%s ctype=%s\n",
                  static_cast<unsigned>(length), static_cast<unsigned long>(messageId),
                  json["status"] | "", json["fmt"] | "", json["ctype"] | "");
    loadMessages();
    return true;
}

bool httpGetText(const String &url, String &payload) {
    if (WiFi.status() != WL_CONNECTED) {
        int code = 0;
        return cellularJsonRequest("GET", url, String(), payload, code) &&
               code >= 200 && code < 300;
    }
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    secure.setInsecure();
    http.setConnectTimeout(7000);
    http.setTimeout(20000);
    addAuthHeader(http);
    const bool began = url.startsWith("https://") ? http.begin(secure, url) : http.begin(plain, url);
    if (!began) return false;
    const int code = http.GET();
    if (code >= 200 && code < 300) payload = http.getString();
    http.end();
    if (code == 401) chatAuthenticated = false;
    return code >= 200 && code < 300;
}

void copyMessageText(char *destination, size_t capacity, JsonVariant value) {
    const char *text = value | "";
    copyText(destination, capacity, text);
}

void loadMessages() {
    if (!chatAuthenticated) return;
    String payload;
    if (!httpGetText(chatEndpoint("/api/chat/messages?for=esp32"), payload)) {
        copyText(status, sizeof(status), "Chat refresh failed");
        return;
    }
    JsonDocument json;
    if (deserializeJson(json, payload)) {
        copyText(status, sizeof(status), "Invalid chat response");
        return;
    }
    JsonArray rows = json["rows"].as<JsonArray>();
    messageCount = 0;
    if (rows.isNull()) return;
    const size_t total = rows.size();
    const size_t start = total > MAX_VISIBLE_MESSAGES ? total - MAX_VISIBLE_MESSAGES : 0;
    size_t index = 0;
    for (JsonObject row : rows) {
        if (index++ < start) continue;
        if (messageCount >= MAX_VISIBLE_MESSAGES) break;
        ChatMessage &message = messages[messageCount++];
        message.id = row["id"] | 0;
        const char *direction = row["direction"] | "";
        const char *rowSender = row["sender"] | "";
        message.deviceMessage = std::strcmp(direction, "esp32") == 0 ||
                                strcasecmp(rowSender, "esp32") == 0;
        char sender[48] = {};
        char text[128] = {};
        copyMessageText(sender, sizeof(sender), row["sender"]);
        copyMessageText(text, sizeof(text), row["text"]);
        copyDisplayText(message.sender, sizeof(message.sender), sender, 12);
        copyDisplayText(message.text, sizeof(message.text), text, 24);
        const char *audioPath = row["audio_path"] | "";
        message.audio = audioPath[0] != '\0';
        const char *created = row["created_at"] | "";
        if (std::strlen(created) >= 16) {
            message.time[0] = created[11];
            message.time[1] = created[12];
            message.time[2] = ':';
            message.time[3] = created[14];
            message.time[4] = created[15];
            message.time[5] = '\0';
        }
    }
    copyText(status, sizeof(status), "Messages updated");
}

bool postChatJson(const char *path, JsonDocument &body, String &response) {
    String request;
    serializeJson(body, request);
    int code = 0;
    if (!chatJsonRequest("POST", path, request, response, code)) return false;
    if (code == 401) {
        chatAuthenticated = false;
        chatToken[0] = '\0';
        return false;
    }
    return code >= 200 && code < 300;
}

void sendPresence() {
    JsonDocument body;
    body["online"] = true;
    String response;
    if (postChatJson("/api/chat/presence", body, response)) {
        Serial.printf("[CHAT] presence online room=%s\n", chatRoom);
    }
}

void acknowledgeMessage(uint32_t id) {
    if (!id) return;
    JsonDocument body;
    body["id"] = id;
    String response;
    if (!postChatJson("/api/chat/esp32/ack", body, response))
        Serial.printf("[CHAT] ack failed id=%lu\n", static_cast<unsigned long>(id));
}

void pollDeviceMessages() {
    String payload;
    if (!httpGetText(chatEndpoint("/api/chat/messages?for=esp32"), payload)) return;
    JsonDocument json;
    if (deserializeJson(json, payload)) return;
    JsonArray rows = json["rows"].as<JsonArray>();
    if (rows.isNull()) return;
    for (JsonObject row : rows) {
        const uint32_t id = row["id"] | 0;
        const char *direction = row["direction"] | "";
        const char *sender = row["sender"] | "";
        const char *text = row["text"] | "";
        const char *audioPath = row["audio_path"] | "";
        const bool fromDevice = std::strcmp(direction, "esp32") == 0 ||
                                strcasecmp(sender, "esp32") == 0 ||
                                std::strcmp(sender, chatDeviceName) == 0;
        if (fromDevice || !id) continue;
        bool handled = false;
        if (audioPath[0]) handled = playMessage(id);
        else if (text[0]) {
            Serial.printf("[CHAT] user text id=%lu text=%s\n",
                          static_cast<unsigned long>(id), text);
            handled = true;
        }
        if (handled) acknowledgeMessage(id);
    }
}

uint32_t latestAudioId(JsonVariant value) {
    uint32_t latest = 0;
    if (value.is<JsonArray>()) {
        for (JsonVariant item : value.as<JsonArray>()) latest = max(latest, latestAudioId(item));
        return latest;
    }
    if (!value.is<JsonObject>()) return 0;
    JsonObject object = value.as<JsonObject>();
    const char *type = object["type"] | object["content_type"] | object["mime"] | "";
    const char *audioUrl = object["audio_path"] | object["audio_url"] | object["url"] | "";
    const bool audio = std::strstr(type, "audio") || std::strstr(type, "wav") ||
                       (audioUrl && audioUrl[0] != '\0');
    if (audio) latest = object["id"] | object["message_id"] | 0;
    for (JsonPair pair : object) {
        if (pair.value().is<JsonArray>() || pair.value().is<JsonObject>()) {
            latest = max(latest, latestAudioId(pair.value()));
        }
    }
    return latest;
}

bool playMessage(uint32_t id) {
    if (id == 0) return false;
    const String audioUrl = chatEndpoint(("/api/chat/audio/" + String(id)).c_str());
    if (!SdCard::downloadFile(audioUrl.c_str(), CHAT_WAV_PATH, 44, chatToken)) {
        copyText(status, sizeof(status), "WAV download failed");
        return false;
    }
    if (!RecordingPage::playWavFile(CHAT_WAV_PATH)) {
        copyText(status, sizeof(status), "WAV playback failed");
        return false;
    }
    playingMessageId = id;
    snprintf(status, sizeof(status), "Playing message %lu", static_cast<unsigned long>(id));
    return true;
}
}

namespace ChatPage {
void setContentUrl(const char *url) { copyText(contentUrl, sizeof(contentUrl), url); }
void open() {
    loadChatCredentials();
    uploadPending = false;
    messageCount = 0;
    copyText(status, sizeof(status), "Tap the microphone to record");
}
void serviceNetwork() {
    loadChatCredentials();
    const uint32_t now = millis();
    if (!chatAuthenticated) {
        if ((WiFi.status() == WL_CONNECTED || cellularModem.isConnected()) &&
            (nextPresenceMs == 0 || static_cast<int32_t>(now - nextPresenceMs) >= 0)) {
            if (!authenticateDevice(false)) authenticateDevice(true);
            nextPresenceMs = now + CHAT_AUTH_RETRY_MS;
        }
        return;
    }
    if (static_cast<int32_t>(now - nextPresenceMs) >= 0) {
        sendPresence();
        nextPresenceMs = now + CHAT_PRESENCE_INTERVAL_MS;
    }
    if (static_cast<int32_t>(now - nextPollMs) >= 0) {
        pollDeviceMessages();
        nextPollMs = now + CHAT_POLL_INTERVAL_MS;
        if (chatUiDirty) {
            chatUiDirty = false;
        }
    }
}

void chatTask(void *) {
    for (;;) {
        serviceNetwork();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
void service() {
    if (chatTaskHandle) return;
    chatTaskHandle = xTaskCreateStaticPinnedToCore(
        chatTask, "chat-network",
        sizeof(chatTaskStack) / sizeof(chatTaskStack[0]), nullptr, 1,
        chatTaskStack, &chatTaskBuffer, 0);
    if (!chatTaskHandle) Serial.println("[CHAT] Network worker start failed");
}
bool sendReply(const char *text) {
    if (!text || !text[0] || !chatAuthenticated) return false;
    JsonDocument body;
    body["text"] = text;
    String response;
    return postChatJson("/api/chat/esp32/reply", body, response);
}
bool authenticateFromSettings(bool registerDevice) {
    loadChatCredentials();
    if (WiFi.status() != WL_CONNECTED && !cellularModem.isConnected()) {
        copyText(status, sizeof(status), "Network unavailable");
        copyText(authStatusText, sizeof(authStatusText), "Network unavailable");
        return false;
    }
    copyText(status, sizeof(status), registerDevice ? "Registering device..." : "Logging in...");
    const bool authenticated = authenticateDevice(registerDevice);
    if (!authenticated) copyText(status, sizeof(status), "Device login failed");
    return authenticated;
}
bool isAuthenticated() { return chatAuthenticated; }
const char *roomName() { loadChatCredentials(); return chatRoom; }
const char *deviceMacAddress() {
    loadChatCredentials();
    copyText(chatMac, sizeof(chatMac), deviceMac().c_str());
    return chatMac;
}
const char *pairingUrl() {
    loadChatCredentials();
    copyText(chatMac, sizeof(chatMac), deviceMac().c_str());
    snprintf(chatPairingUrl, sizeof(chatPairingUrl), "%s/chat/%s?mac=%s",
             CHAT_PAIR_URL, chatRoom, chatMac);
    Serial.printf("[CHAT PAIR] room=%s mac=%s url=%s\n",
                  chatRoom, chatMac, chatPairingUrl);
    return chatPairingUrl;
}
const char *authStatus() { return authStatusText; }
bool recordControlAt(int16_t x, int16_t y) { return inRect(x, y, RECORD_X, RECORD_Y, RECORD_W, RECORD_H); }
bool messageControlBoundsAt(int16_t x, int16_t y, int16_t &left,
                            int16_t &top, int16_t &width, int16_t &height) {
    for (uint8_t index = 0; index < messageCount; ++index) {
        if (!messages[index].audio) continue;
        top = MESSAGE_Y + 8 + index * 62;
        left = messages[index].deviceMessage ? MESSAGE_X + 55 : MESSAGE_X + 7;
        width = 162;
        height = 40;
        if (inRect(x, y, left, top, width, height)) return true;
    }
    return false;
}
bool handleTap(int16_t x, int16_t y) {
    if (!chatAuthenticated) return false;
    if (recordControlAt(x, y)) {
        if (RecordingPage::isRecording()) {
            RecordingPage::stopChatRecording();
            copyText(status, sizeof(status), "Sending...");
        } else if (RecordingPage::startChatRecording()) {
            copyText(status, sizeof(status), "Recording");
        } else copyText(status, sizeof(status), "Microphone unavailable");
        return true;
    }
    int16_t left = 0, top = 0, width = 0, height = 0;
    if (messageControlBoundsAt(x, y, left, top, width, height)) {
        const uint8_t index = static_cast<uint8_t>((top - MESSAGE_Y - 8) / 62);
        const uint32_t selectedId = messages[index].id;
        if (RecordingPage::isPlayingWav() && playingMessageId == selectedId) {
            RecordingPage::stop();
            playingMessageId = 0;
        } else {
            if (RecordingPage::isPlayingWav()) RecordingPage::stop();
            playMessage(selectedId);
        }
        return true;
    }
    return false;
}
bool process() {
    if (!uploadPending && RecordingPage::takeChatRecording(pendingPath, sizeof(pendingPath))) {
        uploadPending = true;
        uploadFile();
        uploadPending = false;
        return true;
    }
    return false;
}
void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    rect(frame, MESSAGE_X, MESSAGE_Y, MESSAGE_W, MESSAGE_H);
    if (!chatAuthenticated) {
        UiLocalization::drawCentered(frame, 166, "CHAT NOT CONNECTED", 1);
        UiLocalization::drawCentered(frame, 194, "OPEN SETTINGS", 1);
    }
    for (uint8_t index = 0; index < messageCount; ++index) {
        const ChatMessage &message = messages[index];
        const int top = MESSAGE_Y + 8 + index * 62;
        const int left = message.deviceMessage ? MESSAGE_X + 55 : MESSAGE_X + 7;
        constexpr int width = 162;
        rect(frame, left, top, width, 40);
        if (message.audio && RecordingPage::isPlayingWav() &&
            playingMessageId == message.id) {
            rect(frame, left + 2, top + 2, width - 4, 36);
        }
        const char *sender = message.sender[0]
            ? message.sender
            : (message.deviceMessage ? "ESP32" : "YOU");
        UiLocalization::drawText(frame, left + 5, top + 3, sender, 1);
        if (message.time[0])
            UiLocalization::drawText(frame, left + width - 34, top + 3, message.time, 1);
        const char *content = message.audio
            ? (UiLocalization::isChinese() ? "语音消息" : "VOICE MESSAGE")
            : message.text;
        if (!content[0]) content = UiLocalization::isChinese() ? "空消息" : "EMPTY MESSAGE";
        UiLocalization::drawText(frame, left + 5, top + 21, content, 1);
    }
    const bool recording = RecordingPage::isRecording();
    rect(frame, RECORD_X, RECORD_Y, RECORD_W, RECORD_H);
    if (recording) {
        for (int y = RECORD_Y + 16; y < RECORD_Y + 36; ++y)
            for (int x = RECORD_X + 10; x < RECORD_X + 30; ++x) pixel(frame, x, y);
    } else {
        const int centerX = RECORD_X + 20;
        const int centerY = RECORD_Y + 24;
        for (int y = -10; y <= 6; ++y)
            for (int x = -6; x <= 6; ++x)
                if (x * x + y * y <= 42) pixel(frame, centerX + x, centerY + y);
        for (int y = centerY + 5; y <= centerY + 12; ++y) pixel(frame, centerX, y);
        for (int x = centerX - 6; x <= centerX + 6; ++x) pixel(frame, x, centerY + 12);
    }
    const char *recordLabel = recording
        ? (UiLocalization::isChinese() ? "停止录音" : "STOP RECORD")
        : (UiLocalization::isChinese() ? "开始录音" : "START RECORD");
    UiLocalization::drawText(frame, RECORD_X + 38,
                             RECORD_Y + (UiLocalization::isChinese() ? 17 : 22),
                             recordLabel, 1);
}
}