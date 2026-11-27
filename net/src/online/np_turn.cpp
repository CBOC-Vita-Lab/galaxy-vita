// TURN client: see np_turn.h.
#include "np_turn.h"

#include <stdio.h>
#include <string.h>

#include "np_hash.h"

namespace NpNet {

namespace {

const uint32_t kCookie = 0x2112A442u;
// message types (method | class): request 0, indication 0x10, success 0x100, error 0x110
const uint16_t kAllocate = 0x0003, kRefresh = 0x0004, kPermission = 0x0008, kChannelBind = 0x0009;
const uint16_t kDataInd = 0x0017;
// attributes
const uint16_t aUsername = 0x0006, aIntegrity = 0x0008, aErrorCode = 0x0009, aChannel = 0x000C, aLifetime = 0x000D, aPeer = 0x0012, aData = 0x0013, aRealm = 0x0014,
               aNonce = 0x0015, aRelayed = 0x0016, aTransport = 0x0019, aXorMapped = 0x0020;

void Put16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
uint32_t Get16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void CopyStr(char* dst, size_t cap, const char* src, size_t n = (size_t)-1) {
    size_t i = 0;
    for (; src && src[i] && i < n && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = 0;
}

// A message under construction.
struct Msg {
    uint8_t* b;
    size_t cap, n;
    Msg(uint8_t* buf, size_t capacity, uint16_t type, const uint8_t txid[12]) : b(buf), cap(capacity), n(20) {
        Put16(b, type);
        Put16(b + 2, 0);
        Put32(b + 4, kCookie);
        memcpy(b + 8, txid, 12);
    }
    void Attr(uint16_t type, const void* v, size_t len) {
        if (n + 4 + ((len + 3) & ~size_t(3)) > cap) return;
        Put16(b + n, type);
        Put16(b + n + 2, uint32_t(len));
        if (len) memcpy(b + n + 4, v, len);
        const size_t pad = ((len + 3) & ~size_t(3)) - len;
        memset(b + n + 4 + len, 0, pad);
        n += 4 + len + pad;
        Put16(b + 2, uint32_t(n - 20));
    }
    void AttrU32(uint16_t type, uint32_t v) {
        uint8_t t[4];
        Put32(t, v);
        Attr(type, t, 4);
    }
    void AttrStr(uint16_t type, const char* s) { Attr(type, s, strlen(s)); }
    void AttrXorAddr(uint16_t type, const Addr& a) {
        uint8_t t[8];
        t[0] = 0;
        t[1] = 1;
        Put16(t + 2, a.port ^ (kCookie >> 16));
        Put32(t + 4, a.ip ^ kCookie);
        Attr(type, t, 8);
    }
};

bool DecodeXorAddr(const uint8_t* v, size_t len, Addr* out) {
    if (len < 8 || v[1] != 1) return false;
    out->port = uint16_t(Get16(v + 2) ^ (kCookie >> 16));
    out->ip = Get32(v + 4) ^ kCookie;
    return true;
}

}  // namespace

struct TurnClient::Parsed {
    uint16_t type;
    uint8_t txid[12];
    bool hasRealm, hasNonce, hasLifetime, hasRelayed, hasMapped, hasPeer, hasData;
    char realm[128], nonce[160];
    int errCode;
    uint32_t lifetime;
    Addr relayed, mapped, peer;
    const uint8_t* data;
    size_t dataLen;
};

namespace {
bool ParseMsg(const uint8_t* d, size_t n, void* outP, bool (*sink)(void*, uint16_t, const uint8_t*, size_t)) {
    if (n < 20 || (d[0] & 0xC0) != 0 || Get32(d + 4) != kCookie) return false;
    const size_t body = Get16(d + 2);
    if (body + 20 > n || (body & 3)) return false;
    size_t o = 20;
    while (o + 4 <= 20 + body) {
        const uint16_t t = uint16_t(Get16(d + o));
        const size_t l = Get16(d + o + 2);
        if (o + 4 + l > 20 + body) return false;
        if (!sink(outP, t, d + o + 4, l)) return false;
        o += 4 + ((l + 3) & ~size_t(3));
    }
    return true;
}
}  // namespace

// ---- helpers ---------------------------------------------------------------------------------------------------------------
void TurnClient::LongTermKey(uint8_t out[16], const char* user, const char* realm, const char* password) {
    char buf[480];
    const int n = snprintf(buf, sizeof buf, "%s:%s:%s", user, realm, password);
    Md5(out, buf, n > 0 ? size_t(n) : 0);
}

// Adds MESSAGE-INTEGRITY: HMAC-SHA1 over the message so far, with the header length already counting the attribute (RFC 5389 15.4).
bool TurnClient::AddIntegrity(uint8_t* msg, size_t* len, size_t cap, const uint8_t key[16]) {
    if (*len + 24 > cap) return false;
    Put16(msg + 2, uint32_t(*len - 20 + 24));
    uint8_t mac[20];
    HmacSha1(mac, key, 16, msg, *len);
    Put16(msg + *len, aIntegrity);
    Put16(msg + *len + 2, 20);
    memcpy(msg + *len + 4, mac, 20);
    *len += 24;
    return true;
}

TurnClient::TurnClient() : env_(0), state_(IDLE), nServers_(0), serverIdx_(0), haveNonce_(false), lifetime_(0), generation_(0), refreshAt_(0), expiresAt_(0), credsRejected_(false), authRefused_(false), randomFailed_(false),
      released_(false), wasReady_(false), reallocAfterRelease_(false), allocRetryAt_(0), orphanSince_(0), refreshFails_(0), nextChannel_(0x4000), lastAuthLen_(0) {
    memset(&stats_, 0, sizeof stats_);
    memset(txns_, 0, sizeof txns_);
    memset(slots_, 0, sizeof slots_);
    user_[0] = pass_[0] = realm_[0] = nonce_[0] = error_[0] = 0;
    memset(key_, 0, sizeof key_);
}

Addr TurnClient::Server() const { return nServers_ ? servers_[serverIdx_] : Addr(); }

void TurnClient::Reset() {
    state_ = IDLE;
    memset(txns_, 0, sizeof txns_);
    memset(slots_, 0, sizeof slots_);
    relayed_ = mapped_ = Addr();
    haveNonce_ = false;
    credsRejected_ = authRefused_ = randomFailed_ = released_ = wasReady_ = reallocAfterRelease_ = false;
    allocRetryAt_ = orphanSince_ = 0;
    refreshFails_ = 0;
    error_[0] = 0;
    nServers_ = serverIdx_ = 0;
    nextChannel_ = 0x4000;
}

void TurnClient::Start(Env* env, const Addr* servers, int count, const char* user, const char* password, const TurnConfig& config) {
    Reset();
    env_ = env;
    cfg_ = config;
    nServers_ = count > 4 ? 4 : count;
    for (int i = 0; i < nServers_; ++i) servers_[i] = servers[i];
    CopyStr(user_, sizeof user_, user);
    CopyStr(pass_, sizeof pass_, password);
    if (nServers_ == 0) {
        Fail("The relay server address is unknown.");
        return;
    }
    StartAlloc();
}

void TurnClient::Fail(const char* why) {
    CopyStr(error_, sizeof error_, why);
    state_ = FAILED;
    memset(txns_, 0, sizeof txns_);
    NPN_LOG("turn: failed: %s\n", why);
}

void TurnClient::SendRaw(const uint8_t* d, size_t n) {
    if (env_ && env_->udp && nServers_) env_->udp->SendTo(servers_[serverIdx_], d, n);
}

TurnClient::Txn* TurnClient::Free() {
    for (int i = 0; i < 8; ++i)
        if (!txns_[i].active) return &txns_[i];
    return 0;
}
TurnClient::Txn* TurnClient::Find(const uint8_t txid[12]) {
    for (int i = 0; i < 8; ++i)
        if (txns_[i].active && memcmp(txns_[i].txid, txid, 12) == 0) return &txns_[i];
    return 0;
}

// An answer to a permission / channel request for an address the slot has since been bound away from says nothing about its current binding.
bool TurnClient::StaleSlotTxn(const Txn* t) const {
    if (t->kind != TK_PERM && t->kind != TK_BIND) return false;
    const Slot& s = slots_[t->arg];
    return !s.used || s.peer.ip != t->peerIp || s.peer.port != t->peerPort || s.channel != t->channel;
}

// (Re)builds the request of a transaction with a fresh transaction id and the current nonce.
void TurnClient::Rebuild(Txn* t) {
    if (!env_->Random(t->txid, 12)) randomFailed_ = true;   // (Tick fails the allocation with the generator's sentence)
    uint16_t type = kAllocate;
    switch (t->kind) {
    case TK_ALLOC: type = kAllocate; break;
    case TK_REFRESH:
    case TK_RELEASE: type = kRefresh; break;
    case TK_PERM: type = kPermission; break;
    case TK_BIND: type = kChannelBind; break;
    }
    Msg m(t->msg, 480, type, t->txid);
    if (t->kind == TK_ALLOC) {
        m.AttrU32(aTransport, 17u << 24);   // UDP
        m.AttrU32(aLifetime, cfg_.lifetimeSec);
    } else if (t->kind == TK_REFRESH) {
        m.AttrU32(aLifetime, cfg_.lifetimeSec);
    } else if (t->kind == TK_RELEASE) {
        m.AttrU32(aLifetime, 0);
    } else if (t->kind == TK_PERM || t->kind == TK_BIND) {
        const Slot& s = slots_[t->arg];
        t->peerIp = s.peer.ip;
        t->peerPort = s.peer.port;
        t->channel = s.channel;
        if (t->kind == TK_BIND) {
            uint8_t ch[4] = {uint8_t(s.channel >> 8), uint8_t(s.channel), 0, 0};
            m.Attr(aChannel, ch, 4);
        }
        m.AttrXorAddr(aPeer, s.peer);
    }
    t->len = m.n;
    t->authed = false;
    if (haveNonce_) {
        m.AttrStr(aUsername, user_);
        m.AttrStr(aRealm, realm_);
        m.AttrStr(aNonce, nonce_);
        t->len = m.n;
        if (AddIntegrity(t->msg, &t->len, sizeof t->msg, key_)) {
            t->authed = true;
            memcpy(lastAuth_, t->msg, t->len);
            lastAuthLen_ = t->len;
        }
    }
}

void TurnClient::Transmit(Txn* t) {
    SendRaw(t->msg, t->len);
    ++stats_.requests;
    const uint32_t wait = cfg_.rtoMs[t->sends < 8 ? t->sends : 7];
    ++t->sends;
    t->nextMs = Now() + (wait ? wait : 1600);
}

void TurnClient::Begin(Kind kind, int arg) {
    Txn* t = Free();
    if (!t) return;
    memset(t, 0, sizeof *t);
    t->active = true;
    t->kind = uint8_t(kind);
    t->arg = arg;
    Rebuild(t);
    Transmit(t);
}

void TurnClient::StartAlloc() {
    state_ = ALLOCATING;
    for (int i = 0; i < 8; ++i)
        if (txns_[i].active && txns_[i].kind != TK_RELEASE) txns_[i].active = false;
    Begin(TK_ALLOC, 0);
}

void TurnClient::SlotsReset() {
    for (int i = 0; i < kMaxSlots; ++i) {
        Slot& s = slots_[i];
        s.permOk = s.bindOk = s.permBusy = s.bindBusy = false;
        s.permAt = s.bindAt = s.retryAt = 0;
    }
}

uint16_t TurnClient::BindPeer(int slot, const Addr& peerRelayed) {
    if (slot < 0 || slot >= kMaxSlots || peerRelayed.IsZero()) return 0;
    Slot& s = slots_[slot];
    if (s.used && s.peer == peerRelayed) return s.channel;
    // another address needs another channel: a channel number stays bound to its first address for ten minutes (RFC 8656 12.2)
    s.used = true;
    s.peer = peerRelayed;
    s.channel = nextChannel_++;
    if (nextChannel_ > 0x7FFE) nextChannel_ = 0x4000;
    s.permOk = s.bindOk = s.permBusy = s.bindBusy = false;
    s.permAt = s.bindAt = s.retryAt = 0;
    NPN_LOG("turn: slot %d -> peer relay %s on channel 0x%04x\n", slot, AddrLog(peerRelayed), unsigned(s.channel));
    return s.channel;
}
bool TurnClient::ChannelBound(int slot) const { return slot >= 0 && slot < kMaxSlots && slots_[slot].used && slots_[slot].bindOk && state_ == READY; }
uint16_t TurnClient::ChannelOf(int slot) const { return slot >= 0 && slot < kMaxSlots && slots_[slot].used ? slots_[slot].channel : 0; }
int TurnClient::SlotForChannel(uint16_t ch) const {
    for (int i = 0; i < kMaxSlots; ++i)
        if (slots_[i].used && slots_[i].channel == ch) return i;
    return -1;
}

void TurnClient::Release() {
    if (released_ || state_ != READY) return;
    released_ = true;
    if (!haveNonce_) return;
    uint8_t txid[12];
    if (!env_->Random(txid, 12)) return;   // (leaving: nothing to say without a transaction id)
    uint8_t buf[480];
    Msg m(buf, sizeof buf, kRefresh, txid);
    m.AttrU32(aLifetime, 0);
    m.AttrStr(aUsername, user_);
    m.AttrStr(aRealm, realm_);
    m.AttrStr(aNonce, nonce_);
    size_t len = m.n;
    if (!AddIntegrity(buf, &len, sizeof buf, key_)) return;
    for (int i = 0; i < 2; ++i) SendRaw(buf, len);   // twice: best effort over UDP, nobody waits for the answer
    NPN_LOG("turn: allocation released\n");
    state_ = IDLE;
    relayed_ = Addr();
}

void TurnClient::LostAllocation(const char* why) {
    NPN_LOG("turn: allocation lost (%s): allocating again\n", why);
    relayed_ = Addr();
    refreshFails_ = 0;
    SlotsReset();
    StartAlloc();
}

void TurnClient::NextServerOrFail(const char* why) {
    if (serverIdx_ + 1 < nServers_ && !wasReady_) {
        ++serverIdx_;
        haveNonce_ = false;
        NPN_LOG("turn: %s: trying the next relay address %s\n", why, AddrLog(servers_[serverIdx_]));
        StartAlloc();
        return;
    }
    Fail("The relay server did not answer.");
}

void TurnClient::TxnTimeout(Txn* t) {
    const Kind k = Kind(t->kind);
    t->active = false;
    switch (k) {
    case TK_ALLOC:
        if (wasReady_) Fail("The relay server did not answer.");
        else NextServerOrFail("no answer to Allocate");
        break;
    case TK_REFRESH:
        if (++refreshFails_ >= 2) LostAllocation("no answer to Refresh");
        else refreshAt_ = Now() + 1000;
        break;
    case TK_RELEASE:
        if (reallocAfterRelease_) { reallocAfterRelease_ = false; StartAlloc(); }
        break;
    case TK_PERM:
        if (StaleSlotTxn(t)) break;
        slots_[t->arg].permBusy = false;
        slots_[t->arg].retryAt = Now() + 2000;
        break;
    case TK_BIND:
        if (StaleSlotTxn(t)) break;
        slots_[t->arg].bindBusy = false;
        slots_[t->arg].retryAt = Now() + 2000;
        break;
    }
}

void TurnClient::Tick() {
    if (!env_ || (state_ != ALLOCATING && state_ != READY)) return;
    if (randomFailed_) { Fail(RandomFailureSentence()); return; }
    const uint64_t now = Now();
    if (allocRetryAt_ && now >= allocRetryAt_) {
        allocRetryAt_ = 0;
        StartAlloc();
    }
    for (int i = 0; i < 8; ++i) {
        Txn* t = &txns_[i];
        if (!t->active || now < t->nextMs) continue;
        int limit = 0;
        while (limit < 8 && cfg_.rtoMs[limit]) ++limit;
        if (t->sends >= (limit ? limit : 1)) {
            TxnTimeout(t);
            if (state_ != ALLOCATING && state_ != READY) return;
            continue;
        }
        ++stats_.retransmits;
        // a retransmission carries the same transaction id and bytes
        Transmit(t);
    }
    if (state_ != READY) return;
    if (now >= refreshAt_) {
        bool busy = false;
        for (int i = 0; i < 8; ++i)
            if (txns_[i].active && (txns_[i].kind == TK_REFRESH || txns_[i].kind == TK_ALLOC)) busy = true;
        if (!busy) {
            Begin(TK_REFRESH, 0);
            refreshAt_ = now + 600000;   // the answer sets the next time
        }
    }
    for (int i = 0; i < kMaxSlots; ++i) {
        Slot& s = slots_[i];
        if (!s.used || now < s.retryAt) continue;
        if (!s.permBusy && (!s.permOk || now >= s.permAt)) {
            s.permBusy = true;
            Begin(TK_PERM, i);
        }
        if (!s.bindBusy && (!s.bindOk || now >= s.bindAt)) {
            s.bindBusy = true;
            Begin(TK_BIND, i);
        }
    }
}

int TurnClient::HandlePacket(const Addr& from, const uint8_t* d, size_t n, const uint8_t** inner, size_t* innerLen, Addr* peer) {
    if (!env_ || (state_ != ALLOCATING && state_ != READY) || nServers_ == 0 || from != servers_[serverIdx_]) return 0;
    if (n < 20 || (d[0] & 0xC0) != 0 || Get32(d + 4) != kCookie) return 0;
    Parsed p;
    memset(&p, 0, sizeof p);
    p.type = uint16_t(Get16(d));
    memcpy(p.txid, d + 8, 12);
    struct S {
        static bool Sink(void* o, uint16_t t, const uint8_t* v, size_t l) {
            Parsed* q = static_cast<Parsed*>(o);
            switch (t) {
            case aRealm: q->hasRealm = true; CopyStr(q->realm, sizeof q->realm, reinterpret_cast<const char*>(v), l); break;
            case aNonce: q->hasNonce = true; CopyStr(q->nonce, sizeof q->nonce, reinterpret_cast<const char*>(v), l); break;
            case aErrorCode: if (l >= 4) q->errCode = int(v[2] & 7) * 100 + int(v[3]); break;
            case aLifetime: if (l >= 4) { q->hasLifetime = true; q->lifetime = Get32(v); } break;
            case aRelayed: q->hasRelayed = DecodeXorAddr(v, l, &q->relayed); break;
            case aXorMapped: q->hasMapped = DecodeXorAddr(v, l, &q->mapped); break;
            case aPeer: q->hasPeer = DecodeXorAddr(v, l, &q->peer); break;
            case aData: q->hasData = true; q->data = v; q->dataLen = l; break;
            default: break;
            }
            return true;
        }
    };
    if (!ParseMsg(d, n, &p, S::Sink)) return 0;
    if (p.type == kDataInd) {
        if (!p.hasData || !p.hasPeer) return 1;
        if (p.dataLen > kMaxDatagram) return 1;   // no datagram of ours is larger: dropped here, whatever the bytes are
        ++stats_.dataIndications;
        *inner = p.data;
        *innerLen = p.dataLen;
        *peer = p.peer;
        return 2;
    }
    Txn* t = Find(p.txid);
    if (!t) return 0;
    const uint16_t cls = p.type & 0x0110;
    if (cls == 0x0100) OnSuccess(t, p);
    else if (cls == 0x0110) OnError(t, p);
    else return 0;
    return 1;
}

void TurnClient::OnSuccess(Txn* t, const Parsed& p) {
    ++stats_.successes;
    const uint64_t now = Now();
    const Kind k = Kind(t->kind);
    t->active = false;
    switch (k) {
    case TK_ALLOC:
        if (!p.hasRelayed) { Fail("The relay server's answer was not understood."); return; }
        relayed_ = p.relayed;
        if (p.hasMapped) mapped_ = p.mapped;
        lifetime_ = p.hasLifetime ? p.lifetime : cfg_.lifetimeSec;
        if (lifetime_ < 10) lifetime_ = 10;           // (a hostile or broken answer must not make us refresh in a tight loop)
        if (lifetime_ > 86400) lifetime_ = 86400;
        expiresAt_ = now + uint64_t(lifetime_) * 1000u;
        refreshAt_ = now + uint64_t(lifetime_) * 10u * cfg_.refreshPct;   // lifetime s * 1000 * pct / 100
        if (lifetime_ > 20 && refreshAt_ > now + uint64_t(lifetime_ - 10) * 1000u) refreshAt_ = now + uint64_t(lifetime_ - 10) * 1000u;
        state_ = READY;
        wasReady_ = true;
        orphanSince_ = 0;
        ++generation_;
        ++stats_.allocations;
        refreshFails_ = 0;
        SlotsReset();
        NPN_LOG("turn: allocated %s (our public address as the relay sees it: %s), lifetime %u s\n", AddrLog(relayed_), AddrLog(mapped_), unsigned(lifetime_));
        break;
    case TK_REFRESH:
        if (p.hasLifetime) lifetime_ = p.lifetime < 10 ? 10 : (p.lifetime > 86400 ? 86400 : p.lifetime);
        expiresAt_ = now + uint64_t(lifetime_) * 1000u;
        refreshAt_ = now + uint64_t(lifetime_ ? lifetime_ : cfg_.lifetimeSec) * 10u * cfg_.refreshPct;
        refreshFails_ = 0;
        ++stats_.refreshes;
        break;
    case TK_RELEASE:
        if (reallocAfterRelease_) { reallocAfterRelease_ = false; StartAlloc(); }
        break;
    case TK_PERM:
        if (StaleSlotTxn(t)) break;
        slots_[t->arg].permBusy = false;
        slots_[t->arg].permOk = true;
        slots_[t->arg].permAt = now + cfg_.permissionRefreshMs;
        ++stats_.permissions;
        break;
    case TK_BIND:
        if (StaleSlotTxn(t)) break;
        slots_[t->arg].bindBusy = false;
        slots_[t->arg].bindOk = true;
        slots_[t->arg].bindAt = now + cfg_.channelRefreshMs;
        ++stats_.bindings;
        break;
    }
}

void TurnClient::OnError(Txn* t, const Parsed& p) {
    ++stats_.errors;
    const Kind k = Kind(t->kind);
    const int code = p.errCode;
    NPN_LOG("turn: request kind %d refused: %d\n", int(k), code);
    if (StaleSlotTxn(t)) { t->active = false; return; }
    // 401 with a nonce (the first Allocate, or a stale nonce on servers that answer 401 instead of 438), 438 stale nonce: take the new
    // realm / nonce and send the request again
    if ((code == 401 || code == 438) && p.hasNonce && p.hasRealm) {
        const bool changed = !haveNonce_ || strcmp(nonce_, p.nonce) != 0 || strcmp(realm_, p.realm) != 0;
        if (!t->authed || (changed && t->authRetries < 3)) {
            CopyStr(realm_, sizeof realm_, p.realm);
            CopyStr(nonce_, sizeof nonce_, p.nonce);
            haveNonce_ = true;
            LongTermKey(key_, user_, realm_, pass_);
            const int tries = t->authRetries + 1;
            const int arg = t->arg;
            const Kind kind = k;
            t->active = false;
            Txn* n = Free();
            if (!n) return;
            memset(n, 0, sizeof *n);
            n->active = true;
            n->kind = uint8_t(kind);
            n->arg = arg;
            n->authRetries = tries;
            Rebuild(n);
            Transmit(n);
            return;
        }
    }
    t->active = false;
    switch (code) {
    case 401:
    case 441:
        if (code == 441 && k == TK_RELEASE && reallocAfterRelease_) {
            // An allocation made with credentials that have since been replaced still holds this socket's address pair and only its own user may delete it:
            // it ends by itself when its lifetime runs out. Ask again every few seconds.
            const uint64_t now = Now();
            if (!orphanSince_) orphanSince_ = now;
            if (SinceMs(now, orphanSince_) > 300000) {
                Fail("The relay still holds your earlier connection. Try again in a few minutes.");
                return;
            }
            reallocAfterRelease_ = false;
            allocRetryAt_ = now + 3000;
            NPN_LOG("turn: an allocation of the earlier credentials is still there: trying again in 3 s\n");
            return;
        }
        if (wasReady_ && k != TK_ALLOC) {
            credsRejected_ = true;
            NPN_LOG("turn: the credentials were refused after the allocation had worked: they ran out\n");
            return;
        }
        authRefused_ = true;
        Fail("The relay refused the sign-in. Try again in a moment.");
        return;
    case 437:   // allocation mismatch (an old allocation of this socket is still there) or an allocation that is gone
        if (k == TK_ALLOC) {
            reallocAfterRelease_ = true;
            state_ = ALLOCATING;
            Begin(TK_RELEASE, 0);
        } else {
            LostAllocation("437");
        }
        return;
    case 486:
    case 508:
        Fail("The relay has no room for another player right now. Try again later.");
        return;
    case 400:
        if (k == TK_BIND) {   // the channel number is taken by another address: use another one
            Slot& s = slots_[t->arg];
            s.bindBusy = false;
            s.channel = nextChannel_++;
            if (nextChannel_ > 0x7FFE) nextChannel_ = 0x4000;
            s.retryAt = Now() + 200;
        } else {
            Fail("The relay server refused a request.");
        }
        return;
    case 403:
        if (k == TK_PERM || k == TK_BIND) {
            Slot& s = slots_[t->arg];
            s.permBusy = s.bindBusy = false;
            s.retryAt = Now() + 5000;
            return;
        }
        Fail("The relay server refused the request (not allowed).");
        return;
    default:
        break;
    }
    if (k == TK_ALLOC) {
        char s[96];
        snprintf(s, sizeof s, "The relay server refused to set up the connection (error %d).", code);
        Fail(s);
    } else if (k == TK_PERM || k == TK_BIND) {
        Slot& s = slots_[t->arg];
        s.permBusy = s.bindBusy = false;
        s.retryAt = Now() + 3000;
    }
}

size_t TurnClient::LastAuthRequest(uint8_t* out, size_t cap, char* realm, char* user, size_t strCap) const {
    if (!lastAuthLen_ || lastAuthLen_ > cap) return 0;
    memcpy(out, lastAuth_, lastAuthLen_);
    CopyStr(realm, strCap, realm_);
    CopyStr(user, strCap, user_);
    return lastAuthLen_;
}

}  // namespace NpNet
