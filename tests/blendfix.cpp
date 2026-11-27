// GXV_POSE_BLENDFIX host proof and measurement (blendfix31; tools/test_blendfix.sh): the blend start after a left-out pose, for every pose cut, against
// the game's own XanimeCore (calc, updateFrame, doFreeze, freezeCopy, and blendfix31's portLeaveOut / portCatchUp / portReadsLast) and J3D joint code.
//
// Built as tests/bowl_pose.cpp (bowl31's two-world harness): tests/par_anim_walk.cpp's models (random joint trees, BCKs, the three core types, fixT,
// envelopes), linked whole with the game units. Two worlds of the same actors run the same logic from one event stream: REF computes every calc-anim
// (the game without cuts); CUT applies one pose cut as LiveActor::calcAnim does (the latch released at a left-out computation; with GXV_POSE_BLENDFIX
// the inputs recorded, XanimePlayer::portLeaveOut; UNDRAWN_LEAN / ANIM_LOD and BOWL_POSE's broken pairs: listed, made by the drawn turn's flush from
// the restored latch, port_anim_flush_stale). The XanimePlayer side is modelled as in bowl_pose.cpp, in the game's order (ModelManager::update at the
// start of movement: frame and latch, updateInterpoleRatio, the core's weights / _1C / _20; then the actor's control: changes of animation as
// changeAnimationSimple / prepareAnimation do them: doFreeze, setBck, the frame control, changeInterpoleFrame).
//
// Rules (CUT): pair    GXV_PAIRPOSE (skip turn: a drawn turn after an undrawn one; never two in a row)
//              phase   GXV_POSE_PHASE=1 (every-2nd members on the skip turn only: the same turns as pair)
//              bowl    GXV_BOWL_POSE (pair + the flush for a broken pair), without bowl's guards (what a penguin would need)
//              lean    GXV_UNDRAWN_LEAN / GXV_ANIM_LOD deferral (every undrawn turn left out, the drawn turn's flush makes it before the picture)
//              every2  PB_ANIM / far-logic 3 alternation, FARANIM / CHARPOSE / FAMPOSE n=2 (by address phase)
//              every4  FARANIM / CHARPOSE / FAMPOSE n=4 (the shipped stage default, cpulod=3)
// Checks (fix on): every computation CUT makes (calc-anim and flush) leaves the joint matrices, envelopes, scale flags and the core's whole joint state
// (_0, _28, _50, _5C, _60, the freeze flags) byte for byte as REF's at the same point; the picture-exact rules (lean, bowl) also show REF's picture
// at every drawn turn; the logic state is equal after every movement.
// Measurement (fix off): per animation change, whether the blend started stale (CUT's last calc-anim left out), and over the blend, the joint rotation
// difference (degrees, local) and joint position difference (model units, joint matrices; the base is a translation) at CUT's computations.
// Kinds: real (70 % one track, 30 % two tracks, _20 = 1: every actor but the player), smooth (two tracks, _20 = 0.5: MarioWalk's smoothing), stress
// (also null track 0, _6 = 1 with freezeCopy, _6 = 2, and between a left-out computation and the change: changeInterpoleFrame, a track swapped, a weight).
// Mutants (each must fail with the fix on): 1 no catch-up at a change; 2 a catch-up from the live inputs instead of the recorded ones; 3 the flush not
// counted as the owed computation (made twice); 4 no catch-up before a computation that reads the last pose; 5 no chained catch-up; 6 the recorded frame
// ratio taken without the latch.
#define main par_anim_walk_main
#include "par_anim_walk.cpp"
#undef main
#include "port/pose_phase.h"
#include <JSystem/J3DGraphBase/J3DVertex.hpp>
#include <algorithm>
#include <chrono>

J3DDrawMtxData::J3DDrawMtxData() {
}
J3DDrawMtxData::~J3DDrawMtxData() {
}

namespace {

int gMut = 0;
bool gFix = true;
enum Rule { kPair, kPhase, kBowl, kLean, kEvery2, kEvery4, kRuleCount };
const char* kRuleName[kRuleCount] = {"pair", "phase", "bowl", "lean", "every2", "every4"};
int gRule = kPair;
const char* gRhythm = "pair";
const char* gKind = "real";
u32 gChangePct10 = 40;  // (per mille per actor per turn: a change of animation)

struct Player {
    f32 frame, rate, f84, i08, i0C;
    bool l88;
    s32 i14;
    u32 bck;
    f32 w[2];
};
struct Actor {
    Data* d;
    J3DModel* model;
    u8 tracks, mode6;
    Vec scale;
    Mtx base;
    J3DMtxBuffer* buf[2];
    XanimeCore* core[2];
    Player pl[2];
    // CUT window state (LiveActor: mPairSkip, mStaleAnim, mStaleXLatch)
    bool pairSkip, listed, staleLatch, skipped;  // skipped: CUT's last calc-anim was left out and nothing made it since
    u32 phase, skippedRun;
    bool nullTrack;
    // measurement
    bool open, openSeen, changedNow;
    u32 openTurn, openDur;
    f32 openOffset, openFirstErr;
    bool openFirst;
    double motionSum;
    u32 motionN;
    std::vector< XjointInfo > prevRef;
};

f32 QuatDeg(const Quaternion& a, const Quaternion& b) {
    f32 d = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d) * 57.29578f;
}

f32 Ratio(const Actor& a, int w, bool forRecord = false) {  // (XanimePlayer::calcAnm / PortCalcAnmRatio)
    const Player& p = a.pl[w];
    const f32 cur = (gMut == 6 && forRecord) ? p.frame : (p.l88 ? p.f84 : p.frame);
    s16 maxFrame = a.d->bcks[p.bck]->anm.getFrameMax();
    if (a.d->bcks[p.bck]->anm.getFrameMax() == 0) maxFrame = 1;
    return cur / maxFrame;
}

void CalcAnm(Actor& a, int w) {
    Player& p = a.pl[w];
    XanimeCore* c = a.core[w];
    c->mFrameRatio = Ratio(a, w);
    c->updateFrame();
    a.d->tree().getRootNode()->setMtxCalc(c);
    j3dSys.setModel(a.model);
    a.d->tree().calc(a.buf[w], a.scale, a.base);
    if (a.d->tree().getWEvlpMtxNum() != 0) {
        a.buf[w]->calcWeightEnvelopeMtx();
    }
    a.d->tree().getRootNode()->setMtxCalc(nullptr);
    p.l88 = false;
}

void UpdateInterp(Player& p) {
    if (p.rate != 0.0f) {
        if (p.i14 != 0) {
            p.i08 += (1.0f - p.i08) / static_cast< f32 >(p.i14);
            p.i14 -= 1;
        } else {
            p.i08 = 1.0f;
        }
        return;
    }
    p.i08 = 1.0f;
    p.i14 = 0;
}
void ChangeInterp(Player& p, XanimeCore* c, s32 k) {  // (XanimePlayer::changeInterpoleFrame)
    p.i14 = k;
    if (k == 0) {
        p.i08 = 1.0f;
        c->_1C = 1.0f;
        return;
    }
    p.i08 = 0.0f;
    c->_1C = 0.0f;
    UpdateInterp(p);
}

struct Ev {
    bool change, mutInterp, swap1, weight, nullToggle, baseMove;
    u32 newBck, newB1, swapB1;
    s32 interp, mutK;
    f32 rate, wv;
    Mtx nb;
};

void Movement(Actor& a, int w, const Ev& e, const Data* d) {
    Player& p = a.pl[w];
    XanimeCore* c = a.core[w];
    if (e.baseMove && w == 0) memcpy(a.base, e.nb, sizeof(Mtx));
    // ModelManager::update (XanimePlayer::updateAfterMovement)
    const f32 maxF = static_cast< f32 >(d->bcks[p.bck]->anm.mFrameMax);
    if (!p.l88) {
        p.f84 = p.frame;
        p.frame += p.rate;
        if (p.frame >= maxF) p.frame -= maxF;
        p.l88 = true;
    }
    UpdateInterp(p);
    for (u32 t = 0; t < a.tracks; t++) c->setWeight(static_cast< u8 >(t), p.w[t]);
    c->_1C = p.i08;
    c->_20 = p.i0C;
    // control()
    p.rate = e.rate;
    if (e.mutInterp) ChangeInterp(p, c, e.mutK);
    if (e.swap1 && a.tracks == 2 && !e.change) c->setBck(1, &d->bcks[e.swapB1]->anm);
    if (e.weight && a.tracks == 2) p.w[1] = e.wv;
    if (e.nullToggle && !e.change) {
        a.nullTrack = w == 1 ? a.nullTrack : !a.nullTrack;  // (toggled once per event, by world 0; world 1 follows)
        c->setBck(0, a.nullTrack ? nullptr : &d->bcks[p.bck]->anm);
    }
    if (e.change) {
        if (w == 1 && gFix) {
            if (gMut == 1) c->mPortOwed = 0;
            if (gMut == 2 && c->mPortOwed != 0) {  // (the catch-up given the live inputs)
                c->mPortOwedRatio = Ratio(a, 1, true);
                c->mPortOwed1C = c->_1C;
                c->mPortOwed20 = c->_20;
                for (u32 t = 0; t < c->mPortOwedTracks; t++) c->mPortOwedTrack[t] = c->mTrackList[t];
            }
        }
        c->doFreeze();
        if (a.mode6 == 1) c->freezeCopy(a.d->data, c, 0, 8);  // (MarioAnimator's: the frozen pose and the per-joint ramp)
        if (w == 0) a.nullTrack = false;
        c->setBck(0, &d->bcks[e.newBck]->anm);
        if (a.tracks == 2) c->setBck(1, &d->bcks[e.newB1]->anm);
        p.bck = e.newBck;
        p.frame = 0.0f;
        p.f84 = 0.0f;
        p.l88 = false;
        ChangeInterp(p, c, e.interp);
    }
}

bool Drawn(u32 t) {
    if (!strcmp(gRhythm, "pair")) return t % 2 == 0;
    if (!strcmp(gRhythm, "broken")) return t % 2 == 0 || Chance(12);
    if (!strcmp(gRhythm, "catchup")) return t % 2 == 0 && !Chance(15);
    if (!strcmp(gRhythm, "plain")) return true;
    return Chance(55);
}

bool SameMatrices(const Actor& a) {
    const u16 n = a.d->tree().getJointNum(), e = a.d->tree().getWEvlpMtxNum();
    if (memcmp(a.buf[0]->mpAnmMtx, a.buf[1]->mpAnmMtx, n * sizeof(Mtx)) != 0 || memcmp(a.buf[0]->mpScaleFlagArr, a.buf[1]->mpScaleFlagArr, n) != 0) return false;
    return e == 0 || (memcmp(a.buf[0]->mpWeightEvlpMtx, a.buf[1]->mpWeightEvlpMtx, e * sizeof(Mtx)) == 0 &&
                      memcmp(a.buf[0]->mpEvlpScaleFlagArr, a.buf[1]->mpEvlpScaleFlagArr, e) == 0);
}
bool SameCore(const Actor& a) {
    const XanimeCore* x = a.core[0];
    const XanimeCore* y = a.core[1];
    // (_29, "this computation made the freeze copy", is not compared: when the change came in the left-out turn's own movement, REF made the copy then
    // and CUT makes it in its next computation, from the same last pose (bowl31's change-on-drawn), so the joint state is the same bytes while that flag
    // differs until the next computation; its only other reader, GXV_ANIM_MEMO / GXV_PAR_ANIM's key, then just refuses a shortcut)
    return memcmp(x->mJointList, y->mJointList, x->mJointCount * sizeof(XjointInfo)) == 0 && x->_28 == y->_28;
}
bool SameLogic(const Actor& a) {
    const Player& x = a.pl[0];
    const Player& y = a.pl[1];
    if (memcmp(&x, &y, sizeof(Player)) != 0 || a.core[0]->_1C != a.core[1]->_1C || a.core[0]->_20 != a.core[1]->_20) return false;
    for (u32 t = 0; t < a.tracks; t++) {  // (field by field: the track array's padding is not initialised)
        const XanimeTrack& u = a.core[0]->mTrackList[t];
        const XanimeTrack& v = a.core[1]->mTrackList[t];
        if (u._0 != v._0 || u.mWeight != v.mWeight || u._8 != v._8 || u._C != v._C) return false;
    }
    return true;
}
void PoseError(const Actor& a, f32* deg, f32* pos) {
    *deg = 0.0f;
    *pos = 0.0f;
    for (u32 j = 0; j < a.core[0]->mJointCount; j++) {
        const f32 d = QuatDeg(a.core[0]->mJointList[j]._28.mRotation, a.core[1]->mJointList[j]._28.mRotation);
        if (d > *deg) *deg = d;
    }
    for (u16 j = 0; j < a.d->tree().getJointNum(); j++) {
        const f32 dx = a.buf[0]->mpAnmMtx[j][0][3] - a.buf[1]->mpAnmMtx[j][0][3], dy = a.buf[0]->mpAnmMtx[j][1][3] - a.buf[1]->mpAnmMtx[j][1][3],
                  dz = a.buf[0]->mpAnmMtx[j][2][3] - a.buf[1]->mpAnmMtx[j][2][3];
        const f32 p = sqrtf(dx * dx + dy * dy + dz * dz);
        if (p > *pos) *pos = p;
    }
}
f32 RefMotion(const Actor& a) {  // (REF's largest joint rotation from its last computation to this one: the joint's own speed per turn)
    f32 m = 0.0f;
    for (u32 j = 0; j < a.core[0]->mJointCount; j++) {
        const f32 d = QuatDeg(a.prevRef[j]._28.mRotation, a.core[0]->mJointList[j]._28.mRotation);
        if (d > m) m = d;
    }
    return m;
}

struct Result {
    u32 turns = 0, computed = 0, leftOut = 0, flushMade = 0;
    u32 changes = 0, interpChanges = 0, stale = 0, staleInterp = 0, staleAge[4] = {0, 0, 0, 0};
    u32 blendsOff = 0, offTurns = 0, maxDur = 0, offsetN = 0;
    double sumOffset = 0, sumOffsetTurns = 0, sumFirst = 0, sumFirstOffset = 0;
    f32 maxPos = 0, maxDeg = 0;
    u32 madeBad = 0, picBad = 0, logicBad = 0;
    u32 catchChange = 0, catchRead = 0, catchChain = 0, paid = 0, cleared = 0;
};

void CloseOpen(Actor& a, Result& r) {
    if (!a.open) return;
    a.open = false;
    if (a.openSeen) {
        ++r.blendsOff;
        r.offTurns += a.openDur;
        if (a.openDur > r.maxDur) r.maxDur = a.openDur;
        r.sumFirst += a.openFirstErr;
        r.sumFirstOffset += a.openOffset;
    }
}

// After CUT made a computation at the same point REF did: the check (fix on) or the measurement (fix off).
void AfterMade(Actor& a, Result& r) {
    const bool same = SameMatrices(a) && SameCore(a);
    if (!same) ++r.madeBad;
    if (!gFix && a.open) {
        bool pose = SameMatrices(a);  // (the pose: the matrices and the last pose _28; a frozen pose _0 left over from the blend is not seen)
        for (u32 j = 0; j < a.core[0]->mJointCount && pose; j++) {
            pose = memcmp(&a.core[0]->mJointList[j]._28, &a.core[1]->mJointList[j]._28, sizeof(XtransformInfo)) == 0;
        }
        if (pose) {
            CloseOpen(a, r);
        } else {
            a.openSeen = true;
            f32 deg, pos;
            PoseError(a, &deg, &pos);
            if (deg > r.maxDeg) r.maxDeg = deg;
            if (pos > r.maxPos) r.maxPos = pos;
            if (a.openFirst) {
                a.openFirst = false;
                a.openFirstErr = deg;
            }
        }
    }
}

Result Run(int turns, u32 seed) {
    gSeed = seed;
    std::vector< Data* > datas;
    for (int i = 0; i < 6; i++) datas.push_back(MakeData());
    const bool stress = !strcmp(gKind, "stress"), smooth = !strcmp(gKind, "smooth");
    std::vector< Actor* > actors;
    for (int i = 0; i < 24; i++) {
        Actor* a = new Actor();
        a->d = datas[Rand() % datas.size()];
        a->model = static_cast< J3DModel* >(calloc(1, sizeof(J3DModel)));
        a->model->mModelData = a->d->data;
        const u8 type4 = static_cast< u8 >(Rand() % 3);
        a->tracks = smooth || Chance(stress ? 50 : 30) ? 2 : 1;
        a->mode6 = stress ? static_cast< u8 >(Chance(15) ? 1 : Chance(10) ? 2 : Chance(20) ? 3 : 0) : (Chance(20) ? 3 : 0);
        a->scale.x = a->scale.y = a->scale.z = Chance(70) ? 1.0f : RandF(0.6f, 1.6f);
        PSMTXIdentity(a->base);
        a->phase = Rand() % 4;
        const u32 bck0 = Rand() % a->d->bcks.size();
        const u32 bck1 = Rand() % a->d->bcks.size();
        const u16 fixJoint = static_cast< u16 >(Rand() % a->d->tree().getJointNum());
        for (int w = 0; w < 2; w++) {
            a->buf[w] = MakeBuffer(a->d);
            XanimeCore* c = new XanimeCore(a->tracks, a->d->tree().getJointNum(), type4);
            c->_6 = a->mode6;
            c->_C = fixJoint;
            a->core[w] = c;
            Player& p = a->pl[w];
            memset(&p, 0, sizeof(p));
            p.bck = bck0;
            p.rate = 1.0f;
            p.i08 = 1.0f;
            p.i0C = smooth ? 0.5f : 1.0f;
            p.w[0] = a->tracks == 2 ? 0.6f : 1.0f;
            p.w[1] = 0.4f;
            c->setBck(0, &a->d->bcks[bck0]->anm);
            if (a->tracks == 2) c->setBck(1, &a->d->bcks[bck1]->anm);
            for (u32 t = 0; t < a->tracks; t++) c->setWeight(static_cast< u8 >(t), p.w[t]);
            c->_1C = 1.0f;
            c->_20 = p.i0C;
        }
        const u16 n = a->d->tree().getJointNum();
        memcpy(a->buf[1]->mpAnmMtx, a->buf[0]->mpAnmMtx, n * sizeof(Mtx));
        memcpy(a->buf[1]->mpScaleFlagArr, a->buf[0]->mpScaleFlagArr, n);
        a->core[0]->doFreeze();
        a->core[1]->doFreeze();
        CalcAnm(*a, 0);
        CalcAnm(*a, 1);
        a->prevRef.assign(a->core[0]->mJointList, a->core[0]->mJointList + a->core[0]->mJointCount);
        actors.push_back(a);
    }
    J3DSys::mParentS.x = J3DSys::mParentS.y = J3DSys::mParentS.z = 1.0f;
    gPortBlendFix = gFix ? 1 : 0;
    gPortBlendFixMeasure = 0;
    memset(&gPortBlendCensus, 0, sizeof(gPortBlendCensus));
    port::posephase::Phase phase;
    Result r;
    u32 seedEv = seed * 7919u + 13u;
    const bool picExact = gRule == kLean || gRule == kBowl;
    for (int t = 0; t < turns; t++) {
        ++r.turns;
        const bool drawn = Drawn(static_cast< u32 >(t));
        const bool skipTurn = phase.Begin(drawn);
        if (drawn) {
            // the flush (port_anim_flush_stale): a listed actor is made from the restored latch; with the fix, that computation is the owed one
            for (Actor* a : actors) {
                if (!a->listed) continue;
                a->listed = false;
                a->pl[1].l88 = a->staleLatch;
                if (gFix && gMut != 3 && a->core[1]->mPortOwed != 0) {
                    a->core[1]->mPortOwed = 2;  // (LiveActor.cpp port_anim_flush_stale: the computation below is the owed one)
                }
                CalcAnm(*a, 1);
                a->skipped = false;
                a->skippedRun = 0;
                ++r.flushMade;
                AfterMade(*a, r);  // (REF made it at the end of the last turn)
            }
            if (picExact) {
                for (Actor* a : actors) {
                    if (!SameMatrices(*a)) ++r.picBad;
                }
            }
        }
        for (Actor* a : actors) {
            gSeed = seedEv;
            Ev e;
            memset(&e, 0, sizeof(e));
            e.change = Rand() % 1000 < gChangePct10;
            e.newBck = Rand() % a->d->bcks.size();
            e.newB1 = Rand() % a->d->bcks.size();
            e.interp = Chance(30) ? 0 : static_cast< s32 >(1 + Rand() % 12);
            e.rate = Chance(10) ? RandF(0.3f, 2.0f) : a->pl[0].rate;
            e.mutInterp = stress && Chance(3);
            e.mutK = static_cast< s32 >(Rand() % 6);
            e.swap1 = stress && Chance(3);
            e.swapB1 = Rand() % a->d->bcks.size();
            e.weight = stress && Chance(3);
            e.wv = Chance(20) ? 0.0f : RandF(0.1f, 0.9f);
            e.nullToggle = stress && Chance(2);
            e.baseMove = Chance(50);
            PSMTXIdentity(e.nb);
            if (stress) {
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++) e.nb[i][j] = RandF(-1.5f, 1.5f);
            }
            e.nb[0][3] = RandF(-400.0f, 400.0f);
            e.nb[1][3] = RandF(-400.0f, 400.0f);
            e.nb[2][3] = RandF(-400.0f, 400.0f);
            seedEv = gSeed;
            a->changedNow = e.change;
            if (e.change) {
                ++r.changes;
                if (e.interp > 0) ++r.interpChanges;
                if (a->skipped) {
                    ++r.stale;
                    if (e.interp > 0) ++r.staleInterp;
                    ++r.staleAge[a->skippedRun >= 3 ? 3 : a->skippedRun];
                    if (!gFix && e.interp > 0) {
                        CloseOpen(*a, r);
                        a->open = true;
                        a->openSeen = false;
                        a->openFirst = true;
                        a->openTurn = static_cast< u32 >(t);
                        a->openDur = 0;
                        // the blend's start as it is (CUT: the last computation made) against the true one (REF: the last turn's), and that in
                        // turns of the actor's own joint motion
                        f32 off = 0.0f;
                        for (u32 j = 0; j < a->core[0]->mJointCount; j++) {
                            const f32 dd = QuatDeg(a->core[0]->mJointList[j]._28.mRotation, a->core[1]->mJointList[j]._28.mRotation);
                            if (dd > off) off = dd;
                        }
                        a->openOffset = off;
                        if (off > 0.0f) {
                            ++r.offsetN;
                            r.sumOffset += off;
                            if (a->motionN != 0 && a->motionSum > 0.0) r.sumOffsetTurns += off / (a->motionSum / a->motionN);
                        }
                    }
                }
            }
            for (int w = 0; w < 2; w++) Movement(*a, w, e, a->d);
            if (!SameLogic(*a)) ++r.logicBad;
        }
        for (Actor* a : actors) {
            a->prevRef.assign(a->core[0]->mJointList, a->core[0]->mJointList + a->core[0]->mJointCount);
            CalcAnm(*a, 0);
            if (!a->changedNow && a->core[0]->_1C >= 1.0f) {  // (the actor's own joint speed, a turn without a change or a blend)
                a->motionSum += RefMotion(*a);
                ++a->motionN;
            }
            bool skip = false;
            switch (gRule) {
            case kPair:
            case kPhase:
            case kBowl:
                skip = skipTurn && !a->pairSkip;
                a->pairSkip = skip;
                break;
            case kLean:
                skip = !drawn;
                break;
            case kEvery2:
                skip = (static_cast< u32 >(t) + a->phase) % 2 != 0;
                break;
            case kEvery4:
                skip = (static_cast< u32 >(t) + a->phase) % 4 != 0;
                break;
            }
            if (skip) {
                ++r.leftOut;
                XanimeCore* c = a->core[1];
                if (gRule == kLean || gRule == kBowl) {
                    a->staleLatch = a->pl[1].l88;
                    a->listed = true;
                }
                if (gFix) {
                    if (gMut == 5 && c->mPortOwed != 0) c->mPortOwed = 0;
                    c->portLeaveOut(Ratio(*a, 1, true));  // (XanimePlayer::portLeaveOut)
                }
                a->pl[1].l88 = false;
                a->skipped = true;
                ++a->skippedRun;
            } else {
                if (gFix && gMut == 4) a->core[1]->mPortOwed = 0;
                a->listed = false;  // (made now: the flush passes the entry by, as LiveActor.cpp's bowl entries; lean never makes a listed one)
                CalcAnm(*a, 1);
                a->skipped = false;
                a->skippedRun = 0;
                ++r.computed;
                AfterMade(*a, r);
            }
            if (!gFix && a->open && a->openSeen) {
                ++a->openDur;  // (turns the blend has been off, counted while it is open)
            }
        }
    }
    for (Actor* a : actors) CloseOpen(*a, r);
    r.catchChange = gPortBlendCensus.atChange;
    r.catchRead = gPortBlendCensus.atRead;
    r.catchChain = gPortBlendCensus.chained + gPortBlendCensus.eager + gPortBlendCensus.atCopy;
    r.paid = gPortBlendCensus.paid;
    r.cleared = gPortBlendCensus.cleared;
    return r;
}

// Cost on this host: the recording at a left-out computation, the catch-up, and a full computation, ns each (median of a few runs).
void Cost() {
    gSeed = 99;
    Data* d = MakeData();
    while (d->tree().getJointNum() < 30) d = MakeData();
    J3DModel* model = static_cast< J3DModel* >(calloc(1, sizeof(J3DModel)));
    model->mModelData = d->data;
    J3DMtxBuffer* buf = MakeBuffer(d);
    XanimeCore* c = new XanimeCore(1, d->tree().getJointNum(), 2);
    c->setBck(0, &d->bcks[0]->anm);
    c->setWeight(0, 1.0f);
    Vec s = {1, 1, 1};
    Mtx base;
    PSMTXIdentity(base);
    gPortBlendFix = 1;
    const int N = 4000;
    auto now = [] { return std::chrono::steady_clock::now(); };
    double full = 1e30, rec = 1e30, both = 1e30;  // (the fastest of 9 rounds each: the host is shared)
    for (int round = 0; round < 9; round++) {
        auto t0 = now();
        for (int i = 0; i < N; i++) {
            c->mFrameRatio = (i % 50) / 50.0f;
            c->updateFrame();
            d->tree().getRootNode()->setMtxCalc(c);
            j3dSys.setModel(model);
            d->tree().calc(buf, s, base);
            d->tree().getRootNode()->setMtxCalc(nullptr);
        }
        auto t1 = now();
        for (int i = 0; i < N; i++) {
            c->portLeaveOut((i % 50) / 50.0f);
            c->mPortOwed = 0;
        }
        auto t2 = now();
        for (int i = 0; i < N; i++) {
            c->portLeaveOut((i % 50) / 50.0f);
            c->portCatchUp(kPortCatchChange);
        }
        auto t3 = now();
        full = std::min(full, std::chrono::duration< double, std::nano >(t1 - t0).count() / N);
        rec = std::min(rec, std::chrono::duration< double, std::nano >(t2 - t1).count() / N);
        both = std::min(both, std::chrono::duration< double, std::nano >(t3 - t2).count() / N);
    }
    printf("cost (host, %u joints, one track): full computation %.0f ns; recording at a left-out computation %.1f ns (%.2f %% of it); catch-up %.0f ns (%.0f %%)\n",
           static_cast< unsigned >(d->tree().getJointNum()), full, rec, 100.0 * rec / full, both - rec, 100.0 * (both - rec) / full);
}

}  // namespace

int main(int argc, char** argv) {
    const int turns = argc > 1 ? atoi(argv[1]) : 1500;
    const u32 seed = argc > 2 ? static_cast< u32 >(atoi(argv[2])) : 4242u;
    gKind = argc > 3 ? argv[3] : "real";
    const char* ruleArg = argc > 4 ? argv[4] : "all";
    gFix = argc > 5 ? atoi(argv[5]) != 0 : true;
    gMut = argc > 6 ? atoi(argv[6]) : 0;
    gChangePct10 = argc > 7 ? static_cast< u32 >(atoi(argv[7])) : 40;
    if (!strcmp(gKind, "cost")) {
        Cost();
        return 0;
    }
    int fail = 0;
    const char* rhythms[] = {"pair", "broken", "catchup", "plain", "random"};
    for (int rule = 0; rule < kRuleCount; rule++) {
        if (strcmp(ruleArg, "all") != 0 && strcmp(ruleArg, kRuleName[rule]) != 0) continue;
        gRule = rule;
        for (const char* rh : rhythms) {
            gRhythm = rh;
            const Result r = Run(turns, seed);
            if (gFix) {
                const bool ok = r.madeBad == 0 && r.picBad == 0 && r.logicBad == 0;
                printf("%-6s %-6s %-8s fix  left out %6u, changes %4u (stale start %4u: %u interpolated) | catch-ups: change %4u read %4u chain %3u, flush %4u | "
                       "computations off %u, pictures off %u, logic off %u%s\n",
                       gKind, kRuleName[rule], rh, r.leftOut, r.changes, r.stale, r.staleInterp, r.catchChange, r.catchRead, r.catchChain, r.paid, r.madeBad,
                       r.picBad, r.logicBad, ok ? "" : "  <-- FAIL");
                if (!ok) ++fail;
            } else {
                // measurement (as before): stale starts and how far off
                printf("%-6s %-6s %-8s old  left out %6u, changes %4u (interpolated %4u), stale start %4u = %.0f %% (ages 1/2/3+ %u/%u/%u; interpolated %u) | "
                       "start off by %.2f turns of the joint's own motion (mean), CUT's first computation after it off by %.2f x the start offset; blends off %u, "
                       "%.1f turns each (max %u) | computations off %u, pictures off %u\n",
                       gKind, kRuleName[rule], rh, r.leftOut, r.changes, r.interpChanges, r.stale, r.changes ? 100.0 * r.stale / r.changes : 0.0,
                       r.staleAge[1], r.staleAge[2], r.staleAge[3], r.staleInterp, r.offsetN ? r.sumOffsetTurns / r.offsetN : 0.0,
                       r.sumFirstOffset > 0.0 ? r.sumFirst / r.sumFirstOffset : 0.0, r.blendsOff, r.blendsOff ? static_cast< double >(r.offTurns) / r.blendsOff : 0.0,
                       r.maxDur, r.madeBad, r.picBad);
                if (r.madeBad != 0 || r.picBad != 0) ++fail;  // (fix off: a difference is the finding; the script expects it)
            }
        }
    }
    printf("blendfix %s %s fix %d mutant %d: %s\n", gKind, ruleArg, gFix ? 1 : 0, gMut, fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
