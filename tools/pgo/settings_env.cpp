// Host helper of tools/pgo/gen_vita_env.py: what the console's Settings start-up (vita/vgx-game/settings_runtime.cpp
// port_settings_startup -> NativeStartup -> port::settings::ApplyStartup) adds to the environment on a fresh console
// (no env.txt, no settings file: the default Config, Safe Saves on), after entry.cpp's own defaults are set. The
// harness has no Settings start-up, so without this layer a profile is recorded with the start-up values of the
// picture mode (GXV_AUTOQ_TARGET=30, GXV_HRW=0, no governors ...) instead of what the console runs.
//
//   env -i <entry.cpp defaults> settings_env [60|30]    -> the whole environment afterwards, one NAME=VALUE a line
//
// 60 (default) = Performance ("Smooth Motion", the release default), 30 = Quality ("Full Detail").
// Built and run by gen_vita_env.py (clang++ -std=c++20, the four sources it names); nothing of it goes into a game build.
#include "port/mode_keys.h"
#include "port/perf_defaults.h"
#include "port/player_settings.h"
#include "port/quality_set.h"
#include "port/settings_launch.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern char** environ;
// the language list comes from the disc on the console; the start-up only reads it for the language row
extern "C" int port_language_count() { return 0; }
extern "C" const char* port_language_name(int) { return ""; }
extern "C" void port_language_set(const char*) {}

int main(int argc, char** argv) {
    const bool q30 = argc > 1 && std::strcmp(argv[1], "30") == 0;
    // entry.cpp takes these snapshots right after env.txt is read, before any default: a fresh console has no env.txt
    auto none = [](const char*) -> const char* { return nullptr; };
    port::qualityset::SnapshotUser(none);
    port::modekeys::SnapshotUser(none);
    port::perfdefaults::SnapshotUser(none);
    auto c = port::settings::DefaultConfig(port::controls::GetControlApi());
    c.safeSaves = true;   // (NativeRetire: always on in 3.0)
    if (q30) c.fpsLimit = 30, c.performance60 = false;
    const auto overrides = port::settings::ApplyStartup(c, true);
    for (char** e = environ; *e; ++e) std::printf("%s\n", *e);
    std::fprintf(stderr, "settings_env: picture mode %s, %u start-up override(s)", port::settings::Mode30(c) ? "Quality (30)" : "Performance (60)",
                 static_cast< unsigned >(overrides.size()));
    for (const auto& o : overrides) std::fprintf(stderr, " %s=%s", o.key.c_str(), o.rawValue.c_str());
    std::fprintf(stderr, "\n");
    return 0;
}
