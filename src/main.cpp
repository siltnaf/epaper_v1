#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"
#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/es8311/es8311.h"
#include "devices/ft6336/FT6X36.h"
#include "devices/sd_card/sd_card.h"
#include "pages/asundar/asundar_page.h"
#include "pages/book/book_page.h"
#include "pages/calculator/calculator_page.h"
#include "pages/clock/clock_page.h"
#include "pages/learn/learn_page.h"
#include "pages/main/main_page.h"
#include "pages/music/music_page.h"
#include "pages/poem/poem_page.h"
#include "pages/settings/settings_page.h"
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
    Calculator,
    Clock,
    Book,
    Voice,
    Music,
    Poem,
    Learn,
};

struct TouchAction {
    PageId page = PageId::Main;
    bool pending = false;
};

static uint8_t frame[XingtaiEpd::FRAME_BYTES];
PageId currentPage = PageId::Main;
TouchAction touchAction;
bool initialEs8311Present = false;
uint8_t initialEs8311Address = 0;
bool initialFt6x36Present = false;

const char *pageName(PageId page) {
    switch (page) {
    case PageId::Settings: return "Settings";
    case PageId::Calculator: return "Calculator";
    case PageId::Clock: return "Clock";
    case PageId::Book: return "Book";
    case PageId::Voice: return "Voice";
    case PageId::Music: return "Music";
    case PageId::Poem: return "Poem";
    case PageId::Learn: return "Learn";
    default: return "Main";
    }
}

PageRenderer rendererFor(PageId page) {
    switch (page) {
    case PageId::Settings: return SettingsPage::render;
    case PageId::Calculator: return CalculatorPage::render;
    case PageId::Clock: return ClockPage::render;
    case PageId::Book: return BookPage::render;
    case PageId::Voice: return VoicePage::render;
    case PageId::Music: return MusicPage::render;
    case PageId::Poem: return PoemPage::render;
    case PageId::Learn: return LearnPage::render;
    default: return MainPage::render;
    }
}

bool rawToDisplay(TPoint raw, int16_t &x, int16_t &y) {
    // The touch glass is rotated 90 degrees relative to the portrait EPD. The
    // displayed framebuffer is also rotated 180 degrees at the EPD boundary,
    // so rotate touch coordinates to keep hit targets aligned with the UI.
    x = static_cast<int16_t>(XingtaiEpd::WIDTH - 1 - raw.y);
    y = static_cast<int16_t>(raw.x);
    return x >= 0 && x < XingtaiEpd::WIDTH && y >= 0 && y < XingtaiEpd::HEIGHT;
}

bool pointInRect(int16_t x, int16_t y, int16_t left, int16_t top, int16_t width, int16_t height) {
    return x >= left && x < left + width && y >= top && y < top + height;
}

PageId mainPageAt(int16_t x, int16_t y) {
    constexpr int16_t iconSize = 48;
    constexpr int16_t hitPadding = 10;
    constexpr int16_t columnGap = 32;
    constexpr int16_t rowGap = 22;
    constexpr int16_t startX = (XingtaiEpd::WIDTH - iconSize * 3 - columnGap * 2) / 2;
    constexpr int16_t startY = 56;
    constexpr PageId pages[3][3] = {
        {PageId::Settings, PageId::Calculator, PageId::Clock},
        {PageId::Book, PageId::Voice, PageId::Music},
        {PageId::Poem, PageId::Learn, PageId::Main},
    };

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            if (row == 2 && column == 2) continue;
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
    int16_t x = 0;
    int16_t y = 0;
    if (!rawToDisplay(point, x, y)) {
        Serial.printf("[TOUCH] out of range x=%d y=%d raw_x=%u raw_y=%u event=%u\n",
                      x, y, point.x, point.y, static_cast<unsigned>(event));
        return;
    }

    if (event != TEvent::Tap) return;
    Serial.printf("[TOUCH] tap x=%d y=%d raw_x=%u raw_y=%u\n", x, y, point.x, point.y);

    if (currentPage != PageId::Main) {
        // Reserve a consistent upper-left Home touch target on content pages.
        if (pointInRect(x, y, 0, 0, 40, 32)) queuePage(PageId::Main);
        return;
    }

    queuePage(mainPageAt(x, y));
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
        audio.setOutputVolume(60);
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
    rendererFor(nextPage)(frame);
    epaper.display(frame);
    epaper.sleep();
    currentPage = nextPage;
    Serial.printf("[UI] %s page ready\n", pageName(currentPage));
}

}

void setup() {
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
    Serial.println("[BOOT] Powering touch controller...");
    powerTouch();
    Serial.println("[BOOT] Initializing shared I2C bus...");
    beginSharedI2c();
    Serial.println("[BOOT] Initializing ES8311 audio codec...");
    beginAudio();
    Serial.println("[BOOT] ES8311 audio initialization complete");
    Serial.println("[BOOT] Initializing touch controller...");
    beginTouch();
    Serial.println("[BOOT] Touch controller initialization complete");

    Serial.println("[BOOT] Rendering Asundar splash frame...");
    AsundarPage::render(frame);

    Serial.println("[BOOT] Initializing e-paper controller...");
    epaper.begin();
    Serial.println("[BOOT] Refreshing Asundar splash...");
    epaper.display(frame);
    Serial.println("[BOOT] Asundar splash refresh complete");

    // Keep the splash-to-home transition direct: do not clear the panel or
    // insert a delay. The controller still needs its mandatory refresh to
    // replace the splash pixels with the home pixels.
    Serial.println("[BOOT] Rendering main page frame...");
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
    delay(5);
}
