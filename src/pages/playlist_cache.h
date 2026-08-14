#pragma once

#include <Arduino.h>

namespace PlaylistCache {

bool load(const char *folder, const String &endpoint, const char *slot, String &payload);
bool save(const char *folder, const String &endpoint, const char *slot, const String &payload);

} // namespace PlaylistCache