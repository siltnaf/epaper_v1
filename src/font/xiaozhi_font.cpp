#include "font/xiaozhi_font.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <WiFi.h>

#include <cstring>

#include "devices/sd_card/sd_card.h"

namespace {

constexpr char FONT_CACHE_PATH[] = "/fonts/xiaozhi-puhui-20.assets.bin";
constexpr char DEFAULT_FONT_DOWNLOAD_URL[] =
    "https://github.com/78/xiaozhi-fonts/releases/download/assets/"
    "none-font_puhui_common_20_4-none.bin";
constexpr char FONT_PREFERENCES_NAMESPACE[] = "xiaozhi-font";
constexpr char FONT_URL_PREFERENCE_KEY[] = "lastUrl";
constexpr char FONT_ASSET_NAME[] = "font_puhui_common_20_4.bin";
constexpr uint32_t FONT_DOWNLOAD_MIN_BYTES = 1200000;
constexpr uint8_t MAX_CMAPS = 32;
constexpr uint8_t GLYPH_CACHE_SIZE = 24;
constexpr uint16_t MAX_GLYPH_BITMAP_BYTES = sizeof(XiaozhiFont::Glyph::bitmap);

struct Cmap {
    uint32_t rangeStart = 0;
    uint16_t rangeLength = 0;
    uint16_t glyphIdStart = 0;
    uint32_t unicodeListOffset = 0;
    uint32_t glyphOffsetListOffset = 0;
    uint16_t listLength = 0;
    uint8_t type = 0;
};

struct CacheEntry {
    bool valid = false;
    uint32_t codepoint = 0;
    XiaozhiFont::Glyph glyph;
};

File fontFile;
bool available = false;
volatile bool provisioning = false;
volatile bool workerStarted = false;
uint32_t fontBase = 0;
uint32_t glyphBitmapBase = 0;
uint32_t glyphDescriptorBase = 0;
uint32_t cmapsBase = 0;
uint8_t bitsPerPixel = 0;
uint8_t cmapCount = 0;
Cmap cmaps[MAX_CMAPS] = {};
CacheEntry glyphCache[GLYPH_CACHE_SIZE] = {};
uint8_t nextCacheSlot = 0;

bool readAt(uint32_t offset, void *destination, size_t length) {
    if (!fontFile || !destination || !fontFile.seek(offset)) return false;
    return fontFile.read(static_cast<uint8_t *>(destination), length) == length;
}

bool readU8(uint32_t offset, uint8_t &value) {
    return readAt(offset, &value, sizeof(value));
}

bool readU16(uint32_t offset, uint16_t &value) {
    uint8_t bytes[2] = {};
    if (!readAt(offset, bytes, sizeof(bytes))) return false;
    value = static_cast<uint16_t>(bytes[0]) |
            static_cast<uint16_t>(bytes[1]) << 8;
    return true;
}

bool readU32(uint32_t offset, uint32_t &value) {
    uint8_t bytes[4] = {};
    if (!readAt(offset, bytes, sizeof(bytes))) return false;
    value = static_cast<uint32_t>(bytes[0]) |
            static_cast<uint32_t>(bytes[1]) << 8 |
            static_cast<uint32_t>(bytes[2]) << 16 |
            static_cast<uint32_t>(bytes[3]) << 24;
    return true;
}

bool locateFontAsset(uint32_t &base) {
    uint32_t fileCount = 0;
    uint32_t storedLength = 0;
    if (!readU32(0, fileCount) || !readU32(8, storedLength)) return false;

    // A raw CBIN can also be manually placed at the cache path.
    if (fileCount == 0 && fontFile.size() >= FONT_DOWNLOAD_MIN_BYTES) {
        base = 0;
        return true;
    }
    if (fileCount == 0 || fileCount > 16 || storedLength > fontFile.size() - 12) return false;

    constexpr uint32_t TABLE_START = 12;
    constexpr uint32_t TABLE_ENTRY_BYTES = 44;
    const uint32_t dataStart = TABLE_START + fileCount * TABLE_ENTRY_BYTES;
    for (uint32_t index = 0; index < fileCount; ++index) {
        const uint32_t entry = TABLE_START + index * TABLE_ENTRY_BYTES;
        char name[33] = {};
        uint32_t size = 0;
        uint32_t offset = 0;
        if (!readAt(entry, name, 32) || !readU32(entry + 32, size) ||
            !readU32(entry + 36, offset)) return false;
        if (std::strncmp(name, FONT_ASSET_NAME, 32) != 0) continue;
        uint8_t magic[2] = {};
        const uint32_t payload = dataStart + offset;
        if (!readAt(payload, magic, sizeof(magic)) || magic[0] != 'Z' || magic[1] != 'Z') {
            return false;
        }
        if (payload + 2 + size > fontFile.size()) return false;
        base = payload + 2;
        return true;
    }
    return false;
}

bool openFont() {
    if (fontFile) fontFile.close();
    fontFile = SD_MMC.open(FONT_CACHE_PATH, FILE_READ);
    if (!fontFile) return false;
    if (!locateFontAsset(fontBase)) {
        Serial.println("[FONT] Xiaozhi asset table is invalid");
        fontFile.close();
        return false;
    }

    // The downloaded CBIN is generated for LVGL 9 on a 32-bit target. Its
    // lv_font_t stores the descriptor's relative offset at byte 24.
    uint32_t descriptorRelative = 0;
    if (!readU32(fontBase + 24, descriptorRelative)) return false;
    const uint32_t descriptorBase = fontBase + descriptorRelative;
    uint32_t bitmapRelative = 0;
    uint32_t glyphRelative = 0;
    uint32_t cmapRelative = 0;
    uint16_t flags = 0;
    if (!readU32(descriptorBase, bitmapRelative) ||
        !readU32(descriptorBase + 4, glyphRelative) ||
        !readU32(descriptorBase + 8, cmapRelative) ||
        !readU16(descriptorBase + 18, flags)) return false;
    glyphBitmapBase = descriptorBase + bitmapRelative;
    glyphDescriptorBase = descriptorBase + glyphRelative;
    cmapsBase = descriptorBase + cmapRelative;
    cmapCount = static_cast<uint8_t>(flags & 0x01FFU);
    bitsPerPixel = static_cast<uint8_t>((flags >> 9) & 0x0FU);
    if (cmapCount == 0 || cmapCount > MAX_CMAPS || bitsPerPixel != 4) {
        Serial.printf("[FONT] Unsupported CBIN cmap_count=%u bpp=%u\n", cmapCount, bitsPerPixel);
        return false;
    }

    for (uint8_t index = 0; index < cmapCount; ++index) {
        const uint32_t offset = cmapsBase + static_cast<uint32_t>(index) * 20U;
        Cmap &cmap = cmaps[index];
        if (!readU32(offset, cmap.rangeStart) ||
            !readU16(offset + 4, cmap.rangeLength) ||
            !readU16(offset + 6, cmap.glyphIdStart) ||
            !readU32(offset + 8, cmap.unicodeListOffset) ||
            !readU32(offset + 12, cmap.glyphOffsetListOffset) ||
            !readU16(offset + 16, cmap.listLength) ||
            !readU8(offset + 18, cmap.type)) return false;
    }

    for (CacheEntry &entry : glyphCache) entry.valid = false;
    nextCacheSlot = 0;
    available = true;
    Serial.printf("[FONT] Xiaozhi PuHui ready from %s (%u cmaps, %u bpp)\n",
                  FONT_CACHE_PATH, cmapCount, bitsPerPixel);
    return true;
}

String rememberedDownloadUrl() {
    Preferences preferences;
    if (!preferences.begin(FONT_PREFERENCES_NAMESPACE, true)) {
        return String(DEFAULT_FONT_DOWNLOAD_URL);
    }
    const String url = preferences.getString(FONT_URL_PREFERENCE_KEY,
                                             DEFAULT_FONT_DOWNLOAD_URL);
    preferences.end();
    return url.length() > 0 ? url : String(DEFAULT_FONT_DOWNLOAD_URL);
}

void rememberSuccessfulUrl(const String &url) {
    Preferences preferences;
    if (!preferences.begin(FONT_PREFERENCES_NAMESPACE, false)) return;
    preferences.putString(FONT_URL_PREFERENCE_KEY, url);
    preferences.end();
    Serial.printf("[FONT] Saved working Xiaozhi URL to flash: %s\n", url.c_str());
}

void provisionTask(void *) {
    provisioning = true;
    Serial.println("[FONT] Background provisioner started");
    if (SD_MMC.exists(FONT_CACHE_PATH) && openFont()) {
        Serial.println("[FONT] Existing SD cache is valid; no download needed");
        provisioning = false;
        vTaskDelete(nullptr);
        return;
    }
    SD_MMC.remove(FONT_CACHE_PATH);

    Serial.println("[FONT] Cache missing; waiting for the preferred usable Internet transport");
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    Serial.printf("[FONT] Internet ready through WiFi, IP=%s\n",
                  WiFi.localIP().toString().c_str());

    const String remembered = rememberedDownloadUrl();
    bool downloaded = SdCard::downloadFile(remembered.c_str(), FONT_CACHE_PATH,
                                            FONT_DOWNLOAD_MIN_BYTES);
    String successfulUrl = remembered;
    if (!downloaded && remembered != DEFAULT_FONT_DOWNLOAD_URL) {
        Serial.println("[FONT] Remembered URL failed; retrying the built-in Xiaozhi release URL");
        downloaded = SdCard::downloadFile(DEFAULT_FONT_DOWNLOAD_URL, FONT_CACHE_PATH,
                                          FONT_DOWNLOAD_MIN_BYTES);
        successfulUrl = DEFAULT_FONT_DOWNLOAD_URL;
    }
    if (downloaded && openFont()) {
        rememberSuccessfulUrl(successfulUrl);
    } else {
        Serial.println("[FONT] Background font provisioning failed; it will retry next boot");
    }
    provisioning = false;
    vTaskDelete(nullptr);
}

int32_t sparseIndex(const Cmap &cmap, uint16_t relativeCodepoint) {
    int32_t low = 0;
    int32_t high = static_cast<int32_t>(cmap.listLength) - 1;
    while (low <= high) {
        const int32_t middle = low + (high - low) / 2;
        uint16_t value = 0;
        if (!readU16(cmapsBase + cmap.unicodeListOffset + middle * 2U, value)) return -1;
        if (value == relativeCodepoint) return middle;
        if (value < relativeCodepoint) low = middle + 1;
        else high = middle - 1;
    }
    return -1;
}

uint32_t glyphIdFor(uint32_t codepoint) {
    for (uint8_t index = 0; index < cmapCount; ++index) {
        const Cmap &cmap = cmaps[index];
        if (codepoint < cmap.rangeStart || codepoint >= cmap.rangeStart + cmap.rangeLength) continue;
        const uint16_t relative = static_cast<uint16_t>(codepoint - cmap.rangeStart);
        if (cmap.type == 2) return cmap.glyphIdStart + relative; // FORMAT0_TINY
        if (cmap.type == 0) { // FORMAT0_FULL
            uint8_t glyphOffset = 0;
            if (!readU8(cmapsBase + cmap.glyphOffsetListOffset + relative, glyphOffset)) return 0;
            return cmap.glyphIdStart + glyphOffset;
        }
        const int32_t sparse = sparseIndex(cmap, relative);
        // CMAP ranges can overlap. A sparse map that covers the numeric range
        // but does not list this codepoint must not hide a later map.
        if (sparse < 0) continue;
        if (cmap.type == 3) return cmap.glyphIdStart + sparse; // SPARSE_TINY
        if (cmap.type == 1) { // SPARSE_FULL
            uint16_t glyphOffset = 0;
            if (!readU16(cmapsBase + cmap.glyphOffsetListOffset + sparse * 2U, glyphOffset)) return 0;
            return cmap.glyphIdStart + glyphOffset;
        }
    }
    return 0;
}

bool loadGlyph(uint32_t codepoint, XiaozhiFont::Glyph &glyph) {
    const uint32_t glyphId = glyphIdFor(codepoint);
    if (glyphId == 0) return false;

    // This font exceeds 1 MiB, so lv_font_conv emits the "large" 16-byte
    // descriptor: bitmap index, advance (1/16 px), box, and signed offsets.
    const uint32_t descriptor = glyphDescriptorBase + glyphId * 16U;
    uint32_t bitmapIndex = 0;
    uint32_t advance16 = 0;
    uint16_t rawOffsetX = 0;
    uint16_t rawOffsetY = 0;
    if (!readU32(descriptor, bitmapIndex) || !readU32(descriptor + 4, advance16) ||
        !readU16(descriptor + 8, glyph.width) || !readU16(descriptor + 10, glyph.height) ||
        !readU16(descriptor + 12, rawOffsetX) || !readU16(descriptor + 14, rawOffsetY)) return false;
    glyph.offsetX = static_cast<int16_t>(rawOffsetX);
    glyph.offsetY = static_cast<int16_t>(rawOffsetY);
    glyph.advance = static_cast<uint16_t>((advance16 + 8U) / 16U);
    glyph.bitmapBytes = static_cast<uint16_t>((glyph.width * glyph.height * bitsPerPixel + 7U) / 8U);
    if (glyph.bitmapBytes > MAX_GLYPH_BITMAP_BYTES) return false;
    if (glyph.bitmapBytes > 0 && !readAt(glyphBitmapBase + bitmapIndex, glyph.bitmap,
                                          glyph.bitmapBytes)) return false;
    return true;
}

}

namespace XiaozhiFont {

void beginBackgroundProvisioning() {
    if (available || workerStarted) return;
    if (!SdCard::isMounted()) {
        Serial.println("[FONT] SD card is required for the Xiaozhi Chinese font");
        return;
    }
    if (SD_MMC.exists(FONT_CACHE_PATH) && openFont()) {
        Serial.println("[FONT] Found usable Xiaozhi font cache during startup");
        return;
    }
    workerStarted = xTaskCreate(provisionTask, "font-provision", 8192, nullptr, 1, nullptr) == pdPASS;
    if (!workerStarted) {
        Serial.println("[FONT] Could not start background font provisioner");
    }
}

bool isAvailable() {
    return available;
}

bool isProvisioning() {
    return provisioning;
}

const Glyph *glyph(uint32_t codepoint) {
    if (!available) return nullptr;
    for (CacheEntry &entry : glyphCache) {
        if (entry.valid && entry.codepoint == codepoint) return &entry.glyph;
    }
    CacheEntry &entry = glyphCache[nextCacheSlot++ % GLYPH_CACHE_SIZE];
    entry.valid = loadGlyph(codepoint, entry.glyph);
    entry.codepoint = codepoint;
    return entry.valid ? &entry.glyph : nullptr;
}

const char *cachePath() {
    return FONT_CACHE_PATH;
}

}