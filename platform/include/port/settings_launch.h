#pragma once
#include "port/settings_ui.h"
namespace port::settings {
enum class LaunchRequest : uint8_t { Play, Settings, Malformed };
LaunchRequest ParseLaunch(int, const char* const*, const char*, size_t);
LanguageCatalog ReadLanguages();
std::vector<Override> ApplyStartup(const Config&, bool settingsEnabled);
}
