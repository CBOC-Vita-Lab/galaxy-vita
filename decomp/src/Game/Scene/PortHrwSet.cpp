// GXV_HRW (hrw28): the significance set. In the pair's first picture (A) a few actors are TAGGED (their models carry an owner id, DrawBuffer.cpp emits the owner marker around their draws);
// at the start of the pair's second turn (B) the ones that matter most are drawn again, in B's camera, from the pose A's calc-anim left them in, inside the budget; the render thread
// (vgx_hrw.h Compose) puts those draws in place of the retained ones. An actor that is not drawn fresh stays as the first picture drew it. Anything this file cannot isolate safely
// is not tagged at all and stays retained (counted by reason). Pure logic: platform/include/port/hrw_set.h; tests/hrw28_test.cpp.
#ifdef TARGET_PC
#include "Game/LiveActor/AllLiveActorGroup.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/LiveActor/ModelManager.hpp"
#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/NameObj/NameObjListExecutor.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/System/DrawBuffer.hpp"
#include "Game/System/DrawBufferExecuter.hpp"
#include "Game/System/DrawBufferGroup.hpp"
#include "Game/System/DrawBufferHolder.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "port/compat.h"
#include "port/hrw_set.h"
#include "port/host_vector.h"  // (heap36: sSeen / pre outlive the scene: host memory, never the scene heap the game thread's `new` would give)
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int port_hrw_mode(void);
extern "C" int port_hrw_view_deferred_cfg(void);  // (GameScene.cpp)
extern "C" int port_view_pending(void);
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((format(printf, 1, 2), weak));
bool port_live_actor_valid(const LiveActor*);
void port_live_actor_snapshot();

namespace {
namespace hs = port::hrwset;

constexpr int kMaxMem = 6;
constexpr int kMaxUnits = 64;

struct Member {
    LiveActor* actor;
    J3DModel* model;
    DrawBufferGroup* group;
    DrawBufferExecuter* exec;
    int type;
};
struct Unit {
    LiveActor* root;
    uint8_t id;
    int n;
    Member mem[kMaxMem];
    float eyeA[3];  // eye-space centre in the first picture
    float radius;
    float worldA[3];
};
struct Seen {
    LiveActor* actor;
    int type;
    DrawBufferGroup* group;
    DrawBufferExecuter* exec;
};

hs::Book sBook;
hs::PairStats sStats;
PortHostVector<Seen> sSeen;  // (heap36: was std::vector, grown by the game thread in play (GXV_HRW_SET=1): its storage came from the scene's heap and dangled after the stage change)
PORT_HEAP_GUARD(sSeen);
Unit sUnits[kMaxUnits];
int sNUnits = 0;
bool sTagging = false;
uint32_t sPair = 0, sPairId = 0;
const void* sScene = nullptr;
float sMarioWorldA[3] = {};
bool sMarioA = false;
float sVA[12] = {};

int EnvInt(const char* name, int def) {
    const char* v = getenv(name);
    return v && *v ? atoi(v) : def;
}
float EnvF(const char* name, float def) {
    const char* v = getenv(name);
    return v && *v ? static_cast<float>(atof(v)) : def;
}
bool SetOn() {
    static const int on = EnvInt("GXV_HRW_SET", 0);
    return on != 0;
}
const char* SkipName() {
    static const char* s = getenv("GXV_HRW_SET_SKIP");
    return s && *s ? s : nullptr;
}

// The passes the set refreshes, and the alpha state the first picture draws each in (GameScene::draw3D): map objects' opaque pass with alpha and destination alpha on (before the shadow
// volumes), everything else off.
bool TypeOk(int t) {
    switch (t) {
    case MR::DrawBufferType_MapObj:
    case MR::DrawBufferType_MapObjWeakLight:
    case MR::DrawBufferType_MapObjStrongLight:
    case MR::DrawBufferType_NoSilhouettedMapObj:
    case MR::DrawBufferType_NoSilhouettedMapObjWeakLight:
    case MR::DrawBufferType_NoSilhouettedMapObjStrongLight:
    case MR::DrawBufferType_NPC:
    case MR::DrawBufferType_Ride:
    case MR::DrawBufferType_Enemy:
    case MR::DrawBufferType_EnemyDecoration:
        return true;
    default:
        return false;
    }
}
bool AlphaOn(int t, bool xlu) {
    return !xlu && (t == MR::DrawBufferType_MapObj || t == MR::DrawBufferType_MapObjWeakLight || t == MR::DrawBufferType_MapObjStrongLight);
}

NameObjListExecutor* Executor() {
    GameSystem* sys = SingletonHolder< GameSystem >::get();
    return sys != nullptr && sys->mSceneController != nullptr ? sys->mSceneController->getNameObjListExecutor() : nullptr;
}

void ViewMtx(float v[12]) {
    const TPos3f& view = MR::getCameraViewMtx();
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) v[r * 4 + c] = view.mMtx[r][c];
}
void ToEye(const float v[12], const float w[3], float e[3]) {
    for (int r = 0; r < 3; r++) e[r] = v[r * 4] * w[0] + v[r * 4 + 1] * w[1] + v[r * 4 + 2] * w[2] + v[r * 4 + 3];
}
hs::Params MakeParams() {
    hs::Params p;
    p.screenH = 456.0f;
    const float fovy = MR::getFovy();
    const float t = std::tan(fovy * 0.5f * 3.14159265f / 180.0f);
    p.focalPx = t > 0.01f ? p.screenH * 0.5f / t : 560.0f;
    p.nearR = EnvF("GXV_HRW_SET_NEAR", 900.0f);
    p.minScore = EnvF("GXV_HRW_SET_MIN", 1.0f);
    p.budgetMs = EnvF("GXV_HRW_FRESH_MS", 2.5f);
    p.maxN = EnvInt("GXV_HRW_SET_MAX", 16);
    return p;
}
double NowMs() { return static_cast<double>(port_host_time_ns()) / 1e6; }

void Tag(Unit& u, bool on) {
    for (int i = 0; i < u.n; ++i) {
        if (u.mem[i].model != nullptr) u.mem[i].model->mPortHrwTag = on ? u.id : 0;
    }
}
void Untag() {
    for (int i = 0; i < sNUnits; ++i) Tag(sUnits[i], false);
    sNUnits = 0;
    sTagging = false;
}
LiveActor* RootOf(LiveActor* a) {
    for (int i = 0; i < 4 && a->mPortHrwHost != nullptr; ++i) a = a->mPortHrwHost;
    return a;
}
}  // namespace

// The owner phase of a pass for the first picture's tag scope: 0 when no pair is tagging or the pass is not one the set refreshes (the tag scope then does nothing).
extern "C" unsigned port_hrw_set_phase(int bufferType, int xlu) {
    if (!sTagging || !TypeOk(bufferType)) return 0;
    return hs::PhaseOf(bufferType, xlu != 0);
}

// The first picture, just before its draw (every pair): scan the actors that are drawn, pick the likely significant ones, tag them. tag == 0 (census mode): scan and count only.
extern "C" void port_hrw_set_begin(unsigned pairId, int tag) {
    Untag();
    sScene = nullptr;
    sMarioA = false;
    if (!SetOn()) return;
    NameObjListExecutor* ex = Executor();
    if (ex == nullptr || ex->mBufferHolder == nullptr || !MR::isExistSceneObj(SceneObj_AllLiveActorGroup)) return;
    if (port_hrw_view_deferred_cfg()) {
        // the view matrices of everything but Mario are made at the next picture's flush in this configuration: the set cannot be drawn for the second picture. Nothing is scanned or tagged (no cost); counted.
        ++sStats.reasons[hs::kRsnViewDeferred];
        return;
    }
    const int64_t t0 = port_host_time_ns();
    sScene = ex;
    sPairId = pairId;
    ++sPair;
    ViewMtx(sVA);
    if (MR::isExistMario()) {
        const TVec3f* m = MR::getPlayerCenterPos();
        sMarioWorldA[0] = m->x; sMarioWorldA[1] = m->y; sMarioWorldA[2] = m->z;
        sMarioA = true;
    }
    // every drawn actor of every group (a part's pass matters too: a unit whose part is drawn in another pass is refused)
    sSeen.clear();
    DrawBufferHolder* holder = ex->mBufferHolder;
    const int ngroups = holder->mBufferGroups.size();
    for (int t = 0; t < ngroups && t < hs::kBufferTypeLimit; ++t) {
        DrawBufferGroup* g = holder->getDrawBufferGroup(t);
        for (DrawBufferExecuter** pe = g->mActiveExecutors.begin(); pe != g->mActiveExecutors.end(); ++pe) {
            DrawBufferExecuter* e = *pe;
            for (int i = 0; i < e->mNumActors; ++i) sSeen.push_back(Seen{e->mActors[i], t, g, e});
        }
    }
    const hs::Params prm = MakeParams();
    struct Pre { int unit; float score; };
    static PortHostVector<Pre> pre;  // (heap36: host memory, as sSeen)
    pre.clear();
    static Unit cand[kMaxUnits * 4];
    int nc = 0;
    uint32_t reasons[hs::kRsnCount] = {};
    const char* skip = SkipName();
    for (const Seen& s : sSeen) {
        LiveActor* a = s.actor;
        if (a == nullptr || a->mPortHrwHost != nullptr || !TypeOk(s.type)) continue;  // (parts come with their root)
        if (a->mFlFam != 0) { ++reasons[hs::kRsnFamily]; continue; }
        if (a->mModelManager == nullptr || a->mModelManager->mModel == nullptr || a->mFlag.mIsDead || a->mFlag.mIsHiddenModel) { ++reasons[hs::kRsnNotDrawn]; continue; }
        if (a->mActorLightCtrl == nullptr) { ++reasons[hs::kRsnNoLight]; continue; }
        if (a->mStaleAnim) { ++reasons[hs::kRsnJoints]; continue; }
        if (skip != nullptr && a->mName != nullptr && strstr(a->mName, skip) != nullptr) { ++reasons[hs::kRsnFamily]; continue; }
        if (nc >= kMaxUnits * 4) { ++reasons[hs::kRsnNoRoom]; continue; }
        Unit& u = cand[nc];
        u.root = a; u.id = 0; u.n = 0;
        u.mem[u.n++] = Member{a, a->mModelManager->mModel, s.group, s.exec, s.type};
        ++nc;
    }
    // the parts: every drawn actor whose host chain ends at a candidate root joins its unit; in a pass the set does not refresh it spoils the unit
    bool spoiled[kMaxUnits * 4] = {};
    for (const Seen& s : sSeen) {
        LiveActor* a = s.actor;
        if (a == nullptr || a->mPortHrwHost == nullptr) continue;
        LiveActor* root = RootOf(a);
        for (int k = 0; k < nc; ++k) {
            if (cand[k].root != root) continue;
            if (!TypeOk(s.type) || a->mModelManager == nullptr || a->mModelManager->mModel == nullptr || a->mActorLightCtrl == nullptr || a->mStaleAnim || cand[k].n >= kMaxMem) spoiled[k] = true;
            else cand[k].mem[cand[k].n++] = Member{a, a->mModelManager->mModel, s.group, s.exec, s.type};
            break;
        }
    }
    // score by where the unit is in the first picture
    for (int k = 0; k < nc; ++k) {
        Unit& u = cand[k];
        if (spoiled[k]) { ++reasons[hs::kRsnPartPass]; continue; }
        const LiveActor* a = u.root;
        u.worldA[0] = a->mPosition.x; u.worldA[1] = a->mPosition.y; u.worldA[2] = a->mPosition.z;
        ToEye(sVA, u.worldA, u.eyeA);
        const float sc = a->mScale.x > a->mScale.y ? (a->mScale.x > a->mScale.z ? a->mScale.x : a->mScale.z) : (a->mScale.y > a->mScale.z ? a->mScale.y : a->mScale.z);
        u.radius = (a->mLodRadius > 0.0f ? a->mLodRadius : 150.0f) * (sc > 0.01f ? sc : 1.0f);
        hs::Book::Rec& r = sBook.Touch(reinterpret_cast<uintptr_t>(a), sPair);
        hs::Cand c;
        c.id = static_cast<uint32_t>(k);
        for (int i = 0; i < 3; ++i) c.eye[i] = u.eyeA[i];
        c.radius = u.radius;
        float d2 = 1e18f;
        if (sMarioA) {
            const float dx = u.worldA[0] - sMarioWorldA[0], dy = u.worldA[1] - sMarioWorldA[1], dz = u.worldA[2] - sMarioWorldA[2];
            d2 = dx * dx + dy * dy + dz * dz;
        }
        const float cd = std::sqrt(u.eyeA[0] * u.eyeA[0] + u.eyeA[1] * u.eyeA[1] + u.eyeA[2] * u.eyeA[2]);
        c.nearDist = std::sqrt(d2) < cd ? std::sqrt(d2) : cd;
        c.costMs = r.costMs;
        c.stale = r.stale;
        // (the speed estimate: last pair's eye position of it, the camera's own motion included)
        if (r.hasPos && r.samplePair + 1 == sPair) for (int i = 0; i < 3; ++i) c.vel[i] = u.eyeA[i] - r.pos[i];
        for (int i = 0; i < 3; ++i) r.pos[i] = u.eyeA[i];
        r.samplePair = sPair;
        r.hasPos = true;
        const hs::Scored sd = hs::Score(c, prm);
        if (sd.score < prm.minScore) { ++reasons[hs::kRsnBelowMin]; continue; }
        pre.push_back(Pre{k, sd.score + prm.staleGain * static_cast<float>(c.stale < prm.staleCap ? c.stale : prm.staleCap)});
    }
    std::sort(pre.begin(), pre.end(), [](const Pre& a, const Pre& b) { return a.score != b.score ? a.score > b.score : a.unit < b.unit; });
    // tag the best 2 x the set size (the budget picks among them in the second turn): tagging splits the first picture's draw merging, so not more than that
    const int want = prm.maxN * 2 < kMaxUnits ? prm.maxN * 2 : kMaxUnits;
    int id = hs::kIdFirst;
    for (size_t j = 0; j < pre.size(); ++j) {
        if (static_cast<int>(j) >= want || id > hs::kIdLast) { ++reasons[hs::kRsnNoRoom]; continue; }
        Unit& u = sUnits[sNUnits];
        u = cand[pre[j].unit];
        u.id = static_cast<uint8_t>(id++);
        ++sNUnits;
    }
    sStats.scanned += static_cast<uint32_t>(sSeen.size());
    sStats.cands += static_cast<uint32_t>(nc);
    sStats.tagged += static_cast<uint32_t>(sNUnits);
    for (int i = 0; i < hs::kRsnCount; ++i) sStats.reasons[i] += reasons[i];
    sStats.scan_ms += static_cast<double>(port_host_time_ns() - t0) / 1e6;
    sBook.Sweep(sPair);
    if (tag && sNUnits > 0) {
        for (int i = 0; i < sNUnits; ++i) Tag(sUnits[i], true);
        sTagging = true;
        // (the models are posed by this turn's calc-anim after the draw: LiveActor.cpp AnimLodDefers)
    } else {
        sNUnits = 0;
    }
}

// The second turn's start, with the draw state set (GameScene::portHrwFresh): the units drawn again. verify (mode 4, inside the first picture): all of them, no budget.
// Returns the number of units drawn. Always ends the pair's tagging.
extern "C" int port_hrw_set_draw(unsigned pairId, int verify) {
    if (!sTagging || sNUnits == 0 || pairId != sPairId) { Untag(); return 0; }
    NameObjListExecutor* ex = Executor();
    if (port_view_pending()) { sStats.reasons[hs::kRsnViewDeferred] += static_cast<uint32_t>(sNUnits); Untag(); return 0; }  // (a turn left its view pass to the flush: the matrices are not the camera's now)
    if (ex != sScene) { ++sStats.reasons[hs::kRsnScene]; Untag(); return 0; }
    port_live_actor_snapshot();
    const hs::Params prm = MakeParams();
    float vB[12];
    ViewMtx(vB);
    // candidates, now: the pose is the one A's calc-anim made (the units were pinned), the camera B's
    hs::Cand cands[kMaxUnits];
    bool ok[kMaxUnits];
    int n = 0;
    ++sStats.pairs;
    for (int k = 0; k < sNUnits; ++k) {
        Unit& u = sUnits[k];
        ok[k] = false;
        bool good = true;
        for (int i = 0; i < u.n && good; ++i) {
            const LiveActor* a = u.mem[i].actor;
            good = port_live_actor_valid(a) && a->mModelManager != nullptr && a->mModelManager->mModel == u.mem[i].model && !a->mFlag.mIsDead && !a->mFlag.mIsHiddenModel;
            if (good && a->mStaleAnim) { ++sStats.reasons[hs::kRsnJoints]; good = false; i = u.n; break; }
        }
        if (!good) { ++sStats.reasons[hs::kRsnNotDrawn]; continue; }
        float w[3] = {u.root->mPosition.x, u.root->mPosition.y, u.root->mPosition.z}, e[3];
        ToEye(vB, w, e);
        hs::Cand& c = cands[n];
        c = hs::Cand();
        c.id = static_cast<uint32_t>(k);
        for (int i = 0; i < 3; ++i) { c.eye[i] = e[i]; c.vel[i] = e[i] - u.eyeA[i]; }
        c.radius = u.radius;
        const hs::Book::Rec* r = sBook.Find(reinterpret_cast<uintptr_t>(u.root));
        c.costMs = r ? r->costMs : 0.15f;
        c.stale = r ? r->stale : 0;
        c.nearDist = 0;   // (tagged units are already the near / large / fast ones of the first picture: this ranks them by their size and motion now and by how long they waited)
        ok[k] = true;
        ++n;
    }
    // Mario's move between the pictures: what his shadow, silhouette and reflection lag by (they stay as the first picture drew them)
    if (sMarioA && MR::isExistMario()) {
        const TVec3f* m = MR::getPlayerCenterPos();
        float w0[3] = {sMarioWorldA[0], sMarioWorldA[1], sMarioWorldA[2]}, w1[3] = {m->x, m->y, m->z}, e0[3], e1[3];
        ToEye(vB, w0, e0);
        ToEye(vB, w1, e1);
        if (e0[2] < -1.0f && e1[2] < -1.0f) {
            const float dx = e1[0] / -e1[2] * prm.focalPx - e0[0] / -e0[2] * prm.focalPx, dy = e1[1] / -e1[2] * prm.focalPx - e0[1] / -e0[2] * prm.focalPx;
            hs::NoteLag(sStats, std::sqrt(dx * dx + dy * dy));
        }
    }
    int order[kMaxUnits];
    hs::Params sel = prm;
    sel.minScore = 0.0f;  // (they were worth it in the first picture; the order is what is wanted here)
    sel.budgetMs = verify ? 1e9 : prm.budgetMs * 1.5;
    sel.maxN = verify ? kMaxUnits : prm.maxN;
    double used = 0;
    int worth = 0;
    const int m = hs::Select(cands, n, sel, order, kMaxUnits, &used, &worth);
    int done = 0;
    double spent = 0;
    int firstSkipped = m;
    bool drawn[kMaxUnits] = {};
    auto drawUnit = [&](int ci) -> int {
        const int k = static_cast<int>(cands[ci].id);
        Unit& u = sUnits[k];
        const double t0 = NowMs();
        for (int i = 0; i < u.n; ++i) {
            Member& mb = u.mem[i];
            for (int xlu = 0; xlu < 2; ++xlu) {
                GXSetAlphaUpdate(AlphaOn(mb.type, xlu != 0) ? GX_TRUE : GX_FALSE);
                GXSetDstAlpha(AlphaOn(mb.type, xlu != 0) ? GX_TRUE : GX_FALSE, 0);
                port_gx_marker(PORT_GX_OWNER(u.id, hs::PhaseOf(mb.type, xlu != 0)));
                mb.group->portHrwDrawOne(mb.exec, xlu != 0, mb.model);
                port_gx_marker(PORT_GX_OWNER(0, 0));
            }
        }
        drawn[k] = true;
        hs::Book::Rec& r = sBook.Touch(reinterpret_cast<uintptr_t>(u.root), sPair);
        hs::Book::Drawn(r, sPair, NowMs() - t0);
        return 1;
    };
    if (verify) {
        for (int j = 0; j < m; ++j) done += drawUnit(order[j]) >= 0 ? 1 : 0;
    } else {
        done = hs::RunBudgeted(order, m, prm.budgetMs, NowMs, drawUnit, &spent, &firstSkipped);
    }
    for (int j = 0; j < m; ++j) {
        const int k = static_cast<int>(cands[order[j]].id);
        if (!drawn[k]) {
            ++sStats.reasons[hs::kRsnBudget];
            hs::Book::Waited(sBook.Touch(reinterpret_cast<uintptr_t>(sUnits[k].root), sPair));
        }
    }
    for (int ci = 0; ci < n; ++ci) {   // (tagged and ok, but Select left it out: it waited)
        const int k = static_cast<int>(cands[ci].id);
        bool inOrder = false;
        for (int j = 0; j < m; ++j) inOrder |= order[j] == ci;
        if (!inOrder) { ++sStats.reasons[hs::kRsnNoRoom]; hs::Book::Waited(sBook.Touch(reinterpret_cast<uintptr_t>(sUnits[k].root), sPair)); }
    }
    sStats.refreshed += static_cast<uint32_t>(done);
    sStats.stale += static_cast<uint32_t>(n - done);
    sStats.fresh_ms += spent;
    sStats.fresh_budget += prm.budgetMs;
    if (spent > sStats.fresh_max) sStats.fresh_max = spent;
    if (spent > prm.budgetMs) ++sStats.overBudget;
    if (!verify) Untag();   // (verify: the tags stay, the full picture that follows is drawn with them; the next pair's begin clears them)
    return done;
}

// The census: the stats since the last call (the game thread; vgx_galaxy.cpp prints them with the pair line).
extern "C" void port_hrw_set_take_stats(void* out) {
    if (out != nullptr) *static_cast<hs::PairStats*>(out) = sStats;
    sStats.Clear();
}
extern "C" void port_hrw_set_clear(void) { Untag(); }
// The HUD block's time in the first picture (census: what a fresh HUD would cost to draw again)
extern "C" void port_hrw_hud_note(long long ns) {
    const double ms = static_cast<double>(ns) / 1e6;
    ++sStats.hudN;
    sStats.hud_ms += ms;
    if (ms > sStats.hud_max) sStats.hud_max = ms;
}

// The star pointer, drawn fresh for the second picture (GXV_HRW_POINTER) and tagged in the first.
extern "C" int port_hrw_pointer_on(void) {
    static const int on = EnvInt("GXV_HRW_POINTER", 0);
    return on != 0 && port_hrw_mode() != 0 && port_hrw_mode() != 2 && port_hrw_mode() != 3 && port_hrw_mode() != 5;
}
extern "C" void port_hrw_pointer_draw(void) {
    MR::reinitGX();
    port_gx_marker(PORT_GX_OWNER(PORT_HRW_OWN_POINTER, PORT_HRW_PH_POINTER));
    SingletonHolder< GameSystem >::get()->mObjHolder->drawStarPointer();
    port_gx_marker(PORT_GX_OWNER(0, 0));
}
#endif
