// VLink core: sockets, frames, peer address learning, impairment. See vlink.h.
// Ported from the recompiled port's MscLink (port/runtime/src/hle/net/msc_link.cpp, branch mp-one) to an
// instance-based, lock-protected form with an injectable clock; the frame format is unchanged.
#include "vlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <deque>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <sched.h>
#endif

#include "np_log.h"

// netfix B8a: the Vita's C library mishandles 64-bit length modifiers (%llu / %lld): 64-bit counters are logged as %u through this clamp
#ifndef NETFIX_U32C
#define NETFIX_U32C(v) ((unsigned)((uint64_t)(v) > 0xFFFFFFFFull ? 0xFFFFFFFFull : (uint64_t)(v)))
#endif
#define VLINK_LOG(...) NpLogf("vlink", __VA_ARGS__)   // Galaxy: also the Vita log.txt (np_log.h)
#define VLINK_TRACE(...) fprintf(stderr, "[vlink] " __VA_ARGS__)   // per packet (SMG_LINK_TRACE): stderr only

namespace VLink {
namespace {

// Frame (big-endian), 24 bytes then the payload:
//   0 magic "SMGK" (Galaxy; Strikers: "MSLK")  4 version  5 rule byte (informational, see Link::SetRules)  6 payload length (u16)  8 fingerprint  12 src vip  16 dst vip
//   20 src port (u16)  22 dst port (u16)
const uint32_t kMagic = 0x534D474Bu;   // "SMGK" (Galaxy; Strikers 0x4D534C4B "MSLK")
// Fragment (big-endian), kFragHeader = 12 bytes then a piece of a frame: 0 magic "SMGF" (Galaxy; Strikers: "MSLF")  4 id (u16)  6 index (u8)  7 count (u8)
//   8 offset of the piece in the frame (u16)  10 total frame length (u16). Only frames longer than Backend::maxFrame are split.
const uint32_t kFragMagic = 0x534D4746u;   // "SMGF" (Galaxy; Strikers 0x4D534C46 "MSLF")
const size_t kMaxFragments = 8;
const int kReasmSlots = 4;
const uint8_t kVersion = 1;
const size_t kHeader = 24;
const size_t kMaxFrame = kHeader + kMaxPayload;
const size_t kQueueLimit = 128;  // datagrams per socket; the game drains every frame
const int kAddrSlots = 16;

void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

uint64_t SteadyNowMs(void*) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000u + uint64_t(ts.tv_nsec / 1000000);
}

uint64_t SteadyNowUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000u + uint64_t(ts.tv_nsec / 1000);
}

// The critical sections are a few dozen instructions, contention is rare (the network update task and a
// startup thread): a spin lock avoids a dependency on the platform's threading library.
struct Lock {
    volatile int flag;
    Lock() : flag(0) {}
    void Acquire() {
        while (__atomic_test_and_set(&flag, __ATOMIC_ACQUIRE)) {
#if defined(__linux__) || defined(__APPLE__)
            sched_yield();
#endif
        }
    }
    void Release() { __atomic_clear(&flag, __ATOMIC_RELEASE); }
};
struct Guard {
    Lock& l;
    explicit Guard(Lock& lock) : l(lock) { l.Acquire(); }
    ~Guard() { l.Release(); }
};

struct Datagram {
    uint32_t srcVip;
    uint16_t srcPort;
    PeerAddr from;   // core2: the real source (the queue's fairness counts by it)
    std::vector<uint8_t> data;
};

struct Socket {
    bool open;
    uint16_t port;
    std::deque<Datagram> queue;
    Socket() : open(false), port(0) {}
};

// core2 (security review findings 1, 2): a vip is pinned to the real address it was first learnt at. A frame with that vip from
// another address does not move it: it is delivered under an alias vip (AliasVip) that maps to the new address, so the session layer
// sees a new address; it moves a member there only when what came is sealed with that member's key (NAT rebinding), and a forged
// frame fails to open. "wire" is the vip written in frames to that peer (the alias's real vip). One real address holds at most
// kVipsPerAddr entries, and the table evicts the entry heard from least recently, so frames with invented vips cannot push members out.
struct AddrEntry {
    bool used;
    uint32_t vip, wire;
    PeerAddr addr;
    uint64_t lastMs;
    AddrEntry() : used(false), vip(0), wire(0), lastMs(0) { memset(&addr, 0, sizeof addr); }
};
const int kVipsPerAddr = 2;

struct Reasm {
    bool used;
    PeerAddr from;
    uint16_t id, total;
    uint8_t count;
    uint32_t mask;
    uint64_t startedMs;
    uint16_t offset[kMaxFragments], length[kMaxFragments];   // where each piece sits (checked for a gap-free, overlap-free frame on completion)
    uint8_t data[kMaxFrame];
    Reasm() : used(false), id(0), total(0), count(0), mask(0), startedMs(0) {
        memset(&from, 0, sizeof from);
        memset(offset, 0, sizeof offset);
        memset(length, 0, sizeof length);
    }
};

struct Delayed {
    uint64_t dueMs;
    bool broadcast, hasTo;
    PeerAddr to;
    std::vector<uint8_t> frame;
};

}  // namespace

struct LinkImpl {
    mutable Lock lock;
    const Backend* backend;
    bool up;
    uint32_t vip, fingerprint;
    Socket sockets[kMaxSockets];
    AddrEntry addrs[kAddrSlots];
    int nextAddr;
    uint16_t nextEphemeral;
    Stats stats;
    Impairment impairment;
    uint32_t rng;
    std::deque<Delayed> delayed;
    Reasm* reasm;  // kReasmSlots, allocated on first use (the frame buffers are large)
    uint16_t nextFragId;
    uint64_t (*nowMs)(void*);
    void* nowCtx;
    int trace;
    WindowStats window;
    bool lost;
    uint8_t rules;           // our rule byte (Link::SetRules), carried in byte 5 of every frame header
    Incompatible incompatible;   // the last frame refused for a different fingerprint (Link::GetIncompatible)
    uint64_t incompatibleLogMs;

    LinkImpl()
        : backend(0), up(false), vip(0), fingerprint(0), nextAddr(0), nextEphemeral(49152), rng(12345u), reasm(0), nextFragId(1),
          nowMs(SteadyNowMs), nowCtx(0), trace(0), lost(false), rules(0), incompatibleLogMs(0) {
        memset(&incompatible, 0, sizeof incompatible);
        memset(&stats, 0, sizeof stats);
        memset(&impairment, 0, sizeof impairment);
        memset(&window, 0, sizeof window);
    }

    // Every send of the backend goes through here: timed (wave 5 NET instrumentation).
    int BackendSend(const PeerAddr* to, bool broadcast, const void* data, size_t len) {
        const uint64_t t0 = SteadyNowUs();
        const int rc = backend->send(backend->ctx, to, broadcast, data, len);
        const uint64_t us = SteadyNowUs() - t0;
        const uint32_t us32 = us > 0xFFFFFFFFu ? 0xFFFFFFFFu : uint32_t(us);
        ++stats.backendSends;
        stats.sendUsTotal += us;
        if (us32 > stats.sendUsMax) stats.sendUsMax = us32;
        if (us32 > window.sendUsMax) window.sendUsMax = us32;
        return rc;
    }
    ~LinkImpl() { delete[] reasm; }

    bool Tracing() {
        if (trace <= 0) return false;
        --trace;
        return true;
    }
    double Uniform() {  // xorshift32, deterministic per console
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return double(rng) / 4294967296.0;
    }
    bool Impaired() const { return impairment.drop > 0 || impairment.dup > 0 || impairment.delayMs || impairment.jitterMs; }

    Socket* Get(int handle) {
        if (handle < 1 || handle > kMaxSockets) return 0;
        Socket& s = sockets[handle - 1];
        return s.open ? &s : 0;
    }
    const AddrEntry* Find(uint32_t v) const {
        for (int i = 0; i < kAddrSlots; ++i)
            if (addrs[i].used && addrs[i].vip == v) return &addrs[i];
        return 0;
    }
    const PeerAddr* Lookup(uint32_t v) const {
        const AddrEntry* e = Find(v);
        return e ? &e->addr : 0;
    }
    uint32_t WireVip(uint32_t v) const {
        const AddrEntry* e = Find(v);
        return e ? e->wire : v;
    }
    // the alias of (vip, real address): 127.x.y.z (never a console's vip; never the broadcast vip)
    static uint32_t AliasVip(uint32_t v, const PeerAddr& addr) {
        uint32_t h = 2166136261u ^ v;
        for (size_t i = 0; i < sizeof addr.bytes; ++i) h = (h ^ addr.bytes[i]) * 16777619u;
        h &= 0x00FFFFFFu;
        if (!h) h = 1;
        return 0x7F000000u | h;
    }
    // Returns the vip the frame is delivered under (v, or its alias at this address).
    uint32_t Learn(uint32_t v, const PeerAddr& addr, uint64_t now) {
        uint32_t key = v;
        for (int i = 0; i < kAddrSlots; ++i) {
            AddrEntry& e = addrs[i];
            if (!e.used || e.vip != v) continue;
            if (!memcmp(&e.addr, &addr, sizeof addr)) {
                e.lastMs = now;
                return v;
            }
            key = AliasVip(v, addr);   // pinned: another address gets the alias
            break;
        }
        if (key != v)
            for (int i = 0; i < kAddrSlots; ++i) {
                AddrEntry& e = addrs[i];
                if (e.used && e.vip == key && !memcmp(&e.addr, &addr, sizeof addr)) {
                    e.lastMs = now;
                    return key;
                }
            }
        // a new entry: the oldest of this address's own when it has kVipsPerAddr already; else a free one; else the least recent
        int pick = -1, sameAddr = 0, oldestSame = -1, oldest = -1;
        for (int i = 0; i < kAddrSlots; ++i) {
            const AddrEntry& e = addrs[i];
            if (!e.used) {
                if (pick < 0) pick = i;
                continue;
            }
            if (!memcmp(&e.addr, &addr, sizeof addr)) {
                ++sameAddr;
                if (oldestSame < 0 || e.lastMs < addrs[oldestSame].lastMs) oldestSame = i;
            }
            if (oldest < 0 || e.lastMs < addrs[oldest].lastMs) oldest = i;
        }
        if (sameAddr >= kVipsPerAddr) pick = oldestSame;
        else if (pick < 0) pick = oldest;
        AddrEntry& slot = addrs[pick];
        slot.used = true;
        slot.vip = key;
        slot.wire = v;
        slot.addr = addr;
        slot.lastMs = now;
        VLINK_LOG("learned peer %u.%u.%u.%u%s\n", v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255, key != v ? " (at another address: alias)" : "");
        return key;
    }
    Socket* FindBound(uint16_t port) {
        for (int i = 0; i < kMaxSockets; ++i)
            if (sockets[i].open && sockets[i].port == port) return &sockets[i];
        return 0;
    }

    void Deliver(const uint8_t* frame, size_t len, const PeerAddr& from) {
        if (len < kHeader || Get32(frame) != kMagic || frame[4] != kVersion) {
            ++stats.droppedMalformed;
            return;
        }
        const uint16_t payload = Get16(frame + 6);
        if (payload > kMaxPayload || kHeader + payload != len) {
            ++stats.droppedMalformed;
            return;
        }
        const uint32_t src = Get32(frame + 12), dst = Get32(frame + 16);
        if (Get32(frame + 8) != fingerprint) {
            // Fail closed (STR-146): a console with another build, protocol or rule set never reaches a socket. The refusal is recorded so the
            // game can tell the player which kind of mismatch it is (Link::GetIncompatible) and logged at most once per peer and 5 seconds.
            ++stats.droppedFingerprint;
            if (src != vip && src != 0 && src != kBroadcastVip) {
                const uint64_t now = nowMs(nowCtx);
                const bool fresh = incompatible.count == 0 || incompatible.fingerprint != Get32(frame + 8) || incompatible.srcVip != src;
                ++incompatible.count;
                incompatible.fingerprint = Get32(frame + 8);
                incompatible.rules = frame[5];
                incompatible.srcVip = src;
                incompatible.lastMs = now;
                if (fresh || now - incompatibleLogMs > 5000) {
                    incompatibleLogMs = now;
                    VLINK_LOG("refusing %u.%u.%u.%u: another build or rule set (its fingerprint %08X, rules %02X; ours %08X, rules %02X)\n",
                              src >> 24, (src >> 16) & 255, (src >> 8) & 255, src & 255, (unsigned)Get32(frame + 8), (unsigned)frame[5],
                              (unsigned)fingerprint, (unsigned)rules);
                }
            }
            return;
        }
        if (src == vip || src == 0 || src == kBroadcastVip) return;  // our own broadcast echo, or garbage
        if (dst != vip && dst != kBroadcastVip) {                    // addressed to another console
            ++stats.droppedAddress;
            return;
        }
        Socket* socket = FindBound(Get16(frame + 22));
        // core2: only a frame for a bound port teaches an address (frames to nowhere cannot fill the table)
        const uint32_t srcKey = socket ? Learn(src, from, nowMs(nowCtx)) : src;
        if (Tracing())
            VLINK_TRACE("rx %u.%u.%u.%u:%u -> %s:%u len %u%s\n", src >> 24, (src >> 16) & 255, (src >> 8) & 255, src & 255,
                      Get16(frame + 20), dst == kBroadcastVip ? "bcast" : "me", Get16(frame + 22), payload,
                      socket ? "" : " (no socket)");
        if (!socket) {
            ++stats.droppedNoSocket;
            return;
        }
        if (socket->queue.size() >= kQueueLimit) {
            // core2 (claims audit 3): full: a source that holds an eighth of the queue loses its own oldest datagram; anyone else's
            // arrival pushes out the oldest (under a flood that is the flood's: a member is never the one starved)
            ++stats.droppedQueueFull;
            size_t drop = 0, own = 0, firstOwn = 0;
            for (size_t k = 0; k < socket->queue.size(); ++k)
                if (!memcmp(&socket->queue[k].from, &from, sizeof from) && own++ == 0) firstOwn = k;
            if (own >= kQueueLimit / 8) drop = firstOwn;   // (a source holding an eighth of the queue pays for itself)
            socket->queue.erase(socket->queue.begin() + long(drop));
        }
        Datagram d;
        d.srcVip = srcKey;
        d.from = from;
        d.srcPort = Get16(frame + 20);
        d.data.assign(frame + kHeader, frame + len);
        socket->queue.push_back(d);
        ++stats.framesReceived;
        stats.bytesReceived += payload;
        const uint32_t depth = uint32_t(socket->queue.size());
        if (depth > stats.queueHighWater) stats.queueHighWater = depth;
        if (depth > window.queueHighWater) window.queueHighWater = depth;
        if (payload > window.payloadReceivedMax) window.payloadReceivedMax = payload;
    }

    // The one place a frame leaves for the backend: whole when it fits the medium (Backend::maxFrame), else in fragments. Returns the frame
    // length, or <0 when the backend refused a piece.
    int SendFrame(const PeerAddr* to, bool broadcast, const uint8_t* frame, size_t len) {
        const size_t limit = backend->maxFrame;
        if (len > stats.maxBackendFrame) stats.maxBackendFrame = uint32_t(len > limit && limit ? limit : len);
        if (!limit || len <= limit) return BackendSend(to, broadcast, frame, len);
        if (limit <= kFragHeader) return -1;   // (outside review: before the division below, whose chunk would be 0 or wrap)
        const size_t chunk = limit - kFragHeader;
        const size_t count = (len + chunk - 1) / chunk;
        if (count > kMaxFragments) return -1;
        const uint16_t id = nextFragId++;
        if (nextFragId == 0) nextFragId = 1;
        ++stats.framesSplit;
        uint8_t piece[kMaxFrame];
        int result = int(len);
        for (size_t i = 0; i < count; ++i) {
            const size_t offset = i * chunk, n = (len - offset < chunk) ? len - offset : chunk;
            Put32(piece, kFragMagic);
            Put16(piece + 4, id);
            piece[6] = uint8_t(i);
            piece[7] = uint8_t(count);
            Put16(piece + 8, uint16_t(offset));
            Put16(piece + 10, uint16_t(len));
            memcpy(piece + kFragHeader, frame + offset, n);
            ++stats.fragmentsSent;
            if (BackendSend(to, broadcast, piece, kFragHeader + n) < 0) result = -1;
        }
        return result;
    }

    // Incomplete frames older than kReassemblyMs are dropped (counted in droppedFragments, once per frame).
    void ExpireFragments(uint64_t now) {
        if (!reasm) return;
        for (int i = 0; i < kReasmSlots; ++i)
            if (reasm[i].used && now - reasm[i].startedMs > kReassemblyMs) {
                reasm[i].used = false;
                ++stats.droppedFragments;
            }
    }

    // The pieces of a complete frame must tile it exactly: piece 0 at offset 0, every next piece where the previous ended, the last one ending at
    // `total`. Overlapping, gapped or reordered placements (a hostile or corrupt sender) would otherwise deliver bytes of an earlier frame.
    static bool Tiles(const Reasm& r) {
        size_t at = 0;
        for (unsigned i = 0; i < r.count; ++i) {
            if (r.offset[i] != at || r.length[i] == 0) return false;
            at += r.length[i];
        }
        return at == r.total;
    }

    // A fragment arrived: file it under (sender, id); when the frame is complete, deliver it as if it had come whole.
    void HandleFragment(const uint8_t* piece, size_t len, const PeerAddr& from) {
        if (len <= kFragHeader) { ++stats.droppedFragments; return; }
        const uint16_t id = Get16(piece + 4), offset = Get16(piece + 8), total = Get16(piece + 10);
        const unsigned index = piece[6], count = piece[7];
        const size_t n = len - kFragHeader;
        if (count < 2 || count > kMaxFragments || index >= count || total < kHeader || total > kMaxFrame || size_t(offset) + n > total) {
            ++stats.droppedFragments;
            return;
        }
        if (!reasm) reasm = new Reasm[kReasmSlots];
        const uint64_t now = nowMs(nowCtx);
        ExpireFragments(now);
        Reasm* slot = 0;
        for (int i = 0; i < kReasmSlots; ++i) {
            Reasm& r = reasm[i];
            if (r.used && r.id == id && memcmp(&r.from, &from, sizeof from) == 0) slot = &r;
        }
        if (!slot) {
            for (int i = 0; i < kReasmSlots && !slot; ++i)
                if (!reasm[i].used) slot = &reasm[i];
            if (!slot) {  // all busy
                // netfix B5c: one source cannot push a member's partial frame out: the victim is the oldest slot of the source that holds the
                // most of them (the sender itself on a tie); only when every source holds one and the sender holds none is the oldest evicted
                int mine = 0;
                for (int i = 0; i < kReasmSlots; ++i)
                    if (reasm[i].used && memcmp(&reasm[i].from, &from, sizeof from) == 0) ++mine;
                int victimCount = mine;
                const PeerAddr* victimFrom = mine ? &from : 0;
                for (int i = 0; i < kReasmSlots; ++i) {
                    int c = 0;
                    for (int k = 0; k < kReasmSlots; ++k)
                        if (reasm[k].used && memcmp(&reasm[k].from, &reasm[i].from, sizeof from) == 0) ++c;
                    if (c > victimCount) { victimCount = c; victimFrom = &reasm[i].from; }
                }
                for (int i = 0; i < kReasmSlots; ++i) {
                    if (victimFrom && memcmp(&reasm[i].from, victimFrom, sizeof from) != 0) continue;
                    if (!slot || reasm[i].startedMs < slot->startedMs) slot = &reasm[i];
                }
                ++stats.droppedFragments;
            }
            slot->used = true;
            slot->from = from;
            slot->id = id;
            slot->total = total;
            slot->count = uint8_t(count);
            slot->mask = 0;
            slot->startedMs = now;
        }
        if (slot->total != total || slot->count != count) { slot->used = false; ++stats.droppedFragments; return; }
        if (slot->mask & (1u << index)) return;   // a duplicate piece: the first copy stays (a copy with other content must not rewrite it)
        memcpy(slot->data + offset, piece + kFragHeader, n);
        slot->offset[index] = offset;
        slot->length[index] = uint16_t(n);
        slot->mask |= 1u << index;
        if (slot->mask != (1u << count) - 1) return;
        slot->used = false;
        if (!Tiles(*slot)) { ++stats.droppedFragments; return; }
        ++stats.framesReassembled;
        Deliver(slot->data, slot->total, from);
    }

    void ReleaseDelayed() {
        if (delayed.empty()) return;
        const uint64_t now = nowMs(nowCtx);
        // Jitter can reorder: release every due frame, in due-time order.
        for (;;) {
            size_t best = delayed.size();
            for (size_t i = 0; i < delayed.size(); ++i)
                if (delayed[i].dueMs <= now && (best == delayed.size() || delayed[i].dueMs < delayed[best].dueMs)) best = i;
            if (best == delayed.size()) break;
            Delayed d = delayed[best];
            delayed.erase(delayed.begin() + best);
            if (SendFrame(d.hasTo ? &d.to : 0, d.broadcast, &d.frame[0], d.frame.size()) < 0) ++stats.sendErrors;
        }
    }

    void PumpLocked() {
        if (!up) return;
        ReleaseDelayed();
        ExpireFragments(nowMs(nowCtx));   // a lost piece does not keep its slot (and its counter) until the next fragment arrives
        uint8_t frame[kMaxFrame + 64];
        for (int budget = 0; budget < 256; ++budget) {
            PeerAddr from;
            memset(&from, 0, sizeof from);
            const int n = backend->recv(backend->ctx, &from, frame, sizeof frame);
            if (n <= 0) break;
            if (size_t(n) >= 4 && Get32(frame) == kFragMagic) HandleFragment(frame, size_t(n), from);
            else Deliver(frame, size_t(n), from);
        }
    }
};

Link::Link() : m(new LinkImpl) {}
Link::~Link() {
    Stop();
    delete m;
}

void Link::SetBackend(const Backend* backend) { Guard g(m->lock); m->backend = backend; }
const Backend* Link::GetBackend() const { Guard g(m->lock); return m->backend; }
void Link::SetFingerprint(uint32_t fingerprint) { Guard g(m->lock); m->fingerprint = fingerprint; }
uint32_t Link::Fingerprint() const { Guard g(m->lock); return m->fingerprint; }
void Link::SetRules(uint8_t rules) { Guard g(m->lock); m->rules = rules; }
bool Link::GetIncompatible(Incompatible* out) const {
    Guard g(m->lock);
    if (out) *out = m->incompatible;
    return m->incompatible.count != 0;
}
void Link::SetClock(uint64_t (*nowMs)(void*), void* ctx) {
    Guard g(m->lock);
    m->nowMs = nowMs ? nowMs : SteadyNowMs;
    m->nowCtx = ctx;
}
void Link::SetImpairment(const Impairment& impairment, uint32_t seed) {
    Guard g(m->lock);
    m->impairment = impairment;
    if (seed) m->rng = seed;
    if (m->Impaired())
        VLINK_LOG("test impairment: drop %.1f%% dup %.1f%% delay %u ms jitter %u ms\n", impairment.drop * 100, impairment.dup * 100,
                  (unsigned)impairment.delayMs, (unsigned)impairment.jitterMs);
}
void Link::SetTrace(int frames) { Guard g(m->lock); m->trace = frames; }

bool Link::Start() {
    Guard g(m->lock);
    if (m->up) return true;
    if (!m->backend) {
        VLINK_LOG("no network backend available\n");
        return false;
    }
    m->vip = m->backend->local_vip(m->backend->ctx);
    if (m->vip == 0 || m->vip == kBroadcastVip) {
        VLINK_LOG("backend %s has no usable address\n", m->backend->name);
        return false;
    }
    for (int i = 0; i < kAddrSlots; ++i) m->addrs[i] = AddrEntry();
    for (int i = 0; i < kMaxSockets; ++i) m->sockets[i] = Socket();
    m->delayed.clear();
    if (m->reasm)
        for (int i = 0; i < kReasmSlots; ++i) m->reasm[i].used = false;
    memset(&m->stats, 0, sizeof m->stats);
    memset(&m->window, 0, sizeof m->window);
    memset(&m->incompatible, 0, sizeof m->incompatible);
    m->incompatibleLogMs = 0;
    __atomic_store_n(&m->lost, false, __ATOMIC_RELEASE);
    m->up = true;
    // Frames that waited in the backend while the link was down (the ad hoc radio's ring, the UDP socket's buffer) belong to an earlier session:
    // they are thrown away instead of reaching the new session's sockets (STR-146).
    {
        uint8_t stale[kMaxFrame + 64];
        PeerAddr from;
        for (int budget = 0; budget < 512; ++budget) {
            memset(&from, 0, sizeof from);
            if (m->backend->recv(m->backend->ctx, &from, stale, sizeof stale) <= 0) break;
            ++m->stats.droppedStale;
        }
    }
    VLINK_LOG("up on %s as %u.%u.%u.%u, fingerprint %08X\n", m->backend->name, m->vip >> 24, (m->vip >> 16) & 255,
              (m->vip >> 8) & 255, m->vip & 255, (unsigned)m->fingerprint);
    return true;
}

void Link::Stop() {
    Guard g(m->lock);
    if (!m->up) return;
    for (int i = 0; i < kMaxSockets; ++i) m->sockets[i] = Socket();
    m->delayed.clear();
    if (m->backend && m->backend->close) m->backend->close(m->backend->ctx);
    m->up = false;
    const Stats& s = m->stats;
    VLINK_LOG("down (sent %u frames, received %u, dropped fp=%u bad=%u nosock=%u full=%u, send errors %u; largest guest payload %u, "
              "largest frame to the backend %u, split %u into %u fragments, reassembled %u, fragments dropped %u)\n",
              NETFIX_U32C(s.framesSent), NETFIX_U32C(s.framesReceived), NETFIX_U32C(s.droppedFingerprint),
              NETFIX_U32C(s.droppedMalformed), NETFIX_U32C(s.droppedNoSocket), NETFIX_U32C(s.droppedQueueFull),
              NETFIX_U32C(s.sendErrors), (unsigned)s.maxPayloadSent, (unsigned)s.maxBackendFrame, NETFIX_U32C(s.framesSplit),
              NETFIX_U32C(s.fragmentsSent), NETFIX_U32C(s.framesReassembled), NETFIX_U32C(s.droppedFragments));
    VLINK_LOG("down: %u backend sends, %.1f us average, %u us the longest; most datagrams waiting in one socket %u; sends refused while lost %u\n",
              NETFIX_U32C(s.backendSends), s.backendSends ? double(s.sendUsTotal) / double(s.backendSends) : 0.0, (unsigned)s.sendUsMax,
              (unsigned)s.queueHighWater, NETFIX_U32C(s.sendsRefusedLost));
}

bool Link::IsUp() const { Guard g(m->lock); return m->up; }
uint32_t Link::LocalVip() const { Guard g(m->lock); return m->up ? m->vip : 0; }

int Link::OpenSocket() {
    Guard g(m->lock);
    if (!m->up) return -1;
    for (int i = 0; i < kMaxSockets; ++i) {
        if (!m->sockets[i].open) {
            m->sockets[i] = Socket();
            m->sockets[i].open = true;
            return i + 1;
        }
    }
    return -1;
}

bool Link::Bind(int handle, uint16_t port) {
    Guard g(m->lock);
    Socket* s = m->Get(handle);
    if (!s) return false;
    if (port == 0) {
        do {
            port = m->nextEphemeral++;
            if (m->nextEphemeral == 0) m->nextEphemeral = 49152;
        } while (m->FindBound(port));
    } else {
        Socket* other = m->FindBound(port);
        if (other && other != s) return false;
    }
    s->port = port;
    return true;
}

void Link::Close(int handle) {
    Guard g(m->lock);
    if (Socket* s = m->Get(handle)) *s = Socket();
}

bool Link::IsOpen(int handle) const { Guard g(m->lock); return m->Get(handle) != 0; }

uint16_t Link::LocalPort(int handle) const {
    Guard g(m->lock);
    const Socket* s = m->Get(handle);
    return s ? s->port : 0;
}

int Link::SendTo(int handle, uint32_t dstVip, uint16_t dstPort, const void* data, size_t len) {
    Guard g(m->lock);
    Socket* s = m->Get(handle);
    if (!s || !m->up || len > kMaxPayload || (len && !data)) return -1;
    if (__atomic_load_n(&m->lost, __ATOMIC_ACQUIRE)) {
        ++m->stats.sendsRefusedLost;
        return -1;
    }
    if (len > m->window.payloadSentMax) m->window.payloadSentMax = uint32_t(len);
    if (s->port == 0) {
        // implicit bind, as the BSD stack does on the first send
        uint16_t port;
        do {
            port = m->nextEphemeral++;
            if (m->nextEphemeral == 0) m->nextEphemeral = 49152;
        } while (m->FindBound(port));
        s->port = port;
    }
    uint8_t frame[kMaxFrame];
    Put32(frame, kMagic);
    frame[4] = kVersion;
    frame[5] = m->rules;
    Put16(frame + 6, uint16_t(len));
    Put32(frame + 8, m->fingerprint);
    Put32(frame + 12, m->vip);
    Put32(frame + 16, dstVip == kBroadcastVip ? dstVip : m->WireVip(dstVip));   // (core2: an alias goes out as the peer's own vip)
    Put16(frame + 20, s->port);
    Put16(frame + 22, dstPort);
    if (len) memcpy(frame + kHeader, data, len);
    const bool broadcast = dstVip == kBroadcastVip;
    const PeerAddr* to = broadcast ? 0 : m->Lookup(dstVip);
    if (m->Impaired()) {
        if (len > m->stats.maxPayloadSent) m->stats.maxPayloadSent = uint32_t(len);
        ++m->stats.framesSent;
        m->stats.bytesSent += len;
        if (m->Uniform() < m->impairment.drop) return int(len);  // lost in the air
        const int copies = m->Uniform() < m->impairment.dup ? 2 : 1;
        for (int c = 0; c < copies; ++c) {
            Delayed d;
            d.dueMs = m->nowMs(m->nowCtx) + m->impairment.delayMs + uint64_t(m->Uniform() * m->impairment.jitterMs);
            d.broadcast = broadcast || !to;
            d.hasTo = to != 0;
            memset(&d.to, 0, sizeof d.to);
            if (to) d.to = *to;
            d.frame.assign(frame, frame + kHeader + len);
            m->delayed.push_back(d);
        }
        m->PumpLocked();
        return int(len);
    }
    // Unknown unicast destinations go out as broadcast frames; only the addressed console keeps them.
    if (len > m->stats.maxPayloadSent) m->stats.maxPayloadSent = uint32_t(len);
    const int sent = m->SendFrame(to, broadcast || !to, frame, kHeader + len);
    if (m->Tracing())
        VLINK_TRACE("tx :%u -> %u.%u.%u.%u:%u len %u%s%s\n", s->port, dstVip >> 24, (dstVip >> 16) & 255, (dstVip >> 8) & 255,
                  dstVip & 255, dstPort, unsigned(len), (!broadcast && !to) ? " (unknown peer, broadcast)" : "",
                  sent < 0 ? " FAILED" : "");
    if (sent < 0) {
        ++m->stats.sendErrors;
        return -1;
    }
    ++m->stats.framesSent;
    m->stats.bytesSent += len;
    return int(len);
}

int Link::RecvFrom(int handle, void* buffer, size_t capacity, uint32_t* srcVip, uint16_t* srcPort) {
    Guard g(m->lock);
    Socket* s = m->Get(handle);
    if (!s) return -1;
    m->PumpLocked();
    if (s->queue.empty()) return -1;
    Datagram d = s->queue.front();
    s->queue.pop_front();
    const size_t n = d.data.size() < capacity ? d.data.size() : capacity;  // UDP truncates
    if (n) memcpy(buffer, &d.data[0], n);
    if (srcVip) *srcVip = d.srcVip;
    if (srcPort) *srcPort = d.srcPort;
    return int(n);
}

void Link::Pump() {
    Guard g(m->lock);
    m->PumpLocked();
}

void Link::ReleaseDue() {
    Guard g(m->lock);
    if (m->up) m->ReleaseDelayed();
}

Stats Link::GetStats() const {
    Guard g(m->lock);
    return m->stats;
}

WindowStats Link::TakeWindow() {
    Guard g(m->lock);
    const WindowStats w = m->window;
    memset(&m->window, 0, sizeof m->window);
    return w;
}

// No lock: the online backend's controller can find the link lost while it runs inside Backend::recv, i.e. under this link's lock
// (online/np_online.cpp), and the loss handler of np_lan.cpp ends up here. The flag is only read in SendTo, under the lock.
void Link::SetLost(bool lost, const char* why) {
    if (__atomic_exchange_n(&m->lost, lost, __ATOMIC_ACQ_REL) == lost) return;
    if (lost) VLINK_LOG("link declared lost (%s): every send fails from now on\n", why ? why : "no reason given");
}

bool Link::IsLost() const { return __atomic_load_n(&m->lost, __ATOMIC_ACQUIRE); }

Link& Default() {
    static Link link;
    return link;
}

}  // namespace VLink
