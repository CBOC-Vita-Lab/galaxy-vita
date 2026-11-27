// seedgate31 A0: the profile word (platform/vgx/src/vgx_shadergen.h VGX_TEXT_SWITCHES / ProfileWord) host test.
//   vgx_profile_word test            self test: the word changes when any text switch changes (to any other value, empty = default excepted), does not change when a
//                                    switch outside the list changes, equals the default's word when a switch is set to its default text, every single change gives its own
//                                    word, the list has no duplicate, every listed switch is read somewhere in the generator / front end sources (argv[2], a tree root).
//   vgx_profile_word print           the lines the game logs ("profile: word=..." and "profile-sw: ..."), for the environment of this process;
//                                    tools/test_vgx_profile_word.sh compares them with refill_pack.py's own computation (the tool that gates a seed).
#include "vgx_shadergen.h"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <set>
#include <string>
using namespace vgx;
static std::map<std::string, std::string>* g_env = nullptr;
static const char* Get(const char* n) { auto it = g_env->find(n); return it == g_env->end() ? nullptr : it->second.c_str(); }
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
int main(int argc, char** argv) {
    if (argc >= 2 && !std::strcmp(argv[1], "print")) {
        EmitProfileLines([](const char* l) { std::printf("%s\n", l); });
        return 0;
    }
    std::map<std::string, std::string> env;
    g_env = &env;
    const uint64_t base = ProfileWord(Get);
    // shader37: the optional switches (GXV_FS_ATFAST, GXV_VS_NOBRANCH) do not move the word while off: the all-defaults word is the one of the
    // commit before they existed (2683603a), and any other value of them does move it
    CHECK(base == 0xc0edfb62828480ebull, "off-state word %016llx is not the parent's c0edfb62828480eb", static_cast<unsigned long long>(base));
    for (size_t i = 0; i < kTextSwitchOptCount; ++i) {
        env.clear(); env[kTextSwitchesOpt[i].name] = kTextSwitchesOpt[i].dflt;
        CHECK(ProfileWord(Get) == base, "%s at its default changes the word", kTextSwitchesOpt[i].name);
        env[kTextSwitchesOpt[i].name] = "1";
        CHECK(ProfileWord(Get) != base, "%s=1 does not change the word", kTextSwitchesOpt[i].name);
        env.clear();
    }
    CHECK(base != 0, "zero word");
    CHECK(kTextSwitchCount >= 20, "only %zu switches", kTextSwitchCount);
    std::set<std::string> names;
    for (size_t i = 0; i < kTextSwitchCount; ++i) CHECK(names.insert(kTextSwitches[i].name).second, "duplicate %s", kTextSwitches[i].name);
    // 1. each listed switch: any other value changes the word; all those words differ from each other
    std::set<uint64_t> seen{base};
    for (size_t i = 0; i < kTextSwitchCount; ++i) {
        const char* name = kTextSwitches[i].name;
        const std::string dflt = kTextSwitches[i].dflt;
        for (const char* v : {"0", "1", "2", "47", "x"}) {
            env.clear();
            env[name] = v;
            const uint64_t w = ProfileWord(Get);
            if (dflt == v) CHECK(w == base, "%s=%s is the default: the word must not change", name, v);
            else { CHECK(w != base, "%s=%s did not change the word", name, v); if (std::string(v) == "47") CHECK(seen.insert(w).second, "%s=47 has the word of another switch", name); }
        }
        env.clear();
        env[name] = "";                       // empty = unset = default
        CHECK(ProfileWord(Get) == base, "%s empty must read as unset", name);
        env[name] = dflt;                      // the default spelled out = the same profile
        CHECK(ProfileWord(Get) == base, "%s=default must equal unset", name);
    }
    // 2. a switch NOT in the list never changes the word (non-text knobs: queue, logging, split, culling ...)
    for (const char* other : {"GXV_SPLIT", "GXV_STAGEWARM_LIVE_MS", "GXV_PACK_MISSLOG", "GXV_SHADER_CACHE_TAG", "PETARI_BOOT", "GXV_VSOUT_LOG", "GXV_FSX", "GXV_VSOUTX"}) {
        env.clear();
        env[other] = "1";
        CHECK(ProfileWord(Get) == base, "%s is not a text switch but changed the word", other);
    }
    // 3. the release's own difference: VSOUT 1 against 0 must be two words; two switches changed together differ from each one alone
    env.clear(); env["GXV_VSOUT"] = "1"; const uint64_t vs1 = ProfileWord(Get);
    env["GXV_TEXVEC2"] = "1"; const uint64_t both = ProfileWord(Get);
    env.clear(); env["GXV_TEXVEC2"] = "1"; const uint64_t tv1 = ProfileWord(Get);
    CHECK(vs1 != base && both != vs1 && both != tv1 && vs1 != tv1, "pair words");
    // 4. the generator version is part of the word: the text the word hashes ends with it
    env.clear();
    std::string lines;
    EmitProfileLines([&](const char* l) { lines += l; lines += '\n'; }, Get);
    char want[64];
    std::snprintf(want, sizeof want, "profile: word=%016llx gen=%u", static_cast<unsigned long long>(base), static_cast<unsigned>(kShaderGenVersion));
    CHECK(lines.compare(0, std::strlen(want), want) == 0, "first line %s", lines.c_str());
    size_t seenSw = 0;
    for (size_t i = 0; i < kTextSwitchCount; ++i) if (lines.find(std::string(" ") + kTextSwitches[i].name + "=") != std::string::npos) ++seenSw;
    CHECK(seenSw == kTextSwitchCount, "the log lines name %zu of %zu switches", seenSw, kTextSwitchCount);
    size_t longest = 0, pos = 0;
    while (pos < lines.size()) { size_t e = lines.find('\n', pos); longest = std::max(longest, e - pos); pos = e + 1; }
    CHECK(longest < 200, "a log line of %zu characters", longest);
    // 5. (argv[2] = platform/vgx/src) the list and the generator agree: every "GXV_..." string literal of vgx_shadergen.cpp is a listed switch (or a known non-text read),
    //    and every listed switch is spelled out in vgx_shadergen.cpp, vgx_state.cpp or vgx_draw.cpp (so the list cannot hold a dead name, nor the generator a new switch the list lacks)
    if (argc >= 3) {
        auto slurp = [&](const char* f) { std::ifstream in(std::string(argv[2]) + "/" + f); std::stringstream b; b << in.rdbuf(); return b.str(); };
        const std::string gen = slurp("vgx_shadergen.cpp"), all = gen + slurp("vgx_state.cpp") + slurp("vgx_draw.cpp");
        CHECK(!gen.empty(), "cannot read %s/vgx_shadergen.cpp", argv[2]);
        const std::set<std::string> nonText = {"GXV_DBG_LIT", "GXV_AUTOQ_TARGET", "GXV_FPS_LIMIT"}; // (a diagnostic and the two that GXV_FS_HALF_MINOR=perf reads)
        for (size_t i = 0; i < kTextSwitchOptCount; ++i) names.insert(kTextSwitchesOpt[i].name); // (the optional ones are listed too)
        for (size_t p = gen.find("\"GXV_"); p != std::string::npos; p = gen.find("\"GXV_", p + 1)) {
            const size_t e = gen.find('"', p + 1);
            if (e == std::string::npos) break;
            const std::string n = gen.substr(p + 1, e - p - 1);
            if (n.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) continue;
            CHECK(names.count(n) || nonText.count(n), "%s is read by vgx_shadergen.cpp but is not in VGX_TEXT_SWITCHES", n.c_str());
        }
        for (const std::string& n : names) CHECK(all.find("\"" + n + "\"") != std::string::npos, "%s is listed but no source reads it", n.c_str());
    }
    if (g_fail) { std::printf("FAILED (%d)\n", g_fail); return 1; }
    std::printf("ok: %zu text switches, word %016llx, every one changes it, none outside the list does\n", kTextSwitchCount, static_cast<unsigned long long>(base));
    return 0;
}
