#include "Game/LiveActor/ShadowController.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/LiveActor/ShadowDrawer.hpp"
#include "Game/Map/HitInfo.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/GravityUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/StringUtil.hpp"
#ifdef TARGET_PC
extern "C" float port_pb_shadow_clip(void);
extern "C" int port_cc_flag;  // (far30: bit 1 = GXV_DISTCENSUS)
extern "C" void port_dist_note_shadow(float x, float y, float z);
extern "C" float port_pb_player_dist2(float x, float y, float z);  // (decomp/src/Game/LiveActor/LodCtrl.cpp)
extern "C" float port_detail_dist(int rule, float base);  // (detail25)
extern "C" float port_popin_zoom(void);  // (popin24: LodCtrl.cpp)
extern "C" float port_pb_scale(void);    // (popin25: LodCtrl.cpp; the zoom ratio times GXV_PB_DEMO's widening in a flight)
extern "C" int port_pb_demo_off(void);   // (popin25: GXV_PB_DEMO=2 in a flight: the budget is off)
extern "C" float port_pb_lead(float limit);  // (popin25: GXV_POPIN_LEAD: appear earlier for a fast camera)
extern "C" void port_popin_event(int kind, int appear, const LiveActor* pActor, const TVec3f* pPos, float radius);
#include "port/popin.h"
#include "port/popin_site.h"
#include "port/honey_shadow.h"
extern "C" int port_pb_budget_on(void);
extern "C" void port_logic_count(int what, unsigned n);  // (logic12: logging builds' per-turn counts, platform/src/port/perf.cpp)
#include "Game/Map/CollisionCategorizedKeeper.hpp"
#include "Game/Map/CollisionDirector.hpp"
#include "Game/Map/CollisionParts.hpp"
#include "Game/Util/CollisionPartsFilter.hpp"
#include "port/diagnostics.h"
#include "port/host_vector.h"
#include "port/hot_knob.h"
#include "port/shadow_memo.h"
#include "port/shadow_memo_hooks.h"
#include <cstring>
extern "C" int64_t port_host_time_ns(void);
// GXV_UNDRAWN_LEAN (undrawn26, platform/src/port/perf.cpp, port/undrawn_lean.h): a controller that no logic reads is cast once before the picture
// (port_shadow_flush_pending, GameSystem::frameLoop) instead of at the end of every turn. Exact: the cast is a function of the actor's state and the
// world, none of which changes between the end of a turn and the start of the next one; the flush casts the state of the last turn, which is the
// cast the original makes last. Not for counted casts (one-time modes) and not in the Performance budget (its far flag has a hysteresis state).
extern "C" int port_undrawn_lean_shadows(void);
extern "C" int port_undrawn_shadow_decide(const void* ctrl, const char* owner, int logicRead, unsigned collMode, unsigned gravMode, int pb);
namespace {
bool sLeanFlushing = false;
ShadowController* sLeanKeep[0x500];
ShadowController* sLeanRun[0x500];
}  // namespace

// GXV_SHADOWMEMO (shadowmemo, 6 Nov; default 1): ShadowControllerHolder's projection casts (one line cast against the map or the
// water surface per shadow per turn) reuse the last turn's answer when the cast's arguments are bit-identical and nothing it
// could hit has changed since (port/shadow_memo.h has the rule and why it is exact). Static shadows (items, flowers, map objects,
// standing NPCs) then cost a compare instead of a cast. The answer is still read every turn by game code (isShadowProjected,
// getShadowProjectionLength, ...), so it is kept current exactly, not computed lazily.
// shadowmemo2 (r89: Bowser's Star Reactor -3 fps): the changes come from write-side hooks (port/shadow_memo_hooks.h) instead of a
// walk of every collision part per turn; a reused memo moves its log position to now (it looks only at changes since its own last
// look, each through a box prefilter); a memo more than 256 changes behind is cast again. Adaptive gate (per stage): every 300
// turns the memo's own cost (lookups, timed on a sample) is set against what it saved (reuses x the sampled cast cost); at 70 % or
// more it is switched off (hooks disarmed, plain casts) for 1800 turns, then tried again. Transitions go to the basic log.
// GXV_SHADOWMEMO_VERIFY=1: every reused answer is also cast and compared (the cast's answer is used), and once a turn a walk of all
// collision parts audits the hooks (a change no hook reported is "unhooked"); the gate stays on. "shadowmemo:" basic-log line every
// 600 holder turns. GXV_SHADOWMEMO=0: the cast every time, as before (hooks disarmed).
namespace {
    port::popin::HoneyShadowHolds sHoneyHolds;
    bool sHoneyStage = false, sHoneyHoldBatch = false;
    u32 sFloorCostTick = 0;  // (popin31b: the sample counter of the kept shadows timed for the census)
    typedef port::shadowmemo::Snapshot< PortHostVector< port::shadowmemo::SnapEntry > > PortSnap;
    const u32 kMemoScanCap = 256;  // (more changes than this since a memo's last look: cast again)
    port::shadowmemo::Log< 1024 > sMemoLog;
    port::shadowmemo::BatchIndex< 1024 > sMemoIndex;  // (the changes since the holder's last run, on a coarse grid)
    u32 sMemoRunSeq = 0;                               // the log's position at the holder's last run
    bool sMemoActive = false;  // inside ShadowControllerHolder::updateController with the memo on
    bool sMemoVerify = false;
    const void* sFilterVtSensor = nullptr;
    const void* sFilterVtActor = nullptr;
    struct MemoStats {
        u32 runs, casts, hits, missNew, missKey, missEpoch, missLog, missCap, missOff, mismatch, sampled, lookSampled, reshapes, hooks;
        u32 auditUnhooked, auditUnchecked, auditParts;
        u64 castNs, lookNs, scanned, auditNs, indexNs;
    } sMemoStats;
    // the gate (per stage: reset when a holder is made)
    struct MemoGate {
        u32 turns, offLeft, hits, lookups, lookSampled;
        u64 lookNs, indexNs;
        double castNsAvg;  // running average of the sampled casts (ns), 0 until one is sampled
    } sGate;

    // verify's audit: the old walk, compared with what the hooks reported
    struct AuditSink {
        u32 epoch[2];
        PortHostVector< const void* > who;
        void Push(const port::shadowmemo::Change& c) { who.push_back(c.who); }
    };
    PortSnap sAuditSnap[2];
    PORT_HEAP_GUARD(sAuditSnap[0].entries);
    PORT_HEAP_GUARD(sAuditSnap[1].entries);
    AuditSink sAuditSink;
    PORT_HEAP_GUARD(sAuditSink.who);
    u32 sAuditSeq = 0;
    u32 sAuditEpoch[2] = {0, 0};
    bool sAuditPrimed = false;

    int portShadowMemoOn() {
        return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_SHADOWMEMO"); return e && *e == '0' ? 0 : 1; }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
    }
    int portShadowMemoVerify() {
        return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_SHADOWMEMO_VERIFY"); return e && *e == '1' ? 1 : 0; }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
    }

    void portShadowMemoBumpEpochs() {
        sMemoLog.epoch[0]++;
        sMemoLog.epoch[1]++;
    }

    // Verify only: walk one slot's lists (as the first version did every turn) and check every change found against the hooks'
    // log since the last audit. A reshape found is fine if an epoch bump happened since (it drops every memo).
    void portShadowMemoAuditSlot(u32 slot) {
        CollisionCategorizedKeeper* pKeeper = MR::getCollisionDirector()->getCategoryKeeper(slot == 0 ? 0 : 2);
        PortSnap& snap = sAuditSnap[slot];
        sAuditSink.who.clear();
        sAuditSink.epoch[slot] = 0;
        snap.Begin(slot);
        for (s32 zi = 0; zi < pKeeper->mZones.size(); zi++) {
            CollisionZone* pZone = pKeeper->mZones[zi];
            port::shadowmemo::ZoneState zone;
            std::memset(&zone, 0, sizeof(zone));
            zone.zone = pZone;
            zone.count = pZone->mParts.size();
            if (zi != 0) {  // (checkStrikeLine never culls zone 0)
                zone.c[0] = pZone->mCenter.x;
                zone.c[1] = pZone->mCenter.y;
                zone.c[2] = pZone->mCenter.z;
                zone.r = pZone->mRadius;
            }
            snap.Zone(sAuditSink, zone);
            for (u32 i = 0; i < zone.count; i++) {
                CollisionParts* pParts = pZone->mParts[i];
                port::shadowmemo::PartState state;
                std::memset(&state, 0, sizeof(state));
                static_assert(sizeof(pParts->mBaseMatrix) == sizeof(state.base) && sizeof(pParts->mInvBaseMatrix) == sizeof(state.inv),
                              "shadowmemo: 3x4 float matrices");
                state.parts = pParts;
                std::memcpy(state.base, &pParts->mBaseMatrix, sizeof(state.base));
                std::memcpy(state.inv, &pParts->mInvBaseMatrix, sizeof(state.inv));
                state.server = pParts->mServer;
                state.sensor = pParts->mHitSensor;
                state.radius = pParts->_D8;
                state.on = pParts->_CC ? 1 : 0;
                snap.Part(sAuditSink, state);
            }
            sMemoStats.auditParts += zone.count;
        }
        snap.End(sAuditSink);
        if (!sAuditPrimed) {
            return;
        }
        const bool epochMoved = sMemoLog.epoch[slot] != sAuditEpoch[slot];
        if (epochMoved) {
            return;  // (every memo of the slot was dropped since the last audit: whatever changed is covered)
        }
        if (sAuditSink.epoch[slot] != 0) {
            sMemoStats.auditUnhooked++;
            port_log_summary("shadowmemo: AUDIT a zone list changed with no hook (slot %u)", slot);
            return;
        }
        const u32 n = sMemoLog.seq - sAuditSeq;
        for (u32 k = 0; k < sAuditSink.who.size(); k++) {
            if (n > 1024) {
                sMemoStats.auditUnchecked++;
                continue;
            }
            bool found = false;
            for (u32 i = sAuditSeq; i != sMemoLog.seq && !found; ++i) {
                found = sMemoLog.ring[i % 1024].who == sAuditSink.who[k];
            }
            if (!found && sMemoStats.auditUnhooked++ < 8) {
                const CollisionParts* pParts = static_cast< const CollisionParts* >(sAuditSink.who[k]);
                bool isZone = false;
                for (s32 zi = 0; zi < pKeeper->mZones.size(); zi++) {
                    isZone = isZone || pKeeper->mZones[zi] == sAuditSink.who[k];
                }
                port_log_summary("shadowmemo: AUDIT unhooked change: slot %u %s %s", slot, isZone ? "zone" : "part",
                                 isZone ? "" : (pParts->getHostName() != nullptr ? pParts->getHostName() : "?"));
            }
        }
    }
    void portShadowMemoAudit() {
        const int64_t t0 = port_host_time_ns();
        portShadowMemoAuditSlot(0);
        portShadowMemoAuditSlot(1);
        sAuditSeq = sMemoLog.seq;
        sAuditEpoch[0] = sMemoLog.epoch[0];
        sAuditEpoch[1] = sMemoLog.epoch[1];
        sAuditPrimed = true;
        sMemoStats.auditNs += static_cast< u64 >(port_host_time_ns() - t0);
    }

    // The parts filter as a key: null, or one of the two filter kinds the game makes (each has one const field set at its making).
    bool portShadowMemoFilterKey(const CollisionPartsFilterBase* pFilter, const void** pVt, const void** pArg) {
        if (pFilter == nullptr) {
            *pVt = nullptr;
            *pArg = nullptr;
            return true;
        }
        const void* vt = *reinterpret_cast< const void* const* >(pFilter);
        if (vt == sFilterVtSensor) {
            *pArg = static_cast< const CollisionPartsFilterSensor* >(pFilter)->mSensor;
        } else if (vt == sFilterVtActor) {
            *pArg = static_cast< const CollisionPartsFilterActor* >(pFilter)->mActor;
        } else {
            return false;
        }
        *pVt = vt;
        return true;
    }

    // Once per holder run with the memo on (not in verify): the gate's window.
    void portShadowMemoGateTurn() {
        MemoGate& g = sGate;
        if (++g.turns < 300) {
            return;
        }
        const double n = g.turns;
        const double lookNs = g.lookSampled ? static_cast< double >(g.lookNs) / g.lookSampled : 0.0;
        const double overheadMs = (g.lookups * lookNs + g.indexNs) / n / 1e6;
        const double savedMs = g.hits * g.castNsAvg / n / 1e6;
        if (g.castNsAvg > 0.0 && overheadMs >= 0.7 * savedMs) {
            g.offLeft = 1800;
            port_log_summary("shadowmemo: gate OFF for 1800 turns: overhead %.3f ms a turn >= 70 %% of saved %.3f ms (reused %.1f a turn)",
                             overheadMs, savedMs, g.hits / n);
        }
        g.turns = g.hits = g.lookups = g.lookSampled = 0;
        g.lookNs = g.indexNs = 0;
    }

    void portShadowMemoSummary() {
        MemoStats& s = sMemoStats;
        if (++s.runs < 600) {
            return;
        }
        const double n = s.runs;
        const double castUs = s.sampled ? static_cast< double >(s.castNs) / s.sampled / 1000.0 : sGate.castNsAvg / 1000.0;
        const double lookUs = s.lookSampled ? static_cast< double >(s.lookNs) / s.lookSampled / 1000.0 : 0.0;
        const double savedMs = s.hits / n * castUs / 1000.0;
        const double overMs = s.casts / n * lookUs / 1000.0 + s.indexNs / n / 1e6;
        port_log_summary("shadowmemo: a turn: casts %.1f, reused %.1f (%.0f %%), cast %.1f us, lookup %.2f us (sampled); saved ~%.3f ms, "
                         "overhead ~%.3f ms (lookups + index %.3f ms), net ~%.3f ms; hooked changes %.1f, scanned %.1f; reshapes %u; misses new/key/epoch/log/cap/"
                         "unkeyed %u/%u/%u/%u/%u/%u; gate %s; verify %s mismatches %u audit unhooked %u unchecked %u (%.3f ms, %.0f parts)",
                         s.casts / n, s.hits / n, s.casts ? 100.0 * s.hits / s.casts : 0.0, castUs, lookUs, savedMs, overMs, s.indexNs / n / 1e6, savedMs - overMs,
                         s.hooks / n, s.scanned / n, s.reshapes, s.missNew, s.missKey, s.missEpoch, s.missLog, s.missCap, s.missOff,
                         sGate.offLeft ? "off" : "on", sMemoVerify ? "on" : "off", s.mismatch, s.auditUnhooked, s.auditUnchecked,
                         s.auditNs / n / 1e6, s.auditParts / n);
        std::memset(&s, 0, sizeof(s));
    }
}  // namespace

// The hooks (port/shadow_memo_hooks.h).
extern "C" {
int port_shadowmemo_armed = 0;

void port_shadowmemo_part(const void* who, const float* wasTrans, float wasRadius, int wasOn, const float* nowTrans, float nowRadius, int nowOn) {
    if (!port_shadowmemo_armed) {
        return;
    }
    port::shadowmemo::Change c;
    std::memset(&c, 0, sizeof(c));
    c.slot = port::shadowmemo::kAllSlots;
    c.zone = 0;
    c.oldOn = wasOn != 0;
    c.newOn = nowOn != 0;
    c.who = who;
    port::shadowmemo::MakeSphere(&c.was, wasTrans[0], wasTrans[1], wasTrans[2], wasRadius);
    port::shadowmemo::MakeSphere(&c.now, nowTrans[0], nowTrans[1], nowTrans[2], nowRadius);
    sMemoLog.Push(c);
    sMemoStats.hooks++;
}

void port_shadowmemo_zone(const void* who, const float* wasCenter, float wasRadius, const float* nowCenter, float nowRadius) {
    if (!port_shadowmemo_armed) {
        return;
    }
    port::shadowmemo::Change c;
    std::memset(&c, 0, sizeof(c));
    c.slot = port::shadowmemo::kAllSlots;
    c.zone = 1;
    c.who = who;
    port::shadowmemo::MakeSphere(&c.was, wasCenter[0], wasCenter[1], wasCenter[2], wasRadius);
    port::shadowmemo::MakeSphere(&c.now, nowCenter[0], nowCenter[1], nowCenter[2], nowRadius);
    sMemoLog.Push(c);
    sMemoStats.hooks++;
}

void port_shadowmemo_reshape(void) {
    if (!port_shadowmemo_armed) {
        return;
    }
    portShadowMemoBumpEpochs();
    sMemoStats.reshapes++;
}
}
#endif

namespace {
    const s32 sCollisionCountLimit = 1;
    const s32 sGravityCountLimit = 1;
}  // namespace

void ShadowController_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
    (void)-1.0f;
}

ShadowControllerHolder::ShadowControllerHolder() : NameObj("\x89\x65\x8a\xc7\x97\x9d"), _C(), _18(), _24() {
    mFarClip = 4000.0f;
    _C.init(0x500);
    _18.init(0x400);
    MR::connectToScene(this, MR::MovementType_ShadowControllerHolder, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_None);

    if (MR::isEqualStageName("AstroGalaxy") || MR::isEqualStageName("PeachCastleGardenGalaxy") || MR::isEqualStageName("PeachCastleFinalGalaxy")) {
        _24 = true;
    }
#ifdef TARGET_PC
    const char* honeyHold = std::getenv("GXV_HONEY_SHADOW_HOLD");
    sHoneyStage = !(honeyHold && *honeyHold == '0') && MR::isEqualStageName("HoneyBeeKingdomGalaxy");
    sHoneyHolds = {};
    sHoneyHoldBatch = false;
    // (shadowmemo: a new scene: hooks disarmed until its first run arms them, which drops every memo; the audit and the gate
    // start again)
    port_shadowmemo_armed = 0;
    sAuditSnap[0].Reset();
    sAuditSnap[1].Reset();
    sAuditPrimed = false;
    std::memset(&sGate, 0, sizeof(sGate));
    if (sFilterVtSensor == nullptr) {
        CollisionPartsFilterSensor probeSensor(nullptr);
        CollisionPartsFilterActor probeActor(nullptr);
        sFilterVtSensor = *reinterpret_cast< const void* const* >(static_cast< CollisionPartsFilterBase* >(&probeSensor));
        sFilterVtActor = *reinterpret_cast< const void* const* >(static_cast< CollisionPartsFilterBase* >(&probeActor));
    }
#endif
}

ShadowController::ShadowController(LiveActor* pActor, const char* pName)
    : mActor(pActor), mName(pName), mGroupName(""), mDrawer(), mProjectedSensor(), mCollisionPartsFilter(), _18(), _1C(), mDropPos(), mDropDir(),
      mProjPos(), mProjNorm(), _30(0.0f, 0.0f, 0.0f), _3C(0.0f, -1.0f, 0.0f), _48(0.0f, 0.0f, 0.0f), _54(0.0f, 1.0f, 0.0f), _60(1), _61(), _62(),
      _63(), _64(), _65(), _66(), _67(), mStartOffset(50.0f), mDropLength(), _70(), _71(1), _72(1) {
#ifdef TARGET_PC
    std::memset(&mPortMemo, 0, sizeof(mPortMemo));
#endif
    MR::createSceneObj(SceneObj_ShadowControllerHolder);
    MR::addShadowController(this);
}

template MR::Vector< MR::AssignableArray< ShadowController* > >::Vector();
template MR::Vector< MR::AssignableArray< ShadowController* > >::~Vector();
template void MR::Vector< MR::AssignableArray< ShadowController* > >::push_back(ShadowController* const&);

void ShadowControllerHolder::movement() {
    updateController();
}

void ShadowControllerHolder::initAfterPlacement() {
    ShadowController* pController;
    int size = _C.size();

    for (u32 i = 0; i < size; i++) {
        pController = _C[i];

        pController->updateDirection();
        pController->updateProjection();
    }
}

void ShadowControllerHolder::updateController() {
#ifdef TARGET_PC
    // (undrawn26) the controllers this call leaves for the flush stay in the list, the others are cast as always
    u32 leanKept = 0;
    if (!sLeanFlushing && port_undrawn_lean_shadows() && _18.size() != 0 && _18.size() <= 0x500) {
        const bool pb = port_pb_shadow_clip() > 0.0f;
        u32 runN = 0;
        const u32 n = _18.size();
        for (u32 i = 0; i < n; i++) {
            ShadowController* c = _18[i];
            if (port_undrawn_shadow_decide(c, c->mActor != nullptr ? c->mActor->mName : nullptr, c->mPbLogicRead ? 1 : 0, c->_60, c->_61, pb ? 1 : 0)) {
                sLeanKeep[leanKept++] = c;
            } else {
                sLeanRun[runN++] = c;
            }
        }
        if (leanKept != 0) {
            _18.clear();
            for (u32 i = 0; i < runN; i++) {
                _18.push_back(sLeanRun[i]);
            }
        }
    }
#endif
    int size = _18.size();
#ifdef TARGET_PC
    {
        const bool on = portShadowMemoOn() != 0;
        sMemoVerify = on && portShadowMemoVerify() != 0;
        if (sGate.offLeft != 0 && !sMemoVerify) {
            if (--sGate.offLeft == 0) {
                port_log_summary("shadowmemo: gate ON again (retry)");
            }
        } else {
            sGate.offLeft = 0;
        }
        sMemoActive = on && sGate.offLeft == 0;
        if (sMemoActive && !port_shadowmemo_armed) {
            portShadowMemoBumpEpochs();  // (the hooks were off: every memo made before is dropped)
            port_shadowmemo_armed = 1;
            sAuditPrimed = false;
            sMemoRunSeq = sMemoLog.seq;
        } else if (!sMemoActive && port_shadowmemo_armed) {
            port_shadowmemo_armed = 0;
        }
        if (sMemoActive && size != 0) {
            const int64_t t0 = port_host_time_ns();
            sMemoIndex.Build(sMemoLog, sMemoRunSeq);  // (this turn's changes: what the memos reused last turn look at)
            sMemoRunSeq = sMemoLog.seq;
            const int64_t dt = port_host_time_ns() - t0;
            sMemoStats.indexNs += static_cast< u64 >(dt);
            sGate.indexNs += static_cast< u64 >(dt);
        }
        if (sMemoVerify && size != 0 && MR::isExistSceneObj(SceneObj_CollisionDirector)) {
            portShadowMemoAudit();
        }
    }
#endif

#ifdef TARGET_PC
    // GXV_PB_SHADOW (Performance-mode visual budget; 2800 in Performance mode, 0 = as the game): the shadows' far clip, which the game
    // has only in the hub (AstroGalaxy, the castle gardens), applies in every galaxy, so a far actor's shadow is neither projected (no
    // collision cast) nor drawn (volume or surface). Never within 1200 units of the player (Mario's own, his companions').
    const f32 pbShadowClip = port_pb_shadow_clip();
    // (popin31: with GXV_PB_MINPX the floor decides every shadow in every galaxy and the Honeyhive hold has nothing left to do; it stays for the A/B.
    // popin31b: the shadows' own floor, GXV_PB_MINPX_SHADOW, which GXV_PB_MINPX sets when it is unset)
    sHoneyHoldBatch = sHoneyStage && pbShadowClip > 0.0f && port_pb_budget_on() && !port_pb_demo_off() && port::popin::HystPct() == 0.0f && !(port_pb_floor.kHeldK[port::popin::kFlShadow] > 0.0f);
    if (sHoneyHoldBatch) sHoneyHolds.begin();
#endif
    for (u32 i = 0; i < size; i++) {
#ifdef TARGET_PC
        if (pbShadowClip > 0.0f) {
            _18[i]->updateFarClippingPb(_24 ? mFarClip : 100000.0f, pbShadowClip);
        } else
#endif
        if (_24) {
            _18[i]->updateFarClipping(mFarClip);
        }

#ifdef TARGET_PC
        if (__builtin_expect(_18[i]->mPbFloorKept != 0, 0) && port::popin::CostSample(sFloorCostTick)) {
            // popin31b: a shadow the floor kept (it would be far-clipped without it): its update timed for the census's price line
            const int64_t t0 = port_host_time_ns();
            _18[i]->update();
            port_pb_floor_cost(port::popin::kFlShadow, static_cast< u32 >(port_host_time_ns() - t0));
        } else
#endif
        _18[i]->update();
        if (__builtin_expect((port_cc_flag & 2) != 0, 0) && _18[i]->mActor != nullptr) {
            port_dist_note_shadow(_18[i]->mActor->mPosition.x, _18[i]->mActor->mPosition.y, _18[i]->mActor->mPosition.z);  // (far30, GXV_DISTCENSUS)
        }
    }

#ifdef TARGET_PC
    if (sHoneyHoldBatch) sHoneyHolds.end();
#endif
    _18.clear();
#ifdef TARGET_PC
    for (u32 i = 0; i < leanKept; i++) {
        _18.push_back(sLeanKeep[i]);  // (undrawn26: still to be cast, by the next picture's flush)
    }
    if (sMemoActive) {
        sMemoActive = false;
        if (!sMemoVerify) {
            portShadowMemoGateTurn();
        }
    }
    if (portShadowMemoOn()) {
        portShadowMemoSummary();
    }
#endif
}

#ifdef TARGET_PC
// GameSystem::frameLoop, before a picture's draw: the casts the turns since the last picture left out, from the state they ended in.
extern "C" void port_shadow_flush_pending(void) {
    if (!port_undrawn_lean_shadows() || !MR::isExistSceneObj(SceneObj_ShadowControllerHolder)) {
        return;
    }
    ShadowControllerHolder* pHolder = MR::getSceneObj< ShadowControllerHolder >(SceneObj_ShadowControllerHolder);
    if (pHolder->_18.size() == 0) {
        return;
    }
    sLeanFlushing = true;
    pHolder->updateController();
    sLeanFlushing = false;
}
#endif

ShadowControllerList::ShadowControllerList(LiveActor* pActor, u32 listCount) : mShadowList(), mHost(pActor) {
    mShadowList.init(listCount);
}

void ShadowControllerList::addController(ShadowController* pController) {
    mShadowList.push_back(pController);
}

u32 ShadowControllerList::getControllerCount() const {
    return mShadowList.size();
}

ShadowController* ShadowControllerList::getController(u32 idx) const {
    return mShadowList[idx];
}

ShadowController* ShadowControllerList::getController(const char* pName) const {
    if (mShadowList.size() == 1) {
        return mShadowList[0];
    }

    for (u32 i = 0; i < mShadowList.size(); i++) {
        if (MR::isEqualString(pName, mShadowList[i]->mName)) {
            return mShadowList[i];
        }
    }

    return nullptr;
}

void ShadowControllerList::resetCalcCount() {
    for (u32 i = 0; i < mShadowList.size(); i++) {
        ShadowController* controller = mShadowList[i];
        controller->_65 = 0;
        controller->_66 = 0;
    }
}

void ShadowControllerList::requestCalc() {
    for (u32 i = 0; i < mShadowList.size(); i++) {
        mShadowList[i]->requestCalc();
    }
}

void ShadowController::requestCalc() {
    if (!_64) {
        _64 = 1;
        appendToHolder();
    }
}

void ShadowController::update() {
    const bool draw = isDraw();
#ifdef TARGET_PC
    if (sHoneyStage) mHoneyWasDrawn = draw;
#endif
    if (draw) {
        updateDirection();
        updateProjection();
    }

    _64 = 0;
}

void ShadowController::updateFarClipping(f32 clip) {
    TVec3f dropPos;
    getDropPos(&dropPos);
    f32 cameraDistZ = MR::calcCameraDistanceZ(dropPos);
    _67 = cameraDistZ >= clip;
}

#ifdef TARGET_PC
void ShadowController::updateFarClippingPb(f32 clip, f32 pbClip) {
    TVec3f dropPos;
    getDropPos(&dropPos);
    const f32 cameraDistZ = MR::calcCameraDistanceZ(dropPos);
    _67 = cameraDistZ >= clip;  // (the game's own: the hub)
    // (popin24) GXV_POPIN_ZOOM: the far clip and the player's range scale with the zoom ratio (1 when off); GXV_POPIN_HYST: a shadow
    // far-clipped by the budget returns popin-pct nearer than it left. Idempotent for one position.
    const f32 zoom = port_pb_scale();  // (popin25: the zoom ratio times GXV_PB_DEMO's widening)
    const f32 hystPct = port::popin::HystPct();
    const bool was = mPbShadowFar;
    bool far = false;
    bool kept = false;  // (popin31b: the floor kept it)
    if (!_67 && !mPbLogicRead && !port_pb_demo_off()) {
        const f32 limit = port_detail_dist(1, pbClip) * zoom + port_pb_lead(pbClip * zoom);  // (popin25: + GXV_POPIN_LEAD's margin)
        const f32 pl = 1200.0f * zoom;
        far = cameraDistZ >= (was ? limit - port::popin::Band(limit, hystPct) : limit);
        if (far) {
            const f32 plReach = was ? pl - port::popin::Band(pl, hystPct) : pl;
            far = port_pb_player_dist2(dropPos.x, dropPos.y, dropPos.z) > plReach * plReach;
        }
        if (sHoneyHoldBatch && sHoneyHolds.keep(this, mHoneyWasDrawn && !was, far, cameraDistZ, limit)) far = false;
        if (far && __builtin_expect(port::popin::GuardOn(port::popin::kFlShadow), 0)) {
            // popin31: GXV_PB_MINPX: the shadow is clipped only while its caster is under the floor on the picture (the depth along the view is never more than the
            // distance: careful); GXV_PB_DEMO=2's release after a flight: a shadow in the picture is not taken. popin31b: the shadows' own floor
            // (GXV_PB_MINPX_SHADOW); a shadow the floor kept is marked for the census's price line
            const int v = port::popin::CutVerdict(port::popin::kShadow, was, portFloorRadius(), cameraDistZ, dropPos.x, dropPos.y, dropPos.z);
            far = v == port::popin::kCutOk;
            kept = v == port::popin::kCutFloor;
        }
    }
    mPbFloorKept = kept ? 1 : 0;
    if (far) {
        _67 = true;
        port_logic_count(14, 1);
    }
    if (far != was) {
        mPbShadowFar = far;
        if (port::popin::LogOn()) {
            port_popin_event(port::popin::kShadow, far ? 0 : 1, mActor, &dropPos, -1.0f);
        }
    }
}
#endif

#ifdef TARGET_PC
// popin31: the shadow's size for the floor: its caster's model extent (from the actor's origin, its scale and pose in it) with the margin; a caster with no
// model: 300 (the game's default clipping sphere). Measured again every 64 turns (port/popin_site.h CachedExtent).
f32 ShadowController::portFloorRadius() {
    const f32 ext = port::popin::CachedExtent(mPbFloorExt, mPbFloorAt, mActor);
    return port::popin::SizeRadius(ext > 0.0f ? 0.0f : 300.0f, ext, 0.0f);
}
#endif

void ShadowController::updateDirection() {
    if (isCalcGravity()) {
        TVec3f position;
        getDropPos(&position);
        TVec3f previousDirection(_3C);
        if (!isCalcShadowGravity() || !MR::calcDropShadowVectorOrZero(mActor, position, &_3C, nullptr, 0)) {
            if (!MR::calcGravityVectorOrZero(mActor, position, &_3C, nullptr, 0)) {
                _3C = previousDirection;
            }
        }

        if (_61 == 2 || _61 == 5) {
            _66++;
        }
    }
}

void ShadowController::updateProjection() {
    if (isCalcCollision()) {
#ifndef TARGET_PC
        Triangle triangle;
#endif
        TVec3f position;
        getDropPos(&position);
        TVec3f direction;
        getDropDir(&direction);
        position -= direction * mStartOffset;
#ifdef TARGET_PC
        port_logic_count(7, 1);
        // shadowmemo: the same cast as below (the same arguments), or its answer from the memo (port/shadow_memo.h)
        const TVec3f ray(direction * (mDropLength + mStartOffset));
        if (!sMemoActive) {
            mPortMemo.mValid = 0;  // (a cast made outside the holder's run, initAfterPlacement: no snapshot stands behind it)
            portCast(position, ray);
        } else {
            sMemoStats.casts++;
            sGate.lookups++;
            const bool timeIt = (sMemoStats.casts & 7) == 1;  // (a sample of the lookups is timed: the gate's overhead)
            const int64_t tl = timeIt ? port_host_time_ns() : 0;
            const u32 slot = _62 == 0 ? 0 : 1;
            const void* filterVt = nullptr;
            const void* filterArg = nullptr;
            const bool keyed = _62 <= 1 && portShadowMemoFilterKey(mCollisionPartsFilter, &filterVt, &filterArg);
            PortProjMemo& m = mPortMemo;
            bool reuse = false;
            if (!keyed) {
                sMemoStats.missOff++;
            } else if (!m.mValid) {
                sMemoStats.missNew++;
            } else if (m.mSlot != slot || m.mFilter != mCollisionPartsFilter || m.mFilterVt != filterVt || m.mFilterArg != filterArg
                       || std::memcmp(&m.mStart, &position, sizeof(TVec3f)) != 0 || std::memcmp(&m.mRay, &ray, sizeof(TVec3f)) != 0) {
                sMemoStats.missKey++;
            } else if (m.mEpoch != sMemoLog.epoch[slot]) {
                sMemoStats.missEpoch++;
            } else if (m.mSeq != sMemoIndex.from && sMemoLog.seq - m.mSeq > kMemoScanCap) {
                sMemoStats.missCap++;  // (not looked at last turn and far behind: cast again)
            } else if (m.mSeq == sMemoLog.seq) {
                reuse = true;  // (nothing changed since)
            } else {
                port::shadowmemo::Seg seg;
                port::shadowmemo::MakeSeg(&seg, &position.x, &ray.x);
                u32 scanned = 0;
                reuse = sMemoLog.StillValid(slot, seg, m.mSeq, m.mEpoch, &scanned, kMemoScanCap, &sMemoIndex);
                sMemoStats.scanned += scanned;
                if (!reuse) {
                    sMemoStats.missLog++;
                }
            }
            if (reuse) {
                m.mSeq = sMemoLog.seq;  // (its answer stands as of now: the next look starts here)
            }
            if (__builtin_expect(mPbFloorKept != 0, 0) && port::popin::CensusOn()) {
                port_pb_floor_shadow_memo(reuse ? 1 : 0);  // (popin31b: a shadow the floor kept: is its ground probe the memo's answer? the census's price line)
            }
            if (timeIt) {
                const int64_t dl = port_host_time_ns() - tl;
                sMemoStats.lookNs += static_cast< u64 >(dl);
                sMemoStats.lookSampled++;
                sGate.lookNs += static_cast< u64 >(dl);
                sGate.lookSampled++;
            }

            if (reuse && !sMemoVerify) {
                sMemoStats.hits++;
                sGate.hits++;
                _63 = m.mHit;
                if (_63) {
                    _48.set(m.mPos);
                    mProjectedSensor = m.mSensor;
                    _54.set(m.mNorm);
                } else {
                    mProjectedSensor = nullptr;
                }
            } else {
                const bool sample = !reuse && (sMemoStats.casts & 3) == 0;
                const int64_t t0 = sample ? port_host_time_ns() : 0;
                portCast(position, ray);
                if (sample) {
                    const int64_t dc = port_host_time_ns() - t0;
                    sMemoStats.castNs += static_cast< u64 >(dc);
                    sMemoStats.sampled++;
                    sGate.castNsAvg = sGate.castNsAvg > 0.0 ? sGate.castNsAvg * 0.95 + dc * 0.05 : static_cast< double >(dc);
                }
                if (reuse) {  // (verify: the memo's answer against the cast's)
                    sMemoStats.hits++;
                    bool same = (_63 != 0) == (m.mHit != 0);
                    if (same && _63) {
                        same = std::memcmp(&_48, &m.mPos, sizeof(TVec3f)) == 0 && std::memcmp(&_54, &m.mNorm, sizeof(TVec3f)) == 0
                               && mProjectedSensor == m.mSensor;
                    }
                    if (!same && sMemoStats.mismatch++ < 8) {
                        port_log_summary("shadowmemo: MISMATCH host=%s shadow=%s hit %d/%d pos (%g %g %g)/(%g %g %g)",
                                         mActor != nullptr ? mActor->mName : "?", mName != nullptr ? mName : "?", m.mHit, _63, m.mPos.x,
                                         m.mPos.y, m.mPos.z, _48.x, _48.y, _48.z);
                    }
                }
                if (keyed) {
                    m.mStart = position;
                    m.mRay = ray;
                    m.mFilter = mCollisionPartsFilter;
                    m.mFilterVt = filterVt;
                    m.mFilterArg = filterArg;
                    m.mSeq = sMemoLog.seq;
                    m.mEpoch = sMemoLog.epoch[slot];
                    m.mSlot = static_cast< u8 >(slot);
                    m.mHit = _63;
                    if (_63) {
                        m.mPos = _48;
                        m.mNorm = _54;
                        m.mSensor = mProjectedSensor;
                    }
                    m.mValid = 1;
                } else {
                    m.mValid = 0;
                }
            }
        }
#else

        switch (_62) {
        case 0:
            _63 = MR::getFirstPolyOnLineToMap(&_48, &triangle, position, direction * (mDropLength + mStartOffset), mCollisionPartsFilter, nullptr);
            break;
        case 1:
            _63 = MR::getFirstPolyOnLineToWaterSurface(&_48, &triangle, position, direction * (mDropLength + mStartOffset), mCollisionPartsFilter,
                                                       nullptr);
            break;
        }

        if (_63) {
            mProjectedSensor = triangle.getSensor();
            _54.set(*triangle.getNormal(0));
        } else {
            mProjectedSensor = nullptr;
        }
#endif

        if (_60 == 2) {
            _65++;
        }
    }
}

#ifdef TARGET_PC
// The original cast and its stores (updateProjection's body), on the arguments it computed.
void ShadowController::portCast(const TVec3f& rPosition, const TVec3f& rRay) {
    Triangle triangle;

    switch (_62) {
    case 0:
        _63 = MR::getFirstPolyOnLineToMap(&_48, &triangle, rPosition, rRay, mCollisionPartsFilter, nullptr);
        break;
    case 1:
        _63 = MR::getFirstPolyOnLineToWaterSurface(&_48, &triangle, rPosition, rRay, mCollisionPartsFilter, nullptr);
        break;
    }

    if (_63) {
        mProjectedSensor = triangle.getSensor();
        _54.set(*triangle.getNormal(0));
    } else {
        mProjectedSensor = nullptr;
    }
}
#endif

ShadowDrawer* ShadowController::getShadowDrawer() {
    return mDrawer;
}

LiveActor* ShadowController::getHost() const {
    return mActor;
}

void ShadowController::setShadowDrawer(ShadowDrawer* pDrawer) {
    mDrawer = pDrawer;
    pDrawer->setShadowController(this);
}

void ShadowController::getDropPos(TVec3f* pOut) const {
    if (mDropPos) {
        pOut->set(*mDropPos);
    } else {
        if (_1C) {
            PSMTXMultVec(_1C, (const Vec*)&_30, (Vec*)pOut);
        } else {
            pOut->set(_30);
        }
    }
}

void ShadowController::getDropDir(TVec3f* pOut) const {
    if (mDropDir) {
        pOut->set(*mDropDir);
    } else {
        pOut->set(_3C);
    }
}

f32 ShadowController::getDropLength() const {
    return mDropLength;
}

void ShadowController::getProjectionPos(TVec3f* pOut) const {
    if (mProjPos) {
        pOut->set(*mProjPos);
    } else {
        pOut->set(_48);
    }
}

void ShadowController::getProjectionNormal(TVec3f* pOut) const {
    if (mProjNorm) {
        pOut->set(*mProjNorm);
    } else {
        pOut->set(_54);
    }
}

f32 ShadowController::getProjectionLength() const {
    if (!_63) {
        return -1.0f;
    }

    TVec3f position;
    getDropPos(&position);
    TVec3f direction;
    getDropDir(&direction);
    TVec3f difference(_48);
    difference -= position;
    f32 length = direction.dot(difference);
    if (length < 0.0f) {
        return 0.0f;
    }

    return position.distance(_48);
}

bool ShadowController::isProjected() const {
    return _63 != 0;
}

bool ShadowController::isDraw() const {
    if (_67) {
        return false;
    }

    if (!_71) {
        return false;
    }

    if (!_72) {
        return true;
    }

    return MR::isValidDraw(mActor);
}

bool ShadowController::isCalcCollision() const {
    if (!_60) {
        return false;
    }

    if (_60 == 2) {
        return _65 < ::sCollisionCountLimit;
    }

    return true;
}

bool ShadowController::isCalcGravity() const {
    if (_61 == 0 || _61 == 3) {
        return false;
    }

    if (_61 == 0 || _61 == 3) {
        return _66 < ::sGravityCountLimit;
    }

    return true;
}

bool ShadowController::isCalcShadowGravity() const {
    const u8 offset = static_cast< u8 >(-4);
    return static_cast< u8 >(_61 + offset) <= 1;
}

void ShadowController::setGroupName(const char* pName) {
    mGroupName = pName;
}

void ShadowController::setDropPosPtr(const TVec3f* pDropPos) {
    mDropPos = pDropPos;
    _1C = 0;
}

void ShadowController::setDropPosMtxPtr(MtxPtr pMtx, const TVec3f& rOffset) {
    _18 = pMtx;
    mDropPos = nullptr;
    _1C = pMtx;
    _30.set(rOffset);
}

void ShadowController::setDropPosFix(const TVec3f& rPos) {
    _30.set(rPos);
    mDropPos = 0;
    _1C = 0;
}

void ShadowController::setDropDirPtr(const TVec3f* pDropDir) {
    mDropDir = pDropDir;
    _61 = 0;
}

void ShadowController::setProjectionPtr(const TVec3f* pPosition, const TVec3f* pNormal) {
    mProjPos = pPosition;
    mProjNorm = pNormal;
    _60 = 0;
    _63 = 1;
    mProjectedSensor = nullptr;
}

void ShadowController::setDropDirFix(const TVec3f& rDirection) {
    _3C.set(rDirection);
    mDropDir = 0;
    _61 = 0;
}

void ShadowController::setDropLength(f32 len) {
    mDropLength = len;
}

void ShadowController::setDropStartOffset(f32 offs) {
    mStartOffset = offs;
}

void ShadowController::setDropTypeNormal() {
    _62 = 0;
}

void ShadowController::setDropTypeSurface() {
    _62 = 1;
}

void ShadowController::setProjectionFix(const TVec3f& rPosition, const TVec3f& rNormal, bool projected) {
    _48.set(rPosition);
    _54.set(rNormal);
    _63 = projected;
    mProjectedSensor = 0;
}

void ShadowController::onCalcCollision() {
    _60 = 1;
}

void ShadowController::offCalcCollision() {
    _60 = 0;
}

void ShadowController::onCalcCollisionOneTime() {
    _60 = 2;
    _65 = 0;
}

void ShadowController::onCalcDropGravity() {
    TVec3f vec(0.0f, 1.0f, 0.0f);
    _3C.set(vec);
    mDropDir = 0;
    _61 = 1;
}

void ShadowController::onCalcDropGravityOneTime() {
    TVec3f vec(0.0f, 1.0f, 0.0f);
    _3C.set(vec);
    mDropDir = 0;
    _61 = 2;
    _66 = 0;
}

void ShadowController::offCalcDropGravity() {
    _61 = 0;
}

void ShadowController::onCalcDropPrivateGravity() {
    TVec3f vec(0.0f, 1.0f, 0.0f);
    _3C.set(vec);
    mDropDir = 0;
    _61 = 4;
}

void ShadowController::onCalcDropPrivateGravityOneTime() {
    TVec3f vec(0.0f, 1.0f, 0.0f);
    _3C.set(vec);
    mDropDir = 0;
    _61 = 5;
    _66 = 0;
}

void ShadowController::offCalcDropPrivateGravity() {
    _61 = 3;
}

void ShadowController::setCollisionPartsFilter(CollisionPartsFilterBase* pBase) {
    mCollisionPartsFilter = pBase;
}

void ShadowController::onFollowHostScale() {
    _70 = 1;
}

void ShadowController::offFollowHostScale() {
    _70 = 0;
}

bool ShadowController::isFollowHostScale() const {
    return _70;
}

void ShadowController::onVisibleSyncHost() {
    _72 = 1;
}

void ShadowController::offVisibleSyncHost() {
    _72 = 0;
}

void ShadowController::validate() {
    _71 = 1;
}

void ShadowController::invalidate() {
    _71 = 0;
}

namespace MR {
    void addShadowController(ShadowController* pController) {
        getSceneObj< ShadowControllerHolder >(SceneObj_ShadowControllerHolder)->_C.push_back(pController);
    }

    void requestCalcActorShadowAppear(LiveActor* pActor) {
        if (!isInitializeStatePlacementSomething()) {
            if (pActor->mShadowControllerList) {
                pActor->mShadowControllerList->resetCalcCount();
            }

            requestCalcActorShadow(pActor);
        }
    }

    void requestCalcActorShadow(LiveActor* pActor) {
        if (pActor->mShadowControllerList) {
            pActor->mShadowControllerList->requestCalc();
        }
    }
}  // namespace MR

template MR::AssignableArray< ShadowController* >::~AssignableArray();

ShadowControllerHolder::~ShadowControllerHolder() {
}
