// Galaxy 3.0 multiplayer: the party strip and the team star celebration, game side (owner: celebrate). Pure logic: MpPartyLogic.hpp.
//
// Party strip: the other players' heads beside the life counter (drawn by MpPanelDraw with the toasts' MiiIcon heads). This file
// turns the session into the strip's model each frame (normal / quiet / down or bubble / star flash) and follows PlayerLeft (the HUD's
// own appear and hide) through noteHud, so the strip slides in and out with the counters and is gone in cutscenes.
//
// Team star: when a Power Star is won every console (1) lets the puppets play the game's own star-get animation during its star-get
// demo, (2) sends one TALLY with its own counters for this scene (star bits, coins, helped) and whether its player was near the star,
// (3) after the demo, in the existing post-star hold, shows the star card (MpUi::showStarCard): "TEAM STAR!" when every present player
// was near the star, else "<name> got the Power Star!". The card never holds the player and auto-dismisses.
#include <stdlib.h>
#include <string.h>
#include "Game/MP/MpTeam.hpp"
#include "Game/MP/MpSpectate.hpp"
#include "Game/MP/MpAway.hpp"
#include "Game/MP/MpPresent.hpp"   // MP3 ingame
#include "Game/MP/MpPresentLogic.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include "Game/MP/MpDemo.hpp"   // MP3 demo

#include "Game/MP/MpFlow.hpp"
#include "Game/MP/MpFlowLogic.hpp"
#include "Game/MP/MpPartyLogic.hpp"
#include "Game/MP/MpPoseMath.hpp"
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/MP/MpStarBits.hpp"
#include "Game/MP/MpUi.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "port/mp_prof.h"

using namespace MpPartyLogic;

namespace {
    MpPartyLogic::Counter sCount;
    MpPartyLogic::Ledger sLedger;
    MpPartyLogic::Flash sFlash;
    bool sAwayLogged[MP_MAX_PLAYERS];                 // touches: the last away state logged per slot
    MpPartyLogic::LinkGauge sGauge[MP_MAX_PLAYERS];   // touches: the strip's connection bars
    bool sSerialValid = false;
    uint16_t sSerial = 0;

    uint32_t sFrame = 0;          // frames the strip was asked for
    uint32_t sHudFrame = 0;       // the frame PlayerLeft last said it lives
    bool sHudShown = false;
    float sAppear = 0.0f;
    bool sWasActive = false;
    float sPaneX = 0.0f, sPaneY = 0.0f, sPaneScale = 0.0f, sPaneRest = 0.0f;   // MP3 ingame: the counter's picture pane
    uint32_t sPaneFrame = 0;
    MpPartyLogic::StripMotion sMotion;   // MP3 ingame: heads slide in and out (joins and leaves, no words)

    bool sCheer = false;          // the star-get demo of this console runs
    int sCheerAge = 0;
    uint32_t sCheerHash = 0;
    bool sCheerNone = false;      // looked for the animation and found none in this scene
    bool sCheerLogged = false;
    bool sRowLogged = false;      // MP3 ingame: the star row logged once per demo

    bool sStarSeen = false;       // a star of the current serial was accepted
    bool sTallySent = false;
    bool sPendingTally = false;   // star collected while reconnecting: send TALLY once mp_active() returns
    bool sPendingNear = false;
    int sLastLocalSlot = -1;
    bool sCardShown = false;
    bool sCardLive = false;       // a card of serial sCardSerial may still be up
    uint16_t sCardSerial = 0;
    unsigned sCardMask = 0;       // tallies the card was built from

    void ensureSerial() {
        const uint16_t cur = MpGameInternal::serial();
        if (sSerialValid && cur == sSerial) return;
        sSerialValid = true;
        sSerial = cur;
        sCount.reset();
        sLedger.reset(cur);
        sStarSeen = sTallySent = sPendingTally = sPendingNear = sCardShown = false;
        sCardMask = 0;
    }

    unsigned haveMask() {
        unsigned m = 0;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) if (sLedger.have[i]) m |= 1u << i;
        return m;
    }

    void sendTally(bool nearStar) {
        if (sTallySent) return;
        sTallySent = true;
        const Tally t = sCount.tally(sSerial, nearStar);
        uint8_t b[16];
        MpWriter w(b, sizeof b);
        writeTally(w, t);
        const bool ok = w.ok() && mp_send(MP_MSG_TALLY, b, (uint16_t)w.n, true);
        sLedger.note(mp_local_slot(), t);   // (the host's relay does not echo it back)
        mp_log("team tally sent serial=%u bits=%u coins=%u helped=%u near=%d ok=%d", (unsigned)t.serial, (unsigned)t.starBits,
               (unsigned)t.coins, (unsigned)t.helped, (int)t.nearStar, ok ? 1 : 0);
    }

    void buildCard(MpStarCard* c) {
        const unsigned present = MpGameInternal::presentMask();
        c->count = 0;
        c->by = (int8_t)sLedger.by;
        c->team = sLedger.team(present);
        c->teamBits = MpStarBits::teamCount();   // costar
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            if (!(present & (1u << s)) || c->count >= MP_MAX_PLAYERS) continue;
            MpStarCard::Entry& e = c->e[c->count++];
            e.slot = (uint8_t)s;
            e.have = sLedger.have[s];
            e.starBits = e.have ? sLedger.t[s].starBits : 0;
            e.coins = e.have ? sLedger.t[s].coins : 0;
            e.helped = e.have ? sLedger.t[s].helped : 0;
        }
    }
}  // namespace

namespace MpTeam {

bool counterOwnHead() {   // MP3 ingame
    if (!mp_active()) return false;
    return MpPartyLogic::counterPane((int)mp_slot_look(mp_local_slot())) == MpPartyLogic::COUNTER_OWN_HEAD;
}

void noteCounterPane(float x, float y, float scale) {   // MP3 ingame
    if (!(scale > 0.0f) || !(scale < 100.0f)) return;
    sPaneX = x;
    sPaneY = y;
    sPaneScale = scale;
    if (sPaneRest <= 0.0f || scale < sPaneRest) sPaneRest = scale;   // (at rest it is smallest: the count-up only grows it)
    sPaneFrame = sFrame;
}

bool hudShown() { return sHudShown && sFrame - sHudFrame <= 3; }   // MP3 ingame

bool starRowModel(MpPanel::StarRow* out) {   // MP3 ingame
    if (out == nullptr || !mp_active() || mp_mode() != MP_MODE_TOGETHER) return false;
    const float a = MpPresent::starRowAlpha(sCheerAge, sCheer);
    if (a <= 0.0f) return false;
    out->count = 0;
    out->by = -1;
    out->alpha = a;
    for (int s = 0; s < MP_MAX_PLAYERS && out->count < MpPanel::kStarRowMax; s++) {
        if (!mp_slot_present(s)) continue;
        const MpLook look = mp_slot_look(s);
        if (sStarSeen && s == sLedger.by) out->by = (int8_t)out->count;
        out->look[out->count++] = (uint8_t)(look >= 0 && look < MpPanel::kCharacters ? look : 0);
    }
    if (out->count > 0 && !sRowLogged) {
        sRowLogged = true;
        mp_log("team star row count=%d by=%d", out->count, (int)out->by);
    }
    return out->count > 0;
}

void noteHud(bool shown) {
    sHudFrame = sFrame;
    sHudShown = shown;
}

bool stripModel(MpPanel::Strip* out) {
    ++sFrame;
    const bool active = mp_active() || MpDemo::live();   // MP3 demo: the puppet demo shows the strip too
    if (active != sWasActive) {
        sWasActive = active;
        sAppear = 0.0f;
        sFlash.reset();
        sMotion.reset();   // MP3 ingame (the session's first roster is "already in")
        for (bool& a : sAwayLogged) a = false;
    }
    if (!active) return false;
    MpPartyLogic::PartyInput in;
    in.localSlot = mp_local_slot();
    in.presentMask = 0;
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        in.look[s] = 0;
        in.state[s] = MpPanel::PARTY_NORMAL;
        in.flash[s] = sFlash.progress(s);
        in.link[s] = MpPanel::LINK_GOOD;
        in.away[s] = false;
        if (!mp_slot_present(s)) { sGauge[s] = MpPartyLogic::LinkGauge(); continue; }
        in.presentMask |= 1u << s;
        const MpLook look = mp_slot_look(s);
        in.look[s] = (uint8_t)(look >= 0 && look < MpPanel::kCharacters ? look : 0);
        MpPose pose;
        const bool have = s != in.localSlot && MpPuppets::lastPose(s, &pose);
        in.state[s] = partyState(have, have ? pose.flags : 0, s != in.localSlot && mp_slot_quiet(s));
        {   // MP3 touches: away = the pose flag the player's console sends (the local one reads its own state)
            const bool away = s == in.localSlot ? MpAway::local() : (have && (pose.flags & MP_POSE_AWAY) != 0);
            in.away[s] = away;
            if (away != sAwayLogged[s]) {
                sAwayLogged[s] = away;
                mp_log("away slot=%d on=%d", s, away ? 1 : 0);
            }
        }
        if (s != in.localSlot) in.link[s] = sGauge[s].update(mp_slot_ping_ms(s), mp_slot_quiet(s));   // MP3 touches: good / weak / lost
#if defined(TARGET_PC) && !defined(__vita__)
        {   // desktop test hook (never on the Vita): SMG_MP_TEST_LINK="<slot>:<0 good|1 weak|2 lost>,..." forces the bars for a screenshot
            static const char* hook = getenv("SMG_MP_TEST_LINK");
            for (const char* q = hook; q && *q; ) {
                const int hs = atoi(q);
                const char* c = strchr(q, ':');
                if (c && hs == s && atoi(c + 1) >= 0 && atoi(c + 1) <= 2) in.link[s] = static_cast<uint8_t>(atoi(c + 1));
                q = strchr(q, ',');
                if (q) ++q;
            }
        }
#endif
    }
    // follows PlayerLeft: shown while it lives and is not hidden (it speaks every logic turn; two frames of silence = gone)
    const bool want = sHudShown && sFrame - sHudFrame <= 3;
    sAppear += ((want ? 1.0f : 0.0f) - sAppear) * 0.22f;
    if (sAppear < 0.01f && !want) sAppear = 0.0f;
    if (sAppear > 0.99f && want) sAppear = 1.0f;
    makeStrip(in, sAppear, out);
    sMotion.apply(in, out);   // MP3 ingame
    out->counterX = -1.0f;
    if (out->localLook >= 0 && sFrame - sPaneFrame <= 3 && sPaneRest > 0.0f) {   // MP3 ingame: where the game's picture pane is
        const float sw = (float)MR::getScreenWidth(), sh = (float)MR::getScreenHeight();
        const float w = sh * (MR::isScreen16Per9() ? 16.0f / 9.0f : 4.0f / 3.0f);
        out->counterX = sw > 0.0f ? sPaneX * (w / sw) : sPaneX;
        out->counterY = sPaneY;
        out->counterScale = sPaneScale / sPaneRest;
    }
    return (out->count > 0 || out->localLook >= 0) && sAppear > 0.0f;
}

void onStar(int bySlot, uint16_t serial, int starId) {
    ensureSerial();
    if (serial != sSerial || bySlot < 0 || bySlot >= MP_MAX_PLAYERS) return;
    const bool active = mp_active();
    const int localSlot = active ? mp_local_slot() : sLastLocalSlot;
    if (active) sLastLocalSlot = localSlot;
    sStarSeen = true;
    sLedger.by = bySlot;
    sFlash.start(bySlot);
    // near the star: the collector's own pose is where the star was; the collector itself is near by definition
    bool nearStar = bySlot == localSlot;
    int dist = nearStar ? 0 : -1;   // fix3: logged (the radius was tuned on stage data; real sessions show the usual spread)
    if (!nearStar && active && localSlot >= 0) {
        MpPose them, me;
        if (MpPuppets::lastPose(bySlot, &them) && MpPuppets::lastPose(localSlot, &me)) {
            const float d = nearDistance(them.pos, me.pos);
            dist = d < 0.0f ? -1 : d > 1e6f ? 1000000 : static_cast<int>(d);
            nearStar = isNear(them.pos, me.pos);
        }
    }
    mp_log("team star id=%d by=%d near=%d dist=%d", starId, bySlot, nearStar ? 1 : 0, dist);
    if (active) {
        sendTally(nearStar);
    } else if (!sTallySent) {
        sPendingTally = true;
        sPendingNear = nearStar;
        if (localSlot >= 0 && localSlot < MP_MAX_PLAYERS) sLedger.note(localSlot, sCount.tally(sSerial, nearStar));
    }
}

void handle(const MpMsg& m) {
    ensureSerial();
    Tally t;
    if (!readTally(m.data, m.len, &t)) {
        mp_log("team drop TALLY from=%d (invalid)", m.fromSlot);
        return;
    }
    if (m.fromSlot >= MP_MAX_PLAYERS || m.fromSlot == mp_local_slot()) return;
    if (!sLedger.note(m.fromSlot, t)) return;   // another serial, or a repeat
    mp_log("team tally got slot=%d bits=%u coins=%u helped=%u near=%d", m.fromSlot, (unsigned)t.starBits, (unsigned)t.coins,
           (unsigned)t.helped, (int)t.nearStar);
}

void update() {
    ensureSerial();
    if (mp_active()) {
        sLastLocalSlot = mp_local_slot();
        if (sPendingTally) {
            sPendingTally = false;
            sendTally(sPendingNear);
        }
    }
    if (mp_mode() == MP_MODE_TOGETHER && MpGameInternal::sceneExists() && MpGameInternal::sceneState() == MP_SCENE_ACTION)
        sCount.observe(MR::getCoinNum(), MR::getStarPieceNum());
    // MP3 costar: the Star Bit count is the team's; the card shows what this player picked up himself
    if (MpStarBits::teamActive()) sCount.bits = MpStarBits::ownCollected() > kMaxCount ? kMaxCount : MpStarBits::ownCollected();
    sFlash.tick();
    // fix3: the card's own timer (6 s, MpOverlay) is what ends it. It used to go as soon as the next scene played: in a pair run
    // the host moves on at once after the star and a desktop load takes ~1 s, so the card was up 0.6-1 s instead of 6 s. It rides
    // over the load and into the next scene (a window over the screen: it never holds the player or a MOVE).
}

void noteSceneState(int st) {
    const bool cheer = st == MP_SCENE_STAR_GET || st == MP_SCENE_GRAND_STAR_GET;
    if (cheer && !sCheer) {
        sRowLogged = false;   // MP3 ingame
        sCheerAge = 0;
        sCheerHash = 0;
        sCheerNone = false;
        sCheerLogged = false;
    }
    sCheer = cheer;
    if (cheer) ++sCheerAge;
}

// MP3 ingame (the owner): no star card in Play Together any more (it turned co-op into a contest and stood between the star and
// play): the faces stand under the game's own "You got a Star!" instead (starRowModel). The card's code stays (kStarCardInTogether).
constexpr bool kStarCardInTogether = false;

void onClearHold() {
    ensureSerial();
    if (!kStarCardInTogether) return;   // MP3 ingame
    if (mp_mode() != MP_MODE_TOGETHER || !sStarSeen || sSerial != MpGameInternal::serial()) return;
    const unsigned have = haveMask();
    if (sCardShown && have == sCardMask) return;
    MpStarCard c;
    buildCard(&c);
    if (c.count < 1) return;
    if (!sCardShown) mp_log("team card serial=%u team=%d by=%d tallies=%d/%d", (unsigned)sSerial, c.team ? 1 : 0, (int)c.by,
                            MpFlowLogic::popcount4(have), c.count);
    sCardShown = sCardLive = true;
    sCardSerial = sSerial;
    sCardMask = have;
    MpUi::showStarCard(c);
}

void noteHelped() {
    if (!mp_active()) return;
    ensureSerial();
    sCount.help();
}

bool celebrate(MpPoseF* p) {
    if (!sCheer || sCheerNone || MpSpectate::watching()) return false;   // (spectate: the race hold's open view shows the others playing on)
    if (sCheerHash == 0) {
        static const char* const kNames[] = {"powerstarget", "PowerStarGet"};   // (Mario's star-get bck; the Toad puppet maps it to its joy clip)
        for (const char* n : kNames) {
            const uint32_t h = MpPoseMath::animHash(n);
            if (MpPuppetsInt::names().find(h) != nullptr) { sCheerHash = h; break; }
        }
        if (sCheerHash == 0) {
            sCheerNone = true;
            mp_log("team cheer: no star-get animation in this scene's table");
            return false;
        }
    }
    if (!sCheerLogged) {
        sCheerLogged = true;
        mp_log("team cheer: puppets play the star-get animation");
    }
    p->anim = sCheerHash;
    p->flags = (uint16_t)((p->flags | MP_POSE_SIMPLE_BCK) & ~(uint16_t)MP_POSE_UPPER);
    p->upperAnim = 0;
    p->hasTracks = false;
    p->frame = (float)sCheerAge;
    p->rate = 1.0f;
    return true;
}

void sceneEnd() {
    sCheer = false;
    sCheerAge = 0;
}

}  // namespace MpTeam
