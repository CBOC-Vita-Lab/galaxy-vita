// Galaxy 3.0 multiplayer: wire protocol contract (message ids, limits, payload layouts, serialisation helpers).
// Shared by the port side (platform/src/mp) and the game side (decomp/src/Game/MP). Plain C++ types only, no game headers.
// See DESIGN.md section 3. Changing an id or a layout means bumping MP_PROTO_VERSION.
//
// Everything on the wire is little-endian and written field by field with MpWriter / read with MpReader. Never memcpy a struct
// to the wire. Every reader call is bounds-checked: a short or malformed message makes the reader fail() and the whole message is
// dropped by the caller.
#ifndef PORT_MP_PROTO_H
#define PORT_MP_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MP_PROTO_VERSION 5         // 5: racefix (RACEREADY 0x47); starfix (STAR 12 bytes with galaxy hash + scenario, STARV 0x3F host verdicts); 4: worldsync (OBJMAP 0x46, REGCRC repair flag byte); 3: datagram bundles ('GB', netpack); 2: MpSettings.downed, BUBBLE / RESTART, MP_POSE_BUBBLE (bubble agent); CHAT (chat) is part of the 3.0 wire (fix3: v2 = 3.0)
#define MP_MAX_PLAYERS 4
#define MP_NAME_BYTES 16          // UTF-8, NUL padded, not necessarily NUL terminated on the wire
#define MP_MAX_PAYLOAD 1024       // largest message payload (after the session header)
#define MP_SNAPSHOT_MAX 0x2000    // host user-file blocks: mario (0xF80) + luigi (0xF80) + config (0x60), rounded up
#define MP_SNAPSHOT_CHUNK 1000

enum MpMode : uint8_t { MP_MODE_NONE = 0, MP_MODE_TOGETHER = 1, MP_MODE_RACE = 2 };
enum MpConn : uint8_t { MP_CONN_NONE = 0, MP_CONN_ONLINE = 1, MP_CONN_WIFI = 2, MP_CONN_NEARBY = 3, MP_CONN_LOOPBACK = 4,
                        MP_CONN_DIRECT = 5 /* netx: internet by address (UDP straight to the host, no service) */ };

// Characters (owner decisions 8 Nov 11:15 and 11:39): six unique characters for up to four players, one each, chosen on the
// character select screen. Every console plays with Mario's physics; a non-Mario character is drawn by a puppet model on every
// console INCLUDING the player's own (the local MarioActor body is hidden and a "self puppet" follows the local pose).
//   MARIO        : the local MarioActor as is.
//   LUIGI        : Luigi model + MarioAnime with the Luigi swap table (full move set).
//   TOAD         : Kinopio model (red Toad), Mario's state mapped onto Toad's animations (wait/walk/run/spin/swimwait/...).
//   COSMIC       : Cosmic Mario (GhostMario model with its btk/brk shimmer, full move set; solid, not the race ghost alpha).
//   YELLOW_TOAD  : the same Kinopio model and retarget, yellow (colorchange.brk yellow frame).
//   COSMIC_LUIGI : Cosmic Luigi (GhostLuigi model with its shimmer, Luigi swap table, full move set).
// Uniqueness: the host arbitrates (HELLO asks, WELCOME/ROSTER assign; LOOK asks to change, the host accepts if free).
enum MpLook : uint8_t { MP_LOOK_MARIO = 0, MP_LOOK_LUIGI = 1, MP_LOOK_TOAD = 2, MP_LOOK_COSMIC = 3, MP_LOOK_YELLOW_TOAD = 4,
                        MP_LOOK_COSMIC_LUIGI = 5, MP_LOOK_COUNT = 6 };

// Player forms (MarioActor::mPlayerMode / mCurrModel mapped to a stable wire value).
enum MpForm : uint8_t {
    MP_FORM_NORMAL = 0, MP_FORM_FIRE, MP_FORM_ICE, MP_FORM_BEE, MP_FORM_BOO, MP_FORM_SPRING, MP_FORM_RAINBOW, MP_FORM_FLYING,
    MP_FORM_BONE, MP_FORM_COUNT
};

enum MpMsgType : uint8_t {
    // core (port side only; never delivered to the game)
    MP_MSG_HELLO = 0x01, MP_MSG_WELCOME = 0x02, MP_MSG_REJECT = 0x03, MP_MSG_ROSTER = 0x04, MP_MSG_BYE = 0x05,
    MP_MSG_HEARTBEAT = 0x06, MP_MSG_SETTINGS = 0x07,
    MP_MSG_HOST_CHANGE = 0x08,   // new host announces itself: epoch u16, new host slot u8, scene serial u16 (core; then the new host
                                 // re-sends WHERE, SWITCHMAP, race state to everyone)
    MP_MSG_RESUME = 0x09,        // guest -> new host after a migration: epoch u16, scene serial u16, (race) finished u8, turns u32
    MP_MSG_COOKIE = 0x0B,        // core2 (host -> joiner, unauthenticated, never larger than the HELLO it answers): nonce[8], host
                                 // key[32], cookie[16], session u32, host slot u8, flags u8 (bit0: the room needs the code's secret)
    MP_MSG_REDIRECT = 0x0A,      // core2 (host -> guest, reliable): this gathering room closes into the room with session id u32: go there
    MP_MSG_REDIRECT_CODE = 0x0C, // core2 (host -> guest, reliable, sealed): this gathering room becomes a Play by Address room: its LAN
                                 // address u32, UDP port u16, code secret[5]: join it there
    MP_MSG_FRIEND = 0x0D,        // core2 (any -> host -> everyone, reliable, sealed): a console's own friend code, self-announced:
                                 // about slot u8, code[9] ("XXXX-XXXX"; all zero = none). Never in beacons, never before the keys
    // save snapshot (port side assembles; the game side reads the finished image with mp_snapshot_get)
    MP_MSG_SNAPSHOT_BEGIN = 0x10, MP_MSG_SNAPSHOT_CHUNK = 0x11, MP_MSG_SNAPSHOT_DONE = 0x12,
    // game messages (delivered to the game side through mp_poll); 0x20..0x5F
    MP_MSG_STATE = 0x20, MP_MSG_LOOK = 0x21, MP_MSG_EMOTE = 0x22, MP_MSG_BUMP = 0x23,
    MP_MSG_POINTER = 0x24,       // any (host relays), UNRELIABLE, <= 15 Hz while held: the Star Pointer ping (MpPointerMsg; coop)
    MP_MSG_WHERE = 0x30, MP_MSG_MOVE = 0x31, MP_MSG_LOADED = 0x32, MP_MSG_GO = 0x33, MP_MSG_STAR = 0x34, MP_MSG_FINISH = 0x35,
    MP_MSG_RESULTS = 0x36,   /* host only: also carries the series and the host's next-step choice (race; MpSeriesLogic) */
    MP_MSG_SKIPVOTE = 0x37, MP_MSG_DEAD = 0x38, MP_MSG_WARPREQ = 0x39, MP_MSG_HOLD = 0x3A,
    MP_MSG_BUBBLE = 0x3B,        // any (host relays): serial u16, on u8, lives u8, by u8, flags u8 (Play Together "Bubbles" falls)
    MP_MSG_RESTART = 0x3C,       // host only: serial u16 (everyone is in a bubble: each console runs its own miss reload)
    MP_MSG_TALLY = 0x3D,         // any (host relays): serial u16, star bits u16, coins u16, helped u8, near u8 (the team star card; 0x3B/0x3C: bubble)
    MP_MSG_STARV = 0x3F,         // starfix (host only, reliable): the host's verdict on a star claim, sent for every verdict (MpFlowLogic Starv, 11 bytes); guests credit only on it
    MP_MSG_CHAT = 0x3E,          // any (host relays, host checks): a preset phrase id or a short typed text (port/mp_chat.h; chat)
    MP_MSG_SWITCH = 0x40, MP_MSG_SWITCHMAP = 0x41, MP_MSG_KILL = 0x42, MP_MSG_PURPLE = 0x43, MP_MSG_CHIP = 0x44, MP_MSG_REGCRC = 0x45,
    MP_MSG_OBJMAP = 0x46,        // worldsync (host only, reliable): the scene's kill / purple coin / chip state for a joiner or a repair (MpWorldLogic ObjMap; max 138 bytes)
    MP_MSG_RACEREADY = 0x47,     // racefix (guest -> host only, reliable, not relayed): serial u16: this console's race scene is in Action (the host sends GO when all are ready)
   
    MP_MSG_TOAST = 0x50, MP_MSG_STAMP = 0x51,
    MP_MSG_GAME_FIRST = 0x20, MP_MSG_GAME_LAST = 0x5F,
};

// Which messages the host relays to the other guests when a guest sends them (star topology). Unlisted: host only.
static inline bool mp_msg_host_relays(uint8_t t) {
    switch (t) {
    case MP_MSG_STATE: case MP_MSG_LOOK: case MP_MSG_EMOTE: case MP_MSG_BUMP: case MP_MSG_STAR: case MP_MSG_FINISH:
    case MP_MSG_DEAD: case MP_MSG_SWITCH: case MP_MSG_KILL: case MP_MSG_PURPLE: case MP_MSG_CHIP: case MP_MSG_STAMP:
    case MP_MSG_SKIPVOTE: case MP_MSG_LOADED: case MP_MSG_TALLY: case MP_MSG_POINTER: case MP_MSG_BUBBLE:
    case MP_MSG_CHAT:
        return true;
    default:
        return false;
    }
}
// Messages only the host may originate (a guest sending one is a protocol violation: drop and count).
static inline bool mp_msg_host_only(uint8_t t) {
    switch (t) {
    case MP_MSG_WELCOME: case MP_MSG_REJECT: case MP_MSG_ROSTER: case MP_MSG_SETTINGS: case MP_MSG_SNAPSHOT_BEGIN:
    case MP_MSG_SNAPSHOT_CHUNK: case MP_MSG_WHERE: case MP_MSG_MOVE: case MP_MSG_GO: case MP_MSG_RESULTS: case MP_MSG_HOLD:
    case MP_MSG_SWITCHMAP: case MP_MSG_TOAST: case MP_MSG_RESTART: case MP_MSG_OBJMAP: case MP_MSG_STARV:
        return true;
    default:
        return false;
    }
}

enum MpRejectReason : uint8_t {
    MP_REJECT_VERSION = 1, MP_REJECT_FULL = 2, MP_REJECT_MODE = 3, MP_REJECT_KICKED = 4, MP_REJECT_CLOSED = 5, MP_REJECT_BUSY = 6
};
enum MpByeReason : uint8_t { MP_BYE_LEAVE = 1, MP_BYE_HOST_CLOSED = 2, MP_BYE_TIMEOUT = 3, MP_BYE_SUSPEND = 4, MP_BYE_ERROR = 5,
                             MP_BYE_HOST_LEAVING = 6 /* host leaves, session continues: migrate now */,
                             MP_BYE_RESYNC = 7 /* netcore (host -> guest): a new host cannot catch this guest up: rejoin (new membership) */,
                             MP_BYE_REMOVED = 8 /* core2 (host -> a console it removed, sealed): you are no longer in the room: rejoin */ };

// Host-chosen session settings (WELCOME, SETTINGS). Fixed 16 bytes on the wire.
struct MpSettings {
    uint8_t mode;            // MpMode
    uint8_t maxPlayers;      // 2..4
    uint8_t interaction;     // 0 off, 1 friendly (default), 2 rough
    uint8_t sharedKills;     // 0/1 (default 1)
    uint8_t rivals;          // race: 0 solid, 1 ghost (default), 2 hidden
    uint8_t raceWaitSec;     // race: seconds to wait after the first finisher / 10 (0 = no limit); default 18 (3 min)
    uint8_t nameTags;        // 0 off, 1 on (default), 2 on + edge arrows
    uint8_t flags;           // bit0 drop-in allowed (default 1), bit1 shared purple coins (1), bit2 shared star chips (1)
    uint8_t downed;          // Play Together, what a fall does: 0 bubbles (default; NSMB style), 1 infinite lives (checkpoint)
    uint8_t reserved[7];
};
enum MpDowned : uint8_t { MP_DOWNED_BUBBLES = 0, MP_DOWNED_INFINITE = 1, MP_DOWNED_COUNT = 2 };

// Player pose (STATE). See DESIGN.md "MpPose". 51 bytes on the wire (mp_write_pose), optionally followed by a 17-byte tail
// (tag 0x54 + 4 x u32 per-track animation hashes, MpPoseMath::writeState): STATE payloads are 51 or 68 bytes; relay them unchanged.
enum MpPoseFlag : uint16_t {
    MP_POSE_VISIBLE = 1 << 0, MP_POSE_ON_GROUND = 1 << 1, MP_POSE_IN_WATER = 1 << 2, MP_POSE_DEAD = 1 << 3, MP_POSE_IN_DEMO = 1 << 4,
    MP_POSE_SHELL = 1 << 5, MP_POSE_UPPER = 1 << 6, MP_POSE_MIRROR = 1 << 7, MP_POSE_SQUASHED = 1 << 8, MP_POSE_SIMPLE_BCK = 1 << 9,
    MP_POSE_PAUSED = 1 << 10, MP_POSE_FINISHED = 1 << 11,
    MP_POSE_BUBBLE = 1 << 12,  // the player floats in a bubble (Play Together "Bubbles" falls): receivers draw the bubble
    MP_POSE_AWAY = 1 << 13,    // touches: "Be right back" / 30 s idle: a dimmed head on the strip and "zzz" on the tag (cosmetic; old builds ignore the bit)
    MP_POSE_IN_BIND = 1 << 14  // g30 Wave 8: player is bound in a fly-in / Launch Star / ride (suppresses mid-air body pushing; old builds ignore the bit)
};
struct MpPose {
    uint32_t turn;           // sender's logic-turn counter (60/s)
    uint16_t sceneSerial;    // host scene serial the sender is in
    uint8_t zone;            // zone id the player stands in (0 = main), 0xFF unknown
    uint8_t form;            // MpForm
    float pos[3];            // world position (MarioActor::mPosition)
    int16_t quat[4];         // base rotation as a unit quaternion x,y,z,w * 32767
    int8_t vel[3];           // velocity / 2 (units per turn), clamped
    uint8_t scale;           // uniform scale * 64 (64 = 1.0)
    uint32_t anim;           // lower-body animation: Xanime hash, or a hash of the simple bck name when MP_POSE_SIMPLE_BCK
    uint16_t frame;          // frame * 16
    int8_t rate;             // rate * 32
    uint8_t weights[4];      // track weights * 255
    uint32_t upperAnim;      // upper-body animation hash (MP_POSE_UPPER)
    uint16_t upperFrame;     // * 16
    uint16_t flags;          // MpPoseFlag
};

struct MpObjKey {            // placement identity (DESIGN.md section 4, r-world): same on every console
    uint8_t zone;
    uint8_t sub;
    uint16_t lid;
    uint16_t nameHash;       // 15-bit hash of the ASCII placement name (bytes as uint8_t)
};

// ---- serialisation ---------------------------------------------------------------------------------------------------------
struct MpWriter {
    uint8_t* p;
    size_t cap, n;
    bool bad;
    MpWriter(void* buf, size_t capacity) : p((uint8_t*)buf), cap(capacity), n(0), bad(false) {}
    void bytes(const void* src, size_t len) {
        if (bad || len > cap - n) { bad = true; return; }
        memcpy(p + n, src, len); n += len;
    }
    void u8(uint8_t v) { bytes(&v, 1); }
    void u16(uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; bytes(b, 2); }
    void u32(uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; bytes(b, 4); }
    void s8(int8_t v) { u8((uint8_t)v); }
    void s16(int16_t v) { u16((uint16_t)v); }
    void f32(float v) { uint32_t u; memcpy(&u, &v, 4); u32(u); }
    bool ok() const { return !bad; }
};
struct MpReader {
    const uint8_t* p;
    size_t len, n;
    bool bad;
    MpReader(const void* buf, size_t length) : p((const uint8_t*)buf), len(length), n(0), bad(false) {}
    bool bytes(void* dst, size_t count) {
        if (bad || count > len - n) { bad = true; if (dst && count) memset(dst, 0, count); return false; }
        memcpy(dst, p + n, count); n += count; return true;
    }
    uint8_t u8() { uint8_t v = 0; bytes(&v, 1); return v; }
    uint16_t u16() { uint8_t b[2]; if (!bytes(b, 2)) return 0; return (uint16_t)(b[0] | (b[1] << 8)); }
    uint32_t u32() { uint8_t b[4]; if (!bytes(b, 4)) return 0; return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
    int8_t s8() { return (int8_t)u8(); }
    int16_t s16() { return (int16_t)u16(); }
    float f32() { uint32_t u = u32(); float f; memcpy(&f, &u, 4); return f; }
    bool ok() const { return !bad; }
    bool done() const { return !bad && n == len; }   // the whole message was consumed (use for fixed-size messages)
};

// Pose, settings and key codecs (implemented inline so both sides and the tests share one definition).
static inline void mp_write_settings(MpWriter& w, const MpSettings& s) {
    w.u8(s.mode); w.u8(s.maxPlayers); w.u8(s.interaction); w.u8(s.sharedKills); w.u8(s.rivals); w.u8(s.raceWaitSec);
    w.u8(s.nameTags); w.u8(s.flags); w.u8(s.downed); w.bytes(s.reserved, sizeof s.reserved);
}
static inline bool mp_read_settings(MpReader& r, MpSettings* s) {
    s->mode = r.u8(); s->maxPlayers = r.u8(); s->interaction = r.u8(); s->sharedKills = r.u8(); s->rivals = r.u8();
    s->raceWaitSec = r.u8(); s->nameTags = r.u8(); s->flags = r.u8(); s->downed = r.u8(); r.bytes(s->reserved, sizeof s->reserved);
    return r.ok() && (s->mode == MP_MODE_TOGETHER || s->mode == MP_MODE_RACE) && s->maxPlayers >= 2 && s->maxPlayers <= MP_MAX_PLAYERS &&
           s->interaction <= 2 && s->sharedKills <= 1 && s->rivals <= 2 && s->nameTags <= 2 && s->downed < MP_DOWNED_COUNT;
}

// BUBBLE (0x3B): the sender went into its bubble (on 1) or out of it (on 0). lives = the sender's lives after the change;
// by = the slot whose touch popped it (0xFF: its own A press, a restart or a new scene); flags bit0 = out of lives (only a
// teammate's touch can pop it; it rejoins with 1 life), bit1 = an arrival bubble (drop-in / warp to a friend: no life lost, pops
// by itself; never with bit0). Fixed 6 bytes. RESTART (0x3C): serial u16, fixed 2 bytes.
enum MpBubbleFlag : uint8_t { MP_BUBBLE_OUT_OF_LIVES = 1 << 0, MP_BUBBLE_ARRIVAL = 1 << 1 };
#define MP_BUBBLE_BY_NONE 0xFF
struct MpBubbleMsg {
    uint16_t serial;
    uint8_t on;
    uint8_t lives;
    uint8_t by;
    uint8_t flags;
};
static inline void mp_write_bubble(MpWriter& w, const MpBubbleMsg& b) { w.u16(b.serial); w.u8(b.on); w.u8(b.lives); w.u8(b.by); w.u8(b.flags); }
static inline bool mp_read_bubble(MpReader& r, MpBubbleMsg* b) {
    b->serial = r.u16(); b->on = r.u8(); b->lives = r.u8(); b->by = r.u8(); b->flags = r.u8();
    return r.done() && b->on <= 1 && b->lives <= 99 && (b->by < MP_MAX_PLAYERS || b->by == MP_BUBBLE_BY_NONE) &&
           (b->flags & ~(MP_BUBBLE_OUT_OF_LIVES | MP_BUBBLE_ARRIVAL)) == 0 && (b->on || b->flags == 0) &&
           b->flags != (MP_BUBBLE_OUT_OF_LIVES | MP_BUBBLE_ARRIVAL);
}
static inline void mp_write_pose(MpWriter& w, const MpPose& p) {
    w.u32(p.turn); w.u16(p.sceneSerial); w.u8(p.zone); w.u8(p.form);
    for (int i = 0; i < 3; i++) w.f32(p.pos[i]);
    for (int i = 0; i < 4; i++) w.s16(p.quat[i]);
    for (int i = 0; i < 3; i++) w.s8(p.vel[i]);
    w.u8(p.scale); w.u32(p.anim); w.u16(p.frame); w.s8(p.rate);
    for (int i = 0; i < 4; i++) w.u8(p.weights[i]);
    w.u32(p.upperAnim); w.u16(p.upperFrame); w.u16(p.flags);
}
static inline bool mp_pose_float_ok(float f) { return f == f && f > -1.0e6f && f < 1.0e6f; }
static inline bool mp_read_pose(MpReader& r, MpPose* p) {
    p->turn = r.u32(); p->sceneSerial = r.u16(); p->zone = r.u8(); p->form = r.u8();
    for (int i = 0; i < 3; i++) p->pos[i] = r.f32();
    for (int i = 0; i < 4; i++) p->quat[i] = r.s16();
    for (int i = 0; i < 3; i++) p->vel[i] = r.s8();
    p->scale = r.u8(); p->anim = r.u32(); p->frame = r.u16(); p->rate = r.s8();
    for (int i = 0; i < 4; i++) p->weights[i] = r.u8();
    p->upperAnim = r.u32(); p->upperFrame = r.u16(); p->flags = r.u16();
    if (p->form >= MP_FORM_COUNT) p->form = MP_FORM_NORMAL;   // (puppetprep) an unknown form (newer build / garbage) shows the plain player
    if (!r.ok()) return false;
    for (int i = 0; i < 3; i++) if (!mp_pose_float_ok(p->pos[i])) return false;
    return true;
}
static inline void mp_write_key(MpWriter& w, const MpObjKey& k) { w.u8(k.zone); w.u8(k.sub); w.u16(k.lid); w.u16(k.nameHash); }
static inline bool mp_read_key(MpReader& r, MpObjKey* k) {
    k->zone = r.u8(); k->sub = r.u8(); k->lid = r.u16(); k->nameHash = r.u16();
    return r.ok() && k->nameHash < 0x8000;
}
// EMOTE (0x22, reliable): a greeting (coop agent). emote = the stamp it shows (0 "Over here!", 1 "Thanks!", 2 "Wait for me!",
// 3 "Let's go!"); flags bit0 = the sender plays the gesture (its animation reaches the others through STATE). Fixed 2 bytes.
#define MP_EMOTE_COUNT 4
enum MpEmoteFlag : uint8_t { MP_EMOTE_GESTURE = 1 << 0 };
struct MpEmoteMsg {
    uint8_t emote;
    uint8_t flags;
};
static inline void mp_write_emote(MpWriter& w, const MpEmoteMsg& e) { w.u8(e.emote); w.u8(e.flags); }
static inline bool mp_read_emote(MpReader& r, MpEmoteMsg* e) {
    e->emote = r.u8(); e->flags = r.u8();
    return r.done() && e->emote < MP_EMOTE_COUNT && (e->flags & ~MP_EMOTE_GESTURE) == 0;
}
// POINTER (0x24, unreliable, latest wins): the sender holds the Star Pointer button; pos = the world point under its pointer
// (shown to the others as a small pointer in the sender's slot colour; never fed to game logic). on 0 = released (also sent
// once; receivers time a silent pointer out). serial = the sender's scene serial. Fixed 15 bytes.
struct MpPointerMsg {
    uint16_t serial;
    uint8_t on;
    float pos[3];
};
static inline void mp_write_pointer(MpWriter& w, const MpPointerMsg& m) {
    w.u16(m.serial); w.u8(m.on);
    for (int i = 0; i < 3; i++) w.f32(m.pos[i]);
}
static inline bool mp_read_pointer(MpReader& r, MpPointerMsg* m) {
    m->serial = r.u16(); m->on = r.u8();
    for (int i = 0; i < 3; i++) m->pos[i] = r.f32();
    if (!r.done() || m->on > 1) return false;
    for (int i = 0; i < 3; i++) if (!mp_pose_float_ok(m->pos[i])) return false;
    return true;
}

// 15-bit placement-name hash: FNV-1a over the bytes as uint8_t (char signedness differs between x86 and ARM), folded to 15 bits.
static inline uint16_t mp_name_hash15(const char* s) {
    uint32_t h = 2166136261u;
    for (const unsigned char* q = (const unsigned char*)s; q && *q; q++) { h ^= *q; h *= 16777619u; }
    return (uint16_t)((h ^ (h >> 15)) & 0x7FFF);
}

// A message as the game side sees it (mp_poll).
struct MpMsg {
    uint8_t type;            // MpMsgType (0x20..0x5F)
    uint8_t fromSlot;        // original sender (0..3), even when the host relayed it
    uint16_t len;            // payload bytes
    uint8_t data[MP_MAX_PAYLOAD];
};

#endif  // PORT_MP_PROTO_H
