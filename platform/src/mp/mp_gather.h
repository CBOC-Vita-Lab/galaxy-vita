// Galaxy 3.0 multiplayer (core2): the gathering driver (one door, DESIGN.md 10.6) over one MpCore. Port side, no game headers.
// The C API of port/mp_gather.h runs one of these on the global core (mp_gather.cpp); tests run one per core with a virtual clock.
#pragma once

#include "port/mp_gather.h"
#include "mp_core.h"

class MpGather {
public:
    explicit MpGather(MpCore* core) : m_c(core), m_lastError(MP_ERR_NONE) { m_lastReason[0] = 0; reset(); }
    bool start(MpMode mode, MpConn conn, uint64_t t);
    void stop();
    bool switchTo(MpConn conn, uint64_t t);
    void tick(uint64_t t);                 // drive (the C API ticks on every status call; tests every step)
    void status(MpGatherStatus* out, uint64_t t);
    int playingRooms(MpLocalRoom* out, int max, uint64_t t);
    bool joinPlaying(uint32_t roomId, uint64_t t);
    bool begin();
    bool canStart(uint64_t t);
    // core2: Play by Address from the gathering screen. codeAvailable: may the leader ask for a code now (no side effect). getCode:
    // the first call turns the room into a Play by Address room (its members follow, told the LAN address and the code's secret);
    // true with the code once the address is known (a few seconds; status.code shows it too). joinCode: the friend's side.
    bool codeAvailable(uint64_t t);
    bool getCode(char* out, size_t cap, uint64_t t);
    int joinCode(const char* code, uint64_t t);
    static const uint64_t kCodeJoinMs = 20000;   // a code join that has not got in by then: FAILED (the menu gives up at 25 s)
    MpGatherState state() const { return m_state; }
    uint32_t gid() const { return m_gid; }
    // test knobs: the facts a real console reads from the system
    void setRadio(bool wifiConnected, bool nearbyPossible) { m_wifi = wifiConnected; m_nearby = nearbyPossible; }
    // mpwifi44: why the last start() answered false or the gathering went FAILED: the error the player should read (MP_ERR_NONE: nothing
    // failed) and a short reason for the log ("link-wifi-down", "link-open-failed", "core-busy", ...). Kept until the next start().
    MpUiError lastError() const { return m_lastError; }
    const char* lastReason() const { return m_lastReason; }
    // mpwifi44: gathering with no Same Wi-Fi link (it could not be opened although the console is connected): no room yet, the link is
    // tried again every kLanRetryMs, and the far-away doors (a code, Play with Anyone) work meanwhile.
    bool noLan() const { return m_noLan; }
    static const uint64_t kLanRetryMs = 3000;
    void refreshRadio();   // wire: asks the platform's hooks again (when they exist)

    static const uint64_t kListenMs = 1500;     // look before opening a room
    static const uint64_t kAgreeMs = 1000;      // two agreeing beacons over at least this long
    static const uint64_t kClosingMs = 1000;    // CLOSING advertised this long before going
    static const uint64_t kRedirectMs = 1500;   // the closing host waits this long for its guests to go
    static const uint64_t kFollowMs = 8000;     // finding / joining the target room
    static const uint64_t kQuietMs = 1500;      // no other gathering room heard this long: the leader may show Start
    static const uint64_t kArrivingMs = 4000;   // heads of the previous room stay drawn this long while they come

private:
    enum Sub { SUB_NONE, SUB_CLOSING, SUB_REDIRECTED, SUB_LEAVING, SUB_FOLLOW, SUB_JOINING, SUB_RESOLVING };
    struct Room { MpBeacon b; MpAddr addr; uint64_t lastMs; };
    void reset();
    int rooms(Room* out, int max);
    bool better(uint8_t playersA, uint32_t idA, uint8_t playersB, uint32_t idB) const;   // rank A above B
    bool gatheringPeer(const MpBeacon& b) const;
    void openOwnRoom(uint64_t t);
    void follow(uint32_t target, uint64_t t);
    void snapshotHeads();
    MpCore* m_c;
    MpGatherState m_state;
    Sub m_sub;
    MpMode m_mode;
    MpConn m_conn;
    uint32_t m_gid;
    bool m_wifi, m_nearby;
    uint64_t m_phaseAt, m_subAt, m_roomAt, m_lastOtherAt;
    uint32_t m_target;            // the room this one closes into / follows
    uint32_t m_candId, m_candSeq; // the higher room being agreed on
    int m_candCount;
    uint64_t m_candFirst;
    uint32_t m_heard;
    uint64_t m_lastT;             // the last tick's time (begin)
    static const int kShun = 8;
    static const uint64_t kShunMs = 30000;   // a room we could not join (gone, or a forged beacon) is left out this long
    uint32_t m_shunId[kShun];
    uint64_t m_shunUntil[kShun];
    void shun(uint32_t id, uint64_t t);
    static const uint64_t kLowerWaitMs = 6000;   // a room ranked below this one, heard and not closing: Start waits this long for it
    uint32_t m_lowId[kShun];
    uint64_t m_lowFirst[kShun], m_lowLast[kShun];
    uint64_t lowerSince(uint32_t id, uint64_t t);
    bool shunned(uint32_t id) const;
    enum CodeSub { CODE_NONE, CODE_REDIRECTING, CODE_HOSTING, CODE_REGISTERING, CODE_READY };
    CodeSub m_codeSub;
    uint8_t m_secret[5];
    uint64_t m_codeAt;
    char m_code[MP_DIRECT_CODE_MAX];
    bool m_byCode;            // this console is in (or joining) a Play by Address room through a code: losing it is FAILED
    uint32_t m_codeIpFollow;
    uint16_t m_codePortFollow;
    bool joinByCode(uint32_t ip, uint16_t port, const uint8_t secret[5], uint64_t t);
    void codeTick(uint64_t t);
    // code5: the 5-character invite. Host: the address is known -> seal it, register it (CODE_REGISTERING) -> m_code is the five characters;
    // when the service cannot be had the long code (m_codeLong) is the fallback. Released (closed at the service) when the gathering ends.
    uint32_t m_hostIp;
    uint16_t m_hostPort;
    bool m_codeLong;              // the code shown is the long fallback, because the service failed
    bool m_inviteLive;            // a code is registered at the service for this room
    uint8_t m_inviteTries;
    uint64_t m_inviteKeepAt;
    uint32_t m_inviteKeepMs;
    char m_invite[8];             // the five characters registered (also m_code while live)
    char m_resolveCode[8];        // joiner: the five characters being looked up
    uint8_t m_inviteErr;          // joiner: why the lookup failed (MpInviteError), for the status
    void startRegister(uint64_t t);
    void finishLong(uint64_t t, int why);
    void inviteRelease();
    void failInvite(int why);
    // mpwifi44
    MpUiError m_lastError;
    char m_lastReason[32];
    bool m_noLan;
    uint64_t m_lanRetryAt;
    void noteFail(MpUiError e, const char* reason, const char* detail);   // remembers and logs "gather: failed reason=..."
    bool lanFailed(uint64_t t);   // the Same Wi-Fi / Nearby link did not come up: true = gathering goes on without it (m_noLan)
public:
    static const uint64_t kRegisterMs = 10000;   // the service has this long to give a code, else the long one
    static const uint64_t kResolveMs = 12000;    // and this long to answer a friend's lookup
private:
    MpGatherHead m_prev[MP_GATHER_HEADS];   // the heads of the room we came from (drawn while they arrive)
    int m_prevCount;
    uint64_t m_prevUntil;
};
