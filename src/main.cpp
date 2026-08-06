#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <WiFi.h>

#include <cmath>
#include <cstring>

#include "board_pins.h"
#include "devices/audio/opus_player.h"
#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/es8311/es8311.h"
#include "devices/ft6336/FT6X36.h"
#include "devices/sd_card/sd_card.h"
#include "font/xiaozhi_font.h"
#include "pages/asundar/asundar_page.h"
#include "pages/book/book_page.h"
#include "pages/calendar/calendar_page.h"
#include "pages/calculator/calculator_page.h"
#include "pages/clock/clock_page.h"
#include "pages/word/word_page.h"
#include "pages/main/main_page.h"
#include "pages/music/music_page.h"
#include "pages/poem/poem_page.h"
#include "pages/recording/recording_page.h"
#include "pages/settings/settings_page.h"
#include "pages/topbar/topbar_assets.h"
#include "pages/voice/voice_page.h"
#include "ui/loading_indicator.h"
#include "ui/localization.h"

XingtaiEpd epaper;
FT6X36 touch(&Wire, BoardPins::TOUCH_INT);
Es8311 audio(
    Wire,
    {BoardPins::AUDIO_MCLK, BoardPins::AUDIO_SCLK, BoardPins::AUDIO_LRCLK,
     BoardPins::AUDIO_DOUT, BoardPins::AUDIO_DIN, BoardPins::PA_EN, -1});

namespace {

using PageRenderer = void (*)(uint8_t *frame);

enum class PageId : uint8_t {
    Main,
    Settings,
    Calendar,
    Calculator,
    Clock,
    Book,
    Voice,
    Music,
    Poem,
    Word,
    Recording,
};

struct TouchAction {
    PageId page = PageId::Main;
    bool pending = false;
};

static uint8_t frame[XingtaiEpd::FRAME_BYTES];
static uint8_t transitionFrame[XingtaiEpd::FRAME_BYTES];
static uint8_t whiteFrame[XingtaiEpd::FRAME_BYTES] = {};
Preferences touchPreferences;
Preferences settingsPreferences;
Preferences wifiPreferences;
bool calibrationActive = false;
uint8_t calibrationCount = 0;
TPoint calibrationRaw[3] = {};
constexpr int16_t calibrationTargetX[3] = {20, 220, 120};
constexpr int16_t calibrationTargetY[3] = {40, 40, 380};
constexpr float defaultTouchTransform[6] = {
    static_cast<float>(XingtaiEpd::WIDTH - 1) / 299.0f, 0.0f, 0.0f,
    0.0f, static_cast<float>(XingtaiEpd::HEIGHT - 1) / 479.0f, 0.0f,
};
float touchTransform[6] = {
    defaultTouchTransform[0], defaultTouchTransform[1], defaultTouchTransform[2],
    defaultTouchTransform[3], defaultTouchTransform[4], defaultTouchTransform[5],
};
PageId currentPage = PageId::Main;
TouchAction touchAction;
bool initialEs8311Present = false;
uint8_t initialEs8311Address = 0;
bool initialFt6x36Present = false;
SettingsPage::State settingsState = {true, true, 60, 0};
bool lastWifiConnected = false;
bool cellularPowerApplied = false;
volatile bool cellularConnected = false;
volatile bool cellularProbeRunning = false;
uint32_t wifiPriorityStartedMs = 0;
uint32_t lastWifiReconnectAttemptMs = 0;
constexpr uint32_t WIFI_PRIORITY_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 15000;
constexpr uint32_t CELLULAR_DISCONNECTED_POLL_MS = 10000;
constexpr uint32_t CELLULAR_CONNECTED_POLL_MS = 30000;
HardwareSerial cellularSerial(1);
char savedWifiSsid[33] = {};
char savedWifiPassword[64] = {};
char scannedWifiNetworks[6][33] = {};
uint8_t scannedWifiCount = 0;
char wifiPasswordInput[64] = {};
char selectedWifiSsid[33] = {};
char sdEntryNames[8][33] = {};
bool sdEntryDirectories[8] = {};
constexpr char CONTENT_URL_PREFIX[] = "http://";
constexpr size_t CONTENT_URL_PREFIX_LENGTH = sizeof(CONTENT_URL_PREFIX) - 1;
char contentUrl[128] = "http://";
char savedContentUrl[128] = "http://";
uint8_t lastRenderedClockMinute = 0xFF;
volatile bool clockWeatherRequested = false;
volatile bool clockWeatherTaskRunning = false;
volatile bool clockWeatherUpdated = false;
bool touchGestureActive = false;
int16_t touchGestureStartX = 0;
int16_t touchGestureStartY = 0;
int16_t touchGestureLastX = 0;
int16_t touchGestureLastY = 0;
volatile bool touchInterruptPending = false;
bool suppressNextAudioTap = false;

void recoverSharedI2c();
void refreshCurrentPage();
void wipeContentAreaWhite();
void refreshVoiceDirtyRows();
void refreshMusicDirtyRows();
void refreshPoemDirtyRows();
void refreshPoemDisplay();
void refreshWordStrokeWindow(bool wipeFirst);
void clearFrameRegion(uint8_t *buffer, int left, int top, int width, int height);
void showTopbarLoading(bool visible);

void IRAM_ATTR onTouchInterruptSignal() {
    touchInterruptPending = true;
    // Stop decoding immediately from the GPIO interrupt path without touching
    // the shared I2C bus. Cleanup is completed in task context below.
    OpusPlayer::requestStopFromIsr();
}

void serviceTouchInterruptBeforeI2c() {
    bool pending = false;
    noInterrupts();
    pending = touchInterruptPending;
    touchInterruptPending = false;
    interrupts();
    const bool voiceActive = currentPage == PageId::Voice && VoicePage::isAudioActive();
    const bool musicActive = currentPage == PageId::Music && MusicPage::isAudioActive();
    const bool poemActive = currentPage == PageId::Poem && PoemPage::isAudioActive();
    const bool wordActive = currentPage == PageId::Word &&
                            (WordPage::isAudioActive() || WordPage::isAnimating());
    if (!pending || (!voiceActive && !musicActive && !poemActive && !wordActive)) return;

    Serial.printf("[TOUCH IRQ] Stopping %s audio before FT6336 I2C read\n",
                   voiceActive ? "Voice" : musicActive ? "Music" : poemActive ? "Poem" : "Word");
    if (voiceActive) VoicePage::stopAudioFromTouchInterrupt();
    else if (musicActive) MusicPage::stopAudioFromTouchInterrupt();
    else if (poemActive) PoemPage::stopAudioFromTouchInterrupt();
    else WordPage::stopFromTouchInterrupt();
    // Audio codec setup and FT6336 share Wire. Reclock/recover the bus only
    // after the audio task has stopped, then let touch.loop() read coordinates.
    recoverSharedI2c();
    suppressNextAudioTap = true;
    Serial.printf("[TOUCH IRQ] I2C returned to FT6336 SDA=%d SCL=%d INT=%d\n",
                  digitalRead(BoardPins::I2C_SDA), digitalRead(BoardPins::I2C_SCL),
                  digitalRead(BoardPins::TOUCH_INT));
}

String readCellularResponse(uint32_t timeoutMs, const char *tokenA, const char *tokenB) {
    String response;
    const uint32_t started = millis();
    while (millis() - started < timeoutMs) {
        while (cellularSerial.available() > 0) {
            response += static_cast<char>(cellularSerial.read());
            if ((tokenA && response.indexOf(tokenA) >= 0) ||
                (tokenB && response.indexOf(tokenB) >= 0)) {
                return response;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return response;
}

bool sendCellularCommand(const char *command, uint32_t timeoutMs, String &response) {
    while (cellularSerial.available() > 0) cellularSerial.read();
    Serial.printf("[ML307] >> %s\n", command);
    cellularSerial.print(command);
    cellularSerial.print("\r\n");
    response = readCellularResponse(timeoutMs, "OK", "ERROR");
    response.replace("\r", "");
    response.replace("\n", " ");
    Serial.printf("[ML307] << %s\n", response.c_str());
    return response.indexOf("OK") >= 0 && response.indexOf("ERROR") < 0;
}

bool probeCellularConnection() {
    if (!settingsState.cellularEnabled || !cellularPowerApplied ||
        WiFi.status() == WL_CONNECTED) {
        return false;
    }

    String response;
    if (!sendCellularCommand("AT", 2000, response)) return false;
    sendCellularCommand("ATE0", 1000, response);
    sendCellularCommand("AT+CPIN?", 2000, response);
    sendCellularCommand("AT+CSQ", 2000, response);
    response = "";
    sendCellularCommand("AT+MIPCALL?", 3000, response);
    bool ready = response.indexOf("+MIPCALL:") >= 0 &&
                 (response.indexOf(",1") >= 0 || response.indexOf(": 1") >= 0);
    if (!ready && WiFi.status() != WL_CONNECTED && cellularPowerApplied) {
        sendCellularCommand("AT+MIPCALL=1", 8000, response);
        response = "";
        sendCellularCommand("AT+MIPCALL?", 3000, response);
        ready = response.indexOf("+MIPCALL:") >= 0 &&
                (response.indexOf(",1") >= 0 || response.indexOf(": 1") >= 0);
    }
    return ready;
}

void cellularPollingTask(void *) {
    cellularProbeRunning = true;
    cellularSerial.begin(115200, SERIAL_8N1, BoardPins::MODEM_RX, BoardPins::MODEM_TX);
    cellularSerial.setTimeout(200);
    Serial.printf("[ML307] Poller started baud=115200 RX=%d TX=%d\n",
                  BoardPins::MODEM_RX, BoardPins::MODEM_TX);
    while (true) {
        if (!settingsState.cellularEnabled || !cellularPowerApplied ||
            WiFi.status() == WL_CONNECTED) {
            if (cellularConnected) {
                cellularConnected = false;
                Serial.println("[NETWORK] 4G no longer active");
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const bool wasConnected = cellularConnected;
        cellularConnected = probeCellularConnection();
        if (wasConnected != cellularConnected) {
            Serial.printf("[NETWORK] 4G %s\n", cellularConnected ? "connected" : "disconnected");
        } else if (!cellularConnected) {
            Serial.println("[NETWORK] Neither WiFi nor 4G connected; polling continues");
        }
        vTaskDelay(pdMS_TO_TICKS(cellularConnected
                                    ? CELLULAR_CONNECTED_POLL_MS
                                    : CELLULAR_DISCONNECTED_POLL_MS));
    }
}

void loadWifiCredentials() {
    if (!wifiPreferences.begin("wifi", true)) return;
    const String ssid = wifiPreferences.getString("ssid", "");
    const String password = wifiPreferences.getString("password", "");
    wifiPreferences.end();
    std::strncpy(savedWifiSsid, ssid.c_str(), sizeof(savedWifiSsid) - 1);
    std::strncpy(savedWifiPassword, password.c_str(), sizeof(savedWifiPassword) - 1);
}

void saveWifiCredentials() {
    if (!wifiPreferences.begin("wifi", false)) return;
    wifiPreferences.putString("ssid", selectedWifiSsid);
    wifiPreferences.putString("password", wifiPasswordInput);
    wifiPreferences.end();
    std::strncpy(savedWifiSsid, selectedWifiSsid, sizeof(savedWifiSsid) - 1);
    std::strncpy(savedWifiPassword, wifiPasswordInput, sizeof(savedWifiPassword) - 1);
}

void saveSettings() {
    if (!settingsPreferences.begin("settings", false)) {
        Serial.println("[SETTINGS] Failed to open preferences for writing");
        return;
    }
    settingsPreferences.putBool("wifi", settingsState.wifiEnabled);
    settingsPreferences.putBool("cellular", settingsState.cellularEnabled);
    settingsPreferences.putUChar("volume", settingsState.volumePercent);
    settingsPreferences.putUChar("language", settingsState.language);
    settingsPreferences.putString("contentUrl", contentUrl);
    settingsPreferences.putString("ttsVoice", SettingsPage::voiceName());
    settingsPreferences.end();
}

bool contentUrlReachable(const char *url) {
    if (!url || std::strlen(url) <= CONTENT_URL_PREFIX_LENGTH || WiFi.status() != WL_CONNECTED) {
        return false;
    }

    UiLoadingIndicator::Scope loadingIndicator;
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(url)) return false;
    const int responseCode = http.GET();
    http.end();
    Serial.printf("[CONTENT URL] Validation %s returned HTTP code %d\n", url, responseCode);
    return responseCode > 0;
}

void copyAsciiUpper(char *destination, size_t destinationSize, const String &source) {
    if (!destination || destinationSize == 0) return;
    size_t output = 0;
    bool previousSpace = true;
    for (size_t index = 0; index < source.length() && output + 1 < destinationSize; ++index) {
        unsigned char character = static_cast<unsigned char>(source[index]);
        if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
        if (character >= 32 && character <= 126) {
            if (character == ' ' && previousSpace) continue;
            destination[output++] = static_cast<char>(character);
            previousSpace = character == ' ';
        }
    }
    while (output > 0 && destination[output - 1] == ' ') --output;
    destination[output] = '\0';
}

bool fetchClockWeather() {
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    HTTPClient http;
    http.setConnectTimeout(7000);
    http.setTimeout(7000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent("ESP32-ePaper-Clock/1.0");
    // wttr.in resolves the public IP to a location and returns a deliberately
    // compact, pipe-separated response:
    // location|condition|temperature|humidity|wind.
    if (!http.begin("http://wttr.in/?format=%25l%7C%25C%7C%25t%7C%25h%7C%25w")) return false;
    const int responseCode = http.GET();
    const String response = responseCode > 0 ? http.getString() : String();
    http.end();
    if (responseCode < 200 || responseCode >= 400) {
        Serial.printf("[CLOCK] Weather request failed with HTTP code %d\n", responseCode);
        return false;
    }

    const int firstSeparator = response.indexOf('|');
    const int secondSeparator = response.indexOf('|', firstSeparator + 1);
    const int thirdSeparator = response.indexOf('|', secondSeparator + 1);
    const int fourthSeparator = response.indexOf('|', thirdSeparator + 1);
    if (firstSeparator <= 0 || secondSeparator <= firstSeparator ||
        thirdSeparator <= secondSeparator || fourthSeparator <= thirdSeparator) {
        Serial.printf("[CLOCK] Unexpected weather response: %s\n", response.c_str());
        return false;
    }

    char location[32] = {};
    char condition[32] = {};
    char temperature[12] = {};
    char humidity[12] = {};
    char windSpeed[20] = {};
    copyAsciiUpper(location, sizeof(location), response.substring(0, firstSeparator));
    copyAsciiUpper(condition, sizeof(condition), response.substring(firstSeparator + 1, secondSeparator));
    copyAsciiUpper(temperature, sizeof(temperature),
                   response.substring(secondSeparator + 1, thirdSeparator));
    copyAsciiUpper(humidity, sizeof(humidity),
                   response.substring(thirdSeparator + 1, fourthSeparator));
    copyAsciiUpper(windSpeed, sizeof(windSpeed), response.substring(fourthSeparator + 1));
    ClockPage::setWeather(location, condition, temperature, humidity, windSpeed, true);
    clockWeatherUpdated = true;
    Serial.printf("[CLOCK] Weather updated: %s, %s, %s, humidity=%s, wind=%s\n",
                  location, condition, temperature, humidity, windSpeed);
    return true;
}

void clockWeatherTask(void *) {
    clockWeatherTaskRunning = true;
    uint32_t nextAttemptMs = 0;
    while (clockWeatherRequested) {
        const uint32_t nowMs = millis();
        if (static_cast<int32_t>(nowMs - nextAttemptMs) >= 0) {
            const bool updated = fetchClockWeather();
            // Retry quickly while Wi-Fi, DNS, or the weather endpoint is still
            // becoming available. Once successful, refresh every 30 minutes.
            nextAttemptMs = nowMs + (updated ? 30UL * 60UL * 1000UL : 10UL * 1000UL);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    clockWeatherTaskRunning = false;
    vTaskDelete(nullptr);
}

void prepareClockPage() {
    clockWeatherRequested = true;
    if (!clockWeatherTaskRunning) {
        xTaskCreate(clockWeatherTask, "clock-weather", 6144, nullptr, 1, nullptr);
    }
    time_t now = time(nullptr);
    tm timeInfo = {};
    localtime_r(&now, &timeInfo);
    lastRenderedClockMinute = static_cast<uint8_t>(timeInfo.tm_min);
}

void applyWifiSetting(bool waitForConnection = false) {
    if (settingsState.wifiEnabled) {
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
        if (savedWifiSsid[0] != '\0') {
            // A user-triggered enable should restart association immediately,
            // even if the ESP32 Wi-Fi state machine still remembers a prior
            // disabled/disconnected session.
            WiFi.disconnect(false, false);
            delay(50);
            WiFi.begin(savedWifiSsid, savedWifiPassword);
            wifiPriorityStartedMs = millis();
            lastWifiReconnectAttemptMs = wifiPriorityStartedMs;
            Serial.printf("[NETWORK] WiFi priority connection started for %s\n", savedWifiSsid);
            if (waitForConnection) {
                constexpr uint32_t IMMEDIATE_WIFI_TIMEOUT_MS = 10000;
                const uint32_t started = millis();
                while (WiFi.status() != WL_CONNECTED &&
                       millis() - started < IMMEDIATE_WIFI_TIMEOUT_MS) {
                    serviceTouchInterruptBeforeI2c();
                    delay(100);
                }
                if (WiFi.status() == WL_CONNECTED) {
                    Serial.printf("[NETWORK] WiFi connected immediately SSID=%s IP=%s\n",
                                  savedWifiSsid, WiFi.localIP().toString().c_str());
                } else {
                    Serial.printf("[NETWORK] WiFi immediate connection timed out SSID=%s status=%d\n",
                                  savedWifiSsid, static_cast<int>(WiFi.status()));
                }
            }
        } else {
            wifiPriorityStartedMs = 0;
            Serial.println("[NETWORK] WiFi is enabled but no saved network is available");
        }
    } else {
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
        wifiPriorityStartedMs = 0;
        lastWifiReconnectAttemptMs = 0;
    }
    Serial.printf("[SETTINGS] WiFi %s\n", settingsState.wifiEnabled ? "enabled" : "disabled");
}

void applyCellularSetting() {
    const bool shouldPower = settingsState.cellularEnabled && !settingsState.wifiEnabled;
    pinMode(BoardPins::MODEM_PWR, OUTPUT);
    digitalWrite(BoardPins::MODEM_PWR, shouldPower ? HIGH : LOW);
    cellularPowerApplied = shouldPower;
    if (!shouldPower) cellularConnected = false;
    Serial.printf("[NETWORK] 4G modem power %s%s\n",
                  shouldPower ? "enabled" : "disabled",
                  settingsState.cellularEnabled && settingsState.wifiEnabled
                      ? " (WiFi has priority)" : "");
}

void updateNetworkPriority() {
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    const bool anyConnected = wifiConnected || cellularConnected;

    if (!settingsState.cellularEnabled) {
        if (cellularPowerApplied) {
            digitalWrite(BoardPins::MODEM_PWR, LOW);
            cellularPowerApplied = false;
            Serial.println("[NETWORK] 4G modem power disabled by setting");
        }
        return;
    }

    if (!settingsState.wifiEnabled) {
        if (!cellularPowerApplied) {
            digitalWrite(BoardPins::MODEM_PWR, HIGH);
            cellularPowerApplied = true;
            Serial.println("[NETWORK] WiFi is off; 4G modem powered immediately");
        }
        return;
    }

    if (wifiConnected) {
        if (cellularPowerApplied) {
            digitalWrite(BoardPins::MODEM_PWR, LOW);
            cellularPowerApplied = false;
            Serial.println("[NETWORK] WiFi connected; 4G modem powered down");
        }
        return;
    }

    // WiFi remains the preferred transport whenever its setting is enabled.
    // Keep polling the saved network even while 4G is carrying traffic, so a
    // recovered access point automatically takes priority again.
    if (!wifiConnected && savedWifiSsid[0] != '\0' &&
        millis() - lastWifiReconnectAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
        lastWifiReconnectAttemptMs = millis();
        Serial.printf("[NETWORK] Polling WiFi connection for %s%s\n", savedWifiSsid,
                      cellularConnected ? " while 4G remains active" : "");
        WiFi.disconnect(false, false);
        WiFi.begin(savedWifiSsid, savedWifiPassword);
    }

    const bool noSavedWifi = savedWifiSsid[0] == '\0';
    const bool wifiTimedOut = wifiPriorityStartedMs != 0 &&
        millis() - wifiPriorityStartedMs >= WIFI_PRIORITY_TIMEOUT_MS;
    if ((noSavedWifi || wifiTimedOut) && !cellularPowerApplied) {
        digitalWrite(BoardPins::MODEM_PWR, HIGH);
        cellularPowerApplied = true;
        Serial.printf("[NETWORK] WiFi unavailable after priority check; 4G modem powered%s\n",
                      noSavedWifi ? " (no saved WiFi)" : " after 15s timeout");
    }
}

void applyAudioSetting() {
    if (audio.isInitialized()) audio.setOutputVolume(settingsState.volumePercent);
    audio.setSpeakerEnabled(settingsState.volumePercent > 0);
    Serial.printf("[SETTINGS] Audio volume %u%%\n", settingsState.volumePercent);
}

void playAudioTestBeep() {
    if (!audio.isInitialized() || settingsState.volumePercent == 0) {
        Serial.println("[AUDIO TEST] Codec unavailable or volume is muted");
        return;
    }

    OpusPlayer::stop();
    audio.setOutputVolume(settingsState.volumePercent);
    audio.setSpeakerEnabled(true);
    constexpr uint32_t sampleRate = Es8311::DEFAULT_SAMPLE_RATE;
    constexpr uint32_t frequency = 880;
    constexpr size_t sampleCount = sampleRate / 4;
    constexpr size_t chunkSamples = 256;
    int16_t samples[chunkSamples] = {};
    for (size_t offset = 0; offset < sampleCount; offset += chunkSamples) {
        const size_t count = min(chunkSamples, sampleCount - offset);
        for (size_t i = 0; i < count; ++i) {
            const float phase = 2.0f * PI * frequency * (offset + i) / sampleRate;
            samples[i] = static_cast<int16_t>(sinf(phase) * 9000.0f);
        }
        if (audio.write(samples, count, 500) != count) {
            Serial.println("[AUDIO TEST] Short PCM write");
            break;
        }
    }
    std::memset(samples, 0, sizeof(samples));
    audio.write(samples, chunkSamples, 500);
    Serial.printf("[AUDIO TEST] Beep played at %u%% volume\n", settingsState.volumePercent);
}

void loadSettings() {
    bool migratedContentUrl = false;
    if (settingsPreferences.begin("settings", true)) {
        settingsState.wifiEnabled = settingsPreferences.getBool("wifi", true);
        settingsState.cellularEnabled = settingsPreferences.getBool("cellular", true);
        settingsState.volumePercent = settingsPreferences.getUChar("volume", 60);
        settingsState.language = settingsPreferences.getUChar("language", 0);
        const String savedContentUrl = settingsPreferences.getString("contentUrl", "");
        const String savedVoice = settingsPreferences.getString("ttsVoice", "Jasper");
        std::strncpy(contentUrl, savedContentUrl.c_str(), sizeof(contentUrl) - 1);
        if (contentUrl[0] == '\0') std::strcpy(contentUrl, CONTENT_URL_PREFIX);
        // Migrate the known mistyped local server address used by earlier
        // firmware/settings. The active epaper_s3 API is at 192.168.2.220.
        constexpr char MISTYPED_CONTENT_HOST[] = "http://192.169.2.220";
        constexpr size_t MISTYPED_CONTENT_HOST_LENGTH = sizeof(MISTYPED_CONTENT_HOST) - 1;
        if (std::strncmp(contentUrl, MISTYPED_CONTENT_HOST,
                         MISTYPED_CONTENT_HOST_LENGTH) == 0) {
            const char *suffix = contentUrl + MISTYPED_CONTENT_HOST_LENGTH;
            char corrected[sizeof(contentUrl)] = {};
            snprintf(corrected, sizeof(corrected), "http://192.168.2.220%s", suffix);
            std::strncpy(contentUrl, corrected, sizeof(contentUrl) - 1);
            contentUrl[sizeof(contentUrl) - 1] = '\0';
            migratedContentUrl = true;
            Serial.printf("[CONTENT URL] Migrated mistyped server URL to %s\n", contentUrl);
        }
        std::strncpy(::savedContentUrl, contentUrl, sizeof(::savedContentUrl) - 1);
        ::savedContentUrl[sizeof(::savedContentUrl) - 1] = '\0';
        SettingsPage::setVoice(savedVoice.c_str());
        settingsPreferences.end();
    }
    if (settingsState.volumePercent > 100) settingsState.volumePercent = 100;
    SettingsPage::setState(settingsState);
    UiLocalization::setLanguage(settingsState.language);
    loadWifiCredentials();
    if (migratedContentUrl) saveSettings();
}

void scanWifiNetworks() {
    SettingsPage::showWifiScanning();
    refreshCurrentPage();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    const int found = WiFi.scanNetworks();
    scannedWifiCount = found > 0 ? static_cast<uint8_t>(found > 6 ? 6 : found) : 0;
    for (uint8_t index = 0; index < scannedWifiCount; ++index) {
        std::strncpy(scannedWifiNetworks[index], WiFi.SSID(index).c_str(), 32);
        scannedWifiNetworks[index][32] = '\0';
    }
    WiFi.scanDelete();
    SettingsPage::showWifiNetworks(scannedWifiNetworks, scannedWifiCount);
    refreshCurrentPage();
}

void connectSelectedWifi() {
    settingsState.wifiEnabled = true;
    SettingsPage::setState(settingsState);
    WiFi.disconnect(true, false);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(selectedWifiSsid, wifiPasswordInput);
    uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 10000) delay(100);

    bool usedSavedPassword = false;
    const bool sameSavedNetwork = savedWifiSsid[0] != '\0' &&
        std::strcmp(selectedWifiSsid, savedWifiSsid) == 0;
    const bool passwordChanged = std::strcmp(wifiPasswordInput, savedWifiPassword) != 0;
    if (WiFi.status() != WL_CONNECTED && sameSavedNetwork && passwordChanged) {
        Serial.printf("[WIFI] New password failed for %s; retrying last successful password\n",
                      selectedWifiSsid);
        WiFi.disconnect(true, false);
        delay(100);
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(selectedWifiSsid, savedWifiPassword);
        started = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - started < 10000) delay(100);
        usedSavedPassword = WiFi.status() == WL_CONNECTED;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (usedSavedPassword) {
            std::strncpy(wifiPasswordInput, savedWifiPassword, sizeof(wifiPasswordInput) - 1);
            wifiPasswordInput[sizeof(wifiPasswordInput) - 1] = '\0';
            Serial.printf("[WIFI] Reconnected to %s with last successful password\n",
                          selectedWifiSsid);
        } else {
            saveWifiCredentials();
        }
        saveSettings();
        Serial.printf("[WIFI] Connected to %s, IP=%s\n", selectedWifiSsid,
                      WiFi.localIP().toString().c_str());
        SettingsPage::showSettings();
    } else {
        Serial.printf("[WIFI] Connection to %s failed\n", selectedWifiSsid);
        SettingsPage::showWifiPassword(selectedWifiSsid, wifiPasswordInput);
    }
    refreshCurrentPage();
}

const char *pageName(PageId page) {
    switch (page) {
    case PageId::Settings: return "Settings";
    case PageId::Calendar: return "Calendar";
    case PageId::Calculator: return "Calculator";
    case PageId::Clock: return "Clock";
    case PageId::Book: return "Book";
    case PageId::Voice: return "Voice";
    case PageId::Music: return "Music";
    case PageId::Poem: return "Poem";
    case PageId::Word: return "Word";
    case PageId::Recording: return "Recording";
    default: return "Main";
    }
}

PageRenderer rendererFor(PageId page) {
    switch (page) {
    case PageId::Settings: return SettingsPage::render;
    case PageId::Calendar: return CalendarPage::render;
    case PageId::Calculator: return CalculatorPage::render;
    case PageId::Clock: return ClockPage::render;
    case PageId::Book: return BookPage::render;
    case PageId::Voice: return VoicePage::render;
    case PageId::Music: return MusicPage::render;
    case PageId::Poem: return PoemPage::render;
    case PageId::Word: return WordPage::render;
    case PageId::Recording: return RecordingPage::render;
    default: return MainPage::render;
    }
}

void refreshCurrentPage() {
    rendererFor(currentPage)(transitionFrame);
    constexpr size_t topBarBytes = static_cast<size_t>(32) * (XingtaiEpd::WIDTH / 8);
    std::memcpy(transitionFrame, frame, topBarBytes);
    if (currentPage == PageId::Book || currentPage == PageId::Voice ||
        currentPage == PageId::Music || currentPage == PageId::Poem ||
        currentPage == PageId::Word) {
        // Drive the old SD-backed content area white before drawing its next
        // list/detail/page state, reducing ghosted text and borders.
        wipeContentAreaWhite();
    }
    epaper.displayPartial(frame, transitionFrame, 0, 32,
                          XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT - 32);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
}

void wipeContentAreaWhite() {
    constexpr uint16_t topBarHeight = 32;
    constexpr size_t topBarBytes = static_cast<size_t>(topBarHeight) *
                                   (XingtaiEpd::WIDTH / 8);
    epaper.displayPartial(frame, whiteFrame, 0, 32,
                          XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT - 32);
    // Keep software state synchronized with the physical white pre-drive so
    // the following partial update compares against the panel's actual pixels.
    std::memset(frame + topBarBytes, 0x00,
                XingtaiEpd::FRAME_BYTES - topBarBytes);
    epaper.sleep();
}

void refreshCurrentRegion(uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    rendererFor(currentPage)(transitionFrame);
    constexpr size_t topBarBytes = static_cast<size_t>(32) * (XingtaiEpd::WIDTH / 8);
    std::memcpy(transitionFrame, frame, topBarBytes);
    epaper.displayPartial(frame, transitionFrame, x, y, width, height);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
}

void refreshPoemDisplay() {
    constexpr uint16_t popupX = 12;
    constexpr uint16_t popupY = 82;
    constexpr uint16_t popupWidth = 216;
    constexpr uint16_t popupHeight = 10 * 30 + 9 * 2;

    // The poem reader is an in-place window. Only scrub and redraw the window
    // rectangle; leave the playlist and the rest of the page untouched.
    epaper.displayPartial(frame, whiteFrame, popupX, popupY,
                          popupWidth, popupHeight);
    std::memset(frame + popupY * (XingtaiEpd::WIDTH / 8), 0x00,
                static_cast<size_t>(popupHeight) * (XingtaiEpd::WIDTH / 8));
    epaper.sleep();

    PoemPage::render(transitionFrame);
    constexpr size_t topBarBytes = static_cast<size_t>(32) * (XingtaiEpd::WIDTH / 8);
    std::memcpy(transitionFrame, frame, topBarBytes);
    epaper.displayPartial(frame, transitionFrame, popupX, popupY,
                          popupWidth, popupHeight);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
}

void refreshVoiceDirtyRows() {
    int8_t firstRow = -1;
    int8_t secondRow = -1;
    if (!VoicePage::takeDirtyRows(firstRow, secondRow)) return;

    VoicePage::render(transitionFrame);
    auto refreshRow = [&](int8_t row) {
        if (row < 0) return;
        constexpr uint16_t listTop = 82;
        constexpr uint16_t rowPitch = 32;
        epaper.displayPartial(frame, transitionFrame, 12,
                              listTop + static_cast<uint16_t>(row) * rowPitch,
                              216, 30);
    };
    refreshRow(firstRow);
    refreshRow(secondRow);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
}

void refreshMusicDirtyRows() {
    int8_t firstRow = -1;
    int8_t secondRow = -1;
    if (!MusicPage::takeDirtyRows(firstRow, secondRow)) return;

    MusicPage::render(transitionFrame);
    auto refreshRow = [&](int8_t row) {
        if (row < 0) return;
        constexpr uint16_t listTop = 82;
        constexpr uint16_t rowPitch = 32;
        epaper.displayPartial(frame, transitionFrame, 12,
                              listTop + static_cast<uint16_t>(row) * rowPitch,
                              216, 30);
    };
    refreshRow(firstRow);
    refreshRow(secondRow);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
}

void refreshPoemDirtyRows() {
    int8_t firstRow = -1;
    int8_t secondRow = -1;
    if (!PoemPage::takeDirtyRows(firstRow, secondRow)) return;

    PoemPage::render(transitionFrame);
    auto refreshRow = [&](int8_t row) {
        if (row < 0) return;
        constexpr uint16_t listTop = 82;
        constexpr uint16_t rowPitch = 32;
        epaper.displayPartial(frame, transitionFrame, 12,
                              listTop + static_cast<uint16_t>(row) * rowPitch,
                              216, 30);
    };
    refreshRow(firstRow);
    refreshRow(secondRow);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
}

void refreshWordStrokeWindow(bool wipeFirst) {
    // Keep the partial waveform strictly inside the image card and away from
    // its border, which prevents the border from being disturbed.
    constexpr uint16_t strokeLeft = 22;
    constexpr uint16_t strokeTop = 80;
    constexpr uint16_t strokeWidth = 108;
    constexpr uint16_t strokeHeight = 108;

    WordPage::render(transitionFrame);
    if (wipeFirst) {
        epaper.displayPartial(frame, whiteFrame, strokeLeft, strokeTop,
                              strokeWidth, strokeHeight);
        clearFrameRegion(frame, strokeLeft, strokeTop, strokeWidth, strokeHeight);
        epaper.sleep();
    }
    epaper.displayPartial(frame, transitionFrame, strokeLeft, strokeTop,
                          strokeWidth, strokeHeight);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
}

void clearFrameRegion(uint8_t *buffer, int left, int top, int width, int height) {
    for (int y = top; y < top + height; ++y) {
        for (int x = left; x < left + width; ++x) {
            buffer[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] &=
                static_cast<uint8_t>(~(0x80U >> (x % 8)));
        }
    }
}

void showTopbarLoading(bool visible) {
    constexpr int loadingLeft = 58;
    constexpr int loadingWidth = 120;
    constexpr int loadingTop = 0;
    constexpr int loadingHeight = 32;

    if (visible) {
        std::memcpy(transitionFrame, frame, XingtaiEpd::FRAME_BYTES);
        clearFrameRegion(transitionFrame, loadingLeft, loadingTop,
                         loadingWidth, loadingHeight);
        UiLocalization::drawCentered(transitionFrame, 12,
                                     UiLocalization::isChinese() ? "下载中" : "LOADING", 1);
        epaper.displayPartial(frame, transitionFrame, loadingLeft, loadingTop,
                              loadingWidth, loadingHeight);
    } else {
        epaper.displayPartial(transitionFrame, frame, loadingLeft, loadingTop,
                              loadingWidth, loadingHeight);
    }
    epaper.sleep();
}

void refreshWifiTopbar(bool connected) {
    MainPage::setWifiConnected(connected);
    std::memcpy(transitionFrame, frame, XingtaiEpd::FRAME_BYTES);
    clearFrameRegion(transitionFrame, 178, 0, 28, 32);
    if (connected) Topbar::drawWifi(transitionFrame, 181, 2);
    epaper.displayPartial(frame, transitionFrame, 178, 0, 28, 32);
    if (!connected) {
        // Reapply the black-to-white waveform once to scrub residual pigment
        // without touching the adjacent battery icon.
        epaper.displayPartial(frame, transitionFrame, 178, 0, 28, 32);
    }
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
}

void updateWifiTopbar() {
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected == lastWifiConnected) return;
    lastWifiConnected = connected;
    Serial.printf("[TOPBAR] WiFi %s\n", connected ? "connected" : "disconnected");
    refreshWifiTopbar(connected);
}

void updateClockPage() {
    if (currentPage != PageId::Clock) return;
    if (clockWeatherUpdated) {
        clockWeatherUpdated = false;
        refreshCurrentPage();
        return;
    }
    time_t now = time(nullptr);
    tm timeInfo = {};
    localtime_r(&now, &timeInfo);
    const uint8_t minute = static_cast<uint8_t>(timeInfo.tm_min);
    if (minute == lastRenderedClockMinute) return;
    lastRenderedClockMinute = minute;
    refreshCurrentPage();
}

MainPage::FunctionIcon mainIconFor(PageId page) {
    switch (page) {
    case PageId::Settings: return MainPage::FunctionIcon::Settings;
    case PageId::Calendar: return MainPage::FunctionIcon::Calendar;
    case PageId::Calculator: return MainPage::FunctionIcon::Calculator;
    case PageId::Clock: return MainPage::FunctionIcon::Clock;
    case PageId::Book: return MainPage::FunctionIcon::Book;
    case PageId::Voice: return MainPage::FunctionIcon::Voice;
    case PageId::Music: return MainPage::FunctionIcon::Music;
    case PageId::Poem: return MainPage::FunctionIcon::Poem;
    case PageId::Word: return MainPage::FunctionIcon::Word;
    case PageId::Recording: return MainPage::FunctionIcon::Recording;
    default: return MainPage::FunctionIcon::None;
    }
}

bool mainIconBounds(PageId page, uint16_t &x, uint16_t &y, uint16_t &width, uint16_t &height) {
    constexpr uint16_t iconSize = 48;
    constexpr uint16_t framePadding = 4;
    constexpr uint16_t frameSize = iconSize + framePadding * 2;
    constexpr uint16_t columnGap = 32;
    constexpr uint16_t rowGap = 22;
    constexpr uint16_t startX = (XingtaiEpd::WIDTH - iconSize * 3 - columnGap * 2) / 2;
    constexpr uint16_t startY = 56;

    int column = 0;
    int row = 0;
    switch (page) {
    case PageId::Settings: column = 0; row = 0; break;
    case PageId::Calendar: column = 1; row = 0; break;
    case PageId::Calculator: column = 2; row = 0; break;
    case PageId::Clock: column = 0; row = 1; break;
    case PageId::Book: column = 1; row = 1; break;
    case PageId::Voice: column = 2; row = 1; break;
    case PageId::Music: column = 0; row = 2; break;
    case PageId::Poem: column = 1; row = 2; break;
    case PageId::Word: column = 2; row = 2; break;
    case PageId::Recording: column = 0; row = 3; break;
    default: return false;
    }

    x = startX + column * (iconSize + columnGap) - framePadding;
    y = startY + row * (iconSize + rowGap) - framePadding;
    width = frameSize;
    height = frameSize;
    return true;
}

bool rawToDisplay(TPoint raw, int16_t &x, int16_t &y) {
    const float mappedX = touchTransform[0] * raw.x + touchTransform[1] * raw.y + touchTransform[2];
    const float mappedY = touchTransform[3] * raw.x + touchTransform[4] * raw.y + touchTransform[5];
    x = static_cast<int16_t>(mappedX + 0.5f);
    y = static_cast<int16_t>(mappedY + 0.5f);
    return x >= 0 && x < XingtaiEpd::WIDTH && y >= 0 && y < XingtaiEpd::HEIGHT;
}

void calibrationPixel(uint8_t *buffer, int16_t x, int16_t y) {
    if (x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    buffer[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |= 0x80U >> (x % 8);
}

void renderCalibrationTarget(uint8_t *buffer, uint8_t target) {
    std::memset(buffer, 0x00, XingtaiEpd::FRAME_BYTES);
    // display() transfers the framebuffer upside down to the physical EPD.
    // Draw each target at the inverse logical position so it appears at the
    // requested physical screen coordinate for the user.
    const int16_t x = XingtaiEpd::WIDTH - 1 - calibrationTargetX[target];
    const int16_t y = XingtaiEpd::HEIGHT - 1 - calibrationTargetY[target];
    for (int16_t d = -12; d <= 12; ++d) {
        calibrationPixel(buffer, x + d, y);
        calibrationPixel(buffer, x, y + d);
    }
    for (int16_t d = -4; d <= 4; ++d) {
        calibrationPixel(buffer, x + d, y - 8);
        calibrationPixel(buffer, x + d, y + 8);
        calibrationPixel(buffer, x - 8, y + d);
        calibrationPixel(buffer, x + 8, y + d);
    }
}

bool solveCalibration() {
    const float x0 = calibrationRaw[0].x;
    const float y0 = calibrationRaw[0].y;
    const float x1 = calibrationRaw[1].x;
    const float y1 = calibrationRaw[1].y;
    const float x2 = calibrationRaw[2].x;
    const float y2 = calibrationRaw[2].y;
    const float determinant = x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1);
    if (fabsf(determinant) < 1.0f) return false;

    const float targetsX[3] = {calibrationTargetX[0], calibrationTargetX[1], calibrationTargetX[2]};
    const float targetsY[3] = {calibrationTargetY[0], calibrationTargetY[1], calibrationTargetY[2]};
    for (int axis = 0; axis < 2; ++axis) {
        const float *targets = axis == 0 ? targetsX : targetsY;
        const float a = (targets[0] * (y1 - y2) + targets[1] * (y2 - y0) + targets[2] * (y0 - y1)) / determinant;
        const float b = (x0 * (targets[1] - targets[2]) + x1 * (targets[2] - targets[0]) + x2 * (targets[0] - targets[1])) / determinant;
        const float c = (targets[0] * (x1 * y2 - x2 * y1) +
                         targets[1] * (x2 * y0 - x0 * y2) +
                         targets[2] * (x0 * y1 - x1 * y0)) / determinant;
        touchTransform[axis * 3] = a;
        touchTransform[axis * 3 + 1] = b;
        touchTransform[axis * 3 + 2] = c;
    }
    touchPreferences.begin("touch", false);
    touchPreferences.putBytes("matrix", touchTransform, sizeof(touchTransform));
    touchPreferences.putUChar("version", 2);
    touchPreferences.end();
    return true;
}

void loadCalibration() {
    touchPreferences.begin("touch", true);
    if (touchPreferences.getUChar("version", 0) == 2 &&
        touchPreferences.getBytesLength("matrix") == sizeof(touchTransform)) {
        touchPreferences.getBytes("matrix", touchTransform, sizeof(touchTransform));
        Serial.println("[CAL] Loaded touch calibration v2");
    } else {
        std::memcpy(touchTransform, defaultTouchTransform, sizeof(touchTransform));
        Serial.println("[CAL] Saved calibration missing or outdated; using 300x480 panel mapping");
    }
    touchPreferences.end();
}

bool pointInRect(int16_t x, int16_t y, int16_t left, int16_t top, int16_t width, int16_t height) {
    return x >= left && x < left + width && y >= top && y < top + height;
}

bool isHomeIcon(int16_t x, int16_t y) {
    // The visible Home bitmap is 24x24 at (4, 2). Keep a slightly larger,
    // finger-friendly target that remains inside the 32-pixel global top bar.
    constexpr int16_t homeTargetX = 0;
    constexpr int16_t homeTargetY = 0;
    constexpr int16_t homeTargetWidth = 40;
    constexpr int16_t homeTargetHeight = 32;
    return pointInRect(x, y, homeTargetX, homeTargetY,
                       homeTargetWidth, homeTargetHeight);
}

PageId mainPageAt(int16_t x, int16_t y) {
    constexpr int16_t iconSize = 48;
    constexpr int16_t hitPadding = 10;
    constexpr int16_t columnGap = 32;
    constexpr int16_t rowGap = 22;
    constexpr int16_t startX = (XingtaiEpd::WIDTH - iconSize * 3 - columnGap * 2) / 2;
    constexpr int16_t startY = 56;
    constexpr PageId pages[4][3] = {
        {PageId::Settings, PageId::Calendar, PageId::Calculator},
        {PageId::Clock, PageId::Book, PageId::Voice},
        {PageId::Music, PageId::Poem, PageId::Word},
        {PageId::Recording, PageId::Main, PageId::Main},
    };

    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 3; ++column) {
            if (row == 3 && column > 0) continue;
            const int16_t left = startX + column * (iconSize + columnGap) - hitPadding;
            const int16_t top = startY + row * (iconSize + rowGap) - hitPadding;
            if (pointInRect(x, y, left, top, iconSize + hitPadding * 2, iconSize + hitPadding * 2)) {
                return pages[row][column];
            }
        }
    }
    return PageId::Main;
}

void queuePage(PageId page) {
    if (page == currentPage || touchAction.pending) return;
    touchAction.page = page;
    touchAction.pending = true;
}

void handleTouch(TPoint point, TEvent event) {
    if (calibrationActive) {
        if (event == TEvent::Tap && calibrationCount < 3) {
            calibrationRaw[calibrationCount++] = point;
            Serial.printf("[CAL] target=%u raw=(%u,%u)\n", calibrationCount, point.x, point.y);
        }
        return;
    }
    int16_t x = 0;
    int16_t y = 0;
    if (!rawToDisplay(point, x, y)) {
        Serial.printf("[TOUCH] out of range x=%d y=%d raw_x=%u raw_y=%u event=%u\n",
                      x, y, point.x, point.y, static_cast<unsigned>(event));
        return;
    }

    // The EPD presents the framebuffer rotated 180 degrees relative to the
    // touch panel's calibrated physical coordinates.
    const int16_t uiX = XingtaiEpd::WIDTH - 1 - x;
    const int16_t uiY = XingtaiEpd::HEIGHT - 1 - y;

    if (event == TEvent::TouchStart) {
        touchGestureActive = true;
        touchGestureStartX = touchGestureLastX = uiX;
        touchGestureStartY = touchGestureLastY = uiY;
        return;
    }
    if (event == TEvent::TouchMove) {
        if (touchGestureActive) {
            touchGestureLastX = uiX;
            touchGestureLastY = uiY;
        }
        return;
    }
    if (event == TEvent::TouchEnd) {
        if (touchGestureActive) {
            touchGestureLastX = uiX;
            touchGestureLastY = uiY;
            const int16_t deltaX = touchGestureLastX - touchGestureStartX;
            const int16_t deltaY = touchGestureLastY - touchGestureStartY;
            touchGestureActive = false;
            const bool poemPopupOpen = currentPage == PageId::Poem && PoemPage::isPopupOpen();
            if ((poemPopupOpen || currentPage == PageId::Book ||
                 currentPage == PageId::Voice || currentPage == PageId::Music ||
                 currentPage == PageId::Poem || currentPage == PageId::Word) &&
                touchGestureStartY >= 32 &&
                (abs(deltaX) >= 35 || abs(deltaY) >= 35)) {
                Serial.printf("[TOUCH] SWIPE start=(%d,%d) end=(%d,%d) delta=(%d,%d)\n",
                              touchGestureStartX, touchGestureStartY,
                              touchGestureLastX, touchGestureLastY, deltaX, deltaY);
                const bool changed = poemPopupOpen
                    ? PoemPage::handleSwipe(deltaX, deltaY)
                    : currentPage == PageId::Book
                    ? BookPage::handleSwipe(deltaX, deltaY)
                    : currentPage == PageId::Voice
                        ? VoicePage::handleSwipe(deltaX, deltaY)
                        : currentPage == PageId::Music
                            ? MusicPage::handleSwipe(deltaX, deltaY)
                            : currentPage == PageId::Word
                                ? WordPage::handleSwipe(deltaX, deltaY)
                                : PoemPage::handleSwipe(deltaX, deltaY);
                if (changed) {
                    if (poemPopupOpen || (currentPage == PageId::Poem && PoemPage::isPopupOpen())) {
                        refreshPoemDisplay();
                    } else {
                        refreshCurrentPage();
                    }
                }
            }
        }
        return;
    }
    if (event != TEvent::Tap) return;
    Serial.printf("[TOUCH] TAP raw=(%u,%u) mapped=(%d,%d) ui=(%d,%d)\n",
                  point.x, point.y, x, y, uiX, uiY);

    // A touch interrupt raised during Voice playback is dedicated to stopping
    // audio and returning Wire to FT6336. Consume that same physical tap before
    // global navigation so it cannot leave a stale suppression flag behind.
    if ((currentPage == PageId::Voice || currentPage == PageId::Music ||
         currentPage == PageId::Poem || currentPage == PageId::Word) &&
        suppressNextAudioTap) {
        suppressNextAudioTap = false;
        Serial.println("[TOUCH IRQ] Playback-stop tap consumed");
        if (currentPage == PageId::Voice) refreshVoiceDirtyRows();
        else if (currentPage == PageId::Music) refreshMusicDirtyRows();
        else if (currentPage == PageId::Poem && PoemPage::handleTap(uiX, uiY)) refreshCurrentPage();
        else if (currentPage == PageId::Word && WordPage::handleTap(uiX, uiY)) {
            if (WordPage::takeReplayRefreshRequest()) refreshWordStrokeWindow(true);
            else refreshCurrentPage();
        }
        else refreshCurrentPage();
        return;
    }

    // Home is global, including while a poem popup overlays the library.
    // Handle it before popup-local routing so the overlay cannot consume it.
    if (isHomeIcon(uiX, uiY)) {
        Serial.println("[NAVIGATION] Home tapped");
        queuePage(PageId::Main);
        return;
    }

    // The poem reader is an in-place overlay while the page state remains the
    // library. Give the overlay first refusal so its controls cannot fall
    // through to the playlist underneath.
    if (currentPage == PageId::Poem && PoemPage::isPopupOpen()) {
        if (PoemPage::handleTap(uiX, uiY)) refreshPoemDisplay();
        return;
    }

    if (currentPage == PageId::Settings) {
        const SettingsPage::Action action = SettingsPage::actionAt(uiX, uiY);
        switch (action) {
        case SettingsPage::Action::OpenContentUrl:
            SettingsPage::showContentUrl(contentUrl);
            refreshCurrentPage();
            return;
        case SettingsPage::Action::OpenWifiSetup:
            if (!settingsState.wifiEnabled) return;
            scanWifiNetworks();
            return;
        case SettingsPage::Action::ToggleWifi:
            settingsState.wifiEnabled = !settingsState.wifiEnabled;
            applyWifiSetting(settingsState.wifiEnabled);
            applyCellularSetting();
            break;
        case SettingsPage::Action::RefreshSdCard:
            SettingsPage::setSdMounted(SdCard::isMounted());
            {
                const uint8_t count = SdCard::listRoot(sdEntryNames, sdEntryDirectories, 8);
                SettingsPage::showSdPage(sdEntryNames, sdEntryDirectories, count);
            }
            refreshCurrentPage();
            return;
        case SettingsPage::Action::SdBack:
            SettingsPage::showSettings();
            refreshCurrentPage();
            return;
        case SettingsPage::Action::SdRequestFormat:
            SettingsPage::setFormatPending(true);
            refreshCurrentPage();
            return;
        case SettingsPage::Action::SdConfirmFormat:
            if (SdCard::format()) Serial.println("[SD] Format complete");
            else Serial.println("[SD] Format failed");
            SettingsPage::setSdMounted(SdCard::isMounted());
            SettingsPage::showSettings();
            refreshCurrentPage();
            return;
        case SettingsPage::Action::CycleLanguage:
            settingsState.language = settingsState.language == 0 ? 1 : 0;
            UiLocalization::setLanguage(settingsState.language);
            break;
        case SettingsPage::Action::OpenVoiceSelection:
            SettingsPage::showVoiceSelection();
            refreshCurrentPage();
            return;
        case SettingsPage::Action::SelectVoice: {
            const uint8_t index = SettingsPage::voiceIndexAt(uiX, uiY);
            const char *voice = SettingsPage::voiceNameAt(index);
            if (!voice) return;
            SettingsPage::setVoice(voice);
            VoicePage::setVoice(SettingsPage::voiceName());
            saveSettings();
            Serial.printf("[SETTINGS] TTS voice %s\n", SettingsPage::voiceName());
            refreshCurrentPage();
            return;
        }
        case SettingsPage::Action::VoiceBack:
            SettingsPage::showSettings();
            refreshCurrentPage();
            return;
        case SettingsPage::Action::ToggleCellular:
            settingsState.cellularEnabled = !settingsState.cellularEnabled;
            applyCellularSetting();
            break;
        case SettingsPage::Action::VolumeDown:
            settingsState.volumePercent = settingsState.volumePercent >= 10
                ? settingsState.volumePercent - 10 : 0;
            applyAudioSetting();
            break;
        case SettingsPage::Action::VolumeUp:
            settingsState.volumePercent = settingsState.volumePercent <= 90
                ? settingsState.volumePercent + 10 : 100;
            applyAudioSetting();
            break;
        case SettingsPage::Action::TestAudio:
            playAudioTestBeep();
            return;
        case SettingsPage::Action::RetryWifiScan:
            scanWifiNetworks();
            return;
        case SettingsPage::Action::SelectWifiNetwork: {
            const uint8_t index = SettingsPage::networkIndexAt(uiX, uiY);
            if (index >= scannedWifiCount) return;
            std::strncpy(selectedWifiSsid, scannedWifiNetworks[index], sizeof(selectedWifiSsid) - 1);
            selectedWifiSsid[sizeof(selectedWifiSsid) - 1] = '\0';
            wifiPasswordInput[0] = '\0';
            SettingsPage::showWifiPassword(selectedWifiSsid, wifiPasswordInput);
            refreshCurrentPage();
            return;
        }
        case SettingsPage::Action::WifiKey: {
            const char key = SettingsPage::keyAt(uiX, uiY);
            const size_t length = std::strlen(wifiPasswordInput);
            if (key != '\0' && length < sizeof(wifiPasswordInput) - 1) {
                wifiPasswordInput[length] = key;
                wifiPasswordInput[length + 1] = '\0';
            }
            SettingsPage::showWifiPassword(selectedWifiSsid, wifiPasswordInput);
            refreshCurrentRegion(10, 88, 220, 38);
            return;
        }
        case SettingsPage::Action::WifiBackspace: {
            const size_t length = std::strlen(wifiPasswordInput);
            if (length > 0) wifiPasswordInput[length - 1] = '\0';
            SettingsPage::showWifiPassword(selectedWifiSsid, wifiPasswordInput);
            refreshCurrentRegion(10, 88, 220, 38);
            return;
        }
        case SettingsPage::Action::WifiSpace: {
            const size_t length = std::strlen(wifiPasswordInput);
            if (length < sizeof(wifiPasswordInput) - 1) {
                wifiPasswordInput[length] = ' ';
                wifiPasswordInput[length + 1] = '\0';
            }
            SettingsPage::showWifiPassword(selectedWifiSsid, wifiPasswordInput);
            refreshCurrentRegion(10, 88, 220, 38);
            return;
        }
        case SettingsPage::Action::WifiChangeKeyboard:
            SettingsPage::cycleKeyboard();
            refreshCurrentPage();
            return;
        case SettingsPage::Action::WifiConnect:
            connectSelectedWifi();
            return;
        case SettingsPage::Action::WifiCancel:
            SettingsPage::showSettings();
            refreshCurrentPage();
            return;
        case SettingsPage::Action::UrlKey: {
            const char key = SettingsPage::keyAt(uiX, uiY);
            const size_t length = std::strlen(contentUrl);
            if (key != '\0' && length < sizeof(contentUrl) - 1) {
                contentUrl[length] = key;
                contentUrl[length + 1] = '\0';
            }
            SettingsPage::showContentUrl(contentUrl);
            refreshCurrentRegion(10, 68, 220, 58);
            return;
        }
        case SettingsPage::Action::UrlBackspace: {
            const size_t length = std::strlen(contentUrl);
            if (length > CONTENT_URL_PREFIX_LENGTH) contentUrl[length - 1] = '\0';
            SettingsPage::showContentUrl(contentUrl);
            refreshCurrentRegion(10, 68, 220, 58);
            return;
        }
        case SettingsPage::Action::UrlSpace: {
            const size_t length = std::strlen(contentUrl);
            if (length < sizeof(contentUrl) - 1) {
                contentUrl[length] = ' ';
                contentUrl[length + 1] = '\0';
            }
            SettingsPage::showContentUrl(contentUrl);
            refreshCurrentRegion(10, 68, 220, 58);
            return;
        }
        case SettingsPage::Action::UrlChangeKeyboard:
            SettingsPage::cycleKeyboard();
            SettingsPage::showContentUrl(contentUrl);
            refreshCurrentPage();
            return;
        case SettingsPage::Action::UrlSave:
            if (contentUrlReachable(contentUrl)) {
                std::strncpy(savedContentUrl, contentUrl, sizeof(savedContentUrl) - 1);
                savedContentUrl[sizeof(savedContentUrl) - 1] = '\0';
                saveSettings();
                SettingsPage::showSettings();
                Serial.printf("[CONTENT URL] Saved reachable URL: %s\n", contentUrl);
            } else {
                std::strncpy(contentUrl, savedContentUrl, sizeof(contentUrl) - 1);
                contentUrl[sizeof(contentUrl) - 1] = '\0';
                SettingsPage::showContentUrl(contentUrl);
                Serial.printf("[CONTENT URL] New URL unreachable; restored: %s\n", contentUrl);
            }
            refreshCurrentPage();
            return;
        case SettingsPage::Action::UrlCancel:
            std::strncpy(contentUrl, savedContentUrl, sizeof(contentUrl) - 1);
            contentUrl[sizeof(contentUrl) - 1] = '\0';
            SettingsPage::showSettings();
            refreshCurrentPage();
            return;
        default:
            return;
        }
        saveSettings();
        SettingsPage::setState(settingsState);
        refreshCurrentPage();
        return;
    }

    if (currentPage == PageId::Calendar) {
        const CalendarPage::Action action = CalendarPage::actionAt(uiX, uiY);
        if (action != CalendarPage::Action::None) {
            CalendarPage::navigate(action);
            refreshCurrentPage();
        }
        return;
    }

    if (currentPage == PageId::Calculator) {
        if (CalculatorPage::handleTap(uiX, uiY)) refreshCurrentPage();
        return;
    }

    if (currentPage == PageId::Book) {
        if (BookPage::handleTap(uiX, uiY)) refreshCurrentPage();
        return;
    }

    if (currentPage == PageId::Voice) {
        if (VoicePage::handleTap(uiX, uiY)) {
            int8_t firstRow = -1;
            int8_t secondRow = -1;
            if (VoicePage::takeDirtyRows(firstRow, secondRow)) {
                // Put the rows back into the queue by rendering through the
                // dedicated helper immediately, without a content-area wipe.
                VoicePage::render(transitionFrame);
                auto refreshRow = [&](int8_t row) {
                    if (row < 0) return;
                    epaper.displayPartial(frame, transitionFrame, 12,
                                          82 + static_cast<uint16_t>(row) * 32,
                                          216, 30);
                };
                refreshRow(firstRow);
                refreshRow(secondRow);
                std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
                epaper.sleep();
            } else {
                refreshCurrentPage();
            }
        }
        return;
    }

    if (currentPage == PageId::Music) {
        if (MusicPage::handleTap(uiX, uiY)) {
            int8_t firstRow = -1;
            int8_t secondRow = -1;
            if (MusicPage::takeDirtyRows(firstRow, secondRow)) {
                MusicPage::render(transitionFrame);
                auto refreshRow = [&](int8_t row) {
                    if (row < 0) return;
                    epaper.displayPartial(frame, transitionFrame, 12,
                                          82 + static_cast<uint16_t>(row) * 32,
                                          216, 30);
                };
                refreshRow(firstRow);
                refreshRow(secondRow);
                std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
                epaper.sleep();
            } else {
                refreshCurrentPage();
            }
        }
        return;
    }

    if (currentPage == PageId::Poem) {
        const bool popupWasOpen = PoemPage::isPopupOpen();
        if (PoemPage::handleTap(uiX, uiY)) {
            if (popupWasOpen || PoemPage::isPopupOpen()) refreshPoemDisplay();
            else refreshCurrentPage();
        }
        return;
    }

    if (currentPage == PageId::Word) {
        if (WordPage::handleTap(uiX, uiY)) {
            if (WordPage::takeReplayRefreshRequest()) {
                refreshWordStrokeWindow(true);
            } else {
                refreshCurrentPage();
            }
        }
        return;
    }

    // Other content pages currently expose no page-specific touch actions.
    if (currentPage != PageId::Main) return;

    const PageId page = mainPageAt(uiX, uiY);
    if (page != PageId::Main) {
        Serial.printf("[FUNCTION ICON] Tapped: %s\n", pageName(page));
    }
    queuePage(page);
}

void powerTouch() {
    pinMode(BoardPins::TOUCH_PWR, OUTPUT);
    digitalWrite(BoardPins::TOUCH_PWR, LOW);
    delay(20);
    digitalWrite(BoardPins::TOUCH_PWR, HIGH);
    delay(200);
}

void beginSharedI2c() {
    Wire.begin(BoardPins::I2C_SDA, BoardPins::I2C_SCL, 100000);
    Wire.setTimeOut(50);
    Serial.printf("[I2C] Shared bus SDA=%d SCL=%d clock=%lu Hz\n",
                  BoardPins::I2C_SDA, BoardPins::I2C_SCL, 100000UL);

    uint8_t count = 0;
    for (uint8_t address = 1; address < 0x7F; ++address) {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C] Device found at 0x%02X%s%s\n", address,
                          (address == Es8311::DEFAULT_ADDRESS || address == Es8311::ALTERNATE_ADDRESS)
                              ? " (ES8311)" : "",
                          address == FT6X36_ADDR ? " (FT6X36)" : "");
            ++count;
            if (address == Es8311::DEFAULT_ADDRESS || address == Es8311::ALTERNATE_ADDRESS) {
                initialEs8311Present = true;
                initialEs8311Address = address;
            }
            if (address == FT6X36_ADDR) initialFt6x36Present = true;
        }
    }
    Serial.printf("[I2C] Scan complete: %u device(s)\n", count);
}

bool i2cDevicePresent(uint8_t address) {
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

void recoverSharedI2c() {
    Wire.end();

    // Release a slave that may be holding SDA after an interrupted transfer.
    pinMode(BoardPins::I2C_SDA, INPUT_PULLUP);
    pinMode(BoardPins::I2C_SCL, OUTPUT_OPEN_DRAIN);
    digitalWrite(BoardPins::I2C_SCL, HIGH);
    delayMicroseconds(5);
    for (uint8_t pulse = 0; pulse < 9 && digitalRead(BoardPins::I2C_SDA) == LOW; ++pulse) {
        digitalWrite(BoardPins::I2C_SCL, LOW);
        delayMicroseconds(5);
        digitalWrite(BoardPins::I2C_SCL, HIGH);
        delayMicroseconds(5);
    }

    Wire.begin(BoardPins::I2C_SDA, BoardPins::I2C_SCL, 100000);
    Wire.setTimeOut(50);
}

bool beginAudio() {
    const bool initialized = audio.begin(Es8311::DEFAULT_SAMPLE_RATE);
    if (initialized) {
        audio.setOutputVolume(settingsState.volumePercent);
        audio.setMicrophoneGain(30);
    }
    Serial.printf("[AUDIO] ES8311 address=0x%02X online=%s initialized=%s sample_rate=%lu "
                  "MCLK=%d SCLK=%d LRCLK=%d DOUT=%d DIN=%d PA=%d\n",
                  audio.address(), audio.isOnline() ? "yes" : "no",
                  initialized ? "yes" : "no", audio.sampleRate(),
                  BoardPins::AUDIO_MCLK, BoardPins::AUDIO_SCLK, BoardPins::AUDIO_LRCLK,
                  BoardPins::AUDIO_DOUT, BoardPins::AUDIO_DIN, BoardPins::PA_EN);
    return initialized;
}

bool beginTouch() {
    touch.registerIsrHandler(onTouchInterruptSignal);
    touch.registerTouchHandler(handleTouch);
    const bool online = touch.begin();
    Serial.printf("[TOUCH] FT6X36 address=0x%02X SDA=%d SCL=%d PWR=%d INT=%d online=%s "
                  "chip=0x%02X vendor=0x%02X firmware=0x%02X\n",
                  FT6X36_ADDR, BoardPins::I2C_SDA, BoardPins::I2C_SCL,
                  BoardPins::TOUCH_PWR, BoardPins::TOUCH_INT, online ? "yes" : "no",
                  touch.chipId(), touch.vendorId(), touch.firmwareVersion());
    return online;
}

void processTouchAction() {
    if (!touchAction.pending) return;

    const PageId nextPage = touchAction.page;
    touchAction.pending = false;

    if (currentPage == PageId::Clock && nextPage != PageId::Clock) {
        clockWeatherRequested = false;
    }
    if (currentPage == PageId::Voice && nextPage != PageId::Voice) {
        VoicePage::stopAudio();
    }
    if (currentPage == PageId::Music && nextPage != PageId::Music) {
        MusicPage::stopAudio();
    }
    if (currentPage == PageId::Poem && nextPage != PageId::Poem) {
        PoemPage::stopAudio();
    }
    if (currentPage == PageId::Word && nextPage != PageId::Word) {
        WordPage::stopAudio();
    }

    Serial.printf("[UI] Opening %s page\n", pageName(nextPage));
    if (nextPage == PageId::Main) {
        // Every Home return cleans and redraws only the content area. The
        // global topbar at y=0..31 remains physically and logically untouched.
        wipeContentAreaWhite();
    }
    if (nextPage == PageId::Settings) SettingsPage::showSettings();
    if (nextPage == PageId::Clock) prepareClockPage();
    if (nextPage == PageId::Book) {
        BookPage::setContentUrl(contentUrl);
        BookPage::openLibrary();
    }
    if (nextPage == PageId::Voice) {
        VoicePage::setContentUrl(contentUrl);
        VoicePage::setVoice(SettingsPage::voiceName());
        VoicePage::openLibrary();
    }
    if (nextPage == PageId::Music) {
        MusicPage::setContentUrl(contentUrl);
        MusicPage::open();
    }
    if (nextPage == PageId::Poem) {
        PoemPage::setContentUrl(contentUrl);
        PoemPage::setVoice(SettingsPage::voiceName());
        PoemPage::openLibrary();
    }
    if (nextPage == PageId::Word) {
        WordPage::setContentUrl(contentUrl);
        WordPage::setVoice(SettingsPage::voiceName());
        WordPage::open();
    }
    rendererFor(nextPage)(transitionFrame);

    // Keep the current top-bar pixels untouched and refresh only the function
    // page area below the 32-pixel bar.
    constexpr uint16_t topBarHeight = 32;
    constexpr size_t rowBytes = XingtaiEpd::WIDTH / 8;
    std::memcpy(transitionFrame, frame, static_cast<size_t>(topBarHeight) * rowBytes);
    epaper.displayPartial(frame, transitionFrame, 0, topBarHeight,
                          XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT - topBarHeight);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
    currentPage = nextPage;
    Serial.printf("[UI] %s page ready\n", pageName(currentPage));
}

}

void setup() {
    pinMode(BoardPins::WAKE, INPUT_PULLUP);
    const bool calibrationRequested = digitalRead(BoardPins::WAKE) == LOW;
    Serial.begin(115200);
    // Give the native USB CDC serial monitor time to attach after reset.
    // The timeout keeps the device booting even when no monitor is connected.
    const uint32_t serialWaitStart = millis();
    while (!Serial && millis() - serialWaitStart < 5000) {
        delay(10);
    }
    delay(100);
    Serial.println();
    Serial.println("========================================");
    Serial.println("[BOOT] ESP32-S3 e-paper startup");
    UiLoadingIndicator::setHandler(showTopbarLoading);
    Serial.printf("[BOOT] Serial ready: %lu baud\n", 115200UL);
    Serial.println("[BOOT] Firmware: 3.7-inch e-paper portrait test");
    Serial.printf("[BOOT] EPD pins: DIN=%d CLK=%d CS=%d DC=%d RST=%d BUSY=%d\n",
                  BoardPins::EP_DIN, BoardPins::EP_CLK, BoardPins::EP_CS,
                  BoardPins::EP_DC, BoardPins::EP_RST, BoardPins::EP_BUSY);
    Serial.println("[BOOT] Loading network settings...");
    loadSettings();
    Serial.println("[BOOT] Starting available Internet transports...");
    applyWifiSetting();
    applyCellularSetting();
    Serial.println("[BOOT] Network priority: WiFi first when enabled, otherwise 4G");
    if (!cellularProbeRunning) {
        xTaskCreate(cellularPollingTask, "ml307-poll", 6144, nullptr, 1, nullptr);
    }
    Serial.println("[BOOT] Checking SD card and Xiaozhi font cache...");
    SdCard::begin();
    SettingsPage::setSdMounted(SdCard::isMounted());
    XiaozhiFont::beginBackgroundProvisioning();
    Serial.println("[BOOT] Powering touch controller...");
    powerTouch();
    Serial.println("[BOOT] Initializing shared I2C bus...");
    beginSharedI2c();
    Serial.println("[BOOT] Initializing ES8311 audio codec...");
    beginAudio();
    VoicePage::setAudio(&audio);
    VoicePage::setVoice(SettingsPage::voiceName());
    PoemPage::setAudio(&audio);
    PoemPage::setVoice(SettingsPage::voiceName());
    applyAudioSetting();
    Serial.println("[BOOT] ES8311 audio initialization complete");
    Serial.println("[BOOT] Initializing touch controller...");
    beginTouch();
    Serial.println("[BOOT] Touch controller initialization complete");

    Serial.println("[BOOT] Initializing e-paper controller...");
    epaper.begin();
    recoverSharedI2c();
    loadCalibration();
    if (calibrationRequested) {
        calibrationActive = true;
        calibrationCount = 0;
        Serial.println("[CAL] WAKE held: touch calibration enabled");
        Serial.println("[CAL] Tap targets: top-left, top-right, bottom-center");
        for (uint8_t target = 0; target < 3; ++target) {
            renderCalibrationTarget(frame, target);
            epaper.display(frame);
            // The EPD and touch controller share the board's power-up sequence;
            // recover the bus after each EPD refresh before polling FT6X36.
            recoverSharedI2c();
            while (calibrationCount <= target) {
                touch.loop();
                delay(5);
            }
        }
        calibrationActive = false;
        if (solveCalibration()) Serial.println("[CAL] Calibration saved to flash");
        else Serial.println("[CAL] Calibration invalid; using previous transform");
    } else {
        Serial.println("[CAL] Using saved touch calibration");
    }
    Serial.println("[BOOT] Rendering Asundar startup logo...");
    AsundarPage::render(frame);
    epaper.display(frame);
    epaper.sleep();
    delay(900);
    recoverSharedI2c();

    Serial.println("[BOOT] Rendering main page frame...");
    // Always establish a clean initial top bar. The runtime state monitor adds
    // Wi-Fi only after the Home frame is physically present and connectivity
    // has been observed in the normal event loop.
    lastWifiConnected = false;
    MainPage::setWifiConnected(false);
    MainPage::render(transitionFrame);
    Serial.println("[BOOT] Transitioning to main page...");
    epaper.displayPartial(frame, transitionFrame, 0, 0,
                          XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    Serial.println("[BOOT] Putting e-paper controller to sleep...");
    epaper.sleep();
    Serial.println("[BOOT] Main page refresh complete");
    recoverSharedI2c();
    Serial.printf("[I2C] Bus recovered after display startup: SDA=%d SCL=%d\n",
                  digitalRead(BoardPins::I2C_SDA), digitalRead(BoardPins::I2C_SCL));
    Serial.printf("[STATUS] I2C ES8311@0x%02X=%s FT6X36@0x%02X=%s\n",
                  audio.address(), i2cDevicePresent(audio.address()) ? "present" : "missing",
                  FT6X36_ADDR, i2cDevicePresent(FT6X36_ADDR) ? "present" : "missing");
    Serial.printf("[STATUS] Initial scan ES8311=%s address=0x%02X FT6X36=%s\n",
                  initialEs8311Present ? "present" : "missing", initialEs8311Address,
                  initialFt6x36Present ? "present" : "missing");
    Serial.printf("[STATUS] AUDIO online=%s initialized=%s; TOUCH online=%s INT=%d\n",
                  audio.isOnline() ? "yes" : "no", audio.isInitialized() ? "yes" : "no",
                  touch.isOnline() ? "yes" : "no", digitalRead(BoardPins::TOUCH_INT));
    Serial.println("[BOOT] Startup sequence complete; entering touch monitoring");
    Serial.println("========================================");
}

void loop() {
    SdCard::processSerialCommand();
    serviceTouchInterruptBeforeI2c();
    touch.loop();
    // If the controller emitted no final Tap event, still restore the stopped
    // Voice row after the interrupt-driven audio shutdown.
    if (currentPage == PageId::Voice) refreshVoiceDirtyRows();
    if (currentPage == PageId::Music) refreshMusicDirtyRows();
    if (currentPage == PageId::Poem) refreshPoemDirtyRows();
    processTouchAction();
    BookPage::processPendingSave();
    VoicePage::processPendingSave();
    PoemPage::processPendingSave();
    if (VoicePage::processAudio() && currentPage == PageId::Voice) {
        refreshVoiceDirtyRows();
    }
    if (MusicPage::processAudio() && currentPage == PageId::Music) {
        refreshMusicDirtyRows();
    }
    if (PoemPage::processAudio() && currentPage == PageId::Poem) {
        refreshCurrentPage();
    }
    if (currentPage == PageId::Word && WordPage::processAnimation()) {
        refreshWordStrokeWindow(false);
    }
    int16_t marqueeTop = 0;
    if (VoicePage::advanceMarquee(marqueeTop) && currentPage == PageId::Voice) {
        VoicePage::renderMarquee(transitionFrame, frame);
        epaper.displayPartial(frame, transitionFrame, 34, marqueeTop + 1, 171, 28);
        std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
        epaper.sleep();
    }
    if (MusicPage::advanceMarquee(marqueeTop) && currentPage == PageId::Music) {
        MusicPage::renderMarquee(transitionFrame, frame);
        epaper.displayPartial(frame, transitionFrame, 34, marqueeTop + 2, 167, 26);
        std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
        epaper.sleep();
    }
    if (PoemPage::advanceMarquee(marqueeTop) && currentPage == PageId::Poem) {
        PoemPage::renderMarquee(transitionFrame, frame);
        epaper.displayPartial(frame, transitionFrame, 34, marqueeTop + 1, 171, 28);
        std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
        epaper.sleep();
    }
    updateNetworkPriority();
    updateWifiTopbar();
    updateClockPage();
    delay(5);
}
