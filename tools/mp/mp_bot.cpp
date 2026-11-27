// Galaxy 3.0 multiplayer: mp_bot, a headless session client for soak and 4-player tests against a real desktop game.
// It runs the same session layer as the game (platform/src/mp, the global MpCore behind port/mp_bridge.h) over POSIX UDP VLink
// (or the in-process loopback), hosts or joins a room, sends a scripted pose at 30 Hz (a circle) and logs what it receives.
//
//   mp_bot --join  --name Bot1 --udp-port 41101 --bcast 127.0.0.1:41100,127.0.0.1:41102 --duration 120
//   mp_bot --host  --name Host --mode race --slot-wait 4 --udp-port 41100 --bcast 127.0.0.1:41101,127.0.0.1:41102
//   mp_bot --loopback 4 --duration 30            (host + 3 guests in one process, no network)
// Flags:
//   --host | --join          host a room, or join the first compatible room found (Same Wi-Fi scan)
//   --conn wifi|nearby       (default wifi = the POSIX UDP VLink on the desktop)
//   --name NAME --look 0..3  player name and wished character (0 Mario, 1 Luigi, 2 Toad, 3 Cosmic Mario; the host assigns a free one)
//   --mode together|race     host: the mode; join: only rooms of this mode
//   --max N                  host: maximum players (default 4)
//   --slot-wait N            host: start the session when N players are present; join: send poses only once N are present
//   --duration S             leave after S seconds (default 60; 0 = until killed)
//   --udp-port P --bcast L --vip V   the UDP VLink (SMG_MP_UDP_PORT / SMG_MP_UDP_BCAST / SMG_MP_UDP_VIP)
//   --radius R --speed DEG   the circle (default 400 units, 90 degrees per second); --center X,Y,Z
//   --events N               every N frames send a reliable STAMP (relayed event; default 0 = none)
//   --loss P --dup P --delay MS --jitter MS   (--loopback only) impair the in-process network (probabilities 0..1)
//   --verify                 send a reliable counted KILL every 10 frames and check the ones received: exit 3 on a duplicate / reorder / gap
//   --leave-after S          leave (and stop) after S seconds, to test the others' migration; --quiet: less logging
//   --mirror SLOT            send the last pose received from SLOT (its animation, frame, flags) moved onto the circle around
//                            that player (--radius): a puppet with real animations on the screens of a profiling run (MP3 profile);
//                            also answers each new scene serial with LOADED, as a game does (no 40 s scene-barrier timeout)
//   --gather                 act as a console on the Play Together gathering screen (the "one door", port/mp_gather.h): look, open or join a room
//                            by the same rules the game's screen uses, press Start once --slot-wait players (default 2) are there. A stand-in for the
//                            Vita in a desktop test of the practice partner (tools/mp/partner) or of another bot; needs the online build.
//   --online-host            host an ONLINE room through the service (account, room code, STUN / punch / TURN): prints "ROOM CODE: XXXXXX"
//   --online-join CODE       join an ONLINE room by its code (the host is a console or another bot)
//   --online-quick           "Play with Anyone": join a PUBLIC online room of --mode, or open one (what the Vita's gathering button does)
//   --online-friend CODE     join the online room of the friend with this friend code (PSXX-XXXX; needs the friendship first)
//   --service URL            the service (default: the build's own, https://galaxy.psvita.dev; http://127.0.0.1:PORT for a local fake)
//   --data-dir DIR           where the online account (DIR/online/online.dat) and mp.cfg (DIR/mp) live; default ./smg-data. Keep ONE per bot.
//   --visibility 0..2        --online-host: 0 code only (default), 1 friends, 2 public
//   --relay-only             online: no direct path, TURN only (SMG_NET_RELAY_ONLY=1)
//   (online needs the binary of tools/mp/online/build_bot_online.sh with the console's build id, and --data-hash of the console's disc)
//   --along D[,L]            (with --mirror) instead of the circle: D units along the followed player's facing (negative = behind him,
//                            so behind the camera that follows him), L units to his right (MP3 lod2: puppets behind the camera)
// Exit status: 0 = the session ran and was left cleanly, 1 = could not join / host, 2 = the session ended with an error.
#include "../../platform/src/mp/mp_core.h"
#include "../../platform/src/mp/mp_transport_loop.h"
// (weak: the plain host-test build of this tool has no service module; tools/mp/online/build_bot_online.sh links it)
extern "C" void mp_service_set_name(const char* nameUtf8) __attribute__((weak));
extern "C" const char* mp_service_last_code(void) __attribute__((weak));
extern "C" int mp_service_ensure_account(void) __attribute__((weak));
#include "port/mp_gather.h"
extern "C" bool mp_gather_start(MpMode mode, MpConn conn) __attribute__((weak));
extern "C" void mp_gather_status(MpGatherStatus* out) __attribute__((weak));
extern "C" bool mp_gather_begin(void) __attribute__((weak));   // 1 ready, 0 working, <0 = -(MpUiError)

#include <math.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;
void OnSignal(int) { g_stop = 1; }

struct Opts {
    bool host = false, join = false;
    MpConn conn = MP_CONN_WIFI;
    const char* name = "Bot";
    int look = 1;
    MpMode mode = MP_MODE_TOGETHER;
    int maxPlayers = 4;
    int slotWait = 0;
    double duration = 60;
    double scanFor = 30;   // --scan-for S: give up looking for a room after S seconds (a game takes ~25-60 s to boot)
    double leaveAfter = 0;
    float radius = 400, speed = 90, cx = 0, cy = 0, cz = 0;
    int events = 0;
    int loopback = 0;
    float loss = 0, dup = 0;
    int delay = 0, jitter = 0;
    bool verify = false;
    bool quiet = false;
    int mirror = -1;
    bool along = false;   // --along D[,L]: with --mirror, stand D units in front of (negative: behind) the followed player's facing, L to the right
    float alongD = 0, alongL = 0;
    bool online = false;
    bool gather = false;
    const char* onlineCode = "";
    int visibility = 0;
    int onlineKind = 0;   // 0 = host / join by code, 1 = quick match, 2 = join a friend
};

void Usage() {
    fprintf(stderr, "usage: mp_bot (--host | --join | --loopback N) [--conn wifi|nearby] [--name N] [--look 0..3] [--mode together|race]\n"
                    "              [--max N] [--slot-wait N] [--duration S] [--leave-after S] [--udp-port P] [--bcast H:P,...] [--vip V]\n"
                    "              [--radius R] [--speed DEG] [--center X,Y,Z] [--events N] [--data-hash HEX] [--scan-for S] [--quiet]\n"
                    "              [--gather] [--online-host [--visibility 0..2] | --online-join CODE | --online-quick | --online-friend CODE]\n"
                    "              [--service URL] [--data-dir DIR] [--relay-only]   (see the header of tools/mp/mp_bot.cpp)\n");
}

double Now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

const char* PhaseName(MpUiPhase p) {
    static const char* n[] = {"idle", "signing-in", "creating", "searching", "joining", "connecting", "lobby", "playing", "leaving", "failed", "reconnecting"};
    return (unsigned)p < sizeof n / sizeof n[0] ? n[p] : "?";
}

// One bot = one MpCore (the global one for a network bot; extra instances in --loopback mode).
struct Bot {
    MpCore* c;
    Opts o;
    int index;
    bool hosting;
    bool joined = false, started = false, everActive = false;
    MpUiPhase lastPhase = MP_UI_IDLE;
    bool leftIdle = false;
    char lastCode[8] = {0};
    uint32_t turn = 0, sent = 0, frames = 0;
    uint32_t rxState[MP_MAX_PLAYERS] = {0};
    float lastPos[MP_MAX_PLAYERS][3] = {};
    MpPose mirrorPose = {};   // --mirror
    bool haveMirror = false;
    uint16_t loadedSerial = 0xFFFF;
    uint32_t rxOther = 0;
    uint32_t vCount = 0, vRx = 0, vDup = 0, vGap = 0, vOrder = 0;
    uint32_t vLast[MP_MAX_PLAYERS] = {0};
    bool vSeen[MP_MAX_PLAYERS] = {false};
    int vSlot = -1;
    double began = 0, lastReport = 0;
    bool left = false;
    MpUiError endError = MP_ERR_NONE;

    void log(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap;
        va_start(ap, fmt);
        fprintf(stdout, "[bot %s] ", o.name);
        vfprintf(stdout, fmt, ap);
        fputc('\n', stdout);
        fflush(stdout);
        va_end(ap);
    }

    void begin() {
        began = Now();
        lastReport = began;
        if (o.online) {   // (the account is made first, as the menus do: frame() waits for mp_service_ensure_account() and then calls beginRoom())
            if (mp_service_set_name) mp_service_set_name(o.name);
            log("online: signing in to %s", getenv("SMG_ONLINE_URL") ? getenv("SMG_ONLINE_URL") : "the build's own service");
            return;
        }
        if (o.gather) {
            if (!mp_gather_start || !mp_gather_start(o.mode, o.conn)) { log("gather could not start"); return; }
            log("gathering (Play Together, conn=%d mode=%d)", o.conn, o.mode);
            return;
        }
        beginRoom();
    }
    bool onlineStarted = false;
    int lastHeads = -1;
    void gatherStep() {
        MpGatherStatus gs;
        mp_gather_status(&gs);
        if ((int)gs.headCount != lastHeads) {
            lastHeads = gs.headCount;
            char names[200] = {0};
            int n = 0;
            for (int i = 0; i < gs.headCount; i++) n += snprintf(names + n, sizeof names - (size_t)n, " %s%s", gs.heads[i].name, (gs.heads[i].flags & MP_HEAD_LEADER) ? "(leader)" : "");
            log("gather: state=%d heads=%d%s canStart=%d heard=%u", gs.state, gs.headCount, names, gs.canStart, (unsigned)gs.heard);
        }
        if (!started && gs.state == MP_GATHER_GATHERING && gs.canStart && gs.headCount >= (o.slotWait > 0 ? o.slotWait : 2)) {
            started = mp_gather_begin();
            if (started) log("gather: Start pressed with %d players", gs.headCount);
        }
    }
    void beginRoom() {
        if (o.online && !hosting) {
            bool ok;
            if (o.onlineKind == 1) { ok = c->uiQuickMatch(o.mode); log("quick match (public rooms, mode %d)", o.mode); }
            else if (o.onlineKind == 2) { ok = c->uiJoinFriend(o.onlineCode); log("joining friend %s", o.onlineCode); }
            else { ok = c->uiJoinCode(MP_CONN_ONLINE, o.onlineCode); log("joining online room %s", o.onlineCode); }
            if (!ok) log("online join could not start");
            joined = true;
        } else if (hosting) {
            MpHostParams p;
            memset(&p, 0, sizeof p);
            p.conn = o.conn;
            p.visibility = (uint8_t)o.visibility;
            p.settings = mp_config_host_settings(o.mode);
            p.settings.maxPlayers = (uint8_t)o.maxPlayers;
            if (!c->uiHost(&p)) log("host could not start");
            else log("hosting conn=%d mode=%d max=%d", o.conn, o.mode, o.maxPlayers);
        } else {
            log("scanning");
        }
    }

    // one 30 Hz frame; returns false when the bot is done
    bool frame() {
        const double t = Now() - began;
        if (o.online && !onlineStarted) {
            const int r = mp_service_ensure_account ? mp_service_ensure_account() : -(int)MP_ERR_SERVICE_DOWN;
            if (r < 0) {
                endError = (MpUiError)-r;
                log("online sign-in failed: error=%d service code '%s'", -r, mp_service_last_code ? mp_service_last_code() : "");
                return false;
            }
            if (r == 1) { onlineStarted = true; log("online: account ready"); beginRoom(); }
            else if (t > 120) { endError = MP_ERR_TIMEOUT; log("online sign-in timed out"); return false; }
            return true;
        }
        MpUiStatus st;
        c->uiStatus(&st);
        if (st.phase != lastPhase) {
            log("phase %s error=%d players=%d", PhaseName(st.phase), st.error, st.players);
            lastPhase = st.phase;
            if (st.phase != MP_UI_IDLE) leftIdle = true;
        }
        if (st.roomCode[0] && strncmp(st.roomCode, lastCode, sizeof lastCode)) {
            snprintf(lastCode, sizeof lastCode, "%s", st.roomCode);
            log("ROOM CODE: %s", lastCode);
        }
        // online: a failed sign-in / room / join ends the bot at once (the LAN flow keeps scanning instead)
        if (o.online && !everActive && ((leftIdle && st.phase == MP_UI_IDLE) || st.phase == MP_UI_FAILED)) {
            endError = st.error != MP_ERR_NONE ? st.error : MP_ERR_SERVICE_DOWN;
            log("online failed: error=%d (service code '%s')", st.error, mp_service_last_code ? mp_service_last_code() : "");
            return false;
        }
        if (!hosting && !joined && st.phase == MP_UI_IDLE) {
            if (st.error != MP_ERR_NONE && everActive) { endError = st.error; return false; }
            const int n = c->uiLocalScan(o.conn);
            for (int i = 0; i < n; i++) {
                MpLocalRoom r;
                if (c->uiLocalGet(i, &r) && r.compatible && r.mode == o.mode && r.players < r.maxPlayers && c->uiJoinLocal(o.conn, i)) {
                    log("joining %s (%d/%d)", r.host, r.players, r.maxPlayers);
                    joined = true;
                    break;
                }
            }
            if (t > o.scanFor && !joined) { log("no room found"); endError = MP_ERR_ROOM_NOT_FOUND; return false; }
        }
        if (c->active()) everActive = true;
        if (everActive && st.phase == MP_UI_IDLE) {
            endError = st.error;
            if (st.error != MP_ERR_NONE) log("session ended error=%d", st.error);
            return false;
        }
        int present = 0;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) if (c->slotPresent(s)) present++;
        if (o.gather && !started) gatherStep();
        if (hosting && !o.gather && !started && st.phase == MP_UI_LOBBY && (o.slotWait <= 0 || present >= o.slotWait)) {
            started = c->uiStart();
            if (started) log("started with %d players", present);
        }
        // MP3 profile (--mirror): a game's LOADED for every scene serial, so the scene barrier releases without its timeout
        if (o.mirror >= 0 && c->active() && c->sceneSerial() != loadedSerial) {
            loadedSerial = c->sceneSerial();
            uint8_t b[2];
            MpWriter w(b, sizeof b);
            w.u16(loadedSerial);
            c->send(MP_MSG_LOADED, b, (uint16_t)w.n, true);
        }
        // pose
        if (c->active() && (o.slotWait <= 0 || present >= o.slotWait || started)) {
            const float a = (float)(t * o.speed * M_PI / 180.0) + (float)(c->localSlot() >= 0 ? c->localSlot() : index) * 1.5708f;
            MpPose p;
            memset(&p, 0, sizeof p);
            p.turn = turn;
            p.sceneSerial = c->sceneSerial();
            p.zone = 0;
            p.form = MP_FORM_NORMAL;
            p.pos[0] = o.cx + o.radius * cosf(a);
            p.pos[1] = o.cy;
            p.pos[2] = o.cz + o.radius * sinf(a);
            const float yaw = -a;   // facing along the circle
            p.quat[1] = (int16_t)(sinf(yaw * 0.5f) * 32767.0f);
            p.quat[3] = (int16_t)(cosf(yaw * 0.5f) * 32767.0f);
            p.vel[0] = (int8_t)(-sinf(a) * 10.0f);
            p.vel[2] = (int8_t)(cosf(a) * 10.0f);
            p.scale = 64;
            p.anim = 0x12345678u;   // (a hash the game does not know: puppets keep their previous animation)
            p.frame = (uint16_t)((turn * 16) % (60 * 16));
            p.rate = 32;
            p.weights[0] = 255;
            p.flags = MP_POSE_VISIBLE | MP_POSE_ON_GROUND;
            if (o.mirror >= 0 && haveMirror) {   // MP3 profile: the followed player's pose, beside him on the circle
                const float px = p.pos[0] - o.cx, pz = p.pos[2] - o.cz;
                p = mirrorPose;
                p.turn = turn;
                if (o.along) {   // MP3 lod2: on the followed player's facing axis instead of the circle
                    const float qx = p.quat[0] / 32767.0f, qy = p.quat[1] / 32767.0f, qz = p.quat[2] / 32767.0f, qw = p.quat[3] / 32767.0f;
                    const float fx = 2 * (qx * qz + qw * qy), fy = 2 * (qy * qz - qw * qx), fz = 1 - 2 * (qx * qx + qy * qy);
                    const float rx = 1 - 2 * (qy * qy + qz * qz), ry = 2 * (qx * qy + qw * qz), rz = 2 * (qx * qz - qw * qy);
                    p.pos[0] += o.alongD * fx + o.alongL * rx;
                    p.pos[1] += o.alongD * fy + o.alongL * ry;
                    p.pos[2] += o.alongD * fz + o.alongL * rz;
                } else {
                    p.pos[0] += px;
                    p.pos[2] += pz;
                }
            }
            uint8_t buf[68];
            MpWriter w(buf, sizeof buf);
            mp_write_pose(w, p);
            if (c->send(MP_MSG_STATE, buf, (uint16_t)w.n, false)) sent++;
            if (o.verify && frames % 10 == 0) {
                uint8_t v[9] = {(uint8_t)c->localSlot()};
                const uint32_t n = vCount;
                memcpy(v + 1, &n, 4);
                if (c->send(MP_MSG_KILL, v, 9, true)) vCount++;
            }
            if (o.events > 0 && frames % (uint32_t)o.events == 0) {
                uint8_t stamp = (uint8_t)(frames / (uint32_t)o.events);
                c->send(MP_MSG_STAMP, &stamp, 1, true);
            }
        }
        // receive
        MpMsg m;
        while (c->poll(&m)) {
            if (m.type == MP_MSG_STATE && m.fromSlot < MP_MAX_PLAYERS) {
                MpReader r(m.data, m.len);
                MpPose p;
                if (mp_read_pose(r, &p)) {
                    rxState[m.fromSlot]++;
                    memcpy(lastPos[m.fromSlot], p.pos, sizeof p.pos);
                    if ((int)m.fromSlot == o.mirror) { mirrorPose = p; haveMirror = true; }
                }
            } else if (m.type == MP_MSG_KILL && m.len == 9 && m.fromSlot < MP_MAX_PLAYERS) {
                uint32_t n;
                memcpy(&n, m.data + 1, 4);
                const int f = m.fromSlot;
                vRx++;
                if (vSeen[f]) {
                    if (n <= vLast[f]) { vDup++; log("verify: duplicate/old event from slot %d: %u after %u", f, n, vLast[f]); }
                    else if (n != vLast[f] + 1) { vGap++; log("verify: gap from slot %d: %u after %u", f, n, vLast[f]); }
                }
                if (!vSeen[f] || n > vLast[f]) { vSeen[f] = true; vLast[f] = n; }
            } else {
                rxOther++;
                if (!o.quiet) log("rx type=0x%02X from=%d len=%d", m.type, m.fromSlot, m.len);
            }
        }
        MpCoreEvent ev;
        while (c->pollEvent(&ev))
            log("event kind=%d slot=%d other=%d epoch=%d serial=%d arg=%u", ev.kind, ev.slot, ev.other, ev.epoch, ev.serial, ev.arg);
        if (Now() - lastReport >= 5.0) {
            lastReport = Now();
            char line[256];
            int n = 0;
            for (int s = 0; s < MP_MAX_PLAYERS; s++)
                if (rxState[s])
                    n += snprintf(line + n, sizeof line - (size_t)n, " slot%d=%u(%.0f,%.0f,%.0f)", s, rxState[s], lastPos[s][0], lastPos[s][1],
                                  lastPos[s][2]);
            line[n] = 0;
            log("t=%.0fs slot=%d host=%d epoch=%d players=%d ping0=%d sent=%u rx:%s other=%u", t, c->localSlot(), c->hostSlot(),
                c->hostEpoch(), present, c->slotPing(c->hostSlot() >= 0 ? c->hostSlot() : 0), sent, n ? line : " none", rxOther);
            memset(rxState, 0, sizeof rxState);
        }
        if (!left && ((o.leaveAfter > 0 && t >= o.leaveAfter) || (o.duration > 0 && t >= o.duration) || g_stop)) {
            log("leaving");
            c->uiLeave();
            left = true;
        }
        if (left && st.phase == MP_UI_IDLE) return false;
        turn += 2;   // logic turns at 60/s
        frames++;
        return true;
    }
};

}  // namespace

int main(int argc, char** argv) {
    Opts o;
    uint32_t dataHash = 0;   // 0: none (a bot then only finds other bots); against a game: its "[mp] boot data hash=" value
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--host")) o.host = true;
        else if (!strcmp(a, "--join")) o.join = true;
        else if (!strcmp(a, "--quiet")) o.quiet = true;
        else if (!strcmp(a, "--conn")) { o.conn = !strcmp(v, "nearby") ? MP_CONN_NEARBY : MP_CONN_WIFI; i++; }
        else if (!strcmp(a, "--name")) { o.name = v; i++; }
        else if (!strcmp(a, "--look")) { o.look = atoi(v); i++; }
        else if (!strcmp(a, "--mode")) { o.mode = !strcmp(v, "race") ? MP_MODE_RACE : MP_MODE_TOGETHER; i++; }
        else if (!strcmp(a, "--max")) { o.maxPlayers = atoi(v); i++; }
        else if (!strcmp(a, "--slot-wait")) { o.slotWait = atoi(v); i++; }
        else if (!strcmp(a, "--duration")) { o.duration = atof(v); i++; }
        else if (!strcmp(a, "--leave-after")) { o.leaveAfter = atof(v); i++; }
        else if (!strcmp(a, "--udp-port")) { setenv("SMG_MP_UDP_PORT", v, 1); i++; }
        else if (!strcmp(a, "--bcast")) { setenv("SMG_MP_UDP_BCAST", v, 1); i++; }
        else if (!strcmp(a, "--vip")) { setenv("SMG_MP_UDP_VIP", v, 1); i++; }
        else if (!strcmp(a, "--radius")) { o.radius = (float)atof(v); i++; }
        else if (!strcmp(a, "--speed")) { o.speed = (float)atof(v); i++; }
        else if (!strcmp(a, "--center")) { sscanf(v, "%f,%f,%f", &o.cx, &o.cy, &o.cz); i++; }
        else if (!strcmp(a, "--events")) { o.events = atoi(v); i++; }
        else if (!strcmp(a, "--loopback")) { o.loopback = atoi(v); i++; }
        else if (!strcmp(a, "--scan-for")) { o.scanFor = atof(v); i++; }
        else if (!strcmp(a, "--data-hash")) { dataHash = (uint32_t)strtoul(v, 0, 16); i++; }   // the game's "boot data hash" (fingerprint)
        else if (!strcmp(a, "--loss")) { o.loss = (float)atof(v); i++; }
        else if (!strcmp(a, "--dup")) { o.dup = (float)atof(v); i++; }
        else if (!strcmp(a, "--delay")) { o.delay = atoi(v); i++; }
        else if (!strcmp(a, "--jitter")) { o.jitter = atoi(v); i++; }
        else if (!strcmp(a, "--verify")) { o.verify = true; }
        else if (!strcmp(a, "--gather")) { o.gather = true; o.host = true; }
        else if (!strcmp(a, "--online-host")) { o.online = true; o.host = true; o.conn = MP_CONN_ONLINE; }
        else if (!strcmp(a, "--online-join")) { o.online = true; o.join = true; o.conn = MP_CONN_ONLINE; o.onlineCode = v; i++; }
        else if (!strcmp(a, "--online-quick")) { o.online = true; o.join = true; o.conn = MP_CONN_ONLINE; o.onlineKind = 1; }
        else if (!strcmp(a, "--online-friend")) { o.online = true; o.join = true; o.conn = MP_CONN_ONLINE; o.onlineKind = 2; o.onlineCode = v; i++; }
        else if (!strcmp(a, "--service")) { setenv("SMG_ONLINE_URL", v, 1); i++; }
        else if (!strcmp(a, "--data-dir")) { setenv("SMG_DATA_DIR", v, 1); setenv("PETARI_DATA_DIR", v, 1); i++; }
        else if (!strcmp(a, "--visibility")) { o.visibility = atoi(v); i++; }
        else if (!strcmp(a, "--relay-only")) { setenv("SMG_NET_RELAY_ONLY", "1", 1); }
        else if (!strcmp(a, "--mirror")) { o.mirror = atoi(v); i++; }
        else if (!strcmp(a, "--along")) { o.along = true; o.alongD = (float)atof(v); const char* c = strchr(v, ','); o.alongL = c ? (float)atof(c + 1) : 0.0f; i++; }
        else { Usage(); return 1; }
    }
    if (!o.loopback && o.host == o.join) { Usage(); return 1; }
    if (o.maxPlayers < 2 || o.maxPlayers > MP_MAX_PLAYERS || o.look < 0 || o.look >= MP_LOOK_COUNT) { Usage(); return 1; }
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
    // (mp.cfg is not touched: the bot's profile comes from the flags)
    if (dataHash) mp_core_set_data_hash(dataHash);
    std::vector<Bot*> bots;
    MpLoopHub* hub = 0;
    if (o.loopback) {
        if (o.loopback < 2 || o.loopback > MP_MAX_PLAYERS) { Usage(); return 1; }
        hub = new MpLoopHub();
        if (o.loss > 0 || o.dup > 0 || o.delay > 0 || o.jitter > 0) {
            MpLoopImpairment imp = {o.loss, o.dup, (uint32_t)o.delay, (uint32_t)o.jitter, 0};
            hub->setImpairment(imp, 4242);
        }
        for (int i = 0; i < o.loopback; i++) {
            MpCoreOptions co;
            memset(&co, 0, sizeof co);
            co.useThread = true;
            Bot* b = new Bot();
            b->c = new MpCore(co);
            b->c->setLoopback(hub, (uint32_t)(i + 1));
            if (dataHash) b->c->setDataHash(dataHash);
            b->o = o;
            char* nm = (char*)malloc(MP_NAME_BYTES + 1);
            snprintf(nm, MP_NAME_BYTES + 1, "%s%d", o.name, i);
            b->o.name = nm;
            b->o.conn = MP_CONN_LOOPBACK;
            b->o.slotWait = o.slotWait ? o.slotWait : o.loopback;
            b->index = i;
            b->hosting = i == 0;
            b->c->setProfile(nm, (MpLook)(i % MP_LOOK_COUNT));
            bots.push_back(b);
        }
    } else {
        Bot* b = new Bot();
        b->c = mp_core_global();
        b->o = o;
        b->index = 0;
        b->hosting = o.host;
        b->c->setProfile(o.name, (MpLook)o.look);
        bots.push_back(b);
    }
    for (Bot* b : bots) b->begin();
    int running = (int)bots.size();
    double next = Now();
    while (running > 0) {
        running = 0;
        for (Bot* b : bots) {
            b->c->tick(b->turn);
            if (!b->left || b->lastPhase != MP_UI_IDLE)
                if (b->frame()) running++;
        }
        next += 1.0 / 30.0;
        const double wait = next - Now();
        if (wait > 0) usleep((useconds_t)(wait * 1e6));
        else next = Now();
    }
    int rc = 0;
    for (Bot* b : bots) {
        if (o.verify) {
            b->log("verify: sent %u, received %u, duplicates %u, gaps %u", b->vCount, b->vRx, b->vDup, b->vGap);
            if (b->vDup || b->vGap || !b->vRx) rc = 3;
        }
        if (!b->everActive) rc = rc ? rc : 1;
        else if (b->endError != MP_ERR_NONE) rc = 2;
        b->log("done: sent %u poses, exit %d", b->sent, rc);
    }
    for (Bot* b : bots) {
        if (b->c != mp_core_global()) {
            delete b->c;
            free((void*)b->o.name);
        }
        delete b;
    }
    mp_core_shutdown();
    delete hub;
    return rc;
}
