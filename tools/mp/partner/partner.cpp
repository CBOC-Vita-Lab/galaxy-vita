// Galaxy 3.0 multiplayer: the practice partner. A small program for a second computer (a Mac or a Linux box in Terminal) that joins
// (or hosts) a room as a second player, so one Vita can try multiplayer alone. It links the real session layer (platform/src/mp), so it
// always speaks the protocol of the tree it was built from; it has no game, no screen and no game data.
//
//   partner                         look for a room on this Wi-Fi (Vita: Multiplayer > Play Together) and join it, follow you
//   partner --join 192.168.1.20     join by address (Vita: Online > Play by Address; the lobby shows the address)
//   partner --host                  open a Same Wi-Fi room the Vita can join (lobby / character / join checks; it picks no galaxy)
//
// Where it stands in a scene: --follow (default) copies the Vita player's poses a moment later and a few steps beside / behind, so it
// jumps and runs like you; --wander walks a small circle near where it first saw you; --idle stands there. Every so often it sends a
// phrase or an emote, and moves its Star Pointer, so those features can be seen. Test levers: --drop-after, --leave-after, --loss,
// --delay, --jitter, --dup, --bubble-me. Ctrl-C leaves cleanly (BYE). Run `partner --help` for every flag.
//
// Left out on purpose: a STAR (a peer may only send the real stars of a scenario: the Vita player collects those), scene changes (a
// bot host has no game to choose a galaxy with), the online service (no TLS), and router port mapping.
#include "../../platform/src/mp/mp_core.h"

// the StageData hash (--data-root): mp_boot.cpp's own routine, compiled into this file (mp_port_boot itself is not called)
#include "../../platform/src/mp/mp_boot.cpp"

#include "../../decomp/include/Game/MP/MpBubbleLogic.hpp"   // pure bubble maths (no game headers)
#include "../../decomp/include/Game/MP/MpPoseMath.hpp"       // pure pose maths: the animation hash, turn arithmetic
#include "partner_impair.h"

#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#ifndef PARTNER_SOURCE_REV
#define PARTNER_SOURCE_REV "unknown"
#endif

namespace {

using MpBubbleLogic::V3;

volatile sig_atomic_t g_stop = 0;
volatile sig_atomic_t g_stopCount = 0;
void OnSignal(int) {
    g_stop = 1;
    g_stopCount = g_stopCount + 1;
    if (g_stopCount >= 2) _exit(130);   // a second Ctrl-C: out at once
}

double Now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

enum Behaviour { B_FOLLOW, B_WANDER, B_IDLE };
enum Goal { G_JOIN_LAN, G_JOIN_ADDR, G_HOST };

struct Opts {
    Goal goal = G_JOIN_LAN;
    std::string address;
    Behaviour behaviour = B_FOLLOW;
    const char* name = "Partner";
    int look = MP_LOOK_LUIGI;
    MpMode mode = MP_MODE_TOGETHER;
    int maxPlayers = 4;
    int startWhen = 2;           // --host: start the session when this many players are in
    const char* room = 0;        // --room NAME: only join a room of this host name
    int followSlot = -1;
    double followDelayMs = 600;
    double followDist = 220;
    double wanderRadius = 250, wanderSpeed = 6;   // units, units per turn
    bool haveAnchor = false;
    V3 anchor = {0, 0, 0};
    double chatEvery = 25, pointerEvery = 15;
    bool bubble = false;
    bool rescue = true;          // when the other player is in a bubble, go to it (a teammate's touch pops it)
    double bubbleAfter = 20;
    double dropAfter = 0, leaveAfter = 0, duration = 0;
    double scanFor = 0;          // 0 = look until stopped
    bool once = false;
    const char* logFile = "partner.log";
    const char* dataRoot = 0;
    uint32_t dataHash = 0;
    bool haveDataHash = false;
    bool quiet = false;
};

void Usage(FILE* f) {
    fprintf(f,
            "Galaxy 3.0 practice partner (protocol %d, build id \"%s\", source %s)\n"
            "\n"
            "usage: partner [what to do] [how to look] [test levers]\n"
            "\n"
            "what to do (default: --join-lan)\n"
            "  --join-lan            look for a Same Wi-Fi room on this network and join it\n"
            "  --join ADDRESS[:PORT] join by address (\"Play by Address\"; port 41100 when left out)\n"
            "  --host                open a Same Wi-Fi room for the Vita to join (it starts when %d players are in)\n"
            "  --room NAME           --join-lan: only the room whose host is called NAME\n"
            "  --once                leave the program when the session ends (default: look again)\n"
            "\n"
            "who it is\n"
            "  --name NAME           player name (default Partner)\n"
            "  --look CHARACTER      mario luigi toad cosmic yellowtoad cosmicluigi, or 0..5 (default luigi; the host gives the\n"
            "                        first free one if yours is taken)\n"
            "  --mode together|race  --host only (default together)\n"
            "  --max N               --host only: players (2..4, default 4)\n"
            "  --start-when N        --host only: start the session when N players are in (default 2; 0 = wait for nothing)\n"
            "\n"
            "in a scene (default --follow)\n"
            "  --follow [SLOT]       copy that player's moves a moment later, a few steps beside / behind (default: the host, or the\n"
            "                        first other player when this is the host)\n"
            "  --follow-delay MS     how far behind in time (default 600)\n"
            "  --follow-dist UNITS   how far away (default 220; Mario is about 150 tall)\n"
            "  --wander              walk a small circle where it first saw the other player (--radius UNITS, --speed UNITS/TURN)\n"
            "  --idle                stand there\n"
            "  --anchor X,Y,Z        where --wander / --idle stand when there is nobody to anchor to (tests)\n"
            "  --chat-every SEC      a phrase and an emote in turns (default 25; 0 = never)\n"
            "  --pointer-every SEC   hold the Star Pointer for 2.5 s (default 15; 0 = never)\n"
            "  --bubble-me [SEC]     fall into a bubble SEC seconds after the other player shows up (default 20); touch it to pop it\n"
            "  --no-rescue           do not go to the other player when they are in a bubble (by default the partner does, which pops it)\n"
            "\n"
            "test levers (times count from the moment it is in the room)\n"
            "  --drop-after SEC      vanish without saying goodbye (tests timeouts and host migration)\n"
            "  --leave-after SEC     leave politely\n"
            "  --duration SEC        stop after SEC seconds (default: until Ctrl-C)\n"
            "  --loss PCT --dup PCT --delay MS --jitter MS    bad network on what the partner sends (and PCT loss of the poses it hears)\n"
            "\n"
            "version\n"
            "  --data-hash HEX       the game-data part of the version fingerprint (the Vita's log shows \"[mp] boot data hash=...\")\n"
            "  --data-root DIR       work it out from a prepared game folder (DIR/sys/fst.bin) instead\n"
            "                        (default: data-hash.txt next to the program, when there is one)\n"
            "  --scan-for SEC        give up looking for a room after SEC seconds (default: never)\n"
            "  --udp-port P --bcast HOST:PORT,...   Same Wi-Fi link port / broadcast targets (two partners on one computer)\n"
            "  --log FILE            log file (default partner.log)\n"
            "  --print-data-hash     with --data-root: print the data hash and stop\n"
            "  --version             print the protocol version and build id\n",
            MP_PROTO_VERSION, MP_BUILD_ID, PARTNER_SOURCE_REV, 2);
}

const char* LookName(int l) {
    static const char* n[] = {"Mario", "Luigi", "Toad", "Cosmic Mario", "Yellow Toad", "Cosmic Luigi"};
    return l >= 0 && l < MP_LOOK_COUNT ? n[l] : "?";
}
int ParseLook(const char* s) {
    if (!s) return -1;
    if (s[0] >= '0' && s[0] <= '9') { const int v = atoi(s); return v >= 0 && v < MP_LOOK_COUNT ? v : -1; }
    std::string t;
    for (; *s; s++) if (*s != ' ' && *s != '-' && *s != '_') t += (char)((*s >= 'A' && *s <= 'Z') ? *s + 32 : *s);
    if (t == "mario") return MP_LOOK_MARIO;
    if (t == "luigi") return MP_LOOK_LUIGI;
    if (t == "toad") return MP_LOOK_TOAD;
    if (t == "cosmic" || t == "cosmicmario") return MP_LOOK_COSMIC;
    if (t == "yellowtoad") return MP_LOOK_YELLOW_TOAD;
    if (t == "cosmicluigi") return MP_LOOK_COSMIC_LUIGI;
    return -1;
}
const char* PhaseName(MpUiPhase p) {
    static const char* n[] = {"idle", "signing in", "creating", "searching", "joining", "connecting", "lobby", "playing", "leaving", "failed", "reconnecting"};
    return (unsigned)p < sizeof n / sizeof n[0] ? n[p] : "?";
}

// ---- small maths (quaternions x y z w, positions as V3) -----------------------------------------------------------------------------
using MpBubbleLogic::add; using MpBubbleLogic::sub; using MpBubbleLogic::mul; using MpBubbleLogic::v3;
struct Q { float x, y, z, w; };
Q QFromWire(const int16_t q[4]) {
    Q r = {q[0] / 32767.0f, q[1] / 32767.0f, q[2] / 32767.0f, q[3] / 32767.0f};
    const float n = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    if (!(n > 1e-6f)) return Q{0, 0, 0, 1};
    return Q{r.x / n, r.y / n, r.z / n, r.w / n};
}
void QToWire(Q q, int16_t out[4]) {
    const float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    const float k = n > 1e-6f ? 32767.0f / n : 0;
    out[0] = (int16_t)lrintf(q.x * k); out[1] = (int16_t)lrintf(q.y * k); out[2] = (int16_t)lrintf(q.z * k);
    out[3] = n > 1e-6f ? (int16_t)lrintf(q.w * k) : 32767;
}
Q QMul(Q a, Q b) {
    return Q{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Q QYaw(float rad) { return Q{0, sinf(rad * 0.5f), 0, cosf(rad * 0.5f)}; }   // about the local up axis
V3 QRot(Q q, V3 v) {   // v + 2w(u x v) + 2 u x (u x v)
    const V3 u = v3(q.x, q.y, q.z);
    const V3 c1 = v3(u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x);
    const V3 c2 = v3(u.y * c1.z - u.z * c1.y, u.z * c1.x - u.x * c1.z, u.x * c1.y - u.y * c1.x);
    return add(v, add(mul(c1, 2.0f * q.w), mul(c2, 2.0f)));
}
V3 PosOf(const MpPose& p) { return v3(p.pos[0], p.pos[1], p.pos[2]); }
void SetPos(MpPose* p, V3 v) { p->pos[0] = v.x; p->pos[1] = v.y; p->pos[2] = v.z; }

// ---- one received pose (the whole STATE payload kept, so the animation tail travels on) ---------------------------------------------------
struct Sample {
    uint8_t raw[MP_MAX_PAYLOAD];
    uint16_t len = 0;
    MpPose pose;
    double at = 0;
};
const int kHist = 256;           // about 8 s of the leader's 30 Hz
const int kLoop = 90;            // 3 s of a moving / standing animation to replay
struct AnimLoop {
    std::vector<Sample> s;
    bool full = false;
    int play = 0;
};

class Partner {
public:
    Opts o;
    MpCore* c = 0;

    // session bookkeeping
    double t0 = 0, tJoin = 0;
    bool everActive = false, started = false, joinedLogged = false;
    MpUiPhase lastPhase = MP_UI_IDLE;
    MpUiError endError = MP_ERR_NONE;
    bool leaving = false, done = false, dropped = false;
    double leaveAt = 0;
    int exitCode = 0;
    uint32_t turn = 0;
    uint16_t loadedSerial = 0xFFFF;
    std::string roomLabel;
    bool scanningNow = false, joinPending = false;
    double nextScanPrint = 0;
    std::vector<uint32_t> seenRooms;
    bool haveIncompat = false;
    int incompatProto = 0;
    uint32_t incompatFp = 0;
    bool versionShown = false;
    double joinStart = 0;
    int instantRetries = 0;
    uint32_t dataHash = 0;

    // leader tracking
    int leader = -1, lastLeader = -1;
    std::vector<Sample> hist;       // ring (kHist)
    int histHead = 0, histCount = 0;
    Sample lastLead;
    bool haveLead = false;
    uint16_t leadSerial = 0xFFFF;
    uint8_t leadZone = 0;
    AnimLoop loops[2];              // 0 standing, 1 moving
    double leadSeenAt = 0, firstLeadAt = 0;
    // wander / idle anchor
    bool anchored = false;
    V3 anchorPos = {0, 0, 0};
    Q anchorQ = {0, 0, 0, 1};
    uint16_t anchorSerial = 0xFFFF;
    uint8_t anchorZone = 0;
    float wanderA = 0;
    // pointer / chat
    double nextChat = 0, nextPointer = 0, pointerUntil = 0, nextPointerSend = 0;
    int chatCount = 0, pointerCount = 0;
    bool pointerOn = false;
    // bubble
    bool bubbled = false;
    int bubbleAge = 0;
    V3 bubbleC = {0, 0, 0}, bubbleV = {0, 0, 0};
    double nextBubble = 0;
    int bubbleCount = 0;
    // counters / status
    uint32_t rxState[MP_MAX_PLAYERS] = {0}, rxWindow = 0, sent = 0, framesSent = 0;
    double lastRxMark = 0, rxRate = 0;
    uint32_t rxLeadTotal = 0, rxChat = 0, rxEmote = 0, rxPointer = 0, rxBubble = 0, droppedRx = 0;
    double lastStatus = 0, lastPeriodic = 0;
    uint32_t rng = 0x2468ACE1u;
    FILE* logf = 0;
    bool tty = false;
    bool statusShown = false;
    std::string lastIncompatMsg;

    // ---- output ----------------------------------------------------------------------------------------------------------------------
    double rnd01() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng >> 8) / double(1u << 24); }
    void logLine(const char* line) {
        if (!logf) return;
        fprintf(logf, "[%7.2f] %s\n", Now() - t0, line);
        fflush(logf);
    }
    void clearStatus() { if (tty && statusShown) { fputs("\r\x1b[K", stdout); statusShown = false; } }
    void say(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char line[600];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line, sizeof line, fmt, ap);
        va_end(ap);
        clearStatus();
        fprintf(stdout, "%s\n", line);
        fflush(stdout);
        logLine(line);
    }
    void info(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {   // log only
        char line[600];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line, sizeof line, fmt, ap);
        va_end(ap);
        logLine(line);
    }

    // ---- the version words ------------------------------------------------------------------------------------------------------------
    std::string versionSentence() {
        char b[400];
        snprintf(b, sizeof b, "this partner speaks protocol %d, build id \"%s\", data hash %08x, fingerprint %08x (source %s)", MP_PROTO_VERSION,
                 MP_BUILD_ID, (unsigned)dataHash, (unsigned)c->fingerprint(), PARTNER_SOURCE_REV);
        return b;
    }
    void explainMismatch(bool haveTheirs) {
        char b[900];
        if (haveTheirs && incompatProto != MP_PROTO_VERSION)
            snprintf(b, sizeof b,
                     "Version mismatch: the Vita's game speaks protocol %d, but %s. Build the partner again from the same source as the Vita "
                     "build (the protocol number must match).",
                     incompatProto, versionSentence().c_str());
        else if (haveTheirs)
            snprintf(b, sizeof b,
                     "Version mismatch: same protocol (%d) but the Vita's fingerprint is %08x and %s. The build id or the game data differs: "
                     "build the partner from the Vita's source and pass --data-hash with the value from the Vita's log (\"[mp] boot data hash=\"), "
                     "or --data-root with the prepared game folder.",
                     incompatProto, (unsigned)incompatFp, versionSentence().c_str());
        else
            snprintf(b, sizeof b,
                     "Version mismatch: the host refused us as a different version; %s. Build the partner from the same source as the Vita "
                     "build and give it the Vita's data hash (--data-hash, from \"[mp] boot data hash=\" in the Vita's log).",
                     versionSentence().c_str());
        if (lastIncompatMsg != b) { say("%s", b); lastIncompatMsg = b; }
    }

    // ---- start ------------------------------------------------------------------------------------------------------------------------
    void begin() {
        t0 = Now();
        tty = isatty(1);
        c = mp_core_global();
        c->setProfile(o.name, (MpLook)o.look);
        if (o.haveDataHash) dataHash = o.dataHash;
        if (dataHash) mp_core_set_data_hash(dataHash);
        char l[400];
        snprintf(l, sizeof l, "Galaxy 3.0 practice partner: protocol %d, build id \"%s\", data hash %08x, fingerprint %08x, source %s", MP_PROTO_VERSION,
                 MP_BUILD_ID, (unsigned)dataHash, (unsigned)c->fingerprint(), PARTNER_SOURCE_REV);
        say("%s", l);
        if (!dataHash)
            say("No data hash given (--data-hash / --data-root / data-hash.txt): a Vita that has read its game data will not accept this partner.");
        say("Name \"%s\", wishes for %s. Ctrl-C leaves cleanly. Log: %s", o.name, LookName(o.look), o.logFile);
        nextChat = 0;
    }

    // ---- joining / hosting ------------------------------------------------------------------------------------------------------------
    void startGoal(double t) {
        if (o.goal == G_HOST) {
            MpHostParams p;
            memset(&p, 0, sizeof p);
            p.conn = MP_CONN_WIFI;
            p.settings = mp_config_host_settings(o.mode);
            p.settings.maxPlayers = (uint8_t)o.maxPlayers;
            if (!c->uiHost(&p)) { say("Could not open a room."); exitCode = 1; done = true; return; }
            roomLabel = std::string("hosting \"") + o.name + "\"";
            say("Opened a Same Wi-Fi room \"%s\" (%s, up to %d players). On the Vita: Multiplayer > Same Wi-Fi > join.", o.name,
                o.mode == MP_MODE_RACE ? "race" : "together", o.maxPlayers);
        } else if (o.goal == G_JOIN_ADDR) {
            if (!c->uiJoinAddress(o.address.c_str())) {
                say("\"%s\" is not an address I can use (try 192.168.1.20 or 192.168.1.20:41100).", o.address.c_str());
                exitCode = 1;
                done = true;
                return;
            }
            joinStart = Now();
            roomLabel = "address " + o.address;
            say("Joining %s ...", o.address.c_str());
        } else {
            scanningNow = true;
            joinPending = false;
            say("Looking for a room on this network ... (Vita: Multiplayer > Play Together, then wait on the gathering screen)");
        }
        (void)t;
    }

    void scanStep(double t) {
        const int n = c->uiLocalScan(MP_CONN_WIFI);
        for (int i = 0; i < n; i++) {
            MpLocalRoom r;
            if (!c->uiLocalGet(i, &r)) continue;
            bool seen = false;
            for (uint32_t id : seenRooms) if (id == r.id) seen = true;
            if (!seen) {
                seenRooms.push_back(r.id);
                say("Found room \"%s\": %s, %d/%d players%s", r.host, r.mode == MP_MODE_RACE ? "race" : "together", r.players, r.maxPlayers,
                    r.compatible ? "" : " (different version)");
            }
            if (!r.compatible) { explainMismatch(haveIncompat); continue; }
            if (o.room && strcmp(o.room, r.host) != 0) continue;
            if (r.mode != o.mode && o.mode == MP_MODE_RACE) continue;
            if (r.players >= r.maxPlayers) continue;
            if (c->uiJoinLocalRoom(MP_CONN_WIFI, r.id)) {
                roomLabel = std::string("room \"") + r.host + "\"";
                say("Joining %s ...", roomLabel.c_str());
                scanningNow = false;
                joinPending = true;
                return;
            }
        }
        if (o.scanFor > 0 && t > o.scanFor) { say("No room found in %.0f s.", o.scanFor); exitCode = 1; done = true; }
    }

    void restartAfterEnd(double t) {
        resetScene();
        everActive = started = joinedLogged = false;
        joinPending = false;
        leaving = false;
        seenRooms.clear();
        if (o.once) { done = true; return; }
        endError = MP_ERR_NONE;
        startGoal(t);
    }

    // ---- scene state --------------------------------------------------------------------------------------------------------------------
    void resetScene() {
        histHead = histCount = 0;
        haveLead = false;
        anchored = false;
        bubbled = false;
        pointerOn = false;
        pointerCount = 0;
    }

    int pickLeader() {
        if (o.followSlot >= 0) return c->slotPresent(o.followSlot) && o.followSlot != c->localSlot() ? o.followSlot : -1;
        const int h = c->hostSlot();
        if (h >= 0 && h != c->localSlot() && c->slotPresent(h)) return h;
        for (int s = 0; s < MP_MAX_PLAYERS; s++)
            if (s != c->localSlot() && c->slotPresent(s)) return s;
        return -1;
    }

    static bool Usable(const MpPose& p) {
        return (p.flags & MP_POSE_VISIBLE) && !(p.flags & (MP_POSE_DEAD | MP_POSE_BUBBLE));
    }

    void onLeaderState(const MpMsg& m, const MpPose& p, double t) {
        if (haveLead && (p.sceneSerial != leadSerial || p.zone != leadZone)) {   // another scene / zone: what we had is no use
            histHead = histCount = 0;
            anchored = false;
            if (p.sceneSerial != leadSerial && bubbled) { bubbled = false; info("bubble ended: new scene"); }
        }
        if (haveLead && MpPoseMath::turnDiff(p.turn, lastLead.pose.turn) <= 0 && MpPoseMath::turnDiff(lastLead.pose.turn, p.turn) < 100000 &&
            p.sceneSerial == leadSerial)
            return;   // an older copy
        Sample& s = hist[(size_t)histHead];
        memcpy(s.raw, m.data, m.len);
        s.len = m.len;
        s.pose = p;
        s.at = t;
        histHead = (histHead + 1) % kHist;
        if (histCount < kHist) histCount++;
        lastLead = s;
        if (!haveLead) { firstLeadAt = t; info("leader slot %d: first pose in scene %u zone %u at (%.0f, %.0f, %.0f)", leader, p.sceneSerial, p.zone, p.pos[0], p.pos[1], p.pos[2]); }
        haveLead = true;
        leadSerial = p.sceneSerial;
        leadZone = p.zone;
        leadSeenAt = t;
        rxLeadTotal++;
        // the animations to borrow for --wander / --idle: three seconds of him standing and of him running, on the ground
        if ((p.flags & MP_POSE_ON_GROUND) && Usable(p) && !(p.flags & MP_POSE_IN_DEMO)) {
            const float sp = hypotf((float)p.vel[0], (float)p.vel[2]);
            const int k = sp >= 3.0f ? 1 : (sp <= 0.5f ? 0 : -1);
            if (k >= 0 && !loops[k].full) {
                loops[k].s.push_back(s);
                if ((int)loops[k].s.size() >= kLoop) { loops[k].full = true; info("learned a %s animation loop from the leader", k ? "running" : "standing"); }
            }
        }
    }

    // The pose to show for the delayed copy of the leader: the newest sample whose turn is at most `delay` behind the newest one.
    const Sample* delayedSample() {
        if (!haveLead || histCount == 0) return 0;
        const int32_t delayTurns = (int32_t)(o.followDelayMs * 0.06);
        const Sample* best = 0;
        for (int i = 0; i < histCount; i++) {
            const Sample& s = hist[(size_t)((histHead - 1 - i + kHist * 2) % kHist)];   // newest first
            if (s.pose.sceneSerial != leadSerial) continue;
            if (MpPoseMath::turnDiff(lastLead.pose.turn, s.pose.turn) >= delayTurns) return &s;
            best = &s;   // (older and older: when nothing is old enough the oldest one stands in)
        }
        return best;
    }

    // ---- what to send each frame -------------------------------------------------------------------------------------------------------------
    struct Out {
        MpPose pose;
        uint8_t tail[MP_MAX_PAYLOAD];
        int tailLen = 0;
        bool ok = false;
    };

    void takeAnim(Out* out, const Sample& s) {   // the animation fields (and the track tail) of a sample
        out->pose.anim = s.pose.anim; out->pose.frame = s.pose.frame; out->pose.rate = s.pose.rate;
        memcpy(out->pose.weights, s.pose.weights, 4);
        out->pose.upperAnim = s.pose.upperAnim; out->pose.upperFrame = s.pose.upperFrame;
        out->pose.flags = s.pose.flags;
        out->pose.scale = s.pose.scale ? s.pose.scale : 64;
        out->tailLen = 0;
        if (s.len > MP_POSE_WIRE_BYTES && s.len - MP_POSE_WIRE_BYTES <= (int)sizeof out->tail && s.raw[MP_POSE_WIRE_BYTES] == MP_POSE_TAIL_TAG) {
            out->tailLen = s.len - MP_POSE_WIRE_BYTES;
            memcpy(out->tail, s.raw + MP_POSE_WIRE_BYTES, (size_t)out->tailLen);
        }
    }
    void defaultAnim(Out* out) {   // nothing learned yet: the base group (the game keeps the pose of whatever it shows)
        out->pose.anim = MpPoseMath::animHash("\x8a\xee\x96\x7b");
        out->pose.frame = (uint16_t)((turn * 8) % (60 * 16));
        out->pose.rate = 32;
        memset(out->pose.weights, 0, 4);
        out->pose.weights[0] = 255;
        out->pose.upperAnim = 0;
        out->pose.upperFrame = 0;
        out->pose.flags = MP_POSE_VISIBLE | MP_POSE_ON_GROUND;
        out->pose.scale = 64;
        out->tailLen = 0;
    }
    void loopAnim(Out* out, int k) {
        AnimLoop& L = loops[k];
        if ((int)L.s.size() >= 20) {
            if (L.play >= (int)L.s.size()) L.play = 0;
            takeAnim(out, L.s[(size_t)L.play++]);
        } else {
            defaultAnim(out);
        }
    }

    void basis(Q q, V3* right, V3* up, V3* fwd) { *right = QRot(q, v3(1, 0, 0)); *up = QRot(q, v3(0, 1, 0)); *fwd = QRot(q, v3(0, 0, 1)); }

    void makeFollow(Out* out) {
        const Sample* s = delayedSample();
        if (!s) return;
        out->pose = s->pose;
        takeAnim(out, *s);
        const Q q = QFromWire(s->pose.quat);
        const V3 off = QRot(q, v3((float)(o.followDist * 0.6), 0, (float)(-o.followDist * 0.8)));
        SetPos(&out->pose, add(PosOf(s->pose), off));
        out->ok = true;
    }

    bool ensureAnchor() {
        if (o.haveAnchor) {   // a place given by hand: used as it is (tests, no other player needed)
            if (!anchored) { anchorPos = o.anchor; anchorQ = Q{0, 0, 0, 1}; anchorSerial = leadSerial; anchorZone = leadZone; anchored = true; wanderA = 0; }
            return true;
        }
        if (haveLead && lastLead.pose.sceneSerial == leadSerial && (!anchored || anchorSerial != leadSerial || anchorZone != leadZone)) {
            anchorPos = PosOf(lastLead.pose);
            anchorQ = QFromWire(lastLead.pose.quat);
            anchorSerial = leadSerial;
            anchorZone = leadZone;
            anchored = true;
            wanderA = 0;
            info("anchored at (%.0f, %.0f, %.0f)", anchorPos.x, anchorPos.y, anchorPos.z);
        }
        return anchored;
    }

    void makeWander(Out* out) {
        if (!ensureAnchor()) return;
        V3 right, up, fwd;
        basis(anchorQ, &right, &up, &fwd);
        const float R = (float)o.wanderRadius;
        const float dA = (float)(o.wanderSpeed * 2.0 / (R > 1 ? R : 1));   // 2 turns per 30 Hz frame
        wanderA += dA;
        const float a = wanderA;
        const V3 pos = add(anchorPos, add(mul(right, R * cosf(a)), mul(fwd, R * sinf(a))));
        const V3 tang = add(mul(right, -sinf(a)), mul(fwd, cosf(a)));
        const Q q = QMul(anchorQ, QYaw(-a));
        out->pose = MpPose();
        memset(&out->pose, 0, sizeof out->pose);
        loopAnim(out, 1);
        if (!loops[1].full && (int)loops[1].s.size() < 20) { out->pose.flags = MP_POSE_VISIBLE | MP_POSE_ON_GROUND; }
        SetPos(&out->pose, pos);
        QToWire(q, out->pose.quat);
        const float v = (float)(o.wanderSpeed * 0.5);
        out->pose.vel[0] = (int8_t)lrintf(tang.x * v); out->pose.vel[1] = (int8_t)lrintf(tang.y * v); out->pose.vel[2] = (int8_t)lrintf(tang.z * v);
        out->ok = true;
    }

    void makeIdle(Out* out) {
        if (!ensureAnchor()) return;
        memset(&out->pose, 0, sizeof out->pose);
        loopAnim(out, 0);
        const V3 off = QRot(anchorQ, v3((float)(o.followDist * 0.6), 0, (float)(o.followDist * 0.8)));
        SetPos(&out->pose, add(anchorPos, off));
        // face the anchor (where the player stood): turn half a circle
        QToWire(QMul(anchorQ, QYaw(3.14159265f)), out->pose.quat);
        out->ok = true;
    }

    // The other player fell into a bubble: a teammate's body touching it pops it (the bubbled console checks puppet positions), so go
    // and stand where its pose is (the feet sit 80 units under the bubble's centre: the body is inside the touch range).
    void makeRescue(Out* out) {
        memset(&out->pose, 0, sizeof out->pose);
        loopAnim(out, 0);
        out->pose.pos[0] = lastLead.pose.pos[0]; out->pose.pos[1] = lastLead.pose.pos[1]; out->pose.pos[2] = lastLead.pose.pos[2];
        memcpy(out->pose.quat, lastLead.pose.quat, sizeof out->pose.quat);
        out->pose.flags = (uint16_t)((out->pose.flags | MP_POSE_VISIBLE | MP_POSE_ON_GROUND) & ~MP_POSE_BUBBLE);
        out->ok = true;
        if (!rescuing) { rescuing = true; say("Slot %d (%s) is in a bubble: going over to pop it.", leader, nameOf(leader)); }
    }
    bool rescuing = false;

    // Bubble: the Play Together "Bubbles" fall as a peer reports it (BUBBLE + the pose flag), floating beside the leader until his
    // body touches it or a pop request (BUMP kind 4) arrives. Same rules and constants as the game (MpBubbleLogic.hpp).
    void enterBubble(double t) {
        if (!haveLead || !Usable(lastLead.pose)) return;
        bubbled = true;
        bubbleAge = 0;
        bubbleV = v3(0, 0, 0);
        const Q q = QFromWire(lastLead.pose.quat);
        bubbleC = add(PosOf(lastLead.pose), mul(QRot(q, v3(0, 1, 0)), 400.0f));
        sendBubble(1, MP_BUBBLE_BY_NONE);
        bubbleCount++;
        say("Fell into a bubble. Touch it (or spin / shoot it) to pop it.");
        (void)t;
    }
    void sendBubble(uint8_t on, uint8_t by) {
        uint8_t b[8];
        MpWriter w(b, sizeof b);
        MpBubbleMsg m = {haveLead ? leadSerial : c->sceneSerial(), on, 2, by, 0};
        mp_write_bubble(w, m);
        c->send(MP_MSG_BUBBLE, b, (uint16_t)w.n, true);
    }
    MpBubbleLogic::Teammate leaderAsTeammate() {
        MpBubbleLogic::Teammate t;
        memset(&t, 0, sizeof t);
        t.valid = haveLead && Now() - leadSeenAt < 2.0 && Usable(lastLead.pose) && !(lastLead.pose.flags & MP_POSE_IN_DEMO);
        if (haveLead) {
            t.feet = PosOf(lastLead.pose);
            t.up = MpBubbleLogic::upOfQuat(lastLead.pose.quat);
            t.onGround = (lastLead.pose.flags & MP_POSE_ON_GROUND) != 0;
        }
        return t;
    }
    void popBubble(int by, double t) {
        bubbled = false;
        sendBubble(0, (uint8_t)by);
        say("Popped by slot %d. Back on my feet.", by);
        nextBubble = t + 25;
    }
    // one turn pair of bubble physics per 30 Hz frame; returns true when the pose is the bubble's
    void bubbleStep(Out* out, double t) {
        bubbleAge += 2;
        MpBubbleLogic::Teammate team[MP_MAX_PLAYERS];
        memset(team, 0, sizeof team);
        const int ls = leader >= 0 ? leader : 0;
        team[ls] = leaderAsTeammate();
        const MpBubbleLogic::Teammate& m = team[ls];
        const V3 up = m.valid ? m.up : v3(0, 1, 0);
        if (m.valid) {
            const V3 target = MpBubbleLogic::hoverPoint(m, bubbleC, v3(1, 0, 0));
            for (int k = 0; k < 2; k++) bubbleC = MpBubbleLogic::stepFloat(bubbleC, &bubbleV, target, up, (uint32_t)(bubbleAge + k));
            if (bubbleAge >= MpBubbleLogic::kPopGraceTurns) {
                const int by = MpBubbleLogic::touchPops(bubbleC, MpBubbleLogic::kRadius, team);
                if (by >= 0) { popBubble(by, t); return; }
            }
        }
        // the pose: feet below the bubble's centre, the leader's up and facing, a standing animation
        memset(&out->pose, 0, sizeof out->pose);
        loopAnim(out, 0);
        SetPos(&out->pose, sub(bubbleC, mul(up, MpBubbleLogic::kRiderDrop)));
        if (haveLead) memcpy(out->pose.quat, lastLead.pose.quat, sizeof out->pose.quat); else out->pose.quat[3] = 32767;
        out->pose.flags = (uint16_t)((out->pose.flags | MP_POSE_VISIBLE | MP_POSE_BUBBLE) & ~MP_POSE_ON_GROUND);
        out->ok = true;
    }
    void onPopRequest(int from, const uint8_t* d, size_t n, double t) {
        bool ok;
        uint8_t target, cause;
        if (!MpBubbleLogic::readPopRequest(d, n, &ok, &target, &cause) || !ok || !bubbled || target != (uint8_t)c->localSlot()) return;
        const MpBubbleLogic::Teammate m = leaderAsTeammate();
        float dist = -1;
        if (m.valid) dist = MpBubbleLogic::len(sub(add(m.feet, mul(m.up, MpBubbleLogic::kBodyUp)), bubbleC));
        if (MpBubbleLogic::acceptPop(cause, bubbleAge, MpBubbleLogic::kPopGraceTurns, m.valid, dist, MpBubbleLogic::kRadius)) popBubble(from, t);
        else info("pop request from slot %d refused (cause %d, age %d, dist %.0f)", from, cause, bubbleAge, dist);
    }

    // ---- frame ---------------------------------------------------------------------------------------------------------------------------
    void sendPose(Out& out) {
        out.pose.turn = turn;
        out.pose.sceneSerial = haveLead ? leadSerial : c->sceneSerial();
        out.pose.zone = haveLead ? leadZone : 0;
        out.pose.form = MP_FORM_NORMAL;
        out.pose.flags |= MP_POSE_VISIBLE;
        if (!bubbled) out.pose.flags &= (uint16_t)~MP_POSE_BUBBLE;
        out.pose.flags &= (uint16_t)~(MP_POSE_DEAD | MP_POSE_MIRROR);
        if (!out.pose.scale) out.pose.scale = 64;
        uint8_t buf[MP_MAX_PAYLOAD];
        MpWriter w(buf, sizeof buf);
        mp_write_pose(w, out.pose);
        if (out.tailLen > 0 && w.n + (size_t)out.tailLen <= sizeof buf) { memcpy(buf + w.n, out.tail, (size_t)out.tailLen); w.n += (size_t)out.tailLen; }
        if (c->send(MP_MSG_STATE, buf, (uint16_t)w.n, false)) { sent++; framesSent++; }
    }

    void periodic(double t, double tj) {
        // a phrase and an emote, in turns
        if (o.chatEvery > 0 && tj >= nextChat) {
            nextChat = tj + o.chatEvery;
            uint8_t b[16];
            MpWriter w(b, sizeof b);
            if (chatCount % 2 == 0) {
                MpChatMsg m;
                memset(&m, 0, sizeof m);
                m.kind = MP_CHAT_PHRASE;
                m.phrase = (uint8_t)((chatCount / 2 * 5 + 1) % MP_CHAT_PHRASES);
                mp_write_chat(w, m);
                if (c->send(MP_MSG_CHAT, b, (uint16_t)w.n, true)) info("sent phrase id=%d", m.phrase);
            } else {
                MpEmoteMsg e = {(uint8_t)((chatCount / 2) % MP_EMOTE_COUNT), 0};
                mp_write_emote(w, e);
                if (c->send(MP_MSG_EMOTE, b, (uint16_t)w.n, true)) info("sent emote id=%d", e.emote);
            }
            chatCount++;
        }
        // the Star Pointer: 15 Hz for 2.5 s on a spot in front of the other player, circling a little, then released
        if (o.pointerEvery > 0) {
            if (!pointerOn && tj >= nextPointer && tj >= 4) {
                pointerOn = true;
                pointerUntil = t + 2.5;
                nextPointer = tj + o.pointerEvery;
                nextPointerSend = 0;
                pointerCount++;
                info("pointer on #%d", pointerCount);
            }
            if (pointerOn) {
                if (t >= pointerUntil) {
                    pointerOn = false;
                    sendPointer(0, t);
                    info("pointer off");
                } else if (t >= nextPointerSend) {
                    nextPointerSend = t + 1.0 / 15.0;
                    sendPointer(1, t);
                }
            }
        }
    }
    void sendPointer(uint8_t on, double t) {
        const Q q = haveLead ? QFromWire(lastLead.pose.quat) : anchorQ;
        const float a = (float)(t * 2.0);
        const V3 p = add(haveLead ? PosOf(lastLead.pose) : anchorPos, QRot(q, v3(120.0f * cosf(a), 0, 300.0f + 120.0f * sinf(a))));
        MpPointerMsg m = {haveLead ? leadSerial : c->sceneSerial(), on, {p.x, p.y, p.z}};
        uint8_t b[16];
        MpWriter w(b, sizeof b);
        mp_write_pointer(w, m);
        c->send(MP_MSG_POINTER, b, (uint16_t)w.n, false);
    }

    void receive(double t) {
        MpMsg m;
        while (c->poll(&m)) {
            const int from = m.fromSlot;
            if (m.type == MP_MSG_STATE || m.type == MP_MSG_POINTER) {
                if (g_partnerImpair.lossPct > 0 && rnd01() * 100.0 < g_partnerImpair.lossPct) { droppedRx++; continue; }
            }
            if (m.type == MP_MSG_STATE && from < MP_MAX_PLAYERS) {
                MpReader r(m.data, m.len);
                MpPose p;
                if (!mp_read_pose(r, &p)) continue;
                rxState[from]++;
                rxWindow++;
                if (from == leader) onLeaderState(m, p, t);
            } else if (m.type == MP_MSG_CHAT) {
                MpReader r(m.data, m.len);
                MpChatMsg cm;
                if (mp_read_chat(r, &cm)) {
                    rxChat++;
                    if (cm.kind == MP_CHAT_PHRASE) say("Slot %d (%s) said phrase %d.", from, nameOf(from), cm.phrase);
                    else say("Slot %d (%s) wrote: %s", from, nameOf(from), cm.text);
                }
            } else if (m.type == MP_MSG_EMOTE) {
                MpReader r(m.data, m.len);
                MpEmoteMsg e;
                if (mp_read_emote(r, &e)) { rxEmote++; say("Slot %d (%s) sent emote %d.", from, nameOf(from), e.emote); }
            } else if (m.type == MP_MSG_POINTER) {
                MpReader r(m.data, m.len);
                MpPointerMsg pm;
                if (mp_read_pointer(r, &pm)) { if (!rxPointer++) say("Slot %d (%s) is using the Star Pointer.", from, nameOf(from)); }
            } else if (m.type == MP_MSG_BUBBLE) {
                MpReader r(m.data, m.len);
                MpBubbleMsg bm;
                if (mp_read_bubble(r, &bm)) { rxBubble++; say("Slot %d (%s) %s.", from, nameOf(from), bm.on ? "is in a bubble" : "left its bubble"); }
            } else if (m.type == MP_MSG_BUMP) {
                onPopRequest(from, m.data, m.len, t);
            } else if (m.type == MP_MSG_WHERE || m.type == MP_MSG_MOVE) {
                info("host sent %s (galaxy/scene change)", m.type == MP_MSG_WHERE ? "WHERE" : "MOVE");
                resetScene();
            } else if (m.type != MP_MSG_STATE) {
                info("rx type=0x%02X from=%d len=%d", m.type, from, m.len);
            }
        }
        MpCoreEvent ev;
        while (c->pollEvent(&ev)) info("event kind=%d slot=%d other=%d epoch=%d serial=%d arg=%u", ev.kind, ev.slot, ev.other, ev.epoch, ev.serial, ev.arg);
    }
    const char* nameOf(int slot) {
        static char b[MP_MAX_PLAYERS][MP_NAME_BYTES + 1];
        if (slot < 0 || slot >= MP_MAX_PLAYERS) return "?";
        c->slotName(slot, b[slot]);
        return b[slot];
    }

    void statusLine(double t) {
        MpUiStatus st;
        c->uiStatus(&st);
        char l[300];
        int n = 0;
        n += snprintf(l + n, sizeof l - (size_t)n, "%s | %s", roomLabel.empty() ? "no room yet" : roomLabel.c_str(), PhaseName(st.phase));
        if (c->active()) {
            int present = 0;
            for (int s = 0; s < MP_MAX_PLAYERS; s++) if (c->slotPresent(s)) present++;
            const int me = c->localSlot();
            n += snprintf(l + n, sizeof l - (size_t)n, " | %d players | me: %s slot %d", present, me >= 0 ? LookName(c->slotLook(me)) : "?", me);
            const int ps = leader >= 0 ? leader : c->hostSlot();
            const int ping = ps >= 0 && ps != me ? c->slotPing(ps) : -1;
            if (ping >= 0) n += snprintf(l + n, sizeof l - (size_t)n, " | ping %d ms", ping);
            else n += snprintf(l + n, sizeof l - (size_t)n, " | ping -");
            n += snprintf(l + n, sizeof l - (size_t)n, " | hear %.0f/s", rxRate);
            if (g_partnerImpair.any()) n += snprintf(l + n, sizeof l - (size_t)n, " | injecting loss %.0f%% delay %u ms", g_partnerImpair.lossPct, g_partnerImpair.delayMs);
            if (bubbled) n += snprintf(l + n, sizeof l - (size_t)n, " | IN A BUBBLE");
            else if (haveLead) n += snprintf(l + n, sizeof l - (size_t)n, " | %s %s", o.behaviour == B_FOLLOW ? "following" : (o.behaviour == B_WANDER ? "wandering near" : "waiting by"), nameOf(leader));
            else n += snprintf(l + n, sizeof l - (size_t)n, " | waiting for the other player to appear in a scene");
        }
        if (tty) {
            fprintf(stdout, "\r\x1b[K%s", l);
            fflush(stdout);
            statusShown = true;
        } else if (t - lastPeriodic >= 5.0) {
            lastPeriodic = t;
            fprintf(stdout, "%s\n", l);
            fflush(stdout);
        }
        if (t - lastPeriodic >= 5.0 || tty) {
            static double lastLog = 0;
            if (t - lastLog >= 5.0) { lastLog = t; info("status: %s | sent %u states, heard %u from leader", l, sent, rxLeadTotal); }
        }
    }

    // One 30 Hz frame; false when the program is done.
    bool frame() {
        const double t = Now();
        const double tt = t - t0;
        turn = (uint32_t)(tt * 60.0);
        c->tick(turn);
        MpUiStatus st;
        c->uiStatus(&st);
        if (st.phase != lastPhase) {
            info("phase %s error=%d players=%d", PhaseName(st.phase), st.error, st.players);
            lastPhase = st.phase;
        }
        if (c->active() && !everActive) {
            everActive = true;
            tJoin = t;
            nextChat = 8;
            nextPointer = 5;
            nextBubble = o.bubbleAfter;   // (from the moment the other player shows up: see below)
        }
        if (scanningNow) scanStep(tt);
        if (done) return false;
        // the session ended
        if (everActive && st.phase == MP_UI_IDLE && !c->active()) {
            endError = st.error;
            if (leaving || g_stop) return false;
            if (st.error == MP_ERR_DIFFERENT_VERSION) { explainMismatch(false); exitCode = 3; }
            else say("The session ended%s.", st.error == MP_ERR_HOST_LEFT ? " (the host left)" : (st.error != MP_ERR_NONE ? " with a problem" : ""));
            restartAfterEnd(tt);
            if (done) return false;
        } else if (joinPending && !everActive && st.phase == MP_UI_IDLE && st.error != MP_ERR_NONE) {
            // joining failed
            joinPending = false;
            if (st.error == MP_ERR_DIFFERENT_VERSION) { explainMismatch(false); exitCode = 3; }
            else if (st.error == MP_ERR_ROOM_FULL) say("The room is full.");
            else if (st.error == MP_ERR_NAME_TAKEN) say("The name \"%s\" is taken in that room; use --name.", o.name);
            else say("Could not join (error %d).", st.error);
            if (o.once && exitCode == 0) exitCode = 1;
            if (o.once) return false;
            seenRooms.clear();
            scanningNow = true;
        } else if (o.goal == G_JOIN_ADDR && !everActive && st.phase == MP_UI_IDLE && st.error == MP_ERR_TIMEOUT && !leaving &&
                   Now() - joinStart < 2.0 && instantRetries < 8) {
            // The core can end a join with "no answer" at once (its connect timer is an unsigned difference of two clock readings taken a
            // moment apart on two threads): not an answer from the network. Asking again is all that is needed.
            instantRetries++;
            info("join ended at once with a timeout (core clock race): asking again (%d)", instantRetries);
            startGoal(tt);
        } else if (o.goal != G_JOIN_LAN && !everActive && st.phase == MP_UI_IDLE && st.error != MP_ERR_NONE && !leaving) {
            if (st.error == MP_ERR_DIFFERENT_VERSION) { explainMismatch(false); exitCode = 3; }
            else if (st.error == MP_ERR_TIMEOUT || st.error == MP_ERR_ROOM_NOT_FOUND)
                say("No answer from %s. Check the address and that the Vita's room is open (and that both are on the same network). If they are: %s.",
                    o.address.c_str(), versionSentence().c_str());
            else say("Could not %s (error %d).", o.goal == G_HOST ? "open the room" : "join", st.error);
            if (exitCode == 0) exitCode = 1;
            return false;
        }
        // joined
        if (c->active() && !joinedLogged && st.phase >= MP_UI_LOBBY) {
            joinedLogged = true;
            const int me = c->localSlot();
            const int got = me >= 0 ? (int)c->slotLook(me) : -1;
            say("In the room as slot %d, character %s%s.", me, LookName(got), got == o.look ? "" : " (the one I wished for was taken)");
            info("joined slot=%d look=%d wished=%d host=%d", me, got, o.look, c->hostSlot());
        }
        int present = 0;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) if (c->slotPresent(s)) present++;
        if (o.goal == G_HOST && !started && st.phase == MP_UI_LOBBY && present >= (o.startWhen > 0 ? o.startWhen : 1)) {
            started = c->uiStart();
            if (started) say("Started the session with %d players.", present);
        }
        if (c->active()) {
            leader = pickLeader();
            if (leader != lastLeader) {
                if (leader >= 0) say("Following slot %d (%s).", leader, nameOf(leader)); else if (lastLeader >= 0) say("The player I was following left.");
                lastLeader = leader;
                histHead = histCount = 0;
                haveLead = false;
                anchored = false;
            }
            if (hist.empty()) hist.resize(kHist);
            // a game's LOADED for every scene serial: the host's scene barrier never waits for us
            if (c->sceneSerial() != loadedSerial) {
                loadedSerial = c->sceneSerial();
                uint8_t b[2];
                MpWriter w(b, sizeof b);
                w.u16(loadedSerial);
                c->send(MP_MSG_LOADED, b, (uint16_t)w.n, true);
                info("loaded serial=%u", loadedSerial);
            }
            if (c->snapshotReady()) c->snapshotConsumed();
            receive(t);
            // the poses of this frame
            const double tj = t - tJoin;
            if (haveLead && t - leadSeenAt > 3.0 && !bubbled) { /* the leader went quiet: our last pose goes on being sent */ }
            Out out;
            memset(&out.pose, 0, sizeof out.pose);
            if (o.bubble && !bubbled && haveLead && nextBubble > 0 && (t - firstLeadAt) >= nextBubble) { enterBubble(t); nextBubble = 0; }
            if (bubbled) bubbleStep(&out, t);
            const bool leadBubbled = haveLead && (lastLead.pose.flags & MP_POSE_BUBBLE) && t - leadSeenAt < 2.0;
            if (!out.ok && o.rescue && leadBubbled) makeRescue(&out);
            if (!leadBubbled && rescuing) { rescuing = false; info("rescue over"); }
            if (!out.ok) {
                if (o.behaviour == B_FOLLOW) makeFollow(&out);
                else if (o.behaviour == B_WANDER) makeWander(&out);
                else makeIdle(&out);
            }
            if (out.ok) sendPose(out);
            if (haveLead || o.haveAnchor) periodic(t, tj);
            // test levers
            if (o.dropAfter > 0 && tj >= o.dropAfter) {
                say("Dropping off the network now without saying goodbye (--drop-after).");
                info("drop: no BYE");
                fflush(stdout);
                _exit(0);
            }
            if (!leaving && ((o.leaveAfter > 0 && tj >= o.leaveAfter) || (o.duration > 0 && tj >= o.duration) || g_stop)) {
                say("Leaving%s.", g_stop ? " (Ctrl-C)" : "");
                c->uiLeave();
                leaving = true;
                leaveAt = t;
            }
        } else {
            // not in a session (scanning, joining): still answer Ctrl-C
            if (g_stop && !leaving) { c->uiLeave(); leaving = true; leaveAt = t; }
            if (g_stop && st.phase == MP_UI_IDLE) return false;
        }
        if (leaving && (st.phase == MP_UI_IDLE || t - leaveAt > 3.0)) return false;
        if (t - lastRxMark >= 1.0) { rxRate = rxWindow / (t - lastRxMark); rxWindow = 0; lastRxMark = t; }
        if (!o.quiet) statusLine(tt);
        return true;
    }
};

}  // namespace

static bool ParseXyz(const char* s, V3* out) {
    float x, y, z;
    if (sscanf(s, "%f,%f,%f", &x, &y, &z) != 3) return false;
    *out = v3(x, y, z);
    return true;
}

static void LogSink(const char* line);
static Partner* g_partner = 0;
static void LogSink(const char* line) {
    if (!g_partner) return;
    g_partner->info("%s", line);
    // what a room of another version told us (mp_lan.cpp logs it when the beacon is not ours)
    char host[40];
    int proto = 0;
    unsigned fp = 0;
    if (sscanf(line, "[mp] scan: room incompatible host=%39s proto=%d fingerprint=%x", host, &proto, &fp) == 3) {
        g_partner->haveIncompat = true;
        g_partner->incompatProto = proto;
        g_partner->incompatFp = fp;
    }
}

static bool ReadDataHashFile(uint32_t* out, const char* argv0) {
    std::string dir = argv0 ? argv0 : "";
    const size_t slash = dir.rfind('/');
    dir = slash == std::string::npos ? "." : dir.substr(0, slash);
    for (const std::string& p : {std::string("data-hash.txt"), dir + "/data-hash.txt"}) {
        if (FILE* f = fopen(p.c_str(), "r")) {
            char b[64] = {0};
            const bool ok = fgets(b, sizeof b, f) != 0;
            fclose(f);
            if (ok) {
                char* end = 0;
                const unsigned long v = strtoul(b, &end, 16);
                if (end != b) { *out = (uint32_t)v; return true; }
            }
        }
    }
    return false;
}

int main(int argc, char** argv) {
    // 25 Nov (console): the Vita's Same Wi-Fi link binds UDP 24100 (net/src/vlink_vita.cpp, mp75: it cannot bind 41100), so a partner on the
    // desktop default 41100 never hears the Vita's room and the Vita never hears the partner (night63/65/67mp: 0 received both ways).
    // The partner speaks on the Vita's port unless SMG_MP_UDP_PORT says otherwise.
    setenv("SMG_MP_UDP_PORT", "24100", 0);   // (the transport reads SMG_MP_UDP_PORT, platform/src/mp/mp_transport_vlink.cpp)
    Opts o;
    bool printHash = false;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "partner: %s needs a value\n", what); exit(1); }
            return argv[++i];
        };
        auto isValue = [&]() { return i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0; };
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { Usage(stdout); return 0; }
        else if (!strcmp(a, "--version")) {
            printf("partner: protocol %d, build id \"%s\", source %s\n", MP_PROTO_VERSION, MP_BUILD_ID, PARTNER_SOURCE_REV);
            return 0;
        }
        else if (!strcmp(a, "--join-lan")) o.goal = G_JOIN_LAN;
        else if (!strcmp(a, "--join")) { o.goal = G_JOIN_ADDR; o.address = next(a); }
        else if (!strcmp(a, "--host")) o.goal = G_HOST;
        else if (!strcmp(a, "--room")) o.room = next(a);
        else if (!strcmp(a, "--once")) o.once = true;
        else if (!strcmp(a, "--name")) o.name = next(a);
        else if (!strcmp(a, "--look")) {
            o.look = ParseLook(next(a));
            if (o.look < 0) { fprintf(stderr, "partner: unknown character (use mario luigi toad cosmic yellowtoad cosmicluigi or 0..5)\n"); return 1; }
        }
        else if (!strcmp(a, "--mode")) o.mode = !strcmp(next(a), "race") ? MP_MODE_RACE : MP_MODE_TOGETHER;
        else if (!strcmp(a, "--max")) o.maxPlayers = atoi(next(a));
        else if (!strcmp(a, "--start-when")) o.startWhen = atoi(next(a));
        else if (!strcmp(a, "--follow")) { o.behaviour = B_FOLLOW; if (isValue()) o.followSlot = atoi(argv[++i]); }
        else if (!strcmp(a, "--follow-delay")) o.followDelayMs = atof(next(a));
        else if (!strcmp(a, "--follow-dist")) o.followDist = atof(next(a));
        else if (!strcmp(a, "--wander")) o.behaviour = B_WANDER;
        else if (!strcmp(a, "--idle")) o.behaviour = B_IDLE;
        else if (!strcmp(a, "--radius")) o.wanderRadius = atof(next(a));
        else if (!strcmp(a, "--speed")) o.wanderSpeed = atof(next(a));
        else if (!strcmp(a, "--anchor")) { if (!ParseXyz(next(a), &o.anchor)) { fprintf(stderr, "partner: --anchor X,Y,Z\n"); return 1; } o.haveAnchor = true; }
        else if (!strcmp(a, "--chat-every")) o.chatEvery = atof(next(a));
        else if (!strcmp(a, "--pointer-every")) o.pointerEvery = atof(next(a));
        else if (!strcmp(a, "--no-rescue")) o.rescue = false;
        else if (!strcmp(a, "--bubble-me")) { o.bubble = true; if (isValue()) o.bubbleAfter = atof(argv[++i]); }
        else if (!strcmp(a, "--drop-after")) o.dropAfter = atof(next(a));
        else if (!strcmp(a, "--leave-after")) o.leaveAfter = atof(next(a));
        else if (!strcmp(a, "--duration")) o.duration = atof(next(a));
        else if (!strcmp(a, "--loss")) g_partnerImpair.lossPct = atof(next(a));
        else if (!strcmp(a, "--dup")) g_partnerImpair.dupPct = atof(next(a));
        else if (!strcmp(a, "--delay")) g_partnerImpair.delayMs = (unsigned)atoi(next(a));
        else if (!strcmp(a, "--jitter")) g_partnerImpair.jitterMs = (unsigned)atoi(next(a));
        else if (!strcmp(a, "--data-hash")) { o.dataHash = (uint32_t)strtoul(next(a), 0, 16); o.haveDataHash = true; }
        else if (!strcmp(a, "--data-root")) o.dataRoot = next(a);
        else if (!strcmp(a, "--print-data-hash")) printHash = true;
        else if (!strcmp(a, "--scan-for")) o.scanFor = atof(next(a));
        else if (!strcmp(a, "--udp-port")) setenv("SMG_MP_UDP_PORT", next(a), 1);
        else if (!strcmp(a, "--bcast")) setenv("SMG_MP_UDP_BCAST", next(a), 1);
        else if (!strcmp(a, "--log")) o.logFile = next(a);
        else if (!strcmp(a, "--quiet")) o.quiet = true;
        else { fprintf(stderr, "partner: unknown option %s (try --help)\n", a); return 1; }
    }
    if (o.maxPlayers < 2 || o.maxPlayers > MP_MAX_PLAYERS) { fprintf(stderr, "partner: --max is 2..4\n"); return 1; }
    if (g_partnerImpair.lossPct < 0) g_partnerImpair.lossPct = 0;
    if (g_partnerImpair.lossPct > 90) g_partnerImpair.lossPct = 90;
    // the same numbers for the Same Wi-Fi link (net/src/vlink_udp.cpp reads them when it makes its socket)
    if (g_partnerImpair.any()) {
        char b[32];
        snprintf(b, sizeof b, "%g", g_partnerImpair.lossPct); setenv("SMG_NET_DROP", b, 1);
        snprintf(b, sizeof b, "%g", g_partnerImpair.dupPct); setenv("SMG_NET_DUP", b, 1);
        snprintf(b, sizeof b, "%u", g_partnerImpair.delayMs); setenv("SMG_NET_DELAY_MS", b, 1);
        snprintf(b, sizeof b, "%u", g_partnerImpair.jitterMs); setenv("SMG_NET_JITTER_MS", b, 1);
    }
    if (printHash) {   // for make_source_bundle.sh: the data hash of a prepared game folder, nothing else
        int files = 0;
        const uint32_t h = o.dataRoot ? StageDataHash(o.dataRoot, &files) : 0;
        if (!h) { fprintf(stderr, "partner: --print-data-hash needs --data-root DIR with DIR/sys/fst.bin\n"); return 1; }
        printf("%08x\n", (unsigned)h);
        return 0;
    }
    Partner p;
    p.o = o;
    g_partner = &p;
    p.logf = fopen(o.logFile, "a");
    if (p.logf) {   // the session layer and the link write their lines to stderr: into the log, not over the status line
        fflush(stderr);
        if (!freopen(o.logFile, "a", stderr)) { /* keep stderr */ }
    }
    mp_log_set_sink(LogSink, true);
    // the data hash: --data-hash, else --data-root, else data-hash.txt next to the program
    uint32_t h = 0;
    int files = 0;
    if (o.haveDataHash) h = o.dataHash;
    else if (o.dataRoot) { h = StageDataHash(o.dataRoot, &files); if (!h) { fprintf(stderr, "partner: could not read %s/sys/fst.bin\n", o.dataRoot); return 1; } }
    else if (ReadDataHashFile(&h, argv[0])) { /* ok */ }
    p.dataHash = h;
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
    signal(SIGPIPE, SIG_IGN);
    p.begin();
    p.startGoal(0);
    double next = Now();
    while (!p.done) {
        if (!p.frame()) break;
        next += 1.0 / 30.0;
        const double wait = next - Now();
        if (wait > 0) usleep((useconds_t)(wait * 1e6));
        else next = Now();
    }
    p.clearStatus();
    p.info("totals: sent %u states, heard %u from the leader, %u chat, %u emote, %u pointer, %u bubble messages, %u poses dropped by --loss", p.sent,
           p.rxLeadTotal, p.rxChat, p.rxEmote, p.rxPointer, p.rxBubble, p.droppedRx);
    p.info("exit %d", p.exitCode);
    mp_core_shutdown();
    if (p.exitCode == 0 && !p.everActive && !g_stop && p.o.goal != G_JOIN_LAN) p.exitCode = 1;
    return p.exitCode;
}
