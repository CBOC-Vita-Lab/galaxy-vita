// logic23 host test (tools/test_logic_prof.sh): GXV_LOGIC_PROF's bookkeeping in platform/src/port/perf.cpp (the real file), driven the
// way the game drives it, under a fake clock where every timer read costs exactly kRead ns and the "work" costs known amounts. The
// "corrected" rows must then equal the work put in (the reads inside a row and one for its own pair taken off), nested systems too,
// the update / calc phases and the lists must add up, the class rows must be per class (vtable) and named by the model, and the
// player's share of a system must be the part made inside the player's list.
#include "port/logic_prof.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" void port_anim_lod_set_category(int type);
extern "C" void port_anim_prof_turn(int drawn);
extern "C" void port_mark_frame_thread(void);

static const int64_t kRead = 300;  // ns a timer read costs
static int64_t sNow = 1000000;
extern "C" int64_t port_host_time_ns(void) {
    sNow += kRead;
    return sNow;
}
static void Work(int64_t ns) { sNow += ns; }
static std::vector< std::string > sLines;
extern "C" void port_log_summary(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sLines.push_back(buf);
    if (getenv("VERBOSE")) printf("%s\n", buf);
}

// objects with distinct vtables
struct Obj {
    void* vt;
    const char* name;
};
static void* const kVtA = reinterpret_cast< void* >(0x1000);
static void* const kVtB = reinterpret_cast< void* >(0x2000);
static void* const kVtM = reinterpret_cast< void* >(0x3000);

static void RunObj(const Obj& o, int64_t work, const char* model, int sys, int64_t sysWork, int innerSys, int64_t innerWork) {
    port_logic_prof_obj_begin(&o, o.name);
    if (model && port_logic_prof_obj_wants_name()) port_logic_prof_obj_name(model);
    Work(work);
    if (sys >= 0) {
        port_logic_prof_sys_begin(sys);
        Work(sysWork);
        if (innerSys >= 0) {
            port_logic_prof_sys_begin(innerSys);
            port_logic_prof_sys_begin(innerSys);  // (re-entered: only the outermost call is timed)
            Work(innerWork);
            port_logic_prof_sys_end(innerSys);
            port_logic_prof_sys_end(innerSys);
        }
        port_logic_prof_sys_end(sys);
    }
    port_logic_prof_obj_end();
}

static double Field(const std::string& line, const char* key) {
    const size_t p = line.find(key);
    if (p == std::string::npos) return NAN;
    return atof(line.c_str() + p + strlen(key));
}
static const std::string* Find(const char* prefix) {
    for (size_t i = sLines.size(); i-- > 0;)
        if (sLines[i].compare(0, strlen(prefix), prefix) == 0) return &sLines[i];
    return nullptr;
}

int main() {
    setenv("GXV_LOGIC_PROF", "2", 1);
    port_mark_frame_thread();
    Obj a1{kVtA, "objA1"}, a2{kVtA, "objA2"}, b{kVtB, "objB"}, mario{kVtM, "mario-obj"};
    port_anim_prof_turn(0);  // (the flag is taken at a turn's end, as in the game)
    for (int turn = 0; turn < 300; ++turn) {
        port_logic_prof_sys_begin(PORT_LS_PH_UPDATE);
        Work(50000);  // (outside the lists: 50 us)
        port_anim_lod_set_category(0x100 | 0x22);  // MapObj
        RunObj(a1, 10000, "flower", PORT_LS_AREA_FIND, 2000, -1, 0);
        RunObj(a2, 10000, nullptr, PORT_LS_GRAVITY, 3000, PORT_LS_GRAVITY_WALK, 1000);
        RunObj(b, 7000, "rock", PORT_LS_COLL, 4000, -1, 0);
        port_anim_lod_set_category(-1);
        port_anim_lod_set_category(0x100 | 0x25);  // Player
        RunObj(mario, 100000, "mario", PORT_LS_GRAVITY, 5000, PORT_LS_GRAVITY_WALK, 2000);
        port_anim_lod_set_category(-1);
        port_logic_prof_sys_end(PORT_LS_PH_UPDATE);
        port_logic_prof_sys_begin(PORT_LS_PH_CALC);
        port_anim_lod_set_category(5);  // calc MapObj
        RunObj(a1, 8000, nullptr, -1, 0, -1, 0);
        port_anim_lod_set_category(-1);
        Work(20000);
        port_logic_prof_sys_end(PORT_LS_PH_CALC);
        port_anim_prof_turn(1);
    }
    int bad = 0;
    auto expect = [&](const char* what, double got, double want) {
        const bool ok = std::fabs(got - want) <= 0.0015;
        printf("%-58s got %.4f want %.4f %s\n", what, got, want, ok ? "ok" : "BAD");
        bad += !ok;
    };
    const std::string* head = Find("logic-prof: 300 turns");
    const std::string* sys = Find("logic-prof: systems");
    const std::string* up1 = Find("logic-prof: update #1 ");
    const std::string* up2 = Find("logic-prof: update #2 ");
    const std::string* up3 = Find("logic-prof: update #3 ");
    const std::string* calc1 = Find("logic-prof: calc #1 ");
    const std::string* cnt = Find("logic-prof: counts");
    if (!head || !sys || !up1 || !up2 || !up3 || !calc1 || !cnt) {
        printf("missing lines (%zu lines)\n", sLines.size());
        return 1;
    }
    // lists: MapObj 10+2 + 10+3+1 + 7+4 = 37 us, Player 100+5+2 = 107 us -> 0.144 ms; update phase 50 + 144 = 0.194 ms
    expect("update phase ms", Field(*head, "update phase "), 0.194);
    expect("update lists ms", Field(*head, "= lists "), 0.144);
    expect("update outside the lists ms", Field(*head, "+ outside the lists "), 0.050);
    expect("calc phase ms", Field(*head, "calc phase "), 0.028);
    expect("calc-anim lists ms", Field(*head, "= calc-anim lists "), 0.008);
    expect("update #1 (mario) ms", Field(*up1, "ms/turn="), 0.107);
    expect("update #2 (class A: 2 objects) ms", Field(*up2, "ms/turn="), 0.026);
    expect("update #2 calls a turn", Field(*up2, "calls/turn="), 2.0);
    expect("update #3 (class B) ms", Field(*up3, "ms/turn="), 0.011);
    expect("calc #1 (class A) ms", Field(*calc1, "ms/turn="), 0.008);
    expect("gravity system ms (A2 4 us + mario 7 us)", Field(*sys, " gravity "), 0.011);
    expect("gravity-walk ms (1 + 2 us, re-entry not doubled)", Field(*sys, "gravity-walk "), 0.003);
    expect("area-find ms", Field(*sys, "area-find "), 0.002);
    expect("collision ms", Field(*sys, "collision "), 0.004);
    {
        const size_t p = sys->find(" gravity ");
        const std::string g = sys->substr(p);
        expect("gravity in the player's lists ms (mario 5 + 2 us)", Field(g, "; "), 0.007);
    }
    const bool named = up1->find("name=mario ") != std::string::npos && up2->find("name=flower ") != std::string::npos &&
                       up3->find("name=rock ") != std::string::npos && up1->find("cat=Player") != std::string::npos &&
                       up2->find("cat=MapObj") != std::string::npos;
    printf("rows named by model and category: %s\n", named ? "ok" : "BAD");
    bad += !named;
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
