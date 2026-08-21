#include "pages/chat/chat_page.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <esp_mac.h>
#include <qrcode.h>

#include <cstring>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/audio/opus_player.h"
#include "devices/ml307/ml307.h"
#include "devices/sd_card/sd_card.h"
#include "font/xiaozhi_font.h"
#include "pages/recording/recording_page.h"
#include "ui/loading_indicator.h"
#include "ui/localization.h"

namespace {
constexpr char CHAT_HTTP_API_URL[] = "http://100.96.7.15:3001";
constexpr char CHAT_PAIR_URL[] = "https://ultraman.tail34ff26.ts.net:8443";
constexpr char DEFAULT_CHAT_ROOM[] = "客厅";
constexpr char DEFAULT_CHAT_DEVICE_NAME[] = "客厅屏";
constexpr uint32_t CHAT_POLL_INTERVAL_MS = 5000;
constexpr uint32_t CHAT_POLL_TASK_STACK_BYTES = 4 * 1024;
char contentUrl[128] = {};
char chatRoom[48] = {};
char chatDeviceName[48] = {};
char chatMac[18] = {};
char chatPairingUrl[192] = {};
bool chatCredentialsLoaded = false;
volatile bool chatUiDirty = false;
bool pairingPopupOpen = false;
TaskHandle_t chatPollTaskHandle = nullptr;
volatile bool chatPageOpen = false;
volatile bool chatPollStopRequested = false;
portMUX_TYPE chatDataMux = portMUX_INITIALIZER_UNLOCKED;
Preferences chatPreferences;
char status[96] = "Tap the microphone to record";
char pendingPath[96] = {};
bool uploadPending = false;
uint32_t messageId = 0;
constexpr char CHAT_OPUS_PATH[] = "/chat/latest.ogg";
constexpr uint8_t MAX_VISIBLE_MESSAGES = 4;
constexpr int MESSAGE_X = 8;
constexpr int MESSAGE_Y = 76;
constexpr int MESSAGE_W = 224;
constexpr int MESSAGE_H = 258;
constexpr int PAIRING_X = 154;
constexpr int PAIRING_Y = 40;
constexpr int PAIRING_W = 76;
constexpr int PAIRING_H = 28;
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

uint8_t snapshotMessages(ChatMessage *destination, uint8_t capacity) {
    if (!destination || capacity == 0) return 0;
    portENTER_CRITICAL(&chatDataMux);
    const uint8_t count = min(messageCount, capacity);
    std::memcpy(destination, messages, static_cast<size_t>(count) * sizeof(ChatMessage));
    portEXIT_CRITICAL(&chatDataMux);
    return count;
}

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
    return String(CHAT_HTTP_API_URL) + (suffix ? suffix : "");
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
    if (chatPreferences.begin("chat-pairing", true)) {
        copyText(chatRoom, sizeof(chatRoom),
                 chatPreferences.getString("room", DEFAULT_CHAT_ROOM).c_str());
        copyText(chatDeviceName, sizeof(chatDeviceName),
                 chatPreferences.getString("name", DEFAULT_CHAT_DEVICE_NAME).c_str());
        chatPreferences.end();
    }
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
    request += "X-Device-Mac: ";
    request += deviceMac();
    request += "\r\n";
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
    WiFiClient plain;
    http.setConnectTimeout(7000);
    http.setTimeout(15000);
    const String url = chatEndpoint(path);
    if (WiFi.status() != WL_CONNECTED)
        return cellularJsonRequest(method, url, body, response, code);
    // Start with plain HTTP. Constructing WiFiClientSecure itself allocates and
    // throws std::bad_alloc on this no-PSRAM board when heap is fragmented.
    const bool began = !url.startsWith("https://") && http.begin(plain, url);
    if (!began) return false;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Device-Mac", deviceMac());
    code = method && std::strcmp(method, "GET") == 0
        ? http.GET() : http.sendRequest(method, body);
    response = code > 0 ? http.getString() : String();
    http.end();
    return code > 0;
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
    http.setConnectTimeout(7000);
    http.setTimeout(65000);
    const bool began = url.startsWith("http://") && http.begin(plain, url);
    if (!began) return false;
    http.addHeader("Content-Type", "audio/wav");
    http.addHeader("X-Device-Mac", deviceMac());
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
    header += "\r\nContent-Type: audio/wav\r\nX-Device-Mac: ";
    header += deviceMac();
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
    http.setConnectTimeout(7000);
    http.setTimeout(8000);
    http.addHeader("X-Device-Mac", deviceMac());
    const bool began = url.startsWith("http://") && http.begin(plain, url);
    if (!began) return false;
    const int code = http.GET();
    if (code >= 200 && code < 300) payload = http.getString();
    http.end();
    return code >= 200 && code < 300;
}

void copyMessageText(char *destination, size_t capacity, JsonVariant value) {
    const char *text = value | "";
    copyText(destination, capacity, text);
}

void loadMessages() {
    String payload;
    if (!httpGetText(chatEndpoint("/api/chat/messages"), payload)) {
        copyText(status, sizeof(status), "Chat refresh failed");
        return;
    }
    JsonDocument json;
    if (deserializeJson(json, payload)) {
        copyText(status, sizeof(status), "Invalid chat response");
        return;
    }
    JsonArray rows = json["rows"].as<JsonArray>();
    if (rows.isNull()) rows = json["messages"].as<JsonArray>();
    if (rows.isNull()) return;
    ChatMessage updatedMessages[MAX_VISIBLE_MESSAGES] = {};
    uint8_t updatedCount = 0;
    const size_t total = rows.size();
    const size_t start = total > MAX_VISIBLE_MESSAGES ? total - MAX_VISIBLE_MESSAGES : 0;
    size_t index = 0;
    for (JsonObject row : rows) {
        if (index++ < start) continue;
        if (updatedCount >= MAX_VISIBLE_MESSAGES) break;
        ChatMessage &message = updatedMessages[updatedCount++];
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
    portENTER_CRITICAL(&chatDataMux);
    const bool changed = messageCount != updatedCount ||
        std::memcmp(messages, updatedMessages, sizeof(messages)) != 0;
    if (changed) {
        std::memcpy(messages, updatedMessages, sizeof(messages));
        messageCount = updatedCount;
        chatUiDirty = true;
    }
    portEXIT_CRITICAL(&chatDataMux);
    copyText(status, sizeof(status), "Messages updated");
    if (changed) {
        Serial.printf("[CHAT SYNC] room=%s rows=%u visible=%u\n",
                      json["room"] | chatRoom, static_cast<unsigned>(total),
                      static_cast<unsigned>(updatedCount));
    }
}

bool postChatJson(const char *path, JsonDocument &body, String &response) {
    body["mac"] = deviceMac();
    String request;
    serializeJson(body, request);
    int code = 0;
    if (!chatJsonRequest("POST", path, request, response, code)) return false;
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
    if (!SdCard::downloadFile(audioUrl.c_str(), CHAT_OPUS_PATH, 64)) {
        copyText(status, sizeof(status), "Opus download failed");
        return false;
    }
    if (!SdCard::isValidOggOpus(CHAT_OPUS_PATH, 64)) {
        copyText(status, sizeof(status), "Unsupported audio format");
        Serial.printf("[CHAT] Message %lu is not Ogg Opus (WebM is unsupported)\n",
                      static_cast<unsigned long>(id));
        return false;
    }
    if (!OpusPlayer::play(CHAT_OPUS_PATH)) {
        copyText(status, sizeof(status), "Opus playback failed");
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
    chatPageOpen = true;
    chatPollStopRequested = false;
    pairingPopupOpen = false;
    uploadPending = false;
    portENTER_CRITICAL(&chatDataMux);
    messageCount = 0;
    std::memset(messages, 0, sizeof(messages));
    portEXIT_CRITICAL(&chatDataMux);
    copyText(status, sizeof(status), "Tap the microphone to record");
}
void close() {
    chatPageOpen = false;
    chatPollStopRequested = true;
    OpusPlayer::stop();
    const uint32_t started = millis();
    while (chatPollTaskHandle && millis() - started < 10000) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    OpusPlayer::stop();
}
void chatPollTask(void *) {
    TickType_t lastWake = xTaskGetTickCount();
    while (!chatPollStopRequested) {
        if (WiFi.status() == WL_CONNECTED || cellularModem.isConnected()) {
            pollDeviceMessages();
            if (chatPollStopRequested) break;
            loadMessages();
        }
        xTaskDelayUntil(&lastWake, pdMS_TO_TICKS(CHAT_POLL_INTERVAL_MS));
    }
    chatPollTaskHandle = nullptr;
    vTaskDelete(nullptr);
}
void service() {
    if (chatPageOpen && !chatPollTaskHandle) {
        chatPollStopRequested = false;
    }
    if (chatPageOpen && !chatPollTaskHandle &&
        xTaskCreatePinnedToCore(chatPollTask, "chat-poll",
                                CHAT_POLL_TASK_STACK_BYTES, nullptr, 1,
                                &chatPollTaskHandle, 0) != pdPASS) {
        chatPollTaskHandle = nullptr;
    }
    if (chatPageOpen && !chatPollTaskHandle) {
        static uint32_t lastFailureLogMs = 0;
        if (millis() - lastFailureLogMs >= CHAT_POLL_INTERVAL_MS) {
            lastFailureLogMs = millis();
            Serial.printf("[CHAT] Poll worker start deferred free=%u largest=%u\n",
                          static_cast<unsigned>(ESP.getFreeHeap()),
                          static_cast<unsigned>(ESP.getMaxAllocHeap()));
        }
    }
}
bool sendReply(const char *text) {
    if (!text || !text[0]) return false;
    JsonDocument body;
    body["text"] = text;
    String response;
    return postChatJson("/api/chat/esp32/reply", body, response);
}
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
bool pairingControlAt(int16_t x, int16_t y) {
    return !pairingPopupOpen && inRect(x, y, PAIRING_X, PAIRING_Y, PAIRING_W, PAIRING_H);
}
bool pairingReturnControlAt(int16_t x, int16_t y) {
    return pairingPopupOpen && inRect(x, y, 18, 350, 204, 42);
}
bool recordControlAt(int16_t x, int16_t y) {
    return !pairingPopupOpen && inRect(x, y, RECORD_X, RECORD_Y, RECORD_W, RECORD_H);
}
bool messageControlBoundsAt(int16_t x, int16_t y, int16_t &left,
                            int16_t &top, int16_t &width, int16_t &height) {
    if (pairingPopupOpen) return false;
    ChatMessage snapshot[MAX_VISIBLE_MESSAGES] = {};
    const uint8_t count = snapshotMessages(snapshot, MAX_VISIBLE_MESSAGES);
    for (uint8_t index = 0; index < count; ++index) {
        if (!snapshot[index].audio) continue;
        top = MESSAGE_Y + 8 + index * 62;
        left = messages[index].deviceMessage ? MESSAGE_X + 55 : MESSAGE_X + 7;
        width = 162;
        height = 40;
        if (inRect(x, y, left, top, width, height)) return true;
    }
    return false;
}
bool handleTap(int16_t x, int16_t y) {
    if (pairingControlAt(x, y)) {
        pairingPopupOpen = true;
        pairingUrl();
        return true;
    }
    if (pairingReturnControlAt(x, y)) {
        pairingPopupOpen = false;
        return true;
    }
    if (pairingPopupOpen) return false;
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
        ChatMessage snapshot[MAX_VISIBLE_MESSAGES] = {};
        const uint8_t count = snapshotMessages(snapshot, MAX_VISIBLE_MESSAGES);
        if (index >= count) return false;
        const uint32_t selectedId = snapshot[index].id;
        if (OpusPlayer::isPlaying() && playingMessageId == selectedId) {
            OpusPlayer::stop();
            playingMessageId = 0;
        } else {
            if (OpusPlayer::isPlaying()) OpusPlayer::stop();
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
    if (chatUiDirty) {
        portENTER_CRITICAL(&chatDataMux);
        chatUiDirty = false;
        portEXIT_CRITICAL(&chatDataMux);
        return true;
    }
    return false;
}
void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    if (pairingPopupOpen) {
        rect(frame, 8, 36, 224, 364);
        rect(frame, 10, 38, 220, 360);
        UiLocalization::drawCentered(frame, 48,
            UiLocalization::isChinese() ? "聊天室配对" : "CHAT ROOM PAIRING", 1);
        constexpr uint8_t QR_VERSION = 5;
        uint8_t qrBuffer[qrcode_getBufferSize(QR_VERSION)] = {};
        QRCode qr = {};
        if (chatPairingUrl[0] &&
            qrcode_initText(&qr, qrBuffer, QR_VERSION, ECC_LOW, chatPairingUrl) == 0) {
            constexpr int moduleScale = 4;
            constexpr int quietModules = 4;
            const int qrPixels = (qr.size + quietModules * 2) * moduleScale;
            const int originX = (XingtaiEpd::WIDTH - qrPixels) / 2 + quietModules * moduleScale;
            const int originY = 70 + quietModules * moduleScale;
            for (uint8_t y = 0; y < qr.size; ++y)
                for (uint8_t x = 0; x < qr.size; ++x)
                    if (qrcode_getModule(&qr, x, y))
                        for (int sy = 0; sy < moduleScale; ++sy)
                            for (int sx = 0; sx < moduleScale; ++sx)
                                pixel(frame, originX + x * moduleScale + sx,
                                      originY + y * moduleScale + sy);
        } else {
            UiLocalization::drawCentered(frame, 150, "QR ERROR", 1);
        }
        UiLocalization::drawCentered(frame, 258, deviceMacAddress(), 1);
        UiLocalization::drawCentered(frame, 280, "MAC PAIRING", 1);
        rect(frame, 18, 350, 204, 42);
        UiLocalization::drawCentered(frame, 365,
            UiLocalization::isChinese() ? "返回" : "RETURN", 1);
        return;
    }
    rect(frame, PAIRING_X, PAIRING_Y, PAIRING_W, PAIRING_H);
    // Linked squares provide a compact pairing symbol beside the command name.
    rect(frame, PAIRING_X + 5, PAIRING_Y + 8, 8, 8);
    rect(frame, PAIRING_X + 11, PAIRING_Y + 12, 8, 8);
    UiLocalization::drawText(frame, PAIRING_X + 24, PAIRING_Y + 10, "PAIRING", 1);
    rect(frame, MESSAGE_X, MESSAGE_Y, MESSAGE_W, MESSAGE_H);
    ChatMessage snapshot[MAX_VISIBLE_MESSAGES] = {};
    const uint8_t visibleCount = snapshotMessages(snapshot, MAX_VISIBLE_MESSAGES);
    for (uint8_t index = 0; index < visibleCount; ++index) {
        const ChatMessage &message = snapshot[index];
        const int top = MESSAGE_Y + 8 + index * 62;
        const int left = message.deviceMessage ? MESSAGE_X + 55 : MESSAGE_X + 7;
        constexpr int width = 162;
        rect(frame, left, top, width, 40);
        if (message.audio && OpusPlayer::isPlaying() &&
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