#include "pages/chat/chat_page.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SD_MMC.h>

#include <cstring>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/ml307/ml307.h"
#include "devices/sd_card/sd_card.h"
#include "pages/recording/recording_page.h"
#include "ui/loading_indicator.h"
#include "ui/localization.h"

namespace {
char contentUrl[128] = {};
char status[96] = "Tap the microphone to record";
char pendingPath[96] = {};
bool uploadPending = false;
bool exitRequested = false;
uint32_t messageId = 0;
constexpr char CHAT_WAV_PATH[] = "/chat/latest.wav";

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
    http.addHeader("Connection", "close");
    const int code = http.sendRequest("POST", &file, length);
    if (code >= 200 && code < 300) response = http.getString();
    http.end();
    return code >= 200 && code < 300;
}
bool uploadFile() {
    File file = SD_MMC.open(pendingPath, FILE_READ);
    if (!file) { copyText(status, sizeof(status), "WAV file unavailable"); return false; }
    const size_t length = file.size();
    String response;
    const String url = endpoint("/api/chat/messages?room=main&sender=x");
    bool ok = false;
    if (WiFi.status() == WL_CONNECTED) ok = uploadWifi(url, file, length, response);
    file.close();
    if (!ok) { copyText(status, sizeof(status), "Chat upload failed"); return false; }
    JsonDocument json;
    if (!deserializeJson(json, response)) {
        messageId = json["id"] | 0;
        if (messageId == 0) messageId = json["message"]["id"] | 0;
    }
    snprintf(status, sizeof(status), "WAV sent (%lu bytes)", static_cast<unsigned long>(length));
    Serial.printf("[CHAT] uploaded bytes=%u id=%lu\n", static_cast<unsigned>(length),
                  static_cast<unsigned long>(messageId));
    return true;
}

bool httpGetText(const String &url, String &payload) {
    if (WiFi.status() != WL_CONNECTED) return cellularModem.httpGet(url.c_str(), payload, 30000);
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    secure.setInsecure();
    http.setConnectTimeout(7000);
    http.setTimeout(20000);
    const bool began = url.startsWith("https://") ? http.begin(secure, url) : http.begin(plain, url);
    if (!began) return false;
    const int code = http.GET();
    if (code >= 200 && code < 300) payload = http.getString();
    http.end();
    return code >= 200 && code < 300;
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
    const char *audioUrl = object["audio_url"] | object["url"] | "";
    const bool audio = std::strstr(type, "audio") || std::strstr(type, "wav") ||
                       (audioUrl && std::strstr(audioUrl, "/api/chat/audio/"));
    if (audio) latest = object["id"] | object["message_id"] | 0;
    for (JsonPair pair : object) {
        if (pair.value().is<JsonArray>() || pair.value().is<JsonObject>()) {
            latest = max(latest, latestAudioId(pair.value()));
        }
    }
    return latest;
}

bool refreshAndPlay() {
    if (messageId == 0) {
        String payload;
        if (!httpGetText(endpoint("/api/chat/messages?room=main"), payload)) {
            copyText(status, sizeof(status), "Chat refresh failed");
            return false;
        }
        JsonDocument json;
        if (deserializeJson(json, payload)) {
            copyText(status, sizeof(status), "Invalid chat response");
            return false;
        }
        messageId = latestAudioId(json.as<JsonVariant>());
    }
    if (messageId == 0) {
        copyText(status, sizeof(status), "No voice messages");
        return false;
    }
    const String audioUrl = endpoint(("/api/chat/audio/" + String(messageId)).c_str());
    if (!SdCard::downloadFile(audioUrl.c_str(), CHAT_WAV_PATH, 44)) {
        copyText(status, sizeof(status), "WAV download failed");
        return false;
    }
    if (!RecordingPage::playWavFile(CHAT_WAV_PATH)) {
        copyText(status, sizeof(status), "WAV playback failed");
        return false;
    }
    snprintf(status, sizeof(status), "Playing message %lu", static_cast<unsigned long>(messageId));
    return true;
}
}

namespace ChatPage {
void setContentUrl(const char *url) { copyText(contentUrl, sizeof(contentUrl), url); }
void open() { exitRequested = false; uploadPending = false; copyText(status, sizeof(status), "Tap the microphone to record"); }
bool returnControlAt(int16_t x, int16_t y) { return inRect(x, y, 10, 40, 48, 28); }
bool recordControlAt(int16_t x, int16_t y) { return inRect(x, y, 24, 150, 192, 100); }
bool refreshControlAt(int16_t x, int16_t y) { return inRect(x, y, 24, 290, 192, 48); }
bool handleTap(int16_t x, int16_t y) {
    if (returnControlAt(x, y)) { exitRequested = true; return true; }
    if (recordControlAt(x, y)) {
        if (RecordingPage::isRecording()) {
            RecordingPage::stopChatRecording();
            copyText(status, sizeof(status), "Saving WAV...");
        } else if (RecordingPage::startChatRecording()) {
            copyText(status, sizeof(status), "Recording...");
        } else copyText(status, sizeof(status), "Microphone unavailable");
        return true;
    }
    if (refreshControlAt(x, y)) {
        if (RecordingPage::isPlayingWav()) RecordingPage::stop();
        else refreshAndPlay();
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
bool takeExitRequest() { const bool result = exitRequested; exitRequested = false; return result; }
void render(uint8_t *frame) {
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    UiLocalization::drawText(frame, 10, 10, "CHAT", 2);
    UiLocalization::drawText(frame, 10, 42, "VOICE MESSAGE", 1);
    rect(frame, 24, 150, 192, 100);
    const bool recording = RecordingPage::isRecording();
    if (recording) {
        for (int y = 182; y < 218; ++y) for (int x = 103; x < 137; ++x) pixel(frame, x, y);
    } else {
        for (int y = 184; y < 216; ++y) for (int x = 104; x < 136; ++x) pixel(frame, x, y);
    }
    rect(frame, 24, 290, 192, 48);
    UiLocalization::drawCentered(frame, 307,
        RecordingPage::isPlayingWav() ? "STOP PLAYBACK" : "PLAY LATEST", 1);
    UiLocalization::drawCentered(frame, 372, status, 1);
    pixel(frame, 18, 52); pixel(frame, 19, 51); pixel(frame, 20, 50);
    pixel(frame, 18, 52); pixel(frame, 18, 64); pixel(frame, 41, 52); pixel(frame, 41, 64);
}
}