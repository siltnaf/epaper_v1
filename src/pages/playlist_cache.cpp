#include "pages/playlist_cache.h"

#include <SD_MMC.h>

#include "devices/sd_card/sd_card.h"
#include "memory_budget.h"

namespace {

constexpr size_t MAX_PLAYLIST_BYTES = MemoryBudget::MAX_JSON_PAYLOAD;
bool cacheEnabled = false;

uint32_t endpointHash(const String &endpoint) {
    uint32_t hash = 2166136261UL;
    for (size_t index = 0; index < endpoint.length(); ++index) {
        hash ^= static_cast<uint8_t>(endpoint[index]);
        hash *= 16777619UL;
    }
    return hash;
}

bool cachePath(const char *folder, const String &endpoint, const char *slot,
               char *path, size_t pathSize) {
    if (!folder || !folder[0] || !slot || !slot[0] || !path || pathSize == 0 ||
        !SdCard::isMounted()) return false;
    if (!SD_MMC.exists(folder) && !SD_MMC.mkdir(folder)) return false;
    char safeSlot[32] = {};
    size_t written = 0;
    for (size_t index = 0; slot[index] && written + 1 < sizeof(safeSlot); ++index) {
        const char value = slot[index];
        safeSlot[written++] = ((value >= 'a' && value <= 'z') ||
                               (value >= 'A' && value <= 'Z') ||
                               (value >= '0' && value <= '9') || value == '-')
            ? value : '_';
    }
    snprintf(path, pathSize, "%s/.playlist-%08lx-%s.json", folder,
             static_cast<unsigned long>(endpointHash(endpoint)), safeSlot);
    return true;
}

} // namespace

namespace PlaylistCache {

void setEnabled(bool enabled) { cacheEnabled = enabled; }

bool isEnabled() { return cacheEnabled; }

bool load(const char *folder, const String &endpoint, const char *slot, String &payload) {
    if (!cacheEnabled) return false;
    SdCard::Lock sdLock;
    if (!sdLock.acquired()) return false;
    char path[128] = {};
    if (!cachePath(folder, endpoint, slot, path, sizeof(path))) return false;
    File file = SD_MMC.open(path, FILE_READ);
    if (!file || file.size() == 0 || file.size() > MAX_PLAYLIST_BYTES ||
        !MemoryBudget::canAllocate(static_cast<size_t>(file.size()))) {
        if (file) file.close();
        return false;
    }
    payload.reserve(static_cast<unsigned>(file.size()));
    payload = file.readString();
    file.close();
    Serial.printf("[PLAYLIST CACHE] hit path=%s bytes=%u\n", path,
                  static_cast<unsigned>(payload.length()));
    return !payload.isEmpty();
}

bool save(const char *folder, const String &endpoint, const char *slot,
          const String &payload) {
    if (!cacheEnabled) return false;
    SdCard::Lock sdLock;
    if (!sdLock.acquired()) return false;
    if (payload.isEmpty() || payload.length() > MAX_PLAYLIST_BYTES) return false;
    char path[128] = {};
    if (!cachePath(folder, endpoint, slot, path, sizeof(path))) return false;
    char temporary[136] = {};
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    SD_MMC.remove(temporary);
    File file = SD_MMC.open(temporary, FILE_WRITE);
    if (!file || file.print(payload) != payload.length()) {
        if (file) file.close();
        SD_MMC.remove(temporary);
        return false;
    }
    file.close();
    SD_MMC.remove(path);
    const bool renamed = SD_MMC.rename(temporary, path);
    if (!renamed) SD_MMC.remove(temporary);
    Serial.printf("[PLAYLIST CACHE] save path=%s bytes=%u result=%s\n", path,
                  static_cast<unsigned>(payload.length()), renamed ? "ok" : "failed");
    return renamed;
}

} // namespace PlaylistCache