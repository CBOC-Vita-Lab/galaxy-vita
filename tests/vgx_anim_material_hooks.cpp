#include "port/diagnostics.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using u32 = std::uint32_t;
using u16 = std::uint16_t;
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
static constexpr bool Hooked = true;
#else
static constexpr bool Hooked = false;
#endif
static bool Enabled;
static std::vector<std::string> Events;
static unsigned ActiveToken, NextToken, Phase, Checkpoints, Scanned, Flagged, MaterialCalls, Submissions, GuardDepth;
static unsigned ReportScanned, ReportFlagged, ReportCalls, Cases;
static bool Locked;

[[noreturn]] static void Fail(const char* why) {
    std::fprintf(stderr, "FAIL: %s\n", why);
    for (const auto& event : Events) std::fprintf(stderr, "  %s\n", event.c_str());
    std::exit(1);
}
static void Require(bool yes, const char* why) { if (!yes) Fail(why); }
static void At(unsigned phase) { Require(!(Hooked && Enabled) || (ActiveToken && Phase == phase), "real work must stay in the correct material phase"); }
extern "C" u32 port_workload_anim_family_begin(unsigned family) {
    Require(family == 2 && !ActiveToken && !Locked, "material begins before lock");
    if (!Enabled) return 0;
    ActiveToken = ++NextToken; Phase = 0;
    return ActiveToken;
}
extern "C" void port_workload_anim_family_end(u32 token) {
    Require(!Locked && !GuardDepth, "family end must remain after unlock and all guards");
    if (!token) return;
    Require(token == ActiveToken && Phase == 4, "family end flushes final cleanup phase");
    ActiveToken = 0;
}
extern "C" void port_workload_anim_material_checkpoint(u32 token, unsigned phase) {
    if (!token) return;
    Require(token == ActiveToken && Locked && phase < 5, "checkpoint matches material token inside unchanged lock");
    Phase = phase;
    ++Checkpoints;
}
extern "C" u32 port_workload_anim_material_diff_context() {
    if (!Enabled) return 0;
    Require(ActiveToken && Phase == 3, "diff context must belong to active diff phase");
    return ActiveToken;
}
extern "C" void port_workload_anim_material_diff_counts(u32 token, u32 scanned, u32 flagged, u32 calls) {
    if (!token) return;
    Require(token == ActiveToken && Phase == 3 && Locked && !GuardDepth, "census submits after the loop, before leaving real diff call");
    ++Submissions;
    ReportScanned += scanned; ReportFlagged += flagged; ReportCalls += calls;
}
namespace MR {
    template <int N> struct MutexHolder { static inline int sMutex; };
    struct ProhibitSchedulerAndInterrupts {
        explicit ProhibitSchedulerAndInterrupts(bool value) {
            At(3); Require(!value && Locked && !GuardDepth, "original false-valued guard construction/order must stay unchanged");
            ++GuardDepth; Events.emplace_back("guard-begin");
        }
        ~ProhibitSchedulerAndInterrupts() { Require(GuardDepth == 1, "balanced scheduler guard"); --GuardDepth; Events.emplace_back("guard-end"); }
    };
}
static void OSLockMutex(int*) { At(0); Require(!Locked, "single original lock"); Locked = true; Events.emplace_back("lock"); }
static void OSUnlockMutex(int*) { At(4); Require(Locked && !GuardDepth, "cleanup precedes original unlock"); Locked = false; Events.emplace_back("unlock"); }
struct ModelManager;
struct DisplayListMaker;
static ModelManager* Manager;
static DisplayListMaker* Maker;
struct Material {
    unsigned id;
    bool internalDiffEnabled = true;
    void (*onDiff)() = nullptr;
    void diff(unsigned flag) {
        At(3); Require(Locked && GuardDepth == 1, "actual material diff stays guarded");
        ++MaterialCalls; Events.emplace_back("material-diff:" + std::to_string(id) + ':' + std::to_string(flag));
        if (onDiff) onDiff();
        if (!internalDiffEnabled) return;
        Events.emplace_back("internal-diff:" + std::to_string(id));
    }
};
struct ModelData {
    unsigned count = 0, countQueries = 0, nodeQueries = 0;
    Material materials[8];
    unsigned getMaterialNum() {
        At(3); ++countQueries;
        return count;
    }
    Material* getMaterialNodePointer(unsigned index) {
        At(3); Require(GuardDepth == 1, "material lookup stays inside original guard"); ++nodeQueries;
        return &materials[index];
    }
};
struct J3DModel {
    ModelData* data;
    void (*onMaterial)() = nullptr;
    ModelData* getModelData() { At(3); Events.emplace_back("model-data"); return data; }
    unsigned getMatPacket(unsigned index) { At(3); Require(GuardDepth == 1, "packet lookup remains guarded"); Events.emplace_back("packet:" + std::to_string(index)); return index; }
    void calcMaterial() { At(2); Events.emplace_back("calc-material"); if (onMaterial) onMaterial(); }
};
struct Sys {
    void setMatPacket(unsigned packet) { At(3); Require(GuardDepth == 1, "packet setter stays guarded"); Events.emplace_back("set-packet:" + std::to_string(packet)); }
} j3dSys;
struct MaterialPlayer {
    const char* name;
    void beginDiff() { At(1); Events.emplace_back(std::string("attach:") + name); }
    void endDiff() { At(4); Events.emplace_back(std::string("detach:") + name); }
};
struct DisplayListMaker {
    J3DModel* mModel;
    u32 mCurFlag[8] = {};
    bool valid = true;
    bool isValidDiff() { At(1); Events.emplace_back("valid"); return valid; }
    void update() { At(1); Events.emplace_back("prepare"); }
    unsigned getDiffFlag(unsigned index) const { At(3); Require(GuardDepth == 1, "diff flag query remains guarded"); Events.emplace_back("diff-flag:" + std::to_string(index)); return mCurFlag[index] + 100; }
    void diff();
};
struct ModelManager {
    DisplayListMaker* mDisplayListMaker;
    MaterialPlayer* mBtkPlayer = nullptr;
    MaterialPlayer* mBrkPlayer = nullptr;
    MaterialPlayer* mBtpPlayer = nullptr;
    MaterialPlayer* mBpkPlayer = nullptr;
    J3DModel* model;
    J3DModel* getJ3DModel() { At(2); Events.emplace_back("manager-model"); return model; }
    void updateDL(bool);
};
// (ModelManager::updateDL's GXV_DRAWDONE_DEFER hooks: no deferred wait is ever pending here)
static void port_gx_drawdone_settle(int) {}
static void port_gx_view_write_from(void*) {}
#include "vgx_anim_material_methods.inc"

static void Reset() {
    Require(!Locked && !ActiveToken && !GuardDepth, "no state leaked between cases");
    Events.clear(); Checkpoints = Scanned = Flagged = MaterialCalls = Submissions = ReportScanned = ReportFlagged = ReportCalls = 0;
}
static void Check(unsigned checkpoints, unsigned scanned, unsigned flagged, unsigned calls, bool invoked, const char* why) {
    Require(!Locked && !ActiveToken && !GuardDepth, why);
    Require(Checkpoints == (Hooked && Enabled ? checkpoints : 0), "actual checkpoints follow only executed material branches");
    Require(Submissions == (Hooked && Enabled && invoked ? 1u : 0u), "one census invocation per actual diff method, including empty loops");
    Require(ReportScanned == (Hooked && Enabled ? scanned : 0) && ReportFlagged == (Hooked && Enabled ? flagged : 0)
        && ReportCalls == (Hooked && Enabled ? calls : 0), "aggregate census reflects actual entered iterations/branches/calls only");
    Require(MaterialCalls == calls, "census counts actual material method calls, not internal diff-enabled work");
    ++Cases;
}
int main(int argc, char**) {
    Enabled = argc == 1;
    ModelData data;
    for (unsigned i = 0; i < 8; ++i) data.materials[i].id = i;
    J3DModel model{&data};
    DisplayListMaker maker{&model};
    Maker = &maker;
    ModelManager manager{};
    manager.mDisplayListMaker = &maker;
    manager.model = &model; Manager = &manager;
    MaterialPlayer btk{"btk"}, brk{"brk"};
    manager.mBtkPlayer = &btk; manager.mBrkPlayer = &brk;
    for (unsigned count = 0; count <= 5; ++count) {
        for (unsigned flags = 0; flags < (1u << count); ++flags) {
            for (unsigned i = 0; i < count; ++i) maker.mCurFlag[i] = (flags >> i) & 1u;
            data.count = count;
            data.countQueries = data.nodeQueries = 0;
            unsigned expectedFlags = 0;
            for (unsigned i = 0; i < count; ++i) expectedFlags += (flags >> i) & 1u;
            Reset(); manager.updateDL(true);
            Check(5, count, expectedFlags, expectedFlags, true, "ordinary diff route must end cleanly");
            Require(data.countQueries == count + 1 && data.nodeQueries == expectedFlags, "original getter query count/order must not be cached or duplicated");
        }
    }
    // A call that returns inside material->diff still counts as a method call, not a GPU/command change.
    data.count = 2; maker.mCurFlag[0] = maker.mCurFlag[1] = 1;
    data.materials[0].internalDiffEnabled = data.materials[1].internalDiffEnabled = false;
    Reset(); manager.updateDL(true); Check(5, 2, 2, 2, true, "internal diff no-op return remains visible in actual-call census");
    data.materials[0].internalDiffEnabled = data.materials[1].internalDiffEnabled = true;
    Reset(); manager.updateDL(false); Check(3, 0, 0, 0, false, "isDiffDL false must not query diff context or census");
    maker.valid = false;
    Reset(); manager.updateDL(true); Check(3, 0, 0, 0, false, "invalid maker still calcMaterial, with no actual diff invocation");
    maker.valid = true;
    manager.mDisplayListMaker = nullptr;
    Reset(); manager.updateDL(true); Check(3, 0, 0, 0, false, "null maker preserves short-circuit and material calculation");
    manager.mDisplayListMaker = &maker;
    // Existing loop condition and future flags are intentionally live after callbacks.
    data.count = 4;
    for (unsigned i = 0; i < 4; ++i) maker.mCurFlag[i] = 1;
    data.materials[0].onDiff = [] { Maker->mModel->data->count = 2; Maker->mCurFlag[1] = 0; };
    data.countQueries = data.nodeQueries = 0;
    Reset(); manager.updateDL(true); Check(5, 2, 1, 1, true, "callback mutation must affect later original condition/flag reads");
    Require(data.countQueries == 3 && data.nodeQueries == 1, "callback-modified loop bound remains live");
    data.materials[0].onDiff = nullptr;
    // Preserve the original late mDisplayListMaker read after calcMaterial callbacks.
    ModelData otherData; otherData.count = 0;
    J3DModel otherModel{&otherData};
    DisplayListMaker otherMaker{&otherModel};
    static DisplayListMaker* replacement;
    replacement = &otherMaker;
    model.onMaterial = [] { Manager->mDisplayListMaker = replacement; Manager->mBtkPlayer = nullptr; };
    Reset(); manager.updateDL(true); Check(5, 0, 0, 0, true, "material callback may replace maker and cleanup player without stale caching");
    Require(otherData.countQueries == 1, "post-calc diff must invoke replacement maker");
    Require(Events.back() == "unlock", "observer checkpoints do not alter game event ordering");
    std::printf("PASS: actual material checkpoints/diff census %s/%s, %u cases\n", Hooked ? "Vita logging" : "inert", Enabled ? "on" : "off", Cases);
}
