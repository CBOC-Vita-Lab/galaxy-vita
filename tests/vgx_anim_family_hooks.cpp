#include "port/diagnostics.h"
#include "port/hot_knob.h"  // (the extracted LiveActor.cpp section reads GXV_ANIM_LOD_XLATCH with PORT_HOT_KNOB)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using u32 = std::uint32_t;
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
static constexpr bool Hooked = true;
#else
static constexpr bool Hooked = false;
#endif
static bool Enabled;
static std::vector<std::string> Events;
static unsigned BeginCalls, EndCalls, NextToken, ActiveToken, ActiveFamily;
static unsigned Cases;

[[noreturn]] static void Fail(const char* why) {
    std::fprintf(stderr, "FAIL: %s\n", why);
    for (const auto& event : Events) std::fprintf(stderr, "  %s\n", event.c_str());
    std::exit(1);
}
static void Require(bool yes, const char* why) { if (!yes) Fail(why); }
static std::string Family(const char* prefix, unsigned family) { return std::string(prefix) + std::to_string(family); }
extern "C" u32 port_workload_anim_family_begin(unsigned family) {
    ++BeginCalls;
    if (!Enabled) return 0;
    Require(!ActiveToken && family < 3, "fixture expects disjoint known family spans");
    ActiveToken = ++NextToken;
    ActiveFamily = family;
    Events.push_back(Family("begin:", family));
    return ActiveToken;
}
extern "C" void port_workload_anim_family_end(u32 token) {
    ++EndCalls;
    if (!token) return;
    Require(token == ActiveToken, "family end must forward its opaque begin ticket");
    Events.push_back(Family("end:", ActiveFamily));
    ActiveToken = 0;
}
// Detailed material checkpoint ordering has its own actual-source fixture.
extern "C" void port_workload_anim_material_checkpoint(u32, unsigned) {}
static void CheckScope(unsigned family) {
    Require(!(Hooked && Enabled) || (ActiveToken && ActiveFamily == family), "work must stay in its exact family scope");
}
static void Reset() {
    Require(!ActiveToken, "previous case left an unmatched scope");
    Events.clear();
    BeginCalls = EndCalls = 0;
}
static void Check(const std::vector<std::string>& expected, unsigned pairs, const char* why) {
    Require(Events == expected, why);
    Require(!ActiveToken, "all family tickets must close");
    Require(BeginCalls == (Hooked ? pairs : 0) && EndCalls == BeginCalls, "only Vita logging branch calls paired family hooks");
    ++Cases;
}
static void Boundary(std::vector<std::string>& events, unsigned family, bool begin) {
    if (Hooked && Enabled) events.push_back(Family(begin ? "begin:" : "end:", family));
}

struct ModelManager;
static ModelManager* MutateManager;
struct XanimePlayer {
    const char* name;
    void calcAnm(int) { CheckScope(1); Events.emplace_back(std::string("prepare:") + name); }
    void clearAnm(int) { CheckScope(1); Events.emplace_back(std::string("clear:") + name); }
};
struct BvaPlayer { void calc() { CheckScope(1); Events.emplace_back("bva"); } };
struct MaterialPlayer {
    const char* name;
    void beginDiff() { CheckScope(2); Events.emplace_back(std::string("attach:") + name); }
    void endDiff() { CheckScope(2); Events.emplace_back(std::string("detach:") + name); }
};
struct DisplayListMaker {
    bool valid = true;
    void (*onUpdate)() = nullptr;
    bool isValidDiff() { CheckScope(2); Events.emplace_back("diff-valid"); return valid; }
    void update() { CheckScope(2); Events.emplace_back("diff-update"); if (onUpdate) onUpdate(); }
    void diff() { CheckScope(2); Events.emplace_back("diff-emit"); }
};
struct J3DModel {
    void (*onCalc)() = nullptr;
    void (*onMaterial)() = nullptr;
    void setBaseScale(int) { Require(!ActiveToken, "scale update precedes exact base-matrix scope"); Events.emplace_back("scale"); }
    void calc() { CheckScope(1); Events.emplace_back("model"); if (onCalc) onCalc(); }
    void calcMaterial() { CheckScope(2); Events.emplace_back("material"); if (onMaterial) onMaterial(); }
};
namespace MR { template <int N> struct MutexHolder { static inline int sMutex; }; }
static void OSLockMutex(int*) {
    Require(!(Hooked && Enabled) || (ActiveToken && ActiveFamily != 0), "model/material mutex must lie inside its family span");
    Events.emplace_back("lock");
}
static void OSUnlockMutex(int*) { Events.emplace_back("unlock"); }
struct ModelManager {
    XanimePlayer* mXanimePlayer = nullptr;
    BvaPlayer* mBvaPlayer = nullptr;
    DisplayListMaker* mDisplayListMaker = nullptr;
    MaterialPlayer* mBtkPlayer = nullptr;
    MaterialPlayer* mBrkPlayer = nullptr;
    MaterialPlayer* mBtpPlayer = nullptr;
    MaterialPlayer* mBpkPlayer = nullptr;
    J3DModel* model;
    void calcAnim();
    void calc();
    void updateDL(bool);
    J3DModel* getJ3DModel() { Events.emplace_back("get-model"); return model; }
};
struct LiveActor {
    struct { bool mIsNoCalcAnim = false; } mFlag;
    ModelManager* mModelManager;
    void* mCollisionParts = nullptr;
    int mScale = 7;
    bool earlyBaseReturn = false;
    ModelManager* switchManager = nullptr;
    void calcAnim();
    void calcAnmMtx();
    virtual void calcAndSetBaseMtx() {
        CheckScope(0);
        Events.emplace_back("base");
        if (earlyBaseReturn) return;
        if (switchManager) { Events.emplace_back("switch-manager"); mModelManager = switchManager; }
    }
};
struct DerivedLiveActor : LiveActor {
    void calcAndSetBaseMtx() override {
        CheckScope(0);
        Events.emplace_back("override-base");
        LiveActor::calcAndSetBaseMtx();
    }
};
namespace MR {
    static J3DModel* getJ3DModel(LiveActor* actor) { Events.emplace_back("actor-model"); return actor->mModelManager->model; }
    static void setCollisionMtx(LiveActor*) { Require(!ActiveToken, "collision update must remain outside all selected family scopes"); Events.emplace_back("collision"); }
}
#include "vgx_anim_family_methods.inc"

static void ModelEvents(std::vector<std::string>& e, bool xanime, bool bva, const char* cleared = "A") {
    Boundary(e, 1, true);
    if (xanime) e.emplace_back("prepare:A");
    if (bva) e.emplace_back("bva");
    e.insert(e.end(), {"lock", "get-model", "model", "unlock"});
    if (cleared) e.emplace_back(std::string("clear:") + cleared);
    Boundary(e, 1, false);
}
static void MaterialEvents(std::vector<std::string>& e, int maker, bool emit, unsigned players) {
    Boundary(e, 2, true);
    e.emplace_back("lock");
    if (maker) e.emplace_back("diff-valid");
    if (maker == 2) {
        e.emplace_back("diff-update");
        const char* names[] = {"btk", "brk", "btp", "bpk"};
        for (unsigned i = 0; i < 4; ++i) if (players & (1u << i)) e.emplace_back(std::string("attach:") + names[i]);
    }
    e.insert(e.end(), {"get-model", "material"});
    if (maker == 2) {
        if (emit) e.emplace_back("diff-emit");
        const char* names[] = {"btk", "brk", "btp", "bpk"};
        for (unsigned i = 0; i < 4; ++i) if (players & (1u << i)) e.emplace_back(std::string("detach:") + names[i]);
    }
    e.emplace_back("unlock");
    Boundary(e, 2, false);
}

int main(int argc, char**) {
    Enabled = argc == 1;
    J3DModel model;
    ModelManager manager;
    manager.model = &model;
    MutateManager = &manager;
    XanimePlayer a{"A"}, b{"B"};
    BvaPlayer bva;
    MaterialPlayer players[] = {{"btk"}, {"brk"}, {"btp"}, {"bpk"}};
    DisplayListMaker maker;
    for (unsigned mask = 0; mask < 4; ++mask) {
        manager.mXanimePlayer = mask & 1 ? &a : nullptr;
        manager.mBvaPlayer = mask & 2 ? &bva : nullptr;
        Reset(); manager.calc();
        std::vector<std::string> expected;
        ModelEvents(expected, mask & 1, mask & 2, mask & 1 ? "A" : nullptr);
        Check(expected, 1, "actual model calc must preserve all null-player branches and complete mutex lifetime");
    }
    for (int diff = 0; diff < 3; ++diff) {
        manager.mDisplayListMaker = diff ? &maker : nullptr;
        maker.valid = diff == 2;
        for (unsigned mask = 0; mask < 16; ++mask) {
            manager.mBtkPlayer = mask & 1 ? &players[0] : nullptr;
            manager.mBrkPlayer = mask & 2 ? &players[1] : nullptr;
            manager.mBtpPlayer = mask & 4 ? &players[2] : nullptr;
            manager.mBpkPlayer = mask & 8 ? &players[3] : nullptr;
            for (unsigned emit = 0; emit < 2; ++emit) {
                Reset(); manager.updateDL(emit);
                std::vector<std::string> expected;
                MaterialEvents(expected, diff, emit, mask);
                Check(expected, 1, "actual updateDL must preserve short-circuit, animation attach/detach, optional diff and lock ordering");
            }
        }
    }
    manager.mXanimePlayer = &a;
    manager.mBvaPlayer = nullptr;
    static XanimePlayer* replacement;
    replacement = &b;
    model.onCalc = [] { MutateManager->mXanimePlayer = replacement; };
    Reset(); manager.calc();
    std::vector<std::string> expected;
    ModelEvents(expected, true, false, "B");
    Check(expected, 1, "callback-mutated Xanime pointer must be read at original cleanup point");
    model.onCalc = [] { MutateManager->mXanimePlayer = nullptr; };
    manager.mXanimePlayer = &a;
    Reset(); manager.calc(); expected.clear(); ModelEvents(expected, true, false, nullptr);
    Check(expected, 1, "callback-cleared Xanime pointer must preserve original conditional cleanup");
    model.onCalc = nullptr;
    manager.mXanimePlayer = nullptr;
    manager.mBtkPlayer = manager.mBrkPlayer = manager.mBtpPlayer = manager.mBpkPlayer = nullptr;
    manager.mDisplayListMaker = nullptr;
    LiveActor actor{};
    actor.mModelManager = &manager;
    actor.mFlag.mIsNoCalcAnim = true;
    Reset(); actor.calcAnim(); Check({}, 0, "no-calc actor early return must remain hook-free");
    actor.mFlag.mIsNoCalcAnim = false;
    actor.mModelManager = nullptr;
    Reset(); actor.calcAnmMtx(); Check({}, 0, "null manager early return must remain hook-free");
    actor.mModelManager = &manager;
    actor.earlyBaseReturn = true;
    actor.mCollisionParts = &actor;
    Reset(); actor.calcAnim();
    expected = {"actor-model", "scale"}; Boundary(expected, 0, true); expected.emplace_back("base"); Boundary(expected, 0, false);
    ModelEvents(expected, false, false, nullptr); MaterialEvents(expected, 0, true, 0); expected.emplace_back("collision");
    Check(expected, 3, "callee early return closes only base scope; model/material/collision ordering stays original");
    ModelManager other;
    other.model = &model;
    other.mXanimePlayer = &b;
    actor.earlyBaseReturn = false;
    actor.switchManager = &other;
    Reset(); actor.calcAnmMtx();
    expected = {"actor-model", "scale"}; Boundary(expected, 0, true); expected.insert(expected.end(), {"base", "switch-manager"}); Boundary(expected, 0, false);
    Boundary(expected, 1, true); expected.insert(expected.end(), {"prepare:B", "lock", "get-model", "model", "unlock", "clear:B"}); Boundary(expected, 1, false);
    MaterialEvents(expected, 0, true, 0);
    Check(expected, 3, "virtual base callback may replace manager; later existing read must not be cached/reordered");
    DerivedLiveActor derived{};
    derived.mModelManager = &manager;
    derived.earlyBaseReturn = true;
    Reset(); derived.calcAnmMtx();
    expected = {"actor-model", "scale"}; Boundary(expected, 0, true); expected.insert(expected.end(), {"override-base", "base"}); Boundary(expected, 0, false);
    ModelEvents(expected, false, false, nullptr); MaterialEvents(expected, 0, true, 0);
    Check(expected, 3, "actual unqualified virtual base call must still dispatch an override inside only base scope");
    std::printf("PASS: actual animation family hooks %s/%s, %u branch/order/callback cases\n", Hooked ? "Vita logging" : "inert", Enabled ? "on" : "off", Cases);
}
