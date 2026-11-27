// deadfix27: port/mode_keys.h: env.txt > mode table > default; "-" leaves a key alone; the census text names written and kept keys.
#include "port/mode_keys.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
using namespace port::modekeys;
int main() {
    std::map<std::string, std::string> env;
    auto get = [&](const char* k) -> const char* { auto i = env.find(k); return i == env.end() ? nullptr : i->second.c_str(); };
    auto put = [&](const char* k, const char* v) { env[k] = v; };
    // entry.cpp's default "0" is in the environment but env.txt did not set it: the snapshot was taken before it
    int perfSet = 0, qualSet = 0;
    for (int i = 0; i < kRowCount; ++i) {
        if (!NoChange(kRows[i].perf)) ++perfSet;
        if (!NoChange(kRows[i].quality)) ++qualSet;
    }
    SnapshotUser(get);
    env["GXV_COLSHADOW_CLIP"] = "0";
    Result r = Apply(false, get, put);
    assert(r.set == perfSet && env["GXV_SHADOW_STENCIL"] == "1" && env["GXV_STALL_FORGIVE"] == "80" && env["GXV_INDCAP_SKIP"] == "1" && env["GXV_BLOOM_FOLD"] == "3" && env["GXV_WATER_BLOOMFOLD"] == "2" && env["GXV_COLSHADOW_CLIP"] == "1" && env["GXV_SKIP_MAXSTEP"] == "2" && env["GXV_DIRECT_DISPLAY"] == "1" && env["GXV_FIRSTCLEAR"] == "1" && env["GXV_TAILDEPTH"] == "1" && env["GXV_HOIST_GRID"] == "1" && env["GXV_VGL_VALIDREGION"] == "1" && env["GXV_LIGHTVOL"] == "3" && env["GXV_FLUSH_OFFSCREEN"] == "1" && env["GXV_PLANT_DRAWCULL"] == "1" && env["GXV_CLEARQ"] == "1" && !env.count("GXV_COLOR_LOADSKIP") && env["GXV_SEAM72"] == "1" && env["GXV_BE_MERGE"] == "1" && env["GXV_INDCAP_VIS"] == "1" && !env.count("GXV_AUTOMIP") && !env.count("GXV_EDGE_DEPTH") && !env.count("GXV_PLAYER_LODBIAS") && std::strstr(r.text, "GXV_COLSHADOW_CLIP=1"));
    assert(env["GXV_IND_FLAT"] == "1");
    // fix31 (owner 17 Nov): C182's Performance warp is off until the console A/B: neither mode writes these keys, so the code defaults (0) apply, as in c181
    for (const char* k : {"GXV_HRW_STAGE_AUTO_RETRY", "GXV_HRW41_DEPTH", "GXV_HRW41_BOXKEY", "GXV_HRW41_BOX_ROUND", "GXV_HRW41_BUDGET"}) {
        assert(!env.count(k));
        for (int i = 0; i < kRowCount; ++i) assert(std::strcmp(kRows[i].key, k) != 0);
    }
    // Quality: exact scene/VRAM/CPU wins + Quality crispness upgrades; Performance-only cuts stay unset
    env.clear(); env["GXV_COLSHADOW_CLIP"] = "0";
    Snapshotted() = false;
    r = Apply(true, get, put);
    assert(r.set == qualSet - 1 && env["GXV_FIRSTCLEAR"] == "1" && env["GXV_TAILDEPTH"] == "1" && env["GXV_HOIST_GRID"] == "1" && env["GXV_VGL_VALIDREGION"] == "1" && env["GXV_EFBSWAP2_BACKOFF"] == "1" && env["GXV_CORNERPASS_ALT"] == "1" && env["GXV_LIGHTVOL"] == "3" && env["GXV_FLUSH_OFFSCREEN"] == "1" && env["GXV_PLANT_DRAWCULL"] == "1" && env["GXV_CLEARQ"] == "1" && !env.count("GXV_COLOR_LOADSKIP") && env["GXV_SEAM72"] == "1" && env["GXV_AUTOMIP"] == "1" && !env.count("GXV_EDGE_DEPTH") && env["GXV_PLAYER_LODBIAS"] == "-50" && env["GXV_PLAYER_DOF"] == "0" && env["GXV_RUNG_LODBIAS_EXTRA_AF"] == "2" && !env.count("GXV_DIRECT_DISPLAY") && !env.count("GXV_HRW") && env["GXV_SHADOW_STENCIL"] == "1" && env["GXV_HOTPROF_TEVCOL"] == "1" && env["GXV_SHAPECULL"] == "1" && env["GXV_FE_STATEFAST"] == "1" && env["GXV_STALL_FORGIVE"] == "80" && env["GXV_INDCAP_SKIP"] == "1" && env["GXV_STAGE_PRESETS"] == "1" && env["GXV_Q30_LOCK_STEPS"] == "28671" && env["GXV_Q30_MID"] == "1" && env["GXV_UPSCALE"] == "1" && env["GXV_UPSCALE_LITE"] == "1" && !env.count("GXV_BLOOM_FOLD") && !env.count("GXV_WATER_BLOOMFOLD") && env["GXV_COLSHADOW_CLIP"] == "0" && r.none == kRowCount - qualSet && env["GXV_POSE_PHASE"] == "1" && env["GXV_PB_DEMO"] == "2" && env["GXV_PB_MINPX_SHADOW"] == "3" && !env.count("GXV_HRW_MARIOTIME") && env["GXV_CPULOD_MEMO"] == "1" && env["GXV_PTCL_PRE"] == "1" && env["GXV_BOWL_POSE"] == "1" && !env.count("GXV_PERF_GPU896"));
    // High-index (>64) Mask128 override check: env.txt wins for keys beyond bit 63
    {
        env.clear(); Snapshotted() = false;
        env["GXV_AUTOMIP"] = "0";
        env["GXV_SEAM72"] = "0";
        SnapshotUser(get);
        assert(UserHas("GXV_AUTOMIP", get) && UserHas("GXV_SEAM72", get) && !UserHas("GXV_PLAYER_LODBIAS", get));
        r = Apply(true, get, put);
        assert(env["GXV_AUTOMIP"] == "0" && env["GXV_SEAM72"] == "0" && env["GXV_PLAYER_LODBIAS"] == "-50" && r.kept == 2);
        Snapshotted() = false;
    }
    // defaults32: the four 19-Nov defaults in Performance, and env.txt wins over each (even 0 / a different value); the ones that stay off have no row
    {
        env.clear(); env["GXV_COLSHADOW_CLIP"] = "0"; Snapshotted() = false;
        SnapshotUser(get);
        r = Apply(false, get, put);
        assert(env["GXV_POSE_PHASE"] == "1" && env["GXV_HRW_MARIOTIME"] == "2" && env["GXV_PB_DEMO"] == "2" && env["GXV_PB_MINPX_SHADOW"] == "3" && env["GXV_PERF_GPU896"] == "0");   // (gpu896: Performance on, Quality no row)
        for (const char* k : {"GXV_PB_MINPX_DECOR", "GXV_PB_MINPX", "GXV_STATIC_INST", "GXV_IDLE25", "GXV_REPLAY25", "GXV_RT_INST", "GXV_STARBATCH", "GXV_POSE_ONDRAW", "GXV_HRW_HALO", "GXV_HRW36_PLANE_RAMP"}) {
            assert(!env.count(k));
            for (int i = 0; i < kRowCount; ++i) assert(std::strcmp(kRows[i].key, k) != 0);
        }
        for (int q = 0; q < 2; ++q)
            for (const char* k : {"GXV_POSE_PHASE", "GXV_HRW_MARIOTIME", "GXV_PB_DEMO", "GXV_PB_MINPX_SHADOW", "GXV_PERF_GPU896", "GXV_LIGHTVOL", "GXV_FLUSH_OFFSCREEN", "GXV_PLANT_DRAWCULL"}) {
                if (q && (!std::strcmp(k, "GXV_HRW_MARIOTIME") || !std::strcmp(k, "GXV_PERF_GPU896"))) continue;   // no Quality value
                env.clear(); Snapshotted() = false; env[k] = "0";            // env.txt line "KEY=0"
                SnapshotUser(get);
                r = Apply(q != 0, get, put);
                assert(env[k] == "0" && r.kept == 1 && std::strstr(r.text, "kept from env.txt") && std::strstr(r.text, (std::string(k) + "=0(the mode would set").c_str()));
            }
        Snapshotted() = false;
    }
    // env.txt set it (even to 0): kept
    env.clear(); env["GXV_COLSHADOW_CLIP"] = "0";
    SnapshotUser(get);
    r = Apply(false, get, put);
    assert(r.set == perfSet - 1 && r.kept == 1 && env["GXV_COLSHADOW_CLIP"] == "0" && std::strstr(r.text, "kept from env.txt"));
    // no snapshot: whatever is present counts as the user's
    Snapshotted() = false;
    env.clear();
    r = Apply(false, get, put);
    assert(r.set == perfSet && !env.count("GXV_INDPREV"));   // INDPREV stays a candidate until a console run proves it
    env["GXV_COLSHADOW_CLIP"] = "0";
    r = Apply(false, get, put);
    assert(r.kept == perfSet);
    // fmt1131: the census names every row, kept or not, with nothing cut: a cut would end in "..." (the lists were 400 / 300 bytes and dropped the rest silently)
    for (int q = 0; q < 2; ++q) {
        Snapshotted() = false;
        auto longGet = [](const char*) -> const char* { return "0.123456789"; };
        r = Apply(q != 0, longGet, [](const char*, const char*) {});
        assert(r.text.size() > 3 && r.text.compare(r.text.size() - 3, 3, "...") != 0);
        int listed = 0;
        for (int i = 0; i < kRowCount; ++i) {
            const char* v = q ? kRows[i].quality : kRows[i].perf;
            if (!NoChange(v)) { ++listed; assert(std::strstr(r.text, kRows[i].key)); }
        }
        assert(listed == r.kept);
    }
    std::puts("mode_keys_test: ok");
    return 0;
}
