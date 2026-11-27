// Galaxy 3.0 multiplayer (core2): one door: gathering (DESIGN.md 10.6). Every console that chooses Play Together / Star Race listens
// for 1.5 s, joins a gathering room it hears, else opens its own not-started room; rooms that hear each other merge into one by a
// rule both compute from beacon contents alone: more players wins, then the lower room id. A deciding room needs two agreeing
// beacons over 1 s, then advertises CLOSING(target) for 1 s (a room never closes into one that is closing; two rooms closing into
// each other: the lower id stays), then REDIRECTs its guests there and goes itself. A started room is never merged.
#include "mp_gather.h"
#include "mp_core_sys.h"
#include "mp_lan.h"

#include "port/mp_demo.h"
#include "port/mp_direct_code.h"
#include "port/mp_invite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" bool mp_gather_wifi_connected(void) __attribute__((weak));   // the platform (Vita: sceNetCtl); absent: assume connected
extern "C" bool mp_gather_nearby_possible(void) __attribute__((weak));  // ad hoc available in this build / console
// code5: the service's invite calls (platform/src/mp/mp_service.cpp). Weak: a host test or a build without the service has no 5-character codes
// (the host gets the long one, the friend is told the service is not there).
extern "C" bool mp_service_enabled(void) __attribute__((weak));
extern "C" bool mp_svc_invite_register_begin(const char*, const char*) __attribute__((weak));
extern "C" int mp_svc_invite_register_poll(int*) __attribute__((weak));
extern "C" void mp_svc_invite_keep(const char*) __attribute__((weak));
extern "C" void mp_svc_invite_close(const char*) __attribute__((weak));
extern "C" bool mp_svc_invite_resolve_begin(const char*) __attribute__((weak));
extern "C" int mp_svc_invite_resolve_poll(char*, int) __attribute__((weak));
extern "C" void mp_svc_invite_cancel(void) __attribute__((weak));

namespace {
MpUiPhase PhaseOf(MpCore* c) {
    MpUiStatus s;
    c->uiStatus(&s);
    return s.phase;
}
MpUiError ErrorOf(MpCore* c) {
    MpUiStatus s;
    c->uiStatus(&s);
    return s.error;
}
const char* ConnText(MpConn c) {
    return c == MP_CONN_WIFI ? "wifi" : c == MP_CONN_NEARBY ? "nearby" : c == MP_CONN_DIRECT ? "direct" : c == MP_CONN_LOOPBACK ? "loopback" : "none";
}
// mpwifi44: SMG_MP_GATHER_NOLAN=0 turns the gathering without a Same Wi-Fi link off (a link that cannot be opened is an error window again)
bool NoLanAllowed() {
    const char* e = getenv("SMG_MP_GATHER_NOLAN");
    return !(e && e[0] == '0');
}
}  // namespace

// mpwifi44: one line that names the exact reason, for the next console log. `detail` is the link's or the core's own sentence.
void MpGather::noteFail(MpUiError e, const char* reason, const char* detail) {
    m_lastError = e == MP_ERR_NONE ? MP_ERR_INTERNAL : e;
    snprintf(m_lastReason, sizeof m_lastReason, "%s", reason);
    mp_log("gather: failed reason=%s error=%d conn=%s wifi=%d state=%d sub=%d code=%d detail=%s", reason, (int)m_lastError, ConnText(m_conn), m_wifi ? 1 : 0,
           (int)m_state, (int)m_sub, (int)m_codeSub, detail && *detail ? detail : "-");
}

// mpwifi44: the scan's link could not be brought up. Until now every such failure read "Couldn't connect to Wi-Fi. Check your Wi-Fi settings."
// and stayed until the game was restarted. Now:
//   - the console really is not connected (the link says so, or the system does): that message (MP_ERR_NO_WIFI), as before;
//   - Same Wi-Fi was asked, the console IS connected, and something else refused (a socket, a bind, the stack): the gathering goes on
//     without the local link (true): the player is on the gathering screen, the link is tried again every kLanRetryMs, and the far-away
//     doors (Invite from Far Away, Play with Anyone: the service's relay is its backup route) work meanwhile;
//   - anything else: false, lastError() has the error to show.
bool MpGather::lanFailed(uint64_t t) {
    char why[96] = {0};
    MpUiError e = m_c->scanError(why, sizeof why);
    if (e == MP_ERR_NONE) e = MP_ERR_INTERNAL;
    refreshRadio();
    if (m_conn == MP_CONN_WIFI && !m_wifi) e = MP_ERR_NO_WIFI;   // (the system says it: whatever the link's own words were)
    if (e == MP_ERR_NO_WIFI) {
        noteFail(e, m_conn == MP_CONN_NEARBY ? "link-nearby-failed" : "link-wifi-down", why);
        return false;
    }
    if (m_conn != MP_CONN_WIFI || !NoLanAllowed()) {
        noteFail(e, "link-open-failed", why);
        return false;
    }
    m_lastError = MP_ERR_NONE;
    snprintf(m_lastReason, sizeof m_lastReason, "link-open-failed");
    if (!m_noLan)
        mp_log("gather: the Same Wi-Fi link could not be opened though Wi-Fi is connected (%s): gathering without it, trying again every %d s", why[0] ? why : "-",
               (int)(kLanRetryMs / 1000));
    m_noLan = true;
    m_lanRetryAt = t;
    m_state = MP_GATHER_GATHERING;
    m_sub = SUB_NONE;
    m_roomAt = t;
    return true;
}

void MpGather::reset() {
    m_state = MP_GATHER_OFF;
    m_sub = SUB_NONE;
    m_mode = MP_MODE_NONE;
    m_conn = MP_CONN_NONE;
    m_gid = 0;
    m_wifi = mp_gather_wifi_connected ? mp_gather_wifi_connected() : true;
    m_nearby = mp_gather_nearby_possible ? mp_gather_nearby_possible() : false;
    m_phaseAt = m_subAt = m_roomAt = m_lastOtherAt = 0;
    m_target = m_candId = m_candSeq = 0;
    m_candCount = 0;
    m_candFirst = 0;
    m_heard = 0;
    m_lastT = 0;
    memset(m_shunId, 0, sizeof m_shunId);
    memset(m_shunUntil, 0, sizeof m_shunUntil);
    memset(m_lowId, 0, sizeof m_lowId);
    memset(m_lowFirst, 0, sizeof m_lowFirst);
    memset(m_lowLast, 0, sizeof m_lowLast);
    m_prevCount = 0;
    m_prevUntil = 0;
    m_codeSub = CODE_NONE;
    memset(m_secret, 0, sizeof m_secret);
    m_codeAt = 0;
    m_code[0] = 0;
    m_byCode = false;
    m_codeIpFollow = 0;
    m_codePortFollow = 0;
    m_hostIp = 0;
    m_hostPort = 0;
    m_codeLong = false;
    m_inviteLive = false;
    m_inviteTries = 0;
    m_inviteKeepAt = 0;
    m_inviteKeepMs = 0;
    m_invite[0] = 0;
    m_resolveCode[0] = 0;
    m_inviteErr = 0;
    m_noLan = false;
    m_lanRetryAt = 0;
}

void MpGather::refreshRadio() {
    if (mp_gather_wifi_connected) m_wifi = mp_gather_wifi_connected();
    if (mp_gather_nearby_possible) m_nearby = mp_gather_nearby_possible();
}

// smallfix: a non-zero gather id from the secure source; false when it fails (then the start is refused: it used to loop forever)
static bool NewGatherId(uint32_t* gid) {
    for (int i = 0; i < 4; i++) {
        uint32_t v;
        if (!mp_sys_random_try(&v)) return false;
        if (v) { *gid = v; return true; }
    }
    return false;
}
// smallfix: the invite maker's random callback, with a failure flag of its own for this call (no sticky state)
static bool s_inviteRngFailed = false;
static uint32_t InviteRandom() {
    uint32_t v = 0;
    if (!mp_sys_random_try(&v)) s_inviteRngFailed = true;
    return v;
}

bool MpGather::start(MpMode mode, MpConn conn, uint64_t t) {
    m_lastError = MP_ERR_NONE;
    m_lastReason[0] = 0;
    if (mode != MP_MODE_TOGETHER && mode != MP_MODE_RACE) {
        noteFail(MP_ERR_INTERNAL, "bad-mode", "");
        return false;
    }
    // mpwifi44: a gathering that FAILED and was never stopped (a start that failed never reaches the gathering screen, so nothing stopped
    // it) refused every later start: "Couldn't connect to Wi-Fi" until the game was restarted. It is cleared here.
    if (m_state == MP_GATHER_FAILED) {
        mp_log("gather: the failed gathering before this one is cleared");
        stop();
    }
    if (m_state != MP_GATHER_OFF) {
        noteFail(MP_ERR_INTERNAL, "already-gathering", "");
        return false;
    }
    refreshRadio();   // wire: the radio as it is NOW (the answers were taken once, when the driver was made)
    if (conn == MP_CONN_NONE) conn = m_wifi ? MP_CONN_WIFI : MP_CONN_NEARBY;
    if (conn != MP_CONN_WIFI && conn != MP_CONN_NEARBY && conn != MP_CONN_LOOPBACK) {
        noteFail(MP_ERR_INTERNAL, "bad-conn", "");
        return false;
    }
    if (PhaseOf(m_c) == MP_UI_FAILED) m_c->uiLeave();   // (mpwifi44: an attempt that failed and was not left: back to idle, as the menu's error window does)
    if (PhaseOf(m_c) != MP_UI_IDLE) {
        char d[24];
        snprintf(d, sizeof d, "phase %d", (int)PhaseOf(m_c));
        noteFail(MP_ERR_INTERNAL, "core-busy", d);
        return false;
    }
    if (!NewGatherId(&m_gid)) {
        noteFail(MP_ERR_INTERNAL, "random", "the secure random source failed");
        return false;
    }
    m_mode = mode;
    m_conn = conn;
    m_c->setGatherId(m_gid);
    m_c->scanRetry();   // (mpwifi44: a link that failed earlier is tried again now, not remembered as failed)
    if (m_c->uiLocalScan(conn) < 0) {
        if (!lanFailed(t)) {   // (the state stays OFF: the next start is a fresh try)
            m_c->scanRetry();
            m_c->setGatherId(0);
            m_gid = 0;
            m_mode = MP_MODE_NONE;
            m_conn = MP_CONN_NONE;
            return false;
        }
        mp_log("gather: start mode=%d conn=%d (no local link)", (int)mode, (int)conn);
        return true;
    }
    m_state = MP_GATHER_LISTENING;
    m_sub = SUB_NONE;
    m_phaseAt = t;
    mp_log("gather: start mode=%d conn=%d", (int)mode, (int)conn);
    return true;
}

void MpGather::stop() {
    if (m_state == MP_GATHER_OFF) return;
    inviteRelease();
    m_c->setGathering(false, 0);
    m_c->uiLeave();
    m_c->setGatherId(0);
    const bool w = m_wifi, n = m_nearby;
    if (m_noLan) m_c->scanRetry();   // (mpwifi44: nothing of the link that failed is kept)
    reset();
    m_wifi = w;
    m_nearby = n;
}

bool MpGather::switchTo(MpConn conn, uint64_t t) {
    if (m_state == MP_GATHER_OFF || m_state == MP_GATHER_STARTED) return false;
    const MpMode mode = m_mode;
    stop();
    return start(mode, conn, t);
}

int MpGather::rooms(Room* out, int max) {
    MpBeacon b[16];
    MpAddr a[16];
    uint64_t last[16];
    const int n = m_c->heardRooms(b, a, last, max < 16 ? max : 16);
    for (int i = 0; i < n; i++) out[i] = Room{b[i], a[i], last[i]};
    return n;
}

bool MpGather::better(uint8_t pa, uint32_t ida, uint8_t pb, uint32_t idb) const { return pa != pb ? pa > pb : ida < idb; }

void MpGather::shun(uint32_t id, uint64_t t) {   // a room we could not get into: left out of the rule for a while
    int slot = 0;
    for (int i = 0; i < kShun; i++)
        if (m_shunUntil[i] < m_shunUntil[slot]) slot = i;
    m_shunId[slot] = id;
    m_shunUntil[slot] = t + kShunMs;
}
bool MpGather::shunned(uint32_t id) const {
    for (int i = 0; i < kShun; i++)
        if (m_shunId[i] == id && m_shunUntil[i] > m_lastT) return true;
    return false;
}

uint64_t MpGather::lowerSince(uint32_t id, uint64_t t) {   // when a room ranked below this one was first heard (kept per id)
    int slot = 0;
    for (int i = 0; i < kShun; i++) {
        if (m_lowId[i] == id && m_lowLast[i] + 3000 >= t) { m_lowLast[i] = t; return m_lowFirst[i]; }
        if (m_lowLast[i] < m_lowLast[slot]) slot = i;
    }
    m_lowId[slot] = id;
    m_lowFirst[slot] = m_lowLast[slot] = t;
    return t;
}

bool MpGather::gatheringPeer(const MpBeacon& b) const {
    return !shunned(b.sessionId) && b.proto == MP_PROTO_VERSION && b.fingerprint == m_c->fingerprint() && b.mode == m_mode && (b.flags & MP_BEACON_GATHERING) &&
           !(b.flags & MP_BEACON_STARTED);
}

void MpGather::openOwnRoom(uint64_t t) {
    MpHostParams p;
    memset(&p, 0, sizeof p);
    p.conn = m_conn;
    p.settings.mode = (uint8_t)m_mode;
    p.settings.maxPlayers = MP_MAX_PLAYERS;
    p.settings.interaction = 1;
    p.settings.sharedKills = 1;
    p.settings.rivals = 1;
    p.settings.raceWaitSec = 18;
    p.settings.nameTags = 1;
    p.settings.flags = 7;
    m_c->uiLeave();   // (stops the scan)
    if (PhaseOf(m_c) != MP_UI_IDLE) return;   // (still leaving something: next tick)
    if (!m_c->uiHost(&p)) {
        noteFail(ErrorOf(m_c), "host-failed", "the room could not be opened");
        m_state = MP_GATHER_FAILED;
        return;
    }
    m_c->setGathering(true, 0);
    m_state = MP_GATHER_GATHERING;
    m_sub = SUB_NONE;
    m_roomAt = t;
    m_candId = 0;
    mp_log("gather: own room");
}

void MpGather::snapshotHeads() {
    MpGatherStatus s;
    memset(&s, 0, sizeof s);
    m_prevCount = 0;
    for (int slot = 0; slot < MP_MAX_PLAYERS && m_prevCount < MP_GATHER_HEADS; slot++) {
        if (!m_c->slotPresent(slot) || slot == m_c->localSlot()) continue;
        MpGatherHead& h = m_prev[m_prevCount];
        h.id = m_c->slotGatherId(slot);
        if (!h.id) continue;
        m_c->slotName(slot, h.name);
        h.look = (uint8_t)m_c->slotLook(slot);
        h.flags = MP_HEAD_ARRIVING;
        m_prevCount++;
    }
}

// Go into room `target` (by its session id): leave this one, scan, join it when it shows.
void MpGather::follow(uint32_t target, uint64_t t) {
    m_target = target;
    m_sub = SUB_FOLLOW;
    m_subAt = t;
    m_state = MP_GATHER_MERGING;
    m_c->setGathering(false, 0);
    if (PhaseOf(m_c) != MP_UI_IDLE) m_c->uiLeave();
}

void MpGather::tick(uint64_t t) {
    m_lastT = t;
    if (m_state == MP_GATHER_OFF || m_state == MP_GATHER_FAILED) return;
    const MpUiPhase ph = PhaseOf(m_c);
    Room r[16];
    const int n = rooms(r, 16);
    for (int i = 0; i < n; i++)
        if (r[i].lastMs + 600 >= t) m_heard++;
    const uint32_t me = m_c->sessionIdNow();
    bool otherGathering = false;
    for (int i = 0; i < n; i++)
        // (a room advertising CLOSING is on its way somewhere: it does not hold the Start prompt back, so forged CLOSING beacons cannot
        // keep every room from starting)
        // (and only a room ranked ABOVE this one holds it back: one ranked below is the one expected to come here; so of two rooms that
        // hear each other only the higher one ever shows "Press Start")
        if (gatheringPeer(r[i].b) && !(r[i].b.flags & MP_BEACON_CLOSING) && r[i].b.sessionId != me && r[i].lastMs + kQuietMs >= t) {
            if (!me || better(r[i].b.players, r[i].b.sessionId, (uint8_t)(1 + m_c->guestCount()), me)) otherGathering = true;
            else if (t - lowerSince(r[i].b.sessionId, t) < kLowerWaitMs) otherGathering = true;   // (it should come: wait for it a while)
        }
    if (otherGathering) m_lastOtherAt = t;

    switch (m_state) {
    case MP_GATHER_LISTENING: {
        if (ph != MP_UI_IDLE) {   // (a join being given up, a room being left: listen once it is over)
            if (ph != MP_UI_LEAVING) m_c->uiLeave();
            m_phaseAt = t;
            return;
        }
        if (m_c->uiLocalScan(m_conn) < 0) {   // mpwifi44: the link failed after the start (a bring-up that takes more than one poll)
            if (!lanFailed(t)) m_state = MP_GATHER_FAILED;
            return;
        }
        if (t - m_phaseAt < kListenMs) return;
        int best = -1;
        for (int i = 0; i < n; i++) {
            const MpBeacon& b = r[i].b;
            if (!gatheringPeer(b) || !(b.flags & MP_BEACON_OPEN) || (b.flags & MP_BEACON_CLOSING) || r[i].lastMs + kListenMs < t) continue;
            if (best < 0 || better(b.players, b.sessionId, r[best].b.players, r[best].b.sessionId)) best = i;
        }
        if (best >= 0 && m_c->uiJoinLocalRoom(m_conn, r[best].b.sessionId)) {
            m_target = r[best].b.sessionId;
            m_state = MP_GATHER_MERGING;
            m_sub = SUB_JOINING;
            m_subAt = t;
            mp_log("gather: joining a room that gathers");
            return;
        }
        openOwnRoom(t);
        return;
    }
    case MP_GATHER_GATHERING: {
        if (m_noLan && m_codeSub == CODE_NONE) {   // mpwifi44: no Same Wi-Fi link and no room: try the link again; the far-away doors are open meanwhile
            if (t - m_lanRetryAt < kLanRetryMs) return;
            m_lanRetryAt = t;
            if (ph != MP_UI_IDLE) {
                if (ph != MP_UI_LEAVING) m_c->uiLeave();
                return;
            }
            m_c->scanRetry();
            if (m_c->uiLocalScan(m_conn) >= 0) {
                m_noLan = false;
                m_state = MP_GATHER_LISTENING;
                m_phaseAt = t;
                mp_log("gather: the Same Wi-Fi link opens now: looking for players");
                return;
            }
            if (!lanFailed(t)) m_state = MP_GATHER_FAILED;   // (Wi-Fi went meanwhile: the Wi-Fi message)
            return;
        }
        if (ph == MP_UI_PLAYING) {
            m_state = MP_GATHER_STARTED;
            inviteRelease();   // code5: the gathering is over: the code stops working at once
            m_c->setGathering(false, 0);
            return;
        }
        if (m_codeSub != CODE_NONE) codeTick(t);
        if (m_codeSub == CODE_REDIRECTING || m_codeSub == CODE_HOSTING) return;   // (the leader's switch to Play by Address)
        if (!m_c->active()) {
            uint32_t ip;
            uint16_t port;
            uint8_t sec[5];
            if (m_c->takeRedirectCode(&ip, &port, sec)) {   // core2: the room moved to Play by Address: follow it there
                snapshotHeads();
                m_prevUntil = t + kArrivingMs + kFollowMs;
                if (ph != MP_UI_IDLE && ph != MP_UI_LEAVING) m_c->uiLeave();
                m_sub = SUB_LEAVING;   // (joined once the old room is left: SUB_LEAVING below)
                m_state = MP_GATHER_MERGING;
                m_subAt = t;
                memcpy(m_secret, sec, 5);
                m_codeIpFollow = ip;
                m_codePortFollow = port;
                return;
            }
            if (m_conn == MP_CONN_DIRECT && m_codeSub == CODE_NONE && ph == MP_UI_IDLE) {   // a by-address room went: no LAN to look in
                noteFail(MP_ERR_HOST_LEFT, "room-gone", "the by-address room is gone");
                m_state = MP_GATHER_FAILED;
                mp_log("gather: the by-address room is gone");
                return;
            }
            const uint32_t to = m_c->takeRedirect();
            if (to) {
                snapshotHeads();
                m_prevUntil = t + kArrivingMs + kFollowMs;
                follow(to, t);
                return;
            }
            if (ph == MP_UI_IDLE) {   // the room went (its host left without a merge): look again
                m_state = MP_GATHER_LISTENING;
                m_phaseAt = t;
                m_c->uiLocalScan(m_conn);
            }
            return;
        }
        // netfix B5a: a guest that became the host by migration inside this gathering room (the leader's console went) keeps the room
        // open to others: the GATHERING flag is per core and only the first host ever set it, so its beacon lost it
        if (m_c->isHost() && ph == MP_UI_LOBBY && !m_c->gathering()) {
            m_c->setGathering(true, 0);
            mp_log("gather: this console is the room's host now: beacon says gathering");
        }
        if (!m_c->isHost() || m_conn == MP_CONN_DIRECT || m_codeSub != CODE_NONE) return;   // (by address: no rooms nearby to merge)
        // the merge rule (host): the best other gathering room heard now
        const uint8_t myPlayers = (uint8_t)(1 + m_c->guestCount());
        int best = -1;
        for (int i = 0; i < n; i++) {
            const MpBeacon& b = r[i].b;
            if (!gatheringPeer(b) || b.sessionId == me || r[i].lastMs + 1200 < t) continue;
            // (a room that is closing, or that has no room for us, is no candidate: it must not hide the best room that is one. Found by
            // gather_soak -forge 2 seed 27: a forged CLOSING beacon claiming 4 players was "best", so a lone room never merged into the
            // real 2-player room it heard, and never showed Start either)
            if ((b.flags & MP_BEACON_CLOSING) || myPlayers + b.players > MP_MAX_PLAYERS) continue;
            if (best < 0 || better(b.players, b.sessionId, r[best].b.players, r[best].b.sessionId)) best = i;
        }
        if (best < 0 || !better(r[best].b.players, r[best].b.sessionId, myPlayers, me) || (r[best].b.flags & MP_BEACON_CLOSING) ||
            myPlayers + r[best].b.players > MP_MAX_PLAYERS) {
            m_candId = 0;
            return;
        }
        const MpBeacon& o = r[best].b;
        if (o.sessionId != m_candId) {
            m_candId = o.sessionId;
            m_candSeq = o.seq;
            m_candCount = 1;
            m_candFirst = t;
            return;
        }
        if ((int32_t)(o.seq - m_candSeq) > 0) {
            m_candSeq = o.seq;
            m_candCount++;
        }
        if (m_candCount < 2 || t - m_candFirst < kAgreeMs) return;
        m_target = o.sessionId;
        m_c->setGathering(true, m_target);
        m_state = MP_GATHER_MERGING;
        m_sub = SUB_CLOSING;
        m_subAt = t;
        snapshotHeads();
        mp_log("gather: closing into a room ahead of this one");
        return;
    }
    case MP_GATHER_MERGING:
        switch (m_sub) {
        case SUB_CLOSING: {
            bool targetHeard = false, targetClosingIntoUs = false;
            for (int i = 0; i < n; i++)
                if (r[i].b.sessionId == m_target && r[i].lastMs + 1200 >= t) {
                    targetHeard = true;
                    targetClosingIntoUs = (r[i].b.flags & MP_BEACON_CLOSING) && r[i].b.closingInto == me;
                }
            if (!targetHeard || (targetClosingIntoUs && me < m_target) || ph != MP_UI_LOBBY) {   // (gone, or the lower id stays)
                m_c->setGathering(true, 0);
                m_state = ph == MP_UI_PLAYING ? MP_GATHER_STARTED : MP_GATHER_GATHERING;
                m_sub = SUB_NONE;
                m_candId = 0;
                mp_log("gather: merge called off");
                return;
            }
            if (t - m_subAt < kClosingMs) return;
            m_prevUntil = t + kArrivingMs + kFollowMs;
            if (m_c->redirectGuests(m_target)) {
                m_sub = SUB_REDIRECTED;
                m_subAt = t;
                return;
            }
            follow(m_target, t);
            return;
        }
        case SUB_REDIRECTED:
            if (m_c->guestCount() > 0 && t - m_subAt < kRedirectMs) return;
            follow(m_target, t);
            return;
        case SUB_FOLLOW: {
            if (t - m_subAt > kFollowMs) {   // the room is gone: start over (own room or another one)
                shun(m_target, t);
                m_state = MP_GATHER_LISTENING;
                m_sub = SUB_NONE;
                m_phaseAt = t;
                return;
            }
            if (ph != MP_UI_IDLE) {
                if (ph == MP_UI_LEAVING) return;
                m_c->uiLeave();
                return;
            }
            m_c->uiLocalScan(m_conn);
            for (int i = 0; i < n; i++)
                if (r[i].b.sessionId == m_target && (r[i].b.flags & MP_BEACON_OPEN) && !(r[i].b.flags & MP_BEACON_STARTED)) {
                    if (m_c->uiJoinLocalRoom(m_conn, m_target)) {
                        m_sub = SUB_JOINING;
                        m_subAt = t;
                    }
                    break;
                }
            return;
        }
        case SUB_RESOLVING: {   // code5: the friend's code is being looked up: the sealed address comes back, opens with the code
            if (!mp_svc_invite_resolve_poll) { failInvite(MP_INV_OFF); return; }
            char blob[MP_INVITE_BLOB_CHARS + 8];
            const int rr = mp_svc_invite_resolve_poll(blob, sizeof blob);
            if (rr == 0) {
                if (t - m_subAt > kResolveMs) {
                    if (mp_svc_invite_cancel) mp_svc_invite_cancel();
                    failInvite(MP_INV_BUSY);
                }
                return;
            }
            if (rr < 0) { failInvite(-rr); return; }
            uint32_t ip = 0;
            uint16_t port = 0;
            uint8_t sec[MP_DIRECT_SECRET_BYTES] = {0};
            if (mp_invite_open(m_resolveCode, blob, &ip, &port, sec) != MP_CODE_OK) { failInvite(MP_INV_SEALED_BAD); return; }
            memcpy(m_secret, sec, 5);
            m_codeIpFollow = ip;
            m_codePortFollow = port;
            m_sub = SUB_LEAVING;   // (the same path as the long code from here)
            m_subAt = t;
            mp_log("gather: the invite code gave an address");
            return;
        }
        case SUB_LEAVING:   // core2: following a room into Play by Address: leave this one, then join by its LAN address and secret
            if (ph != MP_UI_IDLE) {
                if (ph != MP_UI_LEAVING) m_c->uiLeave();
                if (t - m_subAt < kFollowMs) return;
            }
            if (joinByCode(m_codeIpFollow, m_codePortFollow, m_secret, t)) return;
            if (m_byCode) {
                noteFail(ErrorOf(m_c), "code-join-not-started", "");
                m_state = MP_GATHER_FAILED;
                m_sub = SUB_NONE;
                return;
            }
            m_state = MP_GATHER_LISTENING;   // (could not even start: look again nearby)
            m_sub = SUB_NONE;
            m_phaseAt = t;
            return;
        case SUB_JOINING:
            if (m_c->active()) {
                m_state = ph == MP_UI_PLAYING ? MP_GATHER_STARTED : MP_GATHER_GATHERING;
                m_sub = SUB_NONE;
                m_roomAt = t;
                m_candId = 0;
                mp_log("gather: in the room");
                return;
            }
            if (m_byCode) {   // core2: a code join (or following one): the friend's room or nothing
                if ((ph == MP_UI_IDLE && t - m_subAt > 300) || t - m_subAt > kCodeJoinMs) {
                    noteFail(ErrorOf(m_c) != MP_ERR_NONE ? ErrorOf(m_c) : MP_ERR_TIMEOUT, "code-room-unreachable", "");
                    m_c->uiLeave();
                    m_state = MP_GATHER_FAILED;
                    m_sub = SUB_NONE;
                    mp_log("gather: the room of the code could not be reached");
                }
                return;
            }
            if ((ph == MP_UI_IDLE && t - m_subAt > 300) || t - m_subAt > kFollowMs) {
                shun(m_target, t);
                m_state = MP_GATHER_LISTENING;
                m_sub = SUB_NONE;
                m_phaseAt = t;
                m_c->uiLocalScan(m_conn);
            }
            return;
        default:
            return;
        }
    case MP_GATHER_STARTED:
        if (!m_c->active() && ph == MP_UI_IDLE) {
            m_state = MP_GATHER_OFF;
        }
        return;
    default:
        return;
    }
}

bool MpGather::canStart(uint64_t t) {
    return m_state == MP_GATHER_GATHERING && m_sub == SUB_NONE && (m_codeSub == CODE_NONE || m_codeSub == CODE_READY) && m_c->isHost() &&
           PhaseOf(m_c) == MP_UI_LOBBY &&
           t - m_roomAt >= 1000 && (m_lastOtherAt == 0 || t - m_lastOtherAt >= kQuietMs);
}

void MpGather::status(MpGatherStatus* out, uint64_t t) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->state = m_state;
    out->conn = m_conn;
    if (m_state == MP_GATHER_OFF || m_state == MP_GATHER_LISTENING || m_state == MP_GATHER_FAILED) refreshRadio();   // (cheap; a room in play keeps its answers)
    out->wifiConnected = m_wifi;
    out->nearbyPossible = m_nearby;
    out->heard = m_heard;
    out->canStart = canStart(t);
    if (m_codeSub == CODE_READY) snprintf(out->code, sizeof out->code, "%s", m_code);
    out->codeLong = m_codeSub == CODE_READY && m_codeLong;
    out->inviteError = m_state == MP_GATHER_FAILED ? m_inviteErr : 0;
    if (m_state == MP_GATHER_OFF) return;
    Room r[16];
    const int n = rooms(r, 16);
    const uint32_t me = m_c->sessionIdNow();
    for (int i = 0; i < n; i++) {
        if (r[i].b.sessionId == me || r[i].lastMs + 3000 < t) continue;
        out->roomsSeen++;
        // lobby: the beacons the menu never offered: another version, or no free seat (said on the gathering line, no longer silent)
        if (r[i].b.proto != MP_PROTO_VERSION || r[i].b.fingerprint != m_c->fingerprint()) out->otherVersionRooms++;
        else if (r[i].b.maxPlayers > 0 && r[i].b.players >= r[i].b.maxPlayers) out->fullRooms++;
        if ((r[i].b.flags & MP_BEACON_STARTED) && (r[i].b.flags & MP_BEACON_OPEN) && r[i].b.proto == MP_PROTO_VERSION &&
            r[i].b.fingerprint == m_c->fingerprint())
            out->playingRooms++;
    }
    // heads: self first, then the room's players (by id), then those still on their way from the room we came from
    MpGatherHead& self = out->heads[out->headCount++];
    self.id = m_gid;
    m_c->slotName(m_c->localSlot() >= 0 ? m_c->localSlot() : 0, self.name);
    if (m_c->localSlot() < 0) self.name[0] = 0;
    self.look = (uint8_t)(m_c->localSlot() >= 0 ? m_c->slotLook(m_c->localSlot()) : 0);
    self.flags = MP_HEAD_SELF | (m_c->active() && m_c->isHost() && m_state == MP_GATHER_GATHERING ? MP_HEAD_LEADER : 0);
    if (m_noLan && m_state == MP_GATHER_GATHERING && !m_c->active()) {   // mpwifi44: alone with no room yet: this console's own name, and it leads (the code button)
        m_c->localName(self.name);
        self.flags |= MP_HEAD_LEADER;
    }
    const bool inRoom = m_c->active() && (m_state == MP_GATHER_GATHERING || m_state == MP_GATHER_STARTED);
    if (inRoom) {
        const int hs = m_c->hostSlot();
        for (int slot = 0; slot < MP_MAX_PLAYERS && out->headCount < MP_GATHER_HEADS; slot++) {
            if (slot == m_c->localSlot() || !m_c->slotPresent(slot)) continue;
            MpGatherHead& h = out->heads[out->headCount++];
            h.id = m_c->slotGatherId(slot);
            m_c->slotName(slot, h.name);
            h.look = (uint8_t)m_c->slotLook(slot);
            h.flags = slot == hs && m_state == MP_GATHER_GATHERING ? MP_HEAD_LEADER : 0;
        }
    }
    if (t < m_prevUntil)
        for (int i = 0; i < m_prevCount && out->headCount < MP_GATHER_HEADS; i++) {
            bool here = m_prev[i].id == m_gid;
            for (int k = 0; k < out->headCount; k++) here = here || out->heads[k].id == m_prev[i].id;
            if (!here) out->heads[out->headCount++] = m_prev[i];
        }
    // (stable order after self: by id)
    for (int a = 1; a < out->headCount; a++)
        for (int b = a + 1; b < out->headCount; b++)
            if (out->heads[b].id < out->heads[a].id) {
                const MpGatherHead x = out->heads[a];
                out->heads[a] = out->heads[b];
                out->heads[b] = x;
            }
}

int MpGather::playingRooms(MpLocalRoom* out, int max, uint64_t t) {
    Room r[16];
    const int n = rooms(r, 16);
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        const MpBeacon& b = r[i].b;
        if (!(b.flags & MP_BEACON_STARTED) || !(b.flags & MP_BEACON_OPEN) || r[i].lastMs + 3000 < t || shunned(b.sessionId)) continue;
        if (b.proto != MP_PROTO_VERSION || b.fingerprint != m_c->fingerprint()) continue;
        MpLocalRoom& o = out[k++];
        memset(&o, 0, sizeof o);
        memcpy(o.host, b.name, sizeof o.host);
        o.mode = b.mode;
        o.players = b.players;
        o.maxPlayers = b.maxPlayers;
        o.compatible = true;
        o.id = b.sessionId;
    }
    return k;
}

bool MpGather::joinPlaying(uint32_t roomId, uint64_t t) {
    if (m_state == MP_GATHER_OFF || m_state == MP_GATHER_STARTED || !roomId) return false;
    snapshotHeads();
    follow(roomId, t);
    return true;
}

bool MpGather::begin() {
    if (!canStart(m_lastT)) return false;
    if (!m_c->uiStart()) return false;
    inviteRelease();   // code5
    m_c->setGathering(false, 0);
    m_state = MP_GATHER_STARTED;
    return true;
}

// ---- core2: Play by Address with a code ---------------------------------------------------------------------------------------
bool MpGather::codeAvailable(uint64_t t) {
    (void)t;
    if (m_noLan)   // mpwifi44: no Same Wi-Fi room to turn into one: the by-address room is opened directly when the code is asked for
        return m_state == MP_GATHER_GATHERING && m_sub == SUB_NONE && m_codeSub == CODE_NONE && PhaseOf(m_c) == MP_UI_IDLE && m_c->directAvailable();
    return m_state == MP_GATHER_GATHERING && m_sub == SUB_NONE && m_codeSub == CODE_NONE && m_conn != MP_CONN_DIRECT && m_c->active() &&
           m_c->isHost() && PhaseOf(m_c) == MP_UI_LOBBY && m_c->directAvailable();
}

bool MpGather::getCode(char* out, size_t cap, uint64_t t) {
    if (out && cap) out[0] = 0;
    if (m_codeSub == CODE_READY) {
        if (out && cap) snprintf(out, cap, "%s", m_code);
        return true;
    }
    if (m_codeSub != CODE_NONE) return false;   // (on its way: status.code fills in)
    if (!codeAvailable(t)) return false;
    const uint32_t ip = m_c->directLocalIp();
    if (!ip) return false;   // (no address: no Wi-Fi)
    uint8_t secret[5];
    if (!mp_sys_random_fill(secret, sizeof secret)) {   // smallfix: this call's own result; no code is made, the gather fails as its other failures do
        mp_log("gather: no room code made: the secure random source failed");
        noteFail(MP_ERR_INTERNAL, "random", "no room code made: the secure random source failed");
        m_codeSub = CODE_NONE;
        m_state = MP_GATHER_FAILED;
        return false;
    }
    memcpy(m_secret, secret, sizeof secret);
    m_secret[4] &= 0x0F;   // (36 bits: the short code's; the long one carries the same)
    m_codeAt = t;
    if (m_c->redirectGuestsCode(ip, MP_DIRECT_DEFAULT_PORT, m_secret)) {
        m_codeSub = CODE_REDIRECTING;
        snapshotHeads();
        m_prevUntil = t + kArrivingMs + kFollowMs;
    } else {
        m_codeSub = CODE_REDIRECTING;   // (nobody to tell: straight on)
        m_codeAt = t - kRedirectMs;
    }
    mp_log("gather: the room becomes a Play by Address room%s", m_noLan ? " (there was no Same Wi-Fi room: opened directly)" : "");
    m_noLan = false;   // (mpwifi44: from here on it is a by-address gathering like any other)
    codeTick(t);
    if (m_codeSub == CODE_READY) {
        if (out && cap) snprintf(out, cap, "%s", m_code);
        return true;
    }
    return false;
}

// The leader's switch: tell the members (done), wait for them to go, leave the LAN room, host by address with the secret, and make the
// code once the address is known.
void MpGather::codeTick(uint64_t t) {
    const MpUiPhase ph = PhaseOf(m_c);
    switch (m_codeSub) {
    case CODE_REDIRECTING:
        if (m_c->active() && m_c->guestCount() > 0 && t - m_codeAt < kRedirectMs) return;
        if (ph != MP_UI_IDLE) {
            if (ph != MP_UI_LEAVING) m_c->uiLeave();
            return;
        }
        {
            MpHostParams p;
            memset(&p, 0, sizeof p);
            p.settings.mode = (uint8_t)m_mode;
            p.settings.maxPlayers = MP_MAX_PLAYERS;
            p.settings.interaction = 1;
            p.settings.sharedKills = 1;
            p.settings.rivals = 1;
            p.settings.raceWaitSec = 18;
            p.settings.nameTags = 1;
            p.settings.flags = 7;
            if (!m_c->uiHostDirect(&p, m_secret)) {
                noteFail(ErrorOf(m_c), "code-host-failed", "hosting by address failed");
                m_codeSub = CODE_NONE;
                m_state = MP_GATHER_FAILED;
                mp_log("gather: hosting by address failed");
                return;
            }
        }
        m_conn = MP_CONN_DIRECT;
        m_byCode = true;
        m_roomAt = t;
        m_codeSub = CODE_HOSTING;
        return;
    case CODE_HOSTING: {
        MpAddressInfo a;
        if (!m_c->uiHostAddress(&a) || !a.addr[0]) {
            if (ph == MP_UI_IDLE && t - m_codeAt > 2000) {
                noteFail(ErrorOf(m_c), "code-room-ended", "the by-address room ended before its address was known");
                m_codeSub = CODE_NONE;
                m_state = MP_GATHER_FAILED;
            }
            return;
        }
        unsigned b[4] = {0, 0, 0, 0}, port = MP_DIRECT_DEFAULT_PORT;
        if (sscanf(a.addr, "%u.%u.%u.%u:%u", &b[0], &b[1], &b[2], &b[3], &port) < 4 || b[0] > 255 || b[1] > 255 || b[2] > 255 || b[3] > 255 ||
            port == 0 || port > 65535) {
            noteFail(MP_ERR_INTERNAL, "code-address-bad", a.addr);
            m_codeSub = CODE_NONE;
            m_state = MP_GATHER_FAILED;
            return;
        }
        m_hostIp = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
        m_hostPort = (uint16_t)port;
        mp_log("gather: the room's address is known (%s address)", a.source == MP_ADDR_SRC_ROUTER ? "router" : a.source == MP_ADDR_SRC_STUN ? "public" : "local");
        // code5: the code is five characters the service turns into this address; without the service the long code says it all
        if (mp_service_enabled && mp_service_enabled() && mp_svc_invite_register_begin && mp_svc_invite_register_poll) {
            m_inviteTries = 0;
            startRegister(t);
            return;
        }
        finishLong(t, 0);
        return;
    }
    case CODE_REGISTERING: {
        int keepSec = 300;
        const int r = mp_svc_invite_register_poll ? mp_svc_invite_register_poll(&keepSec) : -int(MP_INV_OFF);
        if (r == 0) {
            if (t - m_codeAt > kRegisterMs) {
                if (mp_svc_invite_cancel) mp_svc_invite_cancel();
                finishLong(t, MP_INV_BUSY);
            }
            return;
        }
        if (r == 1) {
            snprintf(m_code, sizeof m_code, "%s", m_invite);
            m_inviteLive = true;
            m_inviteKeepMs = (uint32_t)keepSec * 1000u;
            m_inviteKeepAt = t + m_inviteKeepMs;
            m_codeLong = false;
            m_codeSub = CODE_READY;
            mp_log("gather: the room's code is ready (5 characters, at the service)");
            return;
        }
        if (-r == MP_INV_TAKEN && ++m_inviteTries < 4) {   // (a one in 28 million chance: pick another)
            startRegister(t);
            return;
        }
        finishLong(t, -r);
        return;
    }
    case CODE_READY:
        if (m_inviteLive && m_inviteKeepMs && t >= m_inviteKeepAt) {   // code5: still gathering: the code is renewed (one request per half of its life)
            if (mp_svc_invite_keep) mp_svc_invite_keep(m_invite);
            m_inviteKeepAt = t + m_inviteKeepMs;
        }
        return;
    default:
        return;
    }
}

// code5: seal this room's address under a fresh code and send it to the service. The code is picked here (so the seal can use it as its key);
// the service refuses a live one (invite_taken) and we pick again.
void MpGather::startRegister(uint64_t t) {
    uint8_t nonce[8];
    s_inviteRngFailed = false;
    const bool nonceOk = mp_sys_random_fill(nonce, 8);
    char blob[MP_INVITE_BLOB_CHARS + 8];
    const bool made = nonceOk && mp_invite_make(InviteRandom, m_invite);
    if (!made || s_inviteRngFailed) {
        // smallfix: no secure random for the invite: the existing fallback, the room's long code (it needs no service)
        mp_log("gather: invite not made: the secure random source failed; the long code is used");
        finishLong(t, MP_INV_BUSY);
        return;
    }
    if (!mp_invite_seal(m_invite, m_hostIp, m_hostPort, m_secret, nonce, blob) ||
        !mp_svc_invite_register_begin(m_invite, blob)) {
        // (begin said no: the service is off, or the account cannot be made: the poll has the reason)
    }
    m_codeAt = t;
    m_codeSub = CODE_REGISTERING;
}

// The service could not give a code (why: MpInviteError, 0 = this build has no service): the room's long code, which needs no service. The
// menu says why it is long (status.codeLong) when the service is the reason.
void MpGather::finishLong(uint64_t t, int why) {
    (void)t;
    m_inviteLive = false;
    if (!mp_direct_code_encode(m_hostIp, m_hostPort, m_secret, m_code, sizeof m_code)) {
        noteFail(MP_ERR_INTERNAL, "code-encode", "");
        m_codeSub = CODE_NONE;
        m_state = MP_GATHER_FAILED;
        return;
    }
    m_codeLong = why != 0;
    m_codeSub = CODE_READY;
    mp_log("gather: the room's code is ready (the long one%s%d)", why ? ": the service could not give one, why=" : ", no service ", why);
}

void MpGather::inviteRelease() {
    if (m_inviteLive && mp_svc_invite_close) mp_svc_invite_close(m_invite);
    m_inviteLive = false;
    if (m_codeSub == CODE_REGISTERING && mp_svc_invite_cancel) mp_svc_invite_cancel();
}

void MpGather::failInvite(int why) {
    m_inviteErr = (uint8_t)why;
    {
        char d[24];
        snprintf(d, sizeof d, "invite error %d", why);
        noteFail(why == MP_INV_NO_WIFI ? MP_ERR_NO_WIFI : MP_ERR_ROOM_NOT_FOUND, "invite-lookup", d);
    }
    m_state = MP_GATHER_FAILED;
    m_sub = SUB_NONE;
    mp_log("gather: the code could not be looked up (%d)", why);
}

bool MpGather::joinByCode(uint32_t ip, uint16_t port, const uint8_t secret[5], uint64_t t) {
    if (!m_c->uiJoinDirect(ip, port, secret)) return false;
    m_conn = MP_CONN_DIRECT;
    m_byCode = true;
    m_state = MP_GATHER_MERGING;
    m_sub = SUB_JOINING;
    m_subAt = t;
    mp_log("gather: joining a room by its code");
    return true;
}

// The friend's side: from the menu (gathering or not). The status then shows the friend's room (heads, leader), FAILED when it cannot
// be reached within kCodeJoinMs.
int MpGather::joinCode(const char* code, uint64_t t) {
    uint32_t ip = 0;
    uint16_t port = 0;
    uint8_t s[MP_DIRECT_SECRET_BYTES] = {0};
    const int kind = mp_invite_kind(code);
    char five[MP_INVITE_LEN + 1] = {0};
    if (kind == MP_INVITE_LEN) {   // code5: an invite: the service turns it into the address (a lookup that is part of the joining)
        const int tr = mp_invite_normalize(code, five);
        if (tr != MP_INV_TYPED_OK) return tr == MP_INV_TYPED_LOOKALIKE ? MP_CODE_LOOKALIKE : tr == MP_INV_TYPED_EMPTY ? MP_CODE_EMPTY : MP_CODE_CHARACTER;
    } else {
        const int r = mp_direct_code_decode(code, &ip, &port, s);
        if (r != MP_CODE_OK) return r;
    }
    if (m_state != MP_GATHER_OFF) stop();
    m_lastError = MP_ERR_NONE;   // (mpwifi44: a new attempt: nothing of the last one's reason)
    m_lastReason[0] = 0;
    if (!NewGatherId(&m_gid)) {
        mp_log("gather: not joined: the secure random source failed");
        return MP_CODE_ADDRESS;
    }
    m_c->setGatherId(m_gid);
    m_byCode = true;
    m_state = MP_GATHER_MERGING;
    m_subAt = t;
    if (kind == MP_INVITE_LEN) {
        mp_log("gather: joining by an invite code");
        memcpy(m_resolveCode, five, sizeof five);
        if (mp_svc_invite_resolve_begin && mp_svc_invite_resolve_poll) mp_svc_invite_resolve_begin(five);
        else m_inviteErr = MP_INV_OFF;
        m_sub = SUB_RESOLVING;
        tick(t);
        return MP_CODE_OK;
    }
    mp_log("gather: joining by code");
    memcpy(m_secret, s, 5);
    m_codeIpFollow = ip;
    m_codePortFollow = port;
    m_sub = SUB_LEAVING;   // (joins once nothing else is left: tick)
    tick(t);
    return MP_CODE_OK;
}

// ---- C API on the global core ----------------------------------------------------------------------------------------------
// (flow round 3, door_shots-030805: these are called on the game thread; like every other bridge call they must open the host-allocation
// scope, else what they make - the driver, and through the core the by-address connector and its link - lands on the game's heap,
// which a scene change resets under the network thread: SIGSEGV in MpCore::pumpConnector. tests/mp/gather_alloc_test.cpp)
extern "C" void port_host_alloc_scope_enter(void) __attribute__((weak));
extern "C" void port_host_alloc_scope_leave(void) __attribute__((weak));
namespace {
struct GatherAllocScope {
    GatherAllocScope() { if (port_host_alloc_scope_enter) port_host_alloc_scope_enter(); }
    ~GatherAllocScope() { if (port_host_alloc_scope_leave) port_host_alloc_scope_leave(); }
};
}  // namespace
#define MP_HOST_ALLOC GatherAllocScope gatherAllocScope_
static MpGather* g_gather = 0;
static MpGather* G() {
    if (!g_gather) g_gather = new MpGather(mp_core_global());
    return g_gather;
}
// wire: the puppet demo's menu tour (SMG_MP_DEMO=menus) shows the gathering screen with the mock roster (port/mp_demo.h): no core, no radio.
// The status is made from the roster: this console first (the leader when the roster says host), the others after it; the code button shows.
static void DemoStatus(MpGatherStatus* out) {
    memset(out, 0, sizeof *out);
    out->state = MP_GATHER_GATHERING;
    out->conn = MP_CONN_WIFI;
    out->wifiConnected = true;
    out->nearbyPossible = true;
    out->canStart = mp_demo_roster_is_host() && mp_demo_roster_local_slot() >= 0;
    const int me = mp_demo_roster_local_slot();
    for (int pass = 0; pass < 2; pass++)
        for (int slot = 0; slot < MP_MAX_PLAYERS && out->headCount < MP_GATHER_HEADS; slot++) {
            if (!mp_demo_roster_present(slot) || (slot == me) != (pass == 0)) continue;
            MpGatherHead& h = out->heads[out->headCount++];
            h.id = 1000u + (uint32_t)slot;
            snprintf(h.name, sizeof h.name, "%s", mp_demo_roster_name(slot));
            h.look = (uint8_t)mp_demo_roster_look(slot);
            h.flags = (uint8_t)((slot == me ? MP_HEAD_SELF : 0) | (slot == me && mp_demo_roster_is_host() ? MP_HEAD_LEADER : 0));
        }
}

extern "C" {
bool mp_gather_start(MpMode mode, MpConn conn) { MP_HOST_ALLOC; mp_mark_entered(); return G()->start(mode, conn, mp_sys_now_ms()); }
void mp_gather_stop(void) { MP_HOST_ALLOC; if (g_gather) g_gather->stop(); }
// mpwifi44 (port/mp_gather.h)
int mp_gather_last_error(const char** reason) {
    if (reason) *reason = g_gather ? g_gather->lastReason() : "";
    return g_gather ? (int)g_gather->lastError() : 0;
}
bool mp_gather_switch(MpConn conn) { MP_HOST_ALLOC; return G()->switchTo(conn, mp_sys_now_ms()); }
void mp_gather_status(MpGatherStatus* out) {
    MP_HOST_ALLOC;
    if (g_mpDemoRoster) { if (out) DemoStatus(out); return; }   // wire: the demo's menu tour (never beside a real gathering: the demo has no core)
    if (!g_gather && !mp_entered()) {   // (core2: nothing is made before multiplayer is entered)
        if (out) memset(out, 0, sizeof *out);
        return;
    }
    const uint64_t t = mp_sys_now_ms();
    G()->tick(t);
    G()->status(out, t);
}
int mp_gather_playing_rooms(MpLocalRoom* out, int max) { MP_HOST_ALLOC; return out && max > 0 ? G()->playingRooms(out, max, mp_sys_now_ms()) : 0; }
bool mp_gather_join_playing(uint32_t roomId) { MP_HOST_ALLOC; return G()->joinPlaying(roomId, mp_sys_now_ms()); }
bool mp_gather_begin(void) { MP_HOST_ALLOC; return G()->begin(); }
bool mp_gather_code_available(void) { MP_HOST_ALLOC; if (g_mpDemoRoster) return true; return g_gather && g_gather->codeAvailable(mp_sys_now_ms()); }   // (wire: the demo shows the button)
bool mp_gather_get_code(char* out, size_t cap) {
    MP_HOST_ALLOC;
    if (out && cap) out[0] = 0;
    if (!g_gather) return false;
    const uint64_t t = mp_sys_now_ms();
    g_gather->tick(t);
    return g_gather->getCode(out, cap, t);
}
int mp_ui_join_direct_code(const char* code) { MP_HOST_ALLOC; mp_mark_entered(); return G()->joinCode(code, mp_sys_now_ms()); }
}
