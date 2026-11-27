// GXV_POSE_ONDRAW host proof (ondraw31; tools/test_ondraw.sh): the every-class pose rule (port/pose_ondraw.h, LiveActor.cpp PortOnDraw, the flush, the
// read catch-up, XanimeCore::portRemake) against the game's own XanimeCore (with blendfix31's record and catch-ups) and J3D joint code, byte for byte,
// over the turn and picture rhythm of GameSystem::frameLoop.
//
// Built as tests/bowl_pose.cpp and tests/blendfix.cpp (bowl31's two-world harness): tests/par_anim_walk.cpp's models (random joint trees, BCKs, the three
// core types, fixT, envelopes), linked whole with the game units. Two worlds of the same actors run the same logic from one event stream: REF poses every
// turn (the game as it is); CUT applies the rule as LiveActor.cpp does. Representative classes, each with the readers its kind has in the game:
//   npc     one or two tracks, changes animation often (any turn, also the turn after a skip turn: blendfix31's catch-up), a sensor following its base
//           matrix (it reads the base in every movement and what it reads feeds its logic), and FIRST READS of its joints at random moments (a talk
//           camera, another actor taking it): during the rest of the skip turn (A), in the next turn before its own movement (B) and after it (C)
//   enemy   half of them have a sensor following a joint (handed out at init: the read mark refuses them for good), the rest a base sensor; animation
//           changes on events, and changes SENT by another actor before their own movement (a stomp: doFreeze before update)
//   item    one animation, rate changes, an effect on its base, collected (hidden, not drawn, no calc-anim) and back
//   deco    one animation, no reader
//   parts   an attachment (PartsModel) following an enemy's joint 0 (that enemy is marked): its own base is made from the host's joint every calc-anim
// Plus facts only: a few actors with a joint callback (it writes game state while the joints are made: modelled as a logic write in the computation)
// and a few with a shared core (MarioAnimator's): both must be refused.
// Every turn: drawn turn: the flush (CUT: owed poses, exact from the skip turn's state or counted drift), then the PICTURE: the joint and envelope
// matrices and scale flags of every shown actor must be equal byte for byte; movement (both worlds, the same events; readers read); calc-anim.
// Checks: the picture; the logic state after every movement (clock, interpolation, core inputs, the logic fed by readers); every value a first reader
// read; every computation both worlds made at the same point (matrices and the core's whole joint state). A difference must have been ANNOUNCED (LATE,
// drift) to pass, and in the "classes" scenario nothing may be announced at all.
// Scenarios: classes (all of the above; a first read never follows an animation change in the same window: the record path is exact), late (a sent
// change and then a first read in the same window: the read cannot be made exactly and must be announced LATE), drift (an actor's calc-anim switched
// off in its own movement while its pose is owed: the flush makes a pose from the state then and must announce it).
// Mutants (each must fail): 1 the base not made on the skip turn; 2 the read mark ignored; 3 no catch-up at a first read; 4 a first read made from the
// state now whatever changed (no LATE); 5 no flush for a broken pair; 6 blendfix off; 7 every drawn turn taken for a skip turn; 8 the record's catch-up made from
// the live inputs; 9 the latch not restored by the flush; 10 the flush passes over an owed pose whose calc-anim was switched off (bowl31's condition);
// 11 the joint-callback fact ignored; 12 the visibility animation (BVA) not made with the joints from the record (a stale visibility on the
// picture); 13 the materials of a pose made from the record taken at the frames now under GXV_DL_LAZY 0 / 1; 14 an owed pose made for a reader
// shown without its materials being made as the skip turn's (the material flush makes them from the frames then); 15 the =2 verify's state taken before
// the base (emufix31: an attachment's base writes its position, which mode 1 makes too: false mismatches); 16 the state taken after the joints (the
// verify sees nothing: its negative control, a joint callback's write, is missed).
// Materials and visibility (ondraw31 review): each actor has a material / visibility clock (BTK..BVA) that update() moves; ModelManager::calc applies
// the BVA with the joints; the materials follow GXV_DL_LAZY: 2 (the default) leaves every turn's to the drawn turn's material flush (from the
// frames then), 1 makes them in calc-anim on a drawn turn and leaves an undrawn turn's to that flush. Every rhythm runs with both.
#define main par_anim_walk_main
#include "par_anim_walk.cpp"
#undef main
#include "port/pose_ondraw.h"
#include "port/pose_phase.h"
#include <JSystem/J3DGraphBase/J3DVertex.hpp>
#include <cmath>

J3DDrawMtxData::J3DDrawMtxData() {
}
J3DDrawMtxData::~J3DDrawMtxData() {
}

namespace {
namespace od = ::port::ondraw;

int gMut = 0;
const char* gRhythm = "pair";
const char* gScen = "classes";
int gLazy = 2;           // GXV_DL_LAZY
bool gTurnDrawn = true;  // this turn draws a picture (port_turn_skipped_raw() == 0)
f32 MatOf(f32 f) { return sinf(f * 0.37f) * 3.0f + f; }       // (calcMaterial at a frame)
f32 BvaOf(f32 f) { return static_cast< f32 >(static_cast< int >(f) % 7 < 4 ? 1 : 0); }  // (a visibility track)
enum Cls { kNpc, kEnemy, kItem, kDeco, kParts, kClsN };
const char* const kClsName[kClsN] = {"npc", "enemy", "item", "deco", "parts"};

struct Player {
    f32 frame, rate, f84, i08, i0C;
    bool l88;
    s32 i14;
    u32 bck;
    f32 w[2];
};
struct Logic {
    f32 x, y, z, yaw, acc;
    f32 px, py, pz;  // (emufix31) an attachment's mPosition: PartsModel::calcAndSetBaseMtx writes it from its host's joint (the base computation)
};
struct Actor {
    int cls;
    Data* d;
    J3DModel* model;
    u8 tracks;
    Vec scale;
    Mtx base[2];
    J3DMtxBuffer* buf[2];
    XanimeCore* core[2];
    Player pl[2];
    Logic lg[2];
    bool baseReader, jointSensor, callback, shared;
    bool marked;  // J3DModel::mPortBowl & 1 (sticky)
    Actor* host;
    bool noCalc, hidden;
    f32 mat[2], matRate;      // the material / visibility players' frame (ModelManager::update moves it)
    f32 matOut[2], bvaOut[2]; // what the picture shows: the materials (updateDL) and the visibility (BvaPlayer::calc)
    bool dlStale[2];          // GXV_DL_LAZY: listed for the drawn turn's material flush
    f32 matRec;               // CUT: the frame the left-out computation found (OdEntry::mframe)
    // CUT window (LiveActor: mBowlPose 0x80 / 0x08, mStaleXLatch; OdEntry)
    bool pending, made, remade, staleLatch;
    u32 xhash, bhash;
    bool tainted;  // a difference was announced and the worlds have not met again
};

struct Result {
    u32 pictures = 0, skipTurns = 0, leftOut[kClsN] = {}, refusedRead = 0, refusedCallback = 0, refusedShared = 0;
    u32 owedNext = 0, owedFlush = 0, owedRead = 0, remade = 0, notDrawn = 0, drift = 0, flushRecord = 0, late = 0, reads = 0;
    u32 picBad = 0, logicBad = 0, readerBad = 0, madeBad = 0, unannounced = 0;
    u32 blendCatch = 0;
    u32 vChecked = 0, vParts = 0, vFalse = 0, vControl = 0, vBase = 0;  // (emufix31: the =2 verify's state check, see VerifyCalc)
};

f32 Ratio(const Actor& a, int w) {
    const Player& p = a.pl[w];
    const f32 cur = p.l88 ? p.f84 : p.frame;
    s16 maxFrame = a.d->bcks[p.bck]->anm.getFrameMax();
    if (maxFrame == 0) maxFrame = 1;
    return cur / maxFrame;
}
void TreeCalc(Actor& a, int w) {
    j3dSys.setModel(a.model);
    a.d->tree().calc(a.buf[w], a.scale, a.base[w]);
    if (a.d->tree().getWEvlpMtxNum() != 0) {
        a.buf[w]->calcWeightEnvelopeMtx();
    }
}
// XanimePlayer::calcAnm + J3DModel::calc (ModelManager::calc): the joints from the latch's frame, the core on the root joint.
void CalcAnm(Actor& a, int w) {
    Player& p = a.pl[w];
    XanimeCore* c = a.core[w];
    c->mFrameRatio = Ratio(a, w);
    c->updateFrame();
    a.d->tree().getRootNode()->setMtxCalc(c);
    TreeCalc(a, w);
    a.d->tree().getRootNode()->setMtxCalc(nullptr);
    p.l88 = false;
    if (a.callback) {
        a.lg[w].acc += 0.25f;  // (a joint callback writes game state while the joints are made)
    }
    a.bvaOut[w] = BvaOf(a.mat[w]);  // (ModelManager::calc: the BVA with the joints)
    if (gLazy == 2 || !gTurnDrawn) {
        a.dlStale[w] = true;  // (ModelManager::calcAnim: left to the drawn turn's material flush)
    } else {
        a.matOut[w] = MatOf(a.mat[w]);
        a.dlStale[w] = false;
    }
}
// LiveActor.cpp OdJointsFromRecord's visibility and OdMaterialsForPicture (CUT world): the BVA at the recorded frame; the materials as the skip turn
// makes them.
void BvaFromRecord(Actor& a) {
    if (gMut != 12) a.bvaOut[1] = BvaOf(a.matRec);
}
void MaterialsForPicture(Actor& a) {
    a.matOut[1] = MatOf(gLazy == 2 || gMut == 13 ? a.mat[1] : a.matRec);
    a.dlStale[1] = false;
}
// calcAndSetBaseMtx (the class's own): an attachment follows its host's joint 0 as the host's last computation left it; the others their position.
void CalcBase(Actor& a, int w) {
    if (a.cls == kParts) {
        memcpy(a.base[w], a.host->buf[w]->mpAnmMtx[0], sizeof(Mtx));
        a.base[w][1][3] += 12.0f;
        a.lg[w].px = a.base[w][0][3], a.lg[w].py = a.base[w][1][3], a.lg[w].pz = a.base[w][2][3];  // (mPosition.set(mMtx[.][3]): logic state)
        return;
    }
    const f32 c = cosf(a.lg[w].yaw), s = sinf(a.lg[w].yaw);
    Mtx& m = a.base[w];
    m[0][0] = c, m[0][1] = 0, m[0][2] = s, m[0][3] = a.lg[w].x;
    m[1][0] = 0, m[1][1] = 1, m[1][2] = 0, m[1][3] = a.lg[w].y;
    m[2][0] = -s, m[2][1] = 0, m[2][2] = c, m[2][3] = a.lg[w].z;
}
void FullCalc(Actor& a, int w) {
    CalcBase(a, w);
    CalcAnm(a, w);
}
u32 XHash(const Actor& a) {  // (LiveActor.cpp OdXHash)
    od::Hash h;
    h.Words(a.base[1], sizeof(Mtx));
    h.Words(&a.scale, sizeof(Vec));
    h.F32(a.mat[1]);  // (the material / visibility players' frames: OdHashPlayers)
    const Player& p = a.pl[1];
    h.F32(p.frame);
    h.F32(p.rate);
    h.F32(p.f84);
    h.U32(p.l88 ? 1u : 0u);
    h.U32(p.bck);
    const XanimeCore* c = a.core[1];
    h.F32(c->_1C);
    h.F32(c->_20);
    h.U32(c->_6);
    for (u32 t = 0; t < c->mTrackCount; ++t) {
        h.U32(static_cast< u32 >(reinterpret_cast< uintptr_t >(c->mTrackList[t]._0)));
        h.F32(c->mTrackList[t].mWeight);
        h.F32(c->mTrackList[t]._8);
        h.U32(c->mTrackList[t]._C);
    }
    return h.h;
}
u32 BHash(const Actor& a) {  // (OdBaseHash)
    od::Hash h;
    h.Words(a.base[1], sizeof(Mtx));
    h.Words(&a.scale, sizeof(Vec));
    return h.h;
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
void ChangeInterp(Player& p, XanimeCore* c, s32 k) {
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
void Change(Actor& a, int w, u32 bck, u32 b1, s32 interp) {  // (changeAnimationSimple / prepareAnimation)
    Player& p = a.pl[w];
    XanimeCore* c = a.core[w];
    c->doFreeze();  // (blendfix31: a left-out computation's joint state is made here first)
    c->setBck(0, &a.d->bcks[bck]->anm);
    if (a.tracks == 2) c->setBck(1, &a.d->bcks[b1]->anm);
    p.bck = bck;
    p.frame = 0.0f;
    p.f84 = 0.0f;
    p.l88 = false;
    ChangeInterp(p, c, interp);
}
void Update(Actor& a, int w) {  // (ModelManager::update: XanimePlayer::updateAfterMovement, the interpolation, the core's weights)
    Player& p = a.pl[w];
    XanimeCore* c = a.core[w];
    const f32 maxF = static_cast< f32 >(a.d->bcks[p.bck]->anm.mFrameMax);
    if (!p.l88) {
        p.f84 = p.frame;
        p.frame += p.rate;
        if (p.frame >= maxF) p.frame -= maxF;
        p.l88 = true;
    }
    UpdateInterp(p);
    a.mat[w] += a.matRate;
    if (a.mat[w] >= 40.0f) a.mat[w] -= 40.0f;
    for (u32 t = 0; t < a.tracks; t++) c->setWeight(static_cast< u8 >(t), p.w[t]);
    c->_1C = p.i08;
    c->_20 = p.i0C;
}

struct Ev {
    bool change, msg, readA, readB, readC, hide, show, offCalc, onCalc;
    u32 bck, b1, mbck, mb1;
    s32 interp, minterp;
    f32 rate, dx, dyaw;
};

bool SamePicture(const Actor& a) {  // (the materials and the visibility the picture shows)
    return a.matOut[0] == a.matOut[1] && a.bvaOut[0] == a.bvaOut[1];
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
    return memcmp(x->mJointList, y->mJointList, x->mJointCount * sizeof(XjointInfo)) == 0 && x->_28 == y->_28;
}
bool SameLogic(const Actor& a) {
    const Player& x = a.pl[0];
    const Player& y = a.pl[1];
    if (memcmp(&x, &y, sizeof(Player)) != 0 || memcmp(&a.lg[0], &a.lg[1], sizeof(Logic)) != 0) return false;
    if (a.core[0]->_1C != a.core[1]->_1C || a.core[0]->_20 != a.core[1]->_20) return false;
    for (u32 t = 0; t < a.tracks; t++) {
        const XanimeTrack& u = a.core[0]->mTrackList[t];
        const XanimeTrack& v = a.core[1]->mTrackList[t];
        if (u._0 != v._0 || u.mWeight != v.mWeight || u._8 != v._8 || u._C != v._C) return false;
    }
    return true;
}
void Bad(Actor& a, u32& counter, Result& r) {
    ++counter;
    if (!a.tainted) ++r.unannounced;
}

// The record's catch-up (LiveActor.cpp OdRemake -> XanimeCore::portRemake): the joint tree on the recorded inputs.
struct RemakeCtx {
    Actor* a;
};
void RemakeCalc(void* p) {
    Actor& a = *static_cast< RemakeCtx* >(p)->a;
    TreeCalc(a, 1);
}

// A first reader of an actor's joints (LiveActor.cpp OdRead, mode 1; the read marks the model for good).
void FirstRead(Actor& a, Result& r) {
    if (a.marked || a.hidden) return;
    ++r.reads;
    a.marked = true;
    if (a.pending) {
        bool made = false;
        if (gMut == 4) {
            CalcAnm(a, 1);  // (mutant: made from the state now, whatever changed)
            made = true;
        } else if (gMut != 3 && XHash(a) == a.xhash) {
            if (a.staleLatch) a.pl[1].l88 = true;
            if (a.core[1]->mPortOwed != 0) a.core[1]->mPortOwed = 2;
            CalcAnm(a, 1);
            made = true;
        } else if (gMut != 3 && BHash(a) == a.bhash) {
            if (gMut == 8) {
                CalcAnm(a, 1);  // (mutant: the live inputs instead of the record)
                made = true;
            } else {
                RemakeCtx ctx{&a};
                made = a.core[1]->portRemake(&RemakeCalc, &ctx, a.d->tree().getRootNode());
                if (made) {
                    BvaFromRecord(a);
                    a.remade = true;
                    ++r.remade;
                }
            }
        }
        if (made) {
            a.pending = false;
            a.made = true;
            ++r.owedRead;
        } else if (gMut != 3) {
            ++r.late;
            a.tainted = true;  // (LATE: announced)
        }
    }
    if (memcmp(a.buf[0]->mpAnmMtx[0], a.buf[1]->mpAnmMtx[0], sizeof(Mtx)) != 0) Bad(a, r.readerBad, r);
}

// The GXV_POSE_ONDRAW=2 verify's state check (LiveActor.cpp: OdStateHash), in the REF world, on a skip turn, for an actor the rule would take: the state
// before the computation that mode 1 leaves out must equal the state after it. Mode 1 makes the base on the skip turn (CalcBase), so the state is taken
// between the base and the joints (calcAnmMtx: PortOnDrawBaseMade). emufix31: before, it was taken before the base, and an attachment (PartsModel: an
// NPC's goods in Honeyhive's opening camera) printed "the calc-anim changed the logic state" because its base writes its position from the host's joint;
// mode 1 writes it the same. Negative control: the joint-callback actors (refused by the rule; their callback writes game state while the joints are
// made) are checked here too, as if the callback fact were not known: the check must see them. Mutants: 15 the state taken before the base (the old
// point: false mismatches on the parts); 16 taken after the joints (sees nothing); 17 (fable31fix) the second hash, before the base, dropped (what the base writes is not reported).
u32 VHash(const Actor& a, int w) {
    od::Hash h;
    h.Words(&a.lg[w], sizeof(Logic));
    h.F32(a.pl[w].frame);
    h.F32(a.pl[w].rate);
    h.F32(a.pl[w].f84);
    h.U32(a.pl[w].bck);
    return h.h;
}
void VerifyCalc(Actor& a, bool skip, Result& r) {
    od::Facts f;
    memset(&f, 0, sizeof(f));
    f.skipTurn = skip;
    f.gate = f.list = f.blendFixOn = f.posedOnce = true;
    f.jointsRead = a.marked;
    f.sharedCore = a.shared;
    f.readsLast = a.core[0]->portReadsLast() || a.core[0]->mTrackCount > XanimeCore::kPortOwedTracks;
    if (od::Refuse(f) >= 0) {  // (the callback fact left out on purpose: the negative control)
        FullCalc(a, 0);
        return;
    }
    const u32 before = VHash(a, 0);
    CalcBase(a, 0);
    const u32 mid = VHash(a, 0);
    CalcAnm(a, 0);
    const u32 after = VHash(a, 0);
    const u32 from = gMut == 15 ? before : gMut == 16 ? after : mid;
    ++r.vChecked;
    // fable31fix: the state BEFORE the base is hashed too and reported apart ("base-written state differs": LiveActor.cpp OdEntry::hashPre, Census::baseWrote). Not a
    // mismatch (mode 1 makes the base too), but the verify shows what calcAndSetBaseMtx writes. Mutant 17: that second hash dropped (the count stays 0).
    if (gMut != 17 && !a.callback && mid != before && after == mid) ++r.vBase;
    if (from == after) return;
    if (a.callback) ++r.vControl;
    else {
        ++r.vFalse;
        if (a.cls == kParts) ++r.vParts;
    }
}

bool Drawn(u32 t) {
    if (!strcmp(gRhythm, "pair")) return t % 2 == 0;
    if (!strcmp(gRhythm, "broken")) return t % 2 == 0 || Chance(12);
    if (!strcmp(gRhythm, "catchup")) return t % 2 == 0 && !Chance(15);
    if (!strcmp(gRhythm, "plain")) return true;
    return Chance(55);
}

Result Run(int turns, u32 seed) {
    gSeed = seed;
    const bool late = !strcmp(gScen, "late"), drift = !strcmp(gScen, "drift");
    std::vector< Data* > datas;
    for (int i = 0; i < 6; i++) datas.push_back(MakeData());
    std::vector< Actor* > actors;
    const int counts[kClsN] = {7, 7, 6, 5, 3};
    std::vector< Actor* > hosts;
    for (int cls = 0; cls < kClsN; cls++) {
        for (int i = 0; i < counts[cls]; i++) {
            Actor* a = new Actor();
            a->cls = cls;
            a->d = datas[Rand() % datas.size()];
            a->model = static_cast< J3DModel* >(calloc(1, sizeof(J3DModel)));
            a->model->mModelData = a->d->data;
            const u8 type4 = static_cast< u8 >(Rand() % 3);
            a->tracks = (cls == kNpc || cls == kEnemy) && Chance(35) ? 2 : 1;
            a->scale.x = a->scale.y = a->scale.z = Chance(70) ? 1.0f : RandF(0.6f, 1.6f);
            a->baseReader = cls == kNpc || cls == kItem || (cls == kEnemy && i % 2 == 1) || cls == kParts;
            a->jointSensor = cls == kEnemy && i % 2 == 0;
            a->marked = a->jointSensor;  // (MR::getJointMtx at init: marked before any skip turn)
            a->callback = (cls == kNpc && i == 0) || (cls == kDeco && i == 0);
            a->shared = cls == kEnemy && i == 1;
            if (cls == kEnemy && a->jointSensor) hosts.push_back(a);
            a->host = cls == kParts ? hosts[i % hosts.size()] : nullptr;
            const u32 bck0 = Rand() % a->d->bcks.size(), bck1 = Rand() % a->d->bcks.size();
            const u16 fixJoint = static_cast< u16 >(Rand() % a->d->tree().getJointNum());
            const f32 x = RandF(-2000, 2000), z = RandF(-2000, 2000), yaw = RandF(-3, 3);
            const u8 mode6 = Chance(20) ? 3 : 0;
            a->matRate = Chance(20) ? 0.0f : RandF(0.4f, 1.6f);
            a->mat[0] = a->mat[1] = RandF(0, 40);
            for (int w = 0; w < 2; w++) {
                a->buf[w] = MakeBuffer(a->d);
                XanimeCore* c = new XanimeCore(a->tracks, a->d->tree().getJointNum(), type4);
                c->_6 = mode6;
                c->_C = fixJoint;
                a->core[w] = c;
                Player& p = a->pl[w];
                memset(&p, 0, sizeof(p));
                p.bck = bck0;
                p.rate = 1.0f;
                p.i08 = 1.0f;
                p.i0C = 1.0f;
                p.w[0] = a->tracks == 2 ? 0.6f : 1.0f;
                p.w[1] = 0.4f;
                c->setBck(0, &a->d->bcks[bck0]->anm);
                if (a->tracks == 2) c->setBck(1, &a->d->bcks[bck1]->anm);
                for (u32 t = 0; t < a->tracks; t++) c->setWeight(static_cast< u8 >(t), p.w[t]);
                c->_1C = 1.0f;
                c->_20 = 1.0f;
                a->lg[w] = Logic{x, 0.0f, z, yaw, 0.0f};
            }
            const u16 n = a->d->tree().getJointNum();
            memcpy(a->buf[1]->mpAnmMtx, a->buf[0]->mpAnmMtx, n * sizeof(Mtx));
            memcpy(a->buf[1]->mpScaleFlagArr, a->buf[0]->mpScaleFlagArr, n);
            a->core[0]->doFreeze();
            a->core[1]->doFreeze();
            actors.push_back(a);
        }
    }
    gTurnDrawn = true;
    for (Actor* a : actors) {  // (the first computation: never left out (posed once); hosts first, as the list order below)
        FullCalc(*a, 0);
        FullCalc(*a, 1);
        for (int w = 0; w < 2; w++) {
            a->matOut[w] = MatOf(a->mat[w]);
            a->dlStale[w] = false;
        }
    }
    J3DSys::mParentS.x = J3DSys::mParentS.y = J3DSys::mParentS.z = 1.0f;
    gPortBlendFix = gMut == 6 ? 0 : 1;
    gPortBlendFixMeasure = 0;
    memset(&gPortBlendCensus, 0, sizeof(gPortBlendCensus));
    port::posephase::Phase phase;
    Result r;
    u32 seedEv = seed * 7919u + 13u;
    std::vector< Ev > evs(actors.size());
    bool prevSkip = false;
    for (int t = 0; t < turns; t++) {
        const bool drawn = Drawn(static_cast< u32 >(t));
        bool skip = phase.Begin(drawn);
        if (gMut == 7) skip = drawn;  // (mutant: every drawn turn taken for a skip turn: exact by the flush, but it leaves poses out in a plain 60)
        if (skip) ++r.skipTurns;
        gTurnDrawn = drawn;
        if (drawn) {
            // the flush (port_anim_flush_stale with OdFlushEntry)
            for (Actor* a : actors) {
                if (a->made) {
                    a->made = false;
                    if (!a->hidden && gMut != 14) {
                        MaterialsForPicture(*a);  // (made for a reader; its materials as the skip turn makes them, then its view pass)
                    }
                    a->remade = false;
                    continue;
                }
                if (!a->pending) continue;
                a->pending = false;
                if (a->hidden) {
                    ++r.notDrawn;
                    continue;
                }
                if (gMut == 5) continue;
                if (gMut == 10 && a->noCalc) continue;
                if (XHash(*a) == a->xhash) {
                    if (gMut != 9) a->pl[1].l88 = a->staleLatch;
                    if (a->core[1]->mPortOwed != 0) a->core[1]->mPortOwed = 2;
                    CalcAnm(*a, 1);  // (joints only: the skip turn's base stands)
                    ++r.owedFlush;
                } else {
                    // (an update ran since the skip turn and the next calc-anim did not: the joints and the visibility from the record and the
                    // materials as the skip turn makes them (exact), else from the state now with the skip turn's base: drift, announced)
                    RemakeCtx ctx{a};
                    if (BHash(*a) == a->bhash && a->core[1]->portRemake(&RemakeCalc, &ctx, a->d->tree().getRootNode())) {
                        BvaFromRecord(*a);
                        MaterialsForPicture(*a);
                        ++r.owedFlush;
                        ++r.flushRecord;
                    } else {
                        CalcAnm(*a, 1);
                        ++r.drift;
                        a->tainted = true;
                    }
                }
            }
            for (Actor* a : actors) {  // (port_dl_flush_stale: the listed materials from the frames now, both worlds)
                for (int w = 0; w < 2; w++) {
                    if (a->dlStale[w]) {
                        a->matOut[w] = MatOf(a->mat[w]);
                        a->dlStale[w] = false;
                    }
                }
            }
            ++r.pictures;
            for (Actor* a : actors) {
                if (!a->hidden && !(SameMatrices(*a) && SamePicture(*a))) Bad(*a, r.picBad, r);
            }
        }
        // events (the same for both worlds)
        for (size_t i = 0; i < actors.size(); i++) {
            Actor& a = *actors[i];
            gSeed = seedEv;
            Ev& e = evs[i];
            memset(&e, 0, sizeof(e));
            const u32 chPct = a.cls == kNpc ? 60 : a.cls == kEnemy ? 30 : a.cls == kParts ? 10 : 0;  // (per mille)
            e.change = Rand() % 1000 < chPct;
            e.msg = (a.cls == kEnemy || a.cls == kNpc) && Rand() % 1000 < (late ? 120u : 15u);
            e.bck = Rand() % a.d->bcks.size();
            e.b1 = Rand() % a.d->bcks.size();
            e.mbck = Rand() % a.d->bcks.size();
            e.mb1 = Rand() % a.d->bcks.size();
            e.interp = Chance(30) ? 0 : static_cast< s32 >(1 + Rand() % 12);
            e.minterp = Chance(30) ? 0 : static_cast< s32 >(1 + Rand() % 12);
            e.rate = Chance(10) ? RandF(0.3f, 2.0f) : a.pl[0].rate;
            e.dx = Chance(60) ? RandF(-3, 3) : 0.0f;
            e.dyaw = Chance(30) ? RandF(-0.05f, 0.05f) : 0.0f;
            const u32 rd = Rand() % 1000;
            const u32 readPm = (skip || prevSkip) ? (late ? 120u : 60u) : 4u;  // (first readers, mostly in a window: the paths are what is tested)
            if (rd < readPm) {
                const u32 ph = Rand() % 3;
                e.readA = ph == 0;
                e.readB = ph == 1;
                e.readC = ph == 2;
            }
            if (late && (e.readB || e.readC)) e.msg = true;  // (a sent change, then the first read: LATE)
            if (!late) {
                // (classes / drift: a first read never follows an animation change in its window: the record's catch-up is exact)
                if (e.msg) e.readB = e.readC = false;
                if (e.change) e.readC = false;
            }
            if (a.marked && !a.jointSensor && Chance(2)) a.marked = false;  // (a fresh model: an actor made in play, its joints never handed out)
            e.hide = a.cls == kItem && !a.hidden && Chance(1);
            e.show = a.hidden && Chance(8);
            e.offCalc = drift && (a.cls == kDeco || a.cls == kItem) && !a.noCalc && !a.hidden && Rand() % 1000 < 25;
            e.onCalc = drift && a.noCalc && !a.hidden && Chance(10);
            seedEv = gSeed;
        }
        // movement, in list order
        for (size_t i = 0; i < actors.size(); i++) {
            Actor& a = *actors[i];
            const Ev& e = evs[i];
            if (e.msg) {
                for (int w = 0; w < 2; w++) Change(a, w, e.mbck, e.mb1, e.minterp);  // (sent by an actor that moved before: before its own update)
            }
            if (e.readB) FirstRead(a, r);
            {
                for (int w = 0; w < 2; w++) {
                    Update(a, w);
                    a.pl[w].rate = e.rate;
                    if (e.change) Change(a, w, e.bck, e.b1, e.interp);
                    a.lg[w].x += e.dx;
                    a.lg[w].yaw += e.dyaw;
                    if (a.baseReader) a.lg[w].acc += a.base[w][0][3] * 1e-3f + a.base[w][2][2];  // (a sensor / effect following the base matrix)
                    if (a.jointSensor) a.lg[w].acc += a.buf[w]->mpAnmMtx[0][1][3] * 1e-3f;      // (a sensor following joint 0, handed out at init)
                }
                if (!SameLogic(a)) Bad(a, r.logicBad, r);
            }
            if (e.readC) FirstRead(a, r);
            if (e.hide) a.hidden = a.noCalc = true;   // (hideModel: no calc-anim, not drawn)
            if (e.show) a.hidden = a.noCalc = false;
            if (e.offCalc) a.noCalc = true;           // (MR::offCalcAnim while shown)
            if (e.onCalc) a.noCalc = false;
        }
        // calc-anim, in list order (the hosts before their parts)
        for (Actor* ap : actors) {
            Actor& a = *ap;
            if (a.noCalc) continue;  // (LiveActor::calcAnim returns at once, both worlds)
            VerifyCalc(a, skip, r);  // (REF: the made computation, with the =2 verify's state check)
            if (a.pending || a.made) {
                // (the next calc-anim makes the owed pose, whatever the every-n-th rules say)
                if (a.pending) ++r.owedNext;
                a.pending = a.made = a.remade = false;
                FullCalc(a, 1);
            } else {
                od::Facts f;
                memset(&f, 0, sizeof(f));
                f.skipTurn = skip;
                f.gate = f.list = f.blendFixOn = f.posedOnce = true;
                f.jointsRead = a.marked && gMut != 2;
                f.callback = a.callback && gMut != 11;
                f.sharedCore = a.shared;
                f.readsLast = a.core[1]->portReadsLast() || a.core[1]->mTrackCount > XanimeCore::kPortOwedTracks;
                const int why = od::Refuse(f);
                if (skip && why == od::kRefRead) ++r.refusedRead;
                if (skip && why == od::kRefCallback) ++r.refusedCallback;
                if (skip && why == od::kRefShared) ++r.refusedShared;
                if (why < 0) {
                    if (gMut != 1) CalcBase(a, 1);  // (the base matrix is made as always)
                    a.staleLatch = a.pl[1].l88;
                    if (gPortBlendFix) a.core[1]->portLeaveOut(Ratio(a, 1));
                    a.pl[1].l88 = false;
                    a.pending = true;
                    a.matRec = a.mat[1];  // (OdRecordPlayers)
                    a.xhash = XHash(a);
                    a.bhash = BHash(a);
                    ++r.leftOut[a.cls];
                    continue;
                }
                FullCalc(a, 1);
            }
            if (!(SameMatrices(a) && SameCore(a))) {
                Bad(a, r.madeBad, r);
            } else if (SameLogic(a)) {
                a.tainted = false;  // (the worlds met again)
            }
        }
        for (size_t i = 0; i < actors.size(); i++) {
            if (evs[i].readA) FirstRead(*actors[i], r);  // (a reader in a later list of the same turn)
        }
        prevSkip = skip;
    }
    r.blendCatch = gPortBlendCensus.atChange + gPortBlendCensus.atRead + gPortBlendCensus.chained + gPortBlendCensus.eager + gPortBlendCensus.atCopy;
    return r;
}

int RuleTable() {
    int bad = 0;
    od::Facts f;
    memset(&f, 0, sizeof(f));
    f.skipTurn = f.gate = f.list = f.blendFixOn = f.posedOnce = true;
    if (od::Refuse(f) != -1) ++bad;
    struct {
        bool od::Facts::*m;
        int want;
        bool on;
    } k[] = {{&od::Facts::skipTurn, od::kRefTurn, false},   {&od::Facts::gate, od::kRefTurn, false},         {&od::Facts::list, od::kRefList, false},
             {&od::Facts::blendFixOn, od::kRefBlendFix, false}, {&od::Facts::posedOnce, od::kRefOther, false}, {&od::Facts::otherRule, od::kRefOther, true},
             {&od::Facts::collision, od::kRefPicture, true}, {&od::Facts::camera, od::kRefPicture, true},     {&od::Facts::hrwTag, od::kRefPicture, true},
             {&od::Facts::jointsRead, od::kRefRead, true},   {&od::Facts::callback, od::kRefCallback, true}, {&od::Facts::sharedCore, od::kRefShared, true},
             {&od::Facts::readsLast, od::kRefBlend, true}};
    for (auto& e : k) {
        od::Facts g = f;
        g.*(e.m) = e.on;
        if (od::Refuse(g) != e.want) {
            printf("rule: %s refused as %d\n", od::RefusalName(e.want), od::Refuse(g));
            ++bad;
        }
    }
    // the census's order: the first reason that holds
    od::Facts g = f;
    g.jointsRead = g.callback = g.readsLast = true;
    if (od::Refuse(g) != od::kRefRead) ++bad;
    g.skipTurn = false;
    if (od::Refuse(g) != od::kRefTurn) ++bad;
    const int lists[] = {0, 1, 2, 3, 7, 9, 0x0A, 0x0C, 0x0D, 0x0E, 0x13, 0x14, -1};
    for (int l : lists) {
        if (od::KindOfList(l) != -1) ++bad;
    }
    if (od::KindOfList(4) != od::kEnv || od::KindOfList(5) != od::kMapObj || od::KindOfList(6) != od::kNpc || od::KindOfList(8) != od::kEnemy ||
        od::KindOfList(0x0B) != od::kDeco || od::KindOfList(0x10) != od::kItem)
        ++bad;
    if (od::ParseMode("1") != 1 || od::ParseMode("2") != 2 || od::ParseMode("0") != 0 || od::ParseMode("12") != 0 || od::ParseMode(nullptr) != 0) ++bad;
    od::Census c;
    c.Clear();
    c.skipTurns = 100;
    c.leftOut[od::kNpc] = 2000;  // 20 a skip turn
    c.poseN = 10;
    c.poseNs = 10 * 20000;  // 20 us a pose
    c.posedRead = 100;      // 1 a skip turn
    c.ruleCalls = 30000;
    c.ruleN = 10;
    c.ruleNs = 10 * 500;  // 0.5 us
    const double want = (2000 * 20000.0 - 100 * 20000.0 - 30000 * 500.0) / 100 / 1e6;  // 0.23 ms
    if (fabs(c.SavedMsPerPair() - want) > 1e-9) ++bad;
    printf("rule table: %s\n", bad ? "FAIL" : "ok");
    return bad;
}

}  // namespace

int main(int argc, char** argv) {
    const int turns = argc > 1 ? atoi(argv[1]) : 1500;
    const u32 seed = argc > 2 ? static_cast< u32 >(atoi(argv[2])) : 4242u;
    gScen = argc > 3 ? argv[3] : "classes";
    gMut = argc > 4 ? atoi(argv[4]) : 0;
    int fail = RuleTable();
    const char* rhythms[] = {"pair", "broken", "catchup", "plain", "random"};
    const bool classes = !strcmp(gScen, "classes"), late = !strcmp(gScen, "late");
    for (int lazy = 1; lazy <= 2; ++lazy)
    for (const char* rh : rhythms) {
        gRhythm = rh;
        gLazy = lazy;
        const Result r = Run(turns, seed);
        u32 left = 0;
        for (int k = 0; k < kClsN; k++) left += r.leftOut[k];
        bool ok = r.unannounced == 0 && r.vFalse == 0;  // (emufix31: no false "the calc-anim changed the logic state")
        if (classes || !strcmp(gScen, "drift")) {
            // (drift: a pose owed while the actor's calc-anim is switched off after its update is made by the flush from the record: exact too)
            ok = ok && r.picBad == 0 && r.logicBad == 0 && r.readerBad == 0 && r.madeBad == 0 && r.late == 0 && r.drift == 0;
        }
        if (late) {
            ok = ok && r.picBad == 0 && r.logicBad == 0;  // (a LATE read sees an older pose; the picture and the logic are untouched)
        }
        if (!strcmp(rh, "plain") && left != 0) ok = false;  // (no skip turn in a plain 60)
        if (!strcmp(rh, "pair")) {
            for (int k = 0; k < kClsN; k++) {
                if (r.leftOut[k] == 0) ok = false;  // (every class is taken somewhere)
            }
            if (r.refusedRead == 0 || r.refusedCallback == 0 || r.refusedShared == 0) ok = false;
            if (classes && (r.owedRead == 0 || r.remade == 0 || r.blendCatch == 0)) ok = false;  // (the catch-ups are exercised)
            if (late && r.late == 0) ok = false;
            if (r.vChecked == 0 || r.vControl == 0) ok = false;  // (the =2 verify's state check runs, and its negative control is seen)
            if (r.vBase == 0 || r.vBase > r.vChecked) ok = false;  // (fable31fix: the base-written state is reported apart: the attachments' bases write their position)
            if (!strcmp(gScen, "drift") && r.flushRecord == 0) ok = false;  // (the flush's record path is exercised)
        }
        printf("%-8s %-8s lazy %d skip turns %4u pictures %4u | left out npc %5u enemy %5u item %5u deco %5u parts %5u | refused read %u callback %u shared %u | "
               "owed made: next %5u flush %4u (record %3u) read %3u (record %3u) not drawn %3u drift %3u | first reads %3u LATE %3u | blendfix catch-ups %4u | "
               "diffs: picture %u logic %u reader %u computation %u, unannounced %u | =2 state check: %u checked, false %u (parts %u), control seen %u, base-written %u%s\n",
               gScen, rh, lazy, r.skipTurns, r.pictures, r.leftOut[0], r.leftOut[1], r.leftOut[2], r.leftOut[3], r.leftOut[4], r.refusedRead, r.refusedCallback,
               r.refusedShared, r.owedNext, r.owedFlush, r.flushRecord, r.owedRead, r.remade, r.notDrawn, r.drift, r.reads, r.late, r.blendCatch, r.picBad, r.logicBad,
               r.readerBad, r.madeBad, r.unannounced, r.vChecked, r.vFalse, r.vParts, r.vControl, r.vBase, ok ? "" : "  <-- FAIL");
        if (!ok) ++fail;
    }
    printf("ondraw %s mutant %d: %s\n", gScen, gMut, fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
