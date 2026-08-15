#pragma once

#include <Arduino.h>
#include <esp_heap_caps.h>

namespace MemoryBudget {

// This board has 320 KiB internal RAM and no PSRAM. Keep both a total-heap
// reserve and a contiguous-block reserve because TLS and decoder allocations
// need one sizeable block, not just enough aggregate free bytes.
constexpr size_t MIN_FREE_HEAP = 16 * 1024;
constexpr size_t MIN_LARGEST_BLOCK = 8 * 1024;
constexpr size_t MAX_JSON_PAYLOAD = 16 * 1024;

inline uint32_t freeHeap() {
    return ESP.getFreeHeap();
}

inline uint32_t largestBlock() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

inline bool canAllocate(size_t requiredBytes,
                        size_t freeReserve = MIN_FREE_HEAP,
                        size_t blockReserve = MIN_LARGEST_BLOCK) {
    return freeHeap() >= requiredBytes + freeReserve &&
           largestBlock() >= requiredBytes + blockReserve;
}

inline void log(const char *stage) {
    Serial.printf("[MEM] %s free=%u largest=%u min_free=%u stack_free=%u\n",
                  stage ? stage : "status",
                  static_cast<unsigned>(freeHeap()),
                  static_cast<unsigned>(largestBlock()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()),
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

} // namespace MemoryBudget