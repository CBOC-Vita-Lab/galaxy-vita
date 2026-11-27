// Galaxy 3.0 multiplayer (port side, owner: core): discovery beacons and the scan list. See mp_lan.h.
#include "mp_lan.h"

#include "mp_config.h"
#include "mp_core_sys.h"   // lanfix: MpMutex (the trusted names)
#include "port/mp_chat.h"   // lanfix: mp_chat_clean: room names are cleaned like typed messages
#include "port/mp_bridge.h"  // mp_log (harness: scan results are logged for tests)

#include <string.h>

static const uint8_t kMagic[4] = {'S', 'G', 'B', '1'};

// lanfix: a room name is shown to a player like a typed message, so it is cleaned by the same rules (mp_chat_clean: invalid UTF-8 and
// control / format / bidi characters dropped, astral and private-use characters '?', spaces collapsed, at most 2 combining marks in a row,
// whole characters only). The name is at most MP_NAME_BYTES bytes, so the result always fits out (MP_NAME_BYTES + 1).
static void CleanRoomName(const char* in, size_t n, char* out) {
    char raw[MP_NAME_BYTES + 1];
    size_t k = 0;
    while (k < n && k < MP_NAME_BYTES && in[k]) { raw[k] = in[k]; k++; }
    raw[k] = 0;
    mp_chat_clean(raw, k, out, MP_NAME_BYTES + 1);
}

// lanfix: the trusted names (friends, Played With), set by the menus, read by every scan.
static MpMutex& NamesMu() {
    static MpMutex m;
    return m;
}
static char s_trusted[MP_LAN_TRUSTED_SOURCES][MP_LAN_TRUSTED_PER_SOURCE][MP_NAME_BYTES + 1];
static int s_trustedN[MP_LAN_TRUSTED_SOURCES];

void mp_lan_set_trusted_names(int source, const char (*names)[MP_NAME_BYTES + 1], int n) {
    if (source < 0 || source >= MP_LAN_TRUSTED_SOURCES) return;
    MpLock l(NamesMu());
    s_trustedN[source] = 0;
    if (!names) return;
    for (int i = 0; i < n && s_trustedN[source] < MP_LAN_TRUSTED_PER_SOURCE; i++) {
        char clean[MP_NAME_BYTES + 1];
        CleanRoomName(names[i], MP_NAME_BYTES, clean);   // (the same form a beacon's name takes, so equal names compare equal)
        if (clean[0]) memcpy(s_trusted[source][s_trustedN[source]++], clean, sizeof clean);
    }
}

static bool NameTrusted(const char* name) {
    if (!name[0]) return false;
    MpLock l(NamesMu());
    for (int s = 0; s < MP_LAN_TRUSTED_SOURCES; s++)
        for (int i = 0; i < s_trustedN[s]; i++)
            if (!strncmp(s_trusted[s][i], name, MP_NAME_BYTES)) return true;
    return false;
}

size_t mp_beacon_write(uint8_t* buf, size_t cap, const MpBeacon& b) {
    MpWriter w(buf, cap);
    w.bytes(kMagic, 4);
    w.u8(b.proto);
    w.u8(b.mode);
    w.u8(b.players);
    w.u8(b.maxPlayers);
    w.u8(b.flags);
    w.u8(b.hostSlot);
    w.u16(b.epoch);
    w.u32(b.fingerprint);
    w.u32(b.sessionId);
    w.u32(b.seq);
    char name[MP_NAME_BYTES];
    memset(name, 0, sizeof name);
    memcpy(name, b.name, strnlen(b.name, MP_NAME_BYTES));
    w.bytes(name, MP_NAME_BYTES);
    w.u32(b.closingInto);
    uint8_t tag[MP_BEACON_TAG] = {0};   // (the caller with the room key writes the tag over these bytes)
    w.bytes(tag, sizeof tag);
    return w.ok() ? w.n : 0;
}

size_t mp_probe_write(uint8_t* buf, size_t cap, uint32_t fingerprint) {
    static const uint8_t kProbe[4] = {'S', 'G', 'Q', '1'};
    MpWriter w(buf, cap);
    w.bytes(kProbe, 4);
    w.u8(MP_PROTO_VERSION);
    w.u8(0);
    w.u8(0);
    w.u8(0);
    w.u32(fingerprint);
    return w.ok() ? w.n : 0;
}

bool mp_probe_read(const uint8_t* buf, size_t len) {
    return len == MP_PROBE_BYTES && buf[0] == 'S' && buf[1] == 'G' && buf[2] == 'Q' && buf[3] == '1';
}

bool mp_beacon_read(const uint8_t* buf, size_t len, MpBeacon* out) {
    if (len != MP_BEACON_BYTES && len != MP_BEACON_SIGNED && len != 40) return false;
    MpReader r(buf, len);
    uint8_t magic[4];
    r.bytes(magic, 4);
    if (!r.ok() || memcmp(magic, kMagic, 4) != 0) return false;
    out->proto = r.u8();
    out->mode = r.u8();
    out->players = r.u8();
    out->maxPlayers = r.u8();
    out->flags = r.u8();
    out->hostSlot = r.u8();
    out->epoch = r.u16();
    out->fingerprint = r.u32();
    out->sessionId = r.u32();
    out->seq = r.u32();
    char name[MP_NAME_BYTES];
    r.bytes(name, MP_NAME_BYTES);
    out->closingInto = len >= MP_BEACON_SIGNED ? r.u32() : 0;
    out->tagged = len == MP_BEACON_BYTES;
    if (out->tagged) r.bytes(out->tag, MP_BEACON_TAG);
    else memset(out->tag, 0, sizeof out->tag);
    if (!r.done()) return false;
    if (!(out->flags & MP_BEACON_CLOSING) || out->closingInto == out->sessionId) out->closingInto = 0;
    CleanRoomName(name, MP_NAME_BYTES, out->name);
    // the proto/fingerprint may differ (that is "incompatible", still listed); the rest must make sense
    if (out->maxPlayers < 2 || out->maxPlayers > MP_MAX_PLAYERS || out->players > out->maxPlayers || out->players == 0) return false;
    if (out->hostSlot >= MP_MAX_PLAYERS || out->sessionId == 0) return false;
    return true;
}

static_assert(MP_LAN_MOVE_SILENT_MS < MP_LAN_EXPIRE_MS, "expire() would drop the room before the move rule could ever apply");

void MpLanScan::clear() {
    m_count = 0;
    memset(m_rooms, 0, sizeof m_rooms);
    m_tokens[0] = MP_LAN_NEW_PER_SEC * 1000u;
    m_tokens[1] = MP_LAN_NEW_TRUSTED_PER_SEC * 1000u;
    m_tokenAt[0] = m_tokenAt[1] = 0;
}

// lanfix: a token bucket for NEW rooms (updates of listed rooms are free). group 0 untrusted, 1 trusted. Allocation-free.
bool MpLanScan::takeToken(int group, uint64_t nowMs) {
    const uint32_t rate = group ? MP_LAN_NEW_TRUSTED_PER_SEC : MP_LAN_NEW_PER_SEC;   // per second == milli-rooms per ms
    if (m_tokenAt[group] == 0 || nowMs < m_tokenAt[group]) m_tokenAt[group] = nowMs;   // (first use, or the clock restarted)
    const uint64_t dt = nowMs - m_tokenAt[group];
    m_tokenAt[group] = nowMs;
    uint64_t t = m_tokens[group] + dt * rate;
    if (t > rate * 1000ull) t = rate * 1000ull;
    if (t < 1000) {
        m_tokens[group] = (uint32_t)t;
        return false;
    }
    m_tokens[group] = (uint32_t)(t - 1000);
    return true;
}

void MpLanScan::removeAt(int i) {
    for (int k = i + 1; k < m_count; k++) m_rooms[k - 1] = m_rooms[k];
    m_count--;
}

// The room a new one of `tier` may replace, or -1. tierFull: the new room's own tier has no slot left, so only a stale room of that tier
// may go; otherwise (the list is full) the oldest room of the LOWEST lower tier goes, else the oldest stale one of the same tier.
int MpLanScan::victimFor(int tier, bool tierFull, uint64_t nowMs) const {
    int best = -1;
    if (!tierFull) {
        for (int low = 0; low < tier && best < 0; low++)
            for (int i = 0; i < m_count; i++)
                if (m_rooms[i].trust == low && (best < 0 || m_rooms[i].lastMs < m_rooms[best].lastMs)) best = i;
        if (best >= 0) return best;
    }
    for (int i = 0; i < m_count; i++)
        if (m_rooms[i].trust == tier && nowMs >= m_rooms[i].lastMs && nowMs - m_rooms[i].lastMs > MP_LAN_STALE_MS &&
            (best < 0 || m_rooms[i].lastMs < m_rooms[best].lastMs))
            best = i;
    return best;
}

bool MpLanScan::add(const MpAddr& from, const MpBeacon& beacon, uint64_t nowMs, uint32_t myFingerprint, bool tagValid) {
    MpBeacon b = beacon;   // (cleaned below: whoever built the beacon, the stored name follows the typed-message rules)
    {
        char clean[MP_NAME_BYTES + 1];
        CleanRoomName(b.name, sizeof b.name, clean);
        memcpy(b.name, clean, sizeof clean);
    }
    const int tier = tagValid ? MP_LAN_KEYED : NameTrusted(b.name) ? MP_LAN_NAMED : MP_LAN_UNTRUSTED;
    int counts[3] = {0, 0, 0};
    for (int i = 0; i < m_count; i++) counts[m_rooms[i].trust > 2 ? 2 : m_rooms[i].trust]++;
    for (int i = 0; i < m_count; i++) {
        Room& r = m_rooms[i];
        if (r.b.sessionId != b.sessionId) continue;
        if (r.addr == from) {
            r.candCount = 0;   // (the holder of the room is still beaconing: nobody else takes it over meanwhile)
            if ((int32_t)(b.seq - r.b.seq) <= 0) {   // a copy (ad hoc sends broadcasts twice) or an old one
                r.lastMs = nowMs;
                return false;
            }
        } else {
            if (b.epoch < r.b.epoch) return false;   // an old host of a session that migrated
            // corefix 10: the session id is public: a beacon of the same session from ANOTHER address does not take the room's join address
            // at once (a forged one would send every joiner to the attacker). A scanner knows no roster, so the new address must keep
            // beaconing: MP_LAN_MOVE_BEACONS beacons (a new sequence each) over MP_LAN_MOVE_SPAN_MS. The old address keeps the room meanwhile.
            if (!r.candHost || r.candHost != from.host || nowMs - r.candLast > 4 * 500) {
                r.candHost = from.host;
                r.candFirst = nowMs;
                r.candSeq = b.seq;
                r.candCount = 1;
            } else if ((int32_t)(b.seq - r.candSeq) > 0) {
                r.candSeq = b.seq;
                r.candCount++;
            }
            r.candLast = nowMs;
            if (r.candCount < MP_LAN_MOVE_BEACONS || nowMs - r.candFirst < MP_LAN_MOVE_SPAN_MS) return false;
            // core2 (review finding 11): and only once the address it was first seen at has gone quiet (a host that migrated or left):
            // forged beacons from another address cannot take a room whose host is still beaconing. (netfix B5b: this was 5 s, but expire()
            // drops a room after MP_LAN_EXPIRE_MS = 3 s of silence, so the rule could never apply in the core: now MP_LAN_MOVE_SILENT_MS < expiry)
            if (nowMs - r.lastMs < MP_LAN_MOVE_SILENT_MS) return false;
            mp_log("scan: the session %08x moved to another host address", (unsigned)b.sessionId);
        }
        r.candHost = 0;
        r.candCount = 0;
        r.addr = from;   // (after a host migration the session comes from the new host)
        r.b = b;
        r.lastMs = nowMs;
        {   // lanfix: the room's tier follows its latest beacon; a keyed room stays keyed; a name promotes only while the named tier has room
            int nt = tier;
            if (nt == MP_LAN_NAMED && r.trust != MP_LAN_NAMED && counts[MP_LAN_NAMED] >= MP_LAN_NAMED_MAX) nt = MP_LAN_UNTRUSTED;
            if (r.trust == MP_LAN_KEYED) nt = MP_LAN_KEYED;
            r.trust = (uint8_t)nt;
        }
        const bool was = r.compatible;
        r.compatible = b.proto == MP_PROTO_VERSION && b.fingerprint == myFingerprint && b.mode != MP_MODE_NONE;
        if (was != r.compatible) logRoom(r);
        return true;
    }
    int fromSame = 0;   // core2: one address cannot fill the list with rooms
    for (int i = 0; i < m_count; i++)
        if (m_rooms[i].addr.host == from.host) fromSame++;
    if (fromSame >= MP_LAN_ROOMS_PER_ADDR) return false;
    // lanfix: eviction by trust and staleness, and a rate limit on new rooms
    // corefix2: a name is not proof: one name (a spoofed friend) never holds more than MP_LAN_NAMED_PER_NAME of the named slots; a third room
    // of that name replaces the one of that name heard least recently (so the real friend's room, which keeps beaconing, is listed named)
    int victim = -1;
    if (tier == MP_LAN_NAMED) {
        int same = 0, oldest = -1;
        for (int i = 0; i < m_count; i++)
            if (m_rooms[i].trust == MP_LAN_NAMED && !strncmp(m_rooms[i].b.name, b.name, MP_NAME_BYTES)) {
                same++;
                if (oldest < 0 || m_rooms[i].lastMs < m_rooms[oldest].lastMs) oldest = i;
            }
        if (same >= MP_LAN_NAMED_PER_NAME) victim = oldest;
    }
    const bool tierFull = victim < 0 && ((tier == MP_LAN_UNTRUSTED && counts[0] >= MP_LAN_UNTRUSTED_MAX) || (tier == MP_LAN_NAMED && counts[1] >= MP_LAN_NAMED_MAX));
    if (victim < 0 && (m_count >= MP_LAN_MAX_ROOMS || tierFull)) {
        victim = victimFor(tier, tierFull, nowMs);
        if (victim < 0) return false;
    }
    if (!takeToken(tier ? 1 : 0, nowMs)) return false;
    if (victim >= 0) removeAt(victim);
    Room& r = m_rooms[m_count++];
    memset(&r, 0, sizeof r);
    r.addr = from;
    r.b = b;
    r.lastMs = nowMs;
    r.trust = (uint8_t)tier;
    r.compatible = b.proto == MP_PROTO_VERSION && b.fingerprint == myFingerprint && b.mode != MP_MODE_NONE;
    logRoom(r);
    return true;
}

// One line per room found (and when its compatibility changes): what the room list would show. Tests assert on it.
void MpLanScan::logRoom(const Room& r) {
    mp_log("scan: room host=%.*s mode=%d players=%d compatible=%d", (int)strnlen(r.b.name, MP_NAME_BYTES), r.b.name, (int)r.b.mode,
           (int)r.b.players, r.compatible ? 1 : 0);
    // macbot: what the other side speaks, for the practice partner's "your build differs" message (tests assert on the line above only)
    if (!r.compatible)
        mp_log("scan: room incompatible host=%.*s proto=%d fingerprint=%08x", (int)strnlen(r.b.name, MP_NAME_BYTES), r.b.name, (int)r.b.proto,
               (unsigned)r.b.fingerprint);
}

bool MpLanScan::getBeacon(int index, MpBeacon* b, MpAddr* addr, uint64_t* lastMs) const {
    if (index < 0 || index >= m_count) return false;
    if (b) *b = m_rooms[index].b;
    if (addr) *addr = m_rooms[index].addr;
    if (lastMs) *lastMs = m_rooms[index].lastMs;
    return true;
}

void MpLanScan::expire(uint64_t nowMs) {
    for (int i = 0; i < m_count;) {
        if (nowMs - m_rooms[i].lastMs > MP_LAN_EXPIRE_MS) {
            for (int k = i + 1; k < m_count; k++) m_rooms[k - 1] = m_rooms[k];
            m_count--;
        } else {
            i++;
        }
    }
}

bool MpLanScan::get(int index, MpLocalRoom* out, MpAddr* addr, uint32_t* sessionId) const {
    if (index < 0 || index >= m_count) return false;
    const Room& r = m_rooms[index];
    if (out) {
        memset(out, 0, sizeof *out);
        memcpy(out->host, r.b.name, sizeof out->host);
        out->host[MP_NAME_BYTES] = 0;
        out->mode = r.b.mode;
        out->players = (r.b.flags & 1) ? r.b.players : r.b.maxPlayers;   // closed to drop-in: shown as full
        out->maxPlayers = r.b.maxPlayers;
        out->compatible = r.compatible;   // (version; a full or closed room shows players == max)
        out->id = r.b.sessionId;
    }
    if (addr) *addr = r.addr;
    if (sessionId) *sessionId = r.b.sessionId;
    return true;
}

bool MpLanScan::findSession(uint32_t sessionId, MpAddr* addr) const {
    for (int i = 0; i < m_count; i++)
        if (m_rooms[i].b.sessionId == sessionId) {
            if (addr) *addr = m_rooms[i].addr;
            return true;
        }
    return false;
}
