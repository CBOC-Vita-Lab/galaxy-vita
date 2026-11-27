// pace33 PATCH8: the Performance mode's defaults on a card with only env-user.txt. The real per-mode table (port/mode_keys.h) and the real umbrella defaults (port/perf_defaults.h)
// over a map that stands for the environment: env.txt's keys, then entry.cpp's setenv(.., 0) defaults of the keys concerned, then the mode's apply.
//   g++ -std=c++17 -O1 -Iplatform/include tests/pace33_defaults_test.cpp -o /tmp/pace33/dt && /tmp/pace33/dt
#include "port/mode_keys.h"
#include "port/perf_defaults.h"
#include <cstdio>
#include <map>
#include <string>
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using Env = std::map<std::string, std::string>;
static Env g_env;
static const char* Get(const char* k) { auto it = g_env.find(k); return it == g_env.end() ? nullptr : it->second.c_str(); }
static void Default(const char* k, const char* v) { if (!g_env.count(k)) g_env[k] = v; }
// what entry.cpp does between env.txt and the mode: the snapshots, the umbrella fan-out at the environment's level, then the code defaults
static void Startup(const Env& envTxt) {
    g_env = envTxt;
    port::modekeys::Snapshotted() = false; port::modekeys::UserMask() = 0;
    port::modekeys::SnapshotUser(Get);
    port::perfdefaults::SnapshotUser(Get);
    const int lv = port::cpulod::ClampLevel(Get("GXV_CPULOD") ? std::atol(Get("GXV_CPULOD")) : 0);
    if (lv > 0)
        for (int i = 0; i < port::cpulod::kMemberCount; ++i) {
            const port::cpulod::Member& m = port::cpulod::kMembers[i];
            if (!m.existing || Get(m.name)) continue;
            char b[32]; std::snprintf(b, sizeof b, "%g", static_cast<double>(m.lv[lv])); g_env[m.name] = b;
        }
    for (int i = 0; i < port::cpulod::kMemberCount; ++i) if (port::cpulod::kMembers[i].existing) Default(port::cpulod::kMembers[i].name, "0");   // (each member's own default: off)
    for (const char* k : {"GXV_HRW", "GXV_DRAIN33", "GXV_LATENCY24", "GXV_CPULOD", "GXV_ACROSS", "GXV_STAGE_PRESETS", "GXV_UPSCALE", "GXV_HEADROOM_PCT", "GXV_FE_TWOCORE"}) Default(k, "0");
    Default("GXV_SKIP_MAXSTEP", "3"); Default("GXV_PRESENT_SYNC", "2"); Default("GXV_DRAWDONE_DEFER", "2");
}
static std::string Mode(bool quality, std::string* umbrella = nullptr) {
    const port::modekeys::Result r = port::modekeys::Apply(quality, Get, [](const char* k, const char* v) { g_env[k] = v; });
    if (!quality) { char t[256]; port::perfdefaults::ApplyPerformance([](const char* k, const char* v) { g_env[k] = v; }, t, sizeof t); if (umbrella) *umbrella = t; }
    return r.text;
}
static std::string V(const char* k) { const char* v = Get(k); return v ? v : "(unset)"; }
int main() {
    const Env user = {{"GXV_CHARAA_AUTO", "0"}, {"GXV_SHADERPACK", "1"}, {"GXV_DLWELD", "0"}};
    struct Want { const char* key; const char* perf; } want[] = {
        {"GXV_HRW", "5"}, {"GXV_HRW_WARP_DEPTH", "4"}, {"GXV_HRW_SETTLE", "100"}, {"GXV_HRW_SETTLE_RESUME", "60"}, {"GXV_HRW_WARP_FRESH", "1"}, {"GXV_DRAIN33", "1"}, {"GXV_LATENCY24", "1"}, {"GXV_LATENCY24_WINDOW", "45"},
        {"GXV_FE_GOV", "3"}, {"GXV_GAME_GOV", "3"}, {"GXV_STAGE_PRESETS", "1"}, {"GXV_PAR_DL_VIEW", "0"}, {"GXV_UPSCALE", "1"}, {"GXV_UPSCALE_LITE", "1"}, {"GXV_SKIP_MAXSTEP", "2"},
        {"GXV_CPULOD", "2"}, {"GXV_ACROSS", "1"}, {"GXV_FARLOGIC", "2"}};
    // 1. Performance with env-user.txt alone = the candidate
    Startup(user);
    std::string umb;
    const std::string census = Mode(false, &umb);
    std::printf("Performance, env-user.txt only:\n  %s\n  %s\n", census.c_str(), umb.c_str());
    for (const Want& w : want) { if (V(w.key) != w.perf) std::printf("  %s = %s, wanted %s\n", w.key, V(w.key).c_str(), w.perf); CHECK(V(w.key) == w.perf); }
    const Env perfDefault = g_env;
    // ... and the umbrella's members are exactly what GXV_CPULOD=2 GXV_ACROSS=1 in env.txt give
    Env cand = user; cand["GXV_CPULOD"] = "2"; cand["GXV_ACROSS"] = "1";
    Startup(cand);
    Mode(false);
    int differ = 0;
    for (int i = 0; i < port::cpulod::kMemberCount; ++i) {
        const char* n = port::cpulod::kMembers[i].name;
        if (port::cpulod::kMembers[i].existing && V(n) != (perfDefault.count(n) ? perfDefault.at(n) : "(unset)")) { ++differ; std::printf("  member %s: default %s, candidate env %s\n", n, perfDefault.count(n) ? perfDefault.at(n).c_str() : "(unset)", V(n).c_str()); }
    }
    std::printf("  cpulod members that differ from the candidate env's fan-out: %d\n", differ);
    CHECK(differ == 0);
    // 2. Quality with env-user.txt alone: nothing of this patch
    Startup(user);
    const Env before = g_env;
    Mode(true);
    int changed = 0;
    for (const Want& w : want) if (std::string(w.key) != "GXV_SKIP_MAXSTEP" && std::string(w.key) != "GXV_STAGE_PRESETS" && std::string(w.key) != "GXV_UPSCALE" && std::string(w.key) != "GXV_UPSCALE_LITE" && std::string(w.key).rfind("GXV_HOTPROF_", 0) != 0 && std::string(w.key) != "GXV_FE_STATEFAST" && std::string(w.key) != "GXV_SHAPECULL" && std::string(w.key) != "GXV_SHADOW_STENCIL" && V(w.key) != (before.count(w.key) ? before.at(w.key) : "(unset)")) ++changed;   // (c172: Quality has its own stage table value: 1, it starts the Quality lock ladder)
    CHECK(V("GXV_STAGE_PRESETS") == "1" && V("GXV_UPSCALE") == "1" && V("GXV_UPSCALE_LITE") == "1" && V("GXV_Q30_MID") == "1");   // (c173: the 896x512 step and its upscaler)
    std::printf("Quality, env-user.txt only: %d of this patch's keys changed (must be 0); GXV_HRW=%s GXV_CPULOD=%s GXV_DRAIN33=%s\n", changed, V("GXV_HRW").c_str(), V("GXV_CPULOD").c_str(), V("GXV_DRAIN33").c_str());
    CHECK(changed == 0 && V("GXV_HRW") == "0" && V("GXV_CPULOD") == "0");
    // 3. every key is switchable back by env.txt
    Env back = user;
    for (const char* k : {"GXV_HRW", "GXV_DRAIN33", "GXV_LATENCY24", "GXV_HRW_WARP_FRESH", "GXV_CPULOD", "GXV_ACROSS", "GXV_STAGE_PRESETS", "GXV_FE_GOV", "GXV_GAME_GOV", "GXV_UPSCALE", "GXV_UPSCALE_LITE"}) back[k] = "0";
    back["GXV_HRW_WARP_DEPTH"] = "0"; back["GXV_HRW_SETTLE"] = "90"; back["GXV_LATENCY24_WINDOW"] = "90"; back["GXV_PAR_DL_VIEW"] = "1";
    Startup(back);
    Mode(false, &umb);
    int kept = 0, n = 0;
    for (const auto& kv : back) { ++n; if (V(kv.first.c_str()) == kv.second) ++kept; }
    std::printf("Performance with every key written back in env.txt: %d of %d kept; %s\n", kept, n, umb.c_str());
    CHECK(kept == n);
    for (int i = 0; i < port::cpulod::kMemberCount; ++i) if (port::cpulod::kMembers[i].existing) CHECK(V(port::cpulod::kMembers[i].name) == "0");   // GXV_CPULOD=0: no member written
    // 4. one member written by the user survives the umbrella default
    Env one = user; one[port::cpulod::kMembers[0].name] = "0";
    for (int i = 0; i < port::cpulod::kMemberCount; ++i) if (port::cpulod::kMembers[i].existing) { one.erase(port::cpulod::kMembers[0].name); one[port::cpulod::kMembers[i].name] = "0"; Startup(one); Mode(false); CHECK(V(port::cpulod::kMembers[i].name) == "0"); break; }
    CHECK(port::modekeys::kRowCount <= 128);   // (g30 Wave 3: user mask widened to 128 bits)
    std::printf("mode table rows: %d of 128\n", port::modekeys::kRowCount);
    std::printf(g_fail ? "pace33_defaults_test: FAILED (%d)\n" : "pace33_defaults_test: ALL PASSED\n", g_fail);
    return g_fail != 0;
}
