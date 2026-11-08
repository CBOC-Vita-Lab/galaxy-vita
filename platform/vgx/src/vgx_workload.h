// Optional coarse workload observer. Never waits for the GPU or changes a command.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include "vgx_common.h"

extern "C" int64_t port_host_time_ns(void) __attribute__((weak));
extern "C" int64_t port_thread_cpu_us(void) __attribute__((weak));
extern "C" void port_log_summary(const char*, ...) __attribute__((weak, format(printf, 1, 2)));

namespace vgx { void DlCacheBranchCounts(uint64_t out[7]) __attribute__((weak)); void StateListFastCounters(uint64_t out[3]) __attribute__((weak));
                void DrawReplayCounters5(uint64_t out[5]) __attribute__((weak));
                namespace split { void KeyIdCounters(uint64_t out[4]) __attribute__((weak)); }
                void FeCutCounters(uint64_t out[4]) __attribute__((weak)); void FxMergeCounters(uint64_t out[6]) __attribute__((weak)); }
namespace vgx::workload {
struct PreserveErrno {
    int saved = errno;
    ~PreserveErrno() { errno = saved; }
};
inline bool Enabled() {
#if defined(PORT_LOGGING) && !PORT_LOGGING
    return false;
#else
    // GXV_WORKLOAD_PROBE=1: coarse CPU/wall/count observations in the basic log, also in quiet player builds; default off.
    const bool on = VGX_KNOB([] {
        PreserveErrno keep;
        return EnvInt("GXV_WORKLOAD_PROBE", 0) == 1;
    }()) != 0;
    return on && port_host_time_ns && port_thread_cpu_us && port_log_summary;
#endif
}
inline uint32_t Stride() {
    // GXV_WORKLOAD_STRIDE=1..1024: sample one ProcessStream call in this many; default 32, 1 measures observer overhead/full scopes.
    const uint32_t stride = static_cast<uint32_t>(VGX_KNOB([] {
        const long n = EnvInt("GXV_WORKLOAD_STRIDE", 32);
        return static_cast<uint32_t>(n < 1 ? 1 : (n > 1024 ? 1024 : n));
    }()));
    return stride;
}

enum Lane : unsigned { Turn, Dsp, DspInTurn, PublishInTurn, FinishInTurn, FrontSample, FrontFrame, FrontQueue, BackendExecute, GameViewEntry, LaneCount };
inline constexpr const char* Names[] = {"work-turn", "dsp-all", "dsp-in-turn", "gx-publish-in-turn", "gx-finish-phase-in-turn",
                                      "fe-process-sampled", "fe-frame-including-idle", "fe-backend-queue", "backend-execute", "game-view-entry"};
struct Reading { int64_t wall = 0, cpu = 0; };
struct Totals {
    uint32_t calls = 0, wallSamples = 0, wallUs = 0, cpuSamples = 0, matchedWallUs = 0, cpuUs = 0;
};
inline Totals Delta(const Totals& now, const Totals& old) {
    // Unsigned modular deltas tolerate lifetime counter wrap (report windows are five seconds).
    return {now.calls - old.calls, now.wallSamples - old.wallSamples, now.wallUs - old.wallUs,
            now.cpuSamples - old.cpuSamples, now.matchedWallUs - old.matchedWallUs, now.cpuUs - old.cpuUs};
}
struct Counter {
    std::atomic<uint32_t> calls{0}, wallSamples{0}, wallUs{0}, cpuSamples{0}, matchedWallUs{0}, cpuUs{0};
    void Add(Reading begin, Reading end) {
        calls.fetch_add(1, std::memory_order_relaxed);
        if (begin.wall <= 0 || end.wall < begin.wall || (end.wall - begin.wall) / 1000 > UINT32_MAX) return;
        const auto wall = static_cast<uint32_t>((end.wall - begin.wall) / 1000);
        wallSamples.fetch_add(1, std::memory_order_relaxed);
        wallUs.fetch_add(wall, std::memory_order_relaxed);
        if (begin.cpu <= 0 || end.cpu < begin.cpu || end.cpu - begin.cpu > UINT32_MAX) return;
        cpuSamples.fetch_add(1, std::memory_order_relaxed);
        matchedWallUs.fetch_add(wall, std::memory_order_relaxed);
        cpuUs.fetch_add(static_cast<uint32_t>(end.cpu - begin.cpu), std::memory_order_relaxed);
    }
    Totals Read() const {
        return {calls.load(std::memory_order_relaxed), wallSamples.load(std::memory_order_relaxed), wallUs.load(std::memory_order_relaxed),
                cpuSamples.load(std::memory_order_relaxed), matchedWallUs.load(std::memory_order_relaxed), cpuUs.load(std::memory_order_relaxed)};
    }
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
inline Counter Counters[LaneCount];
inline std::atomic<uint32_t> ProcessCalls{0};
inline Reading ReadClock() { return {port_host_time_ns(), port_thread_cpu_us()}; }
struct ThreadState {
    bool inTurn = false, inFinish = false, frontStarted = false;
    uint32_t generation = 0, processCalls = 0;
    Reading turn, finish, front;
};
inline thread_local ThreadState Thread;

struct ViewEntrySpan {
    enum Mode { Idle, Active, Rejected } mode = Idle;
    uint32_t generation = 0;
    int64_t wall = 0;
};
inline thread_local ViewEntrySpan ViewEntry;
inline void BeginViewEntry() {
    if (!Enabled()) return;
    PreserveErrno keep;
    if (!Thread.inTurn) return;
    ViewEntrySpan& span = ViewEntry;
    if (span.generation == Thread.generation && span.mode != ViewEntrySpan::Idle) {
        span.mode = ViewEntrySpan::Rejected;
        return;
    }
    span = {ViewEntrySpan::Active, Thread.generation, port_host_time_ns()};
}
inline void EndViewEntry() {
    if (!Enabled()) return;
    PreserveErrno keep;
    ViewEntrySpan& span = ViewEntry;
    if (span.mode == ViewEntrySpan::Rejected) return;
    const bool valid = span.mode == ViewEntrySpan::Active && Thread.inTurn && span.generation == Thread.generation;
    span.mode = ViewEntrySpan::Idle;
    if (valid) Counters[GameViewEntry].Add({span.wall, 0}, {port_host_time_ns(), 0});
}

#define VGX_ANIM_CATEGORIES_VERSION 1
inline bool AnimEnabled() {
#if defined(PORT_LOGGING) && !PORT_LOGGING
    return false;
#else
    // Check both literal opt-ins before even reading errno or touching observer TLS.
    const bool requested = VGX_KNOB([] {
        const char* a = getenv("GXV_ANIM_CATEGORIES"), *w = getenv("GXV_WORKLOAD_PROBE");
        return a && a[0] == '1' && !a[1] && w && w[0] == '1' && !w[1];
    }()) != 0;
    return requested && Enabled();
#endif
}
#define VGX_ANIM_FAMILIES_VERSION 1
inline bool AnimFamiliesEnabled() {
#if defined(PORT_LOGGING) && !PORT_LOGGING
    return false;
#else
    const bool requested = VGX_KNOB([] {
        const char* e = getenv("GXV_ANIM_FAMILIES");
        return e && e[0] == '1' && !e[1];
    }()) != 0;
    return requested && AnimEnabled();
#endif
}
#define VGX_ANIM_MATERIAL_VERSION 1
inline bool AnimMaterialEnabled() {
#if defined(PORT_LOGGING) && !PORT_LOGGING
    return false;
#else
    const bool requested = VGX_KNOB([] {
        const char* e = getenv("GXV_ANIM_MATERIAL");
        return e && e[0] == '1' && !e[1];
    }()) != 0;
    return requested && AnimFamiliesEnabled();
#endif
}
inline uint32_t AnimStride() {
    const uint32_t stride = static_cast<uint32_t>(VGX_KNOB([] {
        const long n = EnvInt("GXV_ANIM_STRIDE", 31);
        return static_cast<uint32_t>(n < 1 ? 1 : n > 1024 ? 1024 : n);
    }()));
    return stride;
}
enum AnimRow : unsigned {
    AnimParent, AnimResidual, AnimEnvironment, AnimMapObj, AnimNpc, AnimRide, AnimEnemy, AnimPlayer,
    AnimPlayerDecoration, AnimMapObjDecoration, AnimMirrorMapObj, AnimLayout, AnimLayoutDecoration,
    AnimMovie, AnimItem, AnimUnk12, AnimParticle, AnimIgnorePause, AnimShadow, AnimUnknown, AnimRowCount
};
inline constexpr const char* AnimNames[] = {"nonview", "residual", "environment", "map-objects", "npc", "ride", "enemy", "player",
    "player-decoration", "map-decoration", "mirror-map-objects", "layout", "layout-decoration", "movie-subtitles", "item",
    "unknown-0x12", "particles", "particles-ignore-pause", "shadow-movement", "unknown-category"};
static_assert(sizeof(AnimNames) / sizeof(*AnimNames) == AnimRowCount);
enum AnimCensus : unsigned {
    AnimSelected, AnimDrawn, AnimSkipped, AnimUnknownTurn, AnimStarted, AnimComplete, AnimInvalid, AnimUnfinished,
    AnimNested, AnimUnknownDispatch, AnimOrphan, AnimDepthOverflow, AnimTicketOverflow, AnimCensusCount
};
enum AnimFamilyRow : unsigned {
    FamilyNpcBase, FamilyNpcModel, FamilyNpcMaterial, FamilyNpcResidual,
    FamilyMapBase, FamilyMapModel, FamilyMapMaterial, FamilyMapResidual, FamilyRowCount
};
inline constexpr const char* FamilyNames[] = {"npc-base", "npc-model", "npc-material", "npc-residual",
    "map-objects-base", "map-objects-model", "map-objects-material", "map-objects-residual"};
static_assert(sizeof(FamilyNames) / sizeof(*FamilyNames) == FamilyRowCount);
enum AnimFamilyCensus : unsigned {
    FamilyNested, FamilyUnknown, FamilyOrphan, FamilyUnfinished, FamilyDepthOverflow, FamilyTicketOverflow,
    FamilyClockInvalid, FamilyCensusCount
};
enum AnimMaterialRow : unsigned {
    MaterialNpcEntryLock, MaterialNpcPrepare, MaterialNpcCalc, MaterialNpcDiff, MaterialNpcCleanup,
    MaterialMapEntryLock, MaterialMapPrepare, MaterialMapCalc, MaterialMapDiff, MaterialMapCleanup, MaterialRowCount
};
inline constexpr const char* MaterialNames[] = {"npc-entry-lock", "npc-prepare", "npc-calc-material", "npc-diff", "npc-cleanup",
    "map-objects-entry-lock", "map-objects-prepare", "map-objects-calc-material", "map-objects-diff", "map-objects-cleanup"};
static_assert(sizeof(MaterialNames) / sizeof(*MaterialNames) == MaterialRowCount);
enum AnimMaterialCensus : unsigned {
    MaterialNestedMarks, MaterialBadToken, MaterialBadPhase, MaterialClockInvalid, MaterialUnfinished, MaterialCensusInvalid, MaterialCensusCount
};
inline constexpr unsigned MaterialDiffCount = 8; // Per owner: invocations, scanned iterations, flagged branches, actual call expressions.
struct AnimSnapshot {
    Totals rows[AnimRowCount]{}, families[FamilyRowCount]{}, materials[MaterialRowCount]{};
    uint32_t census[AnimCensusCount]{}, familyCensus[FamilyCensusCount]{};
    uint32_t materialCensus[MaterialCensusCount]{}, diffs[MaterialDiffCount]{};
};
inline Counter AnimCounters[AnimRowCount];
inline Counter FamilyCounters[FamilyRowCount];
inline Counter MaterialCounters[MaterialRowCount];
inline std::atomic<uint32_t> AnimCounts[AnimCensusCount], AnimSequence{0}, AnimTickets{1}, AnimOwner{0};
inline std::atomic<uint32_t> FamilyCounts[FamilyCensusCount];
inline std::atomic<uint32_t> MaterialCounts[MaterialCensusCount], MaterialDiffs[MaterialDiffCount];
inline AnimSnapshot AnimPrevious;
inline uint32_t AnimSkippedReports = 0;
struct AnimThreadState {
    uint32_t owner = 0, generation = 0, turnIndex = 0, parent = 0, depth = 0;
    bool activeTurn = false, selected = false, bad = false, decisionSeen = false, decisionConflict = false;
    int decision = -1;
    int64_t parentWall = 0, rootWall = 0, categoryNs = 0;
    AnimRow rootRow = AnimUnknown;
    uint32_t stack[8]{}, categoryCalls[AnimRowCount]{};
    int64_t categoryWall[AnimRowCount]{};
    Totals rows[AnimRowCount]{};
    uint32_t census[AnimCensusCount]{};
    uint32_t familyDepth = 0, familyKind = 0, familyStack[8]{}, familyCalls[FamilyRowCount]{};
    int familyTarget = -1;
    int64_t familyStart = 0, familyLastEnd = 0, familyRootNs = 0, familyNs[FamilyRowCount]{};
    Totals families[FamilyRowCount]{};
    uint32_t familyCensus[FamilyCensusCount]{};
    unsigned materialPhase = 0;
    bool materialDiffSeen = false;
    int64_t materialPhaseStart = 0, materialRootElapsed = 0, materialRootNs[5]{}, materialNs[MaterialRowCount]{};
    uint32_t materialRootCalls[5]{}, materialCalls[MaterialRowCount]{}, materialParentDiffs[MaterialDiffCount]{};
    Totals materials[MaterialRowCount]{};
    uint32_t materialCensus[MaterialCensusCount]{}, diffs[MaterialDiffCount]{};
};
inline thread_local AnimThreadState AnimThread;
inline int FamilyTarget(AnimRow row) { return row == AnimNpc ? 0 : row == AnimMapObj ? 1 : -1; }
inline void AbandonAnimFamily(AnimThreadState& s) {
    if (!AnimFamiliesEnabled() || !s.familyDepth) return;
    if (AnimMaterialEnabled() && s.familyKind == 2) ++s.materialCensus[MaterialUnfinished];
    ++s.familyCensus[FamilyUnfinished];
    s.familyDepth = 0;
    s.bad = true;
}
inline uint32_t AnimTicket() {
    uint32_t next = AnimTickets.load(std::memory_order_relaxed);
    while (next != UINT32_MAX) {
        if (AnimTickets.compare_exchange_weak(next, next + 1, std::memory_order_relaxed)) return next;
    }
    return 0; // No wrap/reuse: a stale or foreign ticket can never become current again.
}
inline bool ReadAnimSnapshot(AnimSnapshot& result) {
    if (!AnimEnabled()) return false;
    const uint32_t before = AnimSequence.load(std::memory_order_acquire);
    if (before & 1u) return false;
    AnimSnapshot copy;
    for (unsigned i = 0; i < AnimRowCount; ++i) copy.rows[i] = AnimCounters[i].Read();
    for (unsigned i = 0; i < AnimCensusCount; ++i) copy.census[i] = AnimCounts[i].load(std::memory_order_relaxed);
    if (AnimFamiliesEnabled()) {
        for (unsigned i = 0; i < FamilyRowCount; ++i) copy.families[i] = FamilyCounters[i].Read();
        for (unsigned i = 0; i < FamilyCensusCount; ++i) copy.familyCensus[i] = FamilyCounts[i].load(std::memory_order_relaxed);
    }
    if (AnimMaterialEnabled()) {
        for (unsigned i = 0; i < MaterialRowCount; ++i) copy.materials[i] = MaterialCounters[i].Read();
        for (unsigned i = 0; i < MaterialCensusCount; ++i) copy.materialCensus[i] = MaterialCounts[i].load(std::memory_order_relaxed);
        for (unsigned i = 0; i < MaterialDiffCount; ++i) copy.diffs[i] = MaterialDiffs[i].load(std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (before != AnimSequence.load(std::memory_order_relaxed)) return false;
    result = copy;
    return true;
}
inline void AnimPublishTurn(AnimThreadState& s) {
    if (!s.activeTurn) return;
    if (s.selected) {
        if (s.parent) { AbandonAnimFamily(s); ++s.census[AnimUnfinished]; }
        ++s.census[s.decision < 0 ? AnimUnknownTurn : s.decision == 0 ? AnimDrawn : AnimSkipped];
        // Exactly one profiling-owner thread publishes complete local transactions.
        AnimSequence.fetch_add(1, std::memory_order_relaxed);
        // A reader observing any new payload synchronizes this fence with its acquire
        // fence, so its final sequence load cannot observe the preceding even value.
        std::atomic_thread_fence(std::memory_order_release);
        for (unsigned i = 0; i < AnimRowCount; ++i) {
            AnimCounters[i].calls.fetch_add(s.rows[i].calls, std::memory_order_relaxed);
            AnimCounters[i].wallSamples.fetch_add(s.rows[i].wallSamples, std::memory_order_relaxed);
            AnimCounters[i].wallUs.fetch_add(s.rows[i].wallUs, std::memory_order_relaxed);
        }
        for (unsigned i = 0; i < AnimCensusCount; ++i) AnimCounts[i].fetch_add(s.census[i], std::memory_order_relaxed);
        if (AnimFamiliesEnabled()) {
            for (unsigned i = 0; i < FamilyRowCount; ++i) {
                FamilyCounters[i].calls.fetch_add(s.families[i].calls, std::memory_order_relaxed);
                FamilyCounters[i].wallSamples.fetch_add(s.families[i].wallSamples, std::memory_order_relaxed);
                FamilyCounters[i].wallUs.fetch_add(s.families[i].wallUs, std::memory_order_relaxed);
            }
            for (unsigned i = 0; i < FamilyCensusCount; ++i) FamilyCounts[i].fetch_add(s.familyCensus[i], std::memory_order_relaxed);
        }
        if (AnimMaterialEnabled()) {
            for (unsigned i = 0; i < MaterialRowCount; ++i) {
                MaterialCounters[i].calls.fetch_add(s.materials[i].calls, std::memory_order_relaxed);
                MaterialCounters[i].wallSamples.fetch_add(s.materials[i].wallSamples, std::memory_order_relaxed);
                MaterialCounters[i].wallUs.fetch_add(s.materials[i].wallUs, std::memory_order_relaxed);
            }
            for (unsigned i = 0; i < MaterialCensusCount; ++i) MaterialCounts[i].fetch_add(s.materialCensus[i], std::memory_order_relaxed);
            for (unsigned i = 0; i < MaterialDiffCount; ++i) MaterialDiffs[i].fetch_add(s.diffs[i], std::memory_order_relaxed);
        }
        AnimSequence.fetch_add(1, std::memory_order_release);
    }
    s.activeTurn = false;
    s.parent = s.depth = 0;
}
inline void AnimBeginTurn() {
    if (!AnimEnabled()) return;
    PreserveErrno keep;
    AnimThreadState& s = AnimThread;
    if (!s.owner) {
        if (AnimOwner.load(std::memory_order_relaxed)) return;
        const uint32_t ticket = AnimTicket();
        uint32_t empty = 0;
        if (!ticket || !AnimOwner.compare_exchange_strong(empty, ticket, std::memory_order_relaxed)) return;
        s.owner = ticket;
    }
    if (s.owner != AnimOwner.load(std::memory_order_relaxed)) return;
    AnimPublishTurn(s); // A missing EndTurn abandons, rather than reuses, its unfinished parent.
    s.generation = Thread.generation;
    s.selected = s.turnIndex++ % AnimStride() == 0;
    s.activeTurn = true;
    s.decisionSeen = s.decisionConflict = false;
    s.decision = -1;
    for (auto& row : s.rows) row = {};
    for (auto& count : s.census) count = 0;
    if (AnimFamiliesEnabled()) {
        for (auto& row : s.families) row = {};
        for (auto& count : s.familyCensus) count = 0;
    }
    if (AnimMaterialEnabled()) {
        for (auto& row : s.materials) row = {};
        for (auto& n : s.materialCensus) n = 0;
        for (auto& n : s.diffs) n = 0;
    }
    s.census[AnimSelected] = s.selected;
}
inline bool AnimActive(const AnimThreadState& s) {
    return s.owner && s.activeTurn && s.selected && Thread.inTurn && s.generation == Thread.generation;
}
inline void AnimEndTurn() {
    if (!AnimEnabled()) return;
    PreserveErrno keep;
    AnimThreadState& s = AnimThread;
    if (s.owner && s.generation == Thread.generation) AnimPublishTurn(s);
}
inline void AnimDrawDecision(int decision) {
    if (!AnimEnabled()) return;
    PreserveErrno keep;
    AnimThreadState& s = AnimThread;
    if (!AnimActive(s)) return;
    const int value = decision == 0 ? 0 : 1;
    if (s.decisionSeen && s.decision != value) s.decisionConflict = true;
    s.decisionSeen = true;
    s.decision = s.decisionConflict ? -1 : value;
}
inline AnimRow AnimCategory(unsigned domain, unsigned category) {
    if (domain == 1) return category == 45 ? AnimShadow : AnimUnknown;
    if (domain != 0) return AnimUnknown;
    switch (category) {
    case 4: return AnimEnvironment; case 5: return AnimMapObj; case 6: return AnimNpc; case 7: return AnimRide;
    case 8: return AnimEnemy; case 9: return AnimPlayer; case 10: return AnimPlayerDecoration; case 11: return AnimMapObjDecoration;
    case 12: return AnimMirrorMapObj; case 13: return AnimLayout; case 14: return AnimLayoutDecoration; case 15: return AnimMovie;
    case 16: return AnimItem; case 18: return AnimUnk12; case 19: return AnimParticle; case 20: return AnimIgnorePause;
    default: return AnimUnknown;
    }
}
inline bool AnimFamilyActive(const AnimThreadState& s) {
    return AnimActive(s) && s.parent && s.depth && FamilyTarget(s.rootRow) >= 0;
}
inline bool FlushAnimMaterial(AnimThreadState& s, int64_t now) {
    constexpr int64_t maxNs = static_cast<int64_t>(UINT32_MAX) * 1000;
    if (s.materialPhaseStart <= 0 || now < s.materialPhaseStart || now - s.materialPhaseStart > maxNs - s.materialRootElapsed) {
        s.bad = true; ++s.materialCensus[MaterialClockInvalid]; return false;
    }
    const int64_t ns = now - s.materialPhaseStart;
    s.materialRootNs[s.materialPhase] += ns;
    ++s.materialRootCalls[s.materialPhase];
    s.materialRootElapsed += ns; s.materialPhaseStart = now;
    return true;
}
inline void AnimMaterialCheckpoint(uint32_t ticket, unsigned phase) {
    if (!ticket || !AnimMaterialEnabled()) return;
    PreserveErrno keep;
    AnimThreadState& s = AnimThread;
    if (!AnimFamilyActive(s) || !s.familyDepth || s.bad) return;
    for (unsigned i = 1; i < s.familyDepth; ++i) {
        if (s.familyStack[i] == ticket) { ++s.materialCensus[MaterialNestedMarks]; return; }
    }
    if (s.familyKind != 2) return;
    if (s.familyStack[0] != ticket) { s.bad = true; ++s.materialCensus[MaterialBadToken]; return; }
    if (s.familyDepth != 1) { ++s.materialCensus[MaterialNestedMarks]; return; }
    const bool valid = (s.materialPhase == 0 && phase == 1) || (s.materialPhase == 1 && phase == 2) ||
        (s.materialPhase == 2 && phase == 4) || (s.materialPhase == 4 && phase == 3 && !s.materialDiffSeen) ||
        (s.materialPhase == 3 && phase == 4);
    if (!valid) { s.bad = true; ++s.materialCensus[MaterialBadPhase]; return; }
    if (!FlushAnimMaterial(s, port_host_time_ns())) return;
    s.materialPhase = phase;
    s.materialDiffSeen |= phase == 3;
}
inline uint32_t AnimMaterialDiffContext() {
    if (!AnimMaterialEnabled()) return 0;
    PreserveErrno keep;
    const AnimThreadState& s = AnimThread;
    return AnimFamilyActive(s) && s.familyDepth && s.familyKind == 2 && !s.bad && s.materialPhase == 3 ? s.familyStack[0] : 0;
}
inline void AnimMaterialDiffCounts(uint32_t ticket, uint32_t scanned, uint32_t flagged, uint32_t calls) {
    if (!ticket || !AnimMaterialEnabled()) return;
    PreserveErrno keep;
    AnimThreadState& s = AnimThread;
    if (!AnimFamilyActive(s) || !s.familyDepth || s.familyKind != 2 || s.bad) return;
    if (ticket != s.familyStack[0]) { s.bad = true; ++s.materialCensus[MaterialBadToken]; return; }
    if (s.materialPhase != 3) { s.bad = true; ++s.materialCensus[MaterialBadPhase]; return; }
    if (flagged > scanned || calls != flagged) { s.bad = true; ++s.materialCensus[MaterialCensusInvalid]; return; }
    const unsigned first = static_cast<unsigned>(s.familyTarget) * 4;
    ++s.materialParentDiffs[first]; s.materialParentDiffs[first + 1] += scanned;
    s.materialParentDiffs[first + 2] += flagged; s.materialParentDiffs[first + 3] += calls;
}
inline bool EndAnimMaterial(AnimThreadState& s, int64_t now) {
    if (!AnimMaterialEnabled() || s.familyKind != 2) return true;
    if (s.materialPhase != 4) { s.bad = true; ++s.materialCensus[MaterialUnfinished]; return false; }
    if (!FlushAnimMaterial(s, now)) return false;
    if (s.materialRootElapsed != now - s.familyStart) { s.bad = true; ++s.materialCensus[MaterialClockInvalid]; return false; }
    for (unsigned phase = 0; phase < 5; ++phase) {
        const unsigned row = static_cast<unsigned>(s.familyTarget) * 5 + phase;
        s.materialNs[row] += s.materialRootNs[phase]; s.materialCalls[row] += s.materialRootCalls[phase];
    }
    return true;
}
inline uint32_t BeginAnimFamily(unsigned family) {
    if (!AnimFamiliesEnabled()) return 0;
    PreserveErrno keep;
    AnimThreadState& s = AnimThread;
    if (!AnimFamilyActive(s) || s.bad) return 0;
    if (family > 2) { s.bad = true; ++s.familyCensus[FamilyUnknown]; return 0; }
    if (s.familyDepth == sizeof(s.familyStack) / sizeof(*s.familyStack)) {
        s.bad = true; ++s.familyCensus[FamilyDepthOverflow]; return 0;
    }
    const uint32_t ticket = AnimTicket();
    if (!ticket) { s.bad = true; ++s.familyCensus[FamilyTicketOverflow]; return 0; }
    if (!s.familyDepth) {
        s.familyKind = family;
        s.familyStart = port_host_time_ns();
        if (s.familyStart < s.familyLastEnd) { s.bad = true; ++s.familyCensus[FamilyClockInvalid]; return 0; }
        if (family == 2 && AnimMaterialEnabled()) {
            s.materialPhase = 0; s.materialDiffSeen = false;
            s.materialPhaseStart = s.familyStart; s.materialRootElapsed = 0;
            for (auto& n : s.materialRootNs) n = 0;
            for (auto& n : s.materialRootCalls) n = 0;
        }
    } else ++s.familyCensus[FamilyNested];
    s.familyStack[s.familyDepth++] = ticket;
    return ticket;
}
inline void EndAnimFamily(uint32_t ticket) {
    if (!ticket || !AnimFamiliesEnabled()) return;
    PreserveErrno keep;
    AnimThreadState& s = AnimThread;
    if (!AnimFamilyActive(s)) return;
    if (!s.familyDepth || s.familyStack[s.familyDepth - 1] != ticket) { ++s.familyCensus[FamilyOrphan]; return; }
    if (--s.familyDepth || s.bad) return;
    const int64_t now = port_host_time_ns();
    constexpr int64_t maxNs = static_cast<int64_t>(UINT32_MAX) * 1000;
    if (s.familyStart <= 0 || now < s.familyStart || now - s.familyStart > maxNs - s.familyRootNs) {
        s.bad = true; ++s.familyCensus[FamilyClockInvalid]; return;
    }
    const int64_t ns = now - s.familyStart;
    if (!EndAnimMaterial(s, now)) return;
    const unsigned row = static_cast<unsigned>(s.familyTarget) * 4 + s.familyKind;
    ++s.familyCalls[row]; s.familyNs[row] += ns;
    s.familyRootNs += ns; s.familyLastEnd = now;
}
inline uint32_t BeginAnim() {
    if (!AnimEnabled()) return 0;
    PreserveErrno keep;
    AnimThreadState& s = AnimThread;
    if (!AnimActive(s)) return 0;
    if (s.parent) { s.bad = true; return 0; }
    s.parent = AnimTicket();
    if (!s.parent) { ++s.census[AnimTicketOverflow]; return 0; }
    ++s.census[AnimStarted];
    s.bad = false; s.depth = 0; s.categoryNs = 0;
    for (auto& n : s.categoryCalls) n = 0;
    for (auto& n : s.categoryWall) n = 0;
    if (AnimFamiliesEnabled()) {
        s.familyDepth = 0; s.familyTarget = -1;
        for (auto& n : s.familyCalls) n = 0;
        for (auto& n : s.familyNs) n = 0;
    }
    if (AnimMaterialEnabled()) {
        for (auto& n : s.materialNs) n = 0;
        for (auto& n : s.materialCalls) n = 0;
        for (auto& n : s.materialParentDiffs) n = 0;
    }
    s.parentWall = port_host_time_ns();
    return s.parent;
}
inline uint32_t BeginAnimCategory(unsigned domain, unsigned category) {
    if (!AnimEnabled()) return 0;
    PreserveErrno keep;
    AnimThreadState& s = AnimThread;
    if (!AnimActive(s) || !s.parent || s.bad) return 0;
    if (s.depth == sizeof(s.stack) / sizeof(*s.stack)) { s.bad = true; ++s.census[AnimDepthOverflow]; return 0; }
    const uint32_t ticket = AnimTicket();
    if (!ticket) { s.bad = true; ++s.census[AnimTicketOverflow]; return 0; }
    if (!s.depth) {
        s.rootRow = AnimCategory(domain, category);
        s.rootWall = port_host_time_ns();
        s.census[AnimUnknownDispatch] += s.rootRow == AnimUnknown;
        if (AnimFamiliesEnabled()) {
            s.familyTarget = FamilyTarget(s.rootRow);
            s.familyRootNs = 0; s.familyLastEnd = s.rootWall;
        }
    } else ++s.census[AnimNested];
    s.stack[s.depth++] = ticket;
    return ticket;
}
inline void EndAnimCategory(uint32_t ticket) {
    if (!ticket || !AnimEnabled()) return;
    PreserveErrno keep;
    AnimThreadState& s = AnimThread;
    if (!AnimActive(s)) return;
    if (!s.parent || !s.depth || s.stack[s.depth - 1] != ticket) { ++s.census[AnimOrphan]; return; }
    if (--s.depth) return;
    AbandonAnimFamily(s);
    if (s.bad) return;
    const int64_t now = port_host_time_ns();
    constexpr int64_t maxNs = static_cast<int64_t>(UINT32_MAX) * 1000;
    if (s.rootWall <= 0 || now < s.rootWall || now - s.rootWall > maxNs - s.categoryNs) { s.bad = true; return; }
    const int64_t ns = now - s.rootWall;
    if (AnimFamiliesEnabled() && s.familyTarget >= 0 && (s.familyLastEnd > now || s.familyRootNs > ns)) {
        s.bad = true; ++s.familyCensus[FamilyClockInvalid]; return;
    }
    ++s.categoryCalls[s.rootRow];
    s.categoryWall[s.rootRow] += ns;
    s.categoryNs += ns;
}
inline void EndAnim(uint32_t ticket) {
    if (!ticket || !AnimEnabled()) return;
    PreserveErrno keep;
    AnimThreadState& s = AnimThread;
    if (!AnimActive(s)) return;
    if (!s.parent || s.parent != ticket) { ++s.census[AnimOrphan]; return; }
    AbandonAnimFamily(s);
    const int64_t now = s.bad ? 0 : port_host_time_ns();
    constexpr int64_t maxNs = static_cast<int64_t>(UINT32_MAX) * 1000;
    if (s.bad || s.depth || s.parentWall <= 0 || now < s.parentWall || now - s.parentWall > maxNs || s.categoryNs > now - s.parentWall) {
        ++s.census[AnimInvalid];
    } else {
        ++s.census[AnimComplete];
        const uint32_t parentUs = static_cast<uint32_t>((now - s.parentWall) / 1000);
        uint32_t childUs = 0;
        for (unsigned i = AnimEnvironment; i < AnimRowCount; ++i) {
            const uint32_t us = static_cast<uint32_t>(s.categoryWall[i] / 1000);
            s.rows[i].calls += s.categoryCalls[i]; s.rows[i].wallSamples += s.categoryCalls[i]; s.rows[i].wallUs += us;
            childUs += us;
        }
        ++s.rows[AnimParent].calls; ++s.rows[AnimParent].wallSamples; s.rows[AnimParent].wallUs += parentUs;
        ++s.rows[AnimResidual].calls; ++s.rows[AnimResidual].wallSamples;
        s.rows[AnimResidual].wallUs += parentUs - childUs; // Includes sub-microsecond category truncation; exact emitted partition.
        if (AnimFamiliesEnabled()) {
            for (unsigned target = 0; target < 2; ++target) {
                const unsigned category = target ? AnimMapObj : AnimNpc, first = target * 4;
                uint32_t familyUs = 0;
                for (unsigned k = 0; k < 3; ++k) {
                    const unsigned row = first + k;
                    const uint32_t us = static_cast<uint32_t>(s.familyNs[row] / 1000);
                    s.families[row].calls += s.familyCalls[row]; s.families[row].wallSamples += s.familyCalls[row]; s.families[row].wallUs += us;
                    familyUs += us;
                }
                const unsigned residual = first + 3;
                s.families[residual].calls += s.categoryCalls[category]; s.families[residual].wallSamples += s.categoryCalls[category];
                s.families[residual].wallUs += static_cast<uint32_t>(s.categoryWall[category] / 1000) - familyUs;
            }
        }
        if (AnimMaterialEnabled()) {
            for (unsigned target = 0; target < 2; ++target) {
                const unsigned first = target * 5;
                const uint32_t totalUs = static_cast<uint32_t>(s.familyNs[target * 4 + 2] / 1000);
                uint32_t precedingUs = 0;
                for (unsigned phase = 0; phase < 5; ++phase) {
                    const unsigned row = first + phase;
                    // Keep the existing material-family total exact; cleanup absorbs phase truncation.
                    const uint32_t us = phase == 4 ? totalUs - precedingUs : static_cast<uint32_t>(s.materialNs[row] / 1000);
                    s.materials[row].calls += s.materialCalls[row]; s.materials[row].wallSamples += s.materialCalls[row]; s.materials[row].wallUs += us;
                    precedingUs += us;
                }
            }
            for (unsigned i = 0; i < MaterialDiffCount; ++i) s.diffs[i] += s.materialParentDiffs[i];
        }
    }
    s.parent = s.depth = 0;
}
inline void ReportAnim() {
    if (!AnimEnabled()) return;
    AnimSnapshot now;
    if (!ReadAnimSnapshot(now)) { ++AnimSkippedReports; return; }
    uint32_t d[AnimCensusCount];
    for (unsigned i = 0; i < AnimCensusCount; ++i) d[i] = now.census[i] - AnimPrevious.census[i];
    port_log_summary("workload-anim: stride=%u selected_turns=%u drawn=%u skipped=%u unknown=%u parent_started=%u complete=%u invalid=%u unfinished=%u nested_dispatches=%u unknown_dispatches=%u orphan_ends=%u depth_overflow=%u ticket_exhausted=%u snapshot_skips=%u; complete-parent wall partition, residual includes sub-us row truncation; nested work stays in owning category; no CPU timing",
        AnimStride(), d[AnimSelected], d[AnimDrawn], d[AnimSkipped], d[AnimUnknownTurn], d[AnimStarted], d[AnimComplete], d[AnimInvalid], d[AnimUnfinished],
        d[AnimNested], d[AnimUnknownDispatch], d[AnimOrphan], d[AnimDepthOverflow], d[AnimTicketOverflow], AnimSkippedReports);
    for (unsigned i = 0; i < AnimRowCount; ++i) {
        const Totals r = Delta(now.rows[i], AnimPrevious.rows[i]);
        port_log_summary("workload-anim: %s calls=%u wall_samples=%u wall_us=%u cpu_samples=0 matched_wall_us=0 cpu_us=0",
            AnimNames[i], r.calls, r.wallSamples, r.wallUs);
    }
    if (AnimFamiliesEnabled()) {
        uint32_t f[FamilyCensusCount];
        for (unsigned i = 0; i < FamilyCensusCount; ++i) f[i] = now.familyCensus[i] - AnimPrevious.familyCensus[i];
        port_log_summary("workload-anim-family: selected_turns=%u complete_parents=%u nested=%u unknown_ids=%u orphan_ends=%u unfinished=%u depth_overflow=%u ticket_exhausted=%u clock_invalid=%u snapshot_skips=%u; same category snapshot; NPC/Map roots only; outer families disjoint, residual includes unhooked work and sub-us truncation; no CPU timing",
            d[AnimSelected], d[AnimComplete], f[FamilyNested], f[FamilyUnknown], f[FamilyOrphan], f[FamilyUnfinished], f[FamilyDepthOverflow], f[FamilyTicketOverflow], f[FamilyClockInvalid], AnimSkippedReports);
        for (unsigned target = 0; target < 2; ++target) {
            const unsigned category = target ? AnimMapObj : AnimNpc;
            const Totals total = Delta(now.rows[category], AnimPrevious.rows[category]);
            port_log_summary("workload-anim-family: %s-total calls=%u wall_samples=%u wall_us=%u cpu_samples=0 matched_wall_us=0 cpu_us=0",
                target ? "map-objects" : "npc", total.calls, total.wallSamples, total.wallUs);
            for (unsigned k = 0; k < 4; ++k) {
                const unsigned row = target * 4 + k;
                const Totals value = Delta(now.families[row], AnimPrevious.families[row]);
                port_log_summary("workload-anim-family: %s calls=%u wall_samples=%u wall_us=%u cpu_samples=0 matched_wall_us=0 cpu_us=0",
                    FamilyNames[row], value.calls, value.wallSamples, value.wallUs);
            }
        }
    }
    if (AnimMaterialEnabled()) {
        uint32_t m[MaterialCensusCount];
        for (unsigned i = 0; i < MaterialCensusCount; ++i) m[i] = now.materialCensus[i] - AnimPrevious.materialCensus[i];
        port_log_summary("workload-anim-material: selected_turns=%u complete_parents=%u nested_marks=%u bad_token=%u bad_phase=%u clock_invalid=%u unfinished=%u census_invalid=%u snapshot_skips=%u; same material-family snapshot; phase calls are segments; cleanup includes sub-us phase correction; no CPU timing",
            d[AnimSelected], d[AnimComplete], m[MaterialNestedMarks], m[MaterialBadToken], m[MaterialBadPhase], m[MaterialClockInvalid], m[MaterialUnfinished], m[MaterialCensusInvalid], AnimSkippedReports);
        for (unsigned target = 0; target < 2; ++target) {
            const char* name = target ? "map-objects" : "npc";
            const Totals total = Delta(now.families[target * 4 + 2], AnimPrevious.families[target * 4 + 2]);
            port_log_summary("workload-anim-material: %s-total calls=%u wall_samples=%u wall_us=%u cpu_samples=0 matched_wall_us=0 cpu_us=0",
                name, total.calls, total.wallSamples, total.wallUs);
            for (unsigned phase = 0; phase < 5; ++phase) {
                const unsigned row = target * 5 + phase;
                const Totals value = Delta(now.materials[row], AnimPrevious.materials[row]);
                port_log_summary("workload-anim-material: %s calls=%u wall_samples=%u wall_us=%u cpu_samples=0 matched_wall_us=0 cpu_us=0",
                    MaterialNames[row], value.calls, value.wallSamples, value.wallUs);
            }
            const unsigned first = target * 4;
            port_log_summary("workload-anim-material: %s-diff-census invocations=%u scanned=%u flagged=%u material_calls=%u; call expression reached, not proof of emitted work",
                name, now.diffs[first] - AnimPrevious.diffs[first], now.diffs[first + 1] - AnimPrevious.diffs[first + 1],
                now.diffs[first + 2] - AnimPrevious.diffs[first + 2], now.diffs[first + 3] - AnimPrevious.diffs[first + 3]);
        }
    }
    AnimPrevious = now;
    AnimSkippedReports = 0;
}

class Scope {
    Lane lane;
    bool active, inTurn = false;
    uint32_t generation = 0;
    Reading begin;
public:
    explicit Scope(Lane which, bool selected = true) : lane(which), active(selected && Enabled()) {
        if (active) {
            inTurn = Thread.inTurn;
            generation = Thread.generation;
            if ((lane == PublishInTurn || lane == FinishInTurn) && !inTurn) active = false;
            else begin = ReadClock();
        }
    }
    ~Scope() {
        if (!active) return;
        const Reading end = ReadClock();
        if ((lane == PublishInTurn || lane == FinishInTurn) && (!inTurn || !Thread.inTurn || generation != Thread.generation)) return;
        Counters[lane].Add(begin, end);
        if (lane == Dsp && inTurn && Thread.inTurn && generation == Thread.generation) Counters[DspInTurn].Add(begin, end);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
inline void BeginTurn() {
    if (!Enabled()) return;
    ++Thread.generation;
    Thread.inTurn = true;
    Thread.inFinish = false;
    Thread.turn = ReadClock();
#if defined(__vita__) && PORT_LOGGING
    AnimBeginTurn();
#endif
}
inline void FinishWait(bool waiting) {
    if (!Enabled() || !Thread.inTurn || waiting == Thread.inFinish) return;
    if (waiting) Thread.finish = ReadClock();
    else Counters[FinishInTurn].Add(Thread.finish, ReadClock());
    Thread.inFinish = waiting;
}
inline void EndTurn() {
    if (!Enabled() || !Thread.inTurn) return;
#if defined(__vita__) && PORT_LOGGING
    AnimEndTurn();
#endif
    FinishWait(false);
    Counters[Turn].Add(Thread.turn, ReadClock());
    Thread.inTurn = false;
}
inline bool SampleProcess() {
    if (!Enabled()) return false;
    ProcessCalls.fetch_add(1, std::memory_order_relaxed);
    return Thread.processCalls++ % Stride() == 0;
}
inline void FrontPicture() {
    if (!Enabled()) return;
    const Reading now = ReadClock();
    if (Thread.frontStarted) Counters[FrontFrame].Add(Thread.front, now);
    Thread.front = now;
    Thread.frontStarted = true;
}

struct PictureCounts {
    uint32_t pictures = 0, drawCommands = 0, textureCopies = 0, displayCopies = 0, passBegins = 0;
    uint32_t backendScenes = 0, copyQuads = 0, fxCandidates = 0, reducedFxRuns = 0;
    uint32_t candidateRuns = 0, candidateVerts = 0, runs400 = 0, runs800 = 0;
};
// Render thread only: command/call census, not measured driver stores/reloads or GPU time.
inline PictureCounts Pictures, PreviousPictures;
inline uint32_t MaxDrawCommands = 0, MaxBackendScenes = 0, MaxCandidateVerts = 0;
inline void CandidateRun(uint32_t vertices) {
    if (!Enabled()) return;
    ++Pictures.candidateRuns;
    Pictures.candidateVerts += vertices;
    Pictures.runs400 += vertices >= 400;
    Pictures.runs800 += vertices >= 800;
    if (vertices > MaxCandidateVerts) MaxCandidateVerts = vertices;
}
// GXV_FXAREA (with GXV_WORKLOAD_PROBE=1, vgx_backend_gl.cpp FxDecide): how much of the picture the blended draws of a sampled picture
// cover, from their own vertices, in screens (the EFB's pixels): overdraw of transparent effects is the GPU's fill work, which the draw
// counts above do not show. An estimate: triangles clipped to the viewport, a large draw measured on a sample of its triangles.
struct AreaCounts {
    uint32_t pictures = 0;            // pictures measured
    uint64_t blendMilli = 0;          // blended draws outside offscreen passes, colour written: screens x 1000
    uint64_t addMilli = 0;            // of them the additive effect candidates (what GXV_FXHALF can draw at a reduced size)
    uint32_t unknown = 0;             // blended draws whose area is not known here (static buffers, palette positions, a new shader)
    uint32_t runs2 = 0, runs4 = 0;    // additive runs of at least 2 / 4 screens
};
inline AreaCounts Areas, PreviousAreas;
inline uint32_t MaxPictureBlendMilli = 0, MaxRunAreaMilli = 0;
inline void AreaPicture(uint32_t blendMilli, uint32_t addMilli, uint32_t unknown) {
    ++Areas.pictures;
    Areas.blendMilli += blendMilli;
    Areas.addMilli += addMilli;
    Areas.unknown += unknown;
    if (blendMilli > MaxPictureBlendMilli) MaxPictureBlendMilli = blendMilli;
}
inline void AreaRun(uint32_t milli) {
    Areas.runs2 += milli >= 2000;
    Areas.runs4 += milli >= 4000;
    if (milli > MaxRunAreaMilli) MaxRunAreaMilli = milli;
}
inline void Picture(uint32_t draws, uint32_t copies, uint32_t displays, uint32_t passes,
                    uint32_t scenes, uint32_t quads, uint32_t candidates, uint32_t reducedRuns) {
    if (!Enabled()) return;
    ++Pictures.pictures;
    Pictures.drawCommands += draws; Pictures.textureCopies += copies; Pictures.displayCopies += displays; Pictures.passBegins += passes;
    Pictures.backendScenes += scenes; Pictures.copyQuads += quads; Pictures.fxCandidates += candidates; Pictures.reducedFxRuns += reducedRuns;
    if (draws > MaxDrawCommands) MaxDrawCommands = draws;
    if (scenes > MaxBackendScenes) MaxBackendScenes = scenes;
}
// GXV_FE_SPLIT=1 (with GXV_WORKLOAD_PROBE=1): where the front end's time goes, by wall time on its own thread (no waits inside
// these scopes): the top-level command stream, the immediate draws (decode + record), cached display lists, and the draws of them.
// GXV_FE_SPLIT=2: the same line, with every scope counted but only about one in 16 of them timed (at random, per scope, decided
// when it opens), each slot's time scaled up by its scopes / its timed scopes. A timed scope costs two clock reads (sceKernelGet-
// ProcessTimeWide, ~2.6 us on the console), and with =1 the ~4,000 scopes of a lit Comet Observatory picture add more than they
// measure (the front end's own CPU rises by half); =2 adds about a sixteenth of that, so the shares and the absolute times are those
// of the real picture. The counts in brackets are exact either way.
// (fx-merge: the port-private particle commands of GXV_FXMERGE, vgx_state.cpp FxMergeRun, whole; their draws are in "draw" as well)
enum FeSlot : unsigned { FeStream, FeDraw, FeDecode, FeEmit, FeList, FeStatic, FeTiny, FeMatHit, FeGeomHit, FeOther, FeResolve, FeSend, FeFxMerge, FeSlotCount };
inline constexpr const char* FeNames[] = {"stream", "draw", "decode", "emit", "list", "static-draw", "list-tiny", "list-material-hit", "list-geometry-hit", "list-other", "resolve-state", "finish-or-send", "fx-merge"};
// (one knob, the probe's switch and its hooks folded in: every FeScope reads it, some 6,500 a picture of the lit Comet Observatory)
inline int FeSplitMode() {
    return static_cast<int>(VGX_KNOB([] {
        PreserveErrno keep;
        const long v = EnvInt("GXV_FE_SPLIT", 0);
        return Enabled() && (v == 1 || v == 2) ? static_cast<int>(v) : 0;
    }()));
}
inline bool FeSplitOn() { return FeSplitMode() != 0; }
// (GXV_FE_SPLIT=2) whether the scope being opened is timed: about one in 16, from a per-thread linear congruential sequence
// (one sequence for all threads, a relaxed word: a thread_local is a call of __emutls_get_address per scope on the console (-femulated-tls);
// scopes are opened on the player thread, and another thread's draw only reshuffles the choice)
inline std::atomic<uint32_t> FeSampleSeed{0x9E3779B9u};
inline bool FeTimedScope() {
    if (FeSplitMode() != 2) return true;
    const uint32_t s = FeSampleSeed.load(std::memory_order_relaxed) * 1664525u + 1013904223u;
    FeSampleSeed.store(s, std::memory_order_relaxed);
    return (s >> 28) == 0;
}
// (GXV_FE_SPLIT) dynamic draws by whether they joined the record before them, and if not the first merge test that failed
enum MergeWhy : unsigned { MergeJoined, MergeNoPrev, MergeStatic, MergeLayout, MergeShader, MergeVerts, MergeBlocks, MergeTex, MergeRs, MergeOther, MergeWhyCount };
inline constexpr const char* MergeNames[] = {"joined", "no-prev-draw", "prev-static", "prim-layout", "shader", "vertices", "uniform-blocks", "textures", "render-state", "other"};
inline std::atomic<uint32_t> MergeCount[MergeWhyCount];
inline uint32_t MergePrev[MergeWhyCount];
inline std::atomic<uint64_t> FeNs[FeSlotCount];
inline std::atomic<uint32_t> FeN[FeSlotCount];
inline std::atomic<uint32_t> FeNTimed[FeSlotCount]; // (the scopes FeNs holds the time of: all of them with =1)
inline uint64_t FePrevNs[FeSlotCount];
inline uint32_t FePrevN[FeSlotCount];
inline uint32_t FePrevNTimed[FeSlotCount];
// (the caller may move an open scope to another slot: CallList does once its branch is known; the time and the count go to the slot it
// has when it closes. timed: t holds the opening time.)
// (with the breakdown off a scope is a load and a compare, inline: the opening and closing work is out of line)
struct FeScope {
    int slot = -1; bool timed = false; int64_t t = 0;
    explicit __attribute__((always_inline)) FeScope(unsigned s) { if (__builtin_expect(FeSplitMode() != 0, 0)) Open(s); }
    __attribute__((always_inline)) ~FeScope() { if (__builtin_expect(slot >= 0, 0)) Close(); }
    __attribute__((noinline)) void Open(unsigned s) {
        slot = static_cast<int>(s);
        timed = FeTimedScope();
        t = timed ? port_host_time_ns() : 0;
    }
    __attribute__((noinline)) void Close() {
        if (timed) {
            FeNs[slot].fetch_add(static_cast<uint64_t>(port_host_time_ns() - t), std::memory_order_relaxed);
            FeNTimed[slot].fetch_add(1, std::memory_order_relaxed);
        }
        FeN[slot].fetch_add(1, std::memory_order_relaxed);
    }
};
inline Totals Previous[LaneCount];
inline int64_t WindowStart = 0;
inline uint32_t PreviousProcessCalls = 0;
inline void Report() {
    if (!Enabled()) return;
    const int64_t now = port_host_time_ns();
    if (now <= 0) return;
    if (!WindowStart || now < WindowStart) {
        WindowStart = now;
        for (unsigned i = 0; i < LaneCount; ++i) Previous[i] = Counters[i].Read();
        PreviousPictures = Pictures;
        PreviousAreas = Areas;
        PreviousProcessCalls = ProcessCalls.load(std::memory_order_relaxed);
        MaxDrawCommands = MaxBackendScenes = MaxCandidateVerts = 0;
#if defined(__vita__) && PORT_LOGGING
        if (AnimEnabled()) ReadAnimSnapshot(AnimPrevious);
#endif
        return;
    }
    if (now - WindowStart < 5000000000ll) return;
    const uint32_t processCalls = ProcessCalls.load(std::memory_order_relaxed);
    port_log_summary("workload: %.2f s; stride=%u process_calls=%u; rolling asynchronous windows; subset/overlapping rows are NOT additive; CPU is calling-thread time, wall includes waits",
                     (now - WindowStart) / 1e9, Stride(), processCalls - PreviousProcessCalls);
    PreviousProcessCalls = processCalls;
    for (unsigned i = 0; i < LaneCount; ++i) {
        const Totals total = Counters[i].Read(), d = Delta(total, Previous[i]);
        Previous[i] = total;
        port_log_summary("workload: %s calls=%u wall_samples=%u wall_us=%u cpu_samples=%u matched_wall_us=%u cpu_us=%u",
                         Names[i], d.calls, d.wallSamples, d.wallUs, d.cpuSamples, d.matchedWallUs, d.cpuUs);
    }
    const PictureCounts p = Pictures, old = PreviousPictures;
    port_log_summary("workload: census pictures=%u draw_commands=%u max_draw_commands=%u texture_copy_commands=%u display_copy_commands=%u pass_begin_commands=%u backend_target_scenes=%u max_backend_target_scenes=%u copy_quads=%u fx_candidate_draws=%u reduced_fx_runs=%u",
                     p.pictures-old.pictures, p.drawCommands-old.drawCommands, MaxDrawCommands, p.textureCopies-old.textureCopies,
                     p.displayCopies-old.displayCopies, p.passBegins-old.passBegins, p.backendScenes-old.backendScenes, MaxBackendScenes,
                     p.copyQuads-old.copyQuads, p.fxCandidates-old.fxCandidates, p.reducedFxRuns-old.reducedFxRuns);
    port_log_summary("workload: fx_candidate_runs=%u vertices=%u max_run_vertices=%u runs_ge400=%u runs_ge800=%u; census is NOT GPU timing or measured GXM store/reload traffic; ProcessStream is sampled-only, not the entire front end",
                     p.candidateRuns-old.candidateRuns, p.candidateVerts-old.candidateVerts, MaxCandidateVerts, p.runs400-old.runs400, p.runs800-old.runs800);
    {
        const AreaCounts& a = Areas;
        const AreaCounts& b = PreviousAreas;
        const uint32_t n = a.pictures - b.pictures;
        if (n) {
            port_log_summary("workload: fx area (GXV_FXAREA, estimate from vertices): sampled_pictures=%u blended_screens_per_picture=%.2f max_picture_blended=%.2f "
                             "additive_screens_per_picture=%.2f max_additive_run=%.2f additive_runs_ge2=%u runs_ge4=%u unknown_blended_draws=%u",
                             n, (a.blendMilli - b.blendMilli) / 1000.0 / n, MaxPictureBlendMilli / 1000.0, (a.addMilli - b.addMilli) / 1000.0 / n,
                             MaxRunAreaMilli / 1000.0, a.runs2 - b.runs2, a.runs4 - b.runs4, a.unknown - b.unknown);
        }
        PreviousAreas = a;
        MaxPictureBlendMilli = MaxRunAreaMilli = 0;
    }
    if (&DrawReplayCounters5) {
        // (GXV_DRAWREPLAY, vgx_dlcache.cpp: list calls of a picture that repeated the previous picture's and went straight to their cache
        // entry; "failed": a record was found but a test sent the call the normal way; "realigned": found away from the cursor)
        static uint64_t prevRp[5] = {};
        uint64_t rp[5];
        DrawReplayCounters5(rp);
        const uint32_t pics = p.pictures - old.pictures ? p.pictures - old.pictures : 1;
        if (rp[4] != prevRp[4])
            port_log_summary("workload: draw replay per picture: %.1f list calls, %.1f replayed, %.1f failed, %.1f realigned, %.1f recorded the normal way",
                             (rp[4]-prevRp[4])/(double)pics, (rp[0]-prevRp[0])/(double)pics, (rp[1]-prevRp[1])/(double)pics,
                             (rp[2]-prevRp[2])/(double)pics, (rp[3]-prevRp[3])/(double)pics);
        for (int i = 0; i < 5; ++i) prevRp[i] = rp[i];
    }
    if (&split::KeyIdCounters && &FeCutCounters) {
        // (GXV_FECUT_*, see REPORT of the fecut change: shader keys sent to the finishing thread whole / by id (GXV_FECUT_KEYID); material
        // lists replayed from the entry's first cache line, and their deltas applied in the compact layout (GXV_FECUT_SLHOT))
        static uint64_t prevK[4] = {}, prevC[4] = {};
        uint64_t k[4], c[4];
        split::KeyIdCounters(k);
        FeCutCounters(c);
        const uint32_t pics = p.pictures - old.pictures ? p.pictures - old.pictures : 1;
        if (k[0] + k[1] + k[2] + k[3] + c[0] + c[1] != prevK[0] + prevK[1] + prevK[2] + prevK[3] + prevC[0] + prevC[1])
            port_log_summary("workload: fe cuts per picture: fragment keys sent whole %.1f, by id %.1f; vertex keys whole %.1f, by id %.1f; "
                             "material hits %.1f, compact deltas applied %.1f",
                             (k[0]-prevK[0])/(double)pics, (k[1]-prevK[1])/(double)pics, (k[2]-prevK[2])/(double)pics, (k[3]-prevK[3])/(double)pics,
                             (c[0]-prevC[0])/(double)pics, (c[1]-prevC[1])/(double)pics);
        for (int i = 0; i < 4; ++i) { prevK[i] = k[i]; prevC[i] = c[i]; }
    }
    if (&FxMergeCounters) {
        // (GXV_FXMERGE, vgx_state.cpp FxMergeRun: the particle commands the game gathered, the display-list calls in them, how many of
        // those went the short way (one quad list, its decode kept), how many were decoded from the list's bytes again, how many
        // went through CallList as before)
        static uint64_t prevFx[6] = {};
        uint64_t fx[6];
        FxMergeCounters(fx);
        const uint32_t pics = p.pictures - old.pictures ? p.pictures - old.pictures : 1;
        if (fx[0] != prevFx[0])
            port_log_summary("workload: fx-merge per picture: %.1f commands, %.1f items, %.1f list calls, %.1f short, %.1f of them decode reused, %.1f through CallList",
                             (fx[0]-prevFx[0])/(double)pics, (fx[1]-prevFx[1])/(double)pics, (fx[2]-prevFx[2])/(double)pics,
                             (fx[3]-prevFx[3])/(double)pics, (fx[4]-prevFx[4])/(double)pics, (fx[5]-prevFx[5])/(double)pics);
        for (int i = 0; i < 6; ++i) prevFx[i] = fx[i];
    }
    if (FeSplitOn()) {
        const uint32_t pics = p.pictures - old.pictures ? p.pictures - old.pictures : 1;
        char line[900]; int at = 0;
        for (unsigned i = 0; i < FeSlotCount; ++i) {
            const uint64_t ns = FeNs[i].load(std::memory_order_relaxed); const uint32_t n = FeN[i].load(std::memory_order_relaxed);
            const uint32_t nt = FeNTimed[i].load(std::memory_order_relaxed);
            // (GXV_FE_SPLIT=2: the timed scopes' time, scaled by all scopes / timed scopes of the slot in this window)
            const double scale = nt - FePrevNTimed[i] ? static_cast<double>(n - FePrevN[i]) / static_cast<double>(nt - FePrevNTimed[i]) : 0.0;
            at += snprintf(line + at, sizeof line - at, " %s=%.2fms(%u)", FeNames[i], (ns - FePrevNs[i]) * scale / 1e6 / pics, (n - FePrevN[i]) / pics);
            FePrevNs[i] = ns; FePrevN[i] = n; FePrevNTimed[i] = nt;
        }
        port_log_summary("workload: fe-split per picture (wall, front-end thread; calls per picture in brackets%s):%s",
                         FeSplitMode() == 2 ? "; about 1 scope in 16 timed, scaled" : "", line);
        static uint64_t prevDl[7] = {};
        uint64_t dl[7];
        if (&DlCacheBranchCounts) DlCacheBranchCounts(dl); else for (auto& v : dl) v = 0;
        port_log_summary("workload: dl-cache per picture: hits %.1f, rehash-same %.1f, rehash-changed %.1f, bypassed %.1f, builds %.1f, uncacheable %.1f, promoted %.1f",
                         (dl[0]-prevDl[0])/(double)pics, (dl[1]-prevDl[1])/(double)pics, (dl[2]-prevDl[2])/(double)pics, (dl[3]-prevDl[3])/(double)pics,
                         (dl[4]-prevDl[4])/(double)pics, (dl[5]-prevDl[5])/(double)pics, (dl[6]-prevDl[6])/(double)pics);
        for (int i = 0; i < 7; ++i) prevDl[i] = dl[i];
        // (GXV_SLFAST, vgx_state.cpp StateListFastHit: material lists replayed without the geometry cache's detour; "via-table" and
        // "same-bytes" are the ones that went through it before, as "bypassed" calls)
        static uint64_t prevSl[3] = {};
        uint64_t sl[3];
        if (&StateListFastCounters) StateListFastCounters(sl); else for (auto& v : sl) v = 0;
        port_log_summary("workload: state-list fast hits per picture: via-front %.1f, via-table %.1f, of them same-bytes rewrites %.1f",
                         (sl[0]-prevSl[0])/(double)pics, (sl[1]-prevSl[1])/(double)pics, (sl[2]-prevSl[2])/(double)pics);
        for (int i = 0; i < 3; ++i) prevSl[i] = sl[i];
        char mline[400]; int mat = 0;
        for (unsigned i = 0; i < MergeWhyCount; ++i) {
            const uint32_t n = MergeCount[i].load(std::memory_order_relaxed);
            mat += snprintf(mline + mat, sizeof mline - mat, " %s=%.1f", MergeNames[i], (n - MergePrev[i]) / (double)pics);
            MergePrev[i] = n;
        }
        port_log_summary("workload: merges per picture (dynamic draws):%s", mline);
    }
#if defined(__vita__) && PORT_LOGGING
    ReportAnim();
#endif
    PreviousPictures = p;
    MaxDrawCommands = MaxBackendScenes = MaxCandidateVerts = 0;
    WindowStart = now;
}
} // namespace vgx::workload
