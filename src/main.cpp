#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <WiFi.h>

#include <cstring>

#include "board_pins.h"
#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/es8311/es8311.h"
#include "devices/ft6336/FT6X36.h"
#include "devices/sd_card/sd_card.h"
#include "pages/asundar/asundar_page.h"
#include "pages/book/book_page.h"
#include "pages/calendar/calendar_page.h"
#include "pages/calculator/calculator_page.h"
#include "pages/clock/clock_page.h"
#include "pages/learn/learn_page.h"
#include "pages/main/main_page.h"
#include "pages/music/music_page.h"
#include "pages/poem/poem_page.h"
#include "pages/recording/recording_page.h"
#include "pages/settings/settings_page.h"
#include "pages/topbar/topbar_assets.h"
#include "pages/voice/voice_page.h"

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
    Learn,
    Recording,
};

struct TouchAction {
    PageId page = PageId::Main;
    bool pending = false;
};

static uint8_t frame[XingtaiEpd::FRAME_BYTES];
static uint8_t transitionFrame[XingtaiEpd::FRAME_BYTES];
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
char savedWifiSsid[33] = {};
char savedWifiPassword[64] = {};
char scannedWifiNetworks[6][33] = {};
uint8_t scannedWifiCount = 0;
char wifiPasswordInput[64] = {};
char selectedWifiSsid[33] = {};
char sdEntryNames[8][33] = {};
bool sdEntryDirectories[8] = {};
char contentUrl[128] = {};

void recoverSharedI2c();
void refreshCurrentPage();

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

void applyWifiSetting() {
    if (settingsState.wifiEnabled) {
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
        if (savedWifiSsid[0] != '\0') WiFi.begin(savedWifiSsid, savedWifiPassword);
    } else {
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
    }
    Serial.printf("[SETTINGS] WiFi %s\n", settingsState.wifiEnabled ? "enabled" : "disabled");
}

void applyCellularSetting() {
    pinMode(BoardPins::MODEM_PWR, OUTPUT);
    digitalWrite(BoardPins::MODEM_PWR, settingsState.cellularEnabled ? HIGH : LOW);
    Serial.printf("[SETTINGS] 4G modem power %s\n", settingsState.cellularEnabled ? "enabled" : "disabled");
}

void applyAudioSetting() {
    if (audio.isInitialized()) audio.setOutputVolume(settingsState.volumePercent);
    audio.setSpeakerEnabled(settingsState.volumePercent > 0);
    Serial.printf("[SETTINGS] Audio volume %u%%\n", settingsState.volumePercent);
}

void loadSettings() {
    if (settingsPreferences.begin("settings", true)) {
        settingsState.wifiEnabled = settingsPreferences.getBool("wifi", true);
        settingsState.cellularEnabled = settingsPreferences.getBool("cellular", true);
        settingsState.volumePercent = settingsPreferences.getUChar("volume", 60);
        settingsState.language = settingsPreferences.getUChar("language", 0);
        const String savedContentUrl = settingsPreferences.getString("contentUrl", "");
        const String savedVoice = settingsPreferences.getString("ttsVoice", "Jasper");
        std::strncpy(contentUrl, savedContentUrl.c_str(), sizeof(contentUrl) - 1);
        SettingsPage::setVoice(savedVoice.c_str());
        settingsPreferences.end();
    }
    if (settingsState.volumePercent > 100) settingsState.volumePercent = 100;
    SettingsPage::setState(settingsState);
    loadWifiCredentials();
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
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 10000) delay(100);
    if (WiFi.status() == WL_CONNECTED) {
        saveWifiCredentials();
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
    case PageId::Learn: return "Learn";
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
    case PageId::Learn: return LearnPage::render;
    case PageId::Recording: return RecordingPage::render;
    default: return MainPage::render;
    }
}

void refreshCurrentPage() {
    rendererFor(currentPage)(transitionFrame);
    constexpr size_t topBarBytes = static_cast<size_t>(32) * (XingtaiEpd::WIDTH / 8);
    std::memcpy(transitionFrame, frame, topBarBytes);
    epaper.displayPartial(frame, transitionFrame, 0, 32,
                          XingtaiEpd::WIDTH, XingtaiEpd::HEIGHT - 32);
    std::memcpy(frame, transitionFrame, XingtaiEpd::FRAME_BYTES);
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

void clearFrameRegion(uint8_t *buffer, int left, int top, int width, int height) {
    for (int y = top; y < top + height; ++y) {
        for (int x = left; x < left + width; ++x) {
            buffer[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] &=
                static_cast<uint8_t>(~(0x80U >> (x % 8)));
        }
    }
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
    case PageId::Learn: return MainPage::FunctionIcon::Learn;
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
    case PageId::Learn: column = 2; row = 2; break;
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
        {PageId::Music, PageId::Poem, PageId::Learn},
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

    if (event != TEvent::Tap) return;
    // The EPD presents the framebuffer rotated 180 degrees relative to the
    // touch panel's calibrated physical coordinates.
    const int16_t uiX = XingtaiEpd::WIDTH - 1 - x;
    const int16_t uiY = XingtaiEpd::HEIGHT - 1 - y;
    Serial.printf("[TOUCH] TAP raw=(%u,%u) mapped=(%d,%d) ui=(%d,%d)\n",
                  point.x, point.y, x, y, uiX, uiY);

    // Home is a global navigation action. Handle it before any page-specific
    // touch routing so every current and future page returns to the main page.
    if (isHomeIcon(uiX, uiY)) {
        Serial.println("[NAVIGATION] Home tapped");
        queuePage(PageId::Main);
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
            applyWifiSetting();
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
            if (length > 0) contentUrl[length - 1] = '\0';
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
            saveSettings();
            SettingsPage::showSettings();
            refreshCurrentPage();
            return;
        case SettingsPage::Action::UrlCancel:
            if (settingsPreferences.begin("settings", true)) {
                const String saved = settingsPreferences.getString("contentUrl", "");
                settingsPreferences.end();
                std::strncpy(contentUrl, saved.c_str(), sizeof(contentUrl) - 1);
                contentUrl[sizeof(contentUrl) - 1] = '\0';
            }
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

    Serial.printf("[UI] Opening %s page\n", pageName(nextPage));
    if (nextPage == PageId::Settings) SettingsPage::showSettings();
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
    Serial.printf("[BOOT] Serial ready: %lu baud\n", 115200UL);
    Serial.println("[BOOT] Firmware: 3.7-inch e-paper portrait test");
    Serial.printf("[BOOT] EPD pins: DIN=%d CLK=%d CS=%d DC=%d RST=%d BUSY=%d\n",
                  BoardPins::EP_DIN, BoardPins::EP_CLK, BoardPins::EP_CS,
                  BoardPins::EP_DC, BoardPins::EP_RST, BoardPins::EP_BUSY);
    Serial.println("[BOOT] Checking SD card...");
    SdCard::begin();
    SettingsPage::setSdMounted(SdCard::isMounted());
    loadSettings();
    applyWifiSetting();
    applyCellularSetting();
    Serial.println("[BOOT] Powering touch controller...");
    powerTouch();
    Serial.println("[BOOT] Initializing shared I2C bus...");
    beginSharedI2c();
    Serial.println("[BOOT] Initializing ES8311 audio codec...");
    beginAudio();
    applyAudioSetting();
    Serial.println("[BOOT] ES8311 audio initialization complete");
    Serial.println("[BOOT] Initializing touch controller...");
    beginTouch();
    Serial.println("[BOOT] Touch controller initialization complete");

    Serial.println("[BOOT] Rendering Asundar splash frame...");
    AsundarPage::render(frame);

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
    Serial.println("[BOOT] Refreshing Asundar splash...");
    epaper.display(frame);
    Serial.println("[BOOT] Asundar splash refresh complete");

    // Keep the splash-to-home transition direct: do not clear the panel or
    // insert a delay. The controller still needs its mandatory refresh to
    // replace the splash pixels with the home pixels.
    Serial.println("[BOOT] Rendering main page frame...");
    // Always establish a clean initial top bar. The runtime state monitor adds
    // Wi-Fi only after the Home frame is physically present and connectivity
    // has been observed in the normal event loop.
    lastWifiConnected = false;
    MainPage::setWifiConnected(false);
    MainPage::render(frame);
    Serial.println("[BOOT] Refreshing main page...");
    epaper.display(frame);
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
    touch.loop();
    processTouchAction();
    updateWifiTopbar();
    delay(5);
}
