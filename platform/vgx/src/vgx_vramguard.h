// vram30: GXV_VRAM_GUARD, the pure logic (tests/vgx_vramguard.cpp). A diagnostic for the console, default off.
//
// Why: the Strikers port crashed at every boot of a build on the console and never in the emulator: video memory (vitaGL's CDRAM mspace,
// whose free-list headers live INSIDE the memory it hands out) was freed while the GPU still drew into it; the pixels (0xFF000000)
// overwrote the headers and the next allocation followed a pixel as a pointer. The emulator keeps no allocator data in guest video memory.
//
// What: every free of a texture, framebuffer object or renderbuffer name that the back end makes goes through one funnel
// (vgx_backend_gl.cpp: VramFreeTex / VramFreeFbo / VramFreeRb). With the guard on, the funnel does not call vitaGL; it queues the name here.
// A queued name is given to vitaGL only when
//   (a) `defer` presents have gone by since it was queued (default 3), AND
//   (b) the GPU has been finished since (sceGxmFinish, once per present at most, for all the names that are due), AND
//   (c) nothing the back end still holds refers to it (the caller's `refs(kind, name)` says what does, or null).
// A name that is still referenced is logged once per site as "vram-guard: FREE WHILE REFERENCED <what> by <who>" and postponed by `defer`
// presents again (a name postponed `giveUp` times is released anyway, logged once, so a leak cannot starve the console of memory).
// A name queued twice, or freed again after it was freed (a framebuffer object: vitaGL's glDeleteFramebuffers does not clear the object's
// depth-buffer pointer, so a second delete frees that depth buffer a second time), is counted and logged once per site as "DOUBLE FREE";
// the second request is dropped.
//
// Pure: no GL, no clock, no log of its own (callbacks), so the queue and the reference check are tested on the host.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

namespace vgx {
namespace vramguard {

enum Kind : uint8_t { kTex = 0, kFbo = 1, kRb = 2, kKinds = 3 };
inline const char* KindName(unsigned k) {
    static const char* const n[kKinds] = {"texture", "framebuffer object", "renderbuffer"};
    return k < kKinds ? n[k] : "?";
}

struct Item {
    uint8_t kind = kTex;
    uint32_t name = 0;
    uint32_t kib = 0;       // video memory the object is estimated to hold (texture: w x h x 4 when known); 0: unknown
    uint64_t dueAt = 0;     // the present at which it may be given to vitaGL
    uint64_t queuedAt = 0;
    uint16_t site = 0;      // interned site name
    uint16_t postponed = 0; // times a reference held it back
};

struct Stats {
    uint64_t requested[kKinds] = {};
    uint64_t released[kKinds] = {};   // given to vitaGL
    uint64_t postponed = 0;           // events: a due name held back by a reference
    uint64_t doubleRequests = 0;      // a name queued twice, or deleted again after it was deleted
    uint64_t gaveUp = 0;              // names released after giveUp postponements
    uint64_t finishes = 0;            // GPU finishes asked for
    uint64_t finishFailed = 0;        // a finish that did not complete (a scene open): the due names waited
    double finishMs = 0;
    uint64_t backlogKiBMax = 0;       // the largest backlog seen (KiB)
    unsigned backlogMax = 0;          // the largest backlog seen (names)
    uint64_t sinceCensusReq = 0, sinceCensusRel = 0, sinceCensusPost = 0; // the 5-second window
};

class Guard {
public:
    // defer: presents a name waits (>= 0); giveUp: postponements after which a still-referenced name is released (0: never)
    void Configure(unsigned defer, unsigned giveUp) {
        defer_ = defer;
        giveUp_ = giveUp;
    }
    unsigned Defer() const { return defer_; }

    // A site name is interned once (the pointer must outlive the guard: a string literal).
    uint16_t Site(const char* s) {
        for (size_t i = 0; i < sites_.size(); ++i)
            if (sites_[i].name == s || (sites_[i].name && s && std::strcmp(sites_[i].name, s) == 0)) return static_cast<uint16_t>(i);
        sites_.push_back(SiteRec{s, false, false});
        return static_cast<uint16_t>(sites_.size() - 1);
    }
    const char* SiteName(uint16_t i) const { return i < sites_.size() ? sites_[i].name : "?"; }

    // A framebuffer object name just made by vitaGL: a later free is not a double free.
    void NoteGen(uint8_t kind, uint32_t name) {
        if (kind == kFbo) dead_.erase(name);
    }

    // The funnel. Returns true when the guard has taken the free (always: the caller must not call vitaGL).
    // `log` is told about a double free (once per site).
    template <class Log>
    bool Request(uint8_t kind, uint32_t name, uint32_t kib, const char* site, uint64_t present, Log&& log) {
        if (kind >= kKinds || !name) return true;
        ++st_.requested[kind];
        ++st_.sinceCensusReq;
        const uint16_t s = Site(site);
        for (const Item& q : q_)
            if (q.kind == kind && q.name == name) {
                ++st_.doubleRequests;
                if (!sites_[s].dblLogged) {
                    sites_[s].dblLogged = true;
                    char b[240];
                    std::snprintf(b, sizeof b, "vram-guard: DOUBLE FREE %s %u requested at %s while queued by %s: the second request is dropped", KindName(kind), name,
                                  SiteName(s), SiteName(q.site));
                    log(b);
                }
                return true;
            }
        if (kind == kFbo && dead_.count(name)) {
            ++st_.doubleRequests;
            if (!sites_[s].dblLogged) {
                sites_[s].dblLogged = true;
                char b[240];
                std::snprintf(b, sizeof b, "vram-guard: DOUBLE FREE %s %u requested at %s after it was freed (no glGenFramebuffers since): dropped (vitaGL would free its depth buffer a second time)",
                              KindName(kind), name, SiteName(s));
                log(b);
            }
            return true;
        }
        Item it;
        it.kind = kind;
        it.name = name;
        it.kib = kib;
        it.queuedAt = present;
        it.dueAt = present + defer_;
        it.site = s;
        q_.push_back(it);
        backlogKiB_ += kib;
        if (backlogKiB_ > st_.backlogKiBMax) st_.backlogKiBMax = backlogKiB_;
        if (q_.size() > st_.backlogMax) st_.backlogMax = static_cast<unsigned>(q_.size());
        return true;
    }

    // At a present (no scene open). refs(kind, name) -> const char* describing what still holds the name, or nullptr.
    // finish() -> bool: the GPU has been finished (called at most once per Drain, and only when a name is due and unreferenced).
    // release(kind, name): give it to vitaGL. log(const char*): one line.
    template <class Refs, class Finish, class Release, class Log>
    void Drain(uint64_t present, Refs&& refs, Finish&& finish, Release&& release, Log&& log) {
        if (q_.empty()) return;
        bool finishTried = false, finished = false;
        const size_t n = q_.size();
        std::deque<Item> keep;
        for (size_t i = 0; i < n; ++i) {
            Item it = q_.front();
            q_.pop_front();
            if (present < it.dueAt) { keep.push_back(it); continue; }
            const char* who = refs(it.kind, it.name);
            if (who && !(giveUp_ && it.postponed >= giveUp_)) {
                ++st_.postponed;
                ++st_.sinceCensusPost;
                ++it.postponed;
                SiteRec& sr = sites_[it.site];
                if (!sr.refLogged) {
                    sr.refLogged = true;
                    char b[300];
                    std::snprintf(b, sizeof b, "vram-guard: FREE WHILE REFERENCED %s %u (freed at %s, %u KiB) by %s", KindName(it.kind), it.name, SiteName(it.site), it.kib, who);
                    log(b);
                }
                it.dueAt = present + (defer_ ? defer_ : 1);
                keep.push_back(it);
                continue;
            }
            if (!finishTried) { // (a GPU finish only when something is really about to be given back)
                finishTried = true;
                finished = finish();
                ++st_.finishes;
                if (!finished) ++st_.finishFailed;
            }
            if (!finished) { keep.push_back(it); continue; }
            if (who) { // (given up: still referenced after giveUp postponements)
                ++st_.gaveUp;
                char b[300];
                std::snprintf(b, sizeof b, "vram-guard: GAVE UP waiting for %s %u (freed at %s) after %u postponements: still referenced by %s; released", KindName(it.kind), it.name,
                              SiteName(it.site), static_cast<unsigned>(it.postponed), who);
                log(b);
            }
            release(it.kind, it.name);
            ++st_.released[it.kind];
            ++st_.sinceCensusRel;
            if (it.kind == kFbo) dead_.insert(it.name);
            backlogKiB_ -= it.kib <= backlogKiB_ ? it.kib : backlogKiB_;
        }
        q_.swap(keep);
    }

    // Every 5 s: fills `out` with the census line; false when it is not time. nowMs: any monotonic clock.
    bool Census(double nowMs, char* out, size_t cap) {
        if (lastCensusMs_ == 0.0) { lastCensusMs_ = nowMs; return false; }
        if (nowMs - lastCensusMs_ < 5000.0) return false;
        const double win = (nowMs - lastCensusMs_) / 1000.0;
        lastCensusMs_ = nowMs;
        const uint64_t reqAll = st_.requested[kTex] + st_.requested[kFbo] + st_.requested[kRb];
        const uint64_t relAll = st_.released[kTex] + st_.released[kFbo] + st_.released[kRb];
        std::snprintf(out, cap,
                      "vram-guard: %.1f s: frees requested %u (all deferred), given to vitaGL %u, postponed for a reference %u; totals: requested %u (texture %u, fbo %u, renderbuffer %u), "
                      "given %u, postponed %u, double-free requests %u, gave up %u; waiting now %u (%u KiB), largest backlog %u KiB (%u names); GPU finishes %u (%u not completed, %.1f ms); "
                      "defer %u presents (GXV_VRAM_GUARD=1)",
                      win, U(st_.sinceCensusReq), U(st_.sinceCensusRel), U(st_.sinceCensusPost), U(reqAll), U(st_.requested[kTex]), U(st_.requested[kFbo]), U(st_.requested[kRb]),
                      U(relAll), U(st_.postponed), U(st_.doubleRequests), U(st_.gaveUp), static_cast<unsigned>(q_.size()), static_cast<unsigned>(backlogKiB_),
                      static_cast<unsigned>(st_.backlogKiBMax), st_.backlogMax, U(st_.finishes), U(st_.finishFailed), st_.finishMs, defer_);
        st_.sinceCensusReq = st_.sinceCensusRel = st_.sinceCensusPost = 0;
        return true;
    }

    void AddFinishMs(double ms) { st_.finishMs += ms; }
    const Stats& Counters() const { return st_; }
    size_t Waiting() const { return q_.size(); }
    uint64_t WaitingKiB() const { return backlogKiB_; }
    bool Has(uint8_t kind, uint32_t name) const {
        for (const Item& q : q_)
            if (q.kind == kind && q.name == name) return true;
        return false;
    }

private:
    static unsigned U(uint64_t v) { return static_cast<unsigned>(v); }
    struct SiteRec { const char* name; bool refLogged; bool dblLogged; };
    std::deque<Item> q_;
    std::vector<SiteRec> sites_;
    // names of framebuffer objects given back to vitaGL and not generated since (a sorted vector would do; the set is small)
    struct NameSet {
        std::vector<uint32_t> v;
        void insert(uint32_t n) { for (uint32_t x : v) if (x == n) return; v.push_back(n); }
        void erase(uint32_t n) { for (size_t i = 0; i < v.size(); ++i) if (v[i] == n) { v[i] = v.back(); v.pop_back(); return; } }
        bool count(uint32_t n) const { for (uint32_t x : v) if (x == n) return true; return false; }
    } dead_;
    Stats st_;
    uint64_t backlogKiB_ = 0;
    unsigned defer_ = 3, giveUp_ = 600;
    double lastCensusMs_ = 0.0;
};

} // namespace vramguard
} // namespace vgx
