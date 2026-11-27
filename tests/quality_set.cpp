// gfx26: host test of platform/include/port/quality_set.h (the GXV_QUALITY_SET umbrella). Pure logic; a fake environment.
#include "port/quality_set.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>

using namespace port::qualityset;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

struct Env {
    std::map<std::string, std::string> m;
    const char* get(const char* k) const { auto it = m.find(k); return it == m.end() ? nullptr : it->second.c_str(); }
    void put(const char* k, const char* v) { m[k] = v; }
};
static uint32_t maskOf(const Env& e) {
    uint32_t r = 0;
    for (int i = 0; i < kMemberCount; ++i) { const char* v = e.get(kMembers[i].key); if (v && *v) r |= 1u << i; }
    return r;
}
static Result run(Env& e, int level, Mode m, bool proven, uint32_t mask) {
    return Apply(level, m, "test", proven, mask, [&](const char* k) { return e.get(k); }, [&](const char* k, const char* v) { e.put(k, v); });
}

int main() {
    // table sanity
    CHECK(kMemberCount >= 10 && kMemberCount <= 32);
    for (int i = 0; i < kMemberCount; ++i) {
        CHECK(kMembers[i].key && std::strncmp(kMembers[i].key, "GXV_", 4) == 0);
        CHECK(kMembers[i].v1 && kMembers[i].v2 && kMembers[i].what && *kMembers[i].what);
        for (int j = i + 1; j < kMemberCount; ++j) CHECK(std::strcmp(kMembers[i].key, kMembers[j].key) != 0);
        // a level-1 value is a level-2 value too, or level 2 has its own (never "-" at 2 where 1 has one)
        if (!NoChange(kMembers[i].v1)) CHECK(!NoChange(kMembers[i].v2));
        // values parse as integers
        for (const char* v : {kMembers[i].v1, kMembers[i].v2}) if (!NoChange(v)) { char* e = nullptr; (void)std::strtol(v, &e, 10); CHECK(e && !*e); }
    }

    // Resolve
    CHECK(Resolve(nullptr, Mode::Quality).level == (kDefaultAuto ? 2 : 0));
    CHECK(Resolve("", Mode::Performance).level == (kDefaultAuto ? 1 : 0));
    CHECK(Resolve("0", Mode::Quality).level == 0 && Resolve("0", Mode::Quality).fromEnv);
    CHECK(Resolve("1", Mode::Quality).level == 1);
    CHECK(Resolve("2", Mode::Performance).level == 2);
    CHECK(Resolve("auto", Mode::Quality).level == 2);
    CHECK(Resolve("auto", Mode::Locked30).level == 2);
    CHECK(Resolve("auto", Mode::Performance).level == 1);
    CHECK(Resolve("3", Mode::Quality).bad && Resolve("3", Mode::Quality).level == 0);
    CHECK(Resolve("on", Mode::Quality).bad);
    CHECK(Resolve("12", Mode::Quality).bad);
    CHECK(Resolve("-1", Mode::Quality).bad);

    // level 0 touches nothing
    { Env e; Result r = run(e, 0, Mode::Quality, false, 0); CHECK(r.set == 0 && e.m.empty() && std::strstr(r.text, "nothing touched")); }

    // level 2 on an empty environment: every member with a level-2 value is written
    { Env e; Result r = run(e, 2, Mode::Quality, false, 0);
      int want = 0; for (int i = 0; i < kMemberCount; ++i) if (!NoChange(kMembers[i].v2)) ++want;
      CHECK(r.set == want && r.kept == 0 && (int)e.m.size() == want);
      CHECK(std::string(e.get("GXV_AUTOQ_EDGESTEP")) == "0");
      CHECK(std::string(e.get("GXV_EDGE_DEPTH")) == "1");
      CHECK(std::string(e.get("GXV_PLAYER_LODBIAS")) == "-50");
      CHECK(std::strstr(r.text, "GXV_QUALITY_SET=2 (Quality mode"));
      CHECK(std::strstr(r.text, "GXV_EDGE_DEPTH=1*"));        // unproven marked
      CHECK(!std::strstr(r.text, "GXV_EDGE_AA=1*"));          // proven not marked
    }

    // level 1: the Performance subset; EDGESTEP and EDGE_DEPTH are not part of it
    { Env e; Result r = run(e, 1, Mode::Performance, false, 0);
      CHECK(e.get("GXV_AUTOQ_EDGESTEP") == nullptr && e.get("GXV_EDGE_DEPTH") == nullptr && e.get("GXV_AUTOMIP") == nullptr);
      CHECK(std::string(e.get("GXV_HUD_NATIVE")) == "1" && std::string(e.get("GXV_PLAYER_LODBIAS")) == "-25" && std::string(e.get("GXV_PLAYER_DOF")) == "0");
      CHECK(r.set > 0 && r.skipped > 0); }

    // a key env.txt set wins, even set to 0, and the census names it
    { Env e; e.put("GXV_AUTOQ_EDGESTEP", "1"); e.put("GXV_HUD_NATIVE", "0");
      Result r = run(e, 2, Mode::Quality, false, maskOf(e));
      CHECK(std::string(e.get("GXV_AUTOQ_EDGESTEP")) == "1" && std::string(e.get("GXV_HUD_NATIVE")) == "0");
      CHECK(r.kept == 2);
      CHECK(std::strstr(r.text, "kept from env.txt:") && std::strstr(r.text, "GXV_HUD_NATIVE=0(level 2 would set 1)")); }

    // entry.cpp's defaults are overwritten when the snapshot says env.txt did not set them
    { Env e; e.put("GXV_EDGE_AA", "1"); e.put("GXV_AUTOQ_EDGESTEP", "1"); /* defaults, not in the mask */
      Result r = run(e, 2, Mode::Quality, false, 0);
      CHECK(std::string(e.get("GXV_AUTOQ_EDGESTEP")) == "0" && r.kept == 0); }

    // proven only: unproven members are left alone and named
    { Env e; Result r = run(e, 2, Mode::Quality, true, 0);
      CHECK(e.get("GXV_EDGE_DEPTH") == nullptr && e.get("GXV_AUTOMIP") == nullptr && e.get("GXV_HUD_NATIVE") == nullptr);
      CHECK(std::string(e.get("GXV_AUTOQ_EDGESTEP")) == "0" && std::string(e.get("GXV_ANISO")) == "3");
      CHECK(std::strstr(r.text, "left out (unproven): ") && std::strstr(r.text, "GXV_EDGE_DEPTH")); }

    // snapshot: empty text is not the user's; any text is
    { Env e; e.put("GXV_HUD_NATIVE", ""); e.put("GXV_EDGE_DEPTH", "0");
      SnapshotUser([&](const char* k) { return e.get(k); });
      CHECK(Snapshotted());
      uint32_t m = UserMask();
      int ih = -1, id = -1; for (int i = 0; i < kMemberCount; ++i) { if (!std::strcmp(kMembers[i].key, "GXV_HUD_NATIVE")) ih = i; if (!std::strcmp(kMembers[i].key, "GXV_EDGE_DEPTH")) id = i; }
      CHECK(ih >= 0 && id >= 0 && !(m & (1u << ih)) && (m & (1u << id))); }

    // census text never overflows its buffer, even with every member kept
    { Env e; for (int i = 0; i < kMemberCount; ++i) e.put(kMembers[i].key, "123456789");
      Result r = run(e, 2, Mode::Quality, false, 0xFFFFFFFFu);
      CHECK(r.set == 0 && r.kept > 0 && std::strlen(r.text) > 0);
      // fmt1131: every kept member is named in full, nothing cut (a cut ends in "...")
      CHECK(r.text.size() < 3 || r.text.compare(r.text.size() - 3, 3, "...") != 0);
      int named = 0; for (int i = 0; i < kMemberCount; ++i) if (std::strstr(r.text, kMembers[i].key)) ++named;
      CHECK(named >= r.kept); }

    // out-of-range levels clamp
    { Env e; Result r = run(e, 9, Mode::Quality, false, 0); CHECK(r.level == 2); Result q = run(e, -3, Mode::Quality, false, 0); CHECK(q.level == 0); }

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("quality_set: all checks passed (%d members)\n", kMemberCount);
    return 0;
}
