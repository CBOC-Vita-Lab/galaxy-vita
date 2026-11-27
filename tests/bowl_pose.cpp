// GXV_BOWL_POSE host proof (bowl31; tools/test_bowl_pose.sh): the fish pose rule against the game's own XanimeCore / J3DJoint / J3DJointTree /
// J3DAnimation / J3DMtxBuffer code, byte for byte, over the turn and picture rhythm of GameSystem::frameLoop.
//
// Two worlds of the same actors run the same logic from one event stream: the REFERENCE poses every turn (the game as it is); the CUT applies
// port/bowl_pose.h's rule as LiveActor.cpp does (PortBowlPose, port_anim_flush_stale). The models are tests/par_anim_walk.cpp's (random joint trees,
// BCKs, the three core types, fixT, envelopes; linked whole with the game units); this file adds the XanimePlayer side the rule touches: the BCK
// frame, its latch (_84 / _88: updateAfterMovement advances the frame only when the last calcAnm released the latch; calcAnm poses _84 while it is
// set), the animation change (XanimeCore::doFreeze, the new track, the interpolation ratio of updateInterpoleRatio), _20 (XanimePlayer::_0C), a logic
// reader of the joints (a sensor following a joint: it reads them in the next turn's movement).
//
// Every turn:   drawn turn: the flush (cut world: owed poses, latch restored as LiveActor.cpp does), then the PICTURE: both worlds' joint and envelope
//               matrices and scale flags must be equal byte for byte; movement (the same events in both worlds; the reader reads); calc-anim.
//               After every movement both worlds' logic state (frame, _84, _88, interpolation) must be equal: the clock is untouched.
// Rhythms: steady pair (Quality's even 30 / Performance's pair), pair with broken pairs (two drawn turns), runs of undrawn turns (catch-up), plain 60
// (every turn drawn: no skip turn, nothing may be left out), random.
// Animation scenarios: "fish" (one animation from the start, only the rate changes: what Fish does), "change-on-drawn" (changes in drawn turns'
// movement only: the freeze is pending at the skip turn's calc and the rule refuses), "change-any" (penguin-like: a change can come in the turn
// after a skip turn; the cut is then NOT exact, and the test requires that every such picture difference was announced by the LATE detector, which
// is why the penguins are census-only), "smooth" (_20 != 1: refused), "reader" (a joint reader marks its model: refused).
// Mutants (each must fail): skip on the undrawn turn; no flush for a broken pair; the latch not released; the latch not restored by the flush;
// the smoothing guard off; the reader mark ignored.
#define main par_anim_walk_main
#include "par_anim_walk.cpp"
#undef main
#include "port/bowl_pose.h"
#include "port/pose_phase.h"
#include <JSystem/J3DGraphBase/J3DVertex.hpp>

// (J3DJointTree's constructor names these; J3DVertex.cpp is not linked here: the ThreadSanitizer link of par_anim_walk tolerates the unresolved call, a plain one does not)
J3DDrawMtxData::J3DDrawMtxData() {
}
J3DDrawMtxData::~J3DDrawMtxData() {
}

namespace {
namespace bp = ::port::bowlpose;

int gMut = 0;  // 1 skip on undrawn, 2 no flush, 3 latch not released, 4 latch not restored by the flush, 5 smoothing guard off, 6 reader mark ignored

struct Player {   // the XanimePlayer fields the rule and the clock touch (one per world)
    f32 frame, rate, f84, i08;
    bool l88;
    s32 i14;      // interpolation frames left (_20->_14)
    f32 i0C;      // _0C: core._20
    u32 bck;      // the current animation's BCK (track 0)
};
struct Actor {
    Data* d;
    J3DModel* model;
    u8 type4, mode6, tracks;
    u32 bck1;     // track 1's BCK (two-track actors: XanimeCore::calcBlend; one track: calcSingle)
    Vec scale;
    Mtx base;
    bool reader;  // a logic reader of joint 0 (a sensor following a joint): the model is marked read
    J3DMtxBuffer* buf[2];
    XanimeCore* core[2];
    Player pl[2];
    // cut world (index 1) window state, as LiveActor's mBowlPose / mStaleAnim / mStaleXLatch
    bool pending, listed, staleLatch;
    bool tainted;  // a LATE was announced and the two worlds' cores have not met again since
    Mtx readSeen[2];
};

const char* gRhythm = "pair";
bool gChangeOnDrawnOnly = false, gChangeAny = false, gSmooth = false, gReaders = false, gFishOnly = true;

void CalcAnm(Actor& a, int w) {
    Player& p = a.pl[w];
    XanimeCore* c = a.core[w];
    const f32 cur = p.l88 ? p.f84 : p.frame;
    const J3DAnmTransformKey& anm = a.d->bcks[p.bck]->anm;
    c->mFrameRatio = cur / static_cast< f32 >(anm.mFrameMax > 0 ? anm.mFrameMax : 1);
    for (u32 t = 0; t < c->mTrackCount; ++t) {
        c->mTrackList[t]._0 = &a.d->bcks[t == 0 ? p.bck : a.bck1]->anm;
        c->mTrackList[t]._C = 0;
        c->mTrackList[t].mWeight = t == 0 ? 0.6f : 0.4f;
    }
    c->updateFrame();  // (the game's own: the frames onto the shared BCKs, the freeze flag -> _29)
    a.d->tree().getRootNode()->setMtxCalc(c);
    j3dSys.setModel(a.model);
    a.d->tree().calc(a.buf[w], a.scale, a.base);
    if (a.d->tree().getWEvlpMtxNum() != 0) {
        a.buf[w]->calcWeightEnvelopeMtx();
    }
    a.d->tree().getRootNode()->setMtxCalc(nullptr);
    p.l88 = false;
}

void Movement(Actor& a, int w, bool change, u32 newBck, s32 interp, f32 newRate, bool baseMove, const Mtx newBase) {
    Player& p = a.pl[w];
    XanimeCore* c = a.core[w];
    if (a.reader) {
        memcpy(a.readSeen[w], a.buf[w]->mpAnmMtx[0], sizeof(Mtx));  // (the sensor's update: the joint as the last calc-anim left it)
    }
    if (baseMove && w == 0) {
        memcpy(a.base, newBase, sizeof(Mtx));  // (one base for both worlds: the logic state; the cut never changes it)
    }
    p.rate = newRate;
    if (change) {
        c->doFreeze();  // (XanimePlayer::prepareAnimation / changeAnimationSimple)
        p.bck = newBck;
        p.frame = 0.0f;
        p.i14 = interp;
        p.i08 = interp == 0 ? 1.0f : 0.0f;
        p.i0C = gSmooth ? 0.35f : 1.0f;
    }
    // updateAfterMovement
    const f32 maxF = static_cast< f32 >(a.d->bcks[p.bck]->anm.mFrameMax);
    if (!p.l88) {
        p.f84 = p.frame;
        p.frame += p.rate;
        if (p.frame >= maxF) p.frame -= maxF;
        p.l88 = true;
    }
    if (p.i14 != 0) {
        p.i08 += (1.0f - p.i08) / static_cast< f32 >(p.i14);
        p.i14 -= 1;
    } else {
        p.i08 = 1.0f;
    }
    c->_1C = p.i08;
    c->_20 = p.i0C;
}

bool SamePicture(const Actor& a) {
    const u16 n = a.d->tree().getJointNum(), e = a.d->tree().getWEvlpMtxNum();
    if (memcmp(a.buf[0]->mpAnmMtx, a.buf[1]->mpAnmMtx, n * sizeof(Mtx)) != 0 || memcmp(a.buf[0]->mpScaleFlagArr, a.buf[1]->mpScaleFlagArr, n) != 0) {
        return false;
    }
    return e == 0 || (memcmp(a.buf[0]->mpWeightEvlpMtx, a.buf[1]->mpWeightEvlpMtx, e * sizeof(Mtx)) == 0 &&
                      memcmp(a.buf[0]->mpEvlpScaleFlagArr, a.buf[1]->mpEvlpScaleFlagArr, e) == 0);
}
bool SameLogic(const Actor& a) {
    const Player& x = a.pl[0];
    const Player& y = a.pl[1];
    return memcmp(&x, &y, sizeof(Player)) == 0 && a.core[0]->_1C == a.core[1]->_1C && a.core[0]->_20 == a.core[1]->_20;
}

bool Drawn(u32 t) {
    if (!strcmp(gRhythm, "pair")) return t % 2 == 0;
    if (!strcmp(gRhythm, "broken")) return t % 2 == 0 || Chance(12);
    if (!strcmp(gRhythm, "catchup")) return t % 2 == 0 && !Chance(15);
    if (!strcmp(gRhythm, "plain")) return true;
    return Chance(55);
}

struct Result {
    u32 pictures = 0, leftOut = 0, owedNext = 0, owedFlush = 0, picBad = 0, logicBad = 0, readerBad = 0, late = 0, badAfterNoLate = 0, skipTurns = 0;
};

Result Run(int turns, u32 seed) {
    gSeed = seed;
    std::vector< Data* > datas;
    for (int i = 0; i < 6; i++) datas.push_back(MakeData());
    std::vector< Actor* > actors;
    for (int i = 0; i < 24; i++) {
        Actor* a = new Actor;
        memset(a, 0, sizeof(*a));
        a->d = datas[Rand() % datas.size()];
        a->model = static_cast< J3DModel* >(calloc(1, sizeof(J3DModel)));
        a->model->mModelData = a->d->data;
        a->type4 = static_cast< u8 >(Rand() % 3);
        a->mode6 = Chance(20) ? 3 : 0;
        a->scale.x = a->scale.y = a->scale.z = Chance(70) ? 1.0f : RandF(0.6f, 1.6f);
        PSMTXIdentity(a->base);
        a->reader = gReaders && Chance(40);
        const u32 bck0 = Rand() % a->d->bcks.size();
        const u16 fixJoint = static_cast< u16 >(Rand() % a->d->tree().getJointNum());
        a->tracks = gSmooth || Chance(30) ? 2 : 1;  // (smooth: _20 != 1 is read by calcBlend only)
        a->bck1 = Rand() % a->d->bcks.size();
        for (int w = 0; w < 2; w++) {
            a->buf[w] = MakeBuffer(a->d);
            XanimeCore* c = new XanimeCore(a->tracks, a->d->tree().getJointNum(), a->type4);
            c->_6 = a->mode6;
            c->_C = fixJoint;
            for (u32 j = 0; j < c->mJointCount; j++) {
                c->mJointList[j]._0._0.set(1, 1, 1);
                c->mJointList[j]._0._C.set(0, 0, 0);
                c->mJointList[j]._0.mRotation = Quaternion{0, 0, 0, 1};
                c->mJointList[j]._28 = c->mJointList[j]._0;
                c->mJointList[j]._50.set(0, 0, 0);
            }
            a->core[w] = c;
            Player& p = a->pl[w];
            memset(&p, 0, sizeof(p));
            p.bck = bck0;
            p.rate = 1.0f;
            p.i08 = 1.0f;
            p.i0C = gSmooth ? 0.35f : 1.0f;
            c->_1C = 1.0f;
            c->_20 = p.i0C;
        }
        // world 1's buffers start as world 0's
        const u16 n = a->d->tree().getJointNum();
        memcpy(a->buf[1]->mpAnmMtx, a->buf[0]->mpAnmMtx, n * sizeof(Mtx));
        memcpy(a->buf[1]->mpScaleFlagArr, a->buf[0]->mpScaleFlagArr, n);
        a->core[0]->doFreeze();  // (Fish::init: startBck("Swim"): the first animation's freeze and computation, both worlds)
        a->core[1]->doFreeze();
        CalcAnm(*a, 0);
        CalcAnm(*a, 1);
        actors.push_back(a);
    }
    J3DSys::mParentS.x = J3DSys::mParentS.y = J3DSys::mParentS.z = 1.0f;
    port::posephase::Phase phase;
    Result r;
    u32 seedEv = seed * 7919u + 13u;
    for (int t = 0; t < turns; t++) {
        const bool drawn = Drawn(static_cast< u32 >(t));
        bool skip = phase.Begin(drawn);
        if (gMut == 1) skip = drawn;  // (mutant: every drawn turn is taken for a skip turn)
        if (skip) ++r.skipTurns;
        if (drawn) {
            // the flush (port_anim_flush_stale): an owed pose is made from the state the left-out calc-anim had (the latch restored)
            for (Actor* a : actors) {
                if (a->pending) {
                    a->pending = false;
                    a->listed = false;
                    if (gMut != 2) {
                        if (gMut != 4) a->pl[1].l88 = a->staleLatch;
                        CalcAnm(*a, 1);
                        ++r.owedFlush;
                    }
                }
            }
            ++r.pictures;
            for (Actor* a : actors) {
                if (!SamePicture(*a)) {
                    ++r.picBad;
                    if (!a->tainted) ++r.badAfterNoLate;  // (a difference no LATE announced)
                }
            }
        }
        // movement: the same events in both worlds
        for (Actor* a : actors) {
            gSeed = seedEv;
            const bool change = (gChangeAny && Chance(6)) || (gChangeOnDrawnOnly && drawn && Chance(10));
            const u32 newBck = Rand() % a->d->bcks.size();
            const s32 interp = Chance(30) ? 0 : static_cast< s32 >(1 + Rand() % 12);
            const f32 rate = Chance(25) ? RandF(0.2f, 2.5f) : a->pl[0].rate;
            const bool baseMove = Chance(70);
            Mtx nb;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 4; j++) nb[i][j] = RandF(-1.5f, 1.5f) * (j == 3 ? 400.0f : 1.0f);
            seedEv = gSeed;
            for (int w = 0; w < 2; w++) Movement(*a, w, change, newBck, interp, rate, baseMove, nb);
            if (a->reader && memcmp(a->readSeen[0], a->readSeen[1], sizeof(Mtx)) != 0) ++r.readerBad;
            if (!SameLogic(*a)) ++r.logicBad;
        }
        // calc-anim
        for (Actor* a : actors) {
            CalcAnm(*a, 0);
            if (a->pending) {  // (the next calc-anim makes the owed pose, whatever else holds)
                a->pending = false;
                a->listed = false;
                ++r.owedNext;
                if (a->core[1]->_28 != 0) {
                    ++r.late;  // (LiveActor.cpp's LATE detector: a freeze this turn's movement started reads the left-out pose)
                    a->tainted = true;
                }
                CalcAnm(*a, 1);
                continue;
            }
            bp::Facts f;
            memset(&f, 0, sizeof(f));
            f.cls = bp::kFish;
            f.skipTurn = skip;
            f.gate = true;
            f.posedOnce = true;
            f.jointsRead = a->reader && gMut != 6;
            const XanimeCore* c = a->core[1];
            f.blendReadsLast = (c->_20 != 1.0f && gMut != 5) || c->_28 != 0 || c->mTrackList[0]._0 == nullptr;
            if (bp::Refuse(f, true) < 0) {
                a->staleLatch = a->pl[1].l88;
                if (gMut != 3) a->pl[1].l88 = false;
                a->pending = true;
                a->listed = true;
                ++r.leftOut;
            } else {
                CalcAnm(*a, 1);
            }
            if (a->tainted && !a->pending) {
                bool same = true;
                for (u32 j = 0; j < a->core[0]->mJointCount && same; ++j) {
                    same = memcmp(&a->core[0]->mJointList[j], &a->core[1]->mJointList[j], sizeof(XjointInfo)) == 0;
                }
                a->tainted = !(same && SamePicture(*a));
            }
        }
    }
    return r;
}

int RuleTable() {
    // port/bowl_pose.h: each fact refuses with its own reason, in the census's order; the census-only class refuses mode 1 only.
    int bad = 0;
    bp::Facts f;
    memset(&f, 0, sizeof(f));
    f.cls = bp::kFish;
    f.skipTurn = f.gate = f.posedOnce = true;
    if (bp::Refuse(f, true) != -1) ++bad;
    struct {
        bool bp::Facts::*m;
        int want;
        bool on;
    } k[] = {{&bp::Facts::skipTurn, bp::kRefTurn, false}, {&bp::Facts::gate, bp::kRefTurn, false},     {&bp::Facts::posedOnce, bp::kRefOther, false},
             {&bp::Facts::otherRule, bp::kRefOther, true}, {&bp::Facts::collision, bp::kRefPicture, true}, {&bp::Facts::camera, bp::kRefPicture, true},
             {&bp::Facts::hrwTag, bp::kRefPicture, true},  {&bp::Facts::jointsRead, bp::kRefRead, true},  {&bp::Facts::callback, bp::kRefCallback, true},
             {&bp::Facts::parts, bp::kRefParts, true},     {&bp::Facts::blendReadsLast, bp::kRefBlend, true}};
    for (auto& e : k) {
        bp::Facts g = f;
        g.*(e.m) = e.on;
        if (bp::Refuse(g, true) != e.want || bp::Refuse(g, false) != e.want) {
            printf("rule: %s refused as %d, want %d\n", bp::RefusalName(e.want), bp::Refuse(g, true), e.want);
            ++bad;
        }
    }
    bp::Facts p = f;
    p.cls = bp::kPenguin;
    if (bp::Refuse(p, true) != bp::kRefClass || bp::Refuse(p, false) != -1) ++bad;
    if (bp::ParseMode("1") != 1 || bp::ParseMode("2") != 2 || bp::ParseMode("0") != 0 || bp::ParseMode("12") != 0 || bp::ParseMode(nullptr) != 0) ++bad;
    printf("rule table: %s\n", bad ? "FAIL" : "ok");
    return bad;
}

}  // namespace

int main(int argc, char** argv) {
    const int turns = argc > 1 ? atoi(argv[1]) : 2000;
    const u32 seed = argc > 2 ? static_cast< u32 >(atoi(argv[2])) : 4242u;
    const char* scenario = argc > 3 ? argv[3] : "fish";
    gMut = argc > 4 ? atoi(argv[4]) : 0;
    gChangeAny = !strcmp(scenario, "change-any");
    gChangeOnDrawnOnly = !strcmp(scenario, "change-on-drawn");
    gSmooth = !strcmp(scenario, "smooth");
    gReaders = !strcmp(scenario, "reader");
    int fail = RuleTable();
    const char* rhythms[] = {"pair", "broken", "catchup", "plain", "random"};
    for (const char* rh : rhythms) {
        gRhythm = rh;
        const Result r = Run(turns, seed);
        // (change-any: the cut is NOT exact for a class that changes animation in the turn after a skip turn; every difference must have been announced by the
        // LATE detector, and in a pair there must be some: the reason the penguins are census-only)
        const bool exactWanted = !gChangeAny;
        bool ok = r.logicBad == 0 && r.readerBad == 0 && (exactWanted ? r.picBad == 0 : r.badAfterNoLate == 0);
        if (gChangeAny && !strcmp(rh, "pair") && (r.picBad == 0 || r.late == 0)) ok = false;
        if (!strcmp(rh, "plain") && r.leftOut != 0) ok = false;  // (no skip turn in a plain 60)
        printf("%-16s %-8s skip turns %5u pictures %5u left out %6u (owed: next turn %6u, flush %5u) | picture diffs %u, logic diffs %u, reader diffs %u, LATE %u%s\n",
               scenario, rh, r.skipTurns, r.pictures, r.leftOut, r.owedNext, r.owedFlush, r.picBad, r.logicBad, r.readerBad, r.late, ok ? "" : "  <-- FAIL");
        if (!ok) ++fail;
    }
    printf("bowl_pose %s mutant %d: %s\n", scenario, gMut, fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
