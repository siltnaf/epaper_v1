#pragma once
#include <stdint.h>

namespace SettingsPage {

struct State {
    bool wifiEnabled;
    bool cellularEnabled;
    uint8_t volumePercent;
    uint8_t language;
};

enum class Action : uint8_t {
    None,
    OpenContentUrl,
    OpenWifiSetup,
    ToggleWifi,
    ToggleCellular,
    RefreshSdCard,
    CycleLanguage,
    OpenVoiceSelection,
    SelectVoice,
    VoiceBack,
    SdBack,
    SdRequestFormat,
    SdConfirmFormat,
    VolumeDown,
    VolumeUp,
    TestAudio,
    RetryWifiScan,
    SelectWifiNetwork,
    WifiKey,
    WifiBackspace,
    WifiSpace,
    WifiChangeKeyboard,
    WifiConnect,
    WifiCancel,
    UrlKey,
    UrlBackspace,
    UrlSpace,
    UrlChangeKeyboard,
    UrlSave,
    UrlCancel,
};

void render(uint8_t *frame);
void setState(const State &state);
void setSdMounted(bool mounted);
void showSdPage(const char names[][33], const bool directories[], uint8_t count);
void setFormatPending(bool pending);
void showSettings();
Action actionAt(int16_t x, int16_t y);
void showWifiScanning();
void showWifiNetworks(const char networks[][33], uint8_t count);
void showWifiPassword(const char *ssid, const char *password);
void showContentUrl(const char *url);
void setVoice(const char *voice);
const char *voiceName();
const char *voiceNameAt(uint8_t index);
uint8_t voiceIndexAt(int16_t x, int16_t y);
void showVoiceSelection();
void cycleKeyboard();
uint8_t networkIndexAt(int16_t x, int16_t y);
char keyAt(int16_t x, int16_t y);

}