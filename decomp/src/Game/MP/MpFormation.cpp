// Galaxy 3.0 multiplayer: puppet formation, game side (owner: formation). See MpFormation.hpp and MpFormationLogic.hpp.
//
// Once a logic turn (MpPuppets::update, after the puppets' control() set their true pose this turn and before the name tags):
// the local player is the anchor, the camera and his gravity give the frame, the scene state and the demo / bind tests say whether
// it is a cutscene, the first kSpawnTurns playable turns are the spawn window, the star-get demo (Play Together) is the team photo.
// Each remote puppet's drawn pose = its true pose + the logic's offset (ground: the game's own ground check along the gravity at
// the place; the model tilts to the gravity there and, in the photo, turns to the camera). The drawn pose is what the model, its
// shadow, its name tag and speech balloon use; sensors, bumps and the bubble keep the true one.
//
// Debug switches (desktop environment / the Vita's env.txt; not settings): SMG_MP_FORMATION=0 turns it off (before / after
// pictures); SMG_MP_FORMATION_PROBE=1 logs, once per scene, whether a REAL start offset per slot would have ground (the evidence
// behind using a visual spread).
#include "Game/MP/MpFormation.hpp"
#include "Game/MP/MpFlow.hpp"
#include "Game/MP/MpFormationLogic.hpp"
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/MP/MpRemotePlayer.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/GravityUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include <cstdlib>

using MpFormation::V3;

namespace {
    MpFormation::Formation sF;
    bool sLive;
    int sActionTurns;            // playable turns since the scene's init (the spawn window)
    uint32_t sTurn;
    int sEnv = -1;               // SMG_MP_FORMATION: 0 off, else on (read once)
    int sProbeEnv = -1;          // SMG_MP_FORMATION_PROBE
    bool sProbeDone;
    uint8_t sLogged[MpFormation::Formation::SLOTS];   // event lines this scene per slot (capped)
    uint8_t sPick[MpFormation::Formation::SLOTS];     // last logged ground pick
    uint32_t sOnCount, sAppearCount;

    const int cLogCap = 40;

    V3 toV(const TVec3f& v) { return MpFormation::v3(v.x, v.y, v.z); }
    TVec3f toT(const V3& v) { return TVec3f(v.x, v.y, v.z); }

    bool enabled() {
        if (sEnv < 0) {
            const char* e = getenv("SMG_MP_FORMATION");
            sEnv = (e != nullptr && e[0] == '0') ? 0 : 1;
            if (sEnv == 0) mp_log("formation off (SMG_MP_FORMATION=0)");
        }
        return sEnv != 0;
    }

    // unit gravity at a place (false: none, e.g. between planets)
    bool gravityAt(const LiveActor* host, const V3& pos, V3* out) {
        TVec3f g(0.0f, 0.0f, 0.0f);
        if (!MR::calcGravityVectorOrZero(host, toT(pos), &g, nullptr, 0)) return false;
        V3 v = toV(g);
        if (!MpFormation::finite3(v) || !MpFormation::normalize(&v)) return false;
        *out = v;
        return true;
    }

    // The ground under `spot` along the gravity there (fallback: the anchor's): the correction that puts a foot on it. False: no
    // floor within reach, a death area, or a floor too far up / down to be this one.
    bool groundAt(const LiveActor* host, const V3& spot, const V3& fallbackUp, V3* corr, V3* hitOut = nullptr, bool deathTest = true) {
        using namespace MpFormation;
        V3 g;
        if (!gravityAt(host, spot, &g)) g = mul(fallbackUp, -1.0f);
        TVec3f hit;
        if (!MR::getFirstPolyOnLineToMap(&hit, nullptr, toT(sub(spot, mul(g, 150.0f))), toT(mul(g, 450.0f)))) return false;
        const V3 h = toV(hit);
        if (deathTest && MR::isInDeath(toT(sub(h, mul(g, 30.0f))))) return false;
        const V3 c = sub(h, spot);
        if (!finite3(c) || len(c) > kMaxGroundStep) return false;
        *corr = c;
        if (hitOut != nullptr) *hitOut = h;
        return true;
    }

    const char* modeName(uint8_t m) { return m == MpFormation::MODE_ARC ? "photo" : m == MpFormation::MODE_SPACE ? "space" : m == MpFormation::MODE_GROUP ? "group" : "none"; }

    // ---- evidence for the "real start offset" question (SMG_MP_FORMATION_PROBE=1): would moving each slot's player 160 units to
    // the right / left / back / front of the start point (the camera's frame) put him on the same floor?
    const char* probeOne(const LiveActor* host, const V3& anchor, const V3& up, const V3& anchorGround, const V3& dir) {
        using namespace MpFormation;
        const V3 spot = add(anchor, mul(dir, kSpacing));
        const V3 eye0 = add(anchor, mul(up, 80.0f));
        if (MR::isExistMapCollision(toT(eye0), toT(sub(add(spot, mul(up, 80.0f)), eye0)))) return "wall";
        V3 g;
        if (!gravityAt(host, spot, &g)) return "nogravity";
        if (dot(mul(g, -1.0f), up) < 0.94f) return "curve";   // > 20 degrees: a small planet's side
        TVec3f nrm, hit;
        if (!MR::getFirstPolyNormalOnLineToMap(&nrm, toT(sub(spot, mul(g, 150.0f))), toT(mul(g, 450.0f)), &hit, nullptr)) return "void";
        const V3 h = toV(hit);
        if (MR::isInDeath(toT(sub(h, mul(g, 30.0f))))) return "death";
        if (std::fabs(dot(sub(h, anchorGround), up)) > 60.0f) return "step";
        if (dot(toV(nrm), mul(g, -1.0f)) < 0.7f) return "slope";
        return "ok";
    }

    void probe(const LiveActor* host, const MpFormation::TurnIn& t) {
        using namespace MpFormation;
        if (sProbeEnv < 0) {
            const char* e = getenv("SMG_MP_FORMATION_PROBE");
            sProbeEnv = (e != nullptr && e[0] == '1') ? 1 : 0;
        }
        if (sProbeEnv == 0 || sProbeDone || sActionTurns < 30) return;   // (half a second in: he has landed)
        sProbeDone = true;
        V3 corr, ground;
        const V3 up = t.cam.up;
        const bool anchorOk = groundAt(host, t.anchor, up, &corr, &ground);
        const char* r[4] = {"-", "-", "-", "-"};
        if (anchorOk) {
            const V3 dirs[4] = {t.cam.right, mul(t.cam.right, -1.0f), t.cam.back, mul(t.cam.back, -1.0f)};
            for (int i = 0; i < 4; i++) r[i] = probeOne(host, t.anchor, up, ground, dirs[i]);
        }
        const char* stage = MR::getCurrentStageName();
        mp_log("formation probe stage=%s scen=%d ground=%d right=%s left=%s back=%s front=%s", stage != nullptr ? stage : "?",
               (int)MR::getCurrentScenarioNo(), anchorOk ? 1 : 0, r[0], r[1], r[2], r[3]);
    }
}  // namespace

namespace MpFormation {
    void initScene() {
        sF.reset();
        sLive = true;
        sActionTurns = 0;
        sProbeDone = false;
        sOnCount = sAppearCount = 0;
        for (int i = 0; i < Formation::SLOTS; i++) {
            sLogged[i] = 0;
            sPick[i] = 0;
        }
    }

    unsigned placedMask() {   // MP3 lod2
        unsigned m = 0;
        if (!sLive) return 0;
        for (int s = 0; s < Formation::SLOTS && s < 32; s++) {
            if (sF.s[s].mode != MODE_NONE) m |= 1u << s;
        }
        return m;
    }

    unsigned localSpaceMask() {   // wave8: suppress invisible true-position sensor collision while formation visually spreads puppets
        unsigned m = 0;
        if (!sLive) return 0;
        for (int s = 0; s < Formation::SLOTS && s < 32; s++) {
            if (sF.s[s].mode == MODE_SPACE || sF.s[s].mode == MODE_ARC) m |= 1u << s;
        }
        return m;
    }

    void endScene() {
        if (sLive && (sOnCount != 0 || sAppearCount != 0)) {
            mp_log("formation end on=%u appear=%u relatch=%u", (unsigned)sOnCount, (unsigned)sAppearCount, (unsigned)sF.relatches);
        }
        sLive = false;
        sF.reset();
    }

    void update() {
        sTurn++;
        if (!sLive || !enabled()) return;
        MarioActor* a = MpPuppetsInt::localActor();
        const int st = MpGameInternal::sceneState();
        if (st == MP_SCENE_ACTION && sActionTurns < 1000000) sActionTurns++;

        TurnIn t;
        t.active = a != nullptr && mp_active();
        t.cutscene = t.spawn = t.star = false;
        t.anchor = v3(0.0f, 0.0f, 0.0f);
        bool camOk = false;
        if (t.active) {
            t.anchor = toV(a->mPosition);
            camOk = finite3(t.anchor) && makeFrame(toV(a->getGravityVec()), toV(MR::getCamXdir()), toV(MR::getCamZdir()),
                                                   toV(MR::getCamYdir()), &t.cam);
            const bool starGet = st == MP_SCENE_STAR_GET || st == MP_SCENE_GRAND_STAR_GET;
            t.cutscene = starGet || st == MP_SCENE_OPENING || st == MP_SCENE_STARTER || MR::isDemoActive() || MR::isPlayerInBind();
            t.spawn = sActionTurns > 0 && sActionTurns <= kSpawnTurns;
            t.star = starGet && mp_mode() == MP_MODE_TOGETHER;
        }
        sF.begin(t, camOk);

        const int me = mp_local_slot();
        unsigned present = 0;
        for (int s = 0; s < Formation::SLOTS && s < MP_MAX_PLAYERS; s++) {
            if (mp_slot_present(s)) present |= 1u << s;
        }
        const int n = remoteCount(me, present);
        sF.nRemote = n;
        const TVec3f camPos = MR::getCamPos();
        SlotIn ins[Formation::SLOTS];
        bool valid[Formation::SLOTS];
        for (int s = 0; s < Formation::SLOTS; s++) {
            valid[s] = false;
            ins[s].eligible = false;
            ins[s].pos = v3(0.0f, 0.0f, 0.0f);
            ins[s].onGround = ins[s].inDemo = false;
        }
        // pass 1: each puppet against the local player; pass 2: the puppets that overlap each other away from him (groups)
        for (int s = 0; s < Formation::SLOTS && s < MP_MAX_PLAYERS; s++) {
            MpPuppetBase* p = MpPuppetsInt::puppet(s);
            if (s == me || p == nullptr || p->isSelf()) {
                sF.s[s].reset();
                continue;
            }
            const u16 fl = p->poseFlags();
            const int rank = rankOf(s, me, present);
            SlotIn& in = ins[s];
            in.pos = toV(p->mPosition);
            in.onGround = (fl & MP_POSE_ON_GROUND) != 0;
            in.inDemo = (fl & (MP_POSE_IN_DEMO | MP_POSE_IN_BIND)) != 0;
            in.eligible = rank >= 0 && !MR::isDead(p) && p->isDrawable() && (fl & (MP_POSE_BUBBLE | MP_POSE_DEAD)) == 0;
            valid[s] = true;
            sF.decide(s, t, in);
        }
        sF.decideGroups(t, ins);
        for (int s = 0; s < Formation::SLOTS && s < MP_MAX_PLAYERS; s++) {
            if (!valid[s]) continue;
            MpPuppetBase* p = MpPuppetsInt::puppet(s);
            const int rank = rankOf(s, me, present);
            const SlotIn& in = ins[s];
            Slot& q = sF.s[s];
            const uint8_t mode = q.mode;
            if (q.event != EV_NONE && sLogged[s] < cLogCap) {
                sLogged[s]++;
                if (q.event == EV_ON) {
                    sOnCount++;
                    mp_log("formation on slot=%d mode=%s why=%s dist=%d rank=%d side=%d", s, modeName(mode), whyName(q.why),
                           mode == MODE_GROUP ? (int)MpFormation::len(MpFormation::sub(in.pos, q.gpos)) : (int)sF.matched(t, in.pos, nullptr),
                           mode == MODE_GROUP ? (int)q.grank : rank, mode == MODE_GROUP ? 0 : (int)q.side);
                } else {
                    mp_log("formation off slot=%d why=%s", s, whyName(q.why));
                }
            }
            // ground under the candidate places (on the ground, and the photo): at the start, then every 8th turn
            if (mode != MODE_NONE && sF.needsGround(s, in) && sF.wantsGroundProbe(s, sTurn)) {
                uint8_t m = 0;
                V3 corr;
                for (int i = 0; i < Formation::candidates(mode); i++) {
                    if (groundAt(p, sF.candidate(s, t, rank, n, i), t.cam.up, &corr)) m |= (uint8_t)(1u << i);
                }
                sF.setGround(s, m);
                int pick = -1;
                for (int i = 0; i < Formation::candidates(mode) && pick < 0; i++) {
                    if (m & (1u << i)) pick = i;
                }
                if ((uint8_t)(pick + 1) != sPick[s] && sLogged[s] < cLogCap) {
                    sLogged[s]++;
                    mp_log("formation place slot=%d mode=%s pick=%d ground=%x", s, modeName(mode), pick, (unsigned)m);
                }
                sPick[s] = (uint8_t)(pick + 1);
            }
            V3 tg = sF.target(s, t, in, rank, n);
            // feet on the ground at the drawn place (a slope, a small planet's curve)
            if (mode != MODE_NONE && sF.needsGround(s, in) && len(tg) > 1.0f) {
                V3 corr;
                if (groundAt(p, add(in.pos, tg), t.cam.up, &corr, nullptr, false)) tg = add(tg, corr);   // (the place's death test: above)
            }
            const SlotOut o = sF.finish(s, in, tg, mode == MODE_ARC ? 1.0f : 0.0f);
            if (o.appear) {
                sAppearCount++;
                p->startArrival();   // (the drop-in grow / fade-in, at the new place)
                if (sLogged[s] < cLogCap) {
                    sLogged[s]++;
                    mp_log("formation appear slot=%d at=%s", s, mode == MODE_ARC ? "photo" : "home");
                }
            }
            if (!in.eligible || (len(o.offset) < 0.01f && o.face < 0.001f)) {
                continue;   // (drawn at its true pose: step() set it this turn)
            }
            const V3 drawn = add(in.pos, o.offset);
            float rot[3][3];
            p->trueRotation(rot);
            V3 upTrue = column(rot, 1), upDrawn = upTrue;
            if (len(o.offset) > 1.0f) {   // (always while moved: a tilt that came and went with the ground flag would pop)
                V3 gT, gD;
                if (gravityAt(p, in.pos, &gT) && gravityAt(p, drawn, &gD)) {
                    upTrue = mul(gT, -1.0f);
                    upDrawn = mul(gD, -1.0f);
                }
            }
            float out[3][3];
            drawnRotation(rot, upTrue, upDrawn, sub(toV(camPos), drawn), o.face, out);
            p->setDrawPose(toT(drawn), out);
        }
        sF.end();
        if (t.active && camOk) probe(a, t);
    }
}  // namespace MpFormation
