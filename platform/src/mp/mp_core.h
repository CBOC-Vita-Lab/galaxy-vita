// Galaxy 3.0 multiplayer (port side, owner: core): the session layer. Internal API (port side, the integration and the tests);
// the game side uses port/mp_bridge.h (implemented by mp_core.cpp on top of the one global MpCore).
//
// Integration (all on the game thread):
//   boot:       mp_core_boot()                      loads mp.cfg; nothing else runs until a session or a scan is started
//   data hash:  mp_core_set_data_hash(h)            before the first session (fingerprint input: StageData file list hash)
//   per turn:   mp_tick_game(turn)                  (bridge) pumps the session on the game thread; a helper thread pumps it too
//               while a session or a scan exists, so loading screens and pauses never time peers out
//   game state: mp_core_set_where(...)              the current WHERE payload (the core also snoops WHERE/MOVE itself)
//               mp_core_set_race_state(...)         this console's race state for RESUME after a host migration
//               mp_core_poll_event(&ev)             joins, leaves, host changes (re-send SWITCHMAP / race state), RESUMEs
//   power:      Vita: the network thread watches sleep / Wi-Fi itself (mp_core_vita.cpp) and reconnects (MpUiStatus shows
//               phase MP_UI_RECONNECTING with error MP_ERR_NO_WIFI = "Reconnecting...", for up to 30 s). mp_core_suspend() /
//               mp_core_resume() do the same by hand (clean leave, fast rejoin of the same session).
//   Wi-Fi Turbo: mp_wifi_turbo_installed() (mp_core_sys.h) for the UI's tip
//   shutdown:   mp_core_shutdown()                  leaves the session (BYE), stops the helper thread
//
// Wire format (DESIGN.md section 3; details owned by core):
//   header 12 bytes: 'G' 'M' proto type from flags seq(u16) ack(u16) ackbits(u16), little-endian.
//     flags bit0 reliable (seq = hop channel sequence), bit1 ack fields valid, bit2 ack only (type 0, no payload).
//     unreliable: seq = the ORIGINAL sender's 16-bit counter (latest wins per sender and type, also through the host's relay).
//   reliable game messages (0x20..0x5F) carry a 2-byte origin sequence before the game payload (0 = not tracked; tracked for
//   every type the host relays): every console de-duplicates relayed events by (origin slot, origin sequence) and keeps a
//   history of them, so that after a host migration nothing is lost or delivered twice.
//   core payload extensions (beyond DESIGN.md's field lists, appended): HELLO + sessionId u32 (0 = any) + consoleId u32
//   (fix2: never 0; a kick bans that console for the session, a name is never banned; fix3: kept in mp.cfg, so a restart is
//   the same console);
//   WELCOME = slot, sessionId, mode, settings, epoch u16, hostSlot u8, seen[4] u16, serial u16, roster;
//   ROSTER entry = slot, name[16], look, state, ping u16, gen u16, addr u32, port u16, gid u32;
//   HOST_CHANGE + sessionId u32 + seen[4] u16; RESUME + seen[4] u16 + snapshot crc u32 + consoleId u32 (fix3: a new host
//   adopts its guests without a HELLO; the RESUME tells it their ids, so it can still ban them).
//   Characters (MpLook) are unique: HELLO carries the wished look, the host assigns it if free, else the first free one (a
//   rejoin keeps its old look if still free); WELCOME / ROSTER carry the assignment (mp_slot_look reads the roster, also for
//   the local slot). A change (mp_ui_set_profile in a session) goes to the host as a 1-byte LOOK {look} (untracked, never
//   delivered to a game); the host takes it only if free and answers with ROSTER. In every relayed / delivered LOOK the host
//   overwrites the look byte with the slot's assigned look.
#ifndef PORT_MP_CORE_H
#define PORT_MP_CORE_H

#include "port/mp_bridge.h"
#include "port/mp_chat.h"   // MP3 chat
#include "port/mp_core_game.h"
#include "mp_config.h"
#include "mp_core_sys.h"
#include "mp_lan.h"
#include "mp_reliable.h"
#include "mp_siphash.h"
#include "mp_snapshot.h"
#include "mp_transport.h"

class MpLoopHub;

#ifndef MP_BUILD_ID
#define MP_BUILD_ID "dev"
#endif

// Timings (ms)
#define MP_KEEPAWAKE_MS 50           // MP3 netpack: in a session every peer link carries a datagram at least this often each way (Wi-Fi power save drops in after ~100 ms of silence)
#define MP_LOG_BURST 8               // MP3 netpack: log lines a stranger can trigger: burst, then 2 per second per call site
#define MP_RECV_BUDGET_DATAGRAMS 128 // MP3 netpack: packets (netcore: inner packets of bundles count) handled per receiveAll
#define MP_RECV_BUDGET_US 1500       // ... or this much time
#define MP_HEARTBEAT_MS 100          // DESIGN.md section 9: never let the Wi-Fi go quiet (power save) during a session (netopt: a link that carries other
                             // datagrams needs no heartbeat of its own: see MP_HEARTBEAT_PIGGY_MS; a silent link still gets one every MP_KEEPAWAKE_MS)
#define MP_STATE_SERIAL_STALE_MS 1000 // netopt: a guest's last pose is trusted to tell its scene this long (see noteStateSerial)
#define MP_HEARTBEAT_PIGGY_MS 500    // netopt: while other traffic flows the clock/echo (ping) heartbeat rides inside a datagram that leaves anyway, this often
#define MP_QUIET_MS 3000
#define MP_JOIN_GRACE_MS 500         // outside review: a full room lets the oldest never-acknowledging joiner past this go
#define MP_COOKIE_RETRY_MS 2000      // core2: a cookie without a WELCOME this long: the join attempt starts over
#define MP_DEPARTED_MS 30000          // core2: a removed console still sending with its old key is told so this long
#define MP_PROOF_FAILS_PER_SEC 4      // core2: HELLOs with a wrong proof (a guessed code secret) a host looks at per second, from one source address
#define MP_HELLO1_PER_SEC 1000        // corefix2: cookies a host answers per second to unknown sources (stateless; measured: see TestHello1FloodFair); no other budget is spent
#define MP_HELLO1_KNOWN_PER_SEC 100   // corefix2: ... and, apart from it, to the consoles the room knows
#define MP_PAIRKEY_RESERVE 6          // corefix2: of the key-exchange budget, this many per second are for sources that never failed a proof
#define MP_SUSPECT_MS 30000           // corefix2: a source that failed a proof is a suspect for this long
#define MP_PAIRKEYS_PER_SEC 20        // corefix D4: X25519 operations a host spends on HELLO2s per second (a known console is not held to it)
#define MP_PROOF_FAILS_ALL_PER_SEC 16 // corefix D3: ... and from all unknown sources together (bounds the total work); a console the room knows (a roster member, a
                                      // recent leaver: vouchedAddr) is only held to its own allowance, so a stranger cannot spend the honest players' share
#define MP_WAKE_GAP_MS 2000           // core2: no pump this long = this console was frozen: the others' timeouts start again
#define MP_HOST_LOST_MS 5000          // guests: host silent this long -> host migration
#define MP_MIGRATE_STEP_MS 3000       // the k-th successor in line claims k steps later
#define MP_CLAIM_MS 4000              // a claiming host that hears no RESUME in this time is the isolated one
#define MP_RESUME_WAIT_MS 6000        // guests that do not RESUME within this time after a claim are removed
#define MP_STARVE_ROTATE_MS 1000      // corefix2: a takeover with fewer windows than live guests hands a window on every this often, from one that has not answered
#define MP_ACK_STALL_MS 5000          // corefix: host: a JOINER that never acknowledged anything (netcore: not established) is removed after this
#define MP_MERGE_MEMORY_MS 60000      // corefix: a removed member's address still vouches for a merge beacon this long
#define MP_MERGE_BEACONS 3            // corefix: consecutive beacons of the other room needed (over MP_MERGE_SPAN_MS) before this host merges
#define MP_MERGE_SPAN_MS 1500
#define MP_PEER_LOST_MS 15000         // host: a silent guest is removed
#define MP_ESTAB_NOACK_MS 60000       // starfix round 3: host: an ESTABLISHED guest that still sends but acknowledges nothing is removed after this IF it said it is busy (MP_HDR_BUSY
                                      // within MP_BUSY_FRESH_MS: its receive gate refused a message of ours: a 25-40 s load with a full queue); every other silent-ack peer keeps
                                      // MP_PEER_LOST_MS (a 60 s hold for every ghost kept a 4-slot room full in the churn soak). Memory stays bounded (fixed queue, capped backlog).
#define MP_BUSY_FRESH_MS 6000         // (the BUSY window is MP_GATE_GRACE_MS = 2 s, the resend ceiling of a slow link: 6 s spans a few missed refusals)
#define MP_NAME_TAKEOVER_MS 5000      // corefix: peers without a console id: a same-name HELLO takes the slot only after this much silence
#define MP_REJOIN_MS 15000            // a player who left can take the same slot back by name within this time
#define MP_CONNECT_TIMEOUT_MS 10000   // HELLO -> WELCOME
#define MP_LINK_TIMEOUT_MS 30000      // link / online room bring-up
#define MP_LEAVE_FLUSH_MS 600
#ifndef MP_GATE_GRACE_MS
#define MP_GATE_GRACE_MS 2000         // netfix B2: after a refusal by our own gate the peer's rate allowance is x4 for this long; starfix: and our packets to it carry MP_HDR_BUSY for this long
#endif
#define MP_RATE_CAP 200               // messages per second per guest
#define MP_BAD_KICK 50                // bad messages in MP_BAD_WINDOW_MS -> kicked
#define MP_BAD_WINDOW_MS 10000
#define MP_QUEUE_SIZE 256
#define MP_QUEUE_ORIGIN 160           // core2: reliable messages of one origin waiting for the game (a migration's catch-up is 100-200)
#define MP_USEQ_WINDOW 1024           // core2: an unreliable sequence this far ahead of the last accepted one is refused...
#define MP_USEQ_STALE_MS 2000         // ... unless that origin's unreliable stream was silent this long
#define MP_QINLINE 128                // MP3 netpack (memory): queued payload kept inline up to this (every game message but SWITCHMAP)
#define MP_QBIG 16                    // ... longer ones (SWITCHMAP, 568 bytes) use one of this many big blocks
#define MP_QBIG_BYTES 576
#define MP_HISTORY_ENTRIES 512
#define MP_HISTORY_BYTES (64 * 1024)
#define MP_GAME_PREFIX 4              // netcore: a reliable game message starts with origin seq u16 (0 = untracked) and origin gen u16
// core2: a membership generation is 16 bits: low byte = the order (each join of a slot takes the next), high byte = random per admission,
// so two hosts of a split session that admit different consoles into one slot never give them the same generation (soak: two streams
// were taken for one). Equal = all 16 bits; newer = the low byte ahead (8-bit window), or the same low byte and another high byte.
typedef uint16_t MpGen;
static inline bool GenNewer(MpGen a, MpGen b) { const int8_t d = (int8_t)(uint8_t)((a & 0xFF) - (b & 0xFF)); return d > 0 || (d == 0 && a != b); }
static inline int GenAhead(MpGen a, MpGen b) { return (int8_t)(uint8_t)((a & 0xFF) - (b & 0xFF)); }
#define MP_EVENT_QUEUE 32
#define MP_RECONNECT_MS 30000         // give up reconnecting after this long (a lost Wi-Fi / a sleep the OS reported)
#define MP_LINKLOST_RECONNECT_MS 75000 // corefix D1: ... and after this long when the link went silent without a word (a console alone after
                                       // MP_HOST_LOST_MS suspects its own link first): see share/core/RECONNECT.md
#define MP_LINKLOST_HOLD_MS 70000     // corefix D1: host: a guest removed for silence keeps its slot reserved this long (its rejoin window)

enum MpHdrFlag : uint8_t { MP_HDR_RELIABLE = 1, MP_HDR_ACK = 2, MP_HDR_ACK_ONLY = 4,
                                MP_HDR_BUSY = 8 };   // starfix: sender's own receive gate refused a message lately (its game reads nothing: a load): back-pressure, not a dead link; old peers ignore the bit
#define MP_HEADER_BYTES 12
#define MP_DATAGRAM_MAX 1100
#define MP_BUNDLE_HEADER 4            // MP3 netpack: 'G' 'B' proto count (wire v3: packets for one peer share a datagram)
#define MP_BUNDLE_MAX 64
#ifndef MP_BUNDLE_HOLD_MS   // (netcore: overridable for the hold measurement, tests/mp/netcore_measure.cpp)
#define MP_BUNDLE_HOLD_MS 10          // MP3 netpack: the host's poses wait at most this long for company in a datagram (everything else goes at
                                      // once); netcore: 25 -> 10 ms and only with >= 2 guests (netcore_measure: 4 players 98 -> 49 datagrams/s
                                      // a host link for +10 ms; 25 ms gave 35/s for +20..25 ms; with 1 guest no saving)
#endif

// Events for the game side: port/mp_core_game.h (MpCoreEvent, mp_core_poll_event and the other calls the game side makes).

extern "C" {
void mp_core_boot(void);
void mp_core_shutdown(void);
bool mp_core_goodbye(unsigned budgetMs);   // merge fix 5 (bounded: at most budgetMs, then gives up); true = a BYE went out or there was no session
void mp_core_set_data_hash(uint32_t hash);
uint32_t mp_core_fingerprint(void);
bool mp_core_poll_event(MpCoreEvent* out);
// The game's current WHERE payload (DESIGN.md: galaxy u8, scenario u8, selected u8, startId s16, zone u8, serial u16; 8 bytes).
// A new host re-sends it after a migration. The core also takes it from every WHERE / MOVE the host sends or a guest receives.
void mp_core_set_where(const void* payload, uint16_t len);
void mp_core_set_race_state(bool finished, uint32_t turns);
// Host: change the session settings (sent to everyone as SETTINGS).
bool mp_core_update_settings(const MpSettings* s);
// Vita power: suspend = clean leave now (BYE SUSPEND); resume = rejoin the same session if it was left by suspend < 2 min ago.
void mp_core_suspend(void);
bool mp_core_resume(void);
// flow: the scene serial this console is in (the host's value is authoritative; guests set it from MOVE / WHERE). Queued STATE
// messages of an older serial are dropped.
void mp_flow_set_scene_serial(uint16_t serial);
// service (weak): pumped once per network tick (network thread while it runs, else the game thread's mp_tick_game).
void mp_service_pump(void) __attribute__((weak));
}

void mp_log_set_sink(void (*sink)(const char* line), bool quiet);   // mp_log.cpp (tests)
class MpCore;
void mp_core_set_loopback(MpLoopHub* hub, uint32_t endpoint);       // tests / mp_bot: the global core on a loopback hub
MpCore* mp_core_global(void);

struct MpCoreMem;

struct MpCoreOptions {
    bool useThread;                          // the helper thread (off in unit tests)
    uint64_t (*now)(void* ctx);              // clock (null = mp_sys_now_ms)
    void* nowCtx;
    bool* activeFlag;                        // the global bool behind mp_active() (null for test instances)
    bool* heldFlag;                          // MP3 lodgate: the global bool behind port_net_session_lockstep() (null for test instances; set after the fields above, so a zeroed struct is fine)
    uint32_t seed;                           // random source for ids / sequence starts (0 = mp_sys_random)
};

// (perf) What the game thread reads every turn (mode, slots, names, settings, ui status), published by whoever holds m_mutex
// when it lets go (double-buffered seqlock, see MpCore::publishView): the bridge's queries never take the lock, never wait for
// the helper thread, and a helper thread preempted while it publishes never holds a reader up (the other buffer is complete).
struct MpCoreView {   // (MP3 profile: keep the per-turn fields first, up to name: see MpCore::readViewWith)
    bool active, originalHost;
    bool reconnecting;                       // fix2: rejoining after a lost link (uiStatus reports MP_UI_RECONNECTING)
    uint8_t role;                            // MpCore::Role
    MpUiPhase phase;
    MpUiError error;
    int8_t localSlot, hostSlot;
    uint16_t epoch, serial;
    MpSettings settings;
    bool present[MP_MAX_PLAYERS];
    uint8_t look[MP_MAX_PLAYERS];
    char name[MP_MAX_PLAYERS][MP_NAME_BYTES + 1];
    bool peerUsed[MP_MAX_PLAYERS];
    int32_t srtt[MP_MAX_PLAYERS];
    uint16_t rosterPing[MP_MAX_PLAYERS];
    uint8_t rosterState[MP_MAX_PLAYERS];
    uint64_t lastRecv[MP_MAX_PLAYERS];
    uint8_t players, progress;
    char roomCode[8];
    char friendCode[MP_MAX_PLAYERS][10];     // core2: the online service's public player code of each slot ("" = none; see slotFriendCode)
    char annCode[MP_MAX_PLAYERS][10];        // core2: a code the player announced itself (rooms without the service; see slotPlayer)
    uint32_t gid[MP_MAX_PLAYERS];            // core2: gathering identity of each slot (MpCore::slotGatherId)
    uint8_t commit[MP_MAX_PLAYERS][16];      // smallfix2: each slot's rejoin-token commitment (BLAKE2b of the token; zero = none); slotPlayer turns it into MpPlayerInfo::sid
};

// (perf) Game-thread unreliable sends (STATE) while the helper thread runs: a single-producer (game thread) / single-consumer
// (whoever pumps, under m_mutex) ring, so the game thread neither takes the lock nor makes a socket call for them.
struct MpUnreliableOut {
    uint8_t type;
    uint16_t len;
    uint32_t queuedUs;   // MP3 nocap: mp_sys_now_us() when queued (only while the SMG_MP_PERFLOG line measures; else 0)
    uint8_t data[MP_DATAGRAM_MAX - MP_HEADER_BYTES];
};
enum { MP_UOUT_SLOTS = 4 };

// The session object. Every public method takes the lock; the bridge calls them on the global instance. Tests make several.
class MpCore {
public:
    explicit MpCore(const MpCoreOptions& o);
    ~MpCore();

    // configuration
    void setLoopback(MpLoopHub* hub, uint32_t endpoint);   // MP_CONN_LOOPBACK uses this hub endpoint
    void setDataHash(uint32_t h);
    uint32_t fingerprint();
    void setProfile(const char* name, MpLook look);
    void setConsoleKey(const uint8_t key[16]);   // corefix 6: mp.cfg's persistent secret (all zero = keep the random one of this run)
    void setConsoleKeyFailed();                  // smallfix: no key in mp.cfg and none could be made: sessions are refused until one can
    void setConsoleId(uint32_t id);              // tests: a console key made from a 32-bit number (the same number = the same console)

    // bridge
    bool active();
    MpMode mode();
    bool isHost();
    int localSlot();
    int hostSlot();
    uint16_t hostEpoch();
    bool slotPresent(int slot);
    void slotName(int slot, char* out);   // MP_NAME_BYTES + 1
    bool slotFriendCode(int slot, char* out, size_t cap);   // core2: see mp_core_slot_friend_code (port/mp_core_game.h)
    bool slotPlayer(int slot, MpPlayerInfo* out);           // core2: see mp_core_slot_player (port/mp_core_game.h)
    void setOwnFriendCode(const char* code);                // core2: this console's account code ("" / null: none); any thread
    // core2: gathering (platform/src/mp/mp_gather.cpp drives these; DESIGN.md 10.6)
    void setGatherId(uint32_t gid);                 // this console's gathering identity (sent in HELLO, in the roster)
    uint32_t slotGatherId(int slot);
    void setGathering(bool on, uint32_t closingInto);   // host: beacon flags GATHERING / CLOSING
    bool gathering();   // netfix B5a: is the GATHERING flag set on this core
    int heardRooms(MpBeacon* out, MpAddr* addrs, uint64_t* lastMs, int max);   // rooms heard (scanning, or as host from beacons)
    bool redirectGuests(uint32_t sessionId);        // host: REDIRECT every guest; false when none was there
    // core2: Play by Address with a code. The room's guests are told where to follow (REDIRECT_CODE); a guest takes it once.
    bool redirectGuestsCode(uint32_t ip, uint16_t port, const uint8_t secret[5]);
    bool takeRedirectCode(uint32_t* ip, uint16_t* port, uint8_t secret[5]);
    bool uiJoinDirect(uint32_t ip, uint16_t port, const uint8_t secret[5]);   // join a Play by Address room with its code's secret
    bool uiHostDirect(const MpHostParams* p, const uint8_t secret[5]);       // host one (p->conn is set to MP_CONN_DIRECT)
    uint32_t directLocalIp();                       // this console's LAN address for the members (a test core: 10.0.<endpoint>)
    bool directAvailable() const;                   // this build / core can play by address
    uint32_t takeRedirect();                        // guest: the room the host redirected us to (once; 0 none)
    uint32_t sessionIdNow();
    int guestCount();
    MpLook slotLook(int slot);
    int slotPing(int slot);
    bool slotQuiet(int slot);
    void settings(MpSettings* out);
    uint16_t sceneSerial();
    bool send(uint8_t type, const void* payload, uint16_t len, bool reliable);
    bool sendTo(int slot, uint8_t type, const void* payload, uint16_t len, bool reliable);
    bool poll(MpMsg* out);
    uint32_t pollMany(MpMsg* out, uint32_t max);   // netopt: up to max messages under one lock
    uint32_t pollStates(MpMsg* out, uint32_t max); // wave8: drain only MP_MSG_STATE before updateNerve() without touching reliable ordering
    void tick(uint32_t turn);
    bool saveSuppressed();
    bool saveAutoconfirm();
    void snapshotOffer(const void* image, uint32_t len);
    uint32_t snapshotReady();
    uint32_t snapshotGet(void* out, uint32_t cap);
    void snapshotConsumed();
    bool uiHost(const MpHostParams* p);
    bool uiJoinCode(MpConn conn, const char* code);
    bool uiJoinFriend(const char* friendCode);
    bool uiQuickMatch(MpMode mode);
    bool uiJoinLocal(MpConn conn, int index);
    bool uiJoinLocalRoom(MpConn conn, uint32_t roomId);   // fix2: by identity (MpLocalRoom::id)
    bool uiJoinAddress(const char* text);                   // netx: internet by address
    bool joinDirectLocked(uint32_t ip, uint16_t port);
    bool uiHostAddress(MpAddressInfo* out);                 // netx: what the host's lobby shows
    bool uiStart();
    void uiLeave();
    void uiKick(int slot);
    void uiStatus(MpUiStatus* out);
    int uiLocalScan(MpConn conn);
    bool uiLocalGet(int index, MpLocalRoom* out);
    // mpwifi44: why the scan's link could not be brought up (uiLocalScan < 0): the error the player should read (MP_ERR_NO_WIFI only when
    // the Wi-Fi itself is not connected; MP_ERR_INTERNAL for a socket / stack failure) and one line for the log. MP_ERR_NONE: no failure.
    MpUiError scanError(char* why, size_t cap);
    // mpwifi44: drop a scan link that failed, so the next uiLocalScan brings it up again (a failure used to stay until the game restarted
    // when no helper thread ran). Nothing happens to a working scan.
    void scanRetry();
    void localName(char* out);   // mpwifi44: this console's name (MP_NAME_BYTES + 1), in a room or not

    // internal API
    bool pollEvent(MpCoreEvent* out);
    void setWhere(const void* payload, uint16_t len);
    void setRaceState(bool finished, uint32_t turns);
    bool updateSettings(const MpSettings* s);
    void suspend();
    bool resume();
    void shutdown();
    bool goodbye(unsigned budgetMs);   // merge fix 5: the app is being closed: BYE to the others, flushed; bounded, no radio / thread teardown
    void setSceneSerial(uint16_t serial);   // flow: this console's scene serial (mp_flow_set_scene_serial)

    // tests
    void pump();                                                        // one pump at the current clock
    void debugReceive(const MpAddr& from, const void* data, size_t len); // the receive path without a transport
    uint16_t debugExpectedSeq(int slot);                                 // next reliable seq this console expects from slot's peer
    uint32_t debugBadCount() { return m_statBad; }
    uint32_t debugPairKeys() { return m_statPairKeys; }                    // corefix D3/D4: X25519 operations a HELLO2 cost this core
    uint32_t debugStray() { return m_statStray; }                          // netpack tests: datagrams refused as stray (bad bundles ...)
    uint32_t debugRecvDropped() { return m_statRecvDropped; }
    int debugSrtt(int slot);                                                // netpack tests: smoothed RTT of a peer (-1 none)
    int debugUnacked(int slot);
    static void debugFailRelAlloc(int n);   // netopt tests: the next n reliable-window allocations fail
    uint32_t debugStateSkipped() const { return m_statStateSkipped; }
    int debugRelWindows();                   // netopt tests: reliable windows held (a host 3, a guest 1, the successor in line 2)
    static size_t debugMemBytes();                                         // netpack tests: sizeof(MpCoreMem) (the per-session block)                                             // netpack tests: reliable messages waiting for an ack
    uint32_t debugConsoleId() { uint32_t v; memcpy(&v, m_consoleKey, 4); return v; }   // a handle of the persistent secret (tests: setConsoleId)
    void debugConsoleKey(uint8_t out[16]) { memcpy(out, m_consoleKey, 16); }          // tests: restart as the same console (setConsoleKey)
    uint32_t debugWireId() { return m_wireId; }                                       // what peers see (HELLO / RESUME): per session
    uint32_t debugGaps() { return m_statGaps; }
    uint32_t debugStale() { return m_statStale; }
    uint32_t debugSessionId() { return m_sessionId; }                    // netcore tests: which session a console is in (soak)
    MpUiPhase debugPhase() { return m_phase; }
    bool debugHeld() const { return heldNow(); }   // MP3 lodgate: tests (the value behind port_net_session_lockstep())
    MpAddr debugLocalAddr();
    void debugWatch(int bits) { m_mutex.lock(); onWatch(bits); publishView(); m_mutex.unlock(); }   // tests: what the Vita watch reports

private:
    friend struct MpCoreAccess;
    friend class CoreLock;        // (perf) mp_core.cpp: the lock that publishes MpCoreView on unlock
    friend class CoreLockAdopt;
    MpCore(const MpCore&);
    MpCore& operator=(const MpCore&);
    // everything below runs under m_mutex
    struct RosterEntry {
        bool present;
        char name[MP_NAME_BYTES + 1];
        uint8_t look, state;
        MpGen gen;
        uint16_t ping;
        MpAddr addr;
        uint32_t consoleId;     // corefix: the id its HELLO / RESUME carried (host side only; 0 = none): a rejoin must match it
        uint32_t gid;           // core2: the console's gathering identity (stable over gathering merges; 0 = none): heads on the menu
        uint8_t pk[32];         // core2: its session public key (X25519) for this membership: every pair key comes from these
        uint8_t commit[16];     // core2: BLAKE2b of its rejoin token: proof for taking the slot back (rejoin), for any host
    };
    struct PairState {          // core2: the sealed channel with one slot (DESIGN.md section 10)
        bool have, confirmed;   // a key; a datagram sealed with it was received (from then on plain datagrams from it are dropped)
        uint8_t key[32];
        uint64_t sendCtr, rxMax, rxMask;
    };
    struct Peer {
        bool used, resumed, closing, sentThisPump;
        MpAddr addr;
        MpReliable* rel;
        uint16_t helloSeq;
        uint64_t lastRecv, lastSend, lastHeartbeat, closeAt, createdAt;
        uint64_t winAt;         // corefix2: when this peer was given its window at a takeover
        uint64_t ackAt;         // corefix: the last time its acknowledgements made progress (or nothing of ours was waiting for one)
        bool established;       // netcore: it has acknowledged something of ours since this peer began (WELCOME / HOST_CHANGE): a real
                                // console, not a ghost joiner; from then on only MP_PEER_LOST_MS removes it (the ack-stall rule is for joiners)
        uint32_t peerTime;
        uint64_t peerTimeAt;
        bool havePeerTime;
        int srtt;
        uint32_t rttMin, rttMax, rttN, rttSum;   // mp3c24: the raw RTT samples of the current 10 s window (logRtt); zero = none
        uint64_t rateStart, badStart;
        uint64_t busyAt;             // starfix: when this peer last sent MP_HDR_BUSY (0 = never)
        uint64_t gateRefusedUntil;   // netfix B2: our own receive gate refused its message: its re-sends are not a flood until then
        uint32_t rateCount, badCount;
        uint32_t snapVersionSent;
        uint8_t snapTries;
        bool rejoin;
        uint32_t consoleId;     // fix2: from its HELLO (0 = none sent)
        uint8_t pk[32], commit[16];   // core2: from its HELLO (the joiner's key and token commitment, until acceptJoin)
    };
    struct Reservation { bool used, hold; char name[MP_NAME_BYTES + 1]; uint8_t slot, look; uint64_t until; uint32_t consoleId; uint8_t commit[16]; };
    struct Kicked { MpAddr addr; uint32_t consoleId; char name[MP_NAME_BYTES + 1]; uint8_t commit[16]; uint8_t pk[32]; };   // fix2 / core2
    enum Role : uint8_t { ROLE_NONE, ROLE_HOST, ROLE_GUEST };

    MpCoreOptions m_opt;
    MpMutex m_mutex;
    MpMutex m_qMutex;          // wave8: dedicated receive-queue lock (strict order: m_mutex -> m_qMutex; never held across socket I/O)
    MpCoreMem* m_mem;
    MpConnector* m_conn;       // session connector
    MpConnector* m_scanConn;   // scanning connector (IDLE)
    MpConn m_scanKind, m_connKind;
    MpLanScan m_scan;
    uint64_t m_scanLastCall;
    bool m_scanFailed;
    MpUiError m_scanError = MP_ERR_NONE;   // mpwifi44: why (scanError)
    char m_scanWhy[96] = {0};
    MpLoopHub* m_hub;
    uint32_t m_hubEndpoint;
    uint32_t m_dataHash, m_fingerprint, m_rng;
    // corefix 6: the persistent secret never leaves the console; peers see m_wireId = a keyed hash of it and the session id (known at
    // HELLO time for Same Wi-Fi / Nearby joins and rejoins), else a per-run random: a kick bans that id, a rejoin of the same session
    // gives the same id, and no id links two sessions.
    uint8_t m_consoleKey[16];
    bool m_consoleKeyBad = false;   // smallfix: no persisted key and none could be made: no session (randomOk)
    uint32_t m_runId;          // per run random (joins where the session id is not known before the HELLO)
    uint32_t m_wireId;         // the id of the session this console is in / joins / hosts
    uint32_t m_rejoinWireId;
    void useWireId(uint32_t sessionId);

    // session
    Role m_role;
    MpUiPhase m_phase;
    MpUiError m_error;
    uint64_t m_phaseAt;
    bool m_active, m_started, m_originalHost;
    MpSettings m_settings;
    uint8_t m_visibility;
    uint32_t m_sessionId, m_wantSession;
    int m_localSlot, m_hostSlot;
    uint16_t m_epoch;
    uint16_t m_serial;
    // group-restart world sync (protocol-5 wire compatible):
    //   m_restartGateMask: host-side per-slot FIFO gate blocking pre-restart KILL/PURPLE/CHIP of m_restartSerial until post-restart REGCRC
    //   m_restartMissedMask: slots that were absent/held or dropped before completing post-restart REGCRC (sent RESTART on rejoin)
    uint16_t m_restartSerial = 0;
    uint8_t m_restartGateMask = 0;
    uint8_t m_restartMissedMask = 0;
    uint64_t m_restartGateUntil[MP_MAX_PLAYERS] = {};
    void clearRestartGate() {
        m_restartSerial = 0;
        m_restartGateMask = 0;
        m_restartMissedMask = 0;
        memset(m_restartGateUntil, 0, sizeof m_restartGateUntil);
    }
    RosterEntry m_roster[MP_MAX_PLAYERS];
    Peer m_peers[MP_MAX_PLAYERS];
    MpAddr m_hostAddr;           // guest: where the host is (host: own address)
    uint8_t m_wantMode;
    char m_joinCode[16];
    char m_roomCode[8];
    // migration
    bool m_hostLeaving;          // guest: the host said BYE(HOST_LEAVING) / SUSPEND
    uint64_t m_hostGoneAt;       // when the host went silent or said goodbye
    bool m_claiming;             // this console claimed the host role and waits for a RESUME
    uint64_t m_claimAt;
    uint8_t m_lastOldHost;
    bool m_rosterDirty;
    uint64_t m_lastRosterSent;
    // per-origin tracking
    uint16_t m_seen[MP_MAX_PLAYERS];     // last origin sequence delivered per slot (of the membership m_seenGen)
    // netcore: an origin's event stream is (slot, gen): gen = the roster generation of the membership that sent it (a slot is reused
    // by every rejoin). Dedupe, catch-up and re-sends compare (gen, seq), never seq alone. 0 = nothing of that slot yet.
    MpGen m_seenGen[MP_MAX_PLAYERS];
    MpGen m_genFloor[MP_MAX_PLAYERS];  // the newest generation of each slot this console has heard of (a join takes a newer one)
    MpGen m_myGen;
    uint32_t m_gid;                      // core2: gathering identity of this console (0 = not gathering)
    // core2: session crypto (DESIGN.md section 10)
    uint8_t m_sk[32], m_pk[32];          // this membership's key pair (new at every host / join)
    bool m_haveKp;
    uint8_t m_psk[32];                   // the room's pre-shared secret (Play by Address code; zeros elsewhere)
    uint8_t m_roomKey[32];               // MACs beacons (members only); from the first host, in WELCOME
    uint8_t m_cookieSecret[32];          // host: stateless join cookies
    uint8_t m_token[16];                 // this console's rejoin token for the session (its commitment is in the roster)
    bool m_haveToken;
    uint32_t m_tokenSession;             // the session the token belongs to (a rejoin of it keeps the token)
    PairState m_pair[MP_MAX_PLAYERS];
    int m_authSlot;                      // the slot the datagram being handled was sealed by (-1: not sealed)
    bool m_authBad;                      // (bad() counts only for authenticated traffic)
    // joining
    uint8_t m_nonce[8], m_hostPk[32], m_cookie[16];
    bool m_haveCookie;
    uint64_t m_hello1At;
    uint64_t m_proofWindow;
    int m_proofFails;   // all sources together, this second
    struct ProofSrc { uint32_t host; uint64_t window; int fails; uint64_t badUntil; } m_proofSrc[16];   // corefix2: 16 sources; badUntil = suspect (a proof failed)   // corefix D3: failures per source address
    struct Departed { bool used; uint8_t slot; MpAddr addr; uint8_t key[32]; uint64_t until, lastBye; } m_departed[MP_MAX_PLAYERS];
    uint64_t m_departCtr = 0;
    uint64_t m_ctrBase = 0;
    MpAddr m_connectTarget = {0, 0};
    int m_connectGuess = 0;
    bool m_pskAnyConn = false;
    bool pskOn() const { return m_connKind == MP_CONN_DIRECT || m_pskAnyConn; }   // core2: this membership's first sealed counter - 1 (CtrBase)
    bool makeKeys(uint32_t sid);   // smallfix: false = no secure random for a random key pair (nothing kept)
    uint8_t ctrFloorFor(const uint8_t* pk) const;
    void makeToken(uint32_t sid);
    void registerLive();
    void noteDeparted(int slot);
    bool keyFor(int slot);
    void pairReset(int slot);
    void sendHello1(uint64_t t);
    void handleHello1(const MpAddr& from, const uint8_t* hdr, const uint8_t* payload, uint16_t plen, uint64_t t);
    void handleCookie(const MpAddr& from, const uint8_t* payload, uint16_t plen, uint64_t t);
    void cookieFor(const MpAddr& from, const uint8_t* pk, const uint8_t* nonce, uint64_t bucket, uint8_t out[16]);
    int handleSealed(const MpAddr& from, const uint8_t* data, size_t len, uint64_t t);
    bool plainAllowed(const MpAddr& from, const uint8_t* data, size_t len) const;
    void sendRejectNonce(const MpAddr& to, uint8_t reason, uint16_t seq, const uint8_t* nonce);
public:
    void setRoomSecret(const uint8_t* secret, size_t len);
    void debugRoomSecretAnyConn(bool on) { m_pskAnyConn = on; }   // tests: the code secret on a loopback session too (as Play by Address)   // core2: Play by Address: the code's secret (both sides) before host / join
    void debugReceiveAuthed(int slot, const void* data, size_t len);   // tests / fuzz: as if sealed by slot (the parsers behind the seal)
    // tests (single-threaded loop hub only; no lock): open a sealed datagram with any pair key of any live core (taps and filters read
    // what is inside). Never called by the game.
    static bool debugOpenAny(const void* data, size_t len, uint8_t* out, size_t cap, size_t* outLen);
    int debugSlotOf(const MpAddr& a) {   // tests: the peer slot at an address (-1: none)
        for (int i = 0; i < MP_MAX_PLAYERS; i++)
            if (m_peers[i].used && m_peers[i].addr == a) return i;
        return -1;
    }
    void debugDeliverCore(int slot, uint8_t type, uint8_t about, const char* code, size_t n = 9);   // tests: a core message, authenticated
    size_t debugSealAs(int toSlot, const void* data, size_t len, uint8_t* out, size_t cap);   // tests: this console's datagram, sealed for toSlot
    bool debugSealed(int slot) { return slot >= 0 && slot < MP_MAX_PLAYERS && m_pair[slot].confirmed; }
private:
    bool m_gathering;                    // core2: host beacons GATHERING
    uint32_t m_closingInto;              // core2: host beacons CLOSING into this room
    uint32_t m_redirectTo;               // core2: guest: REDIRECT received
    struct { bool have; uint32_t ip; uint16_t port; uint8_t secret[5]; } m_redirectCode;   // core2: guest: REDIRECT_CODE received
    bool m_pskArmed = false;             // core2: setRoomSecret was called for the next session (else a new by-address session has none)
    char m_friendCode[MP_MAX_PLAYERS][10];   // core2: refreshFriendCodes (online rooms only)
    char m_label[MP_MAX_PLAYERS][MP_NAME_BYTES + 1];   // svcfix3: the service's label of a shared name ("Luma#4DXT"), "" = show the roster's name; refreshFriendCodes
    void refreshFriendCodes();                     // own membership generation (from the WELCOME / hosting; never from a later ROSTER)
    // core2: self-announced friend codes (rooms without the service): MP_MSG_FRIEND, sealed, reliable, never logged, never in beacons
    char m_annCode[MP_MAX_PLAYERS][10];
    MpGen m_annGen[MP_MAX_PLAYERS];
    uint64_t m_annAt[MP_MAX_PLAYERS];
    char m_ownCode[10];                  // (under m_mutex: what this session announces)
    MpMutex m_ownCodeMu;                 // (the service thread hands its code over here, never under the core's lock)
    char m_ownCodeIn[10];
    bool m_ownCodeNew = false;
    void pickOwnCode();
    bool announcesCodes() const { return m_connKind != MP_CONN_ONLINE; }
    void sendFriendCode(int to, int about, const char* code);
    void announceOwnCode();
    void sendKnownCodesTo(int slot);
    uint32_t m_statStale;                // events of a slot's earlier membership dropped
    MpChatBucket m_chatBucket[MP_MAX_PLAYERS];   // MP3 chat: the host's relay rate limit per origin (port/mp_chat.h)
    uint32_t m_chatDrops;                // MP3 chat: CHATs the host did not relay (log throttle)
    uint16_t m_myOrigin;                 // own origin sequence
    uint16_t m_uSeq;                     // own unreliable counter
    uint16_t m_lastU[MP_MAX_PLAYERS][64];
    uint64_t m_haveU[MP_MAX_PLAYERS];
    uint64_t m_lastUAt[MP_MAX_PLAYERS];  // core2: when an unreliable message of the origin was last accepted (the sequence window)
    Reservation m_res[MP_MAX_PLAYERS];
    Kicked m_kicked[8];
    // corefix 3: who may vouch for a merge beacon: addresses of removed members (a member that migrated, was cut off and came back)
    struct PastAddr { MpAddr addr; uint64_t until; };
    PastAddr m_past[8];
    int m_pastNext;
    MpAddr m_mergeAddr;                  // the room whose beacons are being counted
    uint64_t m_mergeFirst, m_mergeLast;
    uint32_t m_mergeSeq;
    int m_mergeCount;
    uint8_t m_ghostMask;                 // corefix 4: slots of dead hosts whose events a guest may re-send to the new host (until the next scene serial)
    void dropGhostHistory();
    void rememberAddr(const MpAddr& a, uint64_t t);
    bool vouchedAddr(const MpAddr& a, uint64_t t) const;
    int m_kickedCount, m_kickedNext;   // corefix 9: the kick list is a ring of 8: the oldest entry is the one forgotten
    uint8_t m_where[8];
    bool m_haveWhere;
    bool m_raceFinished;
    uint32_t m_raceTurns;
    // beacons
    uint64_t m_lastBeacon;
    uint64_t m_spareRetryAt = 0, m_spareDropAt = 0;
    uint16_t m_stateSerial[MP_MAX_PLAYERS] = {};     // netopt item 8: the scene serial of each guest's last pose (host only)
    uint64_t m_stateSerialAt[MP_MAX_PLAYERS] = {};   // ... and when it came (0 = none)
    uint32_t m_statStateSkipped = 0;                  // ... poses not sent because of it
    void noteStateSerial(int slot, uint16_t serial);
    bool stateWanted(int slot, uint16_t serial);   // netopt: tendSpareWindows
    uint64_t m_lastRecvPump, m_rateGraceUntil;   // a stalled pump drains a backlog: not a flood
    uint32_t m_beaconSeq;
    uint64_t m_lastProbe, m_probeWindow;   // netx: active discovery (scanner: last probe sent; host: answers this second)
    uint32_t m_probeAnswers;
    // profile
    char m_name[MP_NAME_BYTES + 1];
    uint8_t m_look;
    // game-thread caches
    char m_nameCache[MP_MAX_PLAYERS][MP_NAME_BYTES + 1];
    // snapshot (guest side state)
    bool m_snapReady;      // a complete image not yet consumed by the game
    int m_snapProgress;
    // events
    MpCoreEvent m_events[MP_EVENT_QUEUE];
    int m_evHead, m_evCount;
    // resume after suspend
    bool m_rejoinValid;
    uint64_t m_rejoinAt;
    MpConn m_rejoinConn;
    uint32_t m_rejoinSession;
    uint32_t m_rejoinPeers[MP_MAX_PLAYERS + 8];   // corefix 10: the addresses of the room's members when we left: a beacon of the session from elsewhere is believed only from one of them
    void saveRejoinPeers();
    char m_rejoinCode[16];
    MpAddr m_rejoinAddr;
    uint32_t m_directIp;         // netx: by address (guest): the host's address, kept for the reconnect path
    uint16_t m_directPort;
    // thread
    MpThread m_thread;
    bool m_threadAlive, m_threadExiting, m_threadStop;
    uint64_t m_idleSince;
    uint32_t m_turn;
    // (perf) lock-free state for the game thread: see MpCoreView / MpUnreliableOut above
    MpCoreView m_views[2];
    uint32_t m_viewSeq[2];
    uint32_t m_viewIdx;                      // the complete buffer readers take
    uint32_t m_qPub;                         // queued game messages as of the last unlock (poll's empty check)
    uint32_t m_evPub;                        // queued core events as of the last unlock (pollEvent's empty check)
    bool m_threadRunsPub;                    // the helper thread pumps (the game thread does no network work then)
    MpUnreliableOut m_uOut[MP_UOUT_SLOTS];
    // MP3 netpack: per-peer outgoing datagram under construction (queueOut / flushOut)
    struct OutBuf {
        MpAddr to;
        uint16_t len;
        uint8_t count;
        bool urgent;
        bool hasHello;   // core2: holds a HELLO (sent plain: the host cannot know the key before it)
        uint64_t firstAt;
        uint8_t data[MP_DATAGRAM_MAX + 8];
    };
    OutBuf m_ob[MP_MAX_PLAYERS];
    uint32_t m_obPending;
    uint32_t m_statDatagramsOut, m_statMsgsOut, m_statRecvDropped;
    void noteLinkIn(const MpAddr& from, size_t n);   // MP3 netfeel: per-link receive counters (mp_core_link_counts)
    uint32_t m_uHead, m_uTail;               // consumer (pump, locked) / producer (game thread)
    uint32_t m_uDropped;
    // netlog: the link's counters for the "[mp] net: link" line (pump thread, under the lock; m_uDropped is the game thread's)
    struct NetLinkStat {
        uint64_t winAt;                 // window start (ms; 0 = not started)
        uint32_t txOk, txFail, rx, txBytes, rxBytes;      // datagrams the transport took / refused with an error; datagrams received
        uint32_t rxGapMax;              // longest time between two received datagrams (ms)
        uint64_t lastRx;                // ms of the last received datagram (0 = none yet)
        uint32_t uDropped0;             // m_uDropped at the window's start
    } m_nl;
    int txSend(const MpAddr& to, const void* data, size_t len);   // session()->send, counted
    void logNetLink(uint64_t t);
    void logRtt(uint64_t t);   // mp3c24: "[mp] rtt: ..." every 10 s while a session link is up (log only)
    // beta diagnostics (port/mp_diag.h): session totals for the summary (pump thread, under the lock)
    bool m_dgOn = false;
    uint32_t m_dgRx = 0, m_dgTxFail = 0;
    void diagTick();
    void diagEnd(MpUiError err, const char* why);
    // stats
    uint32_t m_statBad, m_statGaps, m_statStray, m_statPairKeys;
    uint32_t m_h1Tokens, m_h1KnownTokens;   // corefix2: cookie answers (milli-answers), unknown / known sources
    uint64_t m_h1At;
    uint32_t m_pkTokens;    // corefix D4: token bucket of X25519 operations (milli-ops)
    uint64_t m_pkAt;
    uint32_t m_snapPub;     // corefix S3: the image length of a ready snapshot (0 = none), published for the game thread's lock-free read
    uint64_t m_lastStatusLog;
    uint64_t m_lastRttLog = 0;
    // ending (deferred to the end of the pump: a delivery callback must not free the channel it runs in)
    bool m_dying;
    MpUiError m_dieError;
    char m_dieWhy[48] = {0};   // beta diagnostics: why the session ended (the text requestEnd was given)
    uint64_t m_rejectWindow;
    uint32_t m_rejectCount;
    bool m_lookupActive;         // a service room lookup (quick match / friend) runs
    // reconnect (link lost / Wi-Fi gone / resumed from sleep): rejoin the same session for up to MP_RECONNECT_MS
    bool m_dieReconnect, m_reconnect;
    bool m_rejoining;            // MP3 lodgate: a rejoin of the session we left (a link loss, a sleep) is being tried; cleared when it works, ends or the player leaves
    uint64_t m_reconnectStart, m_reconnectNext;
    int m_reconnectTries;
    uint32_t m_dieWindow, m_reconnectWindow;   // corefix D1: the give-up time of this reconnect (MP_RECONNECT_MS or MP_LINKLOST_RECONNECT_MS)
    MpUiError m_reconnectErr;    // fix3: the last failure of a rejoin attempt (shown if the 30 s run out; NO_WIFI when none)
    bool m_rejoinProbing = false;
    uint64_t m_rejoinProbeStart = 0, m_rejoinProbeAt = 0;
    uint64_t m_rejoinJoinAt = 0;
    bool m_rejoinDirectTried = false;
    bool m_rejoinDirectActive = false;
    MpMutex m_svcMutex;          // mp_service_pump runs on one thread at a time

    bool heldNow() const;                    // MP3 lodgate: the session-held rule (see mp_gate.h SessionHeld) on this core's state
    void publishView();                      // under m_mutex (CoreLock does it on every unlock)
    void readView(MpCoreView* out) const;    // any thread, no lock
    // MP3 profile: the same lock-free read for the per-turn getters, but only the first `bytes` of the view (word by word, relaxed
    // atomics as readView), not all of it (~300 bytes) on every mp_local_slot / mp_slot_present / mp_settings call (~100 a turn on
    // the game thread). The scalars, settings, present[] and look[] come first: kViewHead bytes; names: kViewNames.
    template <class F>
    auto readViewWith(size_t bytes, F f) const -> decltype(f(m_views[0])) {
        MpCoreView v;
        for (;;) {
            const uint32_t i = __atomic_load_n(&m_viewIdx, __ATOMIC_ACQUIRE);
            const uint32_t s0 = __atomic_load_n(&m_viewSeq[i], __ATOMIC_ACQUIRE);
            if (s0 & 1) continue;
            loadViewPrefix(&v, &m_views[i], bytes);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&m_viewSeq[i], __ATOMIC_RELAXED) == s0) return f(v);
        }
    }
    static void loadViewPrefix(MpCoreView* dst, const MpCoreView* src, size_t bytes);
    bool sendUnreliableLocked(uint8_t type, const uint8_t* data, uint16_t len);
    void drainUnreliableOut();               // under m_mutex
    static void threadMain(void* self);
    void ensureThread();
    uint64_t now();
    uint32_t rnd();
    void setActive(bool on);
    void setPhase(MpUiPhase p);
    void computeFingerprint();
    MpReliable* relFor(int slot, bool canFail = false);   // netopt: canFail: null when no window is free and none can be got (else: the oldest peer goes)
    MpReliable* relAt(int i);
    bool allocRel(int n);
    void tendSpareWindows(uint64_t t);
    // corefix2: takeover window service (a takeover never fails for lack of a window: see becomeHost)
    uint8_t m_starved = 0, m_starveStale = 0;   // slots present in the roster that hold no window yet / of those, the ones not heard before the host was lost
    uint64_t m_takeoverAt = 0, m_starveNext = 0;
    void startClaimPeer(int s, MpReliable* rel, uint64_t t);
    bool grantWindow(int s, uint64_t t);
    void tendStarved(uint64_t t);
    bool allocMem(bool host = false);
    void freeMem();
    void requestEnd(MpUiError err, const char* why, bool reconnect = false, uint32_t window = 0);
    bool randomOk();   // smallfix: the secure random source works (3 tries), else m_error = MP_ERR_INTERNAL and the start is refused
    bool startRejoinLocked();
    bool joinLocalLocked(MpConn conn, int index);
    void onWatch(int bits);
    void purgeOldStates();
    void finishEnd();
    void clearSession();
    MpConnector* makeConnector(MpConn conn, const MpConnectorParams& p);
    bool startHostingLocked(MpConn conn, const MpSettings& s, uint8_t visibility);
    bool startJoinCodeLocked(const char* code);
    void onWelcome(const uint8_t* data, uint16_t len);
    void onResume(int slot, const uint8_t* data, uint16_t len);
    void queueRemoveAt(int pos);
    void pumpLocked(uint64_t t);
    void pumpConnector(uint64_t t);
    void pumpScan(uint64_t t);
    void pumpTimers(uint64_t t);
    void pumpMigration(uint64_t t);
    void receiveAll(uint64_t t);
    int handleDatagram(const MpAddr& from, const uint8_t* data, size_t len, uint64_t t);
    void handleFromPeer(int slot, const uint8_t* hdr, const uint8_t* payload, uint16_t plen, uint64_t t);
    void handleHelloPacket(const MpAddr& from, const uint8_t* hdr, const uint8_t* payload, uint16_t plen, uint64_t t);
    void handleHostChangePacket(const MpAddr& from, const uint8_t* hdr, const uint8_t* payload, uint16_t plen, uint64_t t);
    static void deliverThunk(void* ctx, const MpRelMsg& m);
    static bool gateThunk(void* ctx, const MpRelMsg& m);   // core2: back-pressure (MpReliable::setGate)
    bool roomFor(int slot, const MpRelMsg& m);
    void deliver(int slot, uint8_t type, uint8_t from, const uint8_t* data, uint16_t len, bool reliable, uint16_t hopSeq);
    void handleCore(int slot, uint8_t type, uint8_t from, const uint8_t* data, uint16_t len);
    void handleGame(int slot, uint8_t type, uint8_t origin, const uint8_t* data, uint16_t len, bool reliable, uint16_t useq, MpGen gen);
    bool rawSend(const MpAddr& to, uint8_t type, uint8_t from, uint8_t flags, uint16_t seq, uint16_t ack, uint16_t bits,
                 const void* payload, uint16_t len);
    void flushOut(int slot);
    int guestLinks() const;   // netcore: peers this host serves (the pose hold needs two or more)
    void flushOutAll();
    void flushOutDue(uint64_t t);
    bool queueOut(int slot, const MpAddr& to, uint8_t type, uint8_t from, uint8_t flags, uint16_t seq, uint16_t ack, uint16_t bits,
                  const void* payload, uint16_t len);
    void handleDatagramOne(const MpAddr& from, const uint8_t* data, size_t len, uint64_t t);
    bool sendToPeer(int slot, uint8_t type, uint8_t from, uint8_t flags, uint16_t seq, const void* payload, uint16_t len);
    void queueHeartbeat(int slot, uint64_t t);   // netopt: the HEARTBEAT (time + echo for the ping); stamps lastHeartbeat
    bool queueReliable(int slot, uint8_t type, uint8_t from, const void* payload, uint16_t len);
    bool queueGameReliable(int slot, uint8_t type, uint8_t origin, uint16_t originSeq, MpGen gen, const void* payload, uint16_t len);
    void sendRejectRaw(const MpAddr& to, uint8_t reason, uint16_t seq);
    void flushPeers(uint64_t t);
    void bad(int slot, const char* why);
    void applyBans(const Kicked* bans, int n);   // corefix2
    void banSlot(int slot);   // corefix D2: the session's ban list (uiKick and the bad-message kick)
public:
    void reportBad(int slot, const char* why);   // core2: the game side's verdict on a message from slot (mp_core_report_bad)
private:
    bool chatRelayOk(int slot, uint8_t origin, const uint8_t* data, uint16_t len, bool reliable);   // MP3 chat (host)
    int enqueueGame(uint8_t type, uint8_t from, const uint8_t* data, uint16_t len, bool reliable, bool tracked);   // 0 ok, 1 full, 2 origin over budget
    void purgeSlot(int slot);
    void pushEvent(uint8_t kind, int slot, int other, uint32_t arg);
    // host
    bool slotHeld(int s, uint64_t t) const { return m_res[s].used && m_res[s].hold && t <= m_res[s].until; }   // corefix2: a link-lost player's slot, kept for its owner only
    int evictableGhost(uint64_t t) const;   // corefix D4: the joiner a full room lets go (-1: none)
    bool helloWouldBeRefused(const uint8_t* commit, uint64_t t, uint8_t* reason) const;   // corefix D4: full / closed, decided without crypto
    int freeSlotFor(const char* name, const uint8_t* commit, const MpAddr& addr, uint64_t t, bool* rejoin);
    void uniqueName(char* name, int exceptSlot) const;   // corefix 7: "Player" -> "Player 2" when another present player has the name
    void acceptJoin(int slot, const MpAddr& from, uint16_t helloSeq, const char* name, uint8_t look, bool rejoin, uint64_t t);
    void removeSlot(int slot, uint8_t reason, const char* why);
    void sendWelcome(int slot);
    void sendRosterAll();
    uint16_t writeRoster(uint8_t* out, uint16_t cap);
    void sendSnapshotTo(int slot);
    bool sendCatchUp(int slot, const uint16_t* seen, const MpGen* gens);   // false: the history cannot serve it (resync)
    void becomeHost(uint64_t t);
    void sendBeacon(uint64_t t);
    size_t buildBeacon(uint8_t* buf, size_t cap);
    void answerProbe(const MpAddr& from, uint64_t t);   // netx: a scanner's probe: our beacon straight to it
    void checkMerge(uint64_t t);          // host: a room of the same session that is ahead of this one: leave this room and join it
    // guest
    void startConnecting(const MpAddr& host, int hostSlotGuess, uint64_t t);
    bool readRoster(MpReader& r, RosterEntry* out, Kicked* bans = 0, int* nBans = 0);   // corefix2: the roster carries the ban list as a trailer
    void applyRoster(const RosterEntry* r, bool fromWelcome);
    void followHost(int slot, const MpAddr& addr, uint16_t epoch, const uint16_t* seen, const MpGen* gens, uint16_t hcSeq, uint64_t t);
    // history
    void historyAdd(uint8_t origin, uint16_t seq, MpGen gen, uint8_t type, const uint8_t* data, uint16_t len);
    bool historyHas(uint8_t origin, MpGen gen, uint16_t seq) const;
    void park(int from, uint8_t origin, MpGen gen, uint16_t seq, uint8_t type, const uint8_t* data, uint16_t len);
    void unpark(uint8_t origin);
    uint16_t seenOf(int slot) const;
    MpGen genOf(int slot) const;
    void noteGen(int slot, MpGen gen);   // m_genFloor
    void snoopWhere(uint8_t type, const uint8_t* data, uint16_t len);
    int presentCount() const;
    bool lookFree(uint8_t look, int exceptSlot) const;
    uint8_t assignLook(uint8_t wish, uint8_t keep, int slot) const;
    void handleLookRequest(int slot, uint8_t look);
    void logStatus(uint64_t t);
};

#endif  // PORT_MP_CORE_H
