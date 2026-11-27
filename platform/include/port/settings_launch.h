#pragma once
#include "port/settings_ui.h"
namespace port::settings {
enum class LaunchRequest : uint8_t { Play, Settings, Malformed };
LaunchRequest ParseLaunch(int, const char* const*, const char*, size_t);
LanguageCatalog ReadLanguages();
std::vector<Override> ApplyStartup(const Config&, bool settingsEnabled);
// gfx26: GXV_QUALITY_SET (port/quality_set.h) for the Picture mode of `config`, once per process; false when it was not asked for or already applied.
// QualitySetCensus() is the "qualityset:" log line ("" when nothing was applied).
bool ApplyQualitySet(const Config&);
// The same without Settings (GXV_SETTINGS=0): the mode is read from GXV_AUTOQ_TARGET / GXV_FPS_LIMIT. Called from entry.cpp after the Settings start-up.
bool ApplyQualitySetFromEnv();
const std::string& QualitySetCensus();
// q30-26: the Quality (30) mode's umbrella levels (port/quality30.h), applied inside ApplyStartup when the mode is 30; the "quality30:" log line ("" in Performance).
const std::string& Quality30Census();
// deadfix27: the per-mode key table (port/mode_keys.h) applied inside ApplyStartup; the "modekeys:" log line.
const std::string& ModeKeysCensus();
const std::string& PerfDefaultsCensus();   // (pace33 PATCH8: the Performance umbrella defaults, port/perf_defaults.h)
// The same without Settings (GXV_SETTINGS=0); a no-op when ApplyStartup did it. Called from entry.cpp after the Settings start-up.
bool ApplyModeKeysFromEnv();
}
