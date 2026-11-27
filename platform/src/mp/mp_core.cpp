// Galaxy 3.0 multiplayer (port side, owner: core): MpCore, the session layer (DESIGN.md sections 3 and 8). See mp_core.h.
// integ: port-side storage must never come from the game's JKR heaps (operator new on a game thread uses the current scene heap,
// which is destroyed at the next scene change while the session's objects live on): every entry point the game calls is a host
// allocation scope (port/heap_routing.h; referenced weakly so the host tests link without the port).
extern "C" void port_host_alloc_scope_enter(void) __attribute__((weak));
extern "C" void port_host_alloc_scope_leave(void) __attribute__((weak));
namespace {
struct MpHostAllocScope {
    MpHostAllocScope() { if (port_host_alloc_scope_enter) port_host_alloc_scope_enter(); }
    ~MpHostAllocScope() { if (port_host_alloc_scope_leave) port_host_alloc_scope_leave(); }
};
}  // namespace
#define MP_HOST_ALLOC MpHostAllocScope mpHostAllocScope_
#if defined(TARGET_PC) && !defined(__vita__)
#include <unistd.h>
#endif
#include "mp_core.h"
#include "mp_seal.h"   // core2: session crypto (Monocypher, internal linkage)
#include "port/mp_prof.h"
#include "port/mp_direct_code.h"   // hooks: SMG_MP_AUTO=join_code
#include "port/mp_gather.h"
#include "port/mp_gate.h"   // MP3 lodgate: SessionHeld
#include "port/mp_nettune.h"   // netlog: the timing knobs and the statistics interval
#include "port/mp_test_env.h"
#include "port/mp_demo.h"   // MP3 demo: the mock roster behind the bridge queries (off unless SMG_MP_DEMO)

#include "mp_transport_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port/mp_test_env.h"   // MP_TEST_GETENV: desktop-only test hooks (SMG_MP_TEST_RECONNECT_MS)

// The rejoin window after a silent link (MP_LINKLOST_RECONNECT_MS). Desktop tests only: SMG_MP_TEST_RECONNECT_MS=<ms> (1000..75000) shortens it so a scenario can
// watch a reconnect give up inside one cutscene; never equal to MP_RECONNECT_MS (the code tells the two windows apart by that value). Not in Vita objects (MP_TEST_GETENV).
uint32_t mp_linklost_window_from(const char* e) {   // (pure: tests/mp/core_tests.cpp checks it)
    if (e && *e) {
        const long v = atol(e);
        if (v >= 1000 && v <= (long)MP_LINKLOST_RECONNECT_MS) return (uint32_t)(v == (long)MP_RECONNECT_MS ? v + 1 : v);
    }
    return MP_LINKLOST_RECONNECT_MS;
}
uint32_t mp_linklost_window_ms() { return mp_linklost_window_from(MP_TEST_GETENV("SMG_MP_TEST_RECONNECT_MS")); }

// netfix B8a: the Vita's C library mishandles 64-bit length modifiers (%llu / %lld): 64-bit counters are logged as %u through this clamp
#ifndef NETFIX_U32C
#define NETFIX_U32C(v) ((unsigned)((uint64_t)(v) > 0xFFFFFFFFull ? 0xFFFFFFFFull : (uint64_t)(v)))
#endif

// The counter base of a membership: its generation's order byte in bits 40..47 (2^40 datagrams per membership; the order byte wraps after
// 255 rejoins of one console in one session, and then that console's next datagrams to a member that kept the old window are dropped
// until it rejoins once more)
static bool WellFormedPlayerCode(const char* c) {   // "XXXX-XXXX" of the service's code alphabet (no I L O 0 1), NUL at [9]
    static const char kAlpha[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    for (int i = 0; i < 9; i++) {
        if (i == 4) { if (c[i] != '-') return false; continue; }
        if (!c[i] || !strchr(kAlpha, c[i])) return false;
    }
    return c[9] == 0;
}
static uint64_t CtrBase(MpGen g) { return (uint64_t)(g & 0xFF) << 40; }
// the host's "you were removed" datagrams (handleSealed): above every membership's counters (the receiver's window has seen those)
static const uint64_t kDepartCtrBase = (0xFFull << 40) + (1ull << 39);

// ---------------------------------------------------------------------------------------------------------------------------
// big buffers: allocated when a session (or a scan) starts, freed when it ends: nothing is allocated in single-player
// integ: mp_netglue.cpp (a strong reference: it pulls the glue, and with it the strong np_glue.h functions, out of the Vita
// runtime archive); mp_service.cpp's build tag (weak: the core's host tests link without the service).
extern "C" void mp_netglue_frame(void);
extern "C" void mp_service_set_build(uint32_t fingerprint) __attribute__((weak));
#include "port/mp_diag.h"   // beta diagnostics: the session summary (weak functions)
// (tsan) written by whichever thread pumps (under that core's lock; mp_bot --loopback has several cores), read by the game thread's
// tick() without a lock: relaxed atomics (64-bit: ldrexd/strexd on the Vita's Cortex-A9).
static uint64_t g_netFrameAt = 0, g_netTailUntil = 0;

namespace {
// MP3 netpack (memory): a queued message keeps its payload inline up to MP_QINLINE bytes (every game message but SWITCHMAP, 568 bytes,
// fits); a longer one takes a block of the session's small pool of big blocks. (It used to be a whole MpMsg, 1032 bytes, per entry.)
struct QEntry {
    bool reliable;
    bool tracked;   // netcore: a tracked game event (part of an origin's exactly-once stream): a roster change never purges it
    uint8_t type, fromSlot;
    int8_t big;   // index into MpCoreMem::bigData, or -1 (inline)
    uint16_t len;
    uint8_t data[MP_QINLINE];
};
// netcore: the largest tracked event is a typed CHAT (2 + 120 bytes); every other relayed type is <= 24 bytes (GameMaxLen). (It was
// 72: a long chat line was never in the history, so a console that missed it never got it after a host migration.)
#define MP_HIST_DATA 124
static_assert(MP_CHAT_MAX_PAYLOAD <= MP_HIST_DATA, "a tracked event must fit a history entry");
struct HistEntry {
    uint8_t origin, type;
    MpGen gen;   // netcore: the origin's membership generation (with seq: the event's identity)
    uint16_t seq, len;
    bool suppressed;   // restart-gate dropped: kept for historyHas() contiguity, skipped by sendCatchUp()
    uint8_t data[MP_HIST_DATA];
};
// netcore (new host): a dead host's event re-sent by one follower before the earlier ones another follower re-sends (each follower
// re-sends from where the new host is, but a late joiner's history starts later): it waits here for them (in order, or never)
#define MP_PARK_ENTRIES 32
struct Parked {
    bool used;
    uint8_t from, origin, type;
    MpGen gen;
    uint16_t seq, len;
    uint8_t data[MP_HIST_DATA];
};
}  // namespace

struct MpCoreMem {
    // MP3 netpack (memory): a console is never its own peer: at most MP_MAX_PLAYERS - 1 windows (MpCore::relFor).
    // netopt: a guest has one peer (the host): window 0 lives in this block, the other MP_MAX_PLAYERS - 2 (99 KB each) are separate blocks
    // that only a host, a console that is about to become one (the successor in line) or a console that just became one holds.
    MpReliable rel0;
    MpReliable* relX[MP_MAX_PLAYERS - 2];
    uint8_t bigData[MP_QBIG][MP_QBIG_BYTES];
    bool bigUsed[MP_QBIG];
    QEntry q[MP_QUEUE_SIZE];
    uint16_t qRing[MP_QUEUE_SIZE];   // queue order: indices into q
    int qHead, qCount;
    uint16_t qFree[MP_QUEUE_SIZE];
    int qFreeCount;
    HistEntry hist[MP_HISTORY_ENTRIES];
    int hHead, hCount;
    Parked park[MP_PARK_ENTRIES];
    MpSnapshotImage image;
    MpSnapshotRx rx;
};

namespace {

inline int16_t SeqDiff(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b); }

const char* ErrName(MpUiError e) {
    switch (e) {
    case MP_ERR_NONE: return "none";
    case MP_ERR_NO_WIFI: return "no-wifi";
    case MP_ERR_SERVICE_DOWN: return "service-down";
    case MP_ERR_UPDATE_REQUIRED: return "update-required";
    case MP_ERR_ROOM_NOT_FOUND: return "room-not-found";
    case MP_ERR_ROOM_FULL: return "room-full";
    case MP_ERR_DIFFERENT_VERSION: return "different-version";
    case MP_ERR_TIMEOUT: return "timeout";
    case MP_ERR_HOST_LEFT: return "host-left";
    case MP_ERR_KICKED: return "kicked";
    case MP_ERR_NAT: return "nat";
    case MP_ERR_NAME_TAKEN: return "name-taken";
    case MP_ERR_BAD_NAME: return "bad-name";
    case MP_ERR_RATE_LIMITED: return "rate-limited";
    default: return "internal";
    }
}

const char* ConnName(MpConn c) {
    switch (c) {
    case MP_CONN_ONLINE: return "online";
    case MP_CONN_WIFI: return "wifi";
    case MP_CONN_NEARBY: return "nearby";
    case MP_CONN_LOOPBACK: return "loopback";
    case MP_CONN_DIRECT: return "direct";
    default: return "none";
    }
}

const char* ByeName(uint8_t r) {
    switch (r) {
    case MP_BYE_LEAVE: return "leave";
    case MP_BYE_HOST_CLOSED: return "host-closed";
    case MP_BYE_TIMEOUT: return "timeout";
    case MP_BYE_SUSPEND: return "suspend";
    case MP_BYE_ERROR: return "error";
    case MP_BYE_HOST_LEAVING: return "host-leaving";
    case MP_BYE_RESYNC: return "resync";
    default: return "?";
    }
}

// Largest payload per game message type (0 = unknown type: dropped). Every message has a fixed maximum (DESIGN.md section 3).
uint16_t GameMaxLen(uint8_t t) {
    switch (t) {
    case MP_MSG_STATE: return 68;   // 51-byte MpPose + optional 17-byte tail (puppet)
    case MP_MSG_LOOK: return 8;
    case MP_MSG_EMOTE: return 8;      // 2 bytes (mp_read_emote)
    case MP_MSG_BUMP: return 16;
    case MP_MSG_POINTER: return 16;   // 15 bytes (mp_read_pointer); unreliable
    case MP_MSG_WHERE: return 16;
    case MP_MSG_MOVE: return 16;
    case MP_MSG_LOADED: return 8;
    case MP_MSG_GO: return 16;
    case MP_MSG_RACEREADY: return 8;  // racefix: serial u16
    case MP_MSG_STAR: return 24;      // 12 bytes (starfix: + galaxy hash, scenario)
    case MP_MSG_STARV: return 16;     // 11 bytes (starfix, host only)
    case MP_MSG_FINISH: return 16;
    case MP_MSG_RESULTS: return 64;
    case MP_MSG_SKIPVOTE: return 16;
    case MP_MSG_DEAD: return 8;
    case MP_MSG_WARPREQ: return 8;
    case MP_MSG_HOLD: return 16;
    case MP_MSG_BUBBLE: return 8;     // 6 bytes (mp_read_bubble)
    case MP_MSG_RESTART: return 8;    // serial u16
    case MP_MSG_TALLY: return 16;
    case MP_MSG_SWITCH: return 16;
    case MP_MSG_SWITCHMAP: return 568;   // header 7 + 33 zone slots x 17 (MpWorldLogic kMapHeader + kMapMaxZones * kMapEntry): the largest legal full map (was 400: a map with bits in more than 23 zones was never sent)
    case MP_MSG_KILL: return 16;
    case MP_MSG_PURPLE: return 16;
    case MP_MSG_CHIP: return 16;
    case MP_MSG_REGCRC: return 16;
    case MP_MSG_OBJMAP: return 138;   // worldsync: header 10 + a 128-byte chunk (MpWorldLogic kObjMapHeader + kObjMapChunkBytes); the summary is 17
    case MP_MSG_TOAST: return 16;
    case MP_MSG_STAMP: return 8;
    case MP_MSG_CHAT: return MP_CHAT_MAX_PAYLOAD;   // 2 (phrase) or 2 + 1..120 bytes of UTF-8 (port/mp_chat.h; chat)
    default: return 0;
    }
}

bool IsGame(uint8_t t) { return t >= MP_MSG_GAME_FIRST && t <= MP_MSG_GAME_LAST; }

uint64_t SysNow(void*) { return mp_sys_now_ms(); }

// MP3 netpack: a log line a stranger's datagrams can trigger is written at most a few times per second (per call site).
// (netcore: the bucket is a static per call site, shared by every MpCore of the process (tests and mp_bot run several, each under its
// own lock): relaxed atomics, so it is not a data race; an approximate count is fine for a log limit)
struct LogBucket { uint64_t at; int tokens; };
bool LogAllow(LogBucket& b, int burst) {
    const uint64_t t = mp_sys_now_ms();
    const uint64_t at = __atomic_load_n(&b.at, __ATOMIC_RELAXED);
    int tokens = __atomic_load_n(&b.tokens, __ATOMIC_RELAXED);
    if (tokens < burst && t - at >= 500) {
        const uint64_t add = (t - at) / 500;
        tokens = (int)(tokens + add > (uint64_t)burst ? (uint64_t)burst : tokens + add);
        __atomic_store_n(&b.at, t, __ATOMIC_RELAXED);
    }
    if (tokens <= 0) {
        __atomic_store_n(&b.tokens, tokens, __ATOMIC_RELAXED);
        return false;
    }
    __atomic_store_n(&b.tokens, tokens - 1, __ATOMIC_RELAXED);
    return true;
}
#define MP_LOG_LIMITED_N(burst, ...) do { static LogBucket lb_ = {0, burst}; if (LogAllow(lb_, burst)) mp_log(__VA_ARGS__); } while (0)
#define MP_LOG_LIMITED(...) MP_LOG_LIMITED_N(MP_LOG_BURST, __VA_ARGS__)

// (coordinator) elapsed time that never wraps: a stamp can be a little later than the pump's own clock read
static inline uint64_t Since(uint64_t t, uint64_t at) { return t > at ? t - at : 0; }

// MP3 netfeel: per remote slot, what came in and went out on the link (the game thread's "net link" line, MpPuppets.cpp, prints the rates
// with the pose buffer's numbers). Written under the core lock (receive and flush), read by the game thread: relaxed atomics, cumulative.
static uint32_t g_linkDgIn[MP_MAX_PLAYERS], g_linkBytesIn[MP_MAX_PLAYERS], g_linkDgOut[MP_MAX_PLAYERS];
extern "C" int mp_core_link_counts(int slot, uint32_t* dgIn, uint32_t* bytesIn, uint32_t* dgOut) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS) return 0;
    *dgIn = __atomic_load_n(&g_linkDgIn[slot], __ATOMIC_RELAXED);
    *bytesIn = __atomic_load_n(&g_linkBytesIn[slot], __ATOMIC_RELAXED);
    *dgOut = __atomic_load_n(&g_linkDgOut[slot], __ATOMIC_RELAXED);
    return 1;
}

struct EmitCtx {
    MpCore* core;
    int slot;
};

void PutName(MpWriter& w, const char* name) {
    char b[MP_NAME_BYTES];
    memset(b, 0, sizeof b);
    memcpy(b, name, strnlen(name, MP_NAME_BYTES));
    w.bytes(b, MP_NAME_BYTES);
}

}  // namespace

// (perf) The session lock: every unlock publishes the game thread's view (MpCoreView) first.
class CoreLock {
public:
    explicit CoreLock(MpCore& c) : m_c(c) { m_c.m_mutex.lock(); }
    ~CoreLock() {
        if (m_c.m_obPending) m_c.flushOutDue(m_c.now());   // MP3 netpack: what this locked operation queued goes out (poses may wait a moment)
        m_c.publishView();
        m_c.m_mutex.unlock();
    }
private:
    CoreLock(const CoreLock&);
    CoreLock& operator=(const CoreLock&);
    MpCore& m_c;
};
// The same for a lock already taken (tryLock).
class CoreLockAdopt {
public:
    explicit CoreLockAdopt(MpCore& c) : m_c(c) {}
    ~CoreLockAdopt() {
        if (m_c.m_obPending) m_c.flushOutDue(m_c.now());   // MP3 netpack
        m_c.publishView();
        m_c.m_mutex.unlock();
    }
private:
    CoreLockAdopt(const CoreLockAdopt&);
    CoreLockAdopt& operator=(const CoreLockAdopt&);
    MpCore& m_c;
};

// Access for static callbacks
struct MpCoreAccess {
    static bool emit(void* ctx, const MpRelMsg& m) {
        EmitCtx* e = (EmitCtx*)ctx;
        return e->core->sendToPeer(e->slot, m.type, m.from, MP_HDR_RELIABLE, m.seq, m.data, m.len);
    }
};

// ---------------------------------------------------------------------------------------------------------------------------
// mp3c24: raw RTT samples per peer (heartbeat echo; reliable-channel ack time), min / mean / max over 10 s. Log only.
template <class P> static inline void rttNote(P& p, uint32_t v) {
    if (!p.rttN || v < p.rttMin) p.rttMin = v;
    if (v > p.rttMax) p.rttMax = v;
    ++p.rttN;
    p.rttSum += v;
}

MpCore::MpCore(const MpCoreOptions& o)
    : m_opt(o), m_mem(0), m_conn(0), m_scanConn(0), m_scanKind(MP_CONN_NONE), m_connKind(MP_CONN_NONE), m_scanLastCall(0),
      m_scanFailed(false), m_hub(0), m_hubEndpoint(0), m_dataHash(0), m_fingerprint(0), m_rng(o.seed), m_role(ROLE_NONE),
      m_phase(MP_UI_IDLE), m_error(MP_ERR_NONE), m_phaseAt(0), m_active(false), m_started(false), m_originalHost(false),
      m_visibility(0), m_sessionId(0), m_wantSession(0), m_localSlot(-1), m_hostSlot(-1), m_epoch(0), m_serial(0),
      m_wantMode(0), m_hostLeaving(false), m_hostGoneAt(0), m_claiming(false), m_claimAt(0), m_lastOldHost(0),
      m_rosterDirty(false), m_lastRosterSent(0), m_myOrigin(0), m_uSeq(0), m_kickedCount(0), m_kickedNext(0), m_haveWhere(false),
      m_raceFinished(false), m_raceTurns(0), m_lastBeacon(0), m_lastRecvPump(0), m_rateGraceUntil(0), m_beaconSeq(0), m_look(0), m_snapReady(false),
      m_snapProgress(0), m_evHead(0), m_evCount(0), m_rejoinValid(false), m_rejoinAt(0), m_rejoinConn(MP_CONN_NONE),
      m_rejoinSession(0), m_threadAlive(false), m_threadExiting(false), m_threadStop(false), m_idleSince(0), m_turn(0),
      m_statBad(0), m_statGaps(0), m_statStray(0), m_statPairKeys(0), m_h1Tokens(MP_HELLO1_PER_SEC * 1000u), m_h1KnownTokens(MP_HELLO1_KNOWN_PER_SEC * 1000u), m_h1At(0), m_pkTokens(MP_PAIRKEYS_PER_SEC * 1000u), m_pkAt(0), m_snapPub(0), m_lastStatusLog(0), m_dying(false), m_dieError(MP_ERR_NONE),
      m_rejectWindow(0), m_rejectCount(0), m_lookupActive(false), m_dieReconnect(false), m_reconnect(false),
      m_reconnectStart(0), m_reconnectNext(0), m_reconnectTries(0), m_dieWindow(0), m_reconnectWindow(MP_RECONNECT_MS), m_reconnectErr(MP_ERR_NONE) {
    m_rejoining = false;
    registerLive();   // core2
    m_chatDrops = 0;
    for (int i = 0; i < MP_MAX_PLAYERS; i++) mp_chat_bucket_reset(&m_chatBucket[i]);   // MP3 chat
    memset(m_views, 0, sizeof m_views);
    m_viewSeq[0] = m_viewSeq[1] = 0;
    m_viewIdx = 0;
    m_qPub = 0;
    m_evPub = 0;
    m_threadRunsPub = false;
    m_uHead = m_uTail = 0;
    m_uDropped = 0;
    memset(&m_nl, 0, sizeof m_nl);   // netlog
    memset(&m_settings, 0, sizeof m_settings);
    memset(m_roster, 0, sizeof m_roster);
    memset(m_peers, 0, sizeof m_peers);
    memset(m_ob, 0, sizeof m_ob);
    m_obPending = 0;
    m_statDatagramsOut = m_statMsgsOut = m_statRecvDropped = 0;
    memset(m_pair, 0, sizeof m_pair);   // core2
    memset(m_departed, 0, sizeof m_departed);
    memset(m_psk, 0, sizeof m_psk);
    memset(m_roomKey, 0, sizeof m_roomKey);
    memset(m_cookieSecret, 0, sizeof m_cookieSecret);
    memset(m_token, 0, sizeof m_token);
    memset(m_sk, 0, sizeof m_sk);
    memset(m_pk, 0, sizeof m_pk);
    m_haveKp = m_haveToken = m_haveCookie = m_authBad = false;
    m_authSlot = -1;
    m_hello1At = m_proofWindow = 0;
    m_proofFails = 0;
    memset(m_proofSrc, 0, sizeof m_proofSrc);
    m_statPairKeys = 0;
    memset(&m_hostAddr, 0, sizeof m_hostAddr);
    memset(m_joinCode, 0, sizeof m_joinCode);
    memset(m_roomCode, 0, sizeof m_roomCode);
    memset(m_seen, 0, sizeof m_seen);
    memset(m_seenGen, 0, sizeof m_seenGen);
    memset(m_genFloor, 0, sizeof m_genFloor);
    m_myGen = 0;
    m_ctrBase = 0;
    memset(m_ownCode, 0, sizeof m_ownCode);
    memset(m_ownCodeIn, 0, sizeof m_ownCodeIn);
    memset(m_friendCode, 0, sizeof m_friendCode);
    memset(m_label, 0, sizeof m_label);
    memset(m_annCode, 0, sizeof m_annCode);   // core2
    memset(m_annGen, 0, sizeof m_annGen);
    memset(m_annAt, 0, sizeof m_annAt);
    m_gid = 0;
    m_gathering = false;
    m_closingInto = 0;
    m_redirectTo = 0;
    memset(&m_redirectCode, 0, sizeof m_redirectCode);
    m_statStale = 0;
    memset(m_lastU, 0, sizeof m_lastU);
    memset(m_haveU, 0, sizeof m_haveU);
    memset(m_lastUAt, 0, sizeof m_lastUAt);
    memset(m_res, 0, sizeof m_res);
    memset(m_kicked, 0, sizeof m_kicked);
    memset(m_past, 0, sizeof m_past);
    memset(m_rejoinPeers, 0, sizeof m_rejoinPeers);
    m_pastNext = 0;
    m_ghostMask = 0;
    memset(&m_mergeAddr, 0, sizeof m_mergeAddr);
    m_mergeFirst = m_mergeLast = 0;
    m_mergeSeq = 0;
    m_mergeCount = 0;
    memset(m_where, 0, sizeof m_where);
    memset(m_nameCache, 0, sizeof m_nameCache);
    memset(m_events, 0, sizeof m_events);
    memset(m_rejoinCode, 0, sizeof m_rejoinCode);
    memset(&m_rejoinAddr, 0, sizeof m_rejoinAddr);
    m_directIp = 0;
    m_directPort = 0;
    m_lastProbe = 0;
    m_probeWindow = 0;
    m_probeAnswers = 0;
    m_name[0] = 0;
    mp_name_or_character(m_name, MP_LOOK_MARIO);   // core2 (never "Player")
    for (int i = 0; i < 16; i += 4) {
        const uint32_t v = rnd();
        memcpy(m_consoleKey + i, &v, 4);
    }
    m_consoleKey[0] |= 1;
    m_runId = rnd() | 1u;
    m_wireId = m_runId;
    m_rejoinWireId = m_runId;
    m_settings.mode = MP_MODE_NONE;
    m_settings.maxPlayers = 2;
    m_settings.interaction = 1;
    m_settings.sharedKills = 1;
    m_settings.rivals = 1;
    m_settings.raceWaitSec = 18;
    m_settings.nameTags = 1;
    m_settings.flags = 7 | MP_SET_TEXT_OFF;   // touches: typed messages start off in every room (the host can turn them on)
    m_settings.downed = MP_DOWNED_BUBBLES;
    computeFingerprint();
    publishView();
}

// ---- the game thread's view (perf) --------------------------------------------------------------------------------------------
// Writer: whoever holds m_mutex, at every unlock (CoreLock). It fills the buffer readers are NOT taking, then makes it current.
// Reader: any thread, no lock; a copy that a second publish overlapped is taken again (two publishes during one ~200-byte copy).
// (tsan) The payload words of the seqlock are copied with relaxed atomic accesses: a reader that overlaps a writer then reads a torn
// but well-defined value (the sequence check discards it) instead of a data race. Word loads/stores, so no more costly than the memcpy they replace.
typedef uint32_t __attribute__((may_alias)) ViewWord;
static void ViewStore(MpCoreView* dst, const MpCoreView* src) {
    ViewWord* d = (ViewWord*)dst;
    const ViewWord* s = (const ViewWord*)src;
    size_t n = sizeof(MpCoreView);
    for (; n >= 4; n -= 4) __atomic_store_n(d++, *s++, __ATOMIC_RELAXED);
    uint8_t* db = (uint8_t*)d;
    const uint8_t* sb = (const uint8_t*)s;
    for (; n; n--) __atomic_store_n(db++, *sb++, __ATOMIC_RELAXED);
}
static void ViewLoad(MpCoreView* dst, const MpCoreView* src) {
    ViewWord* d = (ViewWord*)dst;
    const ViewWord* s = (const ViewWord*)src;
    size_t n = sizeof(MpCoreView);
    for (; n >= 4; n -= 4) *d++ = __atomic_load_n(s++, __ATOMIC_RELAXED);
    uint8_t* db = (uint8_t*)d;
    const uint8_t* sb = (const uint8_t*)s;
    for (; n; n--) *db++ = __atomic_load_n(sb++, __ATOMIC_RELAXED);
}

// MP3 profile: the first `bytes` of a view (rounded up to whole words), as ViewLoad does for all of it.
void MpCore::loadViewPrefix(MpCoreView* dst, const MpCoreView* src, size_t bytes) {
    static_assert(sizeof(MpCoreView) % 4 == 0, "whole words");
    if (bytes > sizeof(MpCoreView)) bytes = sizeof(MpCoreView);
    ViewWord* d = (ViewWord*)dst;
    const ViewWord* s = (const ViewWord*)src;
    for (size_t n = (bytes + 3) / 4; n; n--) *d++ = __atomic_load_n(s++, __ATOMIC_RELAXED);
}
namespace {
const size_t kViewHead = offsetof(MpCoreView, name);                                        // scalars, settings, present, look
const size_t kViewNames = offsetof(MpCoreView, name) + sizeof(((MpCoreView*)0)->name);      // ... and the names
const size_t kViewAll = sizeof(MpCoreView);
}  // namespace

void MpCore::publishView() {
    if (m_opt.heldFlag) __atomic_store_n(m_opt.heldFlag, heldNow(), __ATOMIC_RELEASE);   // MP3 lodgate: every unlock, like the view (the state only changes under the lock)
    const uint32_t i = 1u - m_viewIdx;
    MpCoreView v;
    memset(&v, 0, sizeof v);
    v.active = m_active;
    v.originalHost = m_originalHost;
    v.reconnecting = m_reconnect || (m_dying && m_dieReconnect);
    v.role = (uint8_t)m_role;
    v.phase = m_phase;
    v.error = m_error;
    v.localSlot = (int8_t)m_localSlot;
    v.hostSlot = (int8_t)m_hostSlot;
    v.epoch = m_epoch;
    v.serial = m_serial;
    v.settings = m_settings;
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        v.present[s] = m_roster[s].present;
        v.look[s] = m_roster[s].look;
        memcpy(v.name[s], m_label[s][0] ? m_label[s] : m_roster[s].name, MP_NAME_BYTES + 1);   // svcfix3: a shared name shows its label (everything that shows a name reads it here)
        v.peerUsed[s] = m_peers[s].used;
        v.srtt[s] = m_peers[s].srtt;
        v.rosterPing[s] = m_roster[s].ping;
        v.rosterState[s] = m_roster[s].state;
        v.lastRecv[s] = m_peers[s].lastRecv;
        memcpy(v.commit[s], m_roster[s].commit, 16);   // smallfix2
    }
    v.players = (uint8_t)(m_phase == MP_UI_IDLE ? 0 : presentCount());
    v.progress = (uint8_t)(m_role == ROLE_GUEST && m_mem && m_mem->rx.active() ? m_snapProgress : 100);
    memcpy(v.roomCode, m_roomCode, sizeof v.roomCode);
    v.roomCode[sizeof v.roomCode - 1] = 0;
    memcpy(v.friendCode, m_friendCode, sizeof v.friendCode);
    memcpy(v.annCode, m_annCode, sizeof v.annCode);
    for (int s = 0; s < MP_MAX_PLAYERS; s++) v.gid[s] = m_roster[s].present ? (s == m_localSlot ? m_gid : m_roster[s].gid) : 0;
    __atomic_store_n(&m_viewSeq[i], m_viewSeq[i] + 1, __ATOMIC_RELAXED);   // odd: being written
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ViewStore(&m_views[i], &v);
    __atomic_store_n(&m_viewSeq[i], m_viewSeq[i] + 1, __ATOMIC_RELEASE);  // even: complete
    __atomic_store_n(&m_viewIdx, i, __ATOMIC_RELEASE);
    __atomic_store_n(&m_evPub, (uint32_t)m_evCount, __ATOMIC_RELEASE);
    __atomic_store_n(&m_threadRunsPub, m_threadAlive && !m_threadExiting, __ATOMIC_RELEASE);
}

void MpCore::readView(MpCoreView* out) const {
    for (;;) {
        const uint32_t i = __atomic_load_n(&m_viewIdx, __ATOMIC_ACQUIRE);
        const uint32_t s0 = __atomic_load_n(&m_viewSeq[i], __ATOMIC_ACQUIRE);
        if (s0 & 1) continue;   // (only when two publishes overlapped this read: the index moves on at once)
        ViewLoad(out, &m_views[i]);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&m_viewSeq[i], __ATOMIC_RELAXED) == s0) return;
    }
}

// core2: the live cores (tests read sealed datagrams through debugOpenAny); a handful at most, registered by the constructor
static MpMutex s_liveMutex;
static MpCore* s_live[32];

MpCore::~MpCore() {
    shutdown();
    MpLock g(s_liveMutex);
    for (MpCore*& c : s_live)
        if (c == this) c = 0;
}

void MpCore::registerLive() {
    MpLock g(s_liveMutex);
    for (MpCore*& c : s_live)
        if (!c) { c = this; return; }
}

bool MpCore::debugOpenAny(const void* data, size_t len, uint8_t* out, size_t cap, size_t* outLen) {
    if (len < mpseal::kOverhead + 1 || cap < len) return false;
    MpLock g(s_liveMutex);
    for (MpCore* c : s_live) {
        if (!c) continue;
        for (int s = 0; s < MP_MAX_PLAYERS; s++)
            if (c->m_pair[s].have && mpseal::Open(c->m_pair[s].key, (const uint8_t*)data, len, out, outLen)) return true;
        for (int s = 0; s < MP_MAX_PLAYERS; s++)
            if (c->m_departed[s].used && mpseal::Open(c->m_departed[s].key, (const uint8_t*)data, len, out, outLen)) return true;
    }
    return false;
}

uint64_t MpCore::now() { return m_opt.now ? m_opt.now(m_opt.nowCtx) : SysNow(0); }

uint32_t MpCore::rnd() {
    if (!m_rng) {   // (ids and sequence starts, not key material: the weak generator is fine when the secure source fails; smallfix)
        uint32_t v;
        return mp_sys_random_try(&v) ? v : mp_sys_random();
    }
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    if (!m_rng) m_rng = 0x2545F491u;
    return m_rng;
}

void MpCore::setActive(bool on) {
    __atomic_store_n(&m_active, on, __ATOMIC_RELAXED);   // (tsan) active() reads it without the lock
    if (m_opt.activeFlag) __atomic_store_n(m_opt.activeFlag, on, __ATOMIC_RELEASE);
    if (on && m_opt.heldFlag) __atomic_store_n(m_opt.heldFlag, true, __ATOMIC_RELEASE);   // (MP3 lodgate: never later than active; it drops at the unlock, publishView)
}

// MP3 lodgate: this console still owes the session its world state (see port/mp_gate.h SessionHeld for the states and why).
bool MpCore::heldNow() const {
    return port::mpgate::SessionHeld(m_active, m_reconnect, m_dying && m_dieReconnect, m_rejoining && !m_dying);
}

// GXV_MP_STATE_IMMED (default 1): in a 2-player session (active remote peer count == 1), flush MP_MSG_STATE
// immediately (bundleMs = 0 / urgent send) instead of waiting for coalescing, saving 10-15 ms quantization latency.
static int g_stateImmed = -1;
static int RefreshStateImmed() {
    const char* e = getenv("GXV_MP_STATE_IMMED");
    const int v = (!(e && *e) || atoi(e) != 0) ? 1 : 0;
    __atomic_store_n(&g_stateImmed, v, __ATOMIC_RELAXED);
    return v;
}
static inline bool StateImmedOn() {
    int v = __atomic_load_n(&g_stateImmed, __ATOMIC_RELAXED);
    if (v < 0) v = RefreshStateImmed();
    return v != 0;
}

void MpCore::setPhase(MpUiPhase p) {
    if (m_phase != p) {
        // netlog: a session (or a scan) begins: the timing knobs are read once, clamped, and logged with their effective values
        if (m_phase == MP_UI_IDLE) {
            MpNetTune::load();
            RefreshStateImmed();
            memset(&m_nl, 0, sizeof m_nl);
        }
        m_phase = p;
        m_phaseAt = now();
    }
}

// The release gate (tools/release_gates.py buildid) looks for this exact tag in the ELF: the bare id string is not proof (an id such as 3.0.0 is also the app version).
// Kept by the compiler and the linker (used, retain); the fingerprint is unchanged (the id is the same text).
extern "C" const char mp_build_id_tag[] __attribute__((used, retain)) = "MP_BUILD_ID=" MP_BUILD_ID;

static uint32_t FingerprintOf(uint32_t dataHash) {
    uint8_t buf[64];
    MpWriter w(buf, sizeof buf);
    w.bytes("SMGMP/1", 7);
    const char* build = MP_BUILD_ID;
    uint32_t crc = mp_crc32(0, buf, w.n);
    crc = mp_crc32(crc, build, strlen(build));
    MpWriter t(buf, sizeof buf);
    t.u16(MP_PROTO_VERSION);
    t.u32(dataHash);
    return mp_crc32(crc, buf, t.n);
}

void MpCore::computeFingerprint() { m_fingerprint = FingerprintOf(m_dataHash); }

void MpCore::setLoopback(MpLoopHub* hub, uint32_t endpoint) {
    CoreLock l(*this);
    m_hub = hub;
    m_hubEndpoint = endpoint;
}

void MpCore::setDataHash(uint32_t h) {
    CoreLock l(*this);
    m_dataHash = h;
    computeFingerprint();
}

uint32_t MpCore::fingerprint() {
    CoreLock l(*this);
    return m_fingerprint;
}

void MpCore::setConsoleKey(const uint8_t key[16]) {
    CoreLock l(*this);
    for (int i = 0; i < 16; i++)
        if (key[i]) {
            memcpy(m_consoleKey, key, 16);
            m_consoleKeyBad = false;
            return;
        }
}

// smallfix: mp.cfg has no console key and none could be made (no secure random): the constructor's key may be weak, so no session starts
// until a key can be made (randomOk tries again)
void MpCore::setConsoleKeyFailed() {
    CoreLock l(*this);
    m_consoleKeyBad = true;
}

void MpCore::setConsoleId(uint32_t id) {
    CoreLock l(*this);
    if (!id) return;
    uint32_t x = id;
    for (int i = 0; i < 16; i += 4) {   // (any fixed expansion: tests only)
        memcpy(m_consoleKey + i, &x, 4);
        x = x * 2654435761u + 0x9E3779B9u;
    }
}

// The id this console shows to the session it joins or hosts (a keyed hash of its secret and the session id; per-run random while the
// session id is not known yet).
void MpCore::useWireId(uint32_t sessionId) { m_wireId = sessionId ? mp_session_wire_id(m_consoleKey, sessionId) : m_runId; }

void MpCore::setProfile(const char* name, MpLook look) {
    CoreLock l(*this);
    m_look = (uint8_t)(look < MP_LOOK_COUNT ? look : MP_LOOK_MARIO);
    if (!m_active || m_localSlot < 0) {   // (the name is fixed in a session)
        mp_name_clean(name ? name : "", name ? strlen(name) : 0, m_name);
        mp_name_or_character(m_name, m_look);
    }
    if (!m_active || m_localSlot < 0 || !m_mem || m_roster[m_localSlot].look == m_look) return;
    // in a session: the host decides (characters are unique)
    if (m_role == ROLE_HOST) {
        handleLookRequest(m_localSlot, m_look);
    } else if (m_hostSlot >= 0 && m_peers[m_hostSlot].used) {
        const uint8_t want = m_look;
        queueGameReliable(m_hostSlot, MP_MSG_LOOK, (uint8_t)m_localSlot, 0, 0, &want, 1);
    }
    if (m_dying) finishEnd();
}

bool MpCore::allocMem(bool host) {
    if (m_mem) return !host || allocRel(MP_MAX_PLAYERS - 1);
    MpCoreMem* mem = (MpCoreMem*)calloc(1, sizeof(MpCoreMem));
    if (!mem) {
        mp_log("out of memory (%u bytes)", (unsigned)sizeof(MpCoreMem));
        return false;
    }
    for (int i = 0; i < MP_QUEUE_SIZE; i++) mem->qFree[i] = (uint16_t)(MP_QUEUE_SIZE - 1 - i);
    mem->qFreeCount = MP_QUEUE_SIZE;
    mem->rel0.reset(0);
    mem->rel0.setGate(gateThunk);   // core2
    mem->rx.reset();
    {
        MpLock ql(m_qMutex);
        m_mem = mem;
        __atomic_store_n(&m_qPub, 0, __ATOMIC_RELEASE);
    }
    m_spareRetryAt = m_spareDropAt = 0;
    if (host && !allocRel(MP_MAX_PLAYERS - 1)) {   // (a host holds every window from the start: a room that cannot is not opened, as before)
        freeMem();
        return false;
    }
    return true;
}

static int g_debugRelFail = 0;   // tests: the next n window allocations fail
void MpCore::debugFailRelAlloc(int n) { g_debugRelFail = n; }
int MpCore::debugRelWindows() {
    int n = m_mem ? 1 : 0;
    for (int i = 0; m_mem && i < MP_MAX_PLAYERS - 2; i++) n += m_mem->relX[i] != 0;
    return n;
}

// netopt: window i (0 = in the block; 1.. separate, null while not held)
MpReliable* MpCore::relAt(int i) {
    if (!m_mem) return 0;
    return i == 0 ? &m_mem->rel0 : m_mem->relX[i - 1];
}

// netopt: make sure windows 0..n-1 exist. False when a block could not be had (the ones that were got stay).
bool MpCore::allocRel(int n) {
    if (!m_mem) return false;
    for (int i = 1; i < n && i < MP_MAX_PLAYERS - 1; i++) {
        if (m_mem->relX[i - 1]) continue;
        MpReliable* r = g_debugRelFail > 0 && g_debugRelFail-- ? 0 : (MpReliable*)calloc(1, sizeof(MpReliable));
        if (!r) {
            mp_log("out of memory for a reliable window (%u bytes)", (unsigned)sizeof(MpReliable));
            return false;
        }
        r->reset(0);
        r->setGate(gateThunk);
        m_mem->relX[i - 1] = r;
    }
    return true;
}

// netopt: a guest holds one window (its host's). The successor in line (the guest that claims first when the host is lost, with two
// other guests to serve) keeps a second one ready, allocated here on the helper thread long before it is needed, so a takeover never
// depends on the heap at that moment. Every other guest holds none: if the successors in line before it are gone too, it allocates at
// the takeover (relFor), and when that fails the takeover still happens (the guests it has no window for rejoin by HELLO).
// Called every pump; the work is a few compares unless the designation changes. Windows are given back 3 s after they stop being needed.
void MpCore::tendSpareWindows(uint64_t t) {
    if (!m_mem) return;
    if (m_role != ROLE_GUEST) {
        m_spareDropAt = 0;
        return;
    }
    bool want = false;
    if (m_localSlot >= 0 && m_hostSlot >= 0 && (m_phase == MP_UI_LOBBY || m_phase == MP_UI_PLAYING) && m_connKind != MP_CONN_DIRECT) {
        int rank = 0, others = 0;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            if (s == m_hostSlot || s == m_localSlot || !m_roster[s].present) continue;
            others++;
            if (s < m_localSlot) rank++;
        }
        want = rank == 0 && others >= 2;
    }
    if (want) {
        m_spareDropAt = 0;
        if (!m_mem->relX[0] && t >= m_spareRetryAt && !allocRel(2)) m_spareRetryAt = t + 1000;
        return;
    }
    bool any = false;
    for (int i = 0; i < MP_MAX_PLAYERS - 2; i++) any |= m_mem->relX[i] != 0;
    if (!any) {
        m_spareDropAt = 0;
        return;
    }
    if (!m_spareDropAt) m_spareDropAt = t + 3000;
    if (t < m_spareDropAt) return;
    for (int i = 0; i < MP_MAX_PLAYERS - 2; i++) {
        MpReliable* r = m_mem->relX[i];
        if (!r) continue;
        bool used = false;
        for (int q = 0; q < MP_MAX_PLAYERS; q++) used |= m_peers[q].used && m_peers[q].rel == r;
        if (used) continue;
        m_mem->relX[i] = 0;
        free(r);
    }
    m_spareDropAt = 0;
}

// MP3 netpack (memory): the reliable window of the peer at `slot`. There are MP_MAX_PLAYERS - 1 of them: this console is never a peer
// of itself, so at most that many peers exist at once (a host: every guest; a guest: the host; a guest that becomes host: everyone
// else). An entry is free when no used peer points at it. The peer's own previous window is kept. netopt: a guest holds only window 0
// and the successor in line a second (tendSpareWindows); a console that needs one it does not hold allocates it here. If none is free and
// none can be had, canFail returns null (a join is refused, a takeover goes on without that guest); otherwise (the guest paths: the
// host is the only peer, window 0 is free) the oldest other peer is dropped as before: never a null window.
MpReliable* MpCore::relFor(int slot, bool canFail) {
    MpCoreMem& m = *m_mem;
    if (m_peers[slot].used && m_peers[slot].rel) {
        for (int i = 0; i < MP_MAX_PLAYERS - 1; i++)
            if (m_peers[slot].rel == relAt(i)) return m_peers[slot].rel;
    }
    bool missing = false;
    for (int i = 0; i < MP_MAX_PLAYERS - 1; i++) {
        MpReliable* w = relAt(i);
        if (!w) {
            missing = true;
            continue;
        }
        bool taken = false;
        for (int q = 0; q < MP_MAX_PLAYERS; q++)
            if (q != slot && m_peers[q].used && m_peers[q].rel == w) taken = true;
        if (!taken) return w;
    }
    // netopt: every window held is in use: a guest that is becoming a host (or a host that got a window short) takes one more now
    if (missing) {
        for (int i = 1; i < MP_MAX_PLAYERS - 1; i++) {
            if (m.relX[i - 1]) continue;
            if (allocRel(i + 1)) return m.relX[i - 1];
            break;
        }
    }
    if (canFail) return 0;
    int victim = -1;
    for (int q = 0; q < MP_MAX_PLAYERS; q++)
        if (q != slot && m_peers[q].used && (victim < 0 || m_peers[q].createdAt < m_peers[victim].createdAt)) victim = q;
    mp_log("no free reliable window for slot %d: dropping the peer of slot %d", slot, victim);
    MpReliable* r = m_peers[victim].rel;
    m_peers[victim].used = false;
    m_peers[victim].rel = 0;
    return r;
}

size_t MpCore::debugMemBytes() { return sizeof(MpCoreMem); }

void MpCore::freeMem() {
    MpCoreMem* old = 0;
    {
        MpLock ql(m_qMutex);
        old = m_mem;
        m_mem = 0;
        __atomic_store_n(&m_qPub, 0, __ATOMIC_RELEASE);
    }
    if (old)
        for (int i = 0; i < MP_MAX_PLAYERS - 2; i++) free(old->relX[i]);
    free(old);
}

extern "C" void mp_heap_note(const char* when);   // memfix: C heap use at the milestones (mp_log.cpp)
extern "C" int mp_aids_tick(bool inSession, uint64_t nowMs) __attribute__((weak));   // MP3 hwaids (mp_hwaids.cpp)
extern "C" void mp_aids_resumed(int bits) __attribute__((weak));

// ---- session start / end --------------------------------------------------------------------------------------------------
void MpCore::clearSession() {
    __atomic_store_n(&m_uHead, __atomic_load_n(&m_uTail, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);   // (perf) no stale sends
    m_role = ROLE_NONE;
    m_started = false;
    m_originalHost = false;
    m_sessionId = 0;
    m_wantSession = 0;
    m_localSlot = -1;
    m_hostSlot = -1;
    m_epoch = 0;
    m_serial = 0;
    clearRestartGate();
    memset(m_roster, 0, sizeof m_roster);
    memset(m_peers, 0, sizeof m_peers);
    memset(m_pair, 0, sizeof m_pair);          // core2
    memset(m_departed, 0, sizeof m_departed);
    memset(m_roomKey, 0, sizeof m_roomKey);
    m_haveKp = false;
    m_authSlot = -1;
    m_authBad = false;
    m_haveCookie = false;
    memset(&m_hostAddr, 0, sizeof m_hostAddr);
    m_roomCode[0] = 0;
    m_joinCode[0] = 0;
    m_hostLeaving = false;
    m_claiming = false;
    m_rosterDirty = false;
    memset(m_seen, 0, sizeof m_seen);
    memset(m_seenGen, 0, sizeof m_seenGen);
    memset(m_genFloor, 0, sizeof m_genFloor);
    m_myGen = 0;
    m_ctrBase = 0;
    memset(m_friendCode, 0, sizeof m_friendCode);
    memset(m_label, 0, sizeof m_label);
    memset(m_annCode, 0, sizeof m_annCode);   // core2
    memset(m_annGen, 0, sizeof m_annGen);
    memset(m_annAt, 0, sizeof m_annAt);
    m_gathering = false;
    m_closingInto = 0;
    m_myOrigin = 0;
    m_uSeq = 0;
    for (int i = 0; i < MP_MAX_PLAYERS; i++) mp_chat_bucket_reset(&m_chatBucket[i]);   // MP3 chat
    m_chatDrops = 0;
    memset(m_haveU, 0, sizeof m_haveU);
    memset(m_lastUAt, 0, sizeof m_lastUAt);
    memset(m_res, 0, sizeof m_res);
    m_kickedCount = 0;
    m_kickedNext = 0;
    memset(m_past, 0, sizeof m_past);
    m_mergeCount = 0;
    m_ghostMask = 0;
    m_starved = m_starveStale = 0;
    m_haveWhere = false;
    m_raceFinished = false;
    m_raceTurns = 0;
    m_lastBeacon = 0;
    m_snapReady = false;
    __atomic_store_n(&m_snapPub, 0u, __ATOMIC_RELEASE);
    m_snapProgress = 0;
    m_dying = false;
    m_wantMode = 0;
}

void MpCore::requestEnd(MpUiError err, const char* why, bool reconnect, uint32_t window) {
    if (m_dying) return;
    m_dying = true;
    m_dieError = err;
    snprintf(m_dieWhy, sizeof m_dieWhy, "%s", why ? why : "");
    m_dieWindow = window;
    m_dieReconnect = reconnect && (m_phase == MP_UI_LOBBY || m_phase == MP_UI_PLAYING) && m_localSlot >= 0 &&
                     !(m_connKind == MP_CONN_DIRECT && m_role == ROLE_HOST);
    if (m_dieReconnect) publishView();   // publish MP_UI_RECONNECTING + heldFlag BEFORE active flips false
    setActive(false);
    publishView();                       // publish active = false and (when !m_dieReconnect) heldFlag = false immediately
    mp_log("session ended reason=%s (%s)", ErrName(err), why ? why : "");
    mp_heap_note("mp session end");
}

void MpCore::finishEnd() {
    if (!m_dying) return;
    const MpUiError err = m_dieError;
    const uint64_t t = now();
    // netx: a host by address has nobody to rejoin (its guests cannot migrate through their NATs): it ends
    const bool startRc = m_dieReconnect && !m_reconnect && !(m_connKind == MP_CONN_DIRECT && m_role == ROLE_HOST);
    if (startRc) {
        mp_log("connection lost reason=%s", ErrName(err));   // hooks: the scenarios' line (a session member's link is gone; the rejoin follows)
        // remember the way back (as a suspend does)
        m_rejoinValid = true;
        m_rejoinAt = t;
        m_rejoinConn = m_connKind;
        m_rejoinSession = m_sessionId;
        saveRejoinPeers();
        m_rejoinWireId = m_wireId;
        snprintf(m_rejoinCode, sizeof m_rejoinCode, "%s", m_roomCode);
        if (m_role == ROLE_HOST) memset(&m_rejoinAddr, 0, sizeof m_rejoinAddr);
        else m_rejoinAddr = m_hostAddr;
    }
    // fix3: a rejoin the session itself refuses or that finds no room (ROOM_NOT_FOUND: closed / gone) ends now: the room went on or
    // closed without us, and the player reads why ("The host closed the session.") instead of 30 s of "Reconnecting..."
    const bool contRc = m_reconnect && err != MP_ERR_NONE && err != MP_ERR_KICKED && err != MP_ERR_DIFFERENT_VERSION &&
                        err != MP_ERR_ROOM_NOT_FOUND && t - m_reconnectStart < m_reconnectWindow;
    const bool rcOver = m_reconnect && !contRc;
    // what the player reads if the window runs out. corefix D1: after a silent link a failed attempt (a timeout: nobody answered) says
    // nothing new: the honest reason is "connection lost" (NO_WIFI); only a real answer (room full, ...) is kept
    if (contRc && err != MP_ERR_NO_WIFI && !(m_reconnectWindow != MP_RECONNECT_MS && err == MP_ERR_TIMEOUT)) m_reconnectErr = err;
    m_dieReconnect = false;
    const uint32_t dieWindow = m_dieWindow;
    m_dieWindow = 0;
    diagEnd(err, m_dieWhy);   // beta diagnostics: the summary, before the connector leaves the room (its leave request carries it)
    delete m_conn;
    m_conn = 0;
    if (m_lookupActive && mp_svc_room_lookup_cancel) mp_svc_room_lookup_cancel();
    m_lookupActive = false;
    freeMem();
    clearSession();
    setActive(false);
    m_rejoining = false;   // (MP3 lodgate: this attempt is over; m_reconnect, set below, keeps the hold across the next one)
    m_rejoinDirectActive = false;
    m_connKind = MP_CONN_NONE;
    m_error = rcOver && err == MP_ERR_ROOM_NOT_FOUND ? MP_ERR_HOST_LEFT : err;   // fix3 (the room is gone: the host's session ended)
    m_phase = MP_UI_IDLE;
    m_phaseAt = t;
    m_dying = false;
    if (startRc) {
        m_reconnect = true;
        m_reconnectWindow = dieWindow ? dieWindow : MP_RECONNECT_MS;
        m_reconnectStart = t;
        m_reconnectTries = 0;
        m_reconnectErr = MP_ERR_NONE;
        m_reconnectNext = t + 500;
    } else if (contRc) {
        m_reconnectTries++;
        const uint64_t back = 1000u << (m_reconnectTries < 3 ? m_reconnectTries : 3);
        m_reconnectNext = t + back;
    } else {
        m_reconnect = false;
    }
    if (m_reconnect) {
        m_phase = MP_UI_JOINING;   // "Reconnecting..." (MP_UI_RECONNECTING, error MP_ERR_NO_WIFI) until the rejoin works or 30 s pass
        m_error = MP_ERR_NO_WIFI;
        mp_log("reconnecting (try %d)", m_reconnectTries + 1);
    }
}

// smallfix: a session needs key material (key pair, room key, cookie secret, nonce) from the system's secure random source. When it fails
// (3 tries) the session is refused with the core's normal error path (m_error, false from the start call; the menu shows the error), never
// started with a weak generator.
bool MpCore::randomOk() {
    if (m_consoleKeyBad) {   // the console key could not be made at start-up (no secure random then): try again now; no key, no session
        uint8_t k[16];
        if (!mp_config_console_key(k)) {
            m_error = MP_ERR_INTERNAL;
            mp_log("session refused: this console has no key and the secure random source failed");
            return false;
        }
        memcpy(m_consoleKey, k, 16);
        m_consoleKeyBad = false;
    }
    uint32_t probe[4];
    for (int i = 0; i < 4; i++) {
        if (!mp_sys_random_try(&probe[i])) {
            m_error = MP_ERR_INTERNAL;
            mp_log("session refused: the secure random source failed 3 times, no keys can be made");
            return false;
        }
    }
    return true;
}

bool MpCore::startRejoinLocked() {
    if (!m_rejoinValid) return false;
    if (!randomOk()) return false;   // smallfix: (the non-online branch makes its keys in startConnecting)
    m_wireId = m_rejoinWireId;   // (the same id as before: the host takes the old slot back)
    if (m_rejoinConn == MP_CONN_ONLINE) {
        if (!m_rejoinCode[0]) return false;
        clearSession();
        if (!startJoinCodeLocked(m_rejoinCode)) return false;
    } else {
        MpConnectorParams p;
        memset(&p, 0, sizeof p);
        p.directIp = m_directIp;     // netx: by address: the same host again (its HELLO names the session)
        p.directPort = m_directPort;
        p.linkId = m_rejoinWireId;
        MpConnector* c = makeConnector(m_rejoinConn, p);
        if (!c || !allocMem()) {
            delete c;
            return false;
        }
        delete m_scanConn;
        m_scanConn = 0;
        clearSession();
        delete m_conn;
        m_conn = c;
        m_connKind = m_rejoinConn;
        m_role = ROLE_GUEST;
        m_wantSession = m_rejoinSession;
        setPhase(MP_UI_JOINING);
    }
    m_rejoinProbing = false;
    m_rejoinProbeStart = 0;
    m_rejoinProbeAt = 0;
    m_rejoinJoinAt = 0;
    m_rejoinDirectTried = false;
    m_rejoinDirectActive = false;
    m_phaseAt = now();
    m_rejoining = true;   // MP3 lodgate: held (also for a rejoin after a sleep, which has no m_reconnect) until joined / failed / left
    mp_log("rejoin start conn=%s session=%08x", ConnName(m_rejoinConn), (unsigned)m_rejoinSession);
    return true;
}

void MpCore::onWatch(int bits) {
    if (!bits) return;
    if (m_phase == MP_UI_LOBBY || m_phase == MP_UI_PLAYING) {
        if (bits & MP_WATCH_RESUMED) {
            // the others have long seen us vanish; say goodbye anyway (it may still get out) and come back
            for (int s = 0; s < MP_MAX_PLAYERS && m_mem; s++) {
                if (s == m_localSlot || !m_peers[s].used || m_peers[s].closing) continue;
                uint8_t r = MP_BYE_SUSPEND;
                queueReliable(s, MP_MSG_BYE, (uint8_t)m_localSlot, &r, 1);
            }
            if (m_mem && m_conn && m_conn->session()) flushPeers(now());
        }
        requestEnd(MP_ERR_NO_WIFI, (bits & MP_WATCH_RESUMED) ? "resumed from sleep" : "Wi-Fi lost", true);
    } else if (m_phase != MP_UI_IDLE && m_phase != MP_UI_LEAVING && !(m_reconnect && !m_conn)) {
        requestEnd(MP_ERR_NO_WIFI, (bits & MP_WATCH_RESUMED) ? "resumed from sleep" : "Wi-Fi lost");
    }
    if (m_dying) finishEnd();
}

void MpCore::setSceneSerial(uint16_t serial) {
    CoreLock l(*this);
    if (serial != m_serial) { dropGhostHistory(); clearRestartGate(); }   // corefix 4: a new scene: what a dead host relayed is of no use any more
    m_serial = serial;
    purgeOldStates();
}

void MpCore::purgeOldStates() {
    MpLock ql(m_qMutex);
    if (!m_mem) return;
    for (int i = 0; i < m_mem->qCount;) {
        const QEntry& m = m_mem->q[m_mem->qRing[(m_mem->qHead + i) % MP_QUEUE_SIZE]];
        if (m.type == MP_MSG_STATE && m.len >= 6 && SeqDiff((uint16_t)(m.data[4] | (m.data[5] << 8)), m_serial) < 0) queueRemoveAt(i);
        else i++;
    }
}

MpConnector* MpCore::makeConnector(MpConn conn, const MpConnectorParams& p) {
    switch (conn) {
    case MP_CONN_LOOPBACK: return mp_connector_loop_create(m_hub, m_hubEndpoint);
    case MP_CONN_WIFI:
    case MP_CONN_NEARBY: return mp_connector_vlink_create(conn);
    case MP_CONN_ONLINE: return mp_connector_online_create(p);
    case MP_CONN_DIRECT:   // netx (core2: a test core on a hub plays by address over the hub)
        if (m_hub) return mp_connector_loop_direct_create(m_hub, m_hubEndpoint, p);
        return mp_connector_direct_create ? mp_connector_direct_create(p) : 0;
    default: return 0;
    }
}

bool MpCore::startHostingLocked(MpConn conn, const MpSettings& s, uint8_t visibility) {
    if (!randomOk()) return false;
    uint8_t keyMat[64];   // smallfix: the room key and the cookie secret, from the secure source BEFORE any state changes
    if (!mp_sys_random_fill(keyMat, sizeof keyMat)) {
        m_error = MP_ERR_INTERNAL;
        mp_log("host refused: the secure random source failed, no keys can be made");
        return false;
    }
    // validate the settings through the wire codec (one definition of "valid")
    uint8_t buf[16];
    MpWriter w(buf, sizeof buf);
    mp_write_settings(w, s);
    MpReader r(buf, w.n);
    MpSettings checked;
    if (!w.ok() || !mp_read_settings(r, &checked)) {
        mp_log("host refused: bad settings");
        return false;
    }
    // MP3 chat: a room anyone can join (Quick Match, "anyone") is public for good: no typed messages, whoever hosts it later
    checked.flags = (uint8_t)((checked.flags & ~MP_SET_PUBLIC) | (visibility == 2 ? MP_SET_PUBLIC : 0));
    MpConnectorParams p;
    memset(&p, 0, sizeof p);
    p.host = true;
    snprintf(p.name, sizeof p.name, "%s", m_name);
    p.visibility = visibility;
    p.settings = checked;
    const uint32_t newSession = rnd() | 1u;
    useWireId(newSession);   // corefix 6: the id this console shows this session (the link address of Play by Address is made from it)
    p.linkId = m_wireId;   // netx: by address
    MpConnector* c = 0;
    if (m_scanConn && m_scanKind == conn && conn != MP_CONN_ONLINE && !m_scanFailed) {   // (mpwifi44: never a link that failed: a new one is tried)
        c = m_scanConn;   // the link the menu scanned with
        m_scanConn = 0;
    } else {
        delete m_scanConn;
        m_scanConn = 0;
        c = makeConnector(conn, p);
    }
    m_scan.clear();
    if (!c) {
        m_error = conn == MP_CONN_ONLINE ? MP_ERR_SERVICE_DOWN : MP_ERR_NO_WIFI;
        mp_log("host refused: connection %s not available", ConnName(conn));
        return false;
    }
    if (!allocMem(true)) {
        delete c;
        m_error = MP_ERR_INTERNAL;
        return false;
    }
    delete m_conn;
    clearSession();
    m_conn = c;
    m_connKind = conn;
    m_error = MP_ERR_NONE;
    m_role = ROLE_HOST;
    m_originalHost = true;
    m_settings = checked;
    m_visibility = visibility;
    m_sessionId = newSession;
    m_localSlot = 0;
    m_hostSlot = 0;
    m_roster[0].present = true;
    memcpy(m_roster[0].name, m_name, sizeof m_name);
    m_roster[0].look = m_look;
    m_roster[0].state = 1;
    m_roster[0].gen = (MpGen)(((1 + mp_sys_random() % 255) << 8) | 1);
    m_myGen = m_roster[0].gen;
    m_genFloor[0] = m_roster[0].gen;
    m_roster[0].ping = 0;
    makeKeys(m_sessionId);   // core2: this console's key pair in the session (derived: no random), the room key, the cookie secret, our rejoin token
    memcpy(m_roomKey, keyMat, 32);
    memcpy(m_cookieSecret, keyMat + 32, 32);
    memset(keyMat, 0, sizeof keyMat);
    makeToken(m_sessionId);
    m_ctrBase = CtrBase(m_myGen);
    memcpy(m_roster[0].pk, m_pk, 32);
    mpseal::Mac(m_roster[0].commit, 16, 0, 0, m_token, 16);
    setPhase(conn == MP_CONN_ONLINE ? MP_UI_SIGNING_IN : MP_UI_CREATING);
    mp_log("host start conn=%s mode=%d max=%d", ConnName(conn), checked.mode, checked.maxPlayers);
    return true;
}

bool MpCore::startJoinCodeLocked(const char* code) {
    if (!randomOk()) return false;
    MpConnectorParams p;
    memset(&p, 0, sizeof p);
    p.host = false;
    snprintf(p.code, sizeof p.code, "%s", code);
    snprintf(p.name, sizeof p.name, "%s", m_name);
    MpConnector* c = makeConnector(MP_CONN_ONLINE, p);
    if (!c) {
        m_error = MP_ERR_SERVICE_DOWN;
        return false;
    }
    if (!allocMem()) {
        delete c;
        m_error = MP_ERR_INTERNAL;
        return false;
    }
    delete m_conn;
    m_conn = c;
    m_connKind = MP_CONN_ONLINE;
    m_role = ROLE_GUEST;
    snprintf(m_joinCode, sizeof m_joinCode, "%s", code);
    snprintf(m_roomCode, sizeof m_roomCode, "%.6s", code);
    setPhase(MP_UI_JOINING);
    mp_log("join start conn=online code=%s", code);
    return true;
}

void MpCore::startConnecting(const MpAddr& host, int hostSlotGuess, uint64_t t) {
    m_connectTarget = host;   // core2 (a retry: pumpTimers)
    m_connectGuess = hostSlotGuess;
    m_role = ROLE_GUEST;
    m_hostAddr = host;
    m_hostSlot = hostSlotGuess;
    Peer& p = m_peers[hostSlotGuess];
    memset(&p, 0, sizeof p);
    p.used = true;
    p.resumed = true;
    p.addr = host;
    p.rel = relFor(hostSlotGuess);
    p.helloSeq = (uint16_t)rnd();   // the HELLO is the channel's first message: its sequence tells which attempt an answer belongs to
    p.rel->reset(p.helloSeq);
    p.lastRecv = t;
    p.createdAt = t;
    p.srtt = -1;
    // core2: a fresh key pair and nonce for this attempt; the first HELLO is a small unauthenticated one that the host answers with a
    // cookie (no state there yet); the real HELLO (reliable) follows the cookie (handleCookie)
    if (!makeKeys(m_wantSession) || !mp_sys_random_fill(m_nonce, 8)) {   // smallfix: no secure random: the attempt ends (never zero or weak keys)
        mp_log("connect refused: the secure random source failed, no keys can be made");
        requestEnd(MP_ERR_INTERNAL, "no secure random");
        return;
    }
    m_haveCookie = false;
    setPhase(MP_UI_CONNECTING);
    sendHello1(t);
    mp_log("hello sent to %08x:%u", (unsigned)host.host, (unsigned)host.port);
}

// ---- helper thread ---------------------------------------------------------------------------------------------------------
// MP3 perf3: the helper's busy time (its wake to its sleep, wall clock: what it takes from the front end on core 2), measured once
// the game side asked for it (mp_core_helper_take: the SMG_MP_PERFLOG line). One writer (the helper), one reader (the game).
static int g_helperMeasure = 0;
static uint32_t g_helperBusyUs = 0, g_helperWakes = 0, g_helperMaxUs = 0;
extern "C" void mp_core_helper_take(uint32_t* busyUs, uint32_t* wakes, uint32_t* maxUs) {
    __atomic_store_n(&g_helperMeasure, 1, __ATOMIC_RELAXED);
    *busyUs = __atomic_exchange_n(&g_helperBusyUs, 0u, __ATOMIC_RELAXED);
    *wakes = __atomic_exchange_n(&g_helperWakes, 0u, __ATOMIC_RELAXED);
    *maxUs = __atomic_exchange_n(&g_helperMaxUs, 0u, __ATOMIC_RELAXED);
}

// MP3 nocap: the game thread's queued unreliable sends (STATE 30 Hz, POINTER <= 15 Hz) as the helper thread sent them: how many, how
// many the full ring refused (the helper did not drain MP_UOUT_SLOTS sends in time), and how long they waited (queue to drain).
// Measured while mp_core_helper_take measures (the SMG_MP_PERFLOG line); one writer each side, relaxed counters.
static uint32_t g_sendCount = 0, g_sendRingFull = 0, g_sendLagSumUs = 0, g_sendLagMaxUs = 0;
extern "C" void mp_core_send_take(uint32_t* sent, uint32_t* refused, uint32_t* lagSumUs, uint32_t* lagMaxUs) {
    *sent = __atomic_exchange_n(&g_sendCount, 0u, __ATOMIC_RELAXED);
    *refused = __atomic_exchange_n(&g_sendRingFull, 0u, __ATOMIC_RELAXED);
    *lagSumUs = __atomic_exchange_n(&g_sendLagSumUs, 0u, __ATOMIC_RELAXED);
    *lagMaxUs = __atomic_exchange_n(&g_sendLagMaxUs, 0u, __ATOMIC_RELAXED);
}

void MpCore::threadMain(void* self) {
    MpCore* c = (MpCore*)self;
    mp_watch_begin();
    uint64_t sessionSinceMs = 0;   // MP3 nocap: the session's thread census, once, 5 s into it (SMG_MP_PERFLOG measurement on)
    bool censusDone = false;
    for (;;) {
        const uint64_t wakeUs = __atomic_load_n(&g_helperMeasure, __ATOMIC_RELAXED) ? mp_sys_now_us() : 0;
        bool inSession = false, adHoc = false;
        uint64_t t = 0;
        {
            CoreLock l(*c);
            if (c->m_threadStop) {
                c->m_threadExiting = true;
                break;
            }
            t = c->now();
            c->pumpLocked(t);
            // netx: a router port mapping still being removed keeps the thread (it pumps mp_portmap below)
            const bool idle = c->m_phase == MP_UI_IDLE && !c->m_scanConn && !(mp_portmap_busy && mp_portmap_busy());
            inSession = c->m_phase != MP_UI_IDLE;
            // Nearby = an ad hoc cell: the infrastructure Wi-Fi is down by design, the watch must not read that as "Wi-Fi lost"
            adHoc = (c->m_conn ? c->m_connKind : c->m_scanConn ? c->m_scanKind : c->m_reconnect ? c->m_rejoinConn : MP_CONN_NONE) == MP_CONN_NEARBY;
            if (!idle) c->m_idleSince = 0;
            else if (!c->m_idleSince) c->m_idleSince = t;
            else if (t - c->m_idleSince > 2000) {
                c->m_threadExiting = true;   // nothing to pump: the thread ends (started again with the next session / scan)
                break;
            }
        }
        mp_watch_set_ad_hoc(adHoc);
        if (!inSession) {
            sessionSinceMs = 0;
            censusDone = false;
        } else if (!sessionSinceMs) {
            sessionSinceMs = t ? t : 1;
        } else if (!censusDone && t - sessionSinceMs >= 5000 && __atomic_load_n(&g_helperMeasure, __ATOMIC_RELAXED)) {
            censusDone = true;
            mp_sys_thread_census();   // (outside the core lock; the ad hoc receive worker and np_https exist by now)
        }
        int bits = mp_watch_tick(inSession, t);
        if (mp_aids_tick) bits |= mp_aids_tick(inSession, t);   // MP3 hwaids: the 30 s memory line, SMG_MP_SIM_RESUME (a no-op unless set)
        if (bits && (bits & MP_WATCH_RESUMED) && mp_aids_resumed) mp_aids_resumed(bits);   // MP3 hwaids: network state and memory after a resume
        if (bits) {
            CoreLock l(*c);
            c->onWatch(bits);
        }
        if (mp_service_pump) {
            MpLock l(c->m_svcMutex);
            mp_service_pump();
        }
        if (mp_portmap_pump) mp_portmap_pump();   // netx: UPnP (non-blocking steps; outside the core lock)
        if (wakeUs) {
            const uint64_t d = mp_sys_now_us() - wakeUs;
            const uint32_t us = d > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)d;
            __atomic_fetch_add(&g_helperBusyUs, us, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_helperWakes, 1u, __ATOMIC_RELAXED);
            if (us > __atomic_load_n(&g_helperMaxUs, __ATOMIC_RELAXED)) __atomic_store_n(&g_helperMaxUs, us, __ATOMIC_RELAXED);
        }
        mp_watch_sleep_ms((unsigned)MpNetTune::cur().pollMs);
    }
    mp_watch_end();
}

void MpCore::ensureThread() {
    if (!m_opt.useThread) return;
    bool needJoin, needStart;
    {
        CoreLock l(*this);
        needJoin = m_threadAlive && m_threadExiting;
        needStart = !m_threadAlive || m_threadExiting;
        m_idleSince = 0;
    }
    if (needJoin) m_thread.join();
    if (needStart) {
        CoreLock l(*this);
        m_threadExiting = false;
        m_threadStop = false;
        // (perf) core 2, the Vita's aux core (audio, I/O, the ad hoc receive worker, np_https): never the game's core 0, and not
        // core 1, the render thread's, which this thread (priority 0x70) would preempt for every pump
        m_threadAlive = m_thread.start(threadMain, this, "smg_mp_net", 2);
        if (!m_threadAlive) mp_log("helper thread could not start (the game thread pumps alone)");
    }
}

void MpCore::shutdown() {
    {
        CoreLock l(*this);
        if (m_phase != MP_UI_IDLE) {
            // best effort goodbye, no flush wait
            if (m_role == ROLE_HOST) {
                for (int s = 0; s < MP_MAX_PLAYERS; s++) {
                    if (!m_peers[s].used || !m_mem) continue;
                    uint8_t r = MP_BYE_HOST_LEAVING;
                    queueReliable(s, MP_MSG_BYE, (uint8_t)m_localSlot, &r, 1);
                }
            } else if (m_role == ROLE_GUEST && m_hostSlot >= 0 && m_peers[m_hostSlot].used && m_mem) {
                uint8_t r = MP_BYE_LEAVE;
                queueReliable(m_hostSlot, MP_MSG_BYE, (uint8_t)m_localSlot, &r, 1);
            }
            if (m_mem) flushPeers(now());
            requestEnd(MP_ERR_NONE, "shutdown");
            finishEnd();
        }
        delete m_scanConn;
        m_scanConn = 0;
        m_threadStop = true;
    }
    if (m_threadAlive) {
        m_thread.join();
        m_threadAlive = false;
    }
    if (mp_portmap_finish) mp_portmap_finish(600);   // netx: the router's port mapping goes with us (else it runs out with its lease)
}

// Merge fix 5: the system is closing the app (the Vita's quit path ends the process right after this). What shutdown() does that cannot be
// bounded is left out: no wait for the helper thread, no radio teardown (requestEnd / finishEnd), no UPnP unmapping (600 ms). Only the BYE,
// flushed to the transport, so the others see this console leave at once instead of timing it out after 15 s. The core lock is tried for at most
// budgetMs (the helper thread holds it for short pumps only); when it is not obtained there is no goodbye.
bool MpCore::goodbye(unsigned budgetMs) {
    const uint64_t t0 = mp_sys_now_us();
    while (!m_mutex.tryLock()) {
        if (mp_sys_now_us() - t0 >= (uint64_t)budgetMs * 1000u) return false;
        mp_sys_sleep_ms(1);
    }
    CoreLockAdopt l(*this);
    if (m_phase == MP_UI_IDLE) return true;
    if (!m_mem) return false;
    bool queued = false;
    if (m_role == ROLE_HOST) {
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            if (!m_peers[s].used) continue;
            uint8_t r = MP_BYE_HOST_LEAVING;
            queued |= queueReliable(s, MP_MSG_BYE, (uint8_t)m_localSlot, &r, 1);
        }
    } else if (m_role == ROLE_GUEST && m_hostSlot >= 0 && m_peers[m_hostSlot].used) {
        uint8_t r = MP_BYE_LEAVE;
        queued = queueReliable(m_hostSlot, MP_MSG_BYE, (uint8_t)m_localSlot, &r, 1);
    }
    flushPeers(now());
    mp_log("goodbye: BYE sent before the app closes (%s)", queued ? "queued" : "nobody to tell");
    return true;
}

// ---- pump ------------------------------------------------------------------------------------------------------------------
void MpCore::pump() {
    CoreLock l(*this);
    pumpLocked(now());
}

void MpCore::pumpLocked(uint64_t t) {
    // integ: net/ once per ~frame while multiplayer uses the network (a session, a scan, a reconnect) and 10 s after (the leave
    // message, the radio closing). Under m_mutex: the game thread and the helper thread both pump, net/ sees one at a time.
    if (m_phase != MP_UI_IDLE || m_conn || m_scanConn || m_reconnect) __atomic_store_n(&g_netTailUntil, t + 10000, __ATOMIC_RELAXED);
    const uint64_t tailUntil = __atomic_load_n(&g_netTailUntil, __ATOMIC_RELAXED);
    if (tailUntil && t < tailUntil && t - __atomic_load_n(&g_netFrameAt, __ATOMIC_RELAXED) >= 15) {
        __atomic_store_n(&g_netFrameAt, t, __ATOMIC_RELAXED);
        mp_netglue_frame();
    }
    if (m_scanConn) pumpScan(t);
    if (m_reconnect && !m_conn && m_phase == MP_UI_JOINING && !m_dying) {
        if (t - m_reconnectStart > m_reconnectWindow) {
            m_reconnect = false;
            m_rejoining = false;   // MP3 lodgate: gave up: nothing is held any more
            m_phase = MP_UI_IDLE;
            m_error = m_reconnectErr != MP_ERR_NONE ? m_reconnectErr : MP_ERR_NO_WIFI;   // fix3: the last real reason, if any
            mp_log("reconnect: gave up (%s)", ErrName(m_error));
        } else if (t >= m_reconnectNext && mp_watch_wifi_up() && !startRejoinLocked()) {
            m_reconnectNext = t + 2000;
        }
        return;
    }
    if (m_reconnect && m_conn && !m_dying && t - m_reconnectStart > m_reconnectWindow) {
        // corefix D1: the window is over while an attempt is still running (an attempt may take up to MP_LINK_TIMEOUT_MS): end now
        const MpUiError why = m_reconnectErr != MP_ERR_NONE ? m_reconnectErr : MP_ERR_NO_WIFI;
        mp_log("reconnect: gave up (%s)", ErrName(why));
        requestEnd(why, "reconnect window over");
    }
    if (m_phase != MP_UI_IDLE && !m_dying) {
        if (m_mem) {
            for (int s = 0; s < MP_MAX_PLAYERS; s++) m_peers[s].sentThisPump = false;
        }
        pumpConnector(t);
        if (!m_dying && m_mem && m_conn && m_conn->session()) receiveAll(t);
        refreshFriendCodes();
        pickOwnCode();
        if (!m_dying && m_mem && m_conn && m_conn->session()) drainUnreliableOut();
        // (coordinator, found by the practice-partner agent) The connector and the handlers above stamp things with their own,
        // later clock reads (setPhase, sendHello's phase, handleCore's accept): the timers must not run on the pump's older time,
        // or an unsigned "t - stamp" wraps: a join ended at once with "no answer from the host". Timers and the flush take a
        // fresh time; Since() below also saturates.
        const uint64_t tt = now();
        if (!m_dying && m_mem) pumpTimers(tt > t ? tt : t);
        if (!m_dying && m_mem && m_conn && m_conn->session()) flushPeers(tt > t ? tt : t);
    }
    if (m_dying) finishEnd();
}

void MpCore::pumpConnector(uint64_t t) {
    if (m_phase == MP_UI_SEARCHING) {
        if (!mp_svc_room_lookup_poll) {
            requestEnd(MP_ERR_SERVICE_DOWN, "no service");
            return;
        }
        char code[16] = {0};
        const int r = mp_svc_room_lookup_poll(code, sizeof code);
        if (r == 0) return;
        m_lookupActive = false;
        if (r < 0) {
            requestEnd((MpUiError)(-r), "room lookup failed");
            return;
        }
        if (!code[0]) {
            mp_log("quick match: no open room, hosting a public one");
            MpSettings s = mp_config_host_settings(m_wantMode ? (MpMode)m_wantMode : MP_MODE_TOGETHER);
            if (!startHostingLocked(MP_CONN_ONLINE, s, 2)) requestEnd(MP_ERR_INTERNAL, "quick match host");
        } else if (!startJoinCodeLocked(code)) {
            requestEnd(MP_ERR_SERVICE_DOWN, "join after lookup");
        }
        return;
    }
    if (!m_conn) return;
    const int r = m_conn->poll(t);
    if (r < 0) {
        // fix3: dropped from the online room (not_member): rejoin it as after a sleep, never sit in a session nobody else is in
        const bool rejoin = m_conn->rejoinable();
        if (rejoin) mp_log("dropped from the room (%s): rejoining", m_conn->name());
        requestEnd((MpUiError)(-r), m_conn->name(), rejoin);
        return;
    }
    if (r != MP_CONNECTOR_READY) {
        if (m_phase == MP_UI_SIGNING_IN) setPhase(MP_UI_CREATING);   // (the connector moved past the service sign-in)
        return;
    }
    if (m_phase == MP_UI_CREATING || m_phase == MP_UI_SIGNING_IN) {
        m_hostAddr = m_conn->session()->local();
        m_roster[0].addr = m_hostAddr;
        m_conn->roomCode(m_roomCode, sizeof m_roomCode);
        setPhase(MP_UI_LOBBY);
        setActive(true);
        mp_log("hosting session=%08x conn=%s code=%s", (unsigned)m_sessionId, m_conn->name(), m_roomCode);
    } else if ((m_phase == MP_UI_JOINING && m_role == ROLE_GUEST) ||
               (m_phase == MP_UI_CONNECTING && m_role == ROLE_GUEST && m_rejoining && m_wantSession && m_rejoinDirectActive)) {
        if (m_phase == MP_UI_JOINING && (m_connKind == MP_CONN_ONLINE || m_connKind == MP_CONN_DIRECT)) {   // netx: by address: the link sends to the typed address
            MpAddr b;
            b.host = MP_ADDR_BROADCAST;
            b.port = MP_SESSION_PORT;
            startConnecting(b, 0, t);
        } else if (m_wantSession && m_connKind != MP_CONN_ONLINE && m_connKind != MP_CONN_DIRECT) {
            // fast rejoin after a suspend / link drop: actively probe broadcast + last host + known peers
            // (so routers / APs that drop broadcast beacons still answer unicast) and fall back directly
            // to m_rejoinAddr if discovery port 21318 is dropped while session port 21319 works.
            // Keep pumping discovery beacons even during direct fallback so a migrated host takes over immediately.
            if (!m_rejoinProbing) {
                m_rejoinProbing = true;
                m_rejoinProbeStart = t;
                m_rejoinProbeAt = 0;
            }
            MpTransport* d = m_conn->discovery();
            const uint32_t localHost = m_conn->session() ? m_conn->session()->local().host : 0;
            if (d && (!m_rejoinProbeAt || Since(t, m_rejoinProbeAt) >= MP_PROBE_MS)) {
                m_rejoinProbeAt = t;
                uint8_t probe[MP_PROBE_BYTES];
                if (mp_probe_write(probe, sizeof probe, m_fingerprint)) {
                    uint32_t sent[MP_MAX_PLAYERS + 10];
                    int nSent = 0;
                    auto sendProbeTo = [&](uint32_t host) {
                        if (!host || (localHost && host == localHost)) return;
                        for (int k = 0; k < nSent; k++)
                            if (sent[k] == host) return;
                        if (nSent < (int)(sizeof sent / sizeof sent[0])) sent[nSent++] = host;
                        MpAddr dst;
                        dst.host = host;
                        dst.port = MP_LAN_PORT;
                        d->send(dst, probe, sizeof probe);
                    };
                    sendProbeTo(MP_ADDR_BROADCAST);
                    if (m_rejoinAddr.host != 0 && m_rejoinAddr.host != MP_ADDR_BROADCAST) sendProbeTo(m_rejoinAddr.host);
                    for (int s = 0; s < MP_MAX_PLAYERS + 8; s++) {
                        if (m_rejoinPeers[s] && m_rejoinPeers[s] != MP_ADDR_BROADCAST) sendProbeTo(m_rejoinPeers[s]);
                    }
                }
            }
            uint8_t buf[256];
            MpAddr from;
            bool connected = false;
            for (int i = 0; d && i < 64; i++) {
                const int n = d->recv(&from, buf, sizeof buf);
                if (n <= 0) break;
                MpBeacon b;
                if (mp_beacon_read(buf, (size_t)n, &b) && b.sessionId == m_wantSession && b.fingerprint == m_fingerprint) {
                    // corefix 10: the session id and fingerprint are public: a beacon from an address that was not in the room when we left
                    // (a forged one pointing the rejoin elsewhere) is not followed
                    bool known = m_rejoinAddr.host != 0 && m_rejoinAddr.host != MP_ADDR_BROADCAST && from.host == m_rejoinAddr.host;
                    for (int s = 0; !known && s < MP_MAX_PLAYERS + 8; s++)
                        if (m_rejoinPeers[s] && m_rejoinPeers[s] == from.host) known = true;
                    if (!known) {
                        if (m_statStray++ % 32 == 0) mp_log("rejoin: a beacon of the session from an unknown address %08x is ignored", (unsigned)from.host);
                        continue;
                    }
                    if (m_phase == MP_UI_CONNECTING) {
                        m_rejoinDirectActive = false;
                        if (from.host == m_connectTarget.host) {
                            connected = true;
                            break;   // original host answered on discovery too; keep in-flight direct handshake
                        }
                        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
                            if (m_peers[s].used && m_peers[s].rel) m_peers[s].rel->reset(0);
                            memset(&m_peers[s], 0, sizeof m_peers[s]);
                            pairReset(s);
                        }
                        m_haveCookie = false;
                        mp_log("rejoin: migrated host beacon %08x:%u while direct-connecting, switching", (unsigned)from.host, (unsigned)from.port);
                    }
                    startConnecting(from, b.hostSlot, t);
                    connected = true;
                    break;
                }
            }
            const uint64_t directDelayMs = m_reconnect ? 2000u : 3500u;
            if (m_phase == MP_UI_JOINING && !connected && !m_rejoinDirectTried && Since(t, m_rejoinProbeStart) >= directDelayMs &&
                m_rejoinAddr.host != 0 && m_rejoinAddr.host != MP_ADDR_BROADCAST && m_rejoinAddr.port != 0 &&
                (!m_conn->session() || !(m_rejoinAddr == m_conn->session()->local()))) {
                m_rejoinDirectTried = true;
                m_rejoinDirectActive = true;
                m_rejoinJoinAt = m_phaseAt;
                mp_log("rejoin: no beacon after %u ms, trying direct reconnect to %08x:%u", (unsigned)directDelayMs,
                       (unsigned)m_rejoinAddr.host, (unsigned)m_rejoinAddr.port);
                startConnecting(m_rejoinAddr, 0, t);
            }
        }
    }
}

void MpCore::pumpScan(uint64_t t) {
    if (m_phase != MP_UI_IDLE || t - m_scanLastCall > 5000) {
        if (m_phase == MP_UI_IDLE) {
            delete m_scanConn;
            m_scanConn = 0;
            m_scan.clear();
        }
        return;
    }
    const int r = m_scanConn->poll(t);
    if (r < 0) {
        if (!m_scanFailed) {
            // mpwifi44: the exact reason, kept for whoever asked for the scan (the gathering: it used to show "Couldn't connect to Wi-Fi" for all of them)
            m_scanError = (MpUiError)(-r);
            snprintf(m_scanWhy, sizeof m_scanWhy, "%s", m_scanConn->failWhy());
            mp_log("scan: %s link failed (%s)%s%s", m_scanConn->name(), ErrName(m_scanError), m_scanWhy[0] ? ": " : "", m_scanWhy);
        }
        m_scanFailed = true;
        return;
    }
    if (r != MP_CONNECTOR_READY) return;
    uint8_t buf[256];
    MpAddr from;
    if (MpTransport* d = m_scanConn->discovery()) {
        // netx: active discovery: a probe a second; hosts answer with their beacon unicast (mp_lan.h)
        if (!m_lastProbe || t - m_lastProbe >= MP_PROBE_MS) {
            m_lastProbe = t;
            uint8_t probe[MP_PROBE_BYTES];
            MpAddr all;
            all.host = MP_ADDR_BROADCAST;
            all.port = MP_LAN_PORT;
            if (mp_probe_write(probe, sizeof probe, m_fingerprint)) d->send(all, probe, sizeof probe);
        }
        for (int i = 0; i < 128; i++) {
            const int n = d->recv(&from, buf, sizeof buf);
            if (n <= 0) break;
            MpBeacon b;
            if (!mp_beacon_read(buf, (size_t)n, &b)) continue;
            m_scan.add(from, b, t, m_fingerprint);
        }
    }
    if (MpTransport* s = m_scanConn->session()) {
        for (int i = 0; i < 64; i++)
            if (s->recv(&from, buf, sizeof buf) <= 0) break;   // nothing for a console that is not in a session
    }
    m_scan.expire(t);
}

// MP3 netfeel: a datagram from a member's address counts for that slot (g_linkDgIn); strangers are not counted
void MpCore::noteLinkIn(const MpAddr& from, size_t n) {
    for (int i = 0; i < MP_MAX_PLAYERS; i++)
        if (m_peers[i].used && m_peers[i].addr == from) {
            __atomic_fetch_add(&g_linkDgIn[i], 1u, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_linkBytesIn[i], (uint32_t)n, __ATOMIC_RELAXED);
            return;
        }
}

void MpCore::receiveAll(uint64_t t) {
    MpTransport* s = m_conn->session();
    if (s->lost()) {
        requestEnd(MP_ERR_NO_WIFI, "link lost", true);
        return;
    }
    if (m_lastRecvPump && t - m_lastRecvPump > 500) m_rateGraceUntil = t + 1500;
    if (m_lastRecvPump && t - m_lastRecvPump > MP_WAKE_GAP_MS && m_localSlot >= 0) {
        // core2 (claims audit 1): THIS console was frozen (suspend, a stalled process): the silence was ours, not the others'. Every
        // peer gets its full timeouts again from now (what it sent meanwhile is waiting in our socket): a console that just woke must
        // never take the host role from a healthy host, nor remove guests, for a gap of its own.
        mp_log("woke after %u ms without a pump: the others get their full timeouts", (unsigned)(t - m_lastRecvPump));
        for (int s2 = 0; s2 < MP_MAX_PLAYERS; s2++) {
            Peer& p = m_peers[s2];
            if (!p.used) continue;
            if (p.lastRecv < t) p.lastRecv = t;
            p.ackAt = t;
            if (!p.resumed && p.createdAt < t) p.createdAt = t;
        }
        if (m_claiming) m_claimAt = t;
    }
    m_lastRecvPump = t;
    uint8_t buf[MP_DATAGRAM_MAX + 64];
    MpAddr from;
    // MP3 netpack: a budget per pump (datagrams and time), so a flood cannot hold the lock the game thread waits on; the rest is
    // dropped (reliable messages come again, STATE is replaced by the next one)
    // (netcore: the budget counts packets, not datagrams: a bundle carries up to MP_BUNDLE_MAX of them, so a datagram budget let a
    // flood of bundles cost 64x the work per pump)
    const uint64_t startUs = mp_sys_now_us();
    int handled = 0, datagrams = 0;
    bool over = false;
    while (!m_dying) {
        // (the time half only on the real clock: with a test clock (virtual time) it made results depend on how loaded the machine was)
        if (handled >= MP_RECV_BUDGET_DATAGRAMS ||
            (!m_opt.now && !(datagrams & 7) && datagrams && mp_sys_now_us() - startUs > MP_RECV_BUDGET_US)) {
            over = true;
            break;
        }
        const int n = s->recv(&from, buf, sizeof buf);
        if (n < 0) {
            requestEnd(MP_ERR_NO_WIFI, "link error", true);
            return;
        }
        if (n == 0) break;
        datagrams++;
        m_nl.rx++;   // netlog
        m_dgRx++;    // beta diagnostics
        m_nl.rxBytes += (uint32_t)n;
        if (m_nl.lastRx && t >= m_nl.lastRx && t - m_nl.lastRx > m_nl.rxGapMax) m_nl.rxGapMax = (uint32_t)(t - m_nl.lastRx);
        m_nl.lastRx = t;
        noteLinkIn(from, (size_t)n);
        handled += handleDatagram(from, buf, (size_t)n, t);
    }
    if (over) {
        // What is still waiting: a stranger's datagrams are dropped unread (cheap), so a flood cannot hold the lock. A member's are
        // still handled, within a second budget of the same size: a stranger's flood must not starve the members' ACKs and
        // heartbeats (they would be removed as silent after MP_PEER_LOST_MS), and a member is rate-capped per peer anyway.
        int extra = 0;
        for (int k = 0; k < MP_RECV_BUDGET_DATAGRAMS * 4 && !m_dying; k++) {
            const int n = s->recv(&from, buf, sizeof buf);
            if (n <= 0) break;
            if (extra >= MP_RECV_BUDGET_DATAGRAMS) continue;
            bool member = false;
            for (int i = 0; i < MP_MAX_PLAYERS; i++)
                if (m_peers[i].used && m_peers[i].addr == from) {
                    member = true;
                    break;
                }
            if (member) { noteLinkIn(from, (size_t)n); extra += handleDatagram(from, buf, (size_t)n, t); }
        }
        m_statRecvDropped++;
    }
    if (m_role == ROLE_HOST) {
        if (!m_dying) checkMerge(t);   // (drains the discovery socket)
    } else if (MpTransport* d = m_conn->discovery()) {
        for (int i = 0; i < 64; i++)
            if (d->recv(&from, buf, sizeof buf) <= 0) break;   // beacons of other rooms: not needed as a guest
    }
}

void MpCore::debugReceive(const MpAddr& from, const void* data, size_t len) {
    CoreLock l(*this);
    if (m_phase == MP_UI_IDLE || !m_mem || m_dying) return;
    handleDatagram(from, (const uint8_t*)data, len, now());
    if (m_dying) finishEnd();
}

uint16_t MpCore::debugExpectedSeq(int slot) {
    CoreLock l(*this);
    if (slot < 0 || slot >= MP_MAX_PLAYERS || !m_peers[slot].used) return 0;
    return m_peers[slot].rel->expected();
}

int MpCore::debugSrtt(int slot) {
    CoreLock l(*this);
    if (slot < 0 || slot >= MP_MAX_PLAYERS || !m_peers[slot].used) return -2;
    return m_peers[slot].srtt;
}

int MpCore::debugUnacked(int slot) {
    CoreLock l(*this);
    if (slot < 0 || slot >= MP_MAX_PLAYERS || !m_peers[slot].used) return -1;
    return m_peers[slot].rel->unacked();
}

MpAddr MpCore::debugLocalAddr() {
    CoreLock l(*this);
    MpAddr a;
    memset(&a, 0, sizeof a);
    if (m_conn && m_conn->session()) a = m_conn->session()->local();
    return a;
}

// ---- sending ---------------------------------------------------------------------------------------------------------------
// One packet (12-byte header + payload) into buf; returns its length.
static size_t BuildPacket(uint8_t* buf, uint8_t type, uint8_t from, uint8_t flags, uint16_t seq, uint16_t ack, uint16_t bits,
                          const void* payload, uint16_t len) {
    buf[0] = 'G';
    buf[1] = 'M';
    buf[2] = MP_PROTO_VERSION;
    buf[3] = type;
    buf[4] = from;
    buf[5] = flags;
    buf[6] = (uint8_t)seq;
    buf[7] = (uint8_t)(seq >> 8);
    buf[8] = (uint8_t)ack;
    buf[9] = (uint8_t)(ack >> 8);
    buf[10] = (uint8_t)bits;
    buf[11] = (uint8_t)(bits >> 8);
    if (len) memcpy(buf + MP_HEADER_BYTES, payload, len);
    return MP_HEADER_BYTES + len;
}

bool MpCore::rawSend(const MpAddr& to, uint8_t type, uint8_t from, uint8_t flags, uint16_t seq, uint16_t ack, uint16_t bits,
                     const void* payload, uint16_t len) {
    if (!m_conn || !m_conn->session() || len > MP_DATAGRAM_MAX - MP_HEADER_BYTES) return false;
    uint8_t buf[MP_DATAGRAM_MAX];
    const size_t n = BuildPacket(buf, type, from, flags, seq, ack, bits, payload, len);
    return txSend(to, buf, n) >= 0;
}

// MP3 netpack: coalescing. Packets for one peer collect in m_ob[slot] and leave as one datagram (flushOut: end of every locked
// operation / the buffer is full). One packet alone goes out as the plain packet; two or more as a bundle:
//   'G' 'B' proto u8, count u8, then count x { len u16, packet (12-byte header + payload, len = 12 + payload) }.
// Every packet keeps its own header (seq, ack fields), so the reliable channel does not know the difference.
int MpCore::guestLinks() const {
    int n = 0;
    for (int q = 0; q < MP_MAX_PLAYERS; q++)
        if (q != m_localSlot && m_peers[q].used && m_peers[q].resumed && !m_peers[q].closing) n++;
    return n;
}

void MpCore::flushOut(int slot) {
    OutBuf& o = m_ob[slot];
    if (!o.count) return;
    const uint8_t count = o.count;
    o.count = 0;
    m_obPending &= ~(1u << slot);
    if (!m_conn || !m_conn->session()) return;
    const uint8_t* d;
    size_t n;
    if (count == 1) {
        d = o.data + MP_BUNDLE_HEADER + 2;
        n = o.len - MP_BUNDLE_HEADER - 2;
    } else {
        o.data[0] = 'G';
        o.data[1] = 'B';
        o.data[2] = MP_PROTO_VERSION;
        o.data[3] = count;
        d = o.data;
        n = o.len;
    }
    // core2: everything is sealed with the pair's key, except a datagram holding a HELLO (the joiner's first reliable message)
    if (!o.hasHello && keyFor(slot)) {
        uint8_t sealed[MP_DATAGRAM_MAX + mpseal::kOverhead + 8];
        PairState& ps = m_pair[slot];
        const size_t sn = mpseal::Seal(ps.key, MP_PROTO_VERSION, (uint8_t)(m_localSlot >= 0 ? m_localSlot : 0xFF), ps.sendCtr++, d, n, sealed,
                                       sizeof sealed);
        if (sn) txSend(o.to, sealed, sn);
    } else if (o.hasHello || m_role == ROLE_NONE) {
        txSend(o.to, d, n);
    }   // (no key for a member: nothing goes out plain)
    o.hasHello = false;
    m_statDatagramsOut++;
    if (slot >= 0 && slot < MP_MAX_PLAYERS) __atomic_fetch_add(&g_linkDgOut[slot], 1u, __ATOMIC_RELAXED);
}

// What is due: a buffer holding anything but STATE / POINTER (a reliable message, an ack, a heartbeat) goes at once; a buffer of
// poses only waits up to MP_BUNDLE_HOLD_MS for more of them (the host's relays of the other players' poses arrive spread over the
// turn: together they make one datagram per link per turn instead of one each). Full buffers go at once. In a 2-player session
// (1 active remote peer) with GXV_MP_STATE_IMMED (default 1), there is nothing else to bundle so STATE flushes immediately (holdMs = 0).
void MpCore::flushOutDue(uint64_t t) {
    const uint64_t holdMs = (StateImmedOn() && guestLinks() <= 1) ? 0u : (uint64_t)MpNetTune::cur().bundleMs;
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        if (!(m_obPending & (1u << s))) continue;
        const OutBuf& o = m_ob[s];
        if (o.urgent || o.len > MP_DATAGRAM_MAX - 200 || Since(t, o.firstAt) >= holdMs) flushOut(s);
    }
}

void MpCore::flushOutAll() {
    for (int s = 0; s < MP_MAX_PLAYERS; s++)
        if (m_obPending & (1u << s)) flushOut(s);
}

bool MpCore::queueOut(int slot, const MpAddr& to, uint8_t type, uint8_t from, uint8_t flags, uint16_t seq, uint16_t ack, uint16_t bits,
                      const void* payload, uint16_t len) {
    if (!m_conn || !m_conn->session() || len > MP_DATAGRAM_MAX - MP_HEADER_BYTES) return false;
    OutBuf& o = m_ob[slot];
    const size_t need = 2 + MP_HEADER_BYTES + len;
    if (o.count && (o.to != to || o.len + need > MP_DATAGRAM_MAX - mpseal::kOverhead || o.count >= MP_BUNDLE_MAX ||
                    (type == MP_MSG_HELLO) != o.hasHello)) flushOut(slot);   // (core2: room for the seal; a HELLO travels alone)
    if (!o.count) {
        o.to = to;
        o.len = MP_BUNDLE_HEADER;
        o.urgent = false;
        o.hasHello = type == MP_MSG_HELLO;
        o.firstAt = now();
    }
    // (only the host's links hold poses: it has the relays of several players to put together; a guest sends its one pose per turn at
    // once. netcore: and only with two guests or more: with one, the host's link carries its own pose alone: the hold was pure lag,
    // +25 ms a pose for 20 % fewer datagrams; with 2 / 3 guests a 10 ms hold halves / thirds them: tests/mp/netcore_measure.cpp)
    const bool immedOne = StateImmedOn() && guestLinks() <= 1;
    if (m_role != ROLE_HOST || (flags & (MP_HDR_RELIABLE | MP_HDR_ACK_ONLY)) || (type != MP_MSG_STATE && type != MP_MSG_POINTER) || immedOne)
        o.urgent = true;
    const size_t n = BuildPacket(o.data + o.len + 2, type, from, flags, seq, ack, bits, payload, len);
    o.data[o.len] = (uint8_t)n;
    o.data[o.len + 1] = (uint8_t)(n >> 8);
    o.len = (uint16_t)(o.len + 2 + n);
    o.count++;
    m_obPending |= 1u << slot;
    m_statMsgsOut++;
    return true;
}

// netopt: the heartbeat's two jobs are (1) keep the link audible (a datagram at least every MP_KEEPAWAKE_MS, for Wi-Fi power save, and
// as proof of life: a peer's lastRecv moves on any packet carrying an ack, so a pose does it as well as a heartbeat) and (2) the
// clock echo behind the ping / retransmit timer. Job 1 is done by whatever else leaves the link; job 2 needs one only every
// MP_HEARTBEAT_PIGGY_MS, and that one is queued into the same datagram as the packet being sent (no datagram of its own).
void MpCore::queueHeartbeat(int slot, uint64_t t) {
    Peer& p = m_peers[slot];
    uint8_t hb[8];
    MpWriter w(hb, sizeof hb);
    w.u32((uint32_t)t);
    w.u32(p.havePeerTime ? p.peerTime + (uint32_t)(t - p.peerTimeAt) : 0u);
    p.lastHeartbeat = t;   // (before the send: sendToPeer must not piggyback another)
    sendToPeer(slot, MP_MSG_HEARTBEAT, (uint8_t)(m_localSlot < 0 ? 0xFF : m_localSlot), 0, m_uSeq, hb, 8);
}

bool MpCore::sendToPeer(int slot, uint8_t type, uint8_t from, uint8_t flags, uint16_t seq, const void* payload, uint16_t len) {
    Peer& p = m_peers[slot];
    if (type != MP_MSG_HEARTBEAT && type != 0 && p.rel->recvStarted() && (m_phase == MP_UI_LOBBY || m_phase == MP_UI_PLAYING || m_phase == MP_UI_LEAVING) &&
        !p.closing && Since(now(), p.lastHeartbeat) >= (uint64_t)MpNetTune::cur().piggyMs)
        queueHeartbeat(slot, now());   // netopt: the ping's echo rides along with this packet
    uint16_t ack = 0, bits = 0;
    if (p.rel->recvStarted()) {
        p.rel->ackFields(&ack, &bits);
        flags |= MP_HDR_ACK;
    }
    if (now() < p.gateRefusedUntil) flags |= MP_HDR_BUSY;   // starfix: our game is not reading (a load): the sender must not take that for a dead link
    const bool ok = queueOut(slot, p.addr, type, from, flags, seq, ack, bits, payload, len);
    p.sentThisPump = true;
    p.lastSend = now();
    p.rel->ackDirty = false;
    return ok;
}

bool MpCore::queueReliable(int slot, uint8_t type, uint8_t from, const void* payload, uint16_t len) {
    Peer& p = m_peers[slot];
    if (!p.used || !m_mem) return false;
    if (p.rel->push(type, from, payload, len)) return true;
    // a console this far behind cannot catch up
    if (m_role == ROLE_HOST && slot != m_hostSlot) {
        mp_log("slot %d send backlog full", slot);
        removeSlot(slot, MP_BYE_TIMEOUT, "backlog full");
    } else {
        requestEnd(MP_ERR_INTERNAL, "send backlog full");
    }
    return false;
}

bool MpCore::queueGameReliable(int slot, uint8_t type, uint8_t origin, uint16_t originSeq, MpGen gen, const void* payload, uint16_t len) {
    uint8_t buf[MP_MAX_PAYLOAD + MP_GAME_PREFIX];
    if (len > MP_MAX_PAYLOAD) return false;
    buf[0] = (uint8_t)originSeq;
    buf[1] = (uint8_t)(originSeq >> 8);
    buf[2] = (uint8_t)(originSeq ? gen : 0);   // netcore: the origin's membership generation (DESIGN.md section 3, reliable game events)
    buf[3] = (uint8_t)(originSeq ? gen >> 8 : 0);
    if (len) memcpy(buf + MP_GAME_PREFIX, payload, len);
    return queueReliable(slot, type, origin, buf, (uint16_t)(len + MP_GAME_PREFIX));
}

// ---- core2: session crypto (DESIGN.md section 10) --------------------------------------------------------------------------
static bool s_keyRngFailed = false;   // (this key pair's own result; set by StrongRandom, read by makeKeys right after)
static uint32_t StrongRandom() {
    uint32_t v = 0;
    if (!mp_sys_random_try(&v)) s_keyRngFailed = true;
    return v;
}
static const uint8_t kZeroKey[32] = {0};

// The key pair: per (console, session) when the session is known (host; a join of a known room; a rejoin): BLAKE2b(key = the console
// key in mp.cfg, "SMG3 sk" | session id). A member that left and came back (or restarted) has the same public key, so a member that
// missed its rejoin (a stale roster) still derives the right pair key and a later host change works (core_tests' missed rejoin).
// Otherwise (the session is not known yet) a random one: that is a first membership, nobody holds an older key of it.
// host: the counter floor of a joiner whose key this host has seen in the session (the same console again: see makeKeys): above
// every counter of its earlier memberships, so none of their datagrams can be replayed into the new one. 0 for a new key.
uint8_t MpCore::ctrFloorFor(const uint8_t* pk) const {
    uint8_t f = 0;
    for (int s = 0; s < MP_MAX_PLAYERS; s++)
        if (s != m_localSlot && m_roster[s].gen && !memcmp(m_roster[s].pk, pk, 32)) {
            const uint8_t o = (uint8_t)(m_roster[s].gen & 0xFF);
            if (o >= f) f = (uint8_t)(o == 0xFF ? 0xFF : o + 1);
        }
    return f;
}

// core2 stage 4: SMG_MP_CRYPTO_LOG=1 (env.txt on the Vita): at each key making, one log line with what the crypto costs on this
// console (one pair key = X25519 + BLAKE2b; sealing and opening a 1200-byte datagram). Off by default: nothing is measured.
static void CryptoCostLog(const uint8_t* sk, const uint8_t* pk) {
    static int s_on = -1;
    if (s_on < 0) {
        const char* e = getenv("SMG_MP_CRYPTO_LOG");
        s_on = e && *e == '1';
    }
    if (!s_on) return;
    uint8_t k[32], zero[32] = {0};
    static uint8_t plain[1200], sealed[1200 + mpseal::kOverhead], out[1200 + mpseal::kOverhead];
    uint64_t t0 = mp_sys_now_us();
    mpseal::PairKey(sk, pk, pk, zero, 1, k);
    const uint64_t pairUs = mp_sys_now_us() - t0;
    t0 = mp_sys_now_us();
    size_t n = 0, pl = 0;
    for (int i = 0; i < 10; i++) n = mpseal::Seal(k, MP_PROTO_VERSION, 1, (uint64_t)i + 1, plain, sizeof plain, sealed, sizeof sealed);
    const uint64_t sealUs = mp_sys_now_us() - t0;
    t0 = mp_sys_now_us();
    for (int i = 0; i < 10; i++) mpseal::Open(k, sealed, n, out, &pl);
    const uint64_t openUs = mp_sys_now_us() - t0;
    mp_log("crypto cost: pair key %u us, 1200-byte datagram seal %.1f us, open %.1f us", NETFIX_U32C(pairUs), sealUs / 10.0, openUs / 10.0);
}

bool MpCore::makeKeys(uint32_t sid) {
    mpseal::KeyPair kp;
    if (sid) {
        uint8_t m[7 + 4];
        memcpy(m, "SMG3 sk", 7);
        memcpy(m + 7, &sid, 4);
        mpseal::Mac(kp.sk, 32, m_consoleKey, 16, m, sizeof m);
        mpseal::PublicKey(kp.pk, kp.sk);
    } else {
        s_keyRngFailed = false;
        mpseal::MakeKeyPair(&kp, StrongRandom);
        if (s_keyRngFailed) return false;   // smallfix: nothing is kept
    }
    memcpy(m_sk, kp.sk, 32);
    memcpy(m_pk, kp.pk, 32);
    CryptoCostLog(m_sk, m_pk);
    m_haveKp = true;
    memset(m_pair, 0, sizeof m_pair);
    return true;
}

// The rejoin token of this console in a session: BLAKE2b(key = the console key kept in mp.cfg, "SMG3 rejoin" | session id). The same
// console gets it back after a crash or a restart (the slot is its own again); nobody else can make it (the key never leaves the
// console; the token only travels masked under a pair key; the roster carries its hash).
void MpCore::makeToken(uint32_t sid) {
    uint8_t m[11 + 4];
    memcpy(m, "SMG3 rejoin", 11);
    memcpy(m + 11, &sid, 4);
    mpseal::Mac(m_token, 16, m_consoleKey, 16, m, sizeof m);
    m_haveToken = true;
    m_tokenSession = sid;
}

void MpCore::setRoomSecret(const uint8_t* secret, size_t len) {
    CoreLock l(*this);
    memset(m_psk, 0, sizeof m_psk);
    if (secret && len) mpseal::Mac(m_psk, 32, 0, 0, secret, len);   // (PSK = BLAKE2b(code secret))
    m_pskArmed = secret && len;
}

void MpCore::pairReset(int slot) {
    if (slot >= 0 && slot < MP_MAX_PLAYERS) memset(&m_pair[slot], 0, sizeof m_pair[slot]);
}

// The key of the pair (this console, slot): derived from the slot's public key in the roster (or the joiner's, the host's).
bool MpCore::keyFor(int slot) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS || slot == m_localSlot) return false;
    PairState& ps = m_pair[slot];
    if (ps.have) return true;
    if (!m_haveKp || !m_sessionId) return false;
    const uint8_t* pk = m_roster[slot].present ? m_roster[slot].pk : (m_peers[slot].used ? m_peers[slot].pk : 0);
    if (!pk || !memcmp(pk, kZeroKey, 32)) return false;
    if (!mpseal::PairKey(m_sk, m_pk, pk, pskOn() ? m_psk : kZeroKey, m_sessionId, ps.key)) return false;
    ps.have = true;
    ps.confirmed = false;
    ps.sendCtr = m_ctrBase + 1;
    ps.rxMax = ps.rxMask = 0;
    return true;
}

// host: the stateless cookie of a joiner (address, key, nonce, 10 s bucket): it must come back in the HELLO before any state is kept
void MpCore::cookieFor(const MpAddr& from, const uint8_t* pk, const uint8_t* nonce, uint64_t bucket, uint8_t out[16]) {
    uint8_t m[4 + 2 + 32 + 8 + 8];
    memcpy(m, &from.host, 4);
    memcpy(m + 4, &from.port, 2);
    memcpy(m + 6, pk, 32);
    memcpy(m + 38, nonce, 8);
    memcpy(m + 46, &bucket, 8);
    mpseal::Mac(out, 16, m_cookieSecret, 32, m, sizeof m);
}

// guest: the first HELLO (unreliable, unauthenticated): fingerprint, protocol, our key, our nonce, padding (so that the COOKIE
// that answers it is never larger: no amplification)
void MpCore::sendHello1(uint64_t t) {
    uint8_t b[4 + 2 + 32 + 8 + 24];
    memset(b, 0, sizeof b);
    MpWriter w(b, sizeof b);
    w.u32(m_fingerprint);
    w.u16(MP_PROTO_VERSION);
    w.bytes(m_pk, 32);
    w.bytes(m_nonce, 8);
    rawSend(m_hostAddr, MP_MSG_HELLO, 0xFF, 0, 0, 0, 0, b, sizeof b);
    m_hello1At = t;
}

void MpCore::handleHello1(const MpAddr& from, const uint8_t* hdr, const uint8_t* payload, uint16_t plen, uint64_t t) {
    MpReader r(payload, plen);
    const uint32_t fp = r.u32();
    const uint16_t proto = r.u16();
    uint8_t pk[32], nonce[8];
    r.bytes(pk, 32);
    r.bytes(nonce, 8);
    if (!r.ok()) { m_statStray++; return; }
    if (plen < 70) { m_statStray++; return; }   // (the padding: a cookie is never larger than what asked for it)
    if (m_phase == MP_UI_LEAVING) return;
    // corefix2: a cookie is stateless and cheap, so it spends no budget shared with anything else (a forged flood from varied addresses
    // used up the 40 a second that refusals and honest cookies shared, for as long as it lasted): its own allowance, bounded per second
    // (MP_HELLO1_PER_SEC unknown sources, MP_HELLO1_KNOWN_PER_SEC for a roster member / recent leaver, who never wait behind a stranger)
    {
        const uint64_t dt = m_h1At ? t - m_h1At : 0;
        m_h1At = t;
        const uint32_t capU = MP_HELLO1_PER_SEC * 1000u, capK = MP_HELLO1_KNOWN_PER_SEC * 1000u;
        m_h1Tokens = (uint32_t)(m_h1Tokens + dt * MP_HELLO1_PER_SEC > capU ? capU : m_h1Tokens + dt * MP_HELLO1_PER_SEC);
        m_h1KnownTokens = (uint32_t)(m_h1KnownTokens + dt * MP_HELLO1_KNOWN_PER_SEC > capK ? capK : m_h1KnownTokens + dt * MP_HELLO1_KNOWN_PER_SEC);
        if (vouchedAddr(from, t) && m_h1KnownTokens >= 1000u) m_h1KnownTokens -= 1000u;
        else if (m_h1Tokens >= 1000u) m_h1Tokens -= 1000u;
        else return;
    }
    if (proto != MP_PROTO_VERSION || hdr[2] != MP_PROTO_VERSION || fp != m_fingerprint) {
        MP_LOG_LIMITED("hello rejected reason=version (fingerprint %08x, ours %08x)", (unsigned)fp, (unsigned)m_fingerprint);
        uint8_t b[9];   // (the refusal is part of the cookie allowance, not of the refusal budget)
        b[0] = MP_REJECT_VERSION;
        memcpy(b + 1, nonce, 8);
        rawSend(from, MP_MSG_REJECT, (uint8_t)m_localSlot, 0, 0, 0, 0, b, 9);
        return;
    }
    uint8_t c[4 + 8 + 32 + 16 + 4 + 1 + 1 + 1];
    MpWriter w(c, sizeof c);
    w.bytes(nonce, 8);
    w.bytes(m_pk, 32);
    uint8_t ck[16];
    cookieFor(from, pk, nonce, t / 10000, ck);
    w.bytes(ck, 16);
    w.u32(m_sessionId);
    w.u8((uint8_t)m_localSlot);
    w.u8(pskOn() && memcmp(m_psk, kZeroKey, 32) != 0 ? 1 : 0);   // (the flag says "this room has a code secret": a plain by-address room, typed address, has none and is joinable without one)
    w.u8(ctrFloorFor(pk));   // (the joiner's counters start above this order byte)
    rawSend(from, MP_MSG_COOKIE, (uint8_t)m_localSlot, 0, 0, 0, 0, c, (uint16_t)w.n);
}

void MpCore::handleCookie(const MpAddr& from, const uint8_t* payload, uint16_t plen, uint64_t t) {
    if (m_role != ROLE_GUEST || m_phase != MP_UI_CONNECTING || m_haveCookie) return;
    MpReader r(payload, plen);
    uint8_t nonce[8], hpk[32], ck[16];
    r.bytes(nonce, 8);
    r.bytes(hpk, 32);
    r.bytes(ck, 16);
    const uint32_t sid = r.u32();
    const uint8_t hs = r.u8();
    const uint8_t flags = r.u8();
    const uint8_t floor = r.u8();
    if (!r.done() || memcmp(nonce, m_nonce, 8) != 0 || hs >= MP_MAX_PLAYERS || sid == 0 || (m_wantSession && m_wantSession != sid)) {
        m_statStray++;
        return;
    }
    if (m_hostAddr.host != MP_ADDR_BROADCAST && !(m_hostAddr == from)) { m_statStray++; return; }
    if ((flags & 1) && !memcmp(m_psk, kZeroKey, 32)) {
        requestEnd(MP_ERR_ROOM_NOT_FOUND, "the room needs its code");
        return;
    }
    // the host's real slot: the channel moves there (it was set up at the guess)
    if (hs != m_hostSlot) {
        m_peers[hs] = m_peers[m_hostSlot];
        memset(&m_peers[m_hostSlot], 0, sizeof m_peers[m_hostSlot]);
        m_hostSlot = hs;
    }
    m_hostAddr = from;
    m_peers[hs].addr = from;
    memcpy(m_peers[hs].pk, hpk, 32);
    memcpy(m_hostPk, hpk, 32);
    memcpy(m_cookie, ck, 16);
    m_sessionId = sid;   // (the key's session; WELCOME confirms it)
    m_ctrBase = CtrBase(floor);
    pairReset(hs);
    if (!keyFor(hs)) {
        requestEnd(MP_ERR_INTERNAL, "bad host key");
        return;
    }
    m_haveCookie = true;
    // HELLO (reliable, the channel's first message): as before + our key, nonce, the cookie, the proof that we hold the pair key (and so
    // the room's secret where it has one), our rejoin token (masked under the pair key) and its commitment
    uint8_t buf[200];
    MpWriter w(buf, sizeof buf);
    w.u32(m_fingerprint);
    w.u16(MP_PROTO_VERSION);
    PutName(w, m_name);
    w.u8(m_look);
    w.u8(m_wantMode);
    w.u32(m_wantSession);
    w.u32(m_wireId);
    w.u32(m_gid);
    w.bytes(m_pk, 32);
    w.bytes(m_nonce, 8);
    w.bytes(ck, 16);
    uint8_t pm[5 + 16 + 8], proof[16], mask[16], tok[16], commit[16];
    memcpy(pm, "hello", 5);
    memcpy(pm + 5, ck, 16);
    memcpy(pm + 21, m_nonce, 8);
    mpseal::Mac(proof, 16, m_pair[hs].key, 32, pm, sizeof pm);
    w.bytes(proof, 16);
    makeToken(sid);
    memcpy(pm, "tmask", 5);
    mpseal::Mac(mask, 16, m_pair[hs].key, 32, pm, sizeof pm);
    for (int i = 0; i < 16; i++) tok[i] = (uint8_t)(m_token[i] ^ mask[i]);
    w.bytes(tok, 16);
    mpseal::Mac(commit, 16, 0, 0, m_token, 16);
    w.bytes(commit, 16);
    queueReliable(hs, MP_MSG_HELLO, 0xFF, buf, (uint16_t)w.n);
    (void)t;
}

void MpCore::sendRejectNonce(const MpAddr& to, uint8_t reason, uint16_t seq, const uint8_t* nonce) {
    const uint64_t t = now();
    if (t - m_rejectWindow >= 1000) {
        m_rejectWindow = t;
        m_rejectCount = 0;
    }
    if (++m_rejectCount > 20) return;   // (never an amplifier)
    uint8_t b[9];
    b[0] = reason;
    memcpy(b + 1, nonce, 8);
    rawSend(to, MP_MSG_REJECT, (uint8_t)m_localSlot, 0, seq, 0, 0, b, 9);
}

// A plain (unsealed) datagram may only start a join: HELLO (both kinds), COOKIE, REJECT (to a joining guest, with its nonce: checked
// later). Everything else in a session is sealed: plain ones are dropped without a word and count against nobody.
bool MpCore::plainAllowed(const MpAddr& from, const uint8_t* data, size_t len) const {
    (void)from;
    if (len < MP_HEADER_BYTES || data[0] != 'G' || data[1] != 'M') return false;
    const uint8_t type = data[3];
    if (m_role == ROLE_HOST) return type == MP_MSG_HELLO;
    if (m_role == ROLE_GUEST) return m_phase == MP_UI_CONNECTING && (type == MP_MSG_COOKIE || type == MP_MSG_REJECT);
    return false;
}

// A sealed datagram: the key by the source address (a peer), else by the sender slot in its header (a member whose address moved, a
// new host announcing itself); open, check the replay window, move the peer to the new address if it moved, then handle what is inside
// as coming from that slot. Returns the number of packets inside (the receive budget), 1 when refused.
int MpCore::handleSealed(const MpAddr& from, const uint8_t* data, size_t len, uint64_t t) {
    if (len < mpseal::kOverhead + MP_HEADER_BYTES || len > MP_DATAGRAM_MAX + mpseal::kOverhead || data[2] != MP_PROTO_VERSION) {
        m_statStray++;
        return 1;
    }
    uint8_t plain[MP_DATAGRAM_MAX + 64];
    size_t pl = 0;
    int slot = -1;
    for (int s = 0; s < MP_MAX_PLAYERS && slot < 0; s++)
        if (m_peers[s].used && m_peers[s].addr == from && keyFor(s) && mpseal::Open(m_pair[s].key, data, len, plain, &pl)) slot = s;
    const uint8_t claimed = data[3];
    if (slot < 0 && claimed < MP_MAX_PLAYERS && !(m_peers[claimed].used && m_peers[claimed].addr == from) && keyFor(claimed) &&
        mpseal::Open(m_pair[claimed].key, data, len, plain, &pl))
        slot = claimed;
    if (slot < 0) {
        // a console this host removed: it is told so (sealed, its old key), once a second
        if (m_role == ROLE_HOST)
            for (int i = 0; i < MP_MAX_PLAYERS; i++) {
                Departed& d = m_departed[i];
                if (!d.used || t > d.until || !mpseal::Open(d.key, data, len, plain, &pl)) continue;
                if (t - d.lastBye >= 1000) {
                    d.lastBye = t;
                    uint8_t pk[MP_HEADER_BYTES + 1];
                    pk[0] = 'G'; pk[1] = 'M'; pk[2] = MP_PROTO_VERSION; pk[3] = MP_MSG_BYE; pk[4] = (uint8_t)m_localSlot; pk[5] = 0;
                    memset(pk + 6, 0, 6);
                    pk[MP_HEADER_BYTES] = MP_BYE_REMOVED;
                    uint8_t out[MP_HEADER_BYTES + 1 + mpseal::kOverhead];
                    const size_t n = mpseal::Seal(d.key, MP_PROTO_VERSION, (uint8_t)m_localSlot, kDepartCtrBase + ++m_departCtr, pk, sizeof pk, out,
                                                  sizeof out);
                    if (n && m_conn && m_conn->session()) txSend(from, out, n);
                }
                return 1;
            }
        m_statStray++;
        return 1;
    }
    PairState& ps = m_pair[slot];
    mpseal::Replay rp = {ps.rxMax, ps.rxMask};
    const uint64_t ctr = mpseal::Counter(data);
    if (!rp.check(ctr)) { m_statStray++; return 1; }
    rp.accept(ctr);
    ps.rxMax = rp.max;
    ps.rxMask = rp.mask;
    ps.confirmed = true;
    if (m_peers[slot].used && !(m_peers[slot].addr == from)) {
        // the member's address moved (NAT rebinding, a reconnect): only a datagram sealed with its key can do that
        mp_log("slot %d moved to another address (sealed)", slot);
        m_peers[slot].addr = from;
        if (m_role == ROLE_HOST) m_roster[slot].addr = from, m_rosterDirty = true;
        if (m_role == ROLE_GUEST && slot == m_hostSlot) m_hostAddr = from;
    }
    if (pl >= 2 && plain[0] == 'G' && plain[1] == 'S') { m_statStray++; return 1; }   // (never nested)
    const int prev = m_authSlot;
    m_authSlot = slot;
    const int n = handleDatagram(from, plain, pl, t);
    m_authSlot = prev;
    return n;
}

void MpCore::debugDeliverCore(int slot, uint8_t type, uint8_t about, const char* code, size_t n) {
    CoreLock l(*this);
    uint8_t b[32] = {0};
    b[0] = about;
    if (n > 30) n = 30;
    memcpy(b + 1, code, n);
    if (m_mem && slot >= 0 && slot < MP_MAX_PLAYERS && m_peers[slot].used) {
        const int prev = m_authSlot;
        m_authSlot = slot;
        deliver(slot, type, (uint8_t)slot, b, (uint16_t)(1 + n), true, 0);
        m_authSlot = prev;
    }
}

size_t MpCore::debugSealAs(int toSlot, const void* data, size_t len, uint8_t* out, size_t cap) {
    CoreLock l(*this);
    if (!keyFor(toSlot)) return 0;
    PairState& ps = m_pair[toSlot];
    return mpseal::Seal(ps.key, MP_PROTO_VERSION, (uint8_t)(m_localSlot >= 0 ? m_localSlot : 0xFF), ps.sendCtr++, (const uint8_t*)data, len, out,
                        cap);
}

void MpCore::debugReceiveAuthed(int slot, const void* data, size_t len) {
    CoreLock l(*this);
    if (m_phase == MP_UI_IDLE || !m_mem || m_dying || slot < 0 || slot >= MP_MAX_PLAYERS) return;
    const MpAddr from = m_peers[slot].used ? m_peers[slot].addr : MpAddr{0, 0};
    const int prev = m_authSlot;
    m_authSlot = slot;
    handleDatagram(from, (const uint8_t*)data, len, now());
    m_authSlot = prev;
    if (m_dying) finishEnd();
}

void MpCore::sendRejectRaw(const MpAddr& to, uint8_t reason, uint16_t seq) {
    const uint64_t t = now();
    if (t - m_rejectWindow >= 1000) {
        m_rejectWindow = t;
        m_rejectCount = 0;
    }
    if (++m_rejectCount > 20) return;   // (never an amplifier)
    rawSend(to, MP_MSG_REJECT, (uint8_t)m_localSlot, 0, seq, 0, 0, &reason, 1);
}

void MpCore::flushPeers(uint64_t t) {
    for (int s = 0; s < MP_MAX_PLAYERS && !m_dying; s++) {
        Peer& p = m_peers[s];
        if (!p.used) continue;
        EmitCtx ctx = {this, s};
        p.rel->drainHeld(deliverThunk, &ctx);   // core2: what waited for room in the game's queue
        if (m_dying || !p.used) continue;
        const uint32_t rto = p.srtt > 0 ? (uint32_t)p.srtt * 2u : 200u;
        p.rel->emitDue(t, rto, 32, MpCoreAccess::emit, &ctx);
        if (p.closing) {
            if (p.rel->everythingAcked() || t >= p.closeAt) {
                mp_log("slot %d closed", s);
                p.rel->reset(0);
                memset(&p, 0, sizeof p);
                if (!m_roster[s].present) pairReset(s);   // core2
            }
            continue;
        }
        const bool joined = m_phase == MP_UI_LOBBY || m_phase == MP_UI_PLAYING || m_phase == MP_UI_LEAVING;
        if (joined && p.rel->recvStarted() &&
            Since(t, p.lastSend) >= (uint64_t)MpNetTune::cur().keepAliveMs) {   // MP3 netpack: keep-awake. netopt: only a link with nothing else leaving (was: also every 100 ms)
            queueHeartbeat(s, t);
        }
        if (p.rel->ackDirty && !p.sentThisPump)
            sendToPeer(s, 0, (uint8_t)(m_localSlot < 0 ? 0xFF : m_localSlot), MP_HDR_ACK_ONLY, 0, 0, 0);
    }
    flushOutDue(t);
}

// ---- receiving -------------------------------------------------------------------------------------------------------------
void MpCore::bad(int slot, const char* why) {
    m_statBad++;
    if (m_authSlot < 0 || m_authSlot != slot) return;   // core2: only what that member sealed itself counts against it (review finding 3)
    if (slot < 0 || slot >= MP_MAX_PLAYERS || m_role != ROLE_HOST || slot == m_localSlot) return;
    Peer& p = m_peers[slot];
    if (!p.used || p.closing) return;
    const uint64_t t = now();
    if (t - p.badStart >= MP_BAD_WINDOW_MS) {
        p.badStart = t;
        p.badCount = 0;
    }
    if (++p.badCount == 1) mp_log("bad message from slot %d (%s)", slot, why);
    if (p.badCount > MP_BAD_KICK) {
        mp_log("kicked slot=%d reason=bad-messages", slot);
        banSlot(slot);   // corefix D2: a ban for the session, like uiKick (the flooder was back within a second, into the same slot)
        uint8_t r = MP_BYE_ERROR;
        queueReliable(slot, MP_MSG_BYE, (uint8_t)m_localSlot, &r, 1);
        removeSlot(slot, MP_BYE_ERROR, "bad messages");   // (the peer stays briefly as "closing" so the BYE goes out)
    }
}

// core2: the game side found a message of `slot` implausible (the world guard: rates, distances): it counts like a malformed one
// (MP_BAD_KICK in MP_BAD_WINDOW_MS removes the player). Host only: a guest's verdict on what the host relays would blame the host
// for another player's message.
void MpCore::reportBad(int slot, const char* why) {
    CoreLock l(*this);
    if (m_dying || !m_mem || m_role != ROLE_HOST || slot < 0 || slot >= MP_MAX_PLAYERS || slot == m_localSlot || !m_roster[slot].present) return;
    const int prev = m_authSlot;
    m_authSlot = slot;
    bad(slot, why && *why ? why : "game");
    m_authSlot = prev;
}

// MP3 netpack: a bundle (see flushOut) is checked as a whole first (untrusted: count, every length, no bytes left over), then each
// packet goes through the ordinary path, as if it had come alone (the same address, so the same peer, rate cap and bad counter).
// Returns the number of packets it carried (the receive budget counts packets).
int MpCore::handleDatagram(const MpAddr& from, const uint8_t* data, size_t len, uint64_t t) {
    // core2: sealed datagrams are opened first; a plain one may only start a join (plainAllowed), and never as a bundle
    if (len >= 2 && data[0] == 'G' && data[1] == 'S') {
        if (m_authSlot >= 0) { m_statStray++; return 1; }
        return handleSealed(from, data, len, t);
    }
    if (m_authSlot < 0 && !plainAllowed(from, data, len)) {
        m_statStray++;
        return 1;
    }
    if (len >= 2 && data[0] == 'G' && data[1] == 'B') {
        if (len < MP_BUNDLE_HEADER + 2 + MP_HEADER_BYTES || len > MP_DATAGRAM_MAX || data[2] != MP_PROTO_VERSION) {
            m_statStray++;
            return 1;
        }
        const unsigned count = data[3];
        if (count < 2 || count > MP_BUNDLE_MAX) {
            m_statStray++;
            return 1;
        }
        size_t off = MP_BUNDLE_HEADER;
        for (unsigned i = 0; i < count; i++) {
            if (len - off < 2) { m_statStray++; return 1; }
            const size_t n = (size_t)(data[off] | (data[off + 1] << 8));
            off += 2;
            if (n < MP_HEADER_BYTES || n > len - off || data[off] != 'G' || data[off + 1] != 'M') { m_statStray++; return 1; }
            off += n;
        }
        if (off != len) { m_statStray++; return 1; }
        // (framing only: each packet is then judged on its own, exactly as if it had come alone, so a bad one after good ones leaves
        // the good ones applied: the same as a lost datagram between them would, which every handler already tolerates)
        off = MP_BUNDLE_HEADER;
        for (unsigned i = 0; i < count && !m_dying; i++) {
            const size_t n = (size_t)(data[off] | (data[off + 1] << 8));
            handleDatagramOne(from, data + off + 2, n, t);
            off += 2 + n;
        }
        return (int)count;
    }
    handleDatagramOne(from, data, len, t);
    return 1;
}

void MpCore::handleDatagramOne(const MpAddr& from, const uint8_t* data, size_t len, uint64_t t) {
    if (len < MP_HEADER_BYTES || len > MP_DATAGRAM_MAX || data[0] != 'G' || data[1] != 'M') {
        m_statStray++;
        return;
    }
    const uint8_t type = data[3];
    const uint8_t flags = data[5];
    const uint16_t seq = (uint16_t)(data[6] | (data[7] << 8));
    const uint8_t* payload = data + MP_HEADER_BYTES;
    const uint16_t plen = (uint16_t)(len - MP_HEADER_BYTES);
    int slot = -1;
    if (m_authSlot >= 0 && m_peers[m_authSlot].used) slot = m_authSlot;   // core2: the key says who it is
    for (int s = 0; s < MP_MAX_PLAYERS && slot < 0; s++)
        if (m_peers[s].used && m_peers[s].addr == from) { slot = s; break; }

    if (slot < 0 && m_starved && m_role == ROLE_HOST && type != MP_MSG_HELLO) {   // corefix2: a guest a takeover has no window for talks to us
        for (int s = 0; s < MP_MAX_PLAYERS; s++)
            if ((m_starved >> s & 1) && m_roster[s].present && m_roster[s].addr == from) {
                if (grantWindow(s, t)) return;   // (this datagram is dropped: the guest re-sends, and the claim now goes out to it)
                break;
            }
    }
    if (m_role == ROLE_HOST) {
        if (m_phase != MP_UI_LOBBY && m_phase != MP_UI_PLAYING && m_phase != MP_UI_LEAVING) return;
        if (type == MP_MSG_HELLO && !(flags & MP_HDR_RELIABLE) && m_authSlot < 0) {   // core2: a joiner's first HELLO: a cookie, no state
            handleHello1(from, data, payload, plen, t);
            return;
        }
        if (slot >= 0) {
            Peer& p = m_peers[slot];
            if (type == MP_MSG_HELLO && (flags & MP_HDR_RELIABLE) && m_authSlot < 0) {
                // core2: a plain HELLO from a member's address: a new one is checked like any join first (handleHelloPacket removes the old
                // session at this address only once the cookie and the proof hold); a repeat of the one that started the channel is
                // dropped (plain: its acks prove nothing; the sealed WELCOME acks it)
                if (seq != p.helloSeq && p.resumed && !p.closing) handleHelloPacket(from, data, payload, plen, t);
                return;
            }
            if (type == MP_MSG_HOST_CHANGE && (flags & MP_HDR_RELIABLE) && !p.resumed) {
                // review: a second claimant of this epoch (becomeHost made every present slot our peer): its claim is not an
                // in-channel message (that channel only starts with a RESUME); the lower slot wins, as for a guest
                handleHostChangePacket(from, data, payload, plen, t);
                return;
            }
            handleFromPeer(slot, data, payload, plen, t);
        } else if (type == MP_MSG_HELLO && (flags & MP_HDR_RELIABLE)) {
            handleHelloPacket(from, data, payload, plen, t);
        } else if (type == MP_MSG_HOST_CHANGE && (flags & MP_HDR_RELIABLE)) {
            handleHostChangePacket(from, data, payload, plen, t);
        } else {
            m_statStray++;   // (core2: a console this host removed is told so, sealed, by handleSealed's departed list)
        }
        return;
    }
    if (m_role != ROLE_GUEST) return;
    if (type == MP_MSG_COOKIE && m_authSlot < 0) {   // core2: the host's answer to our first HELLO
        handleCookie(from, payload, plen, t);
        return;
    }
    if (slot < 0 && m_phase == MP_UI_CONNECTING && (type == MP_MSG_WELCOME || type == MP_MSG_REJECT) && m_hostSlot >= 0 &&
        m_peers[m_hostSlot].used && (m_hostAddr.host == MP_ADDR_BROADCAST || m_hostAddr == from)) {
        // the first answer to a broadcast HELLO tells where the host is
        m_hostAddr = from;
        m_peers[m_hostSlot].addr = from;
        slot = m_hostSlot;
    }
    if (slot >= 0) {
        if (type == MP_MSG_REJECT && !(flags & MP_HDR_RELIABLE)) {
            if (m_phase == MP_UI_CONNECTING) {
                // only the answer to THIS attempt's HELLO (the host echoes its sequence): a stray CLOSED sent to our address for packets
                // of a session we just left must not abort the attempt (the host went on to accept the HELLO: a phantom member)
                // core2: and it echoes this attempt's nonce (an unauthenticated refusal: only one that saw our HELLO can make it)
                if (plen != 9 || memcmp(payload + 1, m_nonce, 8) != 0 || (seq && seq != m_peers[slot].helloSeq)) { m_statStray++; return; }
                handleCore(slot, MP_MSG_REJECT, data[4], payload, 1);
            }
            // core2: never in a session: an unauthenticated REJECT(CLOSED) with the host's address ended it (review finding 2). A
            // guest the host removed finds out by silence (and, with keys, by a sealed goodbye)
            return;
        }
        handleFromPeer(slot, data, payload, plen, t);
    } else if (type == MP_MSG_HOST_CHANGE && (flags & MP_HDR_RELIABLE)) {
        handleHostChangePacket(from, data, payload, plen, t);
    } else {
        m_statStray++;
    }
}

void MpCore::handleFromPeer(int slot, const uint8_t* hdr, const uint8_t* payload, uint16_t plen, uint64_t t) {
    Peer& p = m_peers[slot];
    const uint8_t proto = hdr[2];
    const uint8_t type = hdr[3];
    const uint8_t from = hdr[4];
    const uint8_t flags = hdr[5];
    const uint16_t seq = (uint16_t)(hdr[6] | (hdr[7] << 8));
    const uint16_t ack = (uint16_t)(hdr[8] | (hdr[9] << 8));
    const uint16_t bits = (uint16_t)(hdr[10] | (hdr[11] << 8));
    if (proto != MP_PROTO_VERSION) {
        bad(slot, "protocol");
        return;
    }
    // MP3 netpack: acknowledgements (and corefix's liveness / ack progress) come first: a console at its rate cap still gets its acks
    // counted (else they come back as resends)
    // corefix: only a packet that carries an acknowledgement or a reliable message shows a live peer (a bare unreliable datagram from
    // an address costs nothing to forge); and whatever we sent must be acknowledged (ack progress, see pumpTimers)
    if (flags & (MP_HDR_ACK | MP_HDR_RELIABLE)) p.lastRecv = t;
    if (flags & MP_HDR_BUSY) p.busyAt = t ? t : 1;
    if (flags & MP_HDR_ACK) {
        const int waiting = p.rel->unacked();
        const int sample = p.rel->onAck(ack, bits, t);
        if (sample >= 0 && sample < 10000) { p.srtt = p.srtt < 0 ? sample : (p.srtt * 7 + sample) / 8; rttNote(p, (uint32_t)sample); }
        if (p.rel->unacked() < waiting || p.rel->unacked() == 0) p.ackAt = t;
        if (p.rel->unacked() < waiting) p.established = true;   // netcore
    }
    if (m_role == ROLE_HOST && slot != m_localSlot) {
        if (t - p.rateStart >= 1000) {
            p.rateStart = t;
            p.rateCount = 0;
        }
        // core2: during a grace period (a takeover's re-send burst, a stalled pump) the cap is doubled, not lifted, and acks alone
        // do not count (review finding 7: the cap was off then)
        // netfix B2: while our own back-pressure gate refuses its head message (our game does not poll), the sender can only re-send its
        // window: that is no flood. A refusal gives it x4 for MP_GATE_GRACE_MS (a real flooder is still stopped above 4x the cap)
        const bool grace = t < m_rateGraceUntil;
        const bool refused = t < p.gateRefusedUntil;
        const uint32_t cap = refused ? 4 * MP_RATE_CAP : (grace ? 2 * MP_RATE_CAP : MP_RATE_CAP);
        if (!(grace && (flags & MP_HDR_ACK_ONLY)) && ++p.rateCount > cap) {
            bad(slot, "rate");
            return;
        }
    }
    if (p.closing) return;
    if (flags & MP_HDR_ACK_ONLY) {
        if (type != 0 || plen != 0) bad(slot, "ack-only with data");
        return;
    }
    // the header's from-slot: a guest speaks for itself; the host for itself or (relays) for another present player
    if (m_role == ROLE_HOST) {
        // (corefix 4: a guest may re-send what the dead host relayed to it: from = the dead host's slot, reliable relay types only)
        // (netcore: any relay type re-sent for another slot passes the channel, so it is acknowledged and the channel goes on; deliver()
        // keeps only a dead host's: a follower's view of who is gone can be older than this host's)
        const bool ghostRelay = from < MP_MAX_PLAYERS && (flags & MP_HDR_RELIABLE) && IsGame(type) && mp_msg_host_relays(type);
        if (type != MP_MSG_HELLO && from != slot && !ghostRelay) {
            bad(slot, "from-slot");
            return;
        }
    } else if (from >= MP_MAX_PLAYERS) {
        if (!(type == MP_MSG_REJECT && m_phase == MP_UI_CONNECTING)) {
            bad(slot, "from-slot");
            return;
        }
    }
    if (flags & MP_HDR_RELIABLE) {
        if (!p.rel->recvStarted()) {
            // a channel starts with its first message: HELLO (host side), WELCOME / REJECT (joining guest), RESUME (new host)
            // (a joining guest only starts its channel on an answer that acknowledges this attempt's HELLO: a retransmitted WELCOME of an
            // earlier attempt, accepted by the host before it saw the guest start over, would pin the guest to a channel the host has
            // reset: no ROSTER or event would ever arrive again while heartbeats keep the guest looking alive)
            const bool answersHello = (flags & MP_HDR_ACK) && (uint16_t)(ack - p.helloSeq) < 8;
            const bool start = (m_role == ROLE_HOST && ((type == MP_MSG_HELLO && seq == p.helloSeq) || type == MP_MSG_RESUME)) ||
                               (m_role == ROLE_GUEST && m_phase == MP_UI_CONNECTING && (type == MP_MSG_WELCOME || type == MP_MSG_REJECT) && answersHello);
            if (!start) return;
            p.rel->startRecv(seq);
        }
        MpRelMsg m;
        m.seq = seq;
        m.type = type;
        m.from = from;
        m.len = plen;
        m.data = payload;
        EmitCtx ctx = {this, slot};
        p.rel->receive(m, deliverThunk, &ctx);
    } else {
        deliver(slot, type, from, payload, plen, false, seq);
    }
}

// core2: a reliable game message is taken from the channel only when the game's receive queue has room for it (its origin's budget
// and the queue): otherwise it is not acknowledged and comes again (the sender's backlog holds it). So a burst after a freeze, or a
// guest flooding while the host's game loads, waits instead of ending anything (review finding 7, claims audit 1).
bool MpCore::gateThunk(void* c, const MpRelMsg& m) {
    EmitCtx* e = (EmitCtx*)c;
    return e->core->roomFor(e->slot, m);
}
bool MpCore::roomFor(int slot, const MpRelMsg& m) {
    if (!IsGame(m.type)) return true;
    const int origin = m_role == ROLE_GUEST ? m.from : (m.from < MP_MAX_PLAYERS && m.from != slot ? m.from : slot);
    // netfix B1: only entries that cannot be evicted count against the queue (an unreliable pose is dropped by enqueueGame when the
    // queue is full: a console whose game does not poll for seconds fills 256 entries with poses, and refusing every reliable message
    // for that removed a healthy player after MP_PEER_LOST_MS); and a message that needs a big block needs a free one (enqueueGame
    // answers 2 then, which ended the session / removed the sender)
    bool room = true;
    {
        MpLock ql(m_qMutex);
        if (!m_mem) return true;
        int fixed = 0, mine = 0;
        const bool haveOrigin = origin >= 0 && origin < MP_MAX_PLAYERS;
        for (int i = 0; i < m_mem->qCount; i++) {
            const QEntry& q = m_mem->q[m_mem->qRing[(m_mem->qHead + i) % MP_QUEUE_SIZE]];
            if (!q.reliable) continue;
            fixed++;
            if (haveOrigin && q.fromSlot == origin) mine++;
        }
        room = fixed < MP_QUEUE_SIZE - 1 && (!haveOrigin || mine < MP_QUEUE_ORIGIN);
        if (room && m.len > MP_QINLINE) {
            room = false;
            for (int i = 0; i < MP_QBIG; i++)
                if (!m_mem->bigUsed[i]) { room = true; break; }
        }
    }
    if (!room && slot >= 0 && slot < MP_MAX_PLAYERS) m_peers[slot].gateRefusedUntil = now() + MP_GATE_GRACE_MS;   // netfix B2
    return room;
}

void MpCore::deliverThunk(void* c, const MpRelMsg& m) {
    EmitCtx* e = (EmitCtx*)c;
    MpCore* core = e->core;
    // a guest has one channel (to the host); a WELCOME may have moved it to the host's real slot while held messages drain
    const int slot = core->m_role == ROLE_GUEST && core->m_hostSlot >= 0 ? core->m_hostSlot : e->slot;
    core->deliver(slot, m.type, m.from, m.data, m.len, true, m.seq);
}

void MpCore::deliver(int slot, uint8_t type, uint8_t from, const uint8_t* data, uint16_t len, bool reliable, uint16_t seq) {
    if (m_dying || !m_peers[slot].used) return;
    if (type < MP_MSG_GAME_FIRST) {
        if (!reliable && type == MP_MSG_BYE && m_role == ROLE_GUEST && slot == m_hostSlot && m_authSlot == slot && len == 1 &&
            data[0] == MP_BYE_REMOVED && m_phase != MP_UI_LEAVING) {
            // core2: the host removed this console (it took it for gone) and says so, sealed with the key it had: rejoin at once
            mp_log("host says this console was removed");
            requestEnd(MP_ERR_TIMEOUT, "removed by the host", true);
            return;
        }
        if (!reliable && type != MP_MSG_HEARTBEAT) {
            bad(slot, "unreliable core message");
            return;
        }
        handleCore(slot, type, from, data, len);
        return;
    }
    if (!IsGame(type)) {
        bad(slot, "unknown type");
        return;
    }
    // core2 (review finding 4): only poses travel unreliably; any other game message without the reliable channel is dropped (a
    // forged unreliable MOVE / STAR reached the game and was relayed). Not counted: it may not come from the peer at all.
    if (!reliable && type != MP_MSG_STATE && type != MP_MSG_POINTER) {
        m_statStray++;
        return;
    }
    uint8_t origin = (uint8_t)slot;
    if (m_role == ROLE_HOST && from != slot) {
        // a guest re-sends another slot's events only after a host change (corefix 4: a dead host's); netcore: one it took for dead but
        // that is here (a claimant that stepped down), or this host's own: they arrive through the reliable channel and go no further
        if (from < MP_MAX_PLAYERS && from != m_localSlot && (m_ghostMask & (1u << from)) && !m_roster[from].present) origin = from;
        else {
            m_statStray++;
            return;
        }
    }
    uint16_t useq = seq;
    MpGen gen = 0;
    if (reliable) {
        if (len < MP_GAME_PREFIX) {
            bad(slot, "short");
            return;
        }
        useq = (uint16_t)(data[0] | (data[1] << 8));
        gen = (MpGen)(data[2] | (data[3] << 8));
        data += MP_GAME_PREFIX;
        len = (uint16_t)(len - MP_GAME_PREFIX);
    }
    if (m_role == ROLE_GUEST) {
        origin = from;
        if (origin != m_hostSlot) {
            // relayed: only relay types, from another player: a present one, or (a tracked event: the host serves the history of
            // players who left / a dead host, so a console that missed their last events gets them; netcore) any other slot
            const bool ghost = origin < MP_MAX_PLAYERS && (m_ghostMask & (1u << origin));
            const bool tracked = reliable && useq != 0;
            if (!mp_msg_host_relays(type) || origin >= MP_MAX_PLAYERS || origin == m_localSlot || (!m_roster[origin].present && !ghost && !tracked)) {
                bad(slot, "relay");
                return;
            }
        }
    }
    handleGame(slot, type, origin, data, len, reliable, useq, gen);
    if (m_role == ROLE_HOST && reliable && useq != 0 && origin != slot) unpark(origin);
}

// netcore (new host): see Parked. Kept until the events before it arrive; dropped when it is no longer next, the slot's next
// membership has begun, the park is full (then this one is lost like an event the dead host took with it) or the scene changes.
void MpCore::park(int from, uint8_t origin, MpGen gen, uint16_t seq, uint8_t type, const uint8_t* data, uint16_t len) {
    if (!m_mem || len > MP_HIST_DATA) return;
    int freeAt = -1;
    for (int i = 0; i < MP_PARK_ENTRIES; i++) {
        Parked& p = m_mem->park[i];
        if (!p.used) { if (freeAt < 0) freeAt = i; continue; }
        if (p.origin == origin && p.gen == gen && p.seq == seq) return;   // (another follower re-sent it too)
    }
    if (freeAt < 0) {
        MP_LOG_LIMITED("re-sent events of slot %d: no room to hold seq %u until the ones before it come", origin, (unsigned)seq);
        return;
    }
    Parked& p = m_mem->park[freeAt];
    p.used = true;
    p.from = (uint8_t)from;
    p.origin = origin;
    p.gen = gen;
    p.type = type;
    p.seq = seq;
    p.len = len;
    if (len) memcpy(p.data, data, len);
}

void MpCore::unpark(uint8_t origin) {
    if (!m_mem) return;
    for (int round = 0; round < MP_PARK_ENTRIES && !m_dying; round++) {
        int next = -1;
        for (int i = 0; i < MP_PARK_ENTRIES; i++) {
            Parked& p = m_mem->park[i];
            if (!p.used || p.origin != origin) continue;
            if (p.gen != m_seenGen[origin] || SeqDiff(p.seq, m_seen[origin]) <= 0) { p.used = false; continue; }   // (stale)
            if (SeqDiff(p.seq, m_seen[origin]) == 1) next = i;
        }
        if (next < 0) return;
        Parked p = m_mem->park[next];
        m_mem->park[next].used = false;
        handleGame(p.from, p.type, p.origin, p.data, p.len, true, p.seq, p.gen);   // (also when its sender has gone since: the event is valid)
    }
}

// netopt item 8: the host does not relay a pose to a console that is in another scene. A pose carries its scene serial (bytes 4..5)
// and the receiver drops one that is not its own (MpPuppets::handleState) before anything reads it (the last pose, the edge heads, the
// "where is my friend" arrows, spectate and Go to a Friend all read what handleState kept), so those poses were pure cost: a datagram
// slot each, at 30 Hz per pair. The host knows a guest's serial from the guest's own poses. When it does not (no pose yet) or the
// last one is older than MP_STATE_SERIAL_STALE_MS (the console stopped sending: a load, a menu), it relays as before.
void MpCore::noteStateSerial(int slot, uint16_t serial) {
    m_stateSerial[slot] = serial;
    m_stateSerialAt[slot] = now();
    if (!m_stateSerialAt[slot]) m_stateSerialAt[slot] = 1;
}

bool MpCore::stateWanted(int slot, uint16_t serial) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS || !m_stateSerialAt[slot]) return true;
    if (Since(now(), m_stateSerialAt[slot]) > MP_STATE_SERIAL_STALE_MS) return true;
    return m_stateSerial[slot] == serial;
}

void MpCore::handleGame(int slot, uint8_t type, uint8_t origin, const uint8_t* data, uint16_t len, bool reliable, uint16_t s, MpGen gen) {
    const uint16_t maxLen = GameMaxLen(type);
    if (maxLen == 0 || len > maxLen) {
        bad(slot, "length");
        return;
    }
    if (m_role == ROLE_HOST && origin != slot && s == 0) {   // corefix 4: a dead host's event re-sent by a guest is always a tracked one
        bad(slot, "untracked relay of a dead host");
        return;
    }
    if (type == MP_MSG_LOOK && m_role == ROLE_HOST) {
        if (reliable && len == 1 && s == 0) {   // a guest asks for another character (core request, not a game message)
            handleLookRequest(origin, data[0]);
            return;
        }
        if (len >= 1 && data[0] != m_roster[origin].look) {
            // the puppet's LOOK carries the character too: it is always the one the host assigned
            uint8_t fixed[16];
            memcpy(fixed, data, len);
            fixed[0] = m_roster[origin].look;
            handleGame(slot, type, origin, fixed, len, reliable, s, gen);
            return;
        }
    }
    if (mp_msg_host_only(type)) {
        if (m_role == ROLE_HOST || origin != m_hostSlot) {
            bad(slot, "host-only type");
            return;
        }
    }
    if (reliable) {
        if (s != 0) {
            if (!mp_msg_host_relays(type)) {
                bad(slot, "tracked non-relay");
                return;
            }
            // netcore: the event is (origin slot, gen, seq). A newer generation is the slot's next membership: its stream starts
            // over; an older one is a membership that is over (a stale catch-up or re-send): dropped. Within one generation the
            // sequence dedupes (relay + re-send after a host migration) and orders.
            if (gen == 0) {
                bad(slot, "tracked event without a generation");
                return;
            }
            if (m_role == ROLE_HOST && gen != m_roster[origin].gen) {
                // a console speaks for its own membership (the one this host admitted, or learnt at its RESUME), and a re-sent event of a
                // dead host for the membership the roster knows: another generation would let it restart or replay a stream
                m_statStray++;
                return;
            }
            const int gd = !m_seenGen[origin] ? 1 : gen == m_seenGen[origin] ? 0 : GenNewer(gen, m_seenGen[origin]) ? 1 : -1;
            if (gd < 0) {
                m_statStale++;
                return;
            }
            if (gd > 0) {
                m_seenGen[origin] = gen;
                m_seen[origin] = 0;
                noteGen(origin, gen);
            }
            const int16_t d = SeqDiff(s, m_seen[origin]);
            if (d <= 0) return;   // already delivered
            if (d > 1 && m_role == ROLE_HOST && origin != slot) {
                park(slot, origin, gen, s, type, data, len);
                return;
            }
            if (d > 1) {
                m_statGaps++;
                MP_LOG_LIMITED("events lost from slot %d: %d (gen %u seq %u -> %u)", origin, d - 1, (unsigned)gen, (unsigned)m_seen[origin], (unsigned)s);
            }
            m_seen[origin] = s;
            historyAdd(origin, s, gen, type, data, len);
        }
    } else {
        const int ti = type - MP_MSG_GAME_FIRST;
        const uint64_t tnow = now();
        if (m_haveU[origin] & (1ull << ti)) {
            const int16_t d = SeqDiff(s, m_lastU[origin][ti]);
            if (d <= 0) return;   // older than what we have: latest wins
            // core2 (review finding 5): a sequence far ahead (a forged seq + 32000 froze the puppet for 32,000 poses) is refused while the
            // origin's stream is live; after MP_USEQ_STALE_MS of silence (a long gap, a rejoin) any next one is taken
            if (d > MP_USEQ_WINDOW && Since(tnow, m_lastUAt[origin]) < MP_USEQ_STALE_MS) {
                m_statStray++;
                return;
            }
        }
        m_haveU[origin] |= 1ull << ti;
        m_lastU[origin][ti] = s;
        m_lastUAt[origin] = tnow;
    }
    // MP3 chat: the host decides which CHATs its game shows and relays (typed text never in a public room, rate limit)
    if (type == MP_MSG_CHAT && m_role == ROLE_HOST && !chatRelayOk(slot, origin, data, len, reliable)) return;
    // group-restart FIFO gate (host, Play Together): reliable messages from each guest arrive strictly in send order.
    // Any KILL/PURPLE/CHIP of m_restartSerial arriving before that guest's post-restart REGCRC was sent pre-restart -> drop before delivery and relay.
    if (m_role == ROLE_HOST && reliable && m_restartSerial != 0 && origin < MP_MAX_PLAYERS && origin != m_localSlot) {
        const uint8_t bit = (uint8_t)(1u << origin);
        if (type == MP_MSG_REGCRC && len >= 2 && ((m_restartGateMask | m_restartMissedMask) & bit)) {
            const uint16_t mser = (uint16_t)(data[0] | (data[1] << 8));
            if (mser == m_restartSerial) {
                m_restartGateMask &= (uint8_t)~bit;
                m_restartMissedMask &= (uint8_t)~bit;
                m_restartGateUntil[origin] = 0;
            }
        } else if ((type == MP_MSG_KILL || type == MP_MSG_PURPLE || type == MP_MSG_CHIP) && len >= 2 && (m_restartGateMask & bit)) {
            const uint64_t tnow = now();
            if (m_restartGateUntil[origin] && tnow >= m_restartGateUntil[origin]) {
                m_restartGateMask &= (uint8_t)~bit;
                m_restartGateUntil[origin] = 0;
            } else {
                const uint16_t mser = (uint16_t)(data[0] | (data[1] << 8));
                if (mser == m_restartSerial) {
                    if (s && m_mem && m_mem->hCount > 0) {
                        HistEntry& last = m_mem->hist[(m_mem->hHead + m_mem->hCount - 1) % MP_HISTORY_ENTRIES];
                        if (last.origin == origin && last.gen == gen && last.seq == s) last.suppressed = true;
                    }
                    MP_LOG_LIMITED("core restart-gate drop type=%02x origin=%d serial=%u", (unsigned)type, (int)origin, (unsigned)mser);
                    return;
                }
            }
        }
    }
    if (m_role == ROLE_GUEST && origin == m_hostSlot) snoopWhere(type, data, len);
    if (const int qr = enqueueGame(type, origin, data, len, reliable, reliable && s != 0)) {
        // core2 (review finding 7): never the whole session. The host removes the guest that filled the queue (origin over its budget,
        // or the most queued origin when full); a guest whose own game fell that far behind rejoins (a fresh membership: its state
        // comes from the snapshot / WHERE, not from the events it could not hold)
        int offender = origin;
        if (qr == 1) {
            MpLock ql(m_qMutex);
            if (m_mem) {
                int most = -1, cnt[MP_MAX_PLAYERS] = {0, 0, 0, 0};
                for (int i = 0; i < m_mem->qCount; i++) {
                    const QEntry& e = m_mem->q[m_mem->qRing[(m_mem->qHead + i) % MP_QUEUE_SIZE]];
                    if (e.reliable && e.fromSlot < MP_MAX_PLAYERS) cnt[e.fromSlot]++;
                }
                for (int k = 0; k < MP_MAX_PLAYERS; k++)
                    if (most < 0 || cnt[k] > cnt[most]) most = k;
                if (most >= 0) offender = most;
            }
        }
        MP_LOG_LIMITED("receive queue: slot %d has too many messages waiting for the game (%s)", offender, qr == 2 ? "its budget" : "queue full");
        if (m_role == ROLE_HOST) {
            if (offender != m_localSlot && offender >= 0 && m_roster[offender].present && m_peers[offender].used) {
                uint8_t r = MP_BYE_ERROR;
                queueReliable(offender, MP_MSG_BYE, (uint8_t)m_localSlot, &r, 1);
                removeSlot(offender, MP_BYE_ERROR, "receive queue");
            }
        } else {
            requestEnd(MP_ERR_TIMEOUT, "receive queue overflow", true);
        }
        return;
    }
    if (m_role == ROLE_HOST && mp_msg_host_relays(type)) {
        const bool isState = type == MP_MSG_STATE && !reliable && len >= 6;
        const uint16_t stateSerial = isState ? (uint16_t)(data[4] | (data[5] << 8)) : (uint16_t)0;
        if (isState && origin < MP_MAX_PLAYERS && origin != m_localSlot) noteStateSerial(origin, stateSerial);
        for (int q = 0; q < MP_MAX_PLAYERS; q++) {
            if (q == origin || q == m_localSlot) continue;
            Peer& p = m_peers[q];
            if (!p.used || p.closing || !p.resumed || !m_roster[q].present) continue;
            if (isState && !stateWanted(q, stateSerial)) {
                m_statStateSkipped++;
                continue;
            }
            if (reliable) queueGameReliable(q, type, origin, s, gen, data, len);
            else sendToPeer(q, type, origin, 0, s, data, len);
        }
    }
}

// MP3 chat: a public room (opened for anyone by this console, or marked public by the original host: MpSettings.flags)

// MP3 chat (host): show and relay this CHAT? Malformed ones count as bad messages; typed text in a room without typed messages and
// messages over the rate limit are dropped quietly (a log line, throttled). Under m_mutex.
bool MpCore::chatRelayOk(int slot, uint8_t origin, const uint8_t* data, uint16_t len, bool reliable) {
    if (origin >= MP_MAX_PLAYERS) return false;
    MpChatMsg cm;
    const uint8_t v = mp_chat_host_check(data, len, reliable, &m_settings, false, &m_chatBucket[origin], now(), &cm);   // (owner, 11 Nov: typed messages in every kind of game)
    if (v == MP_CHAT_OK) return true;
    if (v == MP_CHAT_BAD) {
        bad(slot, "chat");
        return false;
    }
    // (typed text where the room takes none: every time; the rate limit: the first ones, then every 32nd)
    const bool rate = v == MP_CHAT_RATE;
    if (rate) m_chatDrops++;
    if (!rate || m_chatDrops <= 8 || (m_chatDrops % 32) == 0)
        MP_LOG_LIMITED_N(64, "chat drop slot=%d kind=%s reason=%s (rate drops %u)", origin, cm.kind == MP_CHAT_TEXT ? "text" : "phrase", mp_chat_verdict_name(v),
               (unsigned)m_chatDrops);
    return false;
}

// ---- the game's receive queue (caller holds m_qMutex) -----------------------------------------------------------------------
void MpCore::queueRemoveAt(int pos) {
    MpCoreMem& m = *m_mem;
    const uint16_t idx = m.qRing[(m.qHead + pos) % MP_QUEUE_SIZE];
    if (m.q[idx].big >= 0) m.bigUsed[m.q[idx].big] = false;
    for (int i = pos; i + 1 < m.qCount; i++)
        m.qRing[(m.qHead + i) % MP_QUEUE_SIZE] = m.qRing[(m.qHead + i + 1) % MP_QUEUE_SIZE];
    m.qCount--;
    m.qFree[m.qFreeCount++] = idx;
    __atomic_store_n(&m_qPub, (uint32_t)m.qCount, __ATOMIC_RELEASE);
}

int MpCore::enqueueGame(uint8_t type, uint8_t from, const uint8_t* data, uint16_t len, bool reliable, bool tracked) {
    // a pose from a scene this console has already left is of no use (loads: DESIGN.md request from flow)
    if (type == MP_MSG_STATE && len >= 6 && m_serial && SeqDiff((uint16_t)(data[4] | (data[5] << 8)), m_serial) < 0) return 0;
    MpLock ql(m_qMutex);
    if (!m_mem) return 0;
    MpCoreMem& m = *m_mem;
    if (reliable && from < MP_MAX_PLAYERS) {   // core2: one origin cannot hold the whole queue (review finding 7)
        int mine = 0;
        for (int i = 0; i < m.qCount; i++) {
            const QEntry& q = m.q[m.qRing[(m.qHead + i) % MP_QUEUE_SIZE]];
            if (q.reliable && q.fromSlot == from) mine++;
        }
        if (mine >= MP_QUEUE_ORIGIN) return 2;
    }
    if (m.qCount >= MP_QUEUE_SIZE) {
        // full: the oldest unreliable message goes; a reliable one is never dropped silently
        int victim = -1;
        for (int i = 0; i < m.qCount; i++)
            if (!m.q[m.qRing[(m.qHead + i) % MP_QUEUE_SIZE]].reliable) { victim = i; break; }
        if (victim < 0) {
            if (!reliable) return 0;   // (the new unreliable one is dropped instead)
            return 1;
        }
        queueRemoveAt(victim);
    }
    int big = -1;
    if (len > MP_QINLINE) {
        if (len > MP_QBIG_BYTES) return 0;   // (GameMaxLen allows nothing longer)
        for (int i = 0; i < MP_QBIG && big < 0; i++)
            if (!m.bigUsed[i]) big = i;
        if (big < 0) {   // every big block waits for the game: a reliable message cannot wait elsewhere, an unreliable one is dropped
            if (!reliable) return 0;
            return 2;
        }
        m.bigUsed[big] = true;
    }
    const uint16_t idx = m.qFree[--m.qFreeCount];
    QEntry& e = m.q[idx];
    e.reliable = reliable;
    e.tracked = tracked;
    e.type = type;
    e.fromSlot = from;
    e.len = len;
    e.big = (int8_t)big;
    if (len) memcpy(big >= 0 ? m.bigData[big] : e.data, data, len);
    m.qRing[(m.qHead + m.qCount) % MP_QUEUE_SIZE] = idx;
    m.qCount++;
    __atomic_store_n(&m_qPub, (uint32_t)m.qCount, __ATOMIC_RELEASE);
    return 0;
}

// What a slot sent that the game has not taken yet goes when the slot leaves or is taken by someone else: poses, untracked messages.
// netcore: never a tracked event: it is part of the exactly-once stream every other console delivers too (a roster change can come
// after events of the slot's next membership, e.g. a new host's catch-up; purging them made holes).
void MpCore::purgeSlot(int slot) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS) return;
    {
        MpLock ql(m_qMutex);
        if (m_mem) {
            for (int i = 0; i < m_mem->qCount;) {
                const QEntry& e = m_mem->q[m_mem->qRing[(m_mem->qHead + i) % MP_QUEUE_SIZE]];
                if (e.fromSlot == slot && !e.tracked) queueRemoveAt(i);
                else i++;
            }
        }
    }
    m_haveU[slot] = 0;
}

void MpCore::pushEvent(uint8_t kind, int slot, int other, uint32_t arg) {
    if (m_evCount == MP_EVENT_QUEUE) {
        m_evHead = (m_evHead + 1) % MP_EVENT_QUEUE;
        m_evCount--;
    }
    MpCoreEvent& e = m_events[(m_evHead + m_evCount) % MP_EVENT_QUEUE];
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.slot = (uint8_t)slot;
    e.other = (uint8_t)other;
    e.epoch = m_epoch;
    e.serial = m_serial;
    e.arg = arg;
    m_evCount++;
}

// ---- history (every console keeps the relayed events: any of them may have to serve them as the new host) -------------------
void MpCore::historyAdd(uint8_t origin, uint16_t seq, MpGen gen, uint8_t type, const uint8_t* data, uint16_t len) {
    if (!m_mem || len > sizeof m_mem->hist[0].data) return;
    MpCoreMem& m = *m_mem;
    if (m.hCount == MP_HISTORY_ENTRIES) {
        m.hHead = (m.hHead + 1) % MP_HISTORY_ENTRIES;
        m.hCount--;
    }
    HistEntry& h = m.hist[(m.hHead + m.hCount) % MP_HISTORY_ENTRIES];
    h.origin = origin;
    h.type = type;
    h.gen = gen;
    h.seq = seq;
    h.len = len;
    h.suppressed = false;
    if (len) memcpy(h.data, data, len);
    m.hCount++;
}

// netcore: the history is the last MP_HISTORY_ENTRIES tracked events (every origin, every membership, in the order this console
// delivered or sent them). Nothing is forgotten when a player leaves or a slot is taken again: entries carry (origin, gen, seq), so
// they can never be mistaken for another membership's, and a console that missed the last events of a player who left can still be
// served them after a host migration. (It used to be pruned per slot: a stale new host then served the previous membership's events
// against the current one's sequence numbers: duplicates, and dropped the new ones as duplicates: holes.)
bool MpCore::historyHas(uint8_t origin, MpGen gen, uint16_t seq) const {
    if (!m_mem) return false;
    const MpCoreMem& m = *m_mem;
    for (int i = 0; i < m.hCount; i++) {
        const HistEntry& h = m.hist[(m.hHead + i) % MP_HISTORY_ENTRIES];
        if (h.origin == origin && h.gen == gen && h.seq == seq) return true;
    }
    return false;
}

// corefix 4 / netcore: a dead host's events may be re-sent by guests to the new host until the next scene; then that ends (its
// history entries stay like everyone's, and so does what was delivered: seen / gen are never reset within a session)
void MpCore::dropGhostHistory() {
    m_ghostMask = 0;
    if (m_mem) for (int i = 0; i < MP_PARK_ENTRIES; i++) m_mem->park[i].used = false;
}

uint16_t MpCore::seenOf(int slot) const { return slot == m_localSlot ? m_myOrigin : m_seen[slot]; }
MpGen MpCore::genOf(int slot) const { return slot == m_localSlot ? m_myGen : m_seenGen[slot]; }
void MpCore::noteGen(int slot, MpGen gen) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS || !gen) return;
    if (!m_genFloor[slot] || GenAhead(gen, m_genFloor[slot]) > 0) m_genFloor[slot] = gen;
}

// A follower answered HOST_CHANGE with what it has delivered of every origin (gen, seq): send it what it lacks, oldest first. False
// (nothing sent) when the history no longer reaches back to the first event it lacks of some origin's current membership: the follower
// cannot be caught up and must be admitted again (a new membership: snapshot / state instead of events).
bool MpCore::sendCatchUp(int slot, const uint16_t* seen, const MpGen* gens) {
    MpCoreMem& m = *m_mem;
    for (int o = 0; o < MP_MAX_PLAYERS; o++) {
        if (o == slot) continue;
        const MpGen hg = genOf(o);
        const uint16_t hs = seenOf(o);
        if (!hg || !hs) continue;                                     // nothing of that slot here
        if (gens[o] && gens[o] != hg && GenNewer(gens[o], hg)) continue;          // the follower is ahead (this host missed a rejoin)
        const bool sameGen = gens[o] == hg;
        if (gens[o] && !sameGen) {
            // the follower is still in an earlier membership of o: if this history holds later events of that membership, it must
            // hold the next one the follower needs (else what it sent would have a hole in the middle)
            const uint16_t nx = (uint16_t)(seen[o] + 1);
            bool later = false;
            for (int i = 0; i < m.hCount && !later; i++) {
                const HistEntry& h = m.hist[(m.hHead + i) % MP_HISTORY_ENTRIES];
                later = h.origin == o && h.gen == gens[o] && SeqDiff(h.seq, seen[o]) > 0;
            }
            if (later && !historyHas((uint8_t)o, gens[o], nx ? nx : 1)) {
                mp_log("catch-up slot=%d impossible: origin %d gen %u seq %u is no longer in the history", slot, o, (unsigned)gens[o],
                       (unsigned)(nx ? nx : 1));
                return false;
            }
        }
        if (sameGen && SeqDiff(hs, seen[o]) <= 0) continue;           // it has everything of this membership
        const uint16_t next = (uint16_t)(seen[o] + 1);
        const uint16_t first = sameGen && next ? next : 1;            // (an origin sequence skips 0)
        if (!historyHas((uint8_t)o, hg, first)) {
            mp_log("catch-up slot=%d impossible: origin %d gen %u seq %u is no longer in the history (it has gen %u seq %u)", slot, o,
                   (unsigned)hg, (unsigned)first, (unsigned)gens[o], (unsigned)seen[o]);
            return false;
        }
    }
    int n = 0;
    for (int i = 0; i < m.hCount; i++) {
        const HistEntry& h = m.hist[(m.hHead + i) % MP_HISTORY_ENTRIES];
        if (h.suppressed) continue;
        if (h.origin == slot || h.origin >= MP_MAX_PLAYERS) continue;
        const MpGen fg = gens[h.origin];
        if (fg) {
            if (h.gen != fg && !GenNewer(h.gen, fg)) continue;        // a membership the follower is past
            if (h.gen == fg && SeqDiff(h.seq, seen[h.origin]) <= 0) continue;
        }
        if (!queueGameReliable(slot, h.type, h.origin, h.seq, h.gen, h.data, h.len)) break;
        n++;
    }
    if (n) mp_log("catch-up slot=%d events=%d", slot, n);
    return true;
}

void MpCore::snoopWhere(uint8_t type, const uint8_t* data, uint16_t len) {
    if ((type == MP_MSG_WHERE || type == MP_MSG_MOVE) && len >= 8) {
        memcpy(m_where, data, 8);
        m_haveWhere = true;
        const uint16_t serial = (uint16_t)(data[6] | (data[7] << 8));
        if (serial != m_serial) {
            dropGhostHistory();   // corefix 4
            clearRestartGate();
            m_serial = serial;
            purgeOldStates();
        }
    }
}

bool MpCore::lookFree(uint8_t look, int exceptSlot) const {
    for (int s = 0; s < MP_MAX_PLAYERS; s++)
        if (s != exceptSlot && m_roster[s].present && m_roster[s].look == look) return false;
    return true;
}

// keep (a rejoin's old look) if free, else the wish if free, else the first free look
uint8_t MpCore::assignLook(uint8_t wish, uint8_t keep, int slot) const {
    if (keep < MP_LOOK_COUNT && lookFree(keep, slot)) return keep;
    if (wish < MP_LOOK_COUNT && lookFree(wish, slot)) return wish;
    for (uint8_t l = 0; l < MP_LOOK_COUNT; l++)
        if (lookFree(l, slot)) return l;
    return 0;   // (cannot happen: at most MP_LOOK_COUNT players)
}

void MpCore::handleLookRequest(int slot, uint8_t look) {
    if (look >= MP_LOOK_COUNT || !m_roster[slot].present) return;
    if (m_roster[slot].look == look) return;
    if (!lookFree(look, slot)) {
        MP_LOG_LIMITED("look change refused slot=%d look=%d (taken)", slot, look);
        m_rosterDirty = true;   // (the requester learns its look is unchanged)
        return;
    }
    m_roster[slot].look = look;
    m_rosterDirty = true;
    pushEvent(MP_EV_LOOK, slot, look, 0);
    mp_log("look changed slot=%d look=%d", slot, look);
}

int MpCore::presentCount() const {
    int n = 0;
    for (int s = 0; s < MP_MAX_PLAYERS; s++)
        if (m_roster[s].present) n++;
    return n;
}

// ---- core messages ---------------------------------------------------------------------------------------------------------
void MpCore::handleCore(int slot, uint8_t type, uint8_t from, const uint8_t* data, uint16_t len) {
    const uint64_t t = now();
    Peer& p = m_peers[slot];
    MpReader r(data, len);
    const bool host = m_role == ROLE_HOST;
    switch (type) {
    case MP_MSG_HEARTBEAT: {
        const uint32_t time = r.u32();
        const uint32_t echo = r.u32();
        if (!r.done()) { bad(slot, "heartbeat"); return; }
        p.peerTime = time;
        p.peerTimeAt = t;
        p.havePeerTime = true;
        if (echo) {
            const uint32_t rtt = (uint32_t)t - echo;
            if (rtt >= 1 && rtt < 10000) { p.srtt = p.srtt < 0 ? (int)rtt : (p.srtt * 7 + (int)rtt) / 8; rttNote(p, rtt); }
        }
        return;
    }
    case MP_MSG_HELLO: {
        if (!host) { bad(slot, "hello to a guest"); return; }
        r.u32();
        r.u16();
        char raw[MP_NAME_BYTES];
        r.bytes(raw, sizeof raw);
        const uint8_t look = r.u8();
        char name[MP_NAME_BYTES + 1];
        mp_name_clean(raw, sizeof raw, name);
        mp_name_or_character(name, look);
        uint32_t gid = 0;
        r.u8();   // mode
        if (r.ok() && r.len - r.n >= 12) { r.u32(); r.u32(); gid = r.u32(); }   // (wanted session, console id, core2: gathering identity)
        acceptJoin(slot, p.addr, p.helloSeq, name, look < MP_LOOK_COUNT ? look : 0, p.rejoin, t);
        m_roster[slot].gid = gid;
        return;
    }
    case MP_MSG_WELCOME:
        if (host || m_phase != MP_UI_CONNECTING) { bad(slot, "welcome"); return; }
        onWelcome(data, len);
        return;
    case MP_MSG_REJECT: {
        if (host) { bad(slot, "reject to host"); return; }
        const uint8_t reason = r.u8();
        if (!r.done()) { bad(slot, "reject"); return; }
        MpUiError e = MP_ERR_INTERNAL;
        switch (reason) {
        case MP_REJECT_VERSION: e = MP_ERR_DIFFERENT_VERSION; break;
        case MP_REJECT_FULL: case MP_REJECT_BUSY: e = MP_ERR_ROOM_FULL; break;
        case MP_REJECT_MODE: case MP_REJECT_CLOSED: e = MP_ERR_ROOM_NOT_FOUND; break;
        case MP_REJECT_KICKED: e = MP_ERR_KICKED; break;
        default: break;
        }
        mp_log("rejected by host reason=%d", reason);
        requestEnd(e, "rejected");
        return;
    }
    case MP_MSG_ROSTER: {
        if (host || from != m_hostSlot || m_localSlot < 0) { bad(slot, "roster"); return; }
        RosterEntry ro[MP_MAX_PLAYERS];
        Kicked bans[8];
        int nb = 0;
        if (!readRoster(r, ro, bans, &nb) || !r.done()) { bad(slot, "roster"); return; }
        applyBans(bans, nb);   // corefix2
        applyRoster(ro, false);
        return;
    }
    case MP_MSG_BYE: {
        const uint8_t reason = r.u8();
        if (!r.done()) { bad(slot, "bye"); return; }
        if (host) {
            removeSlot(slot, reason, "bye");
            return;
        }
        mp_log("host says bye reason=%s", ByeName(reason));
        if (reason == MP_BYE_HOST_LEAVING || reason == MP_BYE_SUSPEND || reason == MP_BYE_LEAVE || reason == MP_BYE_TIMEOUT) {
            m_hostLeaving = true;   // migrate now (pumpMigration)
            m_hostGoneAt = t;
        } else if (reason == MP_BYE_ERROR) {
            requestEnd(MP_ERR_KICKED, "removed by host");
        } else if (reason == MP_BYE_RESYNC) {
            requestEnd(MP_ERR_TIMEOUT, "the new host cannot catch this console up", true);   // netcore: rejoin (a new membership)
        } else {
            requestEnd(MP_ERR_HOST_LEFT, "host closed");
        }
        return;
    }
    case MP_MSG_SETTINGS: {
        MpSettings s;
        if (host || from != m_hostSlot || !mp_read_settings(r, &s) || !r.done()) { bad(slot, "settings"); return; }
        m_settings = s;
        mp_log("settings from host mode=%d max=%d downed=%d", s.mode, s.maxPlayers, s.downed);
        return;
    }
    case MP_MSG_FRIEND: {   // core2: a self-announced friend code (rooms without the service). Never logged.
        const uint8_t about = r.u8();
        char code[10];
        r.bytes(code, 9);
        code[9] = 0;
        bool none = true;
        for (int i = 0; i < 9; i++) none = none && code[i] == 0;
        if (!r.done() || about >= MP_MAX_PLAYERS || (!none && !WellFormedPlayerCode(code)) || (host && about != slot) ||
            (!host && from != m_hostSlot)) {
            bad(slot, "friend");
            return;
        }
        if (!announcesCodes() || about == m_localSlot || !m_roster[about].present) { m_statStray++; return; }   // (online: the service's only)
        const uint64_t t = now();
        if (m_annAt[about] && Since(t, m_annAt[about]) < 1000 && memcmp(m_annCode[about], code, 10) != 0) {
            m_statStray++;   // (at most one change a second per player: a flood costs nothing more)
            return;
        }
        m_annAt[about] = t;
        if (!memcmp(m_annCode[about], code, 10)) return;
        memcpy(m_annCode[about], code, 10);
        m_annGen[about] = m_roster[about].gen;
        if (host)
            for (int s = 0; s < MP_MAX_PLAYERS; s++)
                if (s != slot && s != m_localSlot && m_peers[s].used && m_peers[s].resumed && !m_peers[s].closing && m_roster[s].present)
                    sendFriendCode(s, about, m_annCode[about]);
        return;
    }
    case MP_MSG_REDIRECT_CODE: {   // core2: the gathering room becomes a Play by Address room: follow it there (the driver joins it)
        const uint32_t ip = r.u32();
        const uint16_t port = r.u16();
        uint8_t sec[5];
        r.bytes(sec, 5);
        if (host || from != m_hostSlot || !r.done() || !ip || !port || m_started) { bad(slot, "redirect code"); return; }
        m_redirectCode.have = true;
        m_redirectCode.ip = ip;
        m_redirectCode.port = port;
        memcpy(m_redirectCode.secret, sec, 5);
        mp_log("gather: the room moves to Play by Address: following");
        requestEnd(MP_ERR_NONE, "redirected to the room's address");
        return;
    }
    case MP_MSG_REDIRECT: {   // core2: the gathering room closes into another one: go there (the gathering driver joins it)
        const uint32_t sid = r.u32();
        if (host || from != m_hostSlot || !r.done() || sid == 0 || sid == m_sessionId || m_started) { bad(slot, "redirect"); return; }
        m_redirectTo = sid;
        mp_log("gather: the room closes into another one: following");
        requestEnd(MP_ERR_NONE, "redirected to another room");
        return;
    }
    case MP_MSG_HOST_CHANGE: {
        // (accepted in handleHostChangePacket; this is its in-channel delivery)
        const uint16_t epoch = r.u16();
        const uint8_t hs = r.u8();
        const uint16_t serial = r.u16();
        if (!r.ok() || host || hs != m_hostSlot || epoch != m_epoch) return;
        clearRestartGate();
        m_serial = serial;
        return;
    }
    case MP_MSG_RESUME:
        if (!host) { bad(slot, "resume to a guest"); return; }
        onResume(slot, data, len);
        return;
    case MP_MSG_SNAPSHOT_BEGIN: {
        if (host || from != m_hostSlot) { bad(slot, "snapshot"); return; }
        if (!m_mem->rx.begin(data, len)) { bad(slot, "snapshot begin"); return; }
        m_snapProgress = 0;
        return;
    }
    case MP_MSG_SNAPSHOT_CHUNK: {
        if (host || from != m_hostSlot) { bad(slot, "snapshot"); return; }
        bool complete = false;
        if (!m_mem->rx.chunk(data, len, &complete)) {
            bad(slot, "snapshot chunk");
            uint8_t b[8];
            const uint16_t n = mp_snapshot_done_payload(b, sizeof b, 0, false);
            queueReliable(slot, MP_MSG_SNAPSHOT_DONE, (uint8_t)m_localSlot, b, n);
            return;
        }
        m_snapProgress = m_mem->rx.progress();
        if (!complete) return;
        const bool ok = m_mem->rx.crcOk();
        const uint32_t crc = m_mem->rx.expectedCrc();
        if (ok) {
            MpSnapshotImage& im = m_mem->image;
            memcpy(im.bytes, m_mem->rx.bytes(), m_mem->rx.total());
            im.len = m_mem->rx.total();
            im.crc = crc;
            im.version++;
            m_snapReady = true;
            __atomic_store_n(&m_snapPub, (uint32_t)im.len, __ATOMIC_RELEASE);
            pushEvent(MP_EV_SNAPSHOT, m_localSlot, 0, im.len);
        }
        mp_log("snapshot received bytes=%u crc=%08x ok=%d", (unsigned)m_mem->rx.total(), (unsigned)crc, ok ? 1 : 0);
        uint8_t b[8];
        const uint16_t n = mp_snapshot_done_payload(b, sizeof b, crc, ok);
        queueReliable(slot, MP_MSG_SNAPSHOT_DONE, (uint8_t)m_localSlot, b, n);
        return;
    }
    case MP_MSG_SNAPSHOT_DONE: {
        uint32_t crc = 0;
        bool ok = false;
        if (!host || !mp_snapshot_done_read(data, len, &crc, &ok)) { bad(slot, "snapshot done"); return; }
        if (ok && crc == m_mem->image.crc) {
            mp_log("snapshot delivered slot=%d crc=%08x", slot, (unsigned)crc);
            if ((m_roster[slot].state & 3) == 0) {
                m_roster[slot].state = (uint8_t)((m_roster[slot].state & ~3) | 1);
                m_rosterDirty = true;
            }
        } else if (ok) {
            // an older image arrived after a newer was offered: the newer one is on its way
        } else if (++p.snapTries <= 2) {
            mp_log("snapshot failed at slot %d: sending again", slot);
            sendSnapshotTo(slot);
        } else {
            removeSlot(slot, MP_BYE_ERROR, "snapshot failed");
        }
        return;
    }
    default:
        bad(slot, "unknown core type");
        return;
    }
}

// ---- host: joins -----------------------------------------------------------------------------------------------------------
int MpCore::freeSlotFor(const char* name, const uint8_t* commit, const MpAddr& addr, uint64_t t, bool* rejoin) {
    *rejoin = false;
    (void)name;
    (void)addr;
    // core2: a slot is taken over only by the console holding its rejoin token (the HELLO's token matches the slot's commitment, which
    // every console has in the roster, so any host can check it). A console id or a name proves nothing (both are sent in the clear).
    // 1) the same player again (a rejoin after suspend / a lost link): takes over its slot, the old session goes
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        if (s == m_localSlot || !m_roster[s].present) continue;
        if (memcmp(m_roster[s].commit, kZeroKey, 16) == 0 || memcmp(m_roster[s].commit, commit, 16) != 0) continue;
        *rejoin = true;
        return s;
    }
    // 2) a reservation of this console (left less than MP_REJOIN_MS ago)
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        if (s == m_localSlot || !m_res[s].used || t > m_res[s].until) continue;
        if (memcmp(m_res[s].commit, kZeroKey, 16) == 0 || memcmp(m_res[s].commit, commit, 16) != 0) continue;
        if (!m_roster[s].present && !m_peers[s].used) {
            *rejoin = true;
            return s;
        }
    }
    if (presentCount() >= m_settings.maxPlayers) return -1;
    // corefix2: a slot reserved for a player whose link dropped (MP_LINKLOST_HOLD_MS, MP_REJOIN_MS for a lost host) is kept for it
    // for the whole window: a newcomer never gets it (it is told the room is full for now), even when it is the only slot free
    // review: online, the slot is the service room's (the virtual address is base | (service slot + 1)). NpNet centres the star
    // and moves the host role by the service's host_slot (the lowest service slot), the session by its own lowest slot: two
    // guests whose HELLOs arrived in the other order than their service joins would make the two layers pick different new
    // hosts after a host leaves. A service slot already taken here (a stale entry) falls back to the free slot below.
    if (m_connKind == MP_CONN_ONLINE) {
        const int s = (int)(addr.host & 0xFF) - 1;
        if (s >= 0 && s < MP_MAX_PLAYERS && s != m_localSlot && !m_roster[s].present && !m_peers[s].used && !slotHeld(s, t)) return s;
        mp_log("online join: service slot %d not free here, slot by order", s);
    }
    // 3) the lowest free slot, preferring slots nobody has reserved
    int fallback = -1;
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        if (s == m_localSlot || m_roster[s].present || m_peers[s].used) continue;
        if (slotHeld(s, t)) continue;   // corefix2
        if (m_res[s].used && t <= m_res[s].until) {
            if (fallback < 0) fallback = s;
            continue;
        }
        return s;
    }
    return fallback;
}

// corefix D4: the joiner a full room lets go: the oldest that never acknowledged anything (past a short grace); -1: none
int MpCore::evictableGhost(uint64_t t) const {
    int ghost = -1;
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        const Peer& q = m_peers[s];
        if (s == m_localSlot || !q.used || q.closing || !q.resumed || q.established || Since(t, q.createdAt) < MP_JOIN_GRACE_MS) continue;
        if (q.rel->unacked() == 0) continue;
        if (ghost < 0 || q.createdAt < m_peers[ghost].createdAt) ghost = s;
    }
    return ghost;
}

// corefix D4: would this HELLO be refused as FULL or CLOSED whatever its proof says? Decided from the (not yet proven) token commitment
// without any crypto: a console that holds a slot or a live reservation is never "full" (its rejoin always works).
bool MpCore::helloWouldBeRefused(const uint8_t* commit, uint64_t t, uint8_t* reason) const {
    if (memcmp(commit, kZeroKey, 16) != 0) {
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            if (s != m_localSlot && m_roster[s].present && memcmp(m_roster[s].commit, commit, 16) == 0) return false;
            if (s != m_localSlot && m_res[s].used && t <= m_res[s].until && memcmp(m_res[s].commit, commit, 16) == 0) return false;
        }
    }
    if (m_phase == MP_UI_PLAYING && !(m_settings.flags & 1)) {
        *reason = MP_REJECT_CLOSED;
        return true;
    }
    if (presentCount() >= m_settings.maxPlayers && evictableGhost(t) < 0) {
        *reason = MP_REJECT_FULL;
        return true;
    }
    int open = 0;   // corefix2: slots a newcomer may take (a held reservation is not one)
    for (int s = 0; s < MP_MAX_PLAYERS; s++)
        if (s != m_localSlot && !m_roster[s].present && !m_peers[s].used && !slotHeld(s, t)) open++;
    if (!open && evictableGhost(t) < 0) {
        *reason = MP_REJECT_FULL;
        return true;
    }
    return false;
}

void MpCore::handleHelloPacket(const MpAddr& from, const uint8_t* hdr, const uint8_t* payload, uint16_t plen, uint64_t t) {
    const uint16_t seq = (uint16_t)(hdr[6] | (hdr[7] << 8));
    MpReader r(payload, plen);
    const uint32_t fp = r.u32();
    const uint16_t proto = r.u16();
    if (!r.ok()) {
        m_statStray++;
        return;
    }
    if (proto != MP_PROTO_VERSION || hdr[2] != MP_PROTO_VERSION || fp != m_fingerprint) {
        // another version's HELLO (an older build has no cookie step): the 1-byte refusal it understands, stateless and rate-limited
        MP_LOG_LIMITED("hello rejected reason=version (fingerprint %08x, ours %08x)", (unsigned)fp, (unsigned)m_fingerprint);
        sendRejectRaw(from, MP_REJECT_VERSION, seq);
        return;
    }
    char raw[MP_NAME_BYTES];
    r.bytes(raw, sizeof raw);
    const uint8_t look = r.u8();
    const uint8_t mode = r.u8();
    const uint32_t wantSession = r.u32();
    const uint32_t consoleId = r.u32();
    r.u32();   // core2: gathering identity (taken at delivery)
    // core2: the joiner's key, its nonce, our cookie, its proof of the pair key (and so of the room's secret), its masked rejoin token
    // and the token's commitment
    uint8_t pk[32], nonce[8], ck[16], proof[16], tokMasked[16], commit[16];
    r.bytes(pk, 32);
    r.bytes(nonce, 8);
    r.bytes(ck, 16);
    r.bytes(proof, 16);
    r.bytes(tokMasked, 16);
    r.bytes(commit, 16);
    if (!r.done()) {
        m_statStray++;
        return;
    }
    // the cookie: this address asked a moment ago (this 10 s bucket or the one before)
    uint8_t want[16];
    bool cookieOk = false;
    for (int b = 0; b < 2 && !cookieOk; b++) {
        cookieFor(from, pk, nonce, t / 10000 - (uint64_t)b, want);
        cookieOk = mpseal::Equal16(want, ck);
    }
    if (!cookieOk) {
        m_statStray++;
        return;
    }
    // proofs that fail (a guessed code secret). corefix D3: counted per source address (a stranger replaying one wrong HELLO2 used up the
    // room-wide allowance and kept every honest join and rejoin out); a room-wide ceiling still bounds the total work, but a console
    // the room knows (roster, a recent leaver) is not held to it.
    ProofSrc* psrc = 0;
    {
        ProofSrc* oldest = &m_proofSrc[0];
        for (int i = 0; i < 16; i++) {
            if (m_proofSrc[i].host == from.host && m_proofSrc[i].window) { psrc = &m_proofSrc[i]; break; }
            if (m_proofSrc[i].window < oldest->window) oldest = &m_proofSrc[i];
        }
        if (!psrc) {
            psrc = oldest;   // (a table of strangers only ever evicts the quietest: an entry in its second keeps its count)
            psrc->host = from.host;
            psrc->window = t;
            psrc->fails = 0;
            psrc->badUntil = 0;
        }
        if (t - psrc->window >= 1000) {
            psrc->window = t;
            psrc->fails = 0;
        }
    }
    if (t - m_proofWindow >= 1000) {
        m_proofWindow = t;
        m_proofFails = 0;
    }
    // corefix2: fair between sources. A source that failed a proof lately is a suspect; only suspects are held to the room-wide ceiling
    // (a stranger that never failed one is not, so four addresses with valid cookies and wrong proofs cannot fill it against a newcomer),
    // and below, the key-exchange budget keeps MP_PAIRKEY_RESERVE per second for sources that never failed.
    const bool suspect = psrc->badUntil > t;
    if (psrc->fails >= MP_PROOF_FAILS_PER_SEC || (suspect && m_proofFails >= MP_PROOF_FAILS_ALL_PER_SEC && !vouchedAddr(from, t))) {
        m_statStray++;
        return;
    }
    uint8_t key[32];
    // corefix D4: what costs an X25519 and could be refused (or answered) without one is settled first.
    // (1) the kicked and (2) a room that is full or closed are answered at once, from what the HELLO shows (a replayed HELLO2 of a member's
    //     address never gets here: handleFromPeer drops a repeat of the one that started its channel); (3) the rest is metered.
    {
        uint8_t why = 0;
        for (int i = 0; i < m_kickedCount && !why; i++) {   // the kicked, by what the HELLO already shows (answered to that address only)
            const Kicked& k = m_kicked[i];
            const bool tokMatch = memcmp(k.commit, kZeroKey, 16) != 0 && !memcmp(k.commit, commit, 16);
            const bool keyMatch = memcmp(k.pk, kZeroKey, 32) != 0 && !memcmp(k.pk, pk, 32);
            const bool addrMatch = m_connKind != MP_CONN_ONLINE && k.addr.host != 0 && k.addr == from;
            if (tokMatch || keyMatch || addrMatch) {
                why = MP_REJECT_KICKED;
                char nm[MP_NAME_BYTES + 1];
                mp_name_clean(raw, sizeof raw, nm);
                mp_name_or_character(nm, look);
                MP_LOG_LIMITED("hello rejected reason=kicked name=%s%s", nm, tokMatch ? "" : keyMatch ? " (by key)" : " (by address)");
            }
        }
        if (why) {
            sendRejectNonce(from, why, seq, nonce);
            return;
        }
        if (helloWouldBeRefused(commit, t, &why)) {
            MP_LOG_LIMITED("hello rejected reason=%s (before the key exchange)", why == MP_REJECT_FULL ? "full" : "closed");
            sendRejectNonce(from, why, seq, nonce);
            return;
        }
        if (!vouchedAddr(from, t)) {
            const uint64_t dt = m_pkAt ? t - m_pkAt : 0;   // (milli-ops: MP_PAIRKEYS_PER_SEC per 1000 ms)
            m_pkAt = t;
            const uint32_t cap = MP_PAIRKEYS_PER_SEC * 1000u;
            const uint64_t refill = dt * MP_PAIRKEYS_PER_SEC;
            m_pkTokens = (uint32_t)(m_pkTokens + refill > cap ? cap : m_pkTokens + refill);
            if (m_pkTokens < 1000u + (suspect ? MP_PAIRKEY_RESERVE * 1000u : 0u)) {
                m_statStray++;
                MP_LOG_LIMITED("hello dropped: the key-exchange budget (%d per second) is used up", MP_PAIRKEYS_PER_SEC);
                return;
            }
            m_pkTokens -= 1000u;
        }
        m_statPairKeys++;
        if (!mpseal::PairKey(m_sk, m_pk, pk, pskOn() ? m_psk : kZeroKey, m_sessionId, key)) {
            m_statStray++;
            return;
        }
    }
    uint8_t pm[5 + 16 + 8], expect[16], mask[16], token[16], tokHash[16];
    memcpy(pm, "hello", 5);
    memcpy(pm + 5, ck, 16);
    memcpy(pm + 21, nonce, 8);
    mpseal::Mac(expect, 16, key, 32, pm, sizeof pm);
    if (!mpseal::Equal16(expect, proof)) {
        m_proofFails++;
        psrc->fails++;
        psrc->badUntil = t + MP_SUSPECT_MS;   // corefix2
        MP_LOG_LIMITED("hello rejected reason=proof (wrong code secret?)");
        return;   // (no answer: a guesser learns nothing, not even "wrong")
    }
    memcpy(pm, "tmask", 5);
    mpseal::Mac(mask, 16, key, 32, pm, sizeof pm);
    for (int i = 0; i < 16; i++) token[i] = (uint8_t)(tokMasked[i] ^ mask[i]);
    mpseal::Mac(tokHash, 16, 0, 0, token, 16);
    if (!mpseal::Equal16(tokHash, commit)) {
        m_statStray++;
        return;
    }
    char name[MP_NAME_BYTES + 1];
    mp_name_clean(raw, sizeof raw, name);
    mp_name_or_character(name, look);
    for (int i = 0; i < m_kickedCount; i++) {
        // fix2 / corefix 9 / core2: the kicked console: its token (a rejoin), its key, its console id, or (not online) its address. Honest
        // limit: a console that restarts with a new token, key and id from another address is a new console (DESIGN.md 10.4)
        const Kicked& k = m_kicked[i];
        // corefix S4: the console id of a HELLO is sent in the clear and nobody vouches for it: a hostile member could join under a
        // victim's id, get kicked, and so ban the victim. Only what the sealed handshake proves counts: the token (its commitment is
        // checked above), the key (the proof needs its secret half) and, off the internet, the address.
        const bool tokMatch = memcmp(k.commit, kZeroKey, 16) != 0 && !memcmp(k.commit, commit, 16);
        const bool keyMatch = memcmp(k.pk, kZeroKey, 32) != 0 && !memcmp(k.pk, pk, 32);
        const bool addrMatch = m_connKind != MP_CONN_ONLINE && k.addr.host != 0 && k.addr == from;
        if (tokMatch || keyMatch || addrMatch) {
            MP_LOG_LIMITED("hello rejected reason=kicked name=%s%s", name, tokMatch ? "" : keyMatch ? " (by key)" : " (by address)");
            sendRejectNonce(from, MP_REJECT_KICKED, seq, nonce);
            return;
        }
    }
    if (m_phase == MP_UI_LEAVING || (wantSession && wantSession != m_sessionId)) {
        sendRejectNonce(from, MP_REJECT_CLOSED, seq, nonce);
        return;
    }
    if (mode != MP_MODE_NONE && mode != m_settings.mode) {
        MP_LOG_LIMITED("hello rejected reason=mode");
        sendRejectNonce(from, MP_REJECT_MODE, seq, nonce);
        return;
    }
    // outside review (ghost joiners): a session that never acknowledged anything keeps its clock across a new HELLO of the same console
    // (else one HELLO every 4 s held a slot for ever: the never-acknowledged removal restarted each time)
    uint64_t carryCreated = 0, carryAck = 0;
    auto carry = [&](const Peer& old) {
        if (!old.used || old.established) return;
        if (!carryCreated || old.createdAt < carryCreated) carryCreated = old.createdAt, carryAck = old.ackAt;
    };
    for (int s = 0; s < MP_MAX_PLAYERS; s++)   // the same console started over (a new HELLO, proven): its old session at this address goes
        if (s != m_localSlot && m_peers[s].used && m_peers[s].addr == from && m_peers[s].helloSeq != seq && m_peers[s].resumed &&
            !m_peers[s].closing && memcmp(m_roster[s].commit, commit, 16) != 0) {
            MP_LOG_LIMITED("slot %d says hello again: new session", s);
            carry(m_peers[s]);
            removeSlot(s, MP_BYE_LEAVE, "new hello");
        }
    bool rejoin = false;
    int slot = freeSlotFor(name, commit, from, t, &rejoin);
    if (slot < 0) {
        // full: the oldest joiner that never acknowledged anything (past a short grace) gives way (outside review: ghost joiners)
        const int ghost = evictableGhost(t);
        if (ghost >= 0) {
            removeSlot(ghost, MP_BYE_TIMEOUT, "room full: a joiner that never acknowledged gives way");
            slot = freeSlotFor(name, commit, from, t, &rejoin);
        }
    }
    if (!rejoin && m_phase == MP_UI_PLAYING && !(m_settings.flags & 1)) {
        MP_LOG_LIMITED("hello rejected reason=closed");
        sendRejectNonce(from, MP_REJECT_CLOSED, seq, nonce);
        return;
    }
    if (slot < 0) {
        MP_LOG_LIMITED("hello rejected reason=full name=%s", name);
        sendRejectNonce(from, MP_REJECT_FULL, seq, nonce);
        return;
    }
    // netopt: a window for the newcomer. A host that took over holds fewer than it may need; it gets the missing one here, and when the
    // heap has none the HELLO is refused like a full room (the console asks again; nobody already in the room is dropped for it)
    MpReliable* const newRel = relFor(slot, true);
    if (!newRel) {
        MP_LOG_LIMITED("hello rejected reason=no-window name=%s", name);
        sendRejectNonce(from, MP_REJECT_FULL, seq, nonce);
        return;
    }
    if (rejoin && m_peers[slot].used) {
        MP_LOG_LIMITED("slot %d: %s is back, the old session goes", slot, name);
        carry(m_peers[slot]);
        m_peers[slot].rel->reset(0);
    }
    Peer& p = m_peers[slot];
    memset(&p, 0, sizeof p);
    p.used = true;
    p.resumed = true;
    p.addr = from;
    p.rel = newRel;
    p.rel->reset((uint16_t)rnd());
    p.helloSeq = seq;
    p.lastRecv = t;
    p.createdAt = t;
    p.ackAt = t;
    p.srtt = -1;
    p.rejoin = rejoin;
    p.consoleId = consoleId;
    memcpy(p.pk, pk, 32);
    memcpy(p.commit, commit, 16);
    if (carryCreated) {
        p.createdAt = carryCreated;
        p.ackAt = carryAck;
    }
    PairState& ps = m_pair[slot];   // core2: the pair's key from now on (WELCOME and everything after it are sealed)
    memset(&ps, 0, sizeof ps);
    memcpy(ps.key, key, 32);
    ps.have = true;
    ps.sendCtr = m_ctrBase + 1;
    ps.rxMax = CtrBase(ctrFloorFor(pk));   // (the same console's earlier memberships: their datagrams stay old)
    ps.rxMask = 1;
    (void)look;
    const int prev = m_authSlot;
    m_authSlot = slot;   // (the HELLO proved the key: it may count as the member's own traffic)
    handleFromPeer(slot, hdr, payload, plen, t);   // starts the channel at the HELLO and delivers it (-> acceptJoin)
    m_authSlot = prev;
}

static char AsciiLower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static bool SameName(const char* a, const char* b) {   // (ASCII case-insensitive: "player" and "Player" look the same on a name tag)
    for (; *a && *b; a++, b++)
        if (AsciiLower(*a) != AsciiLower(*b)) return false;
    return *a == *b;
}

// corefix 7: two players never show the same name: the later one gets a number ("Player 2"), cut so the whole name still fits
// MP_NAME_BYTES (never splitting a character).
void MpCore::uniqueName(char* name, int exceptSlot) const {
    auto taken = [&](const char* n) {
        for (int s = 0; s < MP_MAX_PLAYERS; s++)
            if (s != exceptSlot && m_roster[s].present && SameName(m_roster[s].name, n)) return true;
        return false;
    };
    if (!taken(name)) return;
    char base[MP_NAME_BYTES + 1];
    for (int n = 2; n < 100; n++) {
        char suffix[8];
        const int sl = snprintf(suffix, sizeof suffix, " %d", n);
        size_t keep = strlen(name);
        if (keep + (size_t)sl > MP_NAME_BYTES) {
            keep = MP_NAME_BYTES - (size_t)sl;
            while (keep > 0 && ((uint8_t)name[keep] & 0xC0) == 0x80) keep--;   // (do not cut inside a character)
        }
        memcpy(base, name, keep);
        while (keep > 0 && base[keep - 1] == ' ') keep--;
        base[keep] = 0;
        char cand[MP_NAME_BYTES + 8];
        snprintf(cand, sizeof cand, "%s%s", base, suffix);
        if (!taken(cand)) {
            memcpy(name, cand, strlen(cand) + 1);
            return;
        }
    }
}

void MpCore::acceptJoin(int slot, const MpAddr& from, uint16_t, const char* name, uint8_t look, bool rejoin, uint64_t t) {
    RosterEntry& e = m_roster[slot];
    uint8_t keep = 0xFF;
    if (rejoin) keep = e.present ? e.look : (m_res[slot].used ? m_res[slot].look : 0xFF);
    e.present = false;   // (its own old look does not count as taken)
    const uint8_t assigned = assignLook(look, keep, slot);
    // netcore: a newer generation than any this console has heard of for the slot (a host that missed a rejoin elsewhere learns the
    // newer ones from the RESUMEs): the new membership's events can never be taken for an earlier one's
    MpGen base = e.gen;
    if (m_genFloor[slot] && (!base || GenAhead(m_genFloor[slot], base) > 0)) base = m_genFloor[slot];
    uint8_t lo = (uint8_t)((base & 0xFF) + 1);
    if (!lo) lo = 1;
    const MpGen gen = (MpGen)(((1 + mp_sys_random() % 255) << 8) | lo);   // core2: random high byte (split hosts never collide)
    memset(&e, 0, sizeof e);
    e.present = true;
    memcpy(e.name, name, MP_NAME_BYTES + 1);
    uniqueName(e.name, slot);   // corefix 7 (e.present is true now, but the slot itself is excluded)
    e.look = assigned;
    e.gen = gen;
    e.ping = 0xFFFF;
    e.addr = from;
    e.consoleId = m_peers[slot].consoleId;
    memcpy(e.pk, m_peers[slot].pk, 32);   // core2: every member derives its pair key with this one from the roster
    memcpy(e.commit, m_peers[slot].commit, 16);   // core2: and any later host can check a rejoin's token against this
    e.state = (uint8_t)(m_mem->image.len ? 0 : (m_started ? 2 : 1));
    m_res[slot].used = false;
    m_claiming = false;   // somebody joined this (new) host: it is not the isolated one (else it ended 4 s after a claim even with a guest in: migration ping-pong)
    noteGen(slot, e.gen);
    m_ghostMask &= (uint8_t)~(1u << slot);
    purgeSlot(slot);   // (netcore: what it delivered of the slot's earlier memberships stays: seen / gen and history are per membership)
    sendWelcome(slot);
    if (m_mem->image.len) sendSnapshotTo(slot);
    sendKnownCodesTo(slot);   // core2
    m_restartGateMask &= (uint8_t)~(1u << slot);
    m_restartGateUntil[slot] = 0;
    if (rejoin && (m_restartMissedMask & (1u << slot)) && m_serial != 0 && m_serial == m_restartSerial && m_settings.mode == MP_MODE_TOGETHER) {
        uint8_t rb[2] = {(uint8_t)(m_restartSerial & 0xFFu), (uint8_t)(m_restartSerial >> 8)};
        queueGameReliable(slot, MP_MSG_RESTART, (uint8_t)m_localSlot, 0, m_myGen, rb, 2);
        m_restartGateMask |= (uint8_t)(1u << slot);
        m_restartGateUntil[slot] = t + 20000;
        mp_log("slot %d rejoined after missed group restart serial=%u: sent RESTART", slot, (unsigned)m_restartSerial);
    } else if (!rejoin) {
        m_restartMissedMask &= (uint8_t)~(1u << slot);
    }
    m_rosterDirty = true;
    if (m_conn) m_conn->playersChanged(presentCount());
    pushEvent(MP_EV_JOINED, slot, rejoin ? 1 : 0, 0);
    mp_log("slot %d joined name=%s rejoin=%d look=%d players=%d", slot, name, rejoin ? 1 : 0, assigned, presentCount());
    (void)t;
}

void MpCore::sendWelcome(int slot) {
    uint8_t buf[MP_MAX_PAYLOAD];
    MpWriter w(buf, sizeof buf);
    w.u8((uint8_t)slot);
    w.u32(m_sessionId);
    w.u8(m_settings.mode);
    mp_write_settings(w, m_settings);
    w.u16(m_epoch);
    w.u8((uint8_t)m_hostSlot);
    for (int s = 0; s < MP_MAX_PLAYERS; s++) w.u16(seenOf(s));
    for (int s = 0; s < MP_MAX_PLAYERS; s++) w.u16(genOf(s));   // netcore: the joiner starts each origin's stream where this host is
    w.u16(m_serial);
    w.bytes(m_roomKey, 32);   // core2: the room key (beacon MACs), only ever sent sealed
    const uint16_t rn = writeRoster(buf + w.n, (uint16_t)(sizeof buf - w.n));
    if (!w.ok() || !rn) return;
    queueReliable(slot, MP_MSG_WELCOME, (uint8_t)m_localSlot, buf, (uint16_t)(w.n + rn));
}

uint16_t MpCore::writeRoster(uint8_t* out, uint16_t cap) {
    MpWriter w(out, cap);
    const uint64_t t = now();
    w.u8((uint8_t)presentCount());
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        RosterEntry& e = m_roster[s];
        if (!e.present) continue;
        uint8_t state = e.state & 3;
        if (s == m_localSlot) state = (uint8_t)(m_started ? 2 : 1);
        else if ((state & 3) != 0) state = (uint8_t)(m_started ? 2 : 1);
        bool quiet = false;
        if (s != m_localSlot && m_peers[s].used) {
            quiet = t - m_peers[s].lastRecv >= MP_QUIET_MS;
            e.ping = m_peers[s].srtt < 0 ? 0xFFFF : (uint16_t)(m_peers[s].srtt > 0xFFFE ? 0xFFFE : m_peers[s].srtt);
        }
        e.state = (uint8_t)(state | (quiet ? 0x40 : 0));
        w.u8((uint8_t)s);
        PutName(w, e.name);
        w.u8(e.look);
        w.u8(e.state);
        w.u16(s == m_localSlot ? 0 : e.ping);
        w.u16(e.gen);
        // core2 (review finding 10): members' addresses only where the topology needs them (Same Wi-Fi / Nearby: any guest may become
        // host; online: the service's vips, which every member has anyway). By address: star only, no migration: nobody gets them.
        const bool addrs = m_connKind != MP_CONN_DIRECT;
        w.u32(addrs ? e.addr.host : 0);
        w.u16(addrs ? e.addr.port : 0);
        w.u32(s == m_localSlot ? m_gid : e.gid);   // core2
        w.bytes(e.pk, 32);   // core2: the member's key, the commitment of its rejoin token
        w.bytes(e.commit, 16);
    }
    // corefix2: the ban list (a trailer, only when there is one): every member holds it, so a removed console stays removed when the
    // host changes. Entries are what the handshake proves (token commitment, key) and, off the internet, the address; names and the
    // clear-text console id stay with the host that banned.
    if (m_kickedCount > 0) {
        w.u8((uint8_t)m_kickedCount);
        for (int i = 0; i < m_kickedCount; i++) {
            const Kicked& k = m_kicked[i];
            w.bytes(k.commit, 16);
            w.bytes(k.pk, 32);
            const bool addrs = m_connKind != MP_CONN_DIRECT && m_connKind != MP_CONN_ONLINE;
            w.u32(addrs ? k.addr.host : 0);
            w.u16(addrs ? k.addr.port : 0);
        }
    }
    return w.ok() ? (uint16_t)w.n : 0;
}

bool MpCore::readRoster(MpReader& r, RosterEntry* out, Kicked* bans, int* nBans) {
    if (nBans) *nBans = 0;
    memset(out, 0, sizeof(RosterEntry) * MP_MAX_PLAYERS);
    const uint8_t n = r.u8();
    if (!r.ok() || n > MP_MAX_PLAYERS) return false;
    for (int i = 0; i < n; i++) {
        const uint8_t s = r.u8();
        char raw[MP_NAME_BYTES];
        r.bytes(raw, sizeof raw);
        const uint8_t look = r.u8();
        const uint8_t state = r.u8();
        const uint16_t ping = r.u16();
        const MpGen gen = r.u16();
        const uint32_t host = r.u32();
        const uint16_t port = r.u16();
        const uint32_t gid = r.u32();   // core2
        uint8_t pk[32], commit[16];
        r.bytes(pk, 32);
        r.bytes(commit, 16);
        if (!r.ok() || s >= MP_MAX_PLAYERS || out[s].present || look >= MP_LOOK_COUNT || (state & 3) > 2) return false;
        RosterEntry& e = out[s];
        e.present = true;
        mp_name_clean(raw, sizeof raw, e.name);
        mp_name_or_character(e.name, look);
        e.look = look;
        e.state = state;
        e.ping = ping;
        e.gen = gen;
        e.addr.host = host;
        e.addr.port = port;
        e.gid = gid;
        memcpy(e.pk, pk, 32);
        memcpy(e.commit, commit, 16);
    }
    if (!r.done() && r.ok()) {   // corefix2: the ban list trailer
        const uint8_t nb = r.u8();
        if (!r.ok() || nb > 8) return false;
        for (int i = 0; i < nb; i++) {
            Kicked k;
            memset(&k, 0, sizeof k);
            r.bytes(k.commit, 16);
            r.bytes(k.pk, 32);
            k.addr.host = r.u32();
            k.addr.port = r.u16();
            if (!r.ok()) return false;
            memcpy(k.name, "(removed)", 10);
            if (bans) bans[i] = k;
        }
        if (nBans) *nBans = nb;
    }
    return r.ok();
}

void MpCore::applyRoster(const RosterEntry* ro, bool fromWelcome) {
    if (!ro[m_localSlot].present) {
        requestEnd(MP_ERR_KICKED, "not in the roster");
        return;
    }
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        if (s == m_localSlot) continue;
        const RosterEntry& was = m_roster[s];
        const RosterEntry& now_ = ro[s];
        if (!fromWelcome) {
            if (was.present && !now_.present) {
                rememberAddr(was.addr, now());   // netcore: a guest may be host later: a removed member's beacon must vouch for a merge then
                purgeSlot(s);   // (netcore: tracked events stay, and so does the dedupe state: see historyHas)
                pushEvent(MP_EV_LEFT, s, 0, 0);
                mp_log("roster: slot %d left", s);
            } else if (now_.present && was.present && was.gen == now_.gen && was.look != now_.look) {
                pushEvent(MP_EV_LOOK, s, now_.look, 0);
            } else if (now_.present && (!was.present || was.gen != now_.gen)) {
                purgeSlot(s);
                m_ghostMask &= (uint8_t)~(1u << s);
                pushEvent(MP_EV_JOINED, s, was.present ? 1 : 0, 0);
                mp_log("roster: slot %d joined name=%s", s, now_.name);
            }
        }
    }
    for (int s = 0; s < MP_MAX_PLAYERS; s++)
        if (ro[s].present) noteGen(s, ro[s].gen);
    for (int s = 0; s < MP_MAX_PLAYERS; s++)   // core2: a slot's key changed (another console there now): its pair key goes
        if (s != m_localSlot && s != m_hostSlot && (!ro[s].present || memcmp(ro[s].pk, m_roster[s].pk, 32) != 0)) pairReset(s);
    const RosterEntry mine = m_roster[m_localSlot];
    if (!fromWelcome && mine.present && mine.look != ro[m_localSlot].look) pushEvent(MP_EV_LOOK, m_localSlot, ro[m_localSlot].look, 0);
    memcpy(m_roster, ro, sizeof m_roster);
    // core2: this console's own key and token commitment are its own, whatever a roster says (a later host sends them on)
    memcpy(m_roster[m_localSlot].pk, m_pk, 32);
    if (m_haveToken) mpseal::Mac(m_roster[m_localSlot].commit, 16, 0, 0, m_token, 16);
    if (fromWelcome) m_roster[m_localSlot].addr = m_conn && m_conn->session() ? m_conn->session()->local() : mine.addr;
    // the host's state says whether the session has started
    if (m_hostSlot >= 0 && m_roster[m_hostSlot].present && (m_roster[m_hostSlot].state & 3) == 2 && !m_started) {
        m_started = true;
        if (m_phase == MP_UI_LOBBY) setPhase(MP_UI_PLAYING);
        pushEvent(MP_EV_STARTED, m_hostSlot, 0, 0);
        mp_log("session started (host)");
        mp_heap_note("mp session start");
    }
}

void MpCore::onWelcome(const uint8_t* data, uint16_t len) {
    MpReader r(data, len);
    const uint8_t slot = r.u8();
    const uint32_t sid = r.u32();
    const uint8_t mode = r.u8();
    MpSettings s;
    const bool sok = mp_read_settings(r, &s);
    const uint16_t epoch = r.u16();
    const uint8_t hs = r.u8();
    uint16_t seen[MP_MAX_PLAYERS];
    MpGen gens[MP_MAX_PLAYERS];
    for (int i = 0; i < MP_MAX_PLAYERS; i++) seen[i] = r.u16();
    for (int i = 0; i < MP_MAX_PLAYERS; i++) gens[i] = r.u16();
    const uint16_t serial = r.u16();
    uint8_t roomKey[32];
    r.bytes(roomKey, 32);
    RosterEntry ro[MP_MAX_PLAYERS];
    Kicked bans[8];
    int nBans = 0;
    if (!r.ok() || !sok || slot >= MP_MAX_PLAYERS || hs >= MP_MAX_PLAYERS || slot == hs || sid == 0 || mode != s.mode ||
        !readRoster(r, ro, bans, &nBans) || !r.done() || !ro[slot].present || !ro[hs].present ||
        (m_wantSession && m_wantSession != sid) || sid != m_sessionId || memcmp(ro[slot].pk, m_pk, 32) != 0 ||
        memcmp(ro[hs].pk, m_hostPk, 32) != 0) {
        bad(m_hostSlot, "welcome");
        requestEnd(MP_ERR_INTERNAL, "bad welcome");
        return;
    }
    // the host's slot may not be the guessed one (after a migration): move the channel
    if (hs != m_hostSlot) {
        m_peers[hs] = m_peers[m_hostSlot];
        memset(&m_peers[m_hostSlot], 0, sizeof m_peers[m_hostSlot]);
        // the channel object stays where it is: the peer just points at it
        m_pair[hs] = m_pair[m_hostSlot];
        pairReset(m_hostSlot);
    }
    m_rejoinDirectActive = false;
    memcpy(m_roomKey, roomKey, 32);
    m_hostSlot = hs;
    m_localSlot = slot;
    m_sessionId = sid;
    m_settings = s;
    m_epoch = epoch;
    clearRestartGate();
    m_serial = serial;
    for (int i = 0; i < MP_MAX_PLAYERS; i++) {
        m_seen[i] = seen[i];
        m_seenGen[i] = gens[i];
        noteGen(i, gens[i]);
    }
    m_myOrigin = 0;
    m_myGen = ro[slot].gen ? ro[slot].gen : 0x0101;   // netcore: this membership's generation (a later ROSTER never changes it)
    // core2: this membership's counters start above the last one's (the same key pair in the session: see makeKeys)
    m_ctrBase = CtrBase(m_myGen);
    for (int i = 0; i < MP_MAX_PLAYERS; i++)
        if (m_pair[i].have && m_pair[i].sendCtr <= m_ctrBase) m_pair[i].sendCtr = m_ctrBase + 1;
    m_seen[slot] = 0;
    m_seenGen[slot] = 0;
    m_roster[slot].present = false;
    applyBans(bans, nBans);   // corefix2
    applyRoster(ro, true);
    m_started = (m_roster[hs].state & 3) == 2;
    setPhase(m_started ? MP_UI_PLAYING : MP_UI_LOBBY);
    setActive(true);
    m_rejoinValid = false;
    m_rejoining = false;
    if (m_reconnect) mp_log("reconnected after=%dms", (int)(now() - m_reconnectStart));   // hooks: the scenarios' line (the rejoin worked)
    m_reconnect = false;
    m_error = MP_ERR_NONE;
    pushEvent(MP_EV_JOINED, slot, 0, 0);
    mp_log("joined slot=%d session=%08x host=%d epoch=%d players=%d", slot, (unsigned)sid, hs, epoch, presentCount());
    if (m_ownCode[0]) announceOwnCode();   // core2 (never before the keys: the WELCOME came sealed)
}

void MpCore::removeSlot(int slot, uint8_t reason, const char* why) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS || slot == m_localSlot) return;
    Peer& p = m_peers[slot];
    const bool wasPresent = m_roster[slot].present;
    if (wasPresent) rememberAddr(m_roster[slot].addr, now());
    if (wasPresent && reason != MP_BYE_ERROR) {
        Reservation& rv = m_res[slot];
        rv.used = true;
        memcpy(rv.name, m_roster[slot].name, sizeof rv.name);
        rv.slot = (uint8_t)slot;
        rv.look = m_roster[slot].look;
        rv.consoleId = m_roster[slot].consoleId;
        memcpy(rv.commit, m_roster[slot].commit, 16);   // core2: only the token's holder takes it back
        // corefix D1: a guest removed for silence is probably a link that will come back (its rejoin window is MP_LINKLOST_RECONNECT_MS)
        rv.until = now() + (reason == MP_BYE_TIMEOUT ? MP_LINKLOST_HOLD_MS : MP_REJOIN_MS);
        rv.hold = reason == MP_BYE_TIMEOUT;   // corefix2: a dropped link: the slot is held for its owner
    }
    if ((reason == MP_BYE_ERROR || reason == MP_BYE_RESYNC) && p.used) {
        p.closing = true;   // the goodbye already queued still goes out
        p.closeAt = now() + 1500;
    } else if (p.used) {
        p.rel->reset(0);
        memset(&p, 0, sizeof p);
    }
    if (m_role == ROLE_HOST) noteDeparted(slot);   // (any reason: a console that left is leaving or idle and ignores it)
    if (!p.used) pairReset(slot);   // (a closing peer keeps its key until its goodbye has gone: flushPeers)
    m_roster[slot].present = false;
    purgeSlot(slot);   // (netcore: its tracked events, history entries and dedupe state stay: see historyHas)
    m_ghostMask &= (uint8_t)~(1u << slot);
    if (wasPresent) {
        m_rosterDirty = true;
        pushEvent(MP_EV_LEFT, slot, 0, reason);
        mp_log("slot %d left reason=%s (%s) players=%d", slot, ByeName(reason), why ? why : "", presentCount());
        if (m_conn) m_conn->playersChanged(presentCount());
    }
}

// core2: a console removed (timed out, kicked, an old host taken for gone) that still sends with its old key is told so, sealed, for
// MP_DEPARTED_MS (handleSealed): it rejoins at once instead of learning it from silence
void MpCore::noteDeparted(int slot) {
    if (!keyFor(slot)) return;
    Departed& d = m_departed[slot];
    d.used = true;
    d.slot = (uint8_t)slot;
    d.addr = m_roster[slot].addr;
    memcpy(d.key, m_pair[slot].key, 32);
    d.until = now() + MP_DEPARTED_MS;
    d.lastBye = 0;
}

void MpCore::sendRosterAll() {
    uint8_t buf[MP_MAX_PAYLOAD];
    const uint16_t n = writeRoster(buf, sizeof buf);
    if (!n) return;
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        Peer& p = m_peers[s];
        if (!p.used || p.closing || !p.resumed || s == m_localSlot) continue;
        queueReliable(s, MP_MSG_ROSTER, (uint8_t)m_localSlot, buf, n);
    }
    m_rosterDirty = false;
    m_lastRosterSent = now();
}

void MpCore::sendSnapshotTo(int slot) {
    MpSnapshotImage& im = m_mem->image;
    if (!im.len) return;
    uint8_t buf[MP_MAX_PAYLOAD];
    uint16_t n = mp_snapshot_begin_payload(buf, sizeof buf, im.len, im.crc);
    if (!queueReliable(slot, MP_MSG_SNAPSHOT_BEGIN, (uint8_t)m_localSlot, buf, n)) return;
    for (uint32_t off = 0; off < im.len; off += MP_SNAPSHOT_CHUNK) {
        n = mp_snapshot_chunk_payload(buf, sizeof buf, im.bytes, im.len, off);
        if (!n || !queueReliable(slot, MP_MSG_SNAPSHOT_CHUNK, (uint8_t)m_localSlot, buf, n)) return;
    }
    m_peers[slot].snapVersionSent = im.version;
    m_roster[slot].state = (uint8_t)(m_roster[slot].state & ~3);   // joining until SNAPSHOT_DONE
    mp_log("snapshot sent slot=%d bytes=%u crc=%08x", slot, (unsigned)im.len, (unsigned)im.crc);
}

// ---- host migration (DESIGN.md section 8) ----------------------------------------------------------------------------------
void MpCore::handleHostChangePacket(const MpAddr& from, const uint8_t* hdr, const uint8_t* payload, uint16_t plen, uint64_t t) {
    if (m_localSlot < 0 || (m_phase != MP_UI_LOBBY && m_phase != MP_UI_PLAYING)) return;
    MpReader r(payload, plen);
    const uint16_t epoch = r.u16();
    const uint8_t x = r.u8();
    r.u16();   // serial (taken at delivery)
    const uint32_t sid = r.u32();
    uint16_t seen[MP_MAX_PLAYERS];
    MpGen gens[MP_MAX_PLAYERS];
    for (int i = 0; i < MP_MAX_PLAYERS; i++) seen[i] = r.u16();
    for (int i = 0; i < MP_MAX_PLAYERS; i++) gens[i] = r.u16();
    if (!r.done() || x >= MP_MAX_PLAYERS || x != hdr[4] || x == m_localSlot || sid != m_sessionId || !m_roster[x].present ||
        hdr[2] != MP_PROTO_VERSION) {
        m_statStray++;
        return;
    }
    // corefix 5: the claimant must be at its roster address (never "no address known": that skipped the check)
    if (!m_roster[x].addr.host || m_roster[x].addr != from) {
        m_statStray++;
        return;
    }
    bool accept = false;
    if (m_role == ROLE_GUEST) {
        const bool hostGone = m_hostLeaving || (m_hostSlot >= 0 && m_peers[m_hostSlot].used &&
                                                t - m_peers[m_hostSlot].lastRecv >= MP_HOST_LOST_MS / 2);
        // corefix 5: the next epoch only, from the successor in line: the same order pumpMigration uses (the k-th present slot after
        // the host claims k steps after the host went), so a present guest cannot make itself host by claiming early or with a jump
        // in the epoch; a successor further down the line is accepted only once the ones before it had their turn (it is then the
        // documented fallback for a successor that is gone too)
        if ((uint16_t)(epoch - m_epoch) == 1 && hostGone && m_hostSlot >= 0 && m_peers[m_hostSlot].used) {
            int rank = 0;
            for (int s = 0; s < MP_MAX_PLAYERS; s++)
                if (s != m_hostSlot && s != x && m_roster[s].present && s < x) rank++;
            const uint64_t goneAt = m_hostLeaving ? m_hostGoneAt : m_peers[m_hostSlot].lastRecv + MP_HOST_LOST_MS;
            if (rank == 0 || t + MP_MIGRATE_STEP_MS / 2 >= goneAt + (uint64_t)rank * MP_MIGRATE_STEP_MS) accept = true;
        } else if (epoch == m_epoch && m_epoch > 0 && x < m_hostSlot && m_hostSlot >= 0 && m_peers[m_hostSlot].used &&
                   t - m_peers[m_hostSlot].createdAt < MP_CLAIM_MS + MP_RESUME_WAIT_MS) {
            accept = true;   // two claims for one epoch: lower slot wins (only while the migration is fresh)
        }
    } else if (m_role == ROLE_HOST) {
        // an alive host never steps down for a later epoch (that claimant lost us, not the other way round); two claimants of
        // one epoch: the lower slot wins (only while our own claim is fresh)
        if (epoch == m_epoch && m_epoch > 0 && x < m_localSlot && t - m_claimAt < MP_CLAIM_MS + MP_RESUME_WAIT_MS) accept = true;
    }
    if (!accept) {
        m_statStray++;   // (not acknowledged: the claimant hears nothing from us)
        return;
    }
    const uint16_t seq = (uint16_t)(hdr[6] | (hdr[7] << 8));
    followHost(x, from, epoch, seen, gens, seq, t);
    handleFromPeer(x, hdr, payload, plen, t);
}

void MpCore::followHost(int x, const MpAddr& addr, uint16_t epoch, const uint16_t* seen, const MpGen* gens, uint16_t hcSeq, uint64_t t) {
    const int old = m_hostSlot;
    const bool wasHost = m_role == ROLE_HOST;
    m_starved = m_starveStale = 0;   // corefix2
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        if (m_peers[s].used) m_peers[s].rel->reset(0);
        memset(&m_peers[s], 0, sizeof m_peers[s]);
    }
    // netcore: a switch within one epoch (two claimants, the lower slot won) leaves the other claimant in: it steps down and follows x
    // too (marking it gone made it "rejoin" at the next ROSTER, and its events were re-sent as a dead host's)
    if (old >= 0 && old != x && old != m_localSlot && m_roster[old].present && epoch != m_epoch) {
        rememberAddr(m_roster[old].addr, t);   // netcore (split heal: see applyRoster)
        m_roster[old].present = false;
        pairReset(old);   // core2
        purgeSlot(old);
        // corefix 4: what the dead host relayed to only some of us stays in our history (and its seen[] counter stays) until the
        // next scene: the new host needs it to serve the guests that missed it, and the dedupe must still know what we delivered
        m_ghostMask |= (uint8_t)(1u << old);
        pushEvent(MP_EV_LEFT, old, 0, MP_BYE_HOST_LEAVING);
    }
    m_role = ROLE_GUEST;
    m_claiming = false;
    m_hostLeaving = false;
    m_hostSlot = x;
    m_epoch = epoch;
    m_hostAddr = addr;
    Peer& p = m_peers[x];
    p.used = true;
    p.resumed = true;
    p.addr = addr;
    p.rel = relFor(x);
    p.rel->reset((uint16_t)rnd());
    p.rel->startRecv(hcSeq);
    p.lastRecv = t;
    p.createdAt = t;
    p.srtt = -1;
    m_mem->rx.reset();
    // RESUME first, then my own events the new host has not seen
    uint8_t buf[48];   // (37 bytes: epoch, serial, finished, turns, seen[4], gens[4] u16, crc, console id)
    MpWriter w(buf, sizeof buf);
    w.u16(m_epoch);
    w.u16(m_serial);
    w.u8(m_raceFinished ? 1 : 0);
    w.u32(m_raceTurns);
    for (int s = 0; s < MP_MAX_PLAYERS; s++) w.u16(seenOf(s));
    for (int s = 0; s < MP_MAX_PLAYERS; s++) w.u16(genOf(s));   // netcore: (gen, seq) per origin; genOf(own slot) = this membership
    w.u32(m_mem->image.len ? m_mem->image.crc : 0);
    w.u32(m_wireId);   // fix3: the new host adopted us without a HELLO: this is how it can still ban this console
    queueReliable(x, MP_MSG_RESUME, (uint8_t)m_localSlot, buf, (uint16_t)w.n);
    // netcore: own events of this membership the new host has not delivered: from the one after its (gen, seq) if it knows this
    // membership, else from the first. If the history no longer reaches back that far, the new host (and through it everyone) would
    // miss some of them for good: this membership ends instead (rejoin: a new one, its stream starts over).
    if (m_myOrigin) {
        const bool known = gens[m_localSlot] == m_myGen;
        const uint16_t next = (uint16_t)(seen[m_localSlot] + 1);
        const uint16_t first = known ? (next ? next : 1) : 1;
        if ((!known || SeqDiff(m_myOrigin, seen[m_localSlot]) > 0) && !historyHas((uint8_t)m_localSlot, m_myGen, first)) {
            mp_log("following new host slot=%d: own events from seq %u are no longer in the history: rejoining", x, (unsigned)first);
            requestEnd(MP_ERR_TIMEOUT, "events lost in the host change", true);
            return;
        }
    }
    int n = 0;
    MpCoreMem& m = *m_mem;
    for (int i = 0; i < m.hCount; i++) {
        const HistEntry& h = m.hist[(m.hHead + i) % MP_HISTORY_ENTRIES];
        // corefix 4: also what the dead host sent us and the new host has not seen (it may have reached only some of the guests)
        const bool ghost = h.origin < MP_MAX_PLAYERS && (m_ghostMask & (1u << h.origin)) && h.origin != m_localSlot;
        if (h.origin != m_localSlot && !ghost) continue;
        if (h.origin == m_localSlot && h.gen != m_myGen) continue;   // (an earlier membership of this console: over)
        const MpGen hg = gens[h.origin];
        if (hg) {
            if ((h.gen != hg && !GenNewer(h.gen, hg)) || (h.gen == hg && SeqDiff(h.seq, seen[h.origin]) <= 0)) continue;
        }
        queueGameReliable(x, h.type, h.origin, h.seq, h.gen, h.data, h.len);
        n++;
    }
    if (m_conn) m_conn->hostChanged(x, false);
    pushEvent(MP_EV_HOST_CHANGED, x, old, 0);
    mp_log("following new host slot=%d epoch=%d%s resent=%d", x, epoch, wasHost ? " (stepped down)" : "", n);
}

// corefix2: begin the claim towards slot s on window rel: the HOST_CHANGE goes out reliably, the guest answers with its RESUME
void MpCore::startClaimPeer(int s, MpReliable* rel, uint64_t t) {
    Peer& p = m_peers[s];
    memset(&p, 0, sizeof p);
    p.used = true;
    p.resumed = false;
    p.addr = m_roster[s].addr;
    p.rel = rel;
    p.rel->reset((uint16_t)rnd());
    p.lastRecv = t;
    p.createdAt = m_takeoverAt;   // (the 6 s to follow run from the claim, not from this window)
    p.winAt = t;
    p.ackAt = t;
    p.srtt = -1;
    uint8_t buf[40];   // (25 bytes)
    MpWriter w(buf, sizeof buf);
    w.u16(m_epoch);
    w.u8((uint8_t)m_localSlot);
    w.u16(m_serial);
    w.u32(m_sessionId);
    for (int k = 0; k < MP_MAX_PLAYERS; k++) w.u16(seenOf(k));
    for (int k = 0; k < MP_MAX_PLAYERS; k++) w.u16(genOf(k));
    queueReliable(s, MP_MSG_HOST_CHANGE, (uint8_t)m_localSlot, buf, (uint16_t)w.n);
    m_starved &= (uint8_t)~(1u << s);
    m_starveStale &= (uint8_t)~(1u << s);
}

// corefix2: a window for starved slot s: a free one, else a new one, else the one of the guest that has had its turn and not answered
// (never a guest that follows). False: nothing to take yet (retried every MP_STARVE_ROTATE_MS and when s talks).
bool MpCore::grantWindow(int s, uint64_t t) {
    if (s < 0 || s >= MP_MAX_PLAYERS || m_peers[s].used || !m_roster[s].present || !m_mem) return false;
    MpReliable* rel = relFor(s, true);
    if (!rel) {
        int victim = -1;
        for (int q = 0; q < MP_MAX_PLAYERS; q++) {
            const Peer& v = m_peers[q];
            if (q == s || q == m_localSlot || !v.used || v.closing) continue;
            const bool unanswered = !v.resumed && Since(t, v.winAt) >= MP_STARVE_ROTATE_MS;
            const bool dead = v.resumed && Since(t, v.lastRecv) >= MP_HOST_LOST_MS;
            if (!unanswered && !dead) continue;
            if (victim < 0 || v.winAt < m_peers[victim].winAt) victim = q;
        }
        if (victim < 0) return false;
        mp_log("takeover: the window of slot %d (no answer) goes to slot %d", victim, s);
        rel = m_peers[victim].rel;
        rel->reset(0);
        memset(&m_peers[victim], 0, sizeof m_peers[victim]);
        m_starved |= (uint8_t)(1u << victim);   // (it gets one again in its turn, or when it talks)
    }
    startClaimPeer(s, rel, t);
    return true;
}

// corefix2: the starved slots of a takeover. Not heard-before-the-loss slots wait for their own word; the others take turns.
void MpCore::tendStarved(uint64_t t) {
    if (!m_starved) return;
    for (int s = 0; s < MP_MAX_PLAYERS; s++)
        if ((m_starved >> s & 1) && (m_peers[s].used || !m_roster[s].present)) m_starved &= (uint8_t)~(1u << s);
    if (Since(t, m_takeoverAt) > MP_RESUME_WAIT_MS) {
        for (int s = 0; s < MP_MAX_PLAYERS && m_starved; s++)
            if (m_starved >> s & 1) {
                m_starved &= (uint8_t)~(1u << s);
                removeSlot(s, MP_BYE_TIMEOUT, "did not follow the new host");
            }
        return;
    }
    if (t < m_starveNext) return;
    m_starveNext = t + MP_STARVE_ROTATE_MS;
    for (int s = 0; s < MP_MAX_PLAYERS; s++)
        if ((m_starved >> s & 1) && !(m_starveStale >> s & 1)) grantWindow(s, t);
}

void MpCore::becomeHost(uint64_t t) {
    const int old = m_hostSlot;
    const uint64_t lastHost = (old >= 0 && m_peers[old].used) ? (m_hostLeaving ? m_hostGoneAt : m_peers[old].lastRecv) : t;
    m_epoch++;
    mp_log("host lost: slot %d takes over epoch=%d (old host %d)", m_localSlot, m_epoch, old);
    // netx: the followers re-send everything the old host never acknowledged, and the catch-up runs both ways: that burst (80+
    // messages in a second under loss) is not a flood, so the rate cap waits a moment (found when faster discovery shifted the timing
    // of core_tests' "host drop, 10% loss": a follower was kicked for "rate" right after the takeover)
    m_rateGraceUntil = t + 3000;
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        if (m_peers[s].used) m_peers[s].rel->reset(0);
        memset(&m_peers[s], 0, sizeof m_peers[s]);
    }
    if (old >= 0 && old != m_localSlot && m_roster[old].present) {
        Reservation& rv = m_res[old];
        rv.used = true;
        memcpy(rv.name, m_roster[old].name, sizeof rv.name);
        rv.slot = (uint8_t)old;
        rv.look = m_roster[old].look;
        rv.consoleId = m_roster[old].consoleId;
        memcpy(rv.commit, m_roster[old].commit, 16);   // core2: only the token's holder takes it back
        rv.until = t + MP_REJOIN_MS;
        rv.hold = !m_hostLeaving;   // corefix2: a host that vanished (not one that said goodbye) is held for
        rememberAddr(m_roster[old].addr, t);   // netcore (split heal: see applyRoster)
        m_roster[old].present = false;
        noteDeparted(old);   // core2: the old host, if it is still there and talks to us, is told to rejoin
        pairReset(old);   // core2
        purgeSlot(old);
        m_ghostMask |= (uint8_t)(1u << old);   // corefix 4: its history stays (see followHost)
        pushEvent(MP_EV_LEFT, old, 0, MP_BYE_HOST_LEAVING);
    }
    m_role = ROLE_HOST;
    clearRestartGate();
    m_hostSlot = m_localSlot;
    m_hostLeaving = false;
    m_claiming = true;
    m_claimAt = t;
    m_lastOldHost = (uint8_t)old;
    m_mem->rx.reset();
    if (m_conn && m_conn->session()) m_hostAddr = m_conn->session()->local();
    m_roster[m_localSlot].addr = m_hostAddr;
    // corefix2: a takeover never fails for lack of a window. The slots are served in the order they were heard (the freshest relayed
    // state first); a slot not heard before the host was lost gets no window until it talks to us itself; a slot that finds no window
    // is "starved" and tendStarved hands windows on (from a guest that has not answered) until it does.
    m_takeoverAt = t;
    m_starved = m_starveStale = 0;
    m_starveNext = t + MP_STARVE_ROTATE_MS;
    int order[MP_MAX_PLAYERS], nOrder = 0;
    for (int s = 0; s < MP_MAX_PLAYERS; s++)
        if (s != m_localSlot && m_roster[s].present) order[nOrder++] = s;
    const uint64_t quietFrom = lastHost > MP_HOST_LOST_MS ? lastHost - MP_HOST_LOST_MS : 0;   // (heard within the silence limit of the host's last word)
    auto stale = [&](int s) { return m_lastUAt[s] != 0 && m_lastUAt[s] < quietFrom; };
    for (int i = 1; i < nOrder; i++) {   // (insertion sort: freshest first, never-heard-of after the heard, stale last)
        const int v = order[i];
        int j = i - 1;
        while (j >= 0) {
            const int a = order[j];
            const bool later = (stale(a) != stale(v)) ? stale(a) : (m_lastUAt[a] < m_lastUAt[v]);
            if (!later) break;
            order[j + 1] = a;
            j--;
        }
        order[j + 1] = v;
    }
    int others = 0;
    for (int i = 0; i < nOrder; i++) {
        const int s = order[i];
        others++;
        if (stale(s)) {
            m_starved |= (uint8_t)(1u << s);
            m_starveStale |= (uint8_t)(1u << s);
            mp_log("takeover: slot %d was not heard before the host was lost: no window until it talks", s);
            continue;
        }
        MpReliable* rel = relFor(s, true);
        if (!rel) {
            m_starved |= (uint8_t)(1u << s);
            mp_log("takeover: no reliable window for slot %d yet (a window is handed on from a guest that does not answer)", s);
            continue;
        }
        startClaimPeer(s, rel, t);
    }
    if (!others) {
        requestEnd(MP_ERR_HOST_LEFT, "the host left and nobody else is here");
        return;
    }
    m_lastBeacon = 0;
    if (m_conn) m_conn->hostChanged(m_localSlot, true);
    pushEvent(MP_EV_BECAME_HOST, m_localSlot, old, 0);
}

void MpCore::onResume(int slot, const uint8_t* data, uint16_t len) {
    MpReader r(data, len);
    const uint16_t epoch = r.u16();
    const uint16_t serial = r.u16();
    const uint8_t finished = r.u8();
    const uint32_t turns = r.u32();
    uint16_t seen[MP_MAX_PLAYERS];
    MpGen gens[MP_MAX_PLAYERS];
    for (int i = 0; i < MP_MAX_PLAYERS; i++) seen[i] = r.u16();
    for (int i = 0; i < MP_MAX_PLAYERS; i++) gens[i] = r.u16();
    const uint32_t crc = r.u32();
    const uint32_t consoleId = r.u32();   // fix3
    Peer& p = m_peers[slot];
    if (!r.done() || epoch != m_epoch || finished > 1) {
        bad(slot, "resume");
        return;
    }
    if (p.resumed) return;
    // netcore: the follower's own membership is the one its events carry (this host may have missed its rejoin); and every newer
    // generation it knows of raises this host's floor for the next join of that slot
    // (bounded: a follower is at most a few rejoins ahead of this host; a gen far ahead would wrap the 8-bit order for everyone and
    // make a later joiner's events look older than its predecessor's, i.e. muted)
    for (int i = 0; i < MP_MAX_PLAYERS; i++) {
        const MpGen known = m_genFloor[i] ? m_genFloor[i] : m_roster[i].gen;
        const int ahead = GenAhead(gens[i], known);
        if (gens[i] && (!known || (ahead > 0 && ahead <= 16))) noteGen(i, gens[i]);
    }
    if (gens[slot] && (gens[slot] == m_roster[slot].gen || m_genFloor[slot] == gens[slot])) m_roster[slot].gen = gens[slot];
    if (!sendCatchUp(slot, seen, gens)) {
        uint8_t bye = MP_BYE_RESYNC;
        queueReliable(slot, MP_MSG_BYE, (uint8_t)m_localSlot, &bye, 1);
        removeSlot(slot, MP_BYE_RESYNC, "cannot be caught up");
        return;
    }
    p.resumed = true;
    p.consoleId = consoleId;
    m_roster[slot].consoleId = consoleId;
    m_claiming = false;   // somebody follows: the migration holds
    mp_log("slot %d resumed epoch=%d serial=%u console=%08x", slot, epoch, (unsigned)serial, (unsigned)consoleId);
    if (m_haveWhere) {
        uint8_t buf[10] = {0, 0};
        memcpy(buf + 2, m_where, 8);
        queueReliable(slot, MP_MSG_WHERE, (uint8_t)m_localSlot, buf, 10);
    }
    m_restartGateMask &= (uint8_t)~(1u << slot);
    m_restartGateUntil[slot] = 0;
    if ((m_restartMissedMask & (1u << slot)) && m_serial != 0 && m_serial == m_restartSerial && serial == m_restartSerial && m_settings.mode == MP_MODE_TOGETHER) {
        uint8_t rb[2] = {(uint8_t)(m_restartSerial & 0xFFu), (uint8_t)(m_restartSerial >> 8)};
        queueGameReliable(slot, MP_MSG_RESTART, (uint8_t)m_localSlot, 0, m_myGen, rb, 2);
        m_restartGateMask |= (uint8_t)(1u << slot);
        m_restartGateUntil[slot] = now() + 20000;
        mp_log("slot %d resumed after missed group restart serial=%u: sent RESTART", slot, (unsigned)m_restartSerial);
    } else {
        m_restartMissedMask &= (uint8_t)~(1u << slot);
    }
    if (m_mem->image.len && crc != m_mem->image.crc) sendSnapshotTo(slot);
    m_rosterDirty = true;
    pushEvent(MP_EV_RESUMED, slot, 0, turns);
    m_events[(m_evHead + m_evCount - 1) % MP_EVENT_QUEUE].finished = finished;
    m_events[(m_evHead + m_evCount - 1) % MP_EVENT_QUEUE].serial = serial;
}

void MpCore::pumpMigration(uint64_t t) {
    if (m_role != ROLE_GUEST || m_localSlot < 0 || m_hostSlot < 0) return;
    if (m_phase != MP_UI_LOBBY && m_phase != MP_UI_PLAYING) return;
    const Peer& h = m_peers[m_hostSlot];
    if (!h.used) return;
    const bool silent = t - h.lastRecv >= MP_HOST_LOST_MS;
    if (!m_hostLeaving && !silent) return;
    const uint64_t goneAt = m_hostLeaving ? m_hostGoneAt : h.lastRecv + MP_HOST_LOST_MS;
    if (m_connKind == MP_CONN_DIRECT) {
        // netx: by address nobody can take over (the guests cannot reach each other through their NATs). A host that said goodbye
        // ends it; a silent one may be a hiccup on the way: the reconnect path asks the same address again for up to 30 s.
        requestEnd(MP_ERR_HOST_LEFT, m_hostLeaving ? "the host left" : "the host is silent", !m_hostLeaving);
        return;
    }
    int rank = 0, others = 0;
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        if (s == m_hostSlot || s == m_localSlot || !m_roster[s].present) continue;
        others++;
        if (s < m_localSlot) rank++;
    }
    if (!others) {
        // corefix D1: nobody else is here and the host never said goodbye: our own link is the first suspect (a Wi-Fi stall the OS did
        // not report). Keep the rejoin token and ask the same session again for MP_LINKLOST_RECONNECT_MS (RECONNECTING meanwhile).
        if (m_hostLeaving) requestEnd(MP_ERR_HOST_LEFT, "the host left");
        else requestEnd(MP_ERR_NO_WIFI, "the host is silent and nobody else is here: our link?", true, mp_linklost_window_ms());
        return;
    }
    if (t >= goneAt + (uint64_t)rank * MP_MIGRATE_STEP_MS) becomeHost(t);
}

// ---- timers ----------------------------------------------------------------------------------------------------------------
void MpCore::sendBeacon(uint64_t t) {
    if (m_connKind != MP_CONN_WIFI && m_connKind != MP_CONN_NEARBY && m_connKind != MP_CONN_LOOPBACK) return;
    if (t - m_lastBeacon < MP_BEACON_MS && m_lastBeacon) return;
    m_lastBeacon = t;
    uint8_t buf[MP_BEACON_BYTES];
    const size_t n = buildBeacon(buf, sizeof buf);
    MpAddr to;
    to.host = MP_ADDR_BROADCAST;
    to.port = MP_LAN_PORT;
    if (n) txSend(to, buf, n);
}

// netx: a scanner's probe (mp_lan.h): this host's beacon, unicast to the prober's discovery socket. At most MP_PROBE_ANSWERS_PER_SEC
// answers a second (a flood of probes costs a stranger more than it costs us); only open rooms of the kinds that beacon.
void MpCore::answerProbe(const MpAddr& from, uint64_t t) {
    if (m_connKind != MP_CONN_WIFI && m_connKind != MP_CONN_NEARBY && m_connKind != MP_CONN_LOOPBACK) return;
    if (from.host == MP_ADDR_BROADCAST || from.port != MP_LAN_PORT || !m_conn || !m_conn->session()) return;
    if (t - m_probeWindow >= 1000) {
        m_probeWindow = t;
        m_probeAnswers = 0;
    }
    if (m_probeAnswers >= MP_PROBE_ANSWERS_PER_SEC) return;
    m_probeAnswers++;
    uint8_t buf[MP_BEACON_BYTES];
    const size_t n = buildBeacon(buf, sizeof buf);
    if (n) txSend(from, buf, n);
}

size_t MpCore::buildBeacon(uint8_t* buf, size_t cap) {
    MpBeacon b;
    memset(&b, 0, sizeof b);
    b.proto = MP_PROTO_VERSION;
    b.mode = m_settings.mode;
    b.players = (uint8_t)presentCount();
    if (m_role == ROLE_HOST) {
        // core2 (claims audit 1): only the players who follow this host count: a claimant's roster still lists everyone (a console that
        // woke from a freeze and claimed made the healthy host "behind" and merged it away)
        int n = 1;
        for (int s2 = 0; s2 < MP_MAX_PLAYERS; s2++)
            if (s2 != m_localSlot && m_roster[s2].present && m_peers[s2].used && m_peers[s2].resumed) n++;
        b.players = (uint8_t)n;
    }
    b.maxPlayers = m_settings.maxPlayers;
    b.flags = (uint8_t)((b.players < b.maxPlayers && (m_phase == MP_UI_LOBBY || (m_settings.flags & 1))) ? 1 : 0);
    b.hostSlot = (uint8_t)m_localSlot;
    b.epoch = m_epoch;
    b.fingerprint = m_fingerprint;
    b.sessionId = m_sessionId;
    b.seq = ++m_beaconSeq;
    memcpy(b.name, m_name, sizeof b.name);
    if (m_gathering && !m_started) b.flags |= MP_BEACON_GATHERING;
    if (m_started) b.flags |= MP_BEACON_STARTED;
    if (m_gathering && !m_started && m_closingInto) {
        b.flags |= MP_BEACON_CLOSING;
        b.closingInto = m_closingInto;
    }
    const size_t n = mp_beacon_write(buf, cap, b);
    // core2: the room key's tag: a host of this session elsewhere (a split) can tell this beacon from a forged one (checkMerge)
    if (n == MP_BEACON_BYTES && memcmp(m_roomKey, kZeroKey, 32) != 0) mpseal::Mac(buf + MP_BEACON_SIGNED, MP_BEACON_TAG, m_roomKey, 32, buf, MP_BEACON_SIGNED);
    return n;
}

// Split sessions: a silent partition (or heavy loss) can make some guests migrate while the old host is alive, so that two rooms of
// ONE session (same sessionId) run side by side, each under its own host, for ever (the old host never hears of the claim: its
// former guests are gone, and an alive host never steps down for a claim it was not told about). Both hosts keep broadcasting
// beacons: a host that hears a beacon of its own session from another room that is ahead of its own (more players; then the later
// epoch; then the lower host slot: a total order, so exactly one room is ahead of all the others) joins that room as a guest
// (the reconnect path: same session, same name), its own guests following by the usual host migration. Only open rooms.
void MpCore::checkMerge(uint64_t t) {
    if (!m_conn) return;
    m_scan.expire(t);
    MpTransport* d = m_conn->discovery();
    if (!d) return;
    const bool may = !m_dying && !m_claiming && m_localSlot >= 0 && (m_phase == MP_UI_LOBBY || m_phase == MP_UI_PLAYING);
    uint8_t buf[128];
    MpAddr from;
    for (int i = 0; i < 64; i++) {
        const int n = d->recv(&from, buf, sizeof buf);
        if (n <= 0) break;
        if (mp_probe_read(buf, (size_t)n)) {   // netx: a scanner asks: answer it directly
            if (!m_dying && m_localSlot >= 0 && (m_phase == MP_UI_LOBBY || m_phase == MP_UI_PLAYING)) answerProbe(from, t);
            continue;
        }
        MpBeacon b;
        if (!mp_beacon_read(buf, (size_t)n, &b)) continue;
        // core2: the host keeps a list of the OTHER rooms it hears (gathering merges, "X is playing": heardRooms)
        if (b.sessionId != m_sessionId && from != m_hostAddr) m_scan.add(from, b, t, m_fingerprint);
        if (!may) continue;
        if (b.sessionId != m_sessionId || b.fingerprint != m_fingerprint || from == m_hostAddr || !(b.flags & 1)) continue;
        const int mine = presentCount();
        const bool ahead = b.players > mine || (b.players == mine && (b.epoch > m_epoch || (b.epoch == m_epoch && b.hostSlot < m_localSlot)));
        if (m_mergeCount && from.host == m_mergeAddr.host && !ahead) m_mergeCount = 0;   // (the room changed its mind: start over)
        if (!ahead) continue;
        // core2: only a beacon tagged with this session's room key (a member made it: the key travels only sealed, in the WELCOME)
        {
            uint8_t want[MP_BEACON_TAG];
            mpseal::Mac(want, sizeof want, m_roomKey, 32, buf, MP_BEACON_SIGNED);
            uint8_t diff = b.tagged ? 0 : 1;
            for (int k = 0; k < MP_BEACON_TAG; k++) diff |= (uint8_t)(want[k] ^ b.tag[k]);
            if (diff || !memcmp(m_roomKey, kZeroKey, 32)) {
                if (m_statStray++ % 64 == 0) mp_log("merge: a beacon of this session without the room key's tag is ignored");
                continue;
            }
        }
        // corefix 3: one forged broadcast beacon (the session id and fingerprint are public) must not end a healthy session: the beacon
        // must come from the address of a member of this session (present, or removed less than MP_MERGE_MEMORY_MS ago), and
        // MP_MERGE_BEACONS consecutive beacons over MP_MERGE_SPAN_MS must agree
        if (!vouchedAddr(from, t)) {
            if (m_statStray++ % 64 == 0) mp_log("merge: a beacon of this session from an unknown address %08x is ignored", (unsigned)from.host);
            continue;
        }
        if (!m_mergeCount || from.host != m_mergeAddr.host || t - m_mergeLast > 4 * MP_BEACON_MS) {
            m_mergeAddr = from;
            m_mergeFirst = t;
            m_mergeSeq = b.seq;
            m_mergeCount = 1;
        } else if ((int32_t)(b.seq - m_mergeSeq) > 0) {   // (ad hoc sends every beacon twice: a copy is not another beacon)
            m_mergeSeq = b.seq;
            m_mergeCount++;
        }
        m_mergeLast = t;
        if (m_mergeCount < MP_MERGE_BEACONS || t - m_mergeFirst < MP_MERGE_SPAN_MS) continue;
        mp_log("merge: a room of this session is ahead (players %d vs %d, epoch %u vs %u): joining it", (int)b.players, mine, (unsigned)b.epoch, (unsigned)m_epoch);
        m_mergeCount = 0;
        requestEnd(MP_ERR_TIMEOUT, "merging into the session's other room", true);
        return;
    }
}

void MpCore::saveRejoinPeers() {
    memset(m_rejoinPeers, 0, sizeof m_rejoinPeers);
    for (int s = 0; s < MP_MAX_PLAYERS; s++) m_rejoinPeers[s] = (s != m_localSlot && m_roster[s].present) ? m_roster[s].addr.host : 0;
    if (m_hostAddr.host) m_rejoinPeers[m_hostSlot >= 0 && m_hostSlot < MP_MAX_PLAYERS ? m_hostSlot : 0] = m_hostAddr.host;
    // (members that left in the last minute count too: after a split the room to merge into is made of the ones that were removed)
    const uint64_t t = now();
    for (int i = 0; i < 8; i++)
        if (m_past[i].addr.host && t <= m_past[i].until) m_rejoinPeers[MP_MAX_PLAYERS + i] = m_past[i].addr.host;
}

void MpCore::rememberAddr(const MpAddr& a, uint64_t t) {
    if (!a.host || a.host == MP_ADDR_BROADCAST) return;
    for (int i = 0; i < 8; i++)
        if (m_past[i].addr.host == a.host) { m_past[i].addr = a; m_past[i].until = t + MP_MERGE_MEMORY_MS; return; }
    m_past[m_pastNext].addr = a;
    m_past[m_pastNext].until = t + MP_MERGE_MEMORY_MS;
    m_pastNext = (m_pastNext + 1) % 8;
}

bool MpCore::vouchedAddr(const MpAddr& a, uint64_t t) const {
    if (!a.host || a.host == MP_ADDR_BROADCAST) return false;
    for (int s = 0; s < MP_MAX_PLAYERS; s++)
        if (s != m_localSlot && m_roster[s].present && m_roster[s].addr == a) return true;   // corefix2: host AND port
    for (int i = 0; i < 8; i++)
        if (m_past[i].addr == a && t <= m_past[i].until) return true;   // (corefix2: host AND port)
    return false;
}

// Elapsed time that never wraps when the stamp is later than t.

void MpCore::pumpTimers(uint64_t t) {
    tendSpareWindows(t);   // netopt
    switch (m_phase) {
    case MP_UI_SIGNING_IN:
    case MP_UI_CREATING:
    case MP_UI_JOINING:
    case MP_UI_SEARCHING:
        if (Since(t, m_phaseAt) > MP_LINK_TIMEOUT_MS) requestEnd(MP_ERR_TIMEOUT, "bring-up took too long");
        return;
    case MP_UI_CONNECTING:
        if (Since(t, m_phaseAt) > MP_CONNECT_TIMEOUT_MS) {
            if (m_rejoining && m_wantSession && m_rejoinDirectActive) {
                m_rejoinDirectActive = false;
                for (int s = 0; s < MP_MAX_PLAYERS; s++) {
                    if (m_peers[s].used && m_peers[s].rel) m_peers[s].rel->reset(0);
                    memset(&m_peers[s], 0, sizeof m_peers[s]);
                    pairReset(s);
                }
                m_haveCookie = false;
                mp_log("rejoin: direct reconnect timed out, returning to beacon listen");
                setPhase(MP_UI_JOINING);
                m_phaseAt = m_rejoinJoinAt ? m_rejoinJoinAt : t;
            } else {
                requestEnd(MP_ERR_TIMEOUT, "no answer from the host");
            }
        }
        else if (!m_haveCookie && Since(t, m_hello1At) >= 300) sendHello1(t);   // core2
        else if (m_haveCookie && Since(t, m_hello1At) >= MP_COOKIE_RETRY_MS) {
            // core2: a cookie but no WELCOME: the console that answered was not the room (an old room at that address that is going,
            // a host that restarted): the attempt starts over, within the same overall timeout
            const uint64_t since = m_phaseAt;
            for (int s = 0; s < MP_MAX_PLAYERS; s++) {
                if (m_peers[s].used) m_peers[s].rel->reset(0);
                memset(&m_peers[s], 0, sizeof m_peers[s]);
                pairReset(s);
            }
            mp_log("no welcome after the cookie: asking again");
            startConnecting(m_connectTarget, m_connectGuess, t);
            m_phaseAt = since;
        }
        return;
    case MP_UI_LEAVING: {
        bool done = true;
        for (int s = 0; s < MP_MAX_PLAYERS; s++)
            if (m_peers[s].used && !m_peers[s].rel->everythingAcked()) done = false;
        if (done || Since(t, m_phaseAt) > MP_LEAVE_FLUSH_MS) {
            requestEnd(MP_ERR_NONE, "left");
        }
        return;
    }
    case MP_UI_LOBBY:
    case MP_UI_PLAYING:
        break;
    default:
        return;
    }
    if (m_role == ROLE_HOST) {
        for (int s = 0; s < MP_MAX_PLAYERS && !m_dying; s++) {
            Peer& p = m_peers[s];
            if (!p.used || p.closing || s == m_localSlot) continue;
            if (p.rel->unacked() == 0) p.ackAt = t;
            if (!p.resumed && Since(t, p.createdAt) > MP_RESUME_WAIT_MS) removeSlot(s, MP_BYE_TIMEOUT, "did not follow the new host");
            else if (Since(t, p.lastRecv) > MP_PEER_LOST_MS) removeSlot(s, MP_BYE_TIMEOUT, "lost");
            // netcore: only a peer that never acknowledged anything (a ghost joiner: a HELLO that never completes the join). An
            // established player whose console froze (suspend, a long Wi-Fi gap) keeps its slot up to MP_PEER_LOST_MS like any silent
            // peer, so it can come back and catch up (it was removed after 5 s, with no rejoin: scenario mp_silence6)
            else if (p.resumed && !p.established && p.rel->unacked() > 0 && Since(t, p.ackAt) > MP_ACK_STALL_MS)
                // corefix: a joiner that never acknowledged its WELCOME (or a guest that stopped acknowledging) holds no slot for ever
                // just by sending something now and then: the head of our reliable queue must make progress
                removeSlot(s, MP_BYE_TIMEOUT, "no acknowledgement");
            else if (p.resumed && p.established && p.rel->unacked() > 0 && Since(t, p.ackAt) > (p.busyAt && Since(p.lastRecv, p.busyAt) < MP_BUSY_FRESH_MS ? MP_ESTAB_NOACK_MS : MP_PEER_LOST_MS))
                // (an established peer whose address still sends but acknowledges nothing: 15 s, or 60 s while its LAST packet still said busy (MP_HDR_BUSY within MP_BUSY_FRESH_MS of it: its game
                // loads with a full receive queue, see MP_ESTAB_NOACK_MS). Measured at the last packet, not at now: a peer that goes quiet falls to the silence rule like everyone else)
                removeSlot(s, MP_BYE_TIMEOUT, "no acknowledgement");
        }
        tendStarved(t);   // corefix2
        if (m_claiming && Since(t, m_claimAt) > MP_CLAIM_MS) {
            // corefix D1: nobody answered our claim: most likely it is our own link that is gone (the others never heard us)
            requestEnd(MP_ERR_NO_WIFI, "nobody followed the new host: our link?", true, mp_linklost_window_ms());
            return;
        }
        if (m_rosterDirty || t - m_lastRosterSent > 2000) sendRosterAll();
        sendBeacon(t);
    } else {
        pumpMigration(t);
    }
    if (t - m_lastStatusLog > 30000) logStatus(t);
    logNetLink(t);   // netlog
    logRtt(t);       // mp3c24
    diagTick();      // beta diagnostics
}

int MpCore::txSend(const MpAddr& to, const void* data, size_t len) {
    const int r = m_conn->session()->send(to, data, len);
    if (r >= 0) { m_nl.txOk++; m_nl.txBytes += (uint32_t)len; } else { m_nl.txFail++; m_dgTxFail++; }
    return r;
}

extern "C" int NpNetUsingRelay(void) __attribute__((weak));   // net/src/online (the online link's path: relay or a direct / punched one)

// netlog: one line per link every SMG_MP_NETLOG_SEC seconds (default 5, 0 = off): the transport, datagrams out / in per second, what the transport refused
// with an error (fail), how long the link was silent at most (gapmax), the game's STATE sends the full hand-over ring turned away (ring_full),
// and on the Vita's ad hoc radio what the radio itself did with the sends (ok / blocked / err; a blocked send (would block, time-out) is NOT
// retried: it is dropped, and the reliable layer resends what mattered). Counters only.
void MpCore::logRtt(uint64_t t) {
    if (!m_conn || m_localSlot < 0 || m_phase == MP_UI_IDLE) { m_lastRttLog = 0; return; }
    if (!m_lastRttLog) { m_lastRttLog = t; return; }
    if (t - m_lastRttLog < 10000) return;
    m_lastRttLog = t;
    char b[200];
    size_t n = 0;
    b[0] = 0;
    for (int s = 0; s < MP_MAX_PLAYERS && n + 48 < sizeof b; ++s) {
        Peer& p = m_peers[s];
        if (!p.used || s == m_localSlot) continue;
        if (p.rttN)
            n += (size_t)snprintf(b + n, sizeof b - n, " s%d %u/%u/%u (n=%u)", s, (unsigned)p.rttMin, (unsigned)(p.rttSum / p.rttN), (unsigned)p.rttMax, (unsigned)p.rttN);
        else
            n += (size_t)snprintf(b + n, sizeof b - n, " s%d none (srtt %d)", s, p.srtt);
        p.rttMin = p.rttMax = p.rttN = p.rttSum = 0;
    }
    mp_log("rtt: min/mean/max ms in the last 10 s:%s", n ? b : " no peer");
}

void MpCore::logNetLink(uint64_t t) {
    const int sec = MpNetTune::cur().logSec;
    if (sec <= 0 || !m_conn || m_localSlot < 0 || m_phase == MP_UI_IDLE) {
        m_nl.winAt = 0;
        return;
    }
    if (!m_nl.winAt) {
        m_nl.winAt = t;
        m_nl.uDropped0 = m_uDropped;
        uint32_t a, b, c;
        mp_sys_radio_send_take(&a, &b, &c);   // (the radio's window starts here too)
        return;
    }
    if (t - m_nl.winAt < (uint64_t)sec * 1000u) return;
    const double span = (double)(t - m_nl.winAt) / 1000.0;
    const char* kind = m_connKind == MP_CONN_NEARBY ? "adhoc"
                       : m_connKind == MP_CONN_ONLINE ? (NpNetUsingRelay && NpNetUsingRelay() ? "relay" : "udp")
                       : m_connKind == MP_CONN_LOOPBACK ? "loop" : "udp";
    char radio[96] = "";
    uint32_t rOk = 0, rBlocked = 0, rErr = 0;
    if (m_connKind == MP_CONN_NEARBY && mp_sys_radio_send_take(&rOk, &rBlocked, &rErr))
        snprintf(radio, sizeof radio, " radio ok=%u blocked=%u (dropped, not retried) err=%u", (unsigned)rOk, (unsigned)rBlocked, (unsigned)rErr);
    mp_log("net: link %s tx %.1f/s %.1f kB/s fail=%u rx %.1f/s %.1f kB/s gapmax=%u ms ring_full=%u%s", kind, m_nl.txOk / span, m_nl.txBytes / span / 1000.0,
           (unsigned)m_nl.txFail, m_nl.rx / span, m_nl.rxBytes / span / 1000.0,
           (unsigned)m_nl.rxGapMax, (unsigned)(m_uDropped - m_nl.uDropped0), radio);
    m_nl.winAt = t;
    m_nl.txOk = m_nl.txFail = m_nl.rx = m_nl.rxGapMax = m_nl.txBytes = m_nl.rxBytes = 0;
    m_nl.uDropped0 = m_uDropped;
}

// ---- beta diagnostics (port/mp_diag.h): the session's numbers ------------------------------------------------------------------------------
// diagTick: once a pump pass. The session goes live with the lobby; the totals and the most players present are handed over (replace, not add).
void MpCore::diagTick() {
    if (!mp_diag_session_begin) return;
    const bool live = (m_phase == MP_UI_LOBBY || m_phase == MP_UI_PLAYING) && m_conn && m_localSlot >= 0 && !m_dying;
    if (live && !m_dgOn) {
        m_dgOn = true;
        m_dgRx = m_dgTxFail = 0;
        mp_diag_session_begin(m_connKind == MP_CONN_ONLINE || m_connKind == MP_CONN_DIRECT ? 1 : 0);
    }
    if (m_dgOn && m_conn) {
        const bool relay = m_connKind == MP_CONN_ONLINE && NpNetUsingRelay && NpNetUsingRelay();
        mp_diag_session_count(m_dgRx, m_dgTxFail, (uint32_t)presentCount(), relay ? 1 : 0);
    }
}

// diagEnd: the connection is about to be deleted (the online room's leave request goes out with it).
void MpCore::diagEnd(MpUiError err, const char* why) {
    if (!m_dgOn) return;
    m_dgOn = false;
    if (mp_diag_session_end) mp_diag_session_end((int)err, why, m_connKind == MP_CONN_ONLINE ? 1 : 0);
}

void MpCore::logStatus(uint64_t t) {
    m_lastStatusLog = t;
    int ping = -1;
    if (m_role == ROLE_GUEST && m_hostSlot >= 0) ping = m_peers[m_hostSlot].srtt;
    mp_log("status %s slot=%d host=%d epoch=%d players=%d ping=%d bad=%u gaps=%u stray=%u out=%u/%u", m_role == ROLE_HOST ? "host" : "guest",
           m_localSlot, m_hostSlot, m_epoch, presentCount(), ping, (unsigned)m_statBad, (unsigned)m_statGaps, (unsigned)m_statStray, (unsigned)m_statDatagramsOut, (unsigned)m_statMsgsOut);
}

// ---- public (each takes the lock) ------------------------------------------------------------------------------------------
bool MpCore::active() { return __atomic_load_n(&m_active, __ATOMIC_RELAXED); }

// (perf) The queries below are the game thread's every-turn reads: they read the published view (MpCoreView), no lock.
MpMode MpCore::mode() {
    return readViewWith(kViewHead, [](const MpCoreView& v) { return v.active ? (MpMode)v.settings.mode : MP_MODE_NONE; });
}

bool MpCore::isHost() {
    return readViewWith(kViewHead, [](const MpCoreView& v) { return v.active && v.role == ROLE_HOST; });
}

int MpCore::localSlot() {
    return readViewWith(kViewHead, [](const MpCoreView& v) { return v.active ? (int)v.localSlot : -1; });
}

int MpCore::hostSlot() {
    return readViewWith(kViewHead, [](const MpCoreView& v) { return v.active ? (int)v.hostSlot : -1; });
}

uint16_t MpCore::hostEpoch() {
    return readViewWith(kViewHead, [](const MpCoreView& v) { return v.active ? v.epoch : (uint16_t)0; });
}

bool MpCore::slotPresent(int slot) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS) return false;
    return readViewWith(kViewHead, [slot](const MpCoreView& v) { return v.active && v.present[slot]; });
}

// core2: each slot's friend code, from this console's own room view at the service (MpConnector::memberCode), never from a peer. Shown
// only when the service lists that slot at the address the roster has for the player in it (online the slot is the service's, the
// address its virtual address): a roster entry the host got wrong, or a slot that changed hands and the view has not caught up with,
// shows nothing. Never on LAN / ad hoc / by address (those connectors have no codes; the kinds are refused here too). Not logged.

// ---- core2: self-announced friend codes --------------------------------------------------------------------------------------
void MpCore::setOwnFriendCode(const char* code) {
    char c[10] = {0};
    if (code && strlen(code) == 9) memcpy(c, code, 9);
    if (c[0] && !WellFormedPlayerCode(c)) return;
    MpLock l(m_ownCodeMu);
    if (!memcmp(m_ownCodeIn, c, 10)) return;
    memcpy(m_ownCodeIn, c, 10);
    m_ownCodeNew = true;
}
void MpCore::pickOwnCode() {   // (pump, under m_mutex)
    bool fresh = false;
    {
        MpLock l(m_ownCodeMu);
        if (m_ownCodeNew) {
            memcpy(m_ownCode, m_ownCodeIn, 10);
            m_ownCodeNew = false;
            fresh = true;
        }
    }
    if (fresh) announceOwnCode();
}
void MpCore::sendFriendCode(int to, int about, const char* code) {
    uint8_t b[10];
    b[0] = (uint8_t)about;
    memcpy(b + 1, code, 9);   // (all zero: "no code any more")
    queueReliable(to, MP_MSG_FRIEND, (uint8_t)m_localSlot, b, 10);
}
// At join (WELCOME / a guest accepted), when the account appears or goes: guests tell the host, the host tells everyone
void MpCore::announceOwnCode() {
    if (!m_active || m_localSlot < 0 || !m_mem || !announcesCodes()) return;
    if (m_role == ROLE_HOST) {
        for (int s = 0; s < MP_MAX_PLAYERS; s++)
            if (s != m_localSlot && m_peers[s].used && m_peers[s].resumed && !m_peers[s].closing && m_roster[s].present)
                sendFriendCode(s, m_localSlot, m_ownCode);
    } else if (m_role == ROLE_GUEST && m_hostSlot >= 0 && m_peers[m_hostSlot].used) {
        sendFriendCode(m_hostSlot, m_localSlot, m_ownCode);
    }
}
// host: a new guest learns the codes the room already knows (the host's own and those announced to it)
void MpCore::sendKnownCodesTo(int slot) {
    if (!announcesCodes()) return;
    if (m_ownCode[0]) sendFriendCode(slot, m_localSlot, m_ownCode);
    for (int s = 0; s < MP_MAX_PLAYERS; s++)
        if (s != slot && s != m_localSlot && m_roster[s].present && m_annCode[s][0]) sendFriendCode(slot, s, m_annCode[s]);
}

void MpCore::refreshFriendCodes() {
    // (MP_CONN_LOOPBACK: the tests' stand-in for the service; Same Wi-Fi, Nearby and by address are never asked)
    const bool online = m_conn && m_active && m_localSlot >= 0 && (m_connKind == MP_CONN_ONLINE || m_connKind == MP_CONN_LOOPBACK);
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        char code[10];
        uint32_t vip = 0;
        const bool ok = online && s != m_localSlot && m_roster[s].present && m_roster[s].addr.host != 0 &&
                        m_conn->memberCode(s, &vip, code) && vip == m_roster[s].addr.host && WellFormedPlayerCode(code);
        if (ok) memcpy(m_friendCode[s], code, 10);
        else memset(m_friendCode[s], 0, 10);
        // svcfix3: the label of a shared name, bound to the slot and the address like the code; printable UTF-8 only (it is shown as text)
        char label[MP_NAME_BYTES + 1];
        bool lok = online && s != m_localSlot && m_roster[s].present && m_roster[s].addr.host != 0 && m_conn->memberLabel(s, &vip, label) &&
                   vip == m_roster[s].addr.host;
        if (lok) {
            label[MP_NAME_BYTES] = 0;
            for (const char* q = label; *q; q++)
                if ((uint8_t)*q < 0x20 || (uint8_t)*q == 0x7F) lok = false;
            lok = lok && label[0];
        }
        if (lok) { memset(m_label[s], 0, sizeof m_label[s]); memcpy(m_label[s], label, strlen(label)); }
        else memset(m_label[s], 0, sizeof m_label[s]);
        // an announced code belongs to the membership it was announced in
        if (m_annCode[s][0] && (!m_active || !m_roster[s].present || m_roster[s].gen != m_annGen[s] || s == m_localSlot))
            memset(m_annCode[s], 0, 10);
    }
}

void MpCore::setGatherId(uint32_t gid) { CoreLock l(*this); m_gid = gid; }
uint32_t MpCore::slotGatherId(int slot) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS) return 0;
    return readViewWith(sizeof(MpCoreView), [slot](const MpCoreView& v) { return v.active && v.present[slot] ? v.gid[slot] : 0u; });
}
void MpCore::setGathering(bool on, uint32_t closingInto) {
    CoreLock l(*this);
    m_gathering = on;
    m_closingInto = on ? closingInto : 0;
    m_lastBeacon = 0;   // (tell the others at once)
}
bool MpCore::gathering() { CoreLock l(*this); return m_gathering; }
int MpCore::heardRooms(MpBeacon* out, MpAddr* addrs, uint64_t* lastMs, int max) {
    CoreLock l(*this);
    int n = 0;
    for (int i = 0; i < m_scan.count() && n < max; i++)
        if (m_scan.getBeacon(i, out ? &out[n] : 0, addrs ? &addrs[n] : 0, lastMs ? &lastMs[n] : 0)) n++;
    return n;
}
bool MpCore::redirectGuests(uint32_t sessionId) {
    CoreLock l(*this);
    if (m_role != ROLE_HOST || m_started || !sessionId) return false;
    uint8_t buf[4] = {(uint8_t)sessionId, (uint8_t)(sessionId >> 8), (uint8_t)(sessionId >> 16), (uint8_t)(sessionId >> 24)};
    int n = 0;
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        Peer& p = m_peers[s];
        if (s == m_localSlot || !p.used || p.closing || !m_roster[s].present) continue;
        queueReliable(s, MP_MSG_REDIRECT, (uint8_t)m_localSlot, buf, 4);
        n++;
    }
    return n > 0;
}
uint32_t MpCore::takeRedirect() { CoreLock l(*this); const uint32_t r = m_redirectTo; m_redirectTo = 0; return r; }
bool MpCore::redirectGuestsCode(uint32_t ip, uint16_t port, const uint8_t secret[5]) {
    CoreLock l(*this);
    if (m_role != ROLE_HOST || m_started || !ip || !port) return false;
    uint8_t buf[11];
    MpWriter w(buf, sizeof buf);
    w.u32(ip);
    w.u16(port);
    w.bytes(secret, 5);
    int n = 0;
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        Peer& p = m_peers[s];
        if (s == m_localSlot || !p.used || p.closing || !m_roster[s].present) continue;
        queueReliable(s, MP_MSG_REDIRECT_CODE, (uint8_t)m_localSlot, buf, (uint16_t)w.n);
        n++;
    }
    return n > 0;
}
bool MpCore::takeRedirectCode(uint32_t* ip, uint16_t* port, uint8_t secret[5]) {
    CoreLock l(*this);
    if (!m_redirectCode.have) return false;
    *ip = m_redirectCode.ip;
    *port = m_redirectCode.port;
    memcpy(secret, m_redirectCode.secret, 5);
    memset(&m_redirectCode, 0, sizeof m_redirectCode);
    return true;
}
uint32_t MpCore::directLocalIp() {
    if (m_hub) return 0x0A000000u | (m_hubEndpoint & 0xFFFFu);
    return mp_direct_local_ip ? mp_direct_local_ip() : 0;
}
bool MpCore::directAvailable() const { return m_hub || mp_connector_direct_create; }
bool MpCore::uiHostDirect(const MpHostParams* p, const uint8_t secret[5]) {
    if (!p) return false;
    MpHostParams q = *p;
    q.conn = MP_CONN_DIRECT;
    setRoomSecret(secret, 5);
    return uiHost(&q);
}
bool MpCore::uiJoinDirect(uint32_t ip, uint16_t port, const uint8_t secret[5]) {
    {
        CoreLock l(*this);
        if (m_phase != MP_UI_IDLE || !ip || !port) return false;
        memset(m_psk, 0, sizeof m_psk);
        mpseal::Mac(m_psk, 32, 0, 0, secret, 5);
        m_pskArmed = true;
        if (!joinDirectLocked(ip, port)) return false;
    }
    ensureThread();
    return true;
}
uint32_t MpCore::sessionIdNow() { CoreLock l(*this); return m_active ? m_sessionId : 0; }
int MpCore::guestCount() { CoreLock l(*this); return m_role == ROLE_HOST && m_active ? presentCount() - 1 : 0; }

bool MpCore::slotFriendCode(int slot, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (slot < 0 || slot >= MP_MAX_PLAYERS || cap < 10) return false;
    struct Code { char s[10]; };
    const Code c = readViewWith(sizeof(MpCoreView), [slot](const MpCoreView& v) {
        Code r;
        if (v.active && v.present[slot] && slot != v.localSlot) memcpy(r.s, v.friendCode[slot], 10);
        else r.s[0] = 0;
        r.s[9] = 0;
        return r;
    });
    if (!c.s[0]) return false;
    memcpy(out, c.s, 10);
    return true;
}

static uint32_t mp_player_sid(const uint8_t* commit) {
    static const uint8_t kKey[12] = {'S', 'M', 'G', '3', ' ', 'm', 'u', 't', 'e', ' ', 'i', 'd'};
    if (!commit || !memcmp(commit, kZeroKey, 16)) return 0;   // (no commitment known: no id; the game falls back to the name)
    uint8_t h[4];
    mpseal::Mac(h, 4, kKey, sizeof kKey, commit, 16);
    uint32_t id;
    memcpy(&id, h, 4);
    return id ? id : 1;
}

bool MpCore::slotPlayer(int slot, MpPlayerInfo* out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (slot < 0 || slot >= MP_MAX_PLAYERS) return false;
    return readViewWith(sizeof(MpCoreView), [slot, out](const MpCoreView& v) {
        if (!v.active || !v.present[slot] || slot == v.localSlot) return false;
        memcpy(out->name, v.name[slot], MP_NAME_BYTES + 1);
        out->name[MP_NAME_BYTES] = 0;
        out->look = v.look[slot];
        if (v.friendCode[slot][0]) {
            memcpy(out->code, v.friendCode[slot], 10);
            out->codeVerified = true;
        } else if (v.annCode[slot][0]) {
            memcpy(out->code, v.annCode[slot], 10);
        }
        out->code[9] = 0;
        // smallfix2: a stable id for this player in this session ("Hide messages" keys on it, not on the name). It comes from the rejoin
        // token's commitment (the same after a drop and rejoin, a name change or another slot; two players with one name differ) as a
        // keyed, truncated BLAKE2b: 32 bits of a hash of a hash, so it tells nothing of the token (and the commitment itself is not shown).
        out->sid = mp_player_sid(v.commit[slot]);
        return true;
    });
}

void MpCore::slotName(int slot, char* out) {
    out[0] = 0;
    if (slot < 0 || slot >= MP_MAX_PLAYERS) return;
    struct Name { char s[MP_NAME_BYTES + 1]; };
    const Name n = readViewWith(kViewNames, [slot](const MpCoreView& v) {
        Name r;
        if (v.active && v.present[slot]) memcpy(r.s, v.name[slot], MP_NAME_BYTES + 1);
        else r.s[0] = 0;
        return r;
    });
    memcpy(out, n.s, MP_NAME_BYTES + 1);
}

MpLook MpCore::slotLook(int slot) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS) return MP_LOOK_MARIO;
    return readViewWith(kViewHead, [slot](const MpCoreView& v) { return (!v.active || !v.present[slot]) ? MP_LOOK_MARIO : (MpLook)v.look[slot]; });
}

int MpCore::slotPing(int slot) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS) return -1;
    return readViewWith(kViewAll, [slot](const MpCoreView& v) -> int {
        if (!v.active || !v.present[slot]) return -1;
        if (slot == v.localSlot) return 0;
        if (v.peerUsed[slot]) return v.srtt[slot];
        return v.rosterPing[slot] == 0xFFFF ? -1 : (int)v.rosterPing[slot];
    });
}

bool MpCore::slotQuiet(int slot) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS) return false;
    struct Q { int8_t state; uint64_t lastRecv; };   // state: 0 not quiet, 1 quiet, 2 the time decides
    const Q q = readViewWith(kViewAll, [slot](const MpCoreView& v) {
        Q r = {0, 0};
        if (!v.active || !v.present[slot] || slot == v.localSlot) return r;
        if (v.peerUsed[slot]) { r.state = 2; r.lastRecv = v.lastRecv[slot]; return r; }
        r.state = (v.rosterState[slot] & 0x40) != 0 ? 1 : 0;
        return r;
    });
    if (q.state == 2) return now() - q.lastRecv >= MP_QUIET_MS;
    return q.state == 1;
}

void MpCore::settings(MpSettings* out) {
    *out = readViewWith(kViewHead, [](const MpCoreView& v) {
        MpSettings r = v.settings;
        if (!v.active) r.mode = MP_MODE_NONE;
        return r;
    });
}

uint16_t MpCore::sceneSerial() {
    return readViewWith(kViewHead, [](const MpCoreView& v) { return v.active ? v.serial : (uint16_t)0; });
}

bool MpCore::send(uint8_t type, const void* payload, uint16_t len, bool reliable) {
    if (!reliable && __atomic_load_n(&m_threadRunsPub, __ATOMIC_ACQUIRE)) {
        // (perf) the helper thread pumps: hand the datagram to it (no lock, no socket call on the game thread). Latest
        // state wins anyway; the checks that need the session are made again when it goes out (sendUnreliableLocked).
        struct V { bool active; int8_t localSlot; uint8_t role; };
        const V v = readViewWith(kViewHead, [](const MpCoreView& w) { return V{w.active, w.localSlot, w.role}; });
        if (!v.active || v.localSlot < 0 || !IsGame(type) || len > GameMaxLen(type) || len > sizeof m_uOut[0].data || (len && !payload))
            return false;
        if (mp_msg_host_only(type) && v.role != ROLE_HOST) return false;
        const uint32_t tail = m_uTail, head = __atomic_load_n(&m_uHead, __ATOMIC_ACQUIRE);
        if (tail - head >= MP_UOUT_SLOTS) {
            m_uDropped++;
            __atomic_fetch_add(&g_sendRingFull, 1u, __ATOMIC_RELAXED);   // MP3 nocap: the SMG_MP_PERFLOG line's "refused"
            return false;
        }
        MpUnreliableOut& o = m_uOut[tail % MP_UOUT_SLOTS];
        o.type = type;
        o.len = len;
        o.queuedUs = __atomic_load_n(&g_helperMeasure, __ATOMIC_RELAXED) ? (uint32_t)mp_sys_now_us() : 0u;
        if (len) memcpy(o.data, payload, len);
        __atomic_store_n(&m_uTail, tail + 1, __ATOMIC_RELEASE);
        return true;
    }
    CoreLock l(*this);
    if (!reliable) return sendUnreliableLocked(type, (const uint8_t*)payload, len);
    if (!m_active || !m_mem || m_localSlot < 0 || !IsGame(type) || len > GameMaxLen(type) || (len && !payload)) return false;
    const bool host = m_role == ROLE_HOST;
    if (mp_msg_host_only(type) && !host) return false;
    const uint8_t* data = (const uint8_t*)payload;
    if (type == MP_MSG_CHAT) {
        // MP3 chat: only well-formed CHATs leave, and typed text only where the room allows it (the host would drop it anyway)
        MpChatMsg cm;
        MpReader cr(data, len);
        if (!mp_read_chat(cr, &cm) || (cm.kind == MP_CHAT_TEXT && !mp_chat_typed_allowed(&m_settings))) return false;
    }
    if (host) snoopWhere(type, data, len);
    if (host && type == MP_MSG_RESTART && len == 2 && m_settings.mode == MP_MODE_TOGETHER) {
        const uint16_t rser = (uint16_t)(data[0] | (data[1] << 8));
        if (rser != 0 && rser == m_serial) {
            const uint64_t tnow = now();
            m_restartSerial = rser;
            m_restartGateMask = 0;
            m_restartMissedMask = 0;
            memset(m_restartGateUntil, 0, sizeof m_restartGateUntil);
            for (int q = 0; q < MP_MAX_PLAYERS; q++) {
                if (q == m_localSlot) continue;
                const bool livePeer = m_roster[q].present && m_peers[q].used && !m_peers[q].closing && m_peers[q].resumed;
                const bool heldPeer = m_roster[q].present || (m_res[q].used && tnow <= m_res[q].until);
                if (livePeer) {
                    m_restartGateMask |= (uint8_t)(1u << q);
                    m_restartMissedMask |= (uint8_t)(1u << q);
                    m_restartGateUntil[q] = tnow + 20000;
                } else if (heldPeer) {
                    m_restartMissedMask |= (uint8_t)(1u << q);
                }
            }
        }
    }
    uint16_t s = 0;
    if (mp_msg_host_relays(type)) {
        if (++m_myOrigin == 0) m_myOrigin = 1;
        s = m_myOrigin;
        historyAdd((uint8_t)m_localSlot, s, m_myGen, type, data, len);
    }
    if (!host) {
        if (m_hostSlot < 0 || !m_peers[m_hostSlot].used) return false;
        return queueGameReliable(m_hostSlot, type, (uint8_t)m_localSlot, s, m_myGen, data, len);
    }
    for (int q = 0; q < MP_MAX_PLAYERS; q++) {
        Peer& p = m_peers[q];
        if (q == m_localSlot || !p.used || p.closing || !m_roster[q].present) continue;
        // a guest that has not answered a new host yet gets tracked events in catch-up and missed RESTART in onResume (after WHERE)
        if (!p.resumed && (s != 0 || type == MP_MSG_RESTART)) continue;
        queueGameReliable(q, type, (uint8_t)m_localSlot, s, m_myGen, data, len);
    }
    if (m_dying) finishEnd();
    return true;
}

// An unreliable game message (STATE): to the host (guest) or every resumed guest (host). Under m_mutex.
bool MpCore::sendUnreliableLocked(uint8_t type, const uint8_t* data, uint16_t len) {
    if (!m_active || !m_mem || m_localSlot < 0 || !IsGame(type) || len > GameMaxLen(type) || (len && !data)) return false;
    const bool host = m_role == ROLE_HOST;
    if (mp_msg_host_only(type) && !host) return false;
    const uint16_t s = ++m_uSeq;
    if (!host) {
        if (m_hostSlot < 0 || !m_peers[m_hostSlot].used) return false;
        return sendToPeer(m_hostSlot, type, (uint8_t)m_localSlot, 0, s, data, len);
    }
    const bool isState = type == MP_MSG_STATE && len >= 6;
    const uint16_t stateSerial = isState ? (uint16_t)(data[4] | (data[5] << 8)) : (uint16_t)0;
    for (int q = 0; q < MP_MAX_PLAYERS; q++) {
        Peer& p = m_peers[q];
        if (q == m_localSlot || !p.used || p.closing || !m_roster[q].present || !p.resumed) continue;
        if (isState && !stateWanted(q, stateSerial)) {   // netopt: that console is in another scene and would drop it
            m_statStateSkipped++;
            continue;
        }
        sendToPeer(q, type, (uint8_t)m_localSlot, 0, s, data, len);
    }
    if (m_dying) finishEnd();
    return true;
}

// The game thread's queued unreliable messages go out (pump, under m_mutex).
void MpCore::drainUnreliableOut() {
    const uint32_t tail = __atomic_load_n(&m_uTail, __ATOMIC_ACQUIRE);
    uint32_t head = m_uHead;
    while (head != tail && !m_dying) {
        const MpUnreliableOut& o = m_uOut[head % MP_UOUT_SLOTS];
        if (o.queuedUs != 0) {
            // MP3 nocap: how long the send waited for the helper thread (its 5 ms sleep, plus any time other threads on its core took)
            const uint32_t lag = (uint32_t)mp_sys_now_us() - o.queuedUs;
            __atomic_fetch_add(&g_sendCount, 1u, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_sendLagSumUs, lag, __ATOMIC_RELAXED);
            if (lag > __atomic_load_n(&g_sendLagMaxUs, __ATOMIC_RELAXED)) __atomic_store_n(&g_sendLagMaxUs, lag, __ATOMIC_RELAXED);
        }
        sendUnreliableLocked(o.type, o.data, o.len);
        head++;
    }
    __atomic_store_n(&m_uHead, tail, __ATOMIC_RELEASE);   // (what a dying session did not send is dropped)
}

bool MpCore::sendTo(int slot, uint8_t type, const void* payload, uint16_t len, bool reliable) {
    CoreLock l(*this);
    if (!m_active || !m_mem || slot < 0 || slot >= MP_MAX_PLAYERS || slot == m_localSlot) return false;
    if (!IsGame(type) || len > GameMaxLen(type) || (len && !payload)) return false;
    if (m_role != ROLE_HOST) {
        // a guest can only talk to the host (e.g. a REGCRC reply); host-only types stay the host's
        if (slot != m_hostSlot || mp_msg_host_only(type) || !m_peers[slot].used) return false;
        if (reliable) return queueGameReliable(slot, type, (uint8_t)m_localSlot, 0, 0, payload, len);
        return sendToPeer(slot, type, (uint8_t)m_localSlot, 0, ++m_uSeq, payload, len);
    }
    Peer& p = m_peers[slot];
    if (!p.used || p.closing || !m_roster[slot].present) return false;
    const uint8_t* data = (const uint8_t*)payload;
    snoopWhere(type, data, len);
    if (reliable) {
        // (a message to one player is not a shared event: not tracked, not in the history)
        return queueGameReliable(slot, type, (uint8_t)m_localSlot, 0, 0, data, len);
    }
    return sendToPeer(slot, type, (uint8_t)m_localSlot, 0, ++m_uSeq, data, len);
}

bool MpCore::poll(MpMsg* out) {
    return pollMany(out, 1) == 1;
}

// wave8: dedicated m_qMutex (never held across socket I/O or crypto) eliminates tryLock() 16.7 ms turn drops.
uint32_t MpCore::pollMany(MpMsg* out, uint32_t max) {
    if (!out || max == 0 || __atomic_load_n(&m_qPub, __ATOMIC_ACQUIRE) == 0) return 0;
    MpLock ql(m_qMutex);
    if (!m_mem) return 0;
    MpCoreMem& m = *m_mem;
    uint32_t n = 0;
    while (n < max && m.qCount > 0) {
        const uint16_t idx = m.qRing[m.qHead];
        const QEntry& src = m.q[idx];
        MpMsg* o = &out[n++];
        o->type = src.type;
        o->fromSlot = src.fromSlot;
        o->len = src.len;
        memcpy(o->data, src.big >= 0 ? m.bigData[src.big] : src.data, src.len);
        if (src.big >= 0) m.bigUsed[src.big] = false;
        m.qHead = (m.qHead + 1) % MP_QUEUE_SIZE;
        m.qCount--;
        m.qFree[m.qFreeCount++] = idx;
    }
    __atomic_store_n(&m_qPub, (uint32_t)m.qCount, __ATOMIC_RELEASE);
    return n;
}

// wave8: drain only unreliable MP_MSG_STATE entries before GameScene::updateNerve(), preserving FIFO order of all remaining messages.
uint32_t MpCore::pollStates(MpMsg* out, uint32_t max) {
    if (!out || max == 0 || __atomic_load_n(&m_qPub, __ATOMIC_ACQUIRE) == 0) return 0;
    MpLock ql(m_qMutex);
    if (!m_mem || m_mem->qCount == 0) return 0;
    MpCoreMem& m = *m_mem;
    uint32_t n = 0;
    for (int i = 0; i < m.qCount && n < max;) {
        const uint16_t idx = m.qRing[(m.qHead + i) % MP_QUEUE_SIZE];
        const QEntry& src = m.q[idx];
        if (src.type == MP_MSG_STATE) {
            MpMsg* o = &out[n++];
            o->type = src.type;
            o->fromSlot = src.fromSlot;
            o->len = src.len;
            memcpy(o->data, src.big >= 0 ? m.bigData[src.big] : src.data, src.len);
            queueRemoveAt(i);
        } else {
            i++;
        }
    }
    return n;
}

void MpCore::tick(uint32_t turn) {
    __atomic_store_n(&m_turn, turn, __ATOMIC_RELAXED);
    // (perf) the helper thread pumps every 5 ms (receive, timers, resends, the queued unreliable sends): nothing to do here, and
    // nothing that could wait for it. Without it (tests, a thread that could not start, idle) the game thread pumps as before.
    if (__atomic_load_n(&m_threadRunsPub, __ATOMIC_ACQUIRE)) return;
    // (perf) single-player after the menus were used once (the core exists): nothing to pump, no lock. Without the helper
    // thread only this thread changes these fields.
    const bool idle = m_phase == MP_UI_IDLE && !m_conn && !m_scanConn && !m_reconnect && !m_dying && !m_mem;
    bool threadRuns = false;
    if (!idle || (__atomic_load_n(&g_netTailUntil, __ATOMIC_RELAXED) != 0 && now() < __atomic_load_n(&g_netTailUntil, __ATOMIC_RELAXED))) {
        CoreLock l(*this);
        pumpLocked(now());
        threadRuns = m_threadAlive && !m_threadExiting;
    }
    if (!threadRuns && mp_service_pump) {
        MpLock l(m_svcMutex);
        mp_service_pump();
    }
}

bool MpCore::saveSuppressed() {
    // only the original host saves during a session; after a migration nobody does (DESIGN.md section 8)
    return readViewWith(kViewHead, [](const MpCoreView& v) { return v.active && !(v.originalHost && v.role == ROLE_HOST && v.epoch == 0); });
}

bool MpCore::saveAutoconfirm() {
    return readViewWith(kViewHead, [](const MpCoreView& v) { return v.active && v.originalHost && v.role == ROLE_HOST && v.epoch == 0; });
}

void MpCore::snapshotOffer(const void* image, uint32_t len) {
    CoreLock l(*this);
    if (!m_active || !m_mem || m_role != ROLE_HOST || !image || len == 0 || len > MP_SNAPSHOT_MAX) return;
    MpSnapshotImage& im = m_mem->image;
    memcpy(im.bytes, image, len);
    im.len = len;
    im.crc = mp_crc32(0, image, len);
    im.version++;
    mp_log("snapshot offered bytes=%u crc=%08x", (unsigned)len, (unsigned)im.crc);
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        Peer& p = m_peers[s];
        if (s == m_localSlot || !p.used || p.closing || !p.resumed || !m_roster[s].present) continue;
        p.snapTries = 0;
        sendSnapshotTo(s);
    }
    if (m_dying) finishEnd();
}

uint32_t MpCore::snapshotReady() {
    // corefix S3: the game thread asks every turn of a session (applySnapshotIfReady) while the helper thread may hold m_mutex through
    // a whole pump (log lines to the SD card, X25519, history scans): the answer is a published value, never a wait for the lock
    return __atomic_load_n(&m_snapPub, __ATOMIC_ACQUIRE);
}

uint32_t MpCore::snapshotGet(void* out, uint32_t cap) {
    CoreLock l(*this);
    if (!m_mem || !m_snapReady || !out || cap < m_mem->image.len) return 0;
    memcpy(out, m_mem->image.bytes, m_mem->image.len);
    return m_mem->image.len;
}

void MpCore::snapshotConsumed() {
    CoreLock l(*this);
    m_snapReady = false;
    __atomic_store_n(&m_snapPub, 0u, __ATOMIC_RELEASE);
}

bool MpCore::uiHost(const MpHostParams* p) {
    {
        CoreLock l(*this);
        if (!p || m_phase != MP_UI_IDLE) return false;
        if (p->conn == MP_CONN_NONE || p->conn > MP_CONN_DIRECT) return false;
        if (!m_pskArmed) memset(m_psk, 0, sizeof m_psk);   // core2: the code secret only for the session it was set for
        m_pskArmed = false;
        if (!startHostingLocked(p->conn, p->settings, p->visibility)) return false;
    }
    ensureThread();
    return true;
}

// netx: internet by address (mp_conn_direct.cpp). The guest's link sends to the typed address; the HELLO goes out as for online.
bool MpCore::uiJoinAddress(const char* text) {
    {
        CoreLock l(*this);
        if (m_phase != MP_UI_IDLE || !text) return false;
        uint32_t ip = 0;
        uint16_t port = 0;
        if (!mp_direct_parse_address || !mp_direct_parse_address(text, &ip, &port)) {
            m_error = MP_ERR_ROOM_NOT_FOUND;
            mp_log("join refused: not an address");
            return false;
        }
        if (!m_pskArmed) memset(m_psk, 0, sizeof m_psk);   // core2: a typed address has no code secret (unless one was just set)
        if (!joinDirectLocked(ip, port)) return false;
    }
    ensureThread();
    return true;
}

bool MpCore::joinDirectLocked(uint32_t ip, uint16_t port) {
    if (!randomOk()) return false;
    {
        m_pskArmed = false;
        MpConnectorParams p;
        memset(&p, 0, sizeof p);
        p.host = false;
        snprintf(p.name, sizeof p.name, "%s", m_name);
        p.directIp = ip;
        p.directPort = port;
        useWireId(0);   // corefix 6: the session id is not known before the HELLO: this run's random id
        p.linkId = m_wireId;
        MpConnector* c = makeConnector(MP_CONN_DIRECT, p);
        if (!c) {
            m_error = MP_ERR_INTERNAL;
            mp_log("join refused: connection direct not available");
            return false;
        }
        if (!allocMem()) {
            delete c;
            m_error = MP_ERR_INTERNAL;
            return false;
        }
        delete m_scanConn;
        m_scanConn = 0;
        clearSession();
        delete m_conn;
        m_conn = c;
        m_connKind = MP_CONN_DIRECT;
        m_role = ROLE_GUEST;
        m_error = MP_ERR_NONE;
        m_directIp = ip;
        m_directPort = port;
        setPhase(MP_UI_JOINING);
        mp_log("join start conn=direct addr=%u.%u.%u.%u:%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, (unsigned)port);
    }
    return true;
}

bool MpCore::uiHostAddress(MpAddressInfo* out) {
    CoreLock l(*this);
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!m_conn || m_connKind != MP_CONN_DIRECT || m_role != ROLE_HOST) return false;
    return m_conn->addressInfo(out);
}

bool MpCore::uiJoinCode(MpConn conn, const char* code) {
    {
        CoreLock l(*this);
        if (m_phase != MP_UI_IDLE || conn != MP_CONN_ONLINE || !code) return false;
        const size_t n = strlen(code);
        if (n < 4 || n > 8) return false;
        char clean[16];
        for (size_t i = 0; i <= n; i++) {
            char c = code[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (c && !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
            clean[i] = c;
        }
        delete m_scanConn;
        m_scanConn = 0;
        clearSession();
        m_error = MP_ERR_NONE;
        useWireId(0);   // corefix 6
        if (!startJoinCodeLocked(clean)) return false;
    }
    ensureThread();
    return true;
}

bool MpCore::uiJoinFriend(const char* friendCode) {
    {
        CoreLock l(*this);
        if (m_phase != MP_UI_IDLE || !friendCode || !*friendCode) return false;
        if (!mp_svc_room_lookup_begin || !mp_svc_room_lookup_begin(1, 0, friendCode)) {
            m_error = MP_ERR_SERVICE_DOWN;
            return false;
        }
        clearSession();
        m_error = MP_ERR_NONE;
        m_lookupActive = true;
        useWireId(0);   // corefix 6
        setPhase(MP_UI_SEARCHING);
        mp_log("join friend start");
    }
    ensureThread();
    return true;
}

bool MpCore::uiQuickMatch(MpMode mode) {
    {
        CoreLock l(*this);
        if (m_phase != MP_UI_IDLE || (mode != MP_MODE_TOGETHER && mode != MP_MODE_RACE)) return false;
        if (!mp_svc_room_lookup_begin || !mp_svc_room_lookup_begin(0, (uint8_t)mode, "")) {
            m_error = MP_ERR_SERVICE_DOWN;
            return false;
        }
        clearSession();
        m_error = MP_ERR_NONE;
        m_wantMode = (uint8_t)mode;
        m_lookupActive = true;
        useWireId(0);   // corefix 6
        setPhase(MP_UI_SEARCHING);
        mp_log("quick match start mode=%d", mode);
    }
    ensureThread();
    return true;
}

bool MpCore::joinLocalLocked(MpConn conn, int index) {
    if (!randomOk()) return false;
    if (m_phase != MP_UI_IDLE || !m_scanConn || m_scanKind != conn) return false;
    MpLocalRoom room;
    MpAddr addr;
    uint32_t sid = 0;
    if (!m_scan.get(index, &room, &addr, &sid)) return false;
    if (!room.compatible) {
        m_error = MP_ERR_DIFFERENT_VERSION;
        mp_log("join refused: room %d is a different version", index);
        return false;
    }
    if (!allocMem()) return false;
    clearSession();
    m_error = MP_ERR_NONE;
    delete m_conn;
    m_conn = m_scanConn;
    m_scanConn = 0;
    m_connKind = conn;
    m_wantSession = sid;
    useWireId(sid);   // corefix 6: the beacon told the session id: a keyed hash of (this console's secret, session id)
    m_scan.clear();
    mp_log("join start conn=%s room=%s", ConnName(conn), room.host);
    startConnecting(addr, 0, now());
    return true;
}

bool MpCore::uiJoinLocal(MpConn conn, int index) {
    {
        CoreLock l(*this);
        if (!joinLocalLocked(conn, index)) return false;
    }
    ensureThread();
    return true;
}

bool MpCore::uiJoinLocalRoom(MpConn conn, uint32_t roomId) {
    {
        CoreLock l(*this);
        int index = -1;
        for (int i = 0; roomId != 0 && i < m_scan.count() && index < 0; i++) {
            uint32_t sid = 0;
            if (m_scan.get(i, 0, 0, &sid) && sid == roomId) index = i;
        }
        if (index < 0) {
            m_error = MP_ERR_ROOM_NOT_FOUND;
            mp_log("join refused: room %08x is gone", (unsigned)roomId);
            return false;
        }
        if (!joinLocalLocked(conn, index)) return false;
    }
    ensureThread();
    return true;
}

bool MpCore::uiStart() {
    CoreLock l(*this);
    if (m_role != ROLE_HOST || m_phase != MP_UI_LOBBY) return false;
    m_started = true;
    setPhase(MP_UI_PLAYING);
    m_rosterDirty = true;
    pushEvent(MP_EV_STARTED, m_localSlot, 0, 0);
    mp_log("session started players=%d", presentCount());
    mp_heap_note("mp session start");
    return true;
}

void MpCore::uiLeave() {
    {
        CoreLock l(*this);
        m_reconnect = false;
        m_rejoining = false;   // MP3 lodgate: leaving drops the hold at once
        switch (m_phase) {
        case MP_UI_IDLE:
            delete m_scanConn;
            m_scanConn = 0;
            m_scan.clear();
            m_error = MP_ERR_NONE;
            return;
        case MP_UI_LEAVING:
            return;
        case MP_UI_LOBBY:
        case MP_UI_PLAYING:
        case MP_UI_CONNECTING: {
            int queued = 0;
            if (m_role == ROLE_HOST) {
                for (int s = 0; s < MP_MAX_PLAYERS; s++) {
                    if (s == m_localSlot || !m_peers[s].used || m_peers[s].closing) continue;
                    uint8_t r = MP_BYE_HOST_LEAVING;   // the session goes on without us (host migration)
                    if (queueReliable(s, MP_MSG_BYE, (uint8_t)m_localSlot, &r, 1)) queued++;
                }
            } else if (m_hostSlot >= 0 && m_peers[m_hostSlot].used) {
                uint8_t r = MP_BYE_LEAVE;
                if (queueReliable(m_hostSlot, MP_MSG_BYE, (uint8_t)(m_localSlot < 0 ? 0xFF : m_localSlot), &r, 1)) queued++;
            }
            setActive(false);
            if (queued && !m_dying) {
                setPhase(MP_UI_LEAVING);
                mp_log("leaving");
                flushPeers(now());
            } else {
                requestEnd(MP_ERR_NONE, "left");
            }
            break;
        }
        default:
            requestEnd(MP_ERR_NONE, "cancelled");
            break;
        }
        if (m_dying) finishEnd();
        m_error = MP_ERR_NONE;
    }
}

// corefix D2: a console removed as a nuisance (the host's kick, the bad-message kick) cannot come back in this session
// corefix2: a guest keeps the host's ban list (it is the next host's); the host is the only one that adds to it
void MpCore::applyBans(const Kicked* bans, int n) {
    if (m_role == ROLE_HOST) return;
    m_kickedCount = n;
    m_kickedNext = n % 8;
    for (int i = 0; i < n; i++) m_kicked[i] = bans[i];
}

void MpCore::banSlot(int slot) {
    const Peer& p = m_peers[slot];
    // corefix 9: a ring: with eight entries full the oldest is dropped (logged), never "the list silently stops at 8"
    const int at = m_kickedNext % 8;
    if (m_kickedCount == 8) mp_log("kick list full: %s (the oldest entry) can join again", m_kicked[at].name);
    else m_kickedCount++;
    m_kicked[at].addr = p.addr;
    m_kicked[at].consoleId = p.consoleId;   // (kept for the log / old entries only: a clear-text id never bans, see handleHelloPacket)
    memcpy(m_kicked[at].commit, m_roster[slot].commit, 16);   // core2
    memcpy(m_kicked[at].pk, m_roster[slot].pk, 32);
    memcpy(m_kicked[at].name, m_roster[slot].name, MP_NAME_BYTES + 1);
    m_kickedNext = (at + 1) % 8;
    m_rosterDirty = true;   // corefix2: the guests hold the list too
}

void MpCore::uiKick(int slot) {
    CoreLock l(*this);
    if (!m_active || m_role != ROLE_HOST || slot < 0 || slot >= MP_MAX_PLAYERS || slot == m_localSlot || !m_roster[slot].present) return;
    Peer& p = m_peers[slot];
    banSlot(slot);
    uint8_t r = MP_REJECT_KICKED;
    if (p.used) queueReliable(slot, MP_MSG_REJECT, (uint8_t)m_localSlot, &r, 1);
    mp_log("kicked slot=%d reason=host", slot);
    mp_log("kick slot=%d target=%d", m_localSlot, slot);   // hooks: the scenarios' line
    removeSlot(slot, MP_BYE_ERROR, "kicked");
    if (m_dying) finishEnd();
}

void MpCore::uiStatus(MpUiStatus* out) {
    if (!out) return;
    // (perf) the menus ask every frame: the published view, no lock. MP3 profile: only the fields asked for (not the whole view),
    // and the room code copied, not formatted (an snprintf every frame, in single-player too once the menus made the core)
    memset(out, 0, sizeof *out);
    readViewWith(kViewAll, [out](const MpCoreView& v) {
        out->phase = v.reconnecting ? MP_UI_RECONNECTING : v.phase;   // fix2: the menus tell a silent rejoin from the end
        out->error = v.error;
        out->players = v.players;
        memcpy(out->roomCode, v.roomCode, sizeof out->roomCode);
        out->progress = v.progress;
        return 0;
    });
    static_assert(sizeof out->roomCode == sizeof m_views[0].roomCode, "room code sizes");
    out->roomCode[sizeof out->roomCode - 1] = 0;   // (as snprintf's "%s": at most 7 characters and a terminator)
    for (size_t i = strnlen(out->roomCode, sizeof out->roomCode); i < sizeof out->roomCode; i++) out->roomCode[i] = 0;
}

int MpCore::uiLocalScan(MpConn conn) {
    {
        CoreLock l(*this);
        if (conn != MP_CONN_WIFI && conn != MP_CONN_NEARBY && conn != MP_CONN_LOOPBACK) return -1;
        if (m_phase != MP_UI_IDLE) return 0;
        const uint64_t t = now();
        if (!m_scanConn || m_scanKind != conn) {
            delete m_scanConn;
            MpConnectorParams p;
            memset(&p, 0, sizeof p);
            m_scanConn = makeConnector(conn, p);
            m_lastProbe = 0;   // netx: the first probe at once
            m_scanKind = conn;
            m_scanFailed = m_scanConn == 0;
            m_scanError = m_scanConn ? MP_ERR_NONE : MP_ERR_INTERNAL;   // mpwifi44
            snprintf(m_scanWhy, sizeof m_scanWhy, "%s", m_scanConn ? "" : "this build has no such link");
            m_scan.clear();
            if (m_scanConn) mp_log("scan start conn=%s", ConnName(conn));
        }
        m_scanLastCall = t;
        if (m_scanFailed) return -1;
        pumpScan(t);
        if (m_scanFailed) return -1;
    }
    ensureThread();
    CoreLock l(*this);
    return m_scan.count();
}

MpUiError MpCore::scanError(char* why, size_t cap) {
    CoreLock l(*this);
    if (why && cap) snprintf(why, cap, "%s", m_scanFailed ? m_scanWhy : "");
    return m_scanFailed ? (m_scanError != MP_ERR_NONE ? m_scanError : MP_ERR_INTERNAL) : MP_ERR_NONE;
}

void MpCore::scanRetry() {
    CoreLock l(*this);
    if (!m_scanFailed) return;
    delete m_scanConn;   // (a failed link holds nothing that works: the next uiLocalScan makes a new one)
    m_scanConn = 0;
    m_scanFailed = false;
    m_scanError = MP_ERR_NONE;
    m_scanWhy[0] = 0;
    m_scan.clear();
}

void MpCore::localName(char* out) {
    CoreLock l(*this);
    memcpy(out, m_name, MP_NAME_BYTES + 1);
    out[MP_NAME_BYTES] = 0;
}

bool MpCore::uiLocalGet(int index, MpLocalRoom* out) {
    CoreLock l(*this);
    return m_scan.get(index, out, 0, 0);
}

bool MpCore::pollEvent(MpCoreEvent* out) {
    if (__atomic_load_n(&m_evPub, __ATOMIC_ACQUIRE) == 0) return false;   // (perf) asked every turn: no lock when none
    CoreLock l(*this);
    if (!m_evCount || !out) return false;
    *out = m_events[m_evHead];
    m_evHead = (m_evHead + 1) % MP_EVENT_QUEUE;
    m_evCount--;
    return true;
}

void MpCore::setWhere(const void* payload, uint16_t len) {
    CoreLock l(*this);
    if (!payload || len < 8) return;
    memcpy(m_where, payload, 8);
    m_haveWhere = true;
    const uint16_t serial = (uint16_t)(m_where[6] | (m_where[7] << 8));
    if (serial != m_serial) clearRestartGate();
    m_serial = serial;
}

void MpCore::setRaceState(bool finished, uint32_t turns) {
    CoreLock l(*this);
    m_raceFinished = finished;
    m_raceTurns = turns;
}

bool MpCore::updateSettings(const MpSettings* s) {
    CoreLock l(*this);
    if (!s || !m_active || m_role != ROLE_HOST || !m_mem) return false;
    uint8_t buf[16];
    MpWriter w(buf, sizeof buf);
    mp_write_settings(w, *s);
    MpReader r(buf, w.n);
    MpSettings checked;
    if (!mp_read_settings(r, &checked) || checked.mode != m_settings.mode) return false;
    checked.flags = (uint8_t)((checked.flags & ~MP_SET_PUBLIC) | (m_settings.flags & MP_SET_PUBLIC));   // MP3 chat: stays public
    buf[7] = checked.flags;   // (the copy that goes out: flags is byte 7 of the settings on the wire)
    m_settings = checked;
    for (int q = 0; q < MP_MAX_PLAYERS; q++) {
        if (q == m_localSlot || !m_peers[q].used || m_peers[q].closing || !m_peers[q].resumed) continue;
        queueReliable(q, MP_MSG_SETTINGS, (uint8_t)m_localSlot, buf, (uint16_t)w.n);
    }
    mp_log("settings changed downed=%d", checked.downed);
    if (m_dying) finishEnd();
    return true;
}

void MpCore::suspend() {
    CoreLock l(*this);
    if (m_phase != MP_UI_LOBBY && m_phase != MP_UI_PLAYING) {
        if (m_phase != MP_UI_IDLE) {
            requestEnd(MP_ERR_NONE, "suspend");
            finishEnd();
        }
        return;
    }
    // remember the way back: the same session (LAN: by its id, whoever hosts it then) or the same room code (online)
    m_rejoinValid = true;
    m_rejoinAt = now();
    m_rejoinConn = m_connKind;
    m_rejoinSession = m_sessionId;
    saveRejoinPeers();
    m_rejoinWireId = m_wireId;
    snprintf(m_rejoinCode, sizeof m_rejoinCode, "%s", m_roomCode);
    if (m_role == ROLE_HOST) memset(&m_rejoinAddr, 0, sizeof m_rejoinAddr);
    else m_rejoinAddr = m_hostAddr;
    // a goodbye goes out right now (there is no time to wait for acknowledgements)
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        if (s == m_localSlot || !m_peers[s].used || m_peers[s].closing) continue;
        uint8_t r = MP_BYE_SUSPEND;
        queueReliable(s, MP_MSG_BYE, (uint8_t)m_localSlot, &r, 1);
    }
    flushPeers(now());
    mp_log("suspend: left the session (rejoin possible)");
    requestEnd(MP_ERR_NONE, "suspend");
    finishEnd();
}

bool MpCore::resume() {
    {
        CoreLock l(*this);
        if (!m_rejoinValid || m_phase != MP_UI_IDLE) return false;
        if (now() - m_rejoinAt > 120000) {
            m_rejoinValid = false;
            return false;
        }
        mp_log("resume: rejoining conn=%s", ConnName(m_rejoinConn));
        if (!startRejoinLocked()) return false;
    }
    ensureThread();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The global session and the bridge (port/mp_bridge.h)
namespace {

bool g_mpActive = false;   // mp_active(): one global bool read
bool g_mpHeld = false;     // MP3 lodgate: port_net_session_lockstep(): one global bool read (active, or a rejoin pending; see port/mp_gate.h)
#define MP_DEMO_ROSTER (g_mpDemoRoster && !g_mpActive)   // MP3 demo: the mock roster answers (never beside a real session)
MpCore* g_core = 0;
uint32_t g_dataHash = 0;
bool g_booted = false;   // fix3: mp_core_boot ran (the game): the console id comes from mp.cfg
MpSettings g_settingsCache;
char g_nameCache[MP_MAX_PLAYERS][MP_NAME_BYTES + 1];

// core2 (claims audit 4): in the game (the port booted), the session object is made only once the player has entered multiplayer
// (mp_entered: the menu, a session, a scan, the autopilot): before that no MpCore, no thread, no socket, no mp.cfg write (the
// console key is made with the core). Bridge calls that need it before then do nothing (CoreIfEntered).
static MpCore* CoreIfEntered();
static char s_ownCode[10];   // core2: this console's account code as the service layer last said
static void mp_core_remember_own_code(const char* code) {
    memset(s_ownCode, 0, sizeof s_ownCode);
    if (code && strlen(code) == 9) memcpy(s_ownCode, code, 9);
}
MpCore* Core() {
    if (!g_core) {
        if (g_booted && !mp_entered()) mp_log("core: made before multiplayer was entered");
        MpCoreOptions o;
        memset(&o, 0, sizeof o);
        const char* nt = getenv("SMG_MP_NO_THREAD");
        o.useThread = !(nt && *nt == '1');
        o.activeFlag = &g_mpActive;
        o.heldFlag = &g_mpHeld;
        g_core = new MpCore(o);
        g_core->setDataHash(g_dataHash);
        const MpConfig* c = mp_config();
        g_core->setProfile(c->name, (MpLook)c->look);
        g_core->setOwnFriendCode(s_ownCode);   // core2 (the service may have told it before the core existed)
        // fix3: the game's console id lives in mp.cfg (made on this first use of multiplayer). Processes that never booted the
        // port (mp_bot, host tests) keep the random id of the run and never write mp.cfg.
        if (g_booted) {
            uint8_t key[16];
            if (mp_config_console_key(key)) g_core->setConsoleKey(key);
            else g_core->setConsoleKeyFailed();   // smallfix
        }
    }
    return g_core;
}

static MpCore* CoreIfEntered() { return g_core ? g_core : (!g_booted || mp_entered() ? Core() : 0); }

// Desktop integration autopilot (SMG_MP_AUTO=host|join|join_addr:<ip[:port]>; SMG_MP_AUTO_JOIN_ANY=1; SMG_MP_AUTO_CONN=wifi|loopback|nearby|direct;
// SMG_MP_AUTO_MODE=together|race;
// SMG_MP_AUTO_START=N: the host starts when N players are present). Runs on the game thread from mp_tick_game.
struct Auto {
    int state;   // 0 unread, 1 off, 2 hosting, 3 scanning, 4 joining, 5 done
    MpConn conn;
    MpMode mode;
    int startAt;
} g_auto = {0, MP_CONN_WIFI, MP_MODE_TOGETHER, 0};

void AutoStep() {
    if (g_auto.state == 0) {
        const char* a = MP_TEST_GETENV("SMG_MP_AUTO");
        if (!a || !*a) { g_auto.state = 1; return; }
        const char* c = MP_TEST_GETENV("SMG_MP_AUTO_CONN");
        if (c && !strcmp(c, "nearby")) g_auto.conn = MP_CONN_NEARBY;
        if (c && !strcmp(c, "direct")) g_auto.conn = MP_CONN_DIRECT;   // netx: host by address
        const char* m = MP_TEST_GETENV("SMG_MP_AUTO_MODE");
        if (m && !strcmp(m, "race")) g_auto.mode = MP_MODE_RACE;
        const char* s = MP_TEST_GETENV("SMG_MP_AUTO_START");
        g_auto.startAt = s ? atoi(s) : 0;
        if (!strcmp(a, "host")) {
            MpHostParams p;
            memset(&p, 0, sizeof p);
            p.conn = g_auto.conn;
            p.settings = mp_config_host_settings(g_auto.mode);
            g_auto.state = Core()->uiHost(&p) ? 2 : 1;
            mp_log("auto: host %s", g_auto.state == 2 ? "started" : "failed");
        } else if (!strcmp(a, "join")) {
            g_auto.state = 3;
            mp_log("auto: scanning");
        } else if (!strncmp(a, "join_addr:", 10)) {
            // netx: join by address (the menu's "Join by Address" with the typed text)
            const bool ok = Core()->uiJoinAddress(a + 10);
            g_auto.state = ok ? 4 : 1;
            g_auto.conn = MP_CONN_DIRECT;
            mp_log("auto: join by address %s: %s", a + 10, ok ? "started" : "refused");
#if defined(TARGET_PC) && !defined(__vita__)
        } else if (!strncmp(a, "join_code:", 10)) {
            // hooks: join through the spoken-code path (the menu's "Join with a Code": mp_ui_join_direct_code, which the gathering tick then follows).
            // join_code:<code> = that code as typed; join_code:wrong = a well-formed code for 127.0.0.1:$SMG_MP_DIRECT_PORT (default 42611) whose
            // secret (01 02 03 04 05) is not the room's, so the host must answer "hello rejected reason=proof". Desktop only.
            char made[MP_DIRECT_CODE_MAX];
            const char* code = a + 10;
            if (!strcmp(code, "wrong")) {
                const char* pe = getenv("SMG_MP_DIRECT_PORT");
                const uint8_t secret[MP_DIRECT_SECRET_BYTES] = {1, 2, 3, 4, 5};
                if (mp_direct_code_encode(0x7F000001u, (uint16_t)(pe && atoi(pe) > 0 ? atoi(pe) : 42611), secret, made, sizeof made) > 0) code = made;
            }
            const int r = mp_ui_join_direct_code(code);
            g_auto.state = r == MP_CODE_OK ? 6 : 1;
            g_auto.conn = MP_CONN_DIRECT;
            mp_log("auto: join by code %s: %s", code, r == MP_CODE_OK ? "started" : "refused");
#endif
        } else {
            g_auto.state = 1;
        }
        return;
    }
    MpCore* core = Core();
#if defined(TARGET_PC) && !defined(__vita__)
    if (g_auto.state == 6) {   // hooks: the code join runs inside the gathering driver: tick it as the menu's screen does, until the session forms or fails
        MpGatherStatus gs;
        mp_gather_status(&gs);
        MpUiStatus st;
        core->uiStatus(&st);
        if (st.phase == MP_UI_LOBBY || st.phase == MP_UI_PLAYING) g_auto.state = 5;
        else if (gs.state == MP_GATHER_FAILED || (gs.state == MP_GATHER_OFF && st.phase == MP_UI_IDLE)) {
            mp_log("auto: join by code ended without a session (gather state %d, error %d)", (int)gs.state, (int)st.error);
            g_auto.state = 5;
        }
        return;
    }
#endif
    if (g_auto.state == 2) {
        MpUiStatus st;
        core->uiStatus(&st);
        if (st.phase == MP_UI_LOBBY && g_auto.startAt > 0 && st.players >= g_auto.startAt) {
            core->uiStart();
            g_auto.state = 5;
        } else if (st.phase == MP_UI_IDLE) {
            g_auto.state = 5;
        }
    } else if (g_auto.state == 3) {
        // SMG_MP_AUTO_JOIN_ANY=1 (tests): also try a room of a different version once, so the refusal and its message
        // (MP_ERR_DIFFERENT_VERSION, "join refused: ... different version") are exercised; the scan goes on after it
        static int sJoinAny = -1;
        static bool sTriedIncompatible = false;
        if (sJoinAny < 0) sJoinAny = MP_TEST_GETENV("SMG_MP_AUTO_JOIN_ANY") && atoi(MP_TEST_GETENV("SMG_MP_AUTO_JOIN_ANY")) != 0 ? 1 : 0;
        const int n = core->uiLocalScan(g_auto.conn);
        for (int i = 0; i < n; i++) {
            MpLocalRoom r;
            if (!core->uiLocalGet(i, &r)) continue;
            if (!r.compatible) {
                if (sJoinAny && !sTriedIncompatible) {
                    sTriedIncompatible = true;
                    const bool ok = core->uiJoinLocal(g_auto.conn, i);
                    MpUiStatus st;
                    core->uiStatus(&st);
                    mp_log("auto: tried incompatible room %s: %s error=%d", r.host, ok ? "joining" : "refused", (int)st.error);
                    if (ok) { g_auto.state = 4; break; }
                }
                continue;
            }
            if (core->uiJoinLocal(g_auto.conn, i)) {
                g_auto.state = 4;
                mp_log("auto: joining %s", r.host);
                break;
            }
        }
    } else if (g_auto.state == 4) {
        MpUiStatus st;
        core->uiStatus(&st);
        if (st.phase == MP_UI_IDLE && g_auto.conn == MP_CONN_DIRECT) {
            // netx: by address there is no scan: ask the same address again a moment later
            static uint64_t sRetryAt = 0;
            const uint64_t t = mp_sys_now_ms();
            if (!sRetryAt) sRetryAt = t + 2000;
            if (t >= sRetryAt) {
                sRetryAt = 0;
                const char* a = MP_TEST_GETENV("SMG_MP_AUTO");
                const bool ok = a && !strncmp(a, "join_addr:", 10) && core->uiJoinAddress(a + 10);
                mp_log("auto: join by address again: %s", ok ? "started" : "refused");
            }
        } else if (st.phase == MP_UI_IDLE) g_auto.state = 3;   // failed: scan again
        else if (st.phase == MP_UI_LOBBY || st.phase == MP_UI_PLAYING) g_auto.state = 5;
    }
}

}  // namespace

extern "C" {

bool mp_active(void) { return __atomic_load_n(&g_mpActive, __ATOMIC_RELAXED); }
// PETARI_LOCKSTEP_TEST=1 (a test switch, default 0, read once at start): this strong definition overrides the weak default in perf.cpp, so the switch is read HERE: a single-player run
// then answers 1 and shows what a session switches off (the "lockstep:" line). Unset: exactly the held flag.
static const int g_lockstepTest = (getenv("PETARI_LOCKSTEP_TEST") != nullptr && atoi(getenv("PETARI_LOCKSTEP_TEST")) != 0) ? 1 : 0;
int port_net_session_lockstep(void) { return __atomic_load_n(&g_mpHeld, __ATOMIC_RELAXED) | g_lockstepTest; }
MpMode mp_mode(void) { return g_mpActive ? Core()->mode() : MP_MODE_NONE; }
bool mp_is_host(void) { if (MP_DEMO_ROSTER) return mp_demo_roster_is_host(); return g_mpActive && Core()->isHost(); }   // MP3 demo: (mock roster: see port/mp_demo.h)
int mp_local_slot(void) { if (MP_DEMO_ROSTER) return mp_demo_roster_local_slot(); return g_mpActive ? Core()->localSlot() : -1; }
int mp_host_slot(void) { if (MP_DEMO_ROSTER) return mp_demo_roster_is_host() ? mp_demo_roster_local_slot() : -1; return g_mpActive ? Core()->hostSlot() : -1; }
uint16_t mp_host_epoch(void) { return g_mpActive ? Core()->hostEpoch() : 0; }
bool mp_slot_present(int slot) { if (MP_DEMO_ROSTER) return mp_demo_roster_present(slot); return g_mpActive && Core()->slotPresent(slot); }
const char* mp_slot_name(int slot) {   // (MP3 profile: a read, no allocation: no host-alloc scope, two emulated-TLS calls a turn per tag)
    if (slot < 0 || slot >= MP_MAX_PLAYERS) return "";
    g_nameCache[slot][0] = 0;
    if (MP_DEMO_ROSTER) { strncpy(g_nameCache[slot], mp_demo_roster_name(slot), MP_NAME_BYTES); g_nameCache[slot][MP_NAME_BYTES] = 0; return g_nameCache[slot]; }
    if (g_mpActive) Core()->slotName(slot, g_nameCache[slot]);
    return g_nameCache[slot];
}
MpLook mp_slot_look(int slot) { if (MP_DEMO_ROSTER) return (MpLook)mp_demo_roster_look(slot); return g_mpActive ? Core()->slotLook(slot) : MP_LOOK_MARIO; }
int mp_slot_ping_ms(int slot) { if (MP_DEMO_ROSTER) return mp_demo_roster_ping(slot); return g_mpActive ? Core()->slotPing(slot) : -1; }
bool mp_slot_quiet(int slot) { if (MP_DEMO_ROSTER) return mp_demo_roster_quiet(slot); return g_mpActive && Core()->slotQuiet(slot); }
const MpSettings* mp_settings(void) {   // (MP3 profile: a read, no allocation: no host-alloc scope; called many times a turn)
    if (g_core) g_core->settings(&g_settingsCache);
    else {
        memset(&g_settingsCache, 0, sizeof g_settingsCache);
        g_settingsCache.maxPlayers = 2;
        g_settingsCache.interaction = 1;
        g_settingsCache.sharedKills = 1;
        g_settingsCache.rivals = 1;
        g_settingsCache.raceWaitSec = 18;
        g_settingsCache.nameTags = 1;
        g_settingsCache.flags = 7 | MP_SET_TEXT_OFF;
        g_settingsCache.downed = MP_DOWNED_BUBBLES;
    }
    if (MP_DEMO_ROSTER) g_settingsCache.maxPlayers = 4;   // MP3 demo: room for every spare puppet (no real session: the core's settings are not touched)
    return &g_settingsCache;
}
uint16_t mp_scene_serial(void) { if (MP_DEMO_ROSTER) return 1; return g_mpActive ? Core()->sceneSerial() : 0; }
bool mp_send(uint8_t type, const void* payload, uint16_t len, bool reliable) { MP_HOST_ALLOC;
    MP_PROF_SCOPE(MP_PROF_NET);
    return g_mpActive && Core()->send(type, payload, len, reliable);
}
bool mp_send_to(int slot, uint8_t type, const void* payload, uint16_t len, bool reliable) { MP_HOST_ALLOC;
    MP_PROF_SCOPE(MP_PROF_NET);
    return g_mpActive && Core()->sendTo(slot, type, payload, len, reliable);
}
bool mp_poll(MpMsg* out) { MP_HOST_ALLOC;
    MP_PROF_SCOPE(MP_PROF_NET);
    return g_core && g_core->poll(out);
}
uint32_t mp_poll_many(MpMsg* out, uint32_t max) { MP_HOST_ALLOC;   // netopt: up to `max` messages under one lock
    MP_PROF_SCOPE(MP_PROF_NET);
    return g_core ? g_core->pollMany(out, max) : 0;
}
uint32_t mp_poll_states(MpMsg* out, uint32_t max) { MP_HOST_ALLOC;   // wave8: pre-update STATE drain
    MP_PROF_SCOPE(MP_PROF_NET);
    return g_core ? g_core->pollStates(out, max) : 0;
}
void mp_tick_game(uint32_t turn) { MP_HOST_ALLOC;
    MP_PROF_SCOPE(MP_PROF_NET);
    // MP3 harness: SMG_MP_AUTO_GO_FILE=<path>: the autopilot opens its session only once that file exists (the harness's supervisor creates it when
    // the console is in play: the first "port: player at <x y z>" line of a real position, i.e. after the file select). The product's
    // singlePlayerGate refuses Single Player while a session is active, so an autopilot that hosts at boot never gets past the entry screen.
    // Polled twice a second. Unset: the autopilot starts at once, as before.
#if defined(TARGET_PC) && !defined(__vita__)
    static const char* sGoFile = MP_TEST_GETENV("SMG_MP_AUTO_GO_FILE");
    static bool sGone = false;
    if (sGoFile && *sGoFile && !sGone && g_auto.state == 0) {
        if (turn % 30 == 0 && access(sGoFile, F_OK) == 0) sGone = true;
        if (!sGone) { if (g_core) g_core->tick(turn); return; }
    }
#endif
    if (g_auto.state != 1) AutoStep();
    if (g_core) g_core->tick(turn);
}
bool mp_save_suppressed(void) { return g_mpActive && Core()->saveSuppressed(); }
bool mp_save_autoconfirm(void) { return g_mpActive && Core()->saveAutoconfirm(); }
void mp_snapshot_offer(const void* image, uint32_t len) { MP_HOST_ALLOC; if (g_mpActive) Core()->snapshotOffer(image, len); }
uint32_t mp_snapshot_ready(void) { MP_HOST_ALLOC; return g_core ? g_core->snapshotReady() : 0; }
uint32_t mp_snapshot_get(void* out, uint32_t cap) { MP_HOST_ALLOC; return g_core ? g_core->snapshotGet(out, cap) : 0; }
void mp_snapshot_consumed(void) { MP_HOST_ALLOC; if (g_core) g_core->snapshotConsumed(); }

void mp_ui_set_profile(const char* nameUtf8, MpLook look) { MP_HOST_ALLOC;
    MpConfig c = *mp_config();
    // (coordinator, found by the scenario sweep) An empty name keeps the name already chosen: the character select passes the
    // online account's name, which is empty for a player who never registered, and that turned every such player into "Player".
    // (13 Nov, default name) A name that differs from the one in use is one the player typed: kept from then on (nameTyped). An empty
    // name, or the one already in use, changes nothing: the name stays the character's and follows a change of character (mp_config Clamp).
    if (nameUtf8 && nameUtf8[0]) {
        char nm[MP_NAME_BYTES + 1];
        mp_name_clean(nameUtf8, strlen(nameUtf8), nm);
        if (nm[0] && strcmp(nm, c.name) != 0) {
            memcpy(c.name, nm, sizeof nm);
            c.nameTyped = 1;
        }
    }
    c.look = (uint8_t)(look < MP_LOOK_COUNT ? look : MP_LOOK_MARIO);
    mp_config_set(&c);
    mp_config_save();
    if (g_core) g_core->setProfile(mp_config()->name, (MpLook)mp_config()->look);   // (core2: a new core takes it from mp.cfg)
}
bool mp_ui_host(const MpHostParams* p) { MP_HOST_ALLOC; mp_mark_entered(); return Core()->uiHost(p); }
bool mp_ui_join_code(MpConn conn, const char* code) { MP_HOST_ALLOC; mp_mark_entered(); return Core()->uiJoinCode(conn, code); }
bool mp_ui_join_friend(const char* friendCode) { MP_HOST_ALLOC; mp_mark_entered(); return Core()->uiJoinFriend(friendCode); }
bool mp_ui_quick_match(MpMode mode) { MP_HOST_ALLOC; mp_mark_entered(); return Core()->uiQuickMatch(mode); }
bool mp_ui_join_local(MpConn conn, int listIndex) { MP_HOST_ALLOC; mp_mark_entered(); return Core()->uiJoinLocal(conn, listIndex); }
bool mp_ui_join_local_room(MpConn conn, uint32_t roomId) { MP_HOST_ALLOC; mp_mark_entered(); return Core()->uiJoinLocalRoom(conn, roomId); }
bool mp_ui_join_address(const char* textUtf8) { MP_HOST_ALLOC; mp_mark_entered(); return Core()->uiJoinAddress(textUtf8); }
bool mp_ui_host_address(MpAddressInfo* out) { MP_HOST_ALLOC; return g_core && g_core->uiHostAddress(out); }
bool mp_ui_start(void) { MP_HOST_ALLOC; mp_mark_entered(); return Core()->uiStart(); }
void mp_ui_leave(void) { MP_HOST_ALLOC; if (CoreIfEntered()) g_core->uiLeave(); }
void mp_ui_kick(int slot) { MP_HOST_ALLOC; if (CoreIfEntered()) g_core->uiKick(slot); }
void mp_ui_status(MpUiStatus* out) { MP_HOST_ALLOC;
    if (!out) return;
    if (!g_core) {
        memset(out, 0, sizeof *out);
        out->phase = MP_UI_IDLE;
        out->error = MP_ERR_NONE;
        return;
    }
    g_core->uiStatus(out);
}
int mp_ui_local_scan(MpConn conn) { MP_HOST_ALLOC; mp_mark_entered(); return Core()->uiLocalScan(conn); }
bool mp_ui_local_get(int index, MpLocalRoom* out) { MP_HOST_ALLOC; return out && g_core && g_core->uiLocalGet(index, out); }

// ---- internal C API (mp_core.h) ----
void mp_core_boot(void) { MP_HOST_ALLOC;
    mp_config_load();
    g_booted = true;
    { const char* a = MP_TEST_GETENV("SMG_MP_AUTO"); if (a && *a) mp_mark_entered(); }   // MP3 netpack: the desktop autopilot runs from mp_tick_game
    if (g_core) g_core->setProfile(mp_config()->name, (MpLook)mp_config()->look);
    if (g_core) {
        uint8_t key[16];
        if (mp_config_console_key(key)) g_core->setConsoleKey(key);
        else g_core->setConsoleKeyFailed();   // smallfix
    }
    mp_log("core ready build=%s proto=%d", MP_BUILD_ID, MP_PROTO_VERSION);
}
void mp_core_shutdown(void) { MP_HOST_ALLOC;
    if (g_core) g_core->shutdown();
}
bool mp_core_goodbye(unsigned budgetMs) { MP_HOST_ALLOC; return !g_core || g_core->goodbye(budgetMs); }
void mp_core_set_data_hash(uint32_t hash) { MP_HOST_ALLOC;
    g_dataHash = hash;
    if (g_core) g_core->setDataHash(hash);
    const uint32_t fp = FingerprintOf(hash);
    if (mp_service_set_build) mp_service_set_build(fp);   // integ: the service's / signaling's X-Msc-Build tag
    mp_log("core data hash=%08x fingerprint=%08x", (unsigned)hash, (unsigned)fp);
}
uint32_t mp_core_fingerprint(void) { MP_HOST_ALLOC; return CoreIfEntered() ? g_core->fingerprint() : FingerprintOf(g_dataHash); }
bool mp_core_poll_event(MpCoreEvent* out) { MP_HOST_ALLOC; return g_core && g_core->pollEvent(out); }
void mp_core_set_where(const void* payload, uint16_t len) { MP_HOST_ALLOC; if (CoreIfEntered()) g_core->setWhere(payload, len); }
void mp_core_set_race_state(bool finished, uint32_t turns) { MP_HOST_ALLOC; if (CoreIfEntered()) g_core->setRaceState(finished, turns); }
bool mp_core_update_settings(const MpSettings* s) { MP_HOST_ALLOC; return g_core && g_core->updateSettings(s); }
void mp_core_suspend(void) { MP_HOST_ALLOC; if (g_core) g_core->suspend(); }
bool mp_core_resume(void) { MP_HOST_ALLOC; mp_mark_entered(); return g_core && g_core->resume(); }
void mp_flow_set_scene_serial(uint16_t serial) { MP_HOST_ALLOC; if (g_core) g_core->setSceneSerial(serial); }
const char* mp_core_saved_name(void) { MP_HOST_ALLOC; return mp_config()->name; }
void mp_core_report_bad(int slot, const char* why) { MP_HOST_ALLOC; if (g_core) g_core->reportBad(slot, why); }
bool mp_core_slot_player(int slot, MpPlayerInfo* out) { MP_HOST_ALLOC; if (out) memset(out, 0, sizeof *out); return g_core && g_core->slotPlayer(slot, out); }
void mp_core_set_own_friend_code(const char* code) {
    MP_HOST_ALLOC;
    mp_core_remember_own_code(code);
    if (g_core) g_core->setOwnFriendCode(code);
}
bool mp_core_slot_friend_code(int slot, char* out, size_t cap) {   // core2
    MP_HOST_ALLOC;
    if (out && cap) out[0] = 0;
    return g_core && g_core->slotFriendCode(slot, out, cap);
}
uint8_t mp_core_saved_look(void) { MP_HOST_ALLOC; return mp_config()->look < MP_LOOK_COUNT ? mp_config()->look : (uint8_t)MP_LOOK_MARIO; }
uint8_t mp_core_saved_downed(void) { MP_HOST_ALLOC; return mp_config()->downed < MP_DOWNED_COUNT ? mp_config()->downed : (uint8_t)MP_DOWNED_BUBBLES; }
void mp_core_save_downed(uint8_t downed) { MP_HOST_ALLOC;
    if (downed >= MP_DOWNED_COUNT || mp_config()->downed == downed) return;
    MpConfig c = *mp_config();
    c.downed = downed;
    mp_config_set(&c);
    mp_config_save();
}
// entry: the entry screen's default cursor (mp.cfg "entry"); written only when it changes
uint8_t mp_core_saved_entry(void) { MP_HOST_ALLOC; return mp_config()->entry <= 1 ? mp_config()->entry : (uint8_t)0; }
void mp_core_save_entry(uint8_t choice) { MP_HOST_ALLOC;
    if (choice > 1 || mp_config()->entry == choice) return;
    MpConfig c = *mp_config();
    c.entry = choice;
    mp_config_set(&c);
    mp_config_save();
}

bool mp_core_rules_hidden(void) { MP_HOST_ALLOC; return mp_config()->rulesHide != 0; }
bool mp_core_rules_wanted(uint32_t hostKey) { MP_HOST_ALLOC;
    if (hostKey == 0 || mp_config()->rulesHide) return false;
    for (int i = 0; i < 4; i++) if (mp_config()->rulesSeen[i] == hostKey) return false;
    return true;
}
void mp_core_rules_done(uint32_t hostKey, bool never) { MP_HOST_ALLOC;
    MpConfig c = *mp_config();
    bool changed = false;
    if (never && !c.rulesHide) { c.rulesHide = 1; changed = true; }
    if (hostKey != 0 && c.rulesSeen[0] != hostKey) {   // newest first, four kept
        int at = 3;
        for (int i = 0; i < 4; i++) if (c.rulesSeen[i] == hostKey) { at = i; break; }
        for (int i = at; i > 0; i--) c.rulesSeen[i] = c.rulesSeen[i - 1];
        c.rulesSeen[0] = hostKey;
        changed = true;
    }
    if (!changed) return;
    mp_config_set(&c);
    mp_config_save();
}

}  // extern "C"

// tests / mp_bot: the global core on a loopback hub
void mp_core_set_loopback(MpLoopHub* hub, uint32_t endpoint) { MP_HOST_ALLOC; mp_mark_entered(); Core()->setLoopback(hub, endpoint); }
MpCore* mp_core_global(void) { MP_HOST_ALLOC; return Core(); }
