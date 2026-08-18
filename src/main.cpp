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
#include "devices/ml307/ml307.h"
#include "devices/sd_card/sd_card.h"
#include "font/xiaozhi_font.h"
#include "memory_budget.h"
#include "pages/asundar/asundar_page.h"
#include "pages/book/book_page.h"
#include "pages/calendar/calendar_page.h"
#include "pages/calculator/calculator_page.h"
#include "pages/cartoon/cartoon_page.h"
#include "pages/clock/clock_page.h"
#include "pages/word/word_page.h"
#include "pages/main/main_page.h"
#include "pages/music/music_page.h"
#include "pages/poem/poem_page.h"
#include "pages/playlist_cache.h"
#include "pages/recording/recording_page.h"
#include "pages/radio/radio_page.h"
#include "pages/settings/settings_page.h"
#include "pages/topbar/topbar_assets.h"
#include "pages/topbar/topbar_bitmap.h"
#include "pages/voice/voice_page.h"
#include "ui/loading_indicator.h"
#include "ui/localization.h"

XingtaiEpd epaper;
FT6X36 touch(&Wire, BoardPins::TOUCH_INT);
Es8311 audio(
    Wire,
    {BoardPins::AUDIO_MCLK, BoardPins::AUDIO_SCLK, BoardPins::AUDIO_LRCLK,
     BoardPins::AUDIO_DIN, BoardPins::AUDIO_DOUT, BoardPins::AUDIO_CE,
     BoardPins::PA_EN, -1});

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
    Cartoon,
    Radio,
};

struct TouchAction {
    PageId page = PageId::Main;
    bool pending = false;
};

static uint8_t frame[XingtaiEpd::FRAME_BYTES];
static uint8_t transitionFrame[XingtaiEpd::FRAME_BYTES];
static uint8_t *calculatorRefreshFrame = nullptr;
Preferences touchPreferences;
Preferences settingsPreferences;
Preferences wifiPreferences;
bool calibrationActive = false;
uint8_t calibrationCount = 0;
TPoint calibrationRaw[4] = {};
// Keep the targets a few pixels inside the physical edges so the crosshair is
// fully visible and the user can touch its center without hitting the bezel.
constexpr int16_t calibrationTargetX[4] = {20, 220, 220, 20};
constexpr int16_t calibrationTargetY[4] = {40, 40, 376, 376};
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
SettingsPage::State settingsState = {true, true, false, 60, 0};
MainPage::NetworkMode lastNetworkMode = MainPage::NetworkMode::None;
volatile bool cellularProbeRunning = false;
TaskHandle_t audioTestTaskHandle = nullptr;
volatile bool audioTestStopRequested = false;
uint32_t wifiPriorityStartedMs = 0;
uint32_t lastWifiReconnectAttemptMs = 0;
constexpr uint32_t WIFI_PRIORITY_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 15000;
constexpr uint32_t CELLULAR_DISCONNECTED_POLL_MS = 10000;
constexpr uint32_t CELLULAR_CONNECTED_POLL_MS = 30000;
constexpr uint32_t SHARED_I2C_CLOCK_HZ = 100000;
constexpr uint32_t CALCULATOR_I2C_CLOCK_HZ = 400000;
constexpr uint16_t SHARED_I2C_TIMEOUT_MS = 50;
constexpr uint16_t CALCULATOR_I2C_TIMEOUT_MS = 10;
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
volatile uint32_t touchInterruptTick = 0;
volatile bool touchWorkflowPriority = false;
PageId openingLoadPage = PageId::Main;
bool openingLoadVisible = false;
bool suppressNextAudioTap = false;
bool suppressMainIconReleaseTap = false;
bool suppressDriverTap = false;
bool dispatchingSyntheticTap = false;
bool homeOutlinePressed = false;
bool homeTouchActive = false;
bool priorityControlFeedbackShown = false;
int16_t priorityControlLeft = 0;
int16_t priorityControlTop = 0;
int16_t priorityControlWidth = 0;
int16_t priorityControlHeight = 0;
volatile bool calculatorRefreshRunning = false;
volatile bool calculatorRefreshPending = false;
TaskHandle_t uiTaskHandle = nullptr;
bool calculatorPressHandled = false;
bool immediateControlPressHandled = false;

void recoverSharedI2c();
void setCalculatorI2cPriority(bool enabled);
void refreshCurrentPage();
void refreshCurrentRegion(uint16_t x, uint16_t y, uint16_t width, uint16_t height);
void startCalculatorRefresh();
void refreshBookReaderContent();
void refreshBookLibraryContent();
bool refreshPendingBookOpenPressed();
void openLocalBookReaderFast();
void wipeContentAreaWhite(bool sleepAfter = true);
void refreshVoiceDirtyRows();
void refreshMusicDirtyRows();
void refreshPoemDirtyRows();
void refreshPoemDisplay();
void refreshWordStrokeWindow(bool wipeFirst);
void queuePage(PageId page);
void startMainIconFeedback(PageId page);
void showHomePressedOutline();
void restoreHomeIcon();
void clearFrameRegion(uint8_t *buffer, int left, int top, int width, int height);
void copyFrameRegion(uint8_t *destination, const uint8_t *source,
                     int left, int top, int width, int height);
void showTopbarLoading(bool visible);
void processOpeningLibraryLoads();
void startOpeningLibraryLoad(PageId page);
void refreshOpeningLibraryContent(PageId page);

void onRecordingTimerEvent() {
    if (uiTaskHandle) xTaskNotifyGive(uiTaskHandle);
}

void IRAM_ATTR onTouchInterruptSignal() {
    touchInterruptPending = true;
    touchInterruptTick = xTaskGetTickCountFromISR();
    touchWorkflowPriority = true;
}

void serviceTouchInterruptBeforeI2c() {
    bool pending = false;
    uint32_t interruptTick = 0;
    noInterrupts();
    pending = touchInterruptPending;
    interruptTick = touchInterruptTick;
    touchInterruptPending = false;
    interrupts();
    const bool voiceActive = currentPage == PageId::Voice && VoicePage::isAudioActive();
    const bool musicActive = currentPage == PageId::Music && MusicPage::isAudioActive();
    const bool poemActive = currentPage == PageId::Poem && PoemPage::isAudioActive();
    // Stroke animation does not use the shared audio/touch I2C path. Including
    // it here allowed a trailing IRQ from the opening tap to cancel the drawing.
    const bool wordActive = currentPage == PageId::Word && WordPage::isAudioActive();
    if (!pending || (!voiceActive && !musicActive && !poemActive && !wordActive)) return;
    if ((voiceActive || musicActive || poemActive || WordPage::isAudioActive()) &&
        !OpusPlayer::acceptsTouchStop(interruptTick)) {
        suppressNextAudioTap = true;
        Serial.println("[TOUCH IRQ] Ignoring play-tap interrupt after audio start");
        return;
    }

    Serial.printf("[TOUCH IRQ] Stopping %s audio before FT6336 I2C read\n",
                   voiceActive ? "Voice" : musicActive ? "Music" : poemActive ? "Poem" : "Word");
    if (voiceActive) VoicePage::stopAudioFromTouchInterrupt();
    else if (musicActive) MusicPage::stopAudioFromTouchInterrupt();
    else if (poemActive) PoemPage::stopAudioFromTouchInterrupt();
    else WordPage::stopFromTouchInterrupt();
    // Audio codec setup and FT6336 share Wire. Reclock/recover the bus only
    // after the audio task has stopped, then let touch.loop() read coordinates.
    recoverSharedI2c();
    // Do not consume this tap: touch.loop() still needs to read its coordinates
    // so normal routing can distinguish same row, another row, or Home.
    Serial.printf("[TOUCH IRQ] I2C returned to FT6336 SDA=%d SCL=%d INT=%d\n",
                  digitalRead(BoardPins::I2C_SDA), digitalRead(BoardPins::I2C_SCL),
                  digitalRead(BoardPins::TOUCH_INT));
}

void cellularPollingTask(void *) {
    cellularProbeRunning = true;
    cellularModem.begin();
    Serial.println("[ML307] Connection poller started");
    while (true) {
        if (!settingsState.cellularEnabled || !cellularModem.isPowered() ||
            WiFi.status() == WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const bool wasConnected = cellularModem.isConnected();
        const bool connected = cellularModem.probeConnection();
        if (wasConnected != connected) {
            Serial.printf("[NETWORK] 4G %s\n", connected ? "connected" : "disconnected");
        } else if (!connected) {
            Serial.println("[NETWORK] Neither WiFi nor 4G connected; polling continues");
        }
        vTaskDelay(pdMS_TO_TICKS(connected
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
    settingsPreferences.putBool("playlistCache", settingsState.playlistCacheEnabled);
    settingsPreferences.putUChar("volume", settingsState.volumePercent);
    settingsPreferences.putUChar("language", settingsState.language);
    settingsPreferences.putString("contentUrl", contentUrl);
    settingsPreferences.putString("ttsVoice", SettingsPage::voiceName());
    settingsPreferences.end();
}

bool normalizeContentUrl(char *url, size_t capacity) {
    if (!url || capacity == 0 || url[0] == '\0') return false;
    if (std::strncmp(url, "http://", 7) == 0 ||
        std::strncmp(url, "https://", 8) == 0) {
        return true;
    }

    const size_t length = std::strlen(url);
    if (length + CONTENT_URL_PREFIX_LENGTH >= capacity) return false;
    std::memmove(url + CONTENT_URL_PREFIX_LENGTH, url, length + 1);
    std::memcpy(url, CONTENT_URL_PREFIX, CONTENT_URL_PREFIX_LENGTH);
    return true;
}

bool contentUrlReachable(const char *url) {
    if (!url || std::strlen(url) <= CONTENT_URL_PREFIX_LENGTH) {
        return false;
    }

    UiLoadingIndicator::Scope loadingIndicator;
    if (WiFi.status() != WL_CONNECTED) {
        String payload;
        const bool reachable = cellularModem.httpGet(url, payload, 10000);
        Serial.printf("[CONTENT URL] 4G validation %s %s\n", url,
                      reachable ? "succeeded" : "failed");
        return reachable;
    }
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
    constexpr char WEATHER_URL[] =
        "http://wttr.in/?format=%25l%7C%25C%7C%25t%7C%25h%7C%25w";
    String response;
    if (WiFi.status() != WL_CONNECTED) {
        if (!cellularModem.httpGet(WEATHER_URL, response, 15000)) return false;
    } else {
        HTTPClient http;
        http.setConnectTimeout(7000);
        http.setTimeout(7000);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.setUserAgent("ESP32-ePaper-Clock/1.0");
        if (!http.begin(WEATHER_URL)) return false;
        const int responseCode = http.GET();
        response = responseCode > 0 ? http.getString() : String();
        http.end();
        if (responseCode < 200 || responseCode >= 400) {
            Serial.printf("[CLOCK] Weather request failed with HTTP code %d\n", responseCode);
            return false;
        }
    }

    // wttr.in resolves the public IP to a location and returns a deliberately
    // compact, pipe-separated response:
    // location|condition|temperature|humidity|wind.
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
            UiLoadingIndicator::Scope loadingIndicator;
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
    cellularModem.setPowered(shouldPower);
    Serial.printf("[NETWORK] 4G modem power %s%s\n",
                  shouldPower ? "enabled" : "disabled",
                  settingsState.cellularEnabled && settingsState.wifiEnabled
                      ? " (WiFi has priority)" : "");
}

void updateNetworkPriority() {
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;

    if (!settingsState.cellularEnabled) {
        if (cellularModem.isPowered()) {
            cellularModem.setPowered(false);
            Serial.println("[NETWORK] 4G modem power disabled by setting");
        }
        return;
    }

    if (!settingsState.wifiEnabled) {
        if (!cellularModem.isPowered()) {
            cellularModem.setPowered(true);
            Serial.println("[NETWORK] WiFi is off; 4G modem powered immediately");
        }
        return;
    }

    if (wifiConnected) {
        if (cellularModem.isPowered()) {
            cellularModem.setPowered(false);
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
                      cellularModem.isConnected() ? " while 4G remains active" : "");
        WiFi.disconnect(false, false);
        WiFi.begin(savedWifiSsid, savedWifiPassword);
    }

    const bool noSavedWifi = savedWifiSsid[0] == '\0';
    const bool wifiTimedOut = wifiPriorityStartedMs != 0 &&
        millis() - wifiPriorityStartedMs >= WIFI_PRIORITY_TIMEOUT_MS;
    if ((noSavedWifi || wifiTimedOut) && !cellularModem.isPowered()) {
        cellularModem.setPowered(true);
        Serial.printf("[NETWORK] WiFi unavailable after priority check; 4G modem powered%s\n",
                      noSavedWifi ? " (no saved WiFi)" : " after 15s timeout");
    }
}

void applyAudioSetting() {
    if (audio.isInitialized()) audio.setOutputVolume(settingsState.volumePercent);
    audio.setSpeakerEnabled(settingsState.volumePercent > 0);
    Serial.printf("[SETTINGS] Audio volume %u%%\n", settingsState.volumePercent);
}

void audioTestTask(void *) {
    constexpr uint32_t sampleRate = Es8311::DEFAULT_SAMPLE_RATE;
    constexpr uint32_t frequency = 880;
    constexpr size_t frameCount = 256;
    constexpr float amplitude = 12000.0f;
    int16_t samples[frameCount * 2] = {};
    float phase = 0.0f;
    const float phaseStep = 2.0f * PI * frequency / sampleRate;

    pinMode(BoardPins::PA_EN, OUTPUT);
    digitalWrite(BoardPins::PA_EN, HIGH);
    audio.setSpeakerEnabled(true);
    vTaskDelay(pdMS_TO_TICKS(60));
    Serial.printf("[AUDIO TEST] Continuous beep started PA_EN=%d level=%d\n",
                  BoardPins::PA_EN, digitalRead(BoardPins::PA_EN));

    while (!audioTestStopRequested) {
        for (size_t frame = 0; frame < frameCount; ++frame) {
            const int16_t sample = static_cast<int16_t>(sinf(phase) * amplitude);
            samples[frame * 2] = sample;
            samples[frame * 2 + 1] = sample;
            phase += phaseStep;
            if (phase >= 2.0f * PI) phase -= 2.0f * PI;
        }
        if (audio.write(samples, frameCount * 2, 500) != frameCount * 2) {
            Serial.println("[AUDIO TEST] Continuous PCM write failed");
            break;
        }
    }

    std::memset(samples, 0, sizeof(samples));
    audio.write(samples, frameCount * 2, 500);
    audio.setSpeakerEnabled(false);
    digitalWrite(BoardPins::PA_EN, LOW);
    Serial.printf("[AUDIO TEST] Continuous beep stopped PA_EN=%d level=%d\n",
                  BoardPins::PA_EN, digitalRead(BoardPins::PA_EN));
    // The beep may end through the normal toggle or an error exit; either way
    // the settings page must drop the bold highlight on the test button.
    SettingsPage::setAudioTestActive(false);
    audioTestTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

void playAudioTestBeep() {
    if (audioTestTaskHandle) {
        audioTestStopRequested = true;
        const uint32_t started = millis();
        while (audioTestTaskHandle && millis() - started < 1500) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        Serial.println(audioTestTaskHandle
                           ? "[AUDIO TEST] Stop timed out"
                           : "[AUDIO TEST] Continuous beep toggled off");
        // The beep task clears the active flag on exit; repaint the settings
        // test-button row so the label is no longer bold.
        if (!audioTestTaskHandle && currentPage == PageId::Settings) {
            refreshCurrentRegion(16, 58 + 7 * 36, 208, 32);
        }
        return;
    }

    if (!audio.isInitialized() || settingsState.volumePercent == 0) {
        Serial.println("[AUDIO TEST] Codec unavailable or volume is muted");
        return;
    }

    OpusPlayer::stop();
    audio.setOutputVolume(settingsState.volumePercent);
    audioTestStopRequested = false;
    if (xTaskCreatePinnedToCore(audioTestTask, "audio-test", 4096, nullptr, 2,
                                &audioTestTaskHandle, 0) != pdPASS) {
        audioTestTaskHandle = nullptr;
        Serial.println("[AUDIO TEST] Could not create continuous beep task");
        return;
    }
    SettingsPage::setAudioTestActive(true);
    if (currentPage == PageId::Settings) {
        // Bold the settings test button while the beep is playing.
        refreshCurrentRegion(16, 58 + 7 * 36, 208, 32);
    }
    Serial.printf("[AUDIO TEST] Continuous beep toggled on at %u%% volume\n",
                  settingsState.volumePercent);
}

void loadSettings() {
    bool migratedContentUrl = false;
    if (settingsPreferences.begin("settings", true)) {
        settingsState.wifiEnabled = settingsPreferences.getBool("wifi", true);
        settingsState.cellularEnabled = settingsPreferences.getBool("cellular", true);
        settingsState.playlistCacheEnabled = settingsPreferences.getBool("playlistCache", false);
        settingsState.volumePercent = settingsPreferences.getUChar("volume", 60);
        settingsState.language = settingsPreferences.getUChar("language", 0);
        const String savedContentUrl = settingsPreferences.getString("contentUrl", "");
        const String savedVoice = settingsPreferences.getString("ttsVoice", "Jasper");
        std::strncpy(contentUrl, savedContentUrl.c_str(), sizeof(contentUrl) - 1);
        if (contentUrl[0] == '\0') {
            std::strcpy(contentUrl, CONTENT_URL_PREFIX);
        } else {
            const bool hadScheme = std::strncmp(contentUrl, "http://", 7) == 0 ||
                                   std::strncmp(contentUrl, "https://", 8) == 0;
            if (!hadScheme && normalizeContentUrl(contentUrl, sizeof(contentUrl))) {
                migratedContentUrl = true;
            }
        }
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
    PlaylistCache::setEnabled(settingsState.playlistCacheEnabled);
    SettingsPage::setState(settingsState);
    UiLocalization::setLanguage(settingsState.language);
    loadWifiCredentials();
    if (migratedContentUrl) saveSettings();
}

void scanWifiNetworks() {
    UiLoadingIndicator::Scope loadingIndicator;
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
    UiLoadingIndicator::Scope loadingIndicator;
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
    case PageId::Cartoon: return "Cartoon";
    case PageId::Radio: return "Radio";
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
    case PageId::Cartoon: return CartoonPage::render;
    case PageId::Radio: return RadioPage::render;
    default: return MainPage::render;
    }
}

void refreshCurrentPage() {
    rendererFor(currentPage)(transitionFrame);
    constexpr size_t topBarBytes = static_cast<size_t>(32) * (XingtaiEpd::WIDTH / 8);
    std::memcpy(transitionFrame, frame, topBarBytes);
    const bool enteringBookReader = currentPage == PageId::Book && BookPage::isReader();
    if (currentPage == PageId::Book || currentPage == PageId::Voice ||
        currentPage == PageId::Music || currentPage == PageId::Poem ||
        currentPage == PageId::Word) {
        // Drive the old SD-backed content area white before drawing its next
        // list/detail/page state, reducing ghosted text and borders.
        wipeContentAreaWhite(enteringBookReader ? false : true);
    }
    if (enteringBookReader) {
        // The reader replaces the library only below the fixed top bar. Wipe
        // that region first, then draw the reader without touching the bar.
        epaper.displayPartial(frame, transitionFrame, 0, 32,
                              XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT - 32);
    } else {
        epaper.displayPartial(frame, transitionFrame, 0, 32,
                              XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT - 32);
    }
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
}

void wipeContentAreaWhite(bool sleepAfter) {
    constexpr uint16_t topBarHeight = 32;
    constexpr size_t topBarBytes = static_cast<size_t>(topBarHeight) *
                                   (XingtaiEpd::WIDTH / 8);
    epaper.displayPartialFill(frame, 0x00, 0, 32,
                              XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT - 32);
    // Keep software state synchronized with the physical white pre-drive so
    // the following partial update compares against the panel's actual pixels.
    std::memset(frame + topBarBytes, 0x00,
                XingtaiEpd::FRAME_BYTES - topBarBytes);
    if (sleepAfter) epaper.sleep();
}

void refreshCurrentRegion(uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    rendererFor(currentPage)(transitionFrame);
    constexpr size_t topBarBytes = static_cast<size_t>(32) * (XingtaiEpd::WIDTH / 8);
    std::memcpy(transitionFrame, frame, topBarBytes);
    epaper.displayPartial(frame, transitionFrame, x, y, width, height);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
}

void calculatorRefreshTask(void *) {
    constexpr uint16_t displayX = 8;
    constexpr uint16_t displayY = 38;
    constexpr uint16_t displayWidth = 224;
    constexpr uint16_t displayHeight = 68;

    epaper.displayPartial(frame, calculatorRefreshFrame, displayX, displayY,
                          displayWidth, displayHeight);
    copyFrameRegion(frame, calculatorRefreshFrame, displayX, displayY,
                    displayWidth, displayHeight);
    epaper.sleep();
    free(calculatorRefreshFrame);
    calculatorRefreshFrame = nullptr;
    calculatorRefreshRunning = false;
    vTaskDelete(nullptr);
}

void startCalculatorRefresh() {
    if (currentPage != PageId::Calculator || calculatorRefreshRunning ||
        !calculatorRefreshPending) {
        return;
    }

    calculatorRefreshPending = false;
    calculatorRefreshFrame = static_cast<uint8_t *>(malloc(XingtaiEpd::FRAME_BYTES));
    if (!calculatorRefreshFrame) {
        calculatorRefreshPending = true;
        Serial.printf("[CALCULATOR] Refresh buffer allocation failed; free_heap=%u largest_block=%u\n",
                      static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(ESP.getMaxAllocHeap()));
        return;
    }
    CalculatorPage::render(calculatorRefreshFrame);
    calculatorRefreshRunning = true;
    if (xTaskCreate(calculatorRefreshTask, "calculator-epd", 4096,
                    nullptr, 1, nullptr) != pdPASS) {
        calculatorRefreshRunning = false;
        calculatorRefreshPending = true;
        free(calculatorRefreshFrame);
        calculatorRefreshFrame = nullptr;
        Serial.printf("[CALCULATOR] Display worker start failed; free_heap=%u largest_block=%u\n",
                      static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(ESP.getMaxAllocHeap()));
    }
}

void refreshCartoonLayout() {
    if (CartoonPage::isReader()) {
        CartoonPage::render(transitionFrame);
        constexpr uint16_t contentTop = 32;
        constexpr size_t topBarBytes = static_cast<size_t>(contentTop) *
                                       (XingtaiEpd::WIDTH / 8);
        std::memcpy(transitionFrame, frame, topBarBytes);
        epaper.displayPartialFill(frame, 0x00, 0, contentTop,
                                  XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT - contentTop);
        std::memset(frame + topBarBytes, 0x00,
                    XingtaiEpd::FRAME_BYTES - topBarBytes);
        epaper.sleep();
        epaper.displayPartial(frame, transitionFrame, 0, contentTop,
                              XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT - contentTop);
        std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
        epaper.sleep();
        return;
    }
    // Cartoon playlist, chapter list, and reader use different controls and
    // borders. White-drive their complete shared layout area before drawing the
    // destination view so no rows or image pixels remain underneath it. Keep
    // the global top bar and the unused bottom edge untouched. Keep two guard
    // rows below the 32 px bar because this panel's wide partial waveform can
    // disturb pixels immediately adjacent to the configured window.
    constexpr uint16_t contentX = 0;
    constexpr uint16_t contentY = 34;
    constexpr uint16_t contentWidth = XingtaiEpd::WIDTH;
    constexpr uint16_t contentBottom = 410;
    constexpr uint16_t contentHeight = contentBottom - contentY;

    CartoonPage::render(transitionFrame);
    clearFrameRegion(transitionFrame, 0, 0, XingtaiEpd::WIDTH, contentY);
    Topbar::drawHome(transitionFrame, 4, 2);
    if (WiFi.status() == WL_CONNECTED) Topbar::drawWifi(transitionFrame, 181, 2);
    else if (cellularModem.isConnected()) Topbar::draw4G(transitionFrame, 184, 2);
    Topbar::drawBattery(transitionFrame, 209, 2);
    copyFrameRegion(transitionFrame, frame, 0, contentBottom,
                    XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT - contentBottom);

    epaper.displayPartialFill(frame, 0x00, contentX, contentY,
                              contentWidth, contentHeight);
    clearFrameRegion(frame, contentX, contentY, contentWidth, contentHeight);
    epaper.sleep();
    epaper.displayPartial(frame, transitionFrame, contentX, contentY,
                          contentWidth, contentHeight);
    copyFrameRegion(frame, transitionFrame, contentX, contentY,
                    contentWidth, contentHeight);
    epaper.sleep();
}

void refreshCartoonListContent(int controlLeft, int controlTop,
                               int controlWidth, int controlHeight) {
    constexpr uint16_t contentX = 12;
    constexpr uint16_t contentY = 82;
    constexpr uint16_t contentWidth = 216;
    constexpr uint16_t contentHeight = 320;
    constexpr uint16_t counterX = 78;
    constexpr uint16_t counterY = 54;
    constexpr uint16_t counterWidth = 84;
    constexpr uint16_t counterHeight = 20;

    CartoonPage::render(transitionFrame);
    epaper.displayPartialFill(frame, 0x00, contentX, contentY,
                              contentWidth, contentHeight);
    clearFrameRegion(frame, contentX, contentY, contentWidth, contentHeight);
    epaper.sleep();
    epaper.displayPartial(frame, transitionFrame, contentX, contentY,
                          contentWidth, contentHeight);
    epaper.displayPartial(frame, transitionFrame, counterX, counterY,
                          counterWidth, counterHeight);
    if (controlWidth > 0 && controlHeight > 0) {
        epaper.displayPartial(frame, transitionFrame, controlLeft, controlTop,
                              controlWidth, controlHeight);
    }
    copyFrameRegion(frame, transitionFrame, contentX, contentY,
                    contentWidth, contentHeight);
    copyFrameRegion(frame, transitionFrame, counterX, counterY,
                    counterWidth, counterHeight);
    if (controlWidth > 0 && controlHeight > 0) {
        copyFrameRegion(frame, transitionFrame, controlLeft, controlTop,
                        controlWidth, controlHeight);
    }
    epaper.sleep();
}

void refreshCartoonImageContent() {
    constexpr uint16_t contentTop = 32;
    constexpr size_t topBarBytes = static_cast<size_t>(contentTop) *
                                   (XingtaiEpd::WIDTH / 8);
    CartoonPage::render(transitionFrame);
    std::memcpy(transitionFrame, frame, topBarBytes);
    epaper.displayPartialFill(frame, 0x00, 0, contentTop,
                              XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT - contentTop);
    std::memset(frame + topBarBytes, 0x00,
                XingtaiEpd::FRAME_BYTES - topBarBytes);
    epaper.sleep();
    epaper.displayPartial(frame, transitionFrame, 0, contentTop,
                          XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT - contentTop);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
}

void refreshBookReaderContent() {
    constexpr uint16_t contentX = 10;
    constexpr uint16_t contentY = 82;
    constexpr uint16_t contentWidth = 220;
    constexpr uint16_t contentHeight = 296;
    // The centered counter occupies the gap between the fixed arrow buttons.
    constexpr uint16_t counterX = 50;
    constexpr uint16_t counterY = 390;
    constexpr uint16_t counterWidth = 140;
    constexpr uint16_t counterHeight = 18;
    BookPage::render(transitionFrame);
    epaper.displayPartialFill(frame, 0x00, contentX, contentY,
                              contentWidth, contentHeight);
    clearFrameRegion(frame, contentX, contentY, contentWidth, contentHeight);
    epaper.sleep();
    epaper.displayPartial(frame, transitionFrame, contentX, contentY,
                          contentWidth, contentHeight);
    epaper.displayPartial(frame, transitionFrame, counterX, counterY,
                          counterWidth, counterHeight);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
}

void openLocalBookReaderFast() {
    constexpr uint16_t contentTop = 32;
    constexpr uint32_t whiteSettleMs = 400;
    const uint32_t refreshStarted = millis();
    BookPage::render(transitionFrame);
    constexpr size_t topBarBytes = static_cast<size_t>(contentTop) *
                                   (XingtaiEpd::WIDTH / 8);
    std::memcpy(transitionFrame, frame, topBarBytes);

    // The first partial refresh powers the UC8253 off, so the black reader phase
    // must perform a normal initialized refresh or its pigment is not driven.
    epaper.displayPartialFill(frame, 0x00, 0, contentTop,
                              XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT - contentTop);
    std::memset(frame + topBarBytes, 0x00,
                XingtaiEpd::FRAME_BYTES - topBarBytes);
    delay(whiteSettleMs);

    epaper.displayPartial(frame, transitionFrame, 0, contentTop,
                          XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT - contentTop);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
    Serial.printf("[BOOK SD] Reader display=%lums (initialized black draw, %lums white settle)\n",
                  static_cast<unsigned long>(millis() - refreshStarted),
                  static_cast<unsigned long>(whiteSettleMs));
}

bool refreshPendingBookOpenPressed() {
    int16_t selectedRowTop = 0;
    if (!BookPage::pendingBookOpenRow(selectedRowTop)) return false;

    constexpr uint32_t pressedSettleMs = 250;
    BookPage::render(transitionFrame);
    epaper.displayPartial(frame, transitionFrame, 12,
                          selectedRowTop, 216, 30);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
    delay(pressedSettleMs);
    Serial.printf("[BOOK] Selected row inverted; loading after %lums settle\n",
                  static_cast<unsigned long>(pressedSettleMs));
    return true;
}

void releaseBookReaderControl(BookPage::ReaderControl control) {
    if (control != BookPage::ReaderControl::Previous &&
        control != BookPage::ReaderControl::Next) {
        return;
    }

    BookPage::render(transitionFrame);
    const uint16_t left = control == BookPage::ReaderControl::Previous ? 8 : 198;
    constexpr uint16_t top = 386;
    constexpr uint16_t width = 34;
    constexpr uint16_t height = 24;
    epaper.displayPartial(frame, transitionFrame, left, top, width, height);
    copyFrameRegion(frame, transitionFrame, left, top, width, height);
    epaper.sleep();
    priorityControlFeedbackShown = false;
}

void refreshBookReaderControlPressed(BookPage::ReaderControl control) {
    BookPage::renderReaderControlPressed(transitionFrame, control);
    uint16_t left = 0;
    uint16_t top = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    switch (control) {
    case BookPage::ReaderControl::Back:
        left = 8; top = 38; width = 50; height = 26;
        break;
    case BookPage::ReaderControl::Previous:
        left = 8; top = 386; width = 34; height = 24;
        break;
    case BookPage::ReaderControl::Next:
        left = 198; top = 386; width = 34; height = 24;
        break;
    default:
        return;
    }
    epaper.displayPartial(frame, transitionFrame, left, top, width, height);
    copyFrameRegion(frame, transitionFrame, left, top, width, height);
    epaper.sleep();
}

void refreshBookLibraryContent() {
    // Preserve BOOKLIST and the four fixed navigation buttons. Refresh only
    // the changing page counter and the rows below the pager.
    constexpr uint16_t counterX = 78;
    constexpr uint16_t counterY = 54;
    constexpr uint16_t counterWidth = 84;
    constexpr uint16_t counterHeight = 20;
    constexpr uint16_t listX = 12;
    constexpr uint16_t listY = 82;
    constexpr uint16_t listWidth = 216;
    constexpr uint16_t listHeight = 318;
    BookPage::render(transitionFrame);
    epaper.displayPartial(frame, transitionFrame, counterX, counterY,
                          counterWidth, counterHeight);
    epaper.displayPartial(frame, transitionFrame, listX, listY,
                          listWidth, listHeight);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
}

void refreshOpeningLibraryContent(PageId page) {
    rendererFor(page)(transitionFrame);
    constexpr uint16_t counterX = 78;
    constexpr uint16_t counterY = 54;
    constexpr uint16_t counterWidth = 84;
    constexpr uint16_t counterHeight = 20;
    epaper.displayPartial(frame, transitionFrame, counterX, counterY,
                          counterWidth, counterHeight);
    if (page == PageId::Word) {
        epaper.displayPartial(frame, transitionFrame, 10, 82, 220, 324);
    } else {
        epaper.displayPartial(frame, transitionFrame, 12, 82, 216, 318);
    }
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
}

void startOpeningLibraryLoad(PageId page) {
    bool started = false;
    switch (page) {
    case PageId::Book: started = BookPage::startLibraryLoad(); break;
    case PageId::Voice: started = VoicePage::startLibraryLoad(); break;
    case PageId::Music: started = MusicPage::startLibraryLoad(); break;
    case PageId::Poem: started = PoemPage::startLibraryLoad(); break;
    case PageId::Word: started = WordPage::startLibraryLoad(); break;
    case PageId::Cartoon: started = CartoonPage::startLibraryLoad(); break;
    case PageId::Radio: started = RadioPage::startLibraryLoad(); break;
    default: return;
    }
    openingLoadPage = page;
    openingLoadVisible = true;
    UiLoadingIndicator::show();
    if (!started) {
        Serial.printf("[UI] %s opening worker was not started; free_heap=%u largest_block=%u\n",
                      pageName(page), static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(ESP.getMaxAllocHeap()));
    }
}

void processOpeningLibraryLoads() {
    struct Completion {
        PageId page;
        bool complete;
    };
    const Completion completions[] = {
        {PageId::Book, BookPage::takeLibraryLoadCompleted()},
        {PageId::Voice, VoicePage::takeLibraryLoadCompleted()},
        {PageId::Music, MusicPage::takeLibraryLoadCompleted()},
        {PageId::Poem, PoemPage::takeLibraryLoadCompleted()},
        {PageId::Word, WordPage::takeLibraryLoadCompleted()},
        {PageId::Cartoon, CartoonPage::takeLibraryLoadCompleted()},
        {PageId::Radio, RadioPage::takeLibraryLoadCompleted()},
    };
    for (const Completion &completion : completions) {
        if (!completion.complete) continue;
        if (openingLoadVisible && openingLoadPage == completion.page) {
            UiLoadingIndicator::hide();
            openingLoadVisible = false;
            openingLoadPage = PageId::Main;
            if (currentPage == completion.page) {
                refreshOpeningLibraryContent(completion.page);
            }
        }
    }
}

void refreshPoemDisplay() {
    constexpr uint16_t popupX = 12;
    constexpr uint16_t popupY = 82;
    constexpr uint16_t popupWidth = 216;
    constexpr uint16_t popupHeight = 10 * 30 + 9 * 2;

    // The poem reader is an in-place window. Only scrub and redraw the window
    // rectangle; leave the playlist and the rest of the page untouched.
    epaper.displayPartialFill(frame, 0x00, popupX, popupY,
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

void refreshPoemPlaybackIcon() {
    constexpr uint16_t iconX = 173;
    constexpr uint16_t iconY = 87;
    constexpr uint16_t iconWidth = 48;
    constexpr uint16_t iconHeight = 28;
    PoemPage::render(transitionFrame);
    epaper.displayPartial(frame, transitionFrame, iconX, iconY,
                          iconWidth, iconHeight);
    copyFrameRegion(frame, transitionFrame, iconX, iconY,
                    iconWidth, iconHeight);
    epaper.sleep();
}

void refreshPoemReplayButton() {
    constexpr uint16_t buttonX = 130;
    constexpr uint16_t buttonY = 370;
    constexpr uint16_t buttonWidth = 48;
    constexpr uint16_t buttonHeight = 26;
    PoemPage::render(transitionFrame);
    epaper.displayPartial(frame, transitionFrame, buttonX, buttonY,
                          buttonWidth, buttonHeight);
    copyFrameRegion(frame, transitionFrame, buttonX, buttonY,
                    buttonWidth, buttonHeight);
    epaper.sleep();
}

void refreshPlaylistRows(PageRenderer renderer, int8_t firstRow, int8_t secondRow) {
    if (!renderer || firstRow < 0) return;
    constexpr uint16_t listLeft = 12;
    constexpr uint16_t listTop = 82;
    constexpr uint16_t rowPitch = 32;
    constexpr uint16_t rowWidth = 216;
    constexpr uint16_t rowHeight = 30;

    renderer(transitionFrame);
    const auto rowTop = [](int8_t row) {
        return static_cast<uint16_t>(listTop + static_cast<uint16_t>(row) * rowPitch);
    };

    // Dirty rows are queued old-first, new-second. Fully drive the previous
    // inverted row white before restoring its normal border/text; this avoids
    // gray remnants from white-on-black glyphs on the monochrome panel.
    const uint16_t previousTop = rowTop(firstRow);
    Serial.printf("[PLAYLIST] Row refresh previous=%d next=%d white_wipe=yes\n",
                  firstRow, secondRow);
    epaper.displayPartialFill(frame, 0x00, listLeft, previousTop,
                              rowWidth, rowHeight);
    clearFrameRegion(frame, listLeft, previousTop, rowWidth, rowHeight);
    epaper.sleep();
    delay(300);
    epaper.displayPartial(frame, transitionFrame, listLeft, previousTop,
                          rowWidth, rowHeight);
    copyFrameRegion(frame, transitionFrame, listLeft, previousTop,
                    rowWidth, rowHeight);
    epaper.sleep();

    if (secondRow >= 0 && secondRow != firstRow) {
        const uint16_t selectedTop = rowTop(secondRow);
        epaper.displayPartial(frame, transitionFrame, listLeft, selectedTop,
                              rowWidth, rowHeight);
        copyFrameRegion(frame, transitionFrame, listLeft, selectedTop,
                        rowWidth, rowHeight);
        epaper.sleep();
    }
}

void refreshVoiceDirtyRows() {
    int8_t firstRow = -1;
    int8_t secondRow = -1;
    if (!VoicePage::takeDirtyRows(firstRow, secondRow)) return;

    refreshPlaylistRows(VoicePage::render, firstRow, secondRow);
}

void refreshMusicDirtyRows() {
    int8_t firstRow = -1;
    int8_t secondRow = -1;
    if (!MusicPage::takeDirtyRows(firstRow, secondRow)) return;

    refreshPlaylistRows(MusicPage::render, firstRow, secondRow);
}

void refreshPoemDirtyRows() {
    int8_t firstRow = -1;
    int8_t secondRow = -1;
    if (!PoemPage::takeDirtyRows(firstRow, secondRow)) return;

    refreshPlaylistRows(PoemPage::render, firstRow, secondRow);
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
        epaper.displayPartialFill(frame, 0x00, strokeLeft, strokeTop,
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

void copyFrameRegion(uint8_t *destination, const uint8_t *source,
                     int left, int top, int width, int height) {
    const int firstByte = max(0, left) / 8;
    const int lastByte = min<int>(XingtaiEpd::WIDTH - 1, left + width - 1) / 8;
    const int firstRow = max(0, top);
    const int lastRow = min<int>(XingtaiEpd::HEIGHT - 1, top + height - 1);
    constexpr size_t rowBytes = XingtaiEpd::WIDTH / 8;
    for (int y = firstRow; y <= lastRow; ++y) {
        std::memcpy(destination + static_cast<size_t>(y) * rowBytes + firstByte,
                    source + static_cast<size_t>(y) * rowBytes + firstByte,
                    static_cast<size_t>(lastByte - firstByte + 1));
    }
}

void showPressedInversion(int left, int top, int width, int height) {
    if (width <= 0 || height <= 0) return;
    std::memcpy(transitionFrame, frame, XingtaiEpd::FRAME_BYTES);
    constexpr size_t rowBytes = XingtaiEpd::WIDTH / 8;
    const int right = min<int>(XingtaiEpd::WIDTH, left + width);
    const int bottom = min<int>(XingtaiEpd::HEIGHT, top + height);
    for (int y = max(0, top); y < bottom; ++y) {
        uint8_t *row = transitionFrame + static_cast<size_t>(y) * rowBytes;
        for (int x = max(0, left); x < right; ++x) {
            row[x / 8] ^= 0x80U >> (x % 8);
        }
    }
    epaper.displayPartial(frame, transitionFrame, left, top, width, height);
    copyFrameRegion(frame, transitionFrame, left, top, width, height);
    epaper.sleep();
    constexpr uint32_t pressedMs = 300;
    delay(pressedMs);
}

void showPressedRoundedInversion(int left, int top, int width, int height) {
    if (width <= 0 || height <= 0) return;
    std::memcpy(transitionFrame, frame, XingtaiEpd::FRAME_BYTES);
    constexpr size_t rowBytes = XingtaiEpd::WIDTH / 8;
    constexpr uint8_t cornerInsets[] = {6, 3, 2, 1, 1, 0};
    const int right = min<int>(XingtaiEpd::WIDTH, left + width);
    const int bottom = min<int>(XingtaiEpd::HEIGHT, top + height);
    for (int y = max(0, top); y < bottom; ++y) {
        const int localY = y - top;
        int inset = 0;
        if (localY < 6) {
            inset = cornerInsets[localY];
        } else if (localY >= height - 6) {
            inset = cornerInsets[height - 1 - localY];
        }
        uint8_t *row = transitionFrame + static_cast<size_t>(y) * rowBytes;
        for (int x = max(0, left + inset); x < min(right, left + width - inset); ++x) {
            row[x / 8] ^= 0x80U >> (x % 8);
        }
    }
    epaper.displayPartial(frame, transitionFrame, left, top, width, height);
    copyFrameRegion(frame, transitionFrame, left, top, width, height);
    epaper.sleep();
    delay(300);
}

void showPressedBoldFrame(int left, int top, int width, int height) {
    if (width < 3 || height < 3) return;
    std::memcpy(transitionFrame, frame, XingtaiEpd::FRAME_BYTES);
    constexpr size_t rowBytes = XingtaiEpd::WIDTH / 8;
    const auto setPixel = [](uint8_t *buffer, int x, int y) {
        if (!buffer || x < 0 || x >= XingtaiEpd::WIDTH ||
            y < 0 || y >= XingtaiEpd::HEIGHT) return;
        buffer[static_cast<size_t>(y) * rowBytes + x / 8] |= 0x80U >> (x % 8);
    };
    const auto drawOutline = [&](int inset) {
        const int x0 = left + inset;
        const int y0 = top + inset;
        const int x1 = left + width - 1 - inset;
        const int y1 = top + height - 1 - inset;
        for (int x = x0; x <= x1; ++x) {
            setPixel(transitionFrame, x, y0);
            setPixel(transitionFrame, x, y1);
        }
        for (int y = y0; y <= y1; ++y) {
            setPixel(transitionFrame, x0, y);
            setPixel(transitionFrame, x1, y);
        }
    };
    drawOutline(0);
    drawOutline(1);
    epaper.displayPartial(frame, transitionFrame, left, top, width, height);
    copyFrameRegion(frame, transitionFrame, left, top, width, height);
    epaper.sleep();
    delay(300);
}

bool showPriorityInversion(int left, int top, int width, int height);

bool showPagerPressedInversion(int16_t x, int16_t y) {
    constexpr int top = 52;
    constexpr int height = 25;
    constexpr int width = 34;
    constexpr int lefts[] = {4, 42, 164, 202};
    if (y < top || y >= top + height) return false;
    for (int left : lefts) {
        if (x >= left && x < left + width) {
            return showPriorityInversion(left, top, width, height);
        }
    }
    return false;
}

bool showPriorityInversion(int left, int top, int width, int height) {
    priorityControlLeft = left;
    priorityControlTop = top;
    priorityControlWidth = width;
    priorityControlHeight = height;
    showPressedInversion(left, top, width, height);
    return true;
}

void restorePriorityControl() {
    if (!priorityControlFeedbackShown) return;
    if (currentPage == PageId::Book && BookPage::isReader()) {
        if ((priorityControlLeft == 8 || priorityControlLeft == 198) &&
            priorityControlTop == 386) {
            // Keep the arrow pressed through the synthetic Tap. The Book handler
            // releases it once, after the new page content has been rendered.
            return;
        }
    }
    std::memcpy(transitionFrame, frame, XingtaiEpd::FRAME_BYTES);
    constexpr size_t rowBytes = XingtaiEpd::WIDTH / 8;
    const int right = min<int>(XingtaiEpd::WIDTH,
                               priorityControlLeft + priorityControlWidth);
    const int bottom = min<int>(XingtaiEpd::HEIGHT,
                                priorityControlTop + priorityControlHeight);
    for (int y = max<int>(0, priorityControlTop); y < bottom; ++y) {
        uint8_t *row = transitionFrame + static_cast<size_t>(y) * rowBytes;
        for (int x = max<int>(0, priorityControlLeft); x < right; ++x) {
            row[x / 8] ^= 0x80U >> (x % 8);
        }
    }
    epaper.displayPartial(frame, transitionFrame, priorityControlLeft,
                          priorityControlTop, priorityControlWidth,
                          priorityControlHeight);
    copyFrameRegion(frame, transitionFrame, priorityControlLeft,
                    priorityControlTop, priorityControlWidth,
                    priorityControlHeight);
    epaper.sleep();
}

bool showPriorityControlPressed(int16_t x, int16_t y) {
    const auto inRect = [](int16_t px, int16_t py, int left, int top,
                           int width, int height) {
        return px >= left && px < left + width && py >= top && py < top + height;
    };
    if (currentPage == PageId::Settings) {
        const SettingsPage::Action action = SettingsPage::actionAt(x, y);
        if (action == SettingsPage::Action::VoiceBack) {
            return showPriorityInversion(14, 260, 100, 40);
        }
        if (action == SettingsPage::Action::SdBack) {
            return showPriorityInversion(18, 340, 96, 42);
        }
        return false;
    }
    if (currentPage == PageId::Book) {
        if (!BookPage::isReader()) return showPagerPressedInversion(x, y);
        if (inRect(x, y, 0, 32, 70, 40)) {
            return showPriorityInversion(8, 38, 50, 26);
        }
        if (inRect(x, y, 0, 374, 60, 42)) {
            return showPriorityInversion(8, 386, 34, 24);
        }
        if (inRect(x, y, 180, 374, 60, 42)) {
            return showPriorityInversion(198, 386, 34, 24);
        }
        return false;
    }
    if (currentPage == PageId::Poem && PoemPage::isPopupOpen()) {
        if (inRect(x, y, 11, 79, 50, 32)) {
            return false;
        }
        if (inRect(x, y, 14, 370, 44, 26)) {
            return showPriorityInversion(14, 370, 44, 26);
        }
        if (inRect(x, y, 62, 370, 40, 26) ||
            inRect(x, y, 130, 370, 48, 26)) {
            return false;
        }
        if (inRect(x, y, 182, 370, 44, 26)) {
            return showPriorityInversion(182, 370, 44, 26);
        }
        return false;
    }
    if (currentPage == PageId::Poem &&
        inRect(x, y, 12, 82, 216, 10 * 32 - 2)) {
        const int row = (y - 82) / 32;
        const int rowTop = 82 + row * 32;
        if (y < rowTop + 30) return showPriorityInversion(12, rowTop, 216, 30);
    }
    if (currentPage == PageId::Word && WordPage::isDetail()) {
        if (inRect(x, y, 6, 36, 64, 36)) {
            return showPriorityInversion(10, 40, 58, 26);
        }
        return false;
    }
    if (currentPage == PageId::Voice || currentPage == PageId::Music ||
        currentPage == PageId::Poem || currentPage == PageId::Word) {
        return showPagerPressedInversion(x, y);
    }
    return false;
}

void showHomePressedOutline() {
    constexpr int left = 0;
    constexpr int top = 0;
    constexpr int width = 32;
    constexpr int height = 32;
    constexpr size_t rowBytes = XingtaiEpd::WIDTH / 8;
    constexpr size_t tileRowBytes = width / 8;
    uint8_t homeTile[tileRowBytes * height] = {};
    // Render Home only as a source for this tile. The rest of transitionFrame
    // must continue to describe the physically displayed content page.
    MainPage::render(transitionFrame);
    for (int row = 0; row < height; ++row) {
        std::memcpy(homeTile + static_cast<size_t>(row) * tileRowBytes,
                    transitionFrame + static_cast<size_t>(top + row) * rowBytes,
                    tileRowBytes);
    }
    std::memcpy(transitionFrame, frame, XingtaiEpd::FRAME_BYTES);
    for (int row = 0; row < height; ++row) {
        std::memcpy(transitionFrame + static_cast<size_t>(top + row) * rowBytes,
                    homeTile + static_cast<size_t>(row) * tileRowBytes,
                    tileRowBytes);
    }
    for (int inset = 0; inset < 1; ++inset) {
        const int frameLeft = left + inset;
        const int frameTop = top + inset;
        const int frameRight = left + width - 1 - inset;
        const int frameBottom = top + height - 1 - inset;
        for (int x = frameLeft; x <= frameRight; ++x) {
            transitionFrame[static_cast<size_t>(frameTop) * rowBytes + x / 8] |=
                0x80U >> (x % 8);
            transitionFrame[static_cast<size_t>(frameBottom) * rowBytes + x / 8] |=
                0x80U >> (x % 8);
        }
        for (int y = frameTop; y <= frameBottom; ++y) {
            transitionFrame[static_cast<size_t>(y) * rowBytes + frameLeft / 8] |=
                0x80U >> (frameLeft % 8);
            transitionFrame[static_cast<size_t>(y) * rowBytes + frameRight / 8] |=
                0x80U >> (frameRight % 8);
        }
    }

    epaper.displayPartial(frame, transitionFrame, left, top, width, height);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
    homeOutlinePressed = true;
    Serial.println("[NAVIGATION] Home pressed; bold frame displayed");
}

void restoreHomeIcon() {
    if (!homeOutlinePressed) return;
    constexpr uint16_t left = 0;
    constexpr uint16_t top = 0;
    constexpr uint16_t width = 32;
    constexpr uint16_t height = 32;
    constexpr size_t rowBytes = XingtaiEpd::WIDTH / 8;
    constexpr size_t tileRowBytes = width / 8;
    uint8_t homeTile[tileRowBytes * height] = {};
    MainPage::render(transitionFrame);
    for (int row = 0; row < height; ++row) {
        std::memcpy(homeTile + static_cast<size_t>(row) * tileRowBytes,
                    transitionFrame + static_cast<size_t>(top + row) * rowBytes,
                    tileRowBytes);
    }
    std::memcpy(transitionFrame, frame, XingtaiEpd::FRAME_BYTES);
    for (int row = 0; row < height; ++row) {
        std::memcpy(transitionFrame + static_cast<size_t>(top + row) * rowBytes,
                    homeTile + static_cast<size_t>(row) * tileRowBytes,
                    tileRowBytes);
    }
    epaper.displayPartial(frame, transitionFrame, left, top, width, height);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
    homeOutlinePressed = false;
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
                                     UiLocalization::isChinese() ? "下载中" : "DOWNLOADING", 1);
        epaper.displayPartial(frame, transitionFrame, loadingLeft, loadingTop,
                              loadingWidth, loadingHeight);
    } else {
        // Reconstruct the physical loading state rather than assuming the
        // shared transition buffer still contains it after page rendering.
        std::memcpy(transitionFrame, frame, XingtaiEpd::FRAME_BYTES);
        clearFrameRegion(transitionFrame, loadingLeft, loadingTop,
                         loadingWidth, loadingHeight);
        UiLocalization::drawCentered(transitionFrame, 12,
                                     UiLocalization::isChinese() ? "下载中" : "DOWNLOADING", 1);
        epaper.displayPartial(transitionFrame, frame, loadingLeft, loadingTop,
                              loadingWidth, loadingHeight);
    }
    epaper.sleep();
}

void refreshNetworkTopbar(MainPage::NetworkMode mode) {
    MainPage::setNetworkMode(mode);
    MainPage::render(transitionFrame);
    std::memcpy(transitionFrame, frame, XingtaiEpd::FRAME_BYTES);
    clearFrameRegion(transitionFrame, 178, 0, 28, 32);
    if (mode == MainPage::NetworkMode::Wifi) {
        Topbar::drawWifi(transitionFrame, 181, 2);
    } else if (mode == MainPage::NetworkMode::Cellular4G) {
        Topbar::draw4G(transitionFrame, 184, 2);
    }
    epaper.displayPartial(frame, transitionFrame, 178, 0, 28, 32);
    if (mode == MainPage::NetworkMode::None) {
        // Reapply the black-to-white waveform once to scrub residual pigment
        // without touching the adjacent battery icon.
        epaper.displayPartial(frame, transitionFrame, 178, 0, 28, 32);
    }
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
}

void updateNetworkTopbar() {
    const MainPage::NetworkMode mode = WiFi.status() == WL_CONNECTED
        ? MainPage::NetworkMode::Wifi
        : cellularModem.isConnected()
            ? MainPage::NetworkMode::Cellular4G
            : MainPage::NetworkMode::None;
    if (mode == lastNetworkMode) return;
    lastNetworkMode = mode;
    const char *label = mode == MainPage::NetworkMode::Wifi ? "WiFi"
        : mode == MainPage::NetworkMode::Cellular4G ? "4G" : "offline";
    Serial.printf("[TOPBAR] Network icon=%s\n", label);
    refreshNetworkTopbar(mode);
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
    case PageId::Cartoon: return MainPage::FunctionIcon::Cartoon;
    case PageId::Radio: return MainPage::FunctionIcon::Radio;
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
    case PageId::Cartoon: column = 1; row = 3; break;
    case PageId::Radio: column = 2; row = 3; break;
    default: return false;
    }

    x = startX + column * (iconSize + columnGap) - framePadding;
    y = startY + row * (iconSize + rowGap) - framePadding;
    width = frameSize;
    height = frameSize;
    return true;
}

void startMainIconFeedback(PageId page) {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    if (page == PageId::Main || !mainIconBounds(page, x, y, width, height)) {
        return;
    }

    MainPage::render(transitionFrame, mainIconFor(page));
    epaper.displayPartial(frame, transitionFrame, x, y, width, height);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
    suppressMainIconReleaseTap = true;
    constexpr uint32_t pressedSettleMs = 500;
    delay(pressedSettleMs);
    Serial.printf("[FUNCTION ICON] Inverted: %s; opening after %lums settle\n",
                  pageName(page), static_cast<unsigned long>(pressedSettleMs));
    // Queue navigation only after the inverted tile has remained visible long
    // enough for the UC8253 pigment and the user to register the touch feedback.
    queuePage(page);
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

bool solveCalibrationAxis(const float normal[3][3], const float rhs[3], float result[3]) {
    float matrix[3][4] = {};
    for (uint8_t row = 0; row < 3; ++row) {
        for (uint8_t column = 0; column < 3; ++column) matrix[row][column] = normal[row][column];
        matrix[row][3] = rhs[row];
    }
    for (uint8_t pivot = 0; pivot < 3; ++pivot) {
        uint8_t best = pivot;
        for (uint8_t row = pivot + 1; row < 3; ++row) {
            if (fabsf(matrix[row][pivot]) > fabsf(matrix[best][pivot])) best = row;
        }
        if (fabsf(matrix[best][pivot]) < 0.0001f) return false;
        if (best != pivot) {
            for (uint8_t column = pivot; column < 4; ++column) {
                const float value = matrix[pivot][column];
                matrix[pivot][column] = matrix[best][column];
                matrix[best][column] = value;
            }
        }
        const float divisor = matrix[pivot][pivot];
        for (uint8_t column = pivot; column < 4; ++column) matrix[pivot][column] /= divisor;
        for (uint8_t row = 0; row < 3; ++row) {
            if (row == pivot) continue;
            const float factor = matrix[row][pivot];
            for (uint8_t column = pivot; column < 4; ++column) {
                matrix[row][column] -= factor * matrix[pivot][column];
            }
        }
    }
    for (uint8_t row = 0; row < 3; ++row) result[row] = matrix[row][3];
    return true;
}

bool solveCalibration() {
    // Fit display = a * raw.x + b * raw.y + c using all four corner samples.
    // The normal equations provide a least-squares affine fit, so one slightly
    // off-center tap does not distort the entire touch surface.
    float normal[3][3] = {};
    float targets[2][3] = {};
    for (uint8_t index = 0; index < 4; ++index) {
        const float values[3] = {
            static_cast<float>(calibrationRaw[index].x),
            static_cast<float>(calibrationRaw[index].y), 1.0f,
        };
        for (uint8_t row = 0; row < 3; ++row) {
            for (uint8_t column = 0; column < 3; ++column) {
                normal[row][column] += values[row] * values[column];
            }
            targets[0][row] += values[row] * calibrationTargetX[index];
            targets[1][row] += values[row] * calibrationTargetY[index];
        }
    }
    float coefficients[3] = {};
    float calibratedTransform[6] = {};
    if (!solveCalibrationAxis(normal, targets[0], coefficients)) return false;
    for (uint8_t index = 0; index < 3; ++index) calibratedTransform[index] = coefficients[index];
    if (!solveCalibrationAxis(normal, targets[1], coefficients)) return false;
    for (uint8_t index = 0; index < 3; ++index) calibratedTransform[index + 3] = coefficients[index];
    std::memcpy(touchTransform, calibratedTransform, sizeof(touchTransform));
    touchPreferences.begin("touch", false);
    touchPreferences.putBytes("matrix", touchTransform, sizeof(touchTransform));
    touchPreferences.putUChar("version", 3);
    touchPreferences.end();
    return true;
}

void loadCalibration() {
    touchPreferences.begin("touch", true);
    if (touchPreferences.getUChar("version", 0) == 3 &&
        touchPreferences.getBytesLength("matrix") == sizeof(touchTransform)) {
        touchPreferences.getBytes("matrix", touchTransform, sizeof(touchTransform));
        Serial.println("[CAL] Loaded four-point touch calibration v3");
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

bool isSettingsKeyboardAction(int16_t x, int16_t y);

bool isImmediateTouchControl(int16_t x, int16_t y) {
    const auto inRect = [](int16_t px, int16_t py, int left, int top,
                           int width, int height) {
        return px >= left && px < left + width && py >= top && py < top + height;
    };
    const auto pagerControl = [&](int top = 52) {
        if (y < top || y >= top + 25) return false;
        return inRect(x, y, 4, top, 34, 25) || inRect(x, y, 42, top, 34, 25) ||
               inRect(x, y, 164, top, 34, 25) || inRect(x, y, 202, top, 34, 25);
    };

    if (currentPage == PageId::Settings) {
        const SettingsPage::Action action = SettingsPage::actionAt(x, y);
        return action != SettingsPage::Action::None &&
               !isSettingsKeyboardAction(x, y) &&
               action != SettingsPage::Action::SelectWifiNetwork &&
               action != SettingsPage::Action::SelectVoice;
    }
    if (currentPage == PageId::Calendar) {
        return CalendarPage::actionAt(x, y) != CalendarPage::Action::None;
    }
    if (currentPage == PageId::Book) {
        if (!BookPage::isReader()) return pagerControl();
        return inRect(x, y, 0, 32, 70, 40) ||
               inRect(x, y, 0, 374, 60, 42) ||
               inRect(x, y, 180, 374, 60, 42);
    }
    if (currentPage == PageId::Voice || currentPage == PageId::Music) {
        return pagerControl();
    }
    if (currentPage == PageId::Poem) {
        if (!PoemPage::isPopupOpen()) return pagerControl();
        return inRect(x, y, 14, 370, 44, 26) ||
               inRect(x, y, 62, 370, 40, 26) ||
               inRect(x, y, 130, 370, 48, 26) ||
               inRect(x, y, 182, 370, 44, 26);
    }
    if (currentPage == PageId::Word) {
        if (!WordPage::isDetail()) return pagerControl();
        return inRect(x, y, 6, 36, 64, 36) ||
               inRect(x, y, 22, 75, 100, 100) ||
               inRect(x, y, 170, 358, 56, 38);
    }
    if (currentPage == PageId::Recording) {
        int16_t left = 0;
        int16_t top = 0;
        int16_t width = 0;
        int16_t height = 0;
        return RecordingPage::returnControlAt(x, y) ||
               RecordingPage::headerControlAt(x, y) ||
               RecordingPage::folderControlAt(x, y) ||
               RecordingPage::pauseControlAt(x, y) ||
               RecordingPage::tagItemBoundsAt(x, y, left, top, width, height) ||
               RecordingPage::pagerControlBoundsAt(x, y, left, top, width, height) ||
               inRect(x, y, 80, 320, 80, 81) ||
               inRect(x, y, 204, 108, 28, 10 * 30 - 2);
    }
    if (currentPage == PageId::Cartoon) {
        int16_t left = 0;
        int16_t top = 0;
        int16_t width = 0;
        int16_t height = 0;
        return CartoonPage::controlBoundsAt(x, y, left, top, width, height);
    }
    if (currentPage == PageId::Radio) return pagerControl();
    return false;
}

bool isSettingsKeyboardAction(int16_t x, int16_t y) {
    if (currentPage != PageId::Settings) return false;
    const SettingsPage::Action action = SettingsPage::actionAt(x, y);
    switch (action) {
    case SettingsPage::Action::WifiKey:
    case SettingsPage::Action::WifiBackspace:
    case SettingsPage::Action::WifiSpace:
    case SettingsPage::Action::WifiChangeKeyboard:
    case SettingsPage::Action::UrlKey:
    case SettingsPage::Action::UrlBackspace:
    case SettingsPage::Action::UrlSpace:
    case SettingsPage::Action::UrlChangeKeyboard:
        return true;
    default:
        return false;
    }
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
        {PageId::Recording, PageId::Cartoon, PageId::Radio},
    };

    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 3; ++column) {
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
        // Use the initial contact for calibration. A release sample can move
        // several pixels while the finger lifts, which is significant for the
        // 23-pixel keyboard keys.
        if (event == TEvent::TouchStart && calibrationCount < 4) {
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
        // A long e-paper feedback refresh can make the FT6336 reject the prior
        // release as a Tap. Once a new physical touch starts, any old release
        // suppression is necessarily stale and must not consume this gesture.
        if (suppressMainIconReleaseTap) suppressMainIconReleaseTap = false;
        if (suppressNextAudioTap) suppressNextAudioTap = false;
        if (currentPage == PageId::Calculator) calculatorPressHandled = false;
        immediateControlPressHandled = false;
        priorityControlFeedbackShown = false;
        if (currentPage != PageId::Main && isHomeIcon(uiX, uiY)) {
            touchGestureActive = false;
            homeTouchActive = true;
            suppressMainIconReleaseTap = true;
            if (!calculatorRefreshRunning) showHomePressedOutline();
            // Keep the current page active for the lifetime of this physical
            // touch. Switching pages here lets later Contact/LiftUp samples be
            // routed against Main and can reopen the page that was just left.
            return;
        }
        if (currentPage == PageId::Main) {
            const PageId page = mainPageAt(uiX, uiY);
            if (page != PageId::Main) {
                touchGestureActive = false;
                startMainIconFeedback(page);
                return;
            }
        }
        // Calculator input is committed on the hardware press event. The later
        // Tap event is only a release notification and must not repeat the key.
        if (currentPage == PageId::Calculator &&
            CalculatorPage::handleTouchStart(uiX, uiY)) {
            calculatorPressHandled = true;
            calculatorRefreshPending = true;
            touchGestureActive = true;
            touchGestureStartX = touchGestureLastX = uiX;
            touchGestureStartY = touchGestureLastY = uiY;
            Serial.printf("[CALCULATOR] Key accepted on press ui=(%d,%d)\n", uiX, uiY);
            return;
        }
        if (isImmediateTouchControl(uiX, uiY)) {
            immediateControlPressHandled = true;
            touchGestureActive = false;
            dispatchingSyntheticTap = true;
            handleTouch(point, TEvent::Tap);
            dispatchingSyntheticTap = false;
            Serial.printf("[TOUCH] Control accepted on press page=%s ui=(%d,%d)\n",
                          pageName(currentPage), uiX, uiY);
            return;
        }
        priorityControlFeedbackShown = showPriorityControlPressed(uiX, uiY);
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
        restorePriorityControl();
        if (homeOutlinePressed) restoreHomeIcon();
        if (homeTouchActive) {
            homeTouchActive = false;
            Serial.println("[NAVIGATION] Home released; opening cached framebuffer");
            queuePage(PageId::Main);
        }
        if (touchGestureActive) {
            touchGestureLastX = uiX;
            touchGestureLastY = uiY;
            const int16_t deltaX = touchGestureLastX - touchGestureStartX;
            const int16_t deltaY = touchGestureLastY - touchGestureStartY;
            touchGestureActive = false;
            const bool poemPopupOpen = currentPage == PageId::Poem && PoemPage::isPopupOpen();
            const bool isSwipe = (poemPopupOpen || currentPage == PageId::Book ||
                 currentPage == PageId::Voice || currentPage == PageId::Music ||
                 currentPage == PageId::Poem || currentPage == PageId::Word) &&
                touchGestureStartY >= 32 &&
                 (abs(deltaX) >= 35 || abs(deltaY) >= 35);
            if (isSwipe) {
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
                    } else if (currentPage == PageId::Book &&
                               BookPage::takeReaderContentRefreshRequest()) {
                        const BookPage::ReaderControl control =
                            BookPage::takeReaderControlPress();
                        if (control != BookPage::ReaderControl::None &&
                            !priorityControlFeedbackShown) {
                            refreshBookReaderControlPressed(control);
                        }
                        releaseBookReaderControl(control);
                        refreshBookReaderContent();
                    } else if (currentPage == PageId::Book &&
                               BookPage::takeLibraryContentRefreshRequest()) {
                        const bool bookRowPressed = refreshPendingBookOpenPressed();
                        if (BookPage::pendingBookOpenIsLocal()) {
                            const uint32_t openStarted = millis();
                            if (BookPage::preparePendingBookOpen() &&
                                BookPage::processPendingBookOpen()) {
                                openLocalBookReaderFast();
                                Serial.printf("[BOOK SD] Tap-to-reader total=%lums\n",
                                              static_cast<unsigned long>(millis() - openStarted));
                            } else {
                                refreshCurrentPage();
                            }
                        } else {
                            if (!bookRowPressed) {
                                refreshBookLibraryContent();
                            }
                            if (BookPage::preparePendingBookOpen()) {
                                refreshCurrentPage();
                                if (BookPage::processPendingBookOpen()) {
                                    refreshBookReaderContent();
                                }
                            }
                        }
                    } else {
                        refreshCurrentPage();
                    }
                }
            } else if (!homeTouchActive && !calculatorPressHandled &&
                       !immediateControlPressHandled) {
                // E-paper feedback can outlast the FT6336 Tap timing window.
                // Dispatch a stationary release now; consume the driver's
                // follow-up Tap if that event is emitted as well.
                suppressDriverTap = true;
                dispatchingSyntheticTap = true;
                handleTouch(point, TEvent::Tap);
                dispatchingSyntheticTap = false;
            }
        }
        calculatorPressHandled = false;
        touchWorkflowPriority = false;
        return;
    }
    if (event != TEvent::Tap) return;
    touchWorkflowPriority = false;
    if (immediateControlPressHandled && !dispatchingSyntheticTap) {
        immediateControlPressHandled = false;
        return;
    }
    if (currentPage == PageId::Calculator && calculatorPressHandled) {
        calculatorPressHandled = false;
        return;
    }
    if (suppressDriverTap && !dispatchingSyntheticTap) {
        suppressDriverTap = false;
        return;
    }
    Serial.printf("[TOUCH] TAP raw=(%u,%u) mapped=(%d,%d) ui=(%d,%d)\n",
                  point.x, point.y, x, y, uiX, uiY);

    // The Home icon action was already accepted on TouchStart. Do not let the
    // FT6336's later LiftUp/Tap event activate a control on the destination page.
    if (suppressMainIconReleaseTap) {
        suppressMainIconReleaseTap = false;
        return;
    }

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
        if (PoemPage::handleTap(uiX, uiY)) {
            if (!priorityControlFeedbackShown &&
                uiX >= 62 && uiX < 102 && uiY >= 370 && uiY < 396) {
                showPressedInversion(62, 370, 40, 26);
            } else if (!priorityControlFeedbackShown &&
                       uiX >= 14 && uiX < 58 && uiY >= 370 && uiY < 396) {
                showPressedInversion(14, 370, 44, 26);
            } else if (!priorityControlFeedbackShown &&
                       uiX >= 130 && uiX < 178 && uiY >= 370 && uiY < 396) {
                showPressedInversion(130, 370, 48, 26);
            } else if (!priorityControlFeedbackShown &&
                       uiX >= 182 && uiX < 226 && uiY >= 370 && uiY < 396) {
                showPressedInversion(182, 370, 44, 26);
            }
            if (PoemPage::takePlaybackIconRefreshRequest()) {
                refreshPoemPlaybackIcon();
            } else if (PoemPage::takeReplayButtonRefreshRequest()) {
                refreshPoemReplayButton();
            } else if (uiX >= 130 && uiX < 178 && uiY >= 370 && uiY < 396) {
                // Keep Replay inverted while its deferred audio start is running.
            } else {
                refreshPoemDisplay();
            }
        }
        return;
    }

    if (currentPage == PageId::Settings) {
        const SettingsPage::Action action = SettingsPage::actionAt(uiX, uiY);
        Serial.printf("[SETTINGS TOUCH] action=%u ui=(%d,%d) url=%s wifi=%d cellular=%d\n",
                      static_cast<unsigned>(action), uiX, uiY, contentUrl,
                      WiFi.status(), cellularModem.isConnected());
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
            UiLoadingIndicator::show();
            SettingsPage::setSdMounted(SdCard::isMounted());
            {
                const uint8_t count = SdCard::listRoot(sdEntryNames, sdEntryDirectories, 8);
                SettingsPage::showSdPage(sdEntryNames, sdEntryDirectories, count);
            }
            UiLoadingIndicator::hide();
            refreshCurrentPage();
            return;
        case SettingsPage::Action::SdBack:
            if (!priorityControlFeedbackShown) {
                showPressedInversion(18, 340, 96, 42);
            }
            SettingsPage::showSettings();
            refreshCurrentPage();
            return;
        case SettingsPage::Action::SdRequestFormat:
            SettingsPage::setFormatPending(true);
            refreshCurrentPage();
            return;
        case SettingsPage::Action::SdConfirmFormat:
            UiLoadingIndicator::show();
            if (SdCard::format()) Serial.println("[SD] Format complete");
            else Serial.println("[SD] Format failed");
            UiLoadingIndicator::hide();
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
            if (!priorityControlFeedbackShown) {
                showPressedInversion(14, 260, 100, 40);
            }
            SettingsPage::showSettings();
            refreshCurrentPage();
            return;
        case SettingsPage::Action::ToggleCellular:
            settingsState.cellularEnabled = !settingsState.cellularEnabled;
            applyCellularSetting();
            break;
        case SettingsPage::Action::TogglePlaylistCache:
            settingsState.playlistCacheEnabled = !settingsState.playlistCacheEnabled;
            PlaylistCache::setEnabled(settingsState.playlistCacheEnabled);
            Serial.printf("[SETTINGS] Playlist cache %s\n",
                          settingsState.playlistCacheEnabled ? "enabled" : "disabled");
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
            const size_t protectedLength =
                std::strncmp(contentUrl, CONTENT_URL_PREFIX, CONTENT_URL_PREFIX_LENGTH) == 0
                    ? CONTENT_URL_PREFIX_LENGTH : 0;
            if (length > protectedLength) contentUrl[length - 1] = '\0';
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
            Serial.printf("[CONTENT URL] Save requested input=%s\n", contentUrl);
            if (!normalizeContentUrl(contentUrl, sizeof(contentUrl))) {
                Serial.println("[CONTENT URL] Normalization failed");
                std::strncpy(contentUrl, savedContentUrl, sizeof(contentUrl) - 1);
                contentUrl[sizeof(contentUrl) - 1] = '\0';
                SettingsPage::showContentUrl(contentUrl);
                refreshCurrentPage();
                return;
            }
            SettingsPage::showContentUrl(contentUrl);
            {
                const bool transportConnected = WiFi.status() == WL_CONNECTED ||
                                                cellularModem.isConnected();
                const bool canSave = !transportConnected || contentUrlReachable(contentUrl);
                if (!transportConnected) {
                    Serial.printf("[CONTENT URL] No active transport; saving without validation: %s\n",
                                  contentUrl);
                }
                if (canSave) {
                    std::strncpy(savedContentUrl, contentUrl, sizeof(savedContentUrl) - 1);
                    savedContentUrl[sizeof(savedContentUrl) - 1] = '\0';
                    saveSettings();
                    SettingsPage::showSettings();
                    Serial.printf("[CONTENT URL] Saved URL: %s\n", contentUrl);
                } else {
                    std::strncpy(contentUrl, savedContentUrl, sizeof(contentUrl) - 1);
                    contentUrl[sizeof(contentUrl) - 1] = '\0';
                    SettingsPage::showContentUrl(contentUrl);
                    Serial.printf("[CONTENT URL] New URL unreachable; restored: %s\n", contentUrl);
                }
            }
            refreshCurrentPage();
            return;
        case SettingsPage::Action::UrlClear:
            contentUrl[0] = '\0';
            SettingsPage::showContentUrl(contentUrl);
            refreshCurrentRegion(10, 68, 220, 58);
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
        // Calculator keys are accepted on TouchStart. A release-generated Tap
        // reaches here only for touches outside the keypad.
        return;
    }

    if (currentPage == PageId::Book) {
        if (BookPage::handleTap(uiX, uiY)) {
            const BookPage::ReaderControl control = BookPage::takeReaderControlPress();
            if (control != BookPage::ReaderControl::None &&
                !priorityControlFeedbackShown) {
                refreshBookReaderControlPressed(control);
            } else if (!BookPage::isReader() && !priorityControlFeedbackShown) {
                showPagerPressedInversion(uiX, uiY);
            }
            if (BookPage::processPendingReaderBack()) {
                refreshCurrentPage();
            } else if (control == BookPage::ReaderControl::Previous ||
                       control == BookPage::ReaderControl::Next) {
                BookPage::takeReaderContentRefreshRequest();
                constexpr uint32_t readerControlPressedHoldMs = 500;
                delay(readerControlPressedHoldMs);
                releaseBookReaderControl(control);
                refreshBookReaderContent();
            } else if (BookPage::takeReaderContentRefreshRequest()) {
                refreshBookReaderContent();
            } else if (BookPage::takeLibraryContentRefreshRequest()) {
                const bool bookRowPressed = refreshPendingBookOpenPressed();
                if (BookPage::pendingBookOpenIsLocal()) {
                    const uint32_t openStarted = millis();
                    if (BookPage::preparePendingBookOpen() &&
                        BookPage::processPendingBookOpen()) {
                        openLocalBookReaderFast();
                        Serial.printf("[BOOK SD] Tap-to-reader total=%lums\n",
                                      static_cast<unsigned long>(millis() - openStarted));
                    } else {
                        refreshCurrentPage();
                    }
                    return;
                }
                if (!bookRowPressed) {
                    refreshBookLibraryContent();
                }
                if (BookPage::preparePendingBookOpen()) {
                    refreshCurrentPage();
                    if (BookPage::processPendingBookOpen()) {
                        refreshBookReaderContent();
                    }
                }
            }
            else refreshCurrentPage();
        }
        return;
    }

    if (currentPage == PageId::Voice) {
        if (VoicePage::handleTap(uiX, uiY)) {
            if (!priorityControlFeedbackShown) showPagerPressedInversion(uiX, uiY);
            int8_t firstRow = -1;
            int8_t secondRow = -1;
            if (VoicePage::takeDirtyRows(firstRow, secondRow)) {
                refreshPlaylistRows(VoicePage::render, firstRow, secondRow);
            } else {
                refreshCurrentPage();
            }
        }
        return;
    }

    if (currentPage == PageId::Music) {
        if (MusicPage::handleTap(uiX, uiY)) {
            if (!priorityControlFeedbackShown) showPagerPressedInversion(uiX, uiY);
            int8_t firstRow = -1;
            int8_t secondRow = -1;
            if (MusicPage::takeDirtyRows(firstRow, secondRow)) {
                refreshPlaylistRows(MusicPage::render, firstRow, secondRow);
            } else {
                refreshCurrentPage();
            }
        }
        return;
    }

    if (currentPage == PageId::Poem) {
        const bool popupWasOpen = PoemPage::isPopupOpen();
        const bool returnTapped = PoemPage::returnControlAt(uiX, uiY);
        const bool replayTapped = PoemPage::replayControlAt(uiX, uiY);
        if (returnTapped) {
            showPressedInversion(62, 370, 40, 26);
        } else if (replayTapped) {
            showPressedInversion(130, 370, 48, 26);
        }
        if (PoemPage::handleTap(uiX, uiY)) {
            if (!popupWasOpen && !priorityControlFeedbackShown &&
                !returnTapped && !replayTapped) {
                showPagerPressedInversion(uiX, uiY);
            }
            if (PoemPage::takePlaybackIconRefreshRequest()) {
                refreshPoemPlaybackIcon();
            } else if (PoemPage::takeReplayButtonRefreshRequest()) {
                refreshPoemReplayButton();
            } else if (replayTapped) {
                // Keep Replay inverted until processAudio() starts playback.
            } else if (popupWasOpen || PoemPage::isPopupOpen()) {
                refreshPoemDisplay();
            }
            else refreshCurrentPage();
        }
        return;
    }

    if (currentPage == PageId::Word) {
        int16_t cardLeft = 0;
        int16_t cardTop = 0;
        int16_t cardWidth = 0;
        int16_t cardHeight = 0;
        const bool wordCardTapped = WordPage::libraryCardBoundsAt(
            uiX, uiY, cardLeft, cardTop, cardWidth, cardHeight);
        if (wordCardTapped) {
            showPressedBoldFrame(cardLeft, cardTop, cardWidth, cardHeight);
        }
        if (WordPage::handleTap(uiX, uiY)) {
            if (!priorityControlFeedbackShown &&
                uiX >= 6 && uiX < 70 && uiY >= 36 && uiY < 72) {
                showPressedInversion(10, 40, 58, 26);
            } else if (!priorityControlFeedbackShown) {
                showPagerPressedInversion(uiX, uiY);
            }
            if (WordPage::takeReplayRefreshRequest()) {
                refreshWordStrokeWindow(true);
            } else {
                refreshCurrentPage();
            }
        }
        return;
    }

    if (currentPage == PageId::Recording) {
        int16_t tagLeft = 0;
        int16_t tagTop = 0;
        int16_t tagWidth = 0;
        int16_t tagHeight = 0;
        int16_t pagerLeft = 0;
        int16_t pagerTop = 0;
        int16_t pagerWidth = 0;
        int16_t pagerHeight = 0;
        if (RecordingPage::returnControlAt(uiX, uiY)) {
            showPressedInversion(10, 40, 48, 28);
        } else if (RecordingPage::headerControlAt(uiX, uiY)) {
            showPressedInversion(62, 40, 136, 30);
        } else if (RecordingPage::folderControlAt(uiX, uiY)) {
            showPressedRoundedInversion(12, 333, 56, 56);
        } else if (RecordingPage::pauseControlAt(uiX, uiY)) {
            showPressedRoundedInversion(172, 333, 56, 56);
        } else if (RecordingPage::tagItemBoundsAt(uiX, uiY, tagLeft, tagTop,
                                                   tagWidth, tagHeight)) {
            showPressedInversion(tagLeft, tagTop, tagWidth, tagHeight);
        } else if (RecordingPage::pagerControlBoundsAt(uiX, uiY, pagerLeft, pagerTop,
                                                        pagerWidth, pagerHeight)) {
            showPressedInversion(pagerLeft, pagerTop, pagerWidth, pagerHeight);
        }
        if (RecordingPage::handleTap(uiX, uiY)) {
            if (RecordingPage::takeExitRequest()) queuePage(PageId::Main);
            else refreshCurrentPage();
        }
        return;
    }

    if (currentPage == PageId::Cartoon) {
        int16_t controlLeft = 0;
        int16_t controlTop = 0;
        int16_t controlWidth = 0;
        int16_t controlHeight = 0;
        int16_t rowLeft = 0;
        int16_t rowTop = 0;
        int16_t rowWidth = 0;
        int16_t rowHeight = 0;
        const bool controlPressed = CartoonPage::controlBoundsAt(
            uiX, uiY, controlLeft, controlTop, controlWidth, controlHeight);
        const bool rowPressed = CartoonPage::rowBoundsAt(
            uiX, uiY, rowLeft, rowTop, rowWidth, rowHeight);
        if (controlPressed && !CartoonPage::isReader()) {
            showPressedInversion(controlLeft, controlTop, controlWidth, controlHeight);
        } else if (rowPressed) {
            showPressedInversion(rowLeft, rowTop, rowWidth, rowHeight);
        }
        const bool changed = CartoonPage::handleTap(uiX, uiY);
        if (changed || rowPressed) {
            const CartoonPage::RefreshMode refreshMode = CartoonPage::takeRefreshMode();
            if (changed && refreshMode == CartoonPage::RefreshMode::ListContent) {
                refreshCartoonListContent(controlLeft, controlTop,
                                          controlWidth, controlHeight);
            } else if (changed && refreshMode == CartoonPage::RefreshMode::ImageContent) {
                refreshCartoonImageContent();
            } else if (changed && refreshMode == CartoonPage::RefreshMode::Layout) {
                refreshCartoonLayout();
            } else {
                refreshCurrentPage();
            }
        }
        return;
    }

    if (currentPage == PageId::Radio) {
        int16_t rowLeft = 0;
        int16_t rowTop = 0;
        int16_t rowWidth = 0;
        int16_t rowHeight = 0;
        const bool rowPressed = RadioPage::rowBoundsAt(
            uiX, uiY, rowLeft, rowTop, rowWidth, rowHeight);
        if (rowPressed) showPressedInversion(rowLeft, rowTop, rowWidth, rowHeight);
        const bool changed = RadioPage::handleTap(uiX, uiY);
        if (changed || rowPressed) refreshCurrentPage();
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
    // R23 is removed, so assert the externally wired ES8311 CE before any I2C
    // transaction. CE high selects the only codec address used here: 0x19.
    pinMode(BoardPins::AUDIO_CE, OUTPUT);
    audio.setChipEnabled(true);
    delay(10);
    Serial.printf("[AUDIO] ES8311 CE GPIO=%d level=%d address=0x%02X\n",
                  BoardPins::AUDIO_CE, digitalRead(BoardPins::AUDIO_CE),
                  Es8311::DEFAULT_ADDRESS);
    Wire.begin(BoardPins::I2C_SDA, BoardPins::I2C_SCL, SHARED_I2C_CLOCK_HZ);
    Wire.setTimeOut(SHARED_I2C_TIMEOUT_MS);
    Serial.printf("[I2C] Shared bus SDA=%d SCL=%d clock=%lu Hz\n",
                  BoardPins::I2C_SDA, BoardPins::I2C_SCL, SHARED_I2C_CLOCK_HZ);

    uint8_t count = 0;
    for (uint8_t address = 1; address < 0x7F; ++address) {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C] Device found at 0x%02X%s%s\n", address,
                          address == Es8311::DEFAULT_ADDRESS ? " (ES8311)" : "",
                          address == FT6X36_ADDR ? " (FT6X36)" : "");
            ++count;
            if (address == Es8311::DEFAULT_ADDRESS) {
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

    Wire.begin(BoardPins::I2C_SDA, BoardPins::I2C_SCL, SHARED_I2C_CLOCK_HZ);
    Wire.setTimeOut(SHARED_I2C_TIMEOUT_MS);
}

void setCalculatorI2cPriority(bool enabled) {
    Wire.setClock(enabled ? CALCULATOR_I2C_CLOCK_HZ : SHARED_I2C_CLOCK_HZ);
    Wire.setTimeOut(enabled ? CALCULATOR_I2C_TIMEOUT_MS : SHARED_I2C_TIMEOUT_MS);
    Serial.printf("[I2C] Calculator touch priority %s: clock=%lu Hz timeout=%u ms\n",
                  enabled ? "enabled" : "disabled",
                  enabled ? CALCULATOR_I2C_CLOCK_HZ : SHARED_I2C_CLOCK_HZ,
                  enabled ? CALCULATOR_I2C_TIMEOUT_MS : SHARED_I2C_TIMEOUT_MS);
}

bool beginAudio() {
    const bool initialized = audio.begin(Es8311::DEFAULT_SAMPLE_RATE);
    if (initialized) {
        audio.setOutputVolume(settingsState.volumePercent);
        audio.setMicrophoneGain(30);
    }
    Serial.printf("[AUDIO] ES8311 address=0x%02X online=%s initialized=%s sample_rate=%lu "
                  "MCLK=%d SCLK=%d LRCLK=%d ESP_TX=%d ESP_RX=%d PA=%d\n",
                  audio.address(), audio.isOnline() ? "yes" : "no",
                  initialized ? "yes" : "no", audio.sampleRate(),
                  BoardPins::AUDIO_MCLK, BoardPins::AUDIO_SCLK, BoardPins::AUDIO_LRCLK,
                  BoardPins::AUDIO_DIN, BoardPins::AUDIO_DOUT, BoardPins::PA_EN);
    if (initialized) {
        // Verify the codec init took effect: all clocks enabled (reg01=0x3F),
        // DAC powered (reg12=0x00), output driver enabled (reg13=0x10).
        uint8_t codecVersion = 0;
        uint8_t reg01 = 0;
        uint8_t reg12 = 0;
        uint8_t reg13 = 0;
        uint8_t dacVolume = 0;
        audio.readRegister(0xFF, codecVersion);
        audio.readRegister(0x01, reg01);
        audio.readRegister(0x12, reg12);
        audio.readRegister(0x13, reg13);
        audio.readRegister(0x32, dacVolume);
        Serial.printf("[AUDIO] Codec ver=0x%02X reg01=0x%02X reg12=0x%02X reg13=0x%02X "
                      "dacVol=0x%02X\n",
                      codecVersion, reg01, reg12, reg13, dacVolume);
    }
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
    if (calculatorRefreshRunning) return;

    const PageId nextPage = touchAction.page;
    touchAction.pending = false;

    if (openingLoadVisible && nextPage != openingLoadPage) {
        UiLoadingIndicator::hide();
        openingLoadVisible = false;
        openingLoadPage = PageId::Main;
    }

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
    if (currentPage == PageId::Recording && nextPage != PageId::Recording) {
        RecordingPage::stop();
    }
    if (currentPage == PageId::Radio && nextPage != PageId::Radio) {
        RadioPage::stop();
    }
    if ((currentPage == PageId::Calculator) != (nextPage == PageId::Calculator)) {
        if (currentPage == PageId::Calculator) {
            calculatorRefreshPending = false;
            calculatorPressHandled = false;
        }
        setCalculatorI2cPriority(nextPage == PageId::Calculator);
    }

    Serial.printf("[UI] Opening %s page\n", pageName(nextPage));
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
    if (nextPage == PageId::Recording) RecordingPage::open();
    if (nextPage == PageId::Cartoon) {
        CartoonPage::setContentUrl(contentUrl);
        CartoonPage::open();
    }
    if (nextPage == PageId::Radio) {
        RadioPage::setContentUrl(contentUrl);
        RadioPage::open();
    }
    const bool fastHomeTransition = nextPage == PageId::Main;
    if (fastHomeTransition) {
        // Clear only the function area below the fixed top bar before drawing
        // the cached Home menu with the partial waveform.
        wipeContentAreaWhite(false);
        MainPage::render(transitionFrame);
    } else {
        rendererFor(nextPage)(transitionFrame);
    }

    // Keep the current top-bar pixels untouched and refresh only the function
    // page area below the 32-pixel bar.
    constexpr uint16_t topBarHeight = 32;
    constexpr size_t rowBytes = XingtaiEpd::WIDTH / 8;
    std::memcpy(transitionFrame, frame, static_cast<size_t>(topBarHeight) * rowBytes);
    // For Home, frame now represents the physical white pre-drive. For every
    // other page it represents the currently displayed page.
    epaper.displayPartial(frame, transitionFrame, 0, topBarHeight,
                          XingtaiEpd::WIDTH,
                          XingtaiEpd::HEIGHT - topBarHeight);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
    epaper.sleep();
    currentPage = nextPage;
    if (nextPage == PageId::Book || nextPage == PageId::Voice ||
        nextPage == PageId::Music || nextPage == PageId::Poem ||
        nextPage == PageId::Word || nextPage == PageId::Cartoon ||
        nextPage == PageId::Radio) {
        startOpeningLibraryLoad(nextPage);
    }
    Serial.printf("[UI] %s page ready\n", pageName(currentPage));
}

}

// GPIO43/GPIO44 are the board's UART0 route to the ML307.
Ml307 cellularModem(Serial0, BoardPins::MODEM_RX, BoardPins::MODEM_TX,
                    BoardPins::MODEM_PWR);

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
    MemoryBudget::log("boot");
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
    RecordingPage::setAudio(&audio);
    uiTaskHandle = xTaskGetCurrentTaskHandle();
    RecordingPage::setTimerEventHandler(onRecordingTimerEvent);
    RadioPage::setAudio(&audio);
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
        Serial.println("[CAL] Tap targets: top-left, top-right, bottom-right, bottom-left");
        for (uint8_t target = 0; target < 4; ++target) {
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
    lastNetworkMode = MainPage::NetworkMode::None;
    MainPage::setNetworkMode(MainPage::NetworkMode::None);
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
    MemoryBudget::log("ready");
    Serial.println("[BOOT] Startup sequence complete; entering touch monitoring");
    Serial.println("========================================");
}

void loop() {
    // Consume one counting notification per EPD update. Partial refresh can
    // exceed one second, so clearing the full count would skip timer labels.
    const bool recordingTimerEvent = ulTaskNotifyTake(pdFALSE, 0) > 0;
    if (recordingTimerEvent && currentPage == PageId::Recording &&
        RecordingPage::takeTimerEvent()) {
        // The timer callback wakes this EPD-owning task. Keep all framebuffer
        // and SPI work here so timer refreshes cannot race another display job.
        refreshCurrentRegion(24, 116, 192, 36);
    }
    serviceTouchInterruptBeforeI2c();
    touch.loop();
    processTouchAction();
    startCalculatorRefresh();
    if (calculatorRefreshRunning) {
        delay(1);
        return;
    }
    processOpeningLibraryLoads();

    // A touch interrupt promotes input handling above every nonessential
    // foreground workflow. While a finger gesture is active, do not advance
    // saves, playback UI, animation, marquee, network indicators, or clock refreshes.
    if (touchWorkflowPriority || touchGestureActive) {
        // Radio row taps queue playback so the audio task is not created from
        // the touch callback. A Tap can arrive without a matching LiftUp after
        // a long e-paper refresh, leaving touchGestureActive set; service the
        // queued station before honoring the gesture-priority early return.
        if (currentPage == PageId::Radio && RadioPage::process()) {
            refreshCurrentPage();
        }
        delay(1);
        return;
    }
    // Touch feedback has priority over a pending 下载中/LOADING top-bar update.
    // Service the indicator only after the current physical gesture and its
    // immediate pressed-state refresh have completed.
    UiLoadingIndicator::service();
    SdCard::processSerialCommand();
    // If the controller emitted no final Tap event, still restore the stopped
    // Voice row after the interrupt-driven audio shutdown.
    if (currentPage == PageId::Voice) refreshVoiceDirtyRows();
    if (currentPage == PageId::Music) refreshMusicDirtyRows();
    if (currentPage == PageId::Poem) refreshPoemDirtyRows();
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
        if (PoemPage::takeReplayButtonRefreshRequest()) {
            refreshPoemReplayButton();
        } else if (PoemPage::takePlaybackIconRefreshRequest()) {
            refreshPoemPlaybackIcon();
        }
    }
    if (currentPage == PageId::Word && WordPage::processAnimation()) {
        refreshWordStrokeWindow(false);
    }
    if (currentPage == PageId::Recording && RecordingPage::process()) {
        refreshCurrentPage();
    }
    if (currentPage == PageId::Radio && RadioPage::process()) {
        refreshCurrentPage();
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
    if (RecordingPage::advanceMarquee(marqueeTop) && currentPage == PageId::Recording) {
        RecordingPage::renderMarquee(transitionFrame, frame);
        epaper.displayPartial(frame, transitionFrame, 43, marqueeTop + 1, 125, 28);
        copyFrameRegion(frame, transitionFrame, 43, marqueeTop + 1, 125, 28);
        epaper.sleep();
    }
    if (RadioPage::advanceMarquee(marqueeTop) && currentPage == PageId::Radio) {
        RadioPage::renderMarquee(transitionFrame, frame);
        epaper.displayPartial(frame, transitionFrame, 40, marqueeTop + 1, 164, 28);
        copyFrameRegion(frame, transitionFrame, 40, marqueeTop + 1, 164, 28);
        epaper.sleep();
    }
    updateNetworkPriority();
    updateNetworkTopbar();
    updateClockPage();
    delay(5);
}
