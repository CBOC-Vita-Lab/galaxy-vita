// core2: scriptable fake of port/mp_gather.h (see mp_gather_fake.h). Not thread-safe (menu tests run on one thread).
#include "mp_gather_fake.h"

#include "port/mp_invite.h"

#include <stdio.h>
#include <string.h>

namespace {
struct Fake {
    MpGatherStatus st;
    uint32_t leader;
    bool otherRoom;
    MpLocalRoom playing[4];
    int playingCount;
    int hiddenFull, hiddenOther;
    int begins;
    uint32_t joinedRoom;
    char joinedCode[MP_DIRECT_CODE_MAX];
    int service;
} g;

// mpwifi44: why the next mp_gather_start fails / why the gathering failed (kept across mp_gather_stop, as the real driver keeps it)
int gStartFail = 0;
int gLastError = 0;
char gLastReason[32] = "";

void Recompute() {
    for (int i = 0; i < g.st.headCount; i++) {
        uint8_t& f = g.st.heads[i].flags;
        f = (uint8_t)((f & ~MP_HEAD_LEADER) | (g.st.heads[i].id == g.leader ? MP_HEAD_LEADER : 0));
    }
    const bool selfLeader = g.st.headCount > 0 && g.st.heads[0].id == g.leader;
    g.st.canStart = g.st.state == MP_GATHER_GATHERING && selfLeader && !g.otherRoom;
    g.st.playingRooms = (uint8_t)g.playingCount;
    g.st.roomsSeen = (uint8_t)(g.playingCount + (g.otherRoom ? 1 : 0) + g.hiddenFull + g.hiddenOther);
    g.st.fullRooms = (uint8_t)g.hiddenFull;
    g.st.otherVersionRooms = (uint8_t)g.hiddenOther;
}
}  // namespace

extern "C" {
void mp_gather_fake_reset(void) { memset(&g, 0, sizeof g); g.st.wifiConnected = true; g.st.nearbyPossible = true; g.service = 2; gStartFail = 0; gLastError = 0; gLastReason[0] = 0; }
// mpwifi44: the next mp_gather_start answers false with this MpUiError and reason (0 = starts work again)
void mp_gather_fake_fail_start(int error, const char* reason) {
    gStartFail = error;
    snprintf(gLastReason, sizeof gLastReason, "%s", reason ? reason : "");
}
// mpwifi44: the gathering goes FAILED with this MpUiError and reason
void mp_gather_fake_fail(int error, const char* reason) {
    g.st.state = MP_GATHER_FAILED;
    gLastError = error;
    snprintf(gLastReason, sizeof gLastReason, "%s", reason ? reason : "");
}
int mp_gather_last_error(const char** reason) {
    if (reason) *reason = gLastReason;
    return gLastError;
}
void mp_gather_fake_set_service(int mode) { g.service = mode; }
void mp_gather_fake_invite_fail(int e) { if (e) { g.st.state = MP_GATHER_FAILED; g.st.inviteError = (uint8_t)e; } }
void mp_gather_fake_set_hidden(int full, int otherVersion) { g.hiddenFull = full; g.hiddenOther = otherVersion; Recompute(); }
void mp_gather_fake_set_radio(bool wifi, bool nearby) { g.st.wifiConnected = wifi; g.st.nearbyPossible = nearby; }
void mp_gather_fake_add_head(uint32_t id, const char* name, uint8_t look) {
    for (int i = 0; i < g.st.headCount; i++)
        if (g.st.heads[i].id == id) { snprintf(g.st.heads[i].name, sizeof g.st.heads[i].name, "%s", name); g.st.heads[i].look = look; return; }
    if (g.st.headCount >= MP_GATHER_HEADS) return;
    MpGatherHead& h = g.st.heads[g.st.headCount];
    memset(&h, 0, sizeof h);
    h.id = id;
    snprintf(h.name, sizeof h.name, "%s", name);
    h.look = look;
    h.flags = g.st.headCount == 0 ? MP_HEAD_SELF : 0;
    if (g.st.headCount == 0 && !g.leader) g.leader = id;
    g.st.headCount++;
    Recompute();
}
void mp_gather_fake_remove_head(uint32_t id) {
    for (int i = 1; i < g.st.headCount; i++)
        if (g.st.heads[i].id == id) {
            memmove(&g.st.heads[i], &g.st.heads[i + 1], sizeof(MpGatherHead) * (size_t)(g.st.headCount - i - 1));
            g.st.headCount--;
            break;
        }
    Recompute();
}
void mp_gather_fake_set_leader(uint32_t id) { g.leader = id; Recompute(); }
void mp_gather_fake_set_arriving(uint32_t id, bool on) {
    for (int i = 0; i < g.st.headCount; i++)
        if (g.st.heads[i].id == id) g.st.heads[i].flags = (uint8_t)(on ? g.st.heads[i].flags | MP_HEAD_ARRIVING : g.st.heads[i].flags & ~MP_HEAD_ARRIVING);
}
void mp_gather_fake_open_room(void) { if (g.st.state == MP_GATHER_LISTENING) g.st.state = MP_GATHER_GATHERING; Recompute(); }
void mp_gather_fake_merge_begin(void) { if (g.st.state == MP_GATHER_GATHERING) g.st.state = MP_GATHER_MERGING; Recompute(); }
void mp_gather_fake_merge_end(uint32_t leader) {
    if (g.st.state != MP_GATHER_MERGING) return;
    g.st.state = MP_GATHER_GATHERING;
    g.leader = leader;
    for (int i = 0; i < g.st.headCount; i++) g.st.heads[i].flags &= (uint8_t)~MP_HEAD_ARRIVING;
    Recompute();
}
void mp_gather_fake_other_room(bool heard) { g.otherRoom = heard; Recompute(); }
void mp_gather_fake_add_playing(uint32_t roomId, const char* host, uint8_t mode, uint8_t players) {
    if (g.playingCount >= 4) return;
    MpLocalRoom& r = g.playing[g.playingCount++];
    memset(&r, 0, sizeof r);
    snprintf(r.host, sizeof r.host, "%s", host);
    r.mode = mode; r.players = players; r.maxPlayers = 4; r.compatible = true; r.id = roomId;
    Recompute();
}
void mp_gather_fake_heard(uint32_t n) { g.st.heard = n; }
int mp_gather_fake_begins(void) { return g.begins; }
uint32_t mp_gather_fake_joined_room(void) { return g.joinedRoom; }
const char* mp_gather_fake_joined_code(void) { return g.joinedCode; }

// ---- the API ----
bool mp_gather_start(MpMode mode, MpConn conn) {
    gLastError = 0;
    if (gStartFail) { gLastError = gStartFail; return false; }
    if (mode == MP_MODE_NONE || g.st.state != MP_GATHER_OFF) return false;
    if (conn == MP_CONN_NONE) conn = g.st.wifiConnected ? MP_CONN_WIFI : MP_CONN_NEARBY;
    if (conn == MP_CONN_NEARBY && !g.st.nearbyPossible) return false;
    g.st.conn = conn;
    g.st.state = MP_GATHER_LISTENING;
    Recompute();
    return true;
}
void mp_gather_stop(void) {
    const bool w = g.st.wifiConnected, n = g.st.nearbyPossible;
    const int sf = gStartFail, le = gLastError;   // (mpwifi44: the reason outlives the stop)
    char lr[sizeof gLastReason];
    memcpy(lr, gLastReason, sizeof lr);
    mp_gather_fake_reset();
    g.st.wifiConnected = w; g.st.nearbyPossible = n;
    gStartFail = sf; gLastError = le;
    memcpy(gLastReason, lr, sizeof lr);
}
bool mp_gather_switch(MpConn conn) {
    if (g.st.state == MP_GATHER_OFF || g.st.state == MP_GATHER_STARTED || (conn != MP_CONN_WIFI && conn != MP_CONN_NEARBY)) return false;
    g.st.conn = conn;
    g.st.state = MP_GATHER_LISTENING;
    if (g.st.headCount > 1) g.st.headCount = 1;
    g.leader = g.st.headCount ? g.st.heads[0].id : 0;
    Recompute();
    return true;
}
void mp_gather_status(MpGatherStatus* out) { if (out) *out = g.st; }
int mp_gather_playing_rooms(MpLocalRoom* out, int max) {
    int n = 0;
    for (; out && n < g.playingCount && n < max; n++) out[n] = g.playing[n];
    return n;
}
bool mp_gather_join_playing(uint32_t roomId) {
    for (int i = 0; i < g.playingCount; i++)
        if (g.playing[i].id == roomId) { g.joinedRoom = roomId; return true; }
    return false;
}
bool mp_gather_begin(void) {
    if (!g.st.canStart) return false;
    g.st.state = MP_GATHER_STARTED;
    g.begins++;
    Recompute();
    return true;
}
bool mp_gather_get_code(char* out, size_t cap) {
    const bool leader = g.st.headCount && g.st.heads[0].id == g.leader;
    if (!out || cap < MP_DIRECT_CODE_MAX || !leader || g.st.state != MP_GATHER_GATHERING || !g.st.wifiConnected) return false;
    const uint8_t secret[5] = {1, 2, 3, 4, 5};
    if (g.service == 0) snprintf(out, cap, "KCBC4");
    else if (!mp_direct_code_encode(0xC0A80105u, 0, secret, out, cap)) return false;
    g.st.codeLong = g.service == 1;
    snprintf(g.st.code, sizeof g.st.code, "%s", out);
    g.st.conn = MP_CONN_DIRECT;
    return true;
}
int mp_ui_join_direct_code(const char* code) {
    uint32_t ip;
    uint16_t port;
    uint8_t s[5];
    if (mp_invite_kind(code) == MP_INVITE_LEN) {   // code5: the lookup runs (the test ends it); a typed look-alike is refused here, no request
        char five[MP_INVITE_LEN + 1];
        const int tr = mp_invite_normalize(code, five);
        if (tr != MP_INV_TYPED_OK) return tr == MP_INV_TYPED_LOOKALIKE ? MP_CODE_LOOKALIKE : tr == MP_INV_TYPED_EMPTY ? MP_CODE_EMPTY : MP_CODE_CHARACTER;
        snprintf(g.joinedCode, sizeof g.joinedCode, "%s", code);
        g.st.state = MP_GATHER_MERGING;
        return MP_CODE_OK;
    }
    const int r = mp_direct_code_decode(code, &ip, &port, s);
    if (r == MP_CODE_OK) snprintf(g.joinedCode, sizeof g.joinedCode, "%s", code);
    return r;
}
}
