// Fake signaling for tests: rooms in memory (one process, the unit tests) or in a directory (two game processes on one machine).
// The room logic is the server's: slots in join order, vip 10.64.0.<slot+1>, a random session key and cookie per room.
#include "np_signal.h"

#if !defined(NP_TARGET_VITA)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <time.h>

#include <map>
#include <string>
#include <vector>

#include "np_crypto.h"
#include "np_hash.h"
#include "np_json.h"
#include "np_signal_http.h"

namespace NpNet {
namespace {

uint32_t SlotVip(int slot) { return 0x0A400000u | uint32_t(slot + 1); }   // 10.64.0.<slot+1>

void CopyName(char* dst, const char* src) {
    size_t i = 0;
    for (; src && src[i] && i < 15; ++i) dst[i] = src[i];
    dst[i] = 0;
}

void MakeInvite(char out[16], uint32_t r) {
    static const char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   // no 0/O/1/I
    for (int i = 0; i < 6; ++i) {
        out[i] = kAlphabet[r % 32];
        r = r / 32 + 977 * (i + 1);
    }
    out[6] = 0;
}

}  // namespace

void FakeMintTurn(TurnInfo* out, const char* urls, const char* secret, uint64_t nowMs, uint32_t reportedTtl, uint32_t realTtl, uint32_t serial) {
    *out = TurnInfo();
    std::string list = urls ? urls : "";
    size_t p = 0;
    while (p <= list.size() && out->nUrls < 4) {
        size_t e = list.find(',', p);
        if (e == std::string::npos) e = list.size();
        const std::string u = list.substr(p, e - p);
        if (!u.empty()) snprintf(out->urls[out->nUrls++], sizeof out->urls[0], "%s", u.c_str());
        p = e + 1;
    }
    snprintf(out->user, sizeof out->user, "%llu:fake%u", (unsigned long long)(nowMs / 1000 + realTtl), serial);
    uint8_t mac[20];
    HmacSha1(mac, secret ? secret : "", secret ? strlen(secret) : 0, out->user, strlen(out->user));
    snprintf(out->pass, sizeof out->pass, "%s", Base64Encode(mac, 20).c_str());
    out->ttlSec = reportedTtl;
}

// ---- in memory ------------------------------------------------------------------------------------------------------------
class SignalMemoryHub {
public:
    struct Room {
        SigRoom room;   // the shared state; mySlot is per member
        bool exists;
        Room() : exists(false) {}
    };
    std::map<std::string, Room> rooms;
    std::vector<PunchReport> reports;
    std::string turnUrls, turnSecret;
    uint32_t turnReported = 3600, turnReal = 3600;
    int turnBudget = -1, turnRequests = 0;
    uint32_t rng;
    explicit SignalMemoryHub(uint32_t seed) : rng(seed ? seed : 1) {}
    uint32_t Next() {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    }
};

SignalMemoryHub* CreateSignalMemoryHub(uint32_t seed) { return new SignalMemoryHub(seed); }
int SignalMemoryHubReports(SignalMemoryHub* hub, PunchReport* out, int max) {
    int n = 0;
    for (size_t i = 0; i < hub->reports.size() && n < max; ++i) out[n++] = hub->reports[i];
    return n;
}
void DestroySignalMemoryHub(SignalMemoryHub* hub) { delete hub; }
void SignalMemoryHubSetTurn(SignalMemoryHub* hub, const char* urls, const char* secret, uint32_t reportedTtl, uint32_t realTtl, int budget) {
    hub->turnUrls = urls ? urls : "";
    hub->turnSecret = secret ? secret : "";
    hub->turnReported = reportedTtl;
    hub->turnReal = realTtl;
    hub->turnBudget = budget;
}
int SignalMemoryHubTurnRequests(SignalMemoryHub* hub) { return hub->turnRequests; }

// Galaxy: a member leaves (Leave or evicted); the host role moves to the lowest remaining slot, as the service does (host migration)
static void HubRemoveMember(SigRoom& r, int slot) {
    if (slot < 0 || slot >= kMaxSlots || !r.members[slot].present) return;
    r.members[slot] = SigMember();
    ++r.version;
    if (slot != r.hostSlot) return;
    for (int s = 0; s < kMaxSlots; ++s)
        if (r.members[s].present) {
            r.hostSlot = s;
            ++r.hostEpoch;
            return;
        }
}
void SignalMemoryHubEvict(SignalMemoryHub* hub, const char* invite, int slot) {
    std::map<std::string, SignalMemoryHub::Room>::iterator it = hub->rooms.find(invite ? invite : "");
    if (it != hub->rooms.end()) HubRemoveMember(it->second.room, slot);
}

namespace {
class MemorySignal : public Signal {
public:
    explicit MemorySignal(SignalMemoryHub* hub) : hub_(hub), status_(IDLE), slot_(-1), now_(SteadyNowMs), nowCtx_(0), turnState_(TURN_NONE) { err_[0] = 0; turnErr_[0] = 0; }
    void SetClock(uint64_t (*now)(void*), void* ctx) override { now_ = now; nowCtx_ = ctx; }
    void RequestTurn() override {
        if (status_ != IN_ROOM) return;
        ++hub_->turnRequests;
        turnErr_[0] = 0;
        turnWait_ = 0;
        if (hub_->turnUrls.empty()) {
            turnState_ = TURN_FAILED;
            snprintf(turnErr_, sizeof turnErr_, "%s", TurnSentence("turn_unavailable", "").c_str());
        } else if (hub_->turnBudget >= 0 && hub_->turnRequests > hub_->turnBudget) {
            turnState_ = TURN_FAILED;
            snprintf(turnErr_, sizeof turnErr_, "%s", TurnSentence("turn_budget", "").c_str());
            turnWait_ = 600000;
        } else {
            FakeMintTurn(&turn_, hub_->turnUrls.c_str(), hub_->turnSecret.c_str(), now_(nowCtx_), hub_->turnReported, hub_->turnReal, uint32_t(hub_->turnRequests));
            turnState_ = TURN_READY;
        }
    }
    TurnStatus TurnState() const override { return turnState_; }
    const TurnInfo& Turn() const override { return turn_; }
    const char* TurnError() const override { return turnErr_; }
    uint32_t TurnWaitMs() const override { return turnState_ == TURN_FAILED ? turnWait_ : 0; }
    void Host(const char* name) override {
        SignalMemoryHub::Room r;
        r.exists = true;
        MakeInvite(r.room.invite, hub_->Next());
        r.room.cookie = hub_->Next();
        for (int i = 0; i < 32; i += 4) {
            const uint32_t v = hub_->Next();
            memcpy(r.room.key + i, &v, 4);
        }
        r.room.members[0].present = true;
        r.room.members[0].slot = 0;
        r.room.members[0].vip = SlotVip(0);
        CopyName(r.room.members[0].name, name);
        r.room.version = 1;
        code_ = r.room.invite;
        hub_->rooms[code_] = r;
        slot_ = 0;
        status_ = IN_ROOM;
        Refresh();
    }
    void Join(const char* invite, const char* name) override {
        std::map<std::string, SignalMemoryHub::Room>::iterator it = hub_->rooms.find(invite ? invite : "");
        if (it == hub_->rooms.end() || !it->second.exists) {
            snprintf(err_, sizeof err_, "no such room");
            status_ = FAILED;
            return;
        }
        SigRoom& r = it->second.room;
        int slot = -1;
        for (int i = 0; i < kMaxSlots && slot < 0; ++i)
            if (!r.members[i].present) slot = i;
        if (slot < 0) {
            snprintf(err_, sizeof err_, "room is full");
            status_ = FAILED;
            return;
        }
        r.members[slot].present = true;
        r.members[slot].slot = uint8_t(slot);
        r.members[slot].vip = SlotVip(slot);
        CopyName(r.members[slot].name, name);
        ++r.version;
        code_ = invite;
        slot_ = slot;
        status_ = IN_ROOM;
        Refresh();
    }
    void PublishCandidates(const SigCandidates& c) override {
        if (status_ != IN_ROOM) return;
        SignalMemoryHub::Room& r = hub_->rooms[code_];
        r.room.members[slot_].cands = c;
        r.room.members[slot_].cands.valid = true;
        ++r.room.version;
    }
    void ReportPunch(const PunchReport& r) override { hub_->reports.push_back(r); }
    void Leave() override {
        if (status_ == IN_ROOM) HubRemoveMember(hub_->rooms[code_].room, slot_);   // Galaxy: with the host role moving on
        status_ = CLOSED;
    }
    void Poll() override {
        if (status_ == IN_ROOM) Refresh();
    }
    Status GetStatus() const override { return status_; }
    const SigRoom& Room() const override { return view_; }
    const char* Error() const override { return err_; }
    const char* ErrorCode() const override { return status_ == CLOSED && dropped_ ? "not_member" : ""; }

private:
    void Refresh() {
        const SigRoom& r = hub_->rooms[code_].room;
        if (slot_ >= 0 && slot_ < kMaxSlots && !r.members[slot_].present) {
            // Galaxy (fix3): the service dropped this member (SignalMemoryHubEvict: silent for a minute): its next call is 403 not_member
            snprintf(err_, sizeof err_, "You are not in that room any more.");
            dropped_ = true;
            status_ = CLOSED;
            return;
        }
        view_ = r;
        view_.mySlot = slot_;
    }
    bool dropped_ = false;
    SignalMemoryHub* hub_;
    uint32_t turnWait_ = 0;
    Status status_;
    int slot_;
    std::string code_;
    SigRoom view_;
    char err_[64];
    uint64_t (*now_)(void*);
    void* nowCtx_;
    TurnStatus turnState_;
    TurnInfo turn_;
    char turnErr_[200];
};

// ---- in a directory ---------------------------------------------------------------------------------------------------------
// <dir>/<code>.room   host-written once: "<cookie hex> <key hex>"
// <dir>/<code>.s<N>   slot claim (created O_EXCL), then rewritten by its owner with: "<name> <pub ip:port> <local ip:port> <nat> <valid>"
class FileSignal : public Signal {
public:
    explicit FileSignal(const char* dir) : dir_(dir), status_(IDLE), slot_(-1), lastSig_(0), turnState_(TURN_NONE), turnSerial_(0) { err_[0] = 0; turnErr_[0] = 0; }
    void Host(const char* name) override {
        uint8_t seed[8];
        SystemRandom(0, seed, sizeof seed);
        uint32_t r;
        memcpy(&r, seed, 4);
        char code[16];
        MakeInvite(code, r);
        // test aid: SMG_NET_INVITE names the room, so that the second process of a pair knows it without a side channel
        if (const char* fixed = getenv("SMG_NET_INVITE"))
            if (*fixed) CopyName(code, fixed);
        // an invite code is chosen for this run; a leftover room of an earlier run with the same code is replaced
        uint8_t key[32];
        SystemRandom(0, key, sizeof key);
        uint32_t cookie;
        SystemRandom(0, &cookie, 4);
        char text[160];
        int n = snprintf(text, sizeof text, "%08x ", cookie);
        for (int i = 0; i < 32; ++i) n += snprintf(text + n, sizeof text - n, "%02x", key[i]);
        for (int s = 0; s < kMaxSlots; ++s) unlink(Path(code, ".s", s).c_str());
        if (!WriteAtomic(Path(code, ".room", -1), text)) {
            snprintf(err_, sizeof err_, "cannot write the room file");
            status_ = FAILED;
            return;
        }
        code_ = code;
        if (!Claim(name)) return;
        status_ = IN_ROOM;
    }
    void Join(const char* invite, const char* name) override {
        code_ = invite ? invite : "";
        FILE* f = fopen(Path(code_.c_str(), ".room", -1).c_str(), "r");
        if (!f) {
            snprintf(err_, sizeof err_, "no such room");
            status_ = FAILED;
            return;
        }
        fclose(f);
        if (!Claim(name)) return;
        status_ = IN_ROOM;
    }
    void PublishCandidates(const SigCandidates& c) override {
        if (status_ != IN_ROOM) return;
        mine_ = c;
        mine_.valid = true;
        WriteMine();
    }
    void RequestTurn() override {
        if (status_ != IN_ROOM) return;
        const char* urls = getenv("SMG_NET_TURN_URLS");
        turnErr_[0] = 0;
        if (!urls || !*urls) {
            turnState_ = TURN_FAILED;
            snprintf(turnErr_, sizeof turnErr_, "%s", TurnSentence("turn_unavailable", "").c_str());
            return;
        }
        const char* secret = getenv("SMG_NET_TURN_SECRET");
        const uint32_t ttl = getenv("SMG_NET_TURN_TTL") ? uint32_t(atoi(getenv("SMG_NET_TURN_TTL"))) : 3600u;
        FakeMintTurn(&turn_, urls, secret ? secret : "", uint64_t(time(0)) * 1000u, ttl, ttl, ++turnSerial_);
        turnState_ = TURN_READY;
    }
    TurnStatus TurnState() const override { return turnState_; }
    const TurnInfo& Turn() const override { return turn_; }
    const char* TurnError() const override { return turnErr_; }
    void ReportPunch(const PunchReport& r) override {
        // appended to <dir>/<code>.r<slot>: "<peer> <ok> <ms> <path> <myNat> <peerNat> <observed>"
        if (status_ != IN_ROOM) return;
        FILE* f = fopen(Path(code_.c_str(), ".r", slot_).c_str(), "a");
        if (!f) return;
        fprintf(f, "%d %d %u %d %d %d %s\n", r.peerSlot, r.ok ? 1 : 0, (unsigned)r.ms, r.pathKind, r.myNat, r.peerNat, AddrStr(r.observed));
        fclose(f);
    }
    void Leave() override {
        if (status_ == IN_ROOM) unlink(Path(code_.c_str(), ".s", slot_).c_str());
        status_ = CLOSED;
    }
    void Poll() override {
        if (status_ != IN_ROOM) return;
        SigRoom r;
        CopyName(r.invite, code_.c_str());
        if (!ReadRoom(&r)) return;
        r.mySlot = slot_;
        uint32_t sig = 2166136261u;
        for (int i = 0; i < kMaxSlots; ++i) {
            const SigMember& m = r.members[i];
            const uint32_t parts[11] = {m.present, m.vip, m.cands.valid, m.cands.pub.ip, m.cands.pub.port, m.cands.local.ip, m.cands.local.port, m.cands.nat, m.cands.gen, m.cands.relay.ip, m.cands.relay.port};
            for (uint32_t p : parts) sig = (sig ^ p) * 16777619u;
        }
        if (sig != lastSig_ || view_.version == 0) {
            lastSig_ = sig;
            r.version = view_.version + 1;
            view_ = r;
        }
    }
    Status GetStatus() const override { return status_; }
    const SigRoom& Room() const override { return view_; }
    const char* Error() const override { return err_; }

private:
    std::string Path(const char* code, const char* ext, int slot) const {
        char buf[320];
        if (slot >= 0) snprintf(buf, sizeof buf, "%s/%s%s%d", dir_.c_str(), code, ext, slot);
        else snprintf(buf, sizeof buf, "%s/%s%s", dir_.c_str(), code, ext);
        return buf;
    }
    static bool WriteAtomic(const std::string& path, const char* text) {
        const std::string tmp = path + ".tmp" + std::to_string(getpid());
        FILE* f = fopen(tmp.c_str(), "w");
        if (!f) return false;
        fputs(text, f);
        fclose(f);
        return rename(tmp.c_str(), path.c_str()) == 0;
    }
    bool Claim(const char* name) {
        for (int s = 0; s < kMaxSlots; ++s) {
            const int fd = open(Path(code_.c_str(), ".s", s).c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
            if (fd < 0) continue;
            close(fd);
            slot_ = s;
            CopyName(name_, name);
            WriteMine();
            return true;
        }
        snprintf(err_, sizeof err_, "room is full");
        status_ = FAILED;
        return false;
    }
    void WriteMine() {
        char text[300];
        snprintf(text, sizeof text, "%s %u.%u.%u.%u:%u %u.%u.%u.%u:%u %d %d %u %u.%u.%u.%u:%u %u.%u.%u.%u:%u", name_[0] ? name_ : "-", mine_.pub.ip >> 24, (mine_.pub.ip >> 16) & 255,
                 (mine_.pub.ip >> 8) & 255, mine_.pub.ip & 255, mine_.pub.port, mine_.local.ip >> 24, (mine_.local.ip >> 16) & 255,
                 (mine_.local.ip >> 8) & 255, mine_.local.ip & 255, mine_.local.port, int(mine_.nat), mine_.valid ? 1 : 0, (unsigned)mine_.gen, mine_.pub2.ip >> 24, (mine_.pub2.ip >> 16) & 255, (mine_.pub2.ip >> 8) & 255, mine_.pub2.ip & 255, mine_.pub2.port,
                 mine_.relay.ip >> 24, (mine_.relay.ip >> 16) & 255, (mine_.relay.ip >> 8) & 255, mine_.relay.ip & 255, mine_.relay.port);
        WriteAtomic(Path(code_.c_str(), ".s", slot_), text);
    }
    bool ReadRoom(SigRoom* r) {
        FILE* f = fopen(Path(code_.c_str(), ".room", -1).c_str(), "r");
        if (!f) return false;
        char line[160] = {0};
        const bool got = fgets(line, sizeof line, f) != 0;
        fclose(f);
        if (!got) return false;
        unsigned cookie = 0;
        char keyHex[80] = {0};
        if (sscanf(line, "%x %70s", &cookie, keyHex) != 2 || strlen(keyHex) != 64) return false;
        r->cookie = cookie;
        for (int i = 0; i < 32; ++i) {
            unsigned v = 0;
            sscanf(keyHex + 2 * i, "%2x", &v);
            r->key[i] = uint8_t(v);
        }
        for (int s = 0; s < kMaxSlots; ++s) {
            FILE* m = fopen(Path(code_.c_str(), ".s", s).c_str(), "r");
            if (!m) continue;
            char text[240] = {0};
            const bool ok = fgets(text, sizeof text, m) != 0;
            fclose(m);
            SigMember& mem = r->members[s];
            mem.present = true;   // claimed (an empty or half-written file still counts as present)
            mem.slot = uint8_t(s);
            mem.vip = SlotVip(s);
            if (!ok) continue;
            char nm[32] = {0}, pub[32] = {0}, loc[32] = {0}, pub2[32] = {0}, rel[32] = {0};
            int nat = 0, valid = 0;
            unsigned gen = 0;
            if (sscanf(text, "%31s %31s %31s %d %d %u %31s %31s", nm, pub, loc, &nat, &valid, &gen, pub2, rel) == 8) {
                ParseAddr(rel, 0, &mem.cands.relay);
                mem.cands.gen = gen;
                ParseAddr(pub2, 0, &mem.cands.pub2);
                CopyName(mem.name, strcmp(nm, "-") ? nm : "");
                mem.cands.valid = valid != 0;
                ParseAddr(pub, 0, &mem.cands.pub);
                ParseAddr(loc, 0, &mem.cands.local);
                mem.cands.nat = uint8_t(nat);
            }
        }
        return true;
    }
    std::string dir_, code_;
    Status status_;
    int slot_;
    uint32_t lastSig_;
    SigCandidates mine_;
    char name_[16] = {0};
    SigRoom view_;
    char err_[64];
    TurnStatus turnState_;
    TurnInfo turn_;
    uint32_t turnSerial_;
    char turnErr_[200];
};
}  // namespace

Signal* CreateMemorySignal(SignalMemoryHub* hub) { return new MemorySignal(hub); }
Signal* CreateFileSignal(const char* directory) { return new FileSignal(directory); }

}  // namespace NpNet

#else  // NP_TARGET_VITA

namespace NpNet {
Signal* CreateMemorySignal(SignalMemoryHub*) { return 0; }
Signal* CreateFileSignal(const char*) { return 0; }
SignalMemoryHub* CreateSignalMemoryHub(uint32_t) { return 0; }
void DestroySignalMemoryHub(SignalMemoryHub*) {}
}  // namespace NpNet

#endif
