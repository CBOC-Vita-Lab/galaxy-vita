// Galaxy 3.0 multiplayer (port side, owner: core): Same Wi-Fi / Nearby discovery. The host broadcasts a beacon every 500 ms to
// MP_LAN_PORT from its session socket (so the source address is the session address to join); scanners keep a list of rooms.
// The ad hoc backend sends every broadcast twice: copies are de-duplicated by the beacon's sequence number.
//
// Beacon (40 bytes, little-endian): magic "SGB1", proto u8, mode u8, players u8, max u8, flags u8 (bit0 open for joining),
// hostSlot u8, epoch u16, fingerprint u32, sessionId u32, seq u32, name[16] (the current host's name).
#ifndef PORT_MP_LAN_H
#define PORT_MP_LAN_H

#include "mp_transport.h"

#define MP_BEACON_BYTES 52          // core2: + closingInto (44) + the room key's tag (52); 40 / 44-byte beacons are still read
#define MP_BEACON_SIGNED 44         // core2: the bytes the tag covers
#define MP_BEACON_TAG 8             // core2: BLAKE2b(key = the room key, the first 44 bytes), 8 bytes: only the room's members can make it
#define MP_BEACON_OPEN 1             // flags: joinable now
#define MP_BEACON_GATHERING 2        // core2: a room that has not started and gathers (one door, DESIGN.md 10.6)
#define MP_BEACON_STARTED 4          // core2: the game has started
#define MP_BEACON_CLOSING 8          // core2: this gathering room is closing into the room closingInto
#define MP_BEACON_MS 500
#define MP_LAN_MOVE_BEACONS 3   // corefix 10: beacons from a new address before a scanned room moves there
#define MP_LAN_MOVE_SPAN_MS 1000
#define MP_LAN_EXPIRE_MS 3000
#define MP_LAN_MAX_ROOMS 16
#define MP_LAN_MOVE_SILENT_MS 2500   // core2 / netfix B5b: a scanned room moves to another address only after its first one was silent this long (< MP_LAN_EXPIRE_MS, or expire() would make the rule dead)
#define MP_LAN_ROOMS_PER_ADDR 2      // core2: rooms one source address may list
// lanfix: a stranger on the same Wi-Fi must not be able to hide a friend's room (review S2: 8 spoofed addresses filled the 16-room list).
// Rooms have a trust tier: 0 untrusted (any beacon, including every untagged 40 / 44-byte one), 1 a host whose name is on the
// trusted-names list (friends, Played With: mp_lan_set_trusted_names), 2 a beacon whose room-key tag the caller verified. A newer room of a
// higher tier evicts the oldest room of a lower tier; a room that stopped beaconing (MP_LAN_STALE_MS) is replaceable by the same tier;
// untrusted rooms never take more than MP_LAN_UNTRUSTED_MAX slots and name-trusted ones no more than MP_LAN_NAMED_MAX (a name is not
// proof); new rooms per second are limited per tier group, so a flood of addresses cannot churn the list or burn the CPU.
#define MP_LAN_STALE_MS 1500
#define MP_LAN_UNTRUSTED_MAX 10
#define MP_LAN_NAMED_MAX 4
#define MP_LAN_NAMED_PER_NAME 2      // corefix2: named slots one name (spoofable) may hold
#define MP_LAN_NEW_PER_SEC 8         // untrusted new rooms per second (burst the same)
#define MP_LAN_NEW_TRUSTED_PER_SEC 16
#define MP_LAN_TRUSTED_SOURCES 2     // 0: the Played With list (flow.cfg), 1: the online friends list
#define MP_LAN_TRUSTED_PER_SOURCE 48
enum MpLanTrust : uint8_t { MP_LAN_UNTRUSTED = 0, MP_LAN_NAMED = 1, MP_LAN_KEYED = 2 };
// The names (UTF-8) whose rooms outrank strangers', one list per source (Played With, friends); each call replaces its source's list.
// Copied (cleaned like a room name); thread-safe; n is clamped. n = 0 clears.
void mp_lan_set_trusted_names(int source, const char (*names)[MP_NAME_BYTES + 1], int n) __attribute__((weak));   // (weak: hosts that link only the menus skip it)

struct MpBeacon {
    uint8_t proto, mode, players, maxPlayers, flags, hostSlot;
    uint16_t epoch;
    uint32_t fingerprint, sessionId, seq;
    char name[MP_NAME_BYTES + 1];
    uint32_t closingInto;   // core2: with MP_BEACON_CLOSING: the room this one is closing into
    bool tagged;            // core2: a 52-byte beacon: tag holds what the host wrote (checked by members only: MpCore::checkMerge)
    uint8_t tag[MP_BEACON_TAG];
};

size_t mp_beacon_write(uint8_t* buf, size_t cap, const MpBeacon& b);
bool mp_beacon_read(const uint8_t* buf, size_t len, MpBeacon* out);   // validates everything; false = drop

// netx: active discovery. A scanner also broadcasts a probe (12 bytes: magic "SGQ1", proto u8, 3 zero bytes, fingerprint u32) to
// MP_LAN_PORT once a second; every host answers it with its beacon sent straight (unicast) to the prober, rate-limited. Unicast
// reaches a scanner where the access point delays or drops broadcasts towards wireless clients (power-save DTIM, "multicast
// enhancement", broadcast filtering), so rooms show up at once instead of after the next broadcast that gets through. Hosts of
// older builds ignore probes (a beacon reader refuses anything but 40 bytes), so both sides stay compatible.
#define MP_PROBE_BYTES 12
#define MP_PROBE_MS 1000
#define MP_PROBE_ANSWERS_PER_SEC 20
size_t mp_probe_write(uint8_t* buf, size_t cap, uint32_t fingerprint);
bool mp_probe_read(const uint8_t* buf, size_t len);

class MpLanScan {
public:
    MpLanScan() { clear(); }
    void clear();
    // A received beacon from `from`. Returns true when it was new information (not a duplicate copy).
    // tagValid: the caller checked the beacon's tag against a room key it holds (only members can); such a room is tier 2.
    bool add(const MpAddr& from, const MpBeacon& b, uint64_t nowMs, uint32_t myFingerprint, bool tagValid = false);
    void expire(uint64_t nowMs);
    int count() const { return m_count; }
    bool get(int index, MpLocalRoom* out, MpAddr* addr, uint32_t* sessionId) const;
    bool findSession(uint32_t sessionId, MpAddr* addr) const;
    int trust(int index) const { return index >= 0 && index < m_count ? m_rooms[index].trust : 0; }   // lanfix: MpLanTrust of a listed room
    bool getBeacon(int index, MpBeacon* b, MpAddr* addr, uint64_t* lastMs) const;   // core2: the last beacon of a listed room
private:
    struct Room {
        MpAddr addr;
        MpBeacon b;
        uint64_t lastMs;
        bool compatible;
        uint32_t candHost, candSeq;   // corefix 10: another address claiming this session (see add)
        uint64_t candFirst, candLast;
        int candCount;
        uint8_t trust;                // lanfix: MpLanTrust
    };
    int victimFor(int tier, bool tierFull, uint64_t nowMs) const;
    void removeAt(int i);
    bool takeToken(int group, uint64_t nowMs);
    static void logRoom(const Room& r);
    Room m_rooms[MP_LAN_MAX_ROOMS];
    int m_count;
    uint32_t m_tokens[2];     // lanfix: new-room budget in milli-rooms, [0] untrusted, [1] trusted
    uint64_t m_tokenAt[2];
};

#endif  // PORT_MP_LAN_H
