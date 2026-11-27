// game24 host test (tools/test_logic_prof3.sh): GXV_LOGIC_PROF=3, the sampled profile, in platform/src/port/perf.cpp (the real file), driven the
// way the game drives it, under a fake clock where every timer read costs exactly kRead ns. Over several 300-turn windows with one turn in four
// "deep" (picked at random): the coarse rows (every turn: the category lists, update's parts, the movement-list function's parts) must equal the
// work put in exactly, whether or not the turn was a deep one; the class rows come from the deep turns only and are scaled to the real cost of
// their category's list (the unsampled turns' whole-list time over the sum of the objects' times), so a class with 30 % of a list's objects'
// work shows 30 % of the list's real time. The part that is not in any named row must show as "unexplained".
#include "port/logic_prof.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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
static void WorkUs(int64_t us) { sNow += us * 1000; }
static std::vector< std::string > sLines;
extern "C" void port_log_summary(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sLines.push_back(buf);
    if (getenv("VERBOSE")) printf("%s\n", buf);
}
struct Obj {
    void* vt;
    const char* name;
};
static void* const kVtX = reinterpret_cast< void* >(0x1000);
static void* const kVtY = reinterpret_cast< void* >(0x2000);
static void* const kVtM = reinterpret_cast< void* >(0x3000);
// NameObjCategoryList::execute's per-object hook, as written there
static void ObjRun(const Obj& o, int64_t us, const char* model) {
    if (::port::LogicProfOn()) {
        port_logic_prof_obj_begin(&o, o.name);
        if (model && port_logic_prof_obj_wants_name()) port_logic_prof_obj_name(model);
        WorkUs(us);
        port_logic_prof_obj_end();
    } else {
        WorkUs(us);
    }
}
// (update25) one actor's movement with two of LiveActor::movement's phases timed (only on a deep turn, as LiveActor::portMovementPhased)
static void ObjRunPhased(const Obj& o, const char* model) {
    if (::port::LogicProfOn()) {
        port_logic_prof_obj_begin(&o, o.name);
        if (model && port_logic_prof_obj_wants_name()) port_logic_prof_obj_name(model);
        PORT_LOGIC_PHASE(PORT_LS_MV_NERVE, WorkUs(600));
        PORT_LOGIC_PHASE(PORT_LS_MV_BINDER, WorkUs(150));
        WorkUs(250);
        port_logic_prof_obj_end();
    } else {
        WorkUs(1000);
    }
}
// (update25) one model of the drawn turn's flush, as port_dl_flush_stale / port_anim_flush_stale write it (kind 0 dl, 1 anim)
static void FlushOne(int kind, const Obj& o, int64_t us, int outcome) {
    if (::port::LogicProfOn(2)) {
        port_logic_prof_flush_begin(kind, &o, o.name);
        WorkUs(us);
        port_logic_prof_flush_end(outcome);
    } else {
        WorkUs(us);
    }
}
// CategoryList::execute's hook, as written in SceneFunction.cpp
static void Cat(int raw, const std::function< void() >& body) {
    port_anim_lod_set_category(raw);
    ::port::LogicCatScope scope(raw);
    body();
    port_anim_lod_set_category(-1);
}
static double Field(const std::string& line, const char* key) {
    const size_t p = line.find(key);
    return p == std::string::npos ? NAN : atof(line.c_str() + p + strlen(key));
}
static const std::string* Find(const char* prefix, int nth = 0) {  // the nth last
    for (size_t i = sLines.size(); i-- > 0;) {
        if (sLines[i].compare(0, strlen(prefix), prefix) == 0 && nth-- == 0) return &sLines[i];
    }
    return nullptr;
}
static Obj sX1{kVtX, "xobj"}, sX2{kVtX, "xobj"}, sX3{kVtX, "xobj"}, sY{kVtY, "yobj"}, sM{kVtM, "mario-obj"};

static Obj sF1{kVtX, "plantA"}, sF2{kVtX, "plantA"}, sF3{kVtY, "plantB"};
static void OneTurn(int turn) {
    if (turn & 1) {  // a drawn turn: its flushes come first (before update, outside the phases)
        FlushOne(0, sF1, 7, 0);
        FlushOne(0, sF2, 7, 0);
        FlushOne(0, sF3, 5, 1);  // (one model left out)
        FlushOne(1, sF1, 17, 0);
    }
    {
        PORT_LOGIC_COARSE(PORT_LS_PH_UPDATE);
        { PORT_LOGIC_COARSE(PORT_LS_U_RESET); WorkUs(40); }
        { PORT_LOGIC_COARSE(PORT_LS_U_SCENECHG); WorkUs(30); }
        {
            PORT_LOGIC_COARSE(PORT_LS_U_OBJHOLDER);
            { PORT_LOGIC_COARSE(PORT_LS_U_OH_RHYTHM); WorkUs(20); }
            { PORT_LOGIC_COARSE(PORT_LS_U_OH_WPAD); WorkUs(30); }
            { PORT_LOGIC_COARSE(PORT_LS_U_OH_ASYNC); WorkUs(10); }
            { PORT_LOGIC_COARSE(PORT_LS_U_OH_STARPTR); WorkUs(40); }
        }
        { PORT_LOGIC_COARSE(PORT_LS_U_HOME); WorkUs(60); }
        WorkUs(8);  // (2737 + 8: in no row: "unexplained")
        {
            PORT_LOGIC_COARSE(PORT_LS_U_NERVE);
            { PORT_LOGIC_COARSE(PORT_LS_U_AUDIO); WorkUs(70); }
            { PORT_LOGIC_COARSE(PORT_LS_U_SEQDIR); WorkUs(80); }
            { PORT_LOGIC_COARSE(PORT_LS_U_WIPE); WorkUs(25); }
            {
                PORT_LOGIC_COARSE(PORT_LS_U_SCENE);
                WorkUs(60);
                {
                    PORT_LOGIC_COARSE(PORT_LS_U_MOVELISTS);
                    { PORT_LOGIC_COARSE(PORT_LS_U_ML_CTRL); WorkUs(15); }
                    { PORT_LOGIC_COARSE(PORT_LS_U_ML_CAMMTX); WorkUs(25); }
                    { PORT_LOGIC_COARSE(PORT_LS_U_ML_REQ); WorkUs(20); }
                    { PORT_LOGIC_COARSE(PORT_LS_U_ML_REQ); WorkUs(15); }
                    WorkUs(12);
                    Cat(0x100 | 0x22, [] {  // MapObj: 40 us of its own, three X objects of 200 us, one Y of 1000 us
                        WorkUs(40);
                        ObjRun(sX1, 200, "flower");
                        ObjRun(sX2, 200, nullptr);
                        ObjRun(sX3, 200, nullptr);
                        ObjRunPhased(sY, "rock");  // 1000 us: nerve 600 + binder 150 + 250 in no phase
                    });
                    Cat(0x100 | 0x25, [] { ObjRun(sM, 500, "mario"); });  // Player
                }
            }
            WorkUs(45);
        }
    }
    {
        PORT_LOGIC_COARSE(PORT_LS_PH_CALC);
        { PORT_LOGIC_COARSE(PORT_LS_C_LATE); WorkUs(20); }
        Cat(5, [] { WorkUs(10); ObjRun(sX1, 150, "flower"); });  // calc-anim MapObj: 160 us
        Cat(0x13, [] { WorkUs(300); });                         // AnimParticle: 300 us (no objects)
        Cat(0x100 | 0x2D, [] { WorkUs(100); });                 // the shadow controllers
        { PORT_LOGIC_COARSE(PORT_LS_C_PTCLFIN); WorkUs(30); }
        { PORT_LOGIC_COARSE(PORT_LS_C_ANIMDISP); WorkUs(50); }
        WorkUs(15);
    }
    port_anim_prof_turn(turn & 1);
}

int main() {
    setenv("GXV_LOGIC_PROF", "3", 1);
    setenv("GXV_LOGIC_PROF_SAMPLE", "4", 1);
    port_mark_frame_thread();
    port_anim_prof_turn(0);
    int deepTurns = 0, turns = 0;
    for (int w = 0; w < 4; ++w) {
        for (int t = 0; t < 300; ++t) {
            deepTurns += port_logic_prof_flag != 0;
            ++turns;
            OneTurn(t);
        }
    }
    int bad = 0;
    auto expect = [&](const char* what, double got, double want, double tol = 0.0015) {
        const bool ok = std::fabs(got - want) <= tol;
        printf("%-62s got %.4f want %.4f %s\n", what, got, want, ok ? "ok" : "BAD");
        bad += !ok;
    };
    printf("deep turns %d of %d\n", deepTurns, turns);
    bad += !(deepTurns > turns / 8 && deepTurns < turns / 2);
    const std::string* head = Find("logic-prof3: ");
    // the lines of the last window: head is the last "logic-prof3:" line (counts); find by content instead
    const std::string* sum = nullptr; const std::string* unexp = nullptr; const std::string* catMove = nullptr; const std::string* catCalc = nullptr;
    for (size_t i = sLines.size(); i-- > 0;) {
        const std::string& l = sLines[i];
        if (!sum && l.find("update phase ") != std::string::npos && l.compare(0, 12, "logic-prof3:") == 0 && l.find("sampled, 1 in") != std::string::npos) sum = &l;
        if (!unexp && l.find("update unexplained") != std::string::npos) unexp = &l;
        if (!catMove && l.find("movement categories a turn, every turn") != std::string::npos) catMove = &l;
        if (!catCalc && l.find("calc-anim categories a turn, every turn") != std::string::npos) catCalc = &l;
    }
    (void)head;
    if (!sum || !unexp || !catMove || !catCalc) {
        printf("missing lines (%zu lines)\n", sLines.size());
        return 1;
    }
    expect("update phase ms (2745 us)", Field(*sum, "update phase "), 2.745);
    expect("reset process", Field(*sum, "reset process "), 0.040);
    expect("scene change check", Field(*sum, "scene change check "), 0.030);
    expect("ObjHolder::update", Field(*sum, "ObjHolder::update "), 0.100);
    expect("audio rhythm", Field(*sum, "audio rhythm "), 0.020);
    expect("pads", Field(*sum, "pads "), 0.030);
    expect("async", Field(*sum, "async "), 0.010);
    expect("star pointer", Field(*sum, "star pointer "), 0.040);
    expect("home/error/dimming", Field(*sum, "home/error/dimming "), 0.060);
    expect("updateNerve", Field(*sum, "updateNerve "), 2.507);
    expect("audio system", Field(*sum, "audio system "), 0.070);
    expect("sequence director", Field(*sum, "sequence director "), 0.080);
    expect("system wipe", Field(*sum, "system wipe "), 0.025);
    expect("scene update", Field(*sum, "scene update "), 2.287);
    expect("movement lists", Field(*sum, "(= movement lists "), 2.227);
    expect("the rest of the scene's update", Field(*sum, "the rest of the scene's update "), 0.060);
    expect("nerve rest", Field(*sum, "+ rest "), 0.045);
    expect("category lists (map objects 1640 + player 500)", Field(*sum, "= category lists "), 2.140);
    expect("scene controller", Field(*sum, "scene controller "), 0.015);
    expect("requirements (20 + 15, two scopes of one id)", Field(*sum, "requirements "), 0.035);
    expect("camera matrix", Field(*sum, "camera matrix "), 0.025);
    expect("movement lists rest", Field(*sum, "camera matrix 0.025 + rest "), 0.012);
    expect("calc phase ms (675 us)", Field(*sum, "calc phase "), 0.675);
    expect("calc-anim lists (160 + particles 300 - particles)", Field(*sum, "= calc-anim lists "), 0.160);
    expect("shadows", Field(*sum, "shadows "), 0.100);
    expect("particles list", Field(*sum, "particles "), 0.300);
    expect("par-late start", Field(*sum, "par-late start "), 0.020);
    expect("particle join", Field(*sum, "particle join "), 0.030);
    expect("par-anim dispatch", Field(*sum, "par-anim dispatch "), 0.050);
    expect("calc rest", Field(*sum, "call overhead) "), 0.015);
    expect("update unexplained (8 us)", Field(*unexp, "minus the parts above) "), 0.008);
    expect("sampling inflation, movement (corrected: none)", Field(*unexp, "inflation x"), 1.00, 0.01);
    expect("movement category MapObj ms, every turn", Field(*catMove, "MapObj "), 1.640);
    expect("movement category Player ms, every turn", Field(*catMove, "Player "), 0.500);
    expect("calc category AnimParticle ms, every turn", Field(*catCalc, "AnimParticle "), 0.300);
    // classes: scaled to the list's real cost: rock 1000 * 1640/1600, flower 600 * 1640/1600, mario 500
    const std::string* c1 = nullptr; const std::string* c2 = nullptr; const std::string* c3 = nullptr;
    for (size_t i = sLines.size(); i-- > 0;) {
        const std::string& l = sLines[i];
        if (l.compare(0, 12, "logic-prof3:") || l.find("movement class #") == std::string::npos) continue;
        if (!c1 && l.find("class #1 ") != std::string::npos) c1 = &l;
        if (!c2 && l.find("class #2 ") != std::string::npos) c2 = &l;
        if (!c3 && l.find("class #3 ") != std::string::npos) c3 = &l;
    }
    if (!c1 || !c2 || !c3) { printf("missing class rows\n"); return 1; }
    expect("class #1 rock est ms (1025 us)", Field(*c1, "est ms/turn="), 1.025);
    expect("class #2 flower est ms (3 x 200 x 1.025 = 615 us)", Field(*c2, "est ms/turn="), 0.615);
    expect("class #2 objects a turn", Field(*c2, "objects/turn="), 3.0, 0.05);
    expect("class #3 mario est ms", Field(*c3, "est ms/turn="), 0.500);
    // (update25) the per-class phases of the rock (class #1): nerve 600 us, binder 150 us, the rest in no phase; flush rows per drawn sampled turn
    const std::string* ph = nullptr; const std::string* fdl = nullptr; const std::string* fdl1 = nullptr; const std::string* fan = nullptr;
    for (size_t i = sLines.size(); i-- > 0;) {
        const std::string& l = sLines[i];
        if (!ph && l.find("class #1 movement phases") != std::string::npos) ph = &l;
        if (!fdl && l.find("flush-dl (material lists) per drawn turn") != std::string::npos) fdl = &l;
        if (!fdl1 && l.find("flush-dl class #1 ") != std::string::npos) fdl1 = &l;
        if (!fan && l.find("flush-anim (far models' joints + view) per drawn turn") != std::string::npos) fan = &l;
    }
    if (!ph || !fdl || !fdl1 || !fan) { printf("missing phase / flush rows\n"); return 1; }
    expect("rock phase nerve ms", Field(*ph, " nerve "), 0.600);
    expect("rock phase binder ms", Field(*ph, " binder "), 0.150);
    expect("rock phases sum", Field(*ph, "(phases "), 0.750);
    expect("rock phases rest", Field(*ph, "the rest "), 0.250);
    expect("flush-dl total ms a drawn turn (7 + 7 + 5)", Field(*fdl, "sampled drawn turns): "), 0.019);
    expect("flush-dl models a drawn turn", Field(*fdl, "ms timed over "), 3.0, 0.05);
    expect("flush-dl left out a drawn turn", Field(*fdl, "models, "), 1.0, 0.05);
    expect("flush-dl class #1 (plantA, 14 us)", Field(*fdl1, "ms/drawn turn="), 0.014);
    expect("flush-dl class #1 models", Field(*fdl1, "models/drawn turn="), 2.0, 0.05);
    expect("flush-dl class #1 us/model", Field(*fdl1, "us/model="), 7.0, 0.1);
    expect("flush-anim total ms a drawn turn (17)", Field(*fan, "sampled drawn turns): "), 0.017);
    printf("flush-dl class #1 named plantA with its vtable: %s\n", fdl1->find("obj=plantA ") != std::string::npos ? "ok" : "BAD");
    bad += fdl1->find("obj=plantA ") == std::string::npos;
    const bool named = c1->find("model=rock ") != std::string::npos && c2->find("model=flower ") != std::string::npos && c3->find("model=mario ") != std::string::npos &&
                       c1->find("obj=yobj ") != std::string::npos && c1->find("cat=MapObj") != std::string::npos;
    printf("rows named by model, NameObj name and category: %s\n", named ? "ok" : "BAD");
    bad += !named;
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
