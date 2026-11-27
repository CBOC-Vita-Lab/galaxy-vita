// GXV_INDCAP_PEEK (peek25; default 0, 2 count only, 1 apply): the game's "Indirect" capture of picture N is left out when it is DEAD in the stream the render
// thread will really execute: (a) nothing after it in N reads the texture before N writes it again, and (b) the NEXT buffer, picture N+1, is already queued and
// writes the whole texture (a plain EFB copy that is not left out) before any command of it reads the texture. Then nothing can ever see N's capture: every
// command that would, in order, is either in N before it, in N after it (none, by (a)), or in N+1 before the overwrite (none, by (b)); a command that runs later sees
// N+1's copy or a later one. Exact by construction (same pixels), no guess and no latency.
//
// Where: the render thread (vgx_galaxy.cpp RenderMain), just before a buffer is executed. The front end cannot do this: it decides N before N+1 exists (that is why the
// existing rule keeps a capture for GXV_INDCAP_PREDICT / _STAGE / _WARMUP). A queued buffer is complete and untouched until the render thread has executed and cleared it
// (FrameSink swaps it in and pushes it under sMtx; only the render thread erases or clears it), so reading sJobs[1] (pointer taken under sMtx) is safe while buffer N runs.
//
// Induction: skipping N's capture leaves the texture holding an older picture's. Picture N+1 then either is executed with its own copy (its readers all come after it, by
// (b)), or its own copy is left out by the same rule, which needs N+2 queued, overwriting, with no earlier read there, and N+1 itself without a reader after it. A copy
// that is not left out (next not queued, a reader, no overwrite) is made as always, so the chain restarts from a real capture. Nothing is carried from one picture to
// the next except that the next buffer is looked at when it is the one that runs.
//
// Refused (kept, counted): the picture or the next is an HRW pair opener / replay ask / partial draw (hrwId, hrwReplayId, hrwFresh), the next is not queued, the capture
// is not the plain Indirect shape (a pass, an atlas cell, depth, a fill, the shadow / bracket / corner / fuse / stencil forms), or any of its readers or doubtful writers.
// Pure decision on two CmdBufs: host-tested (tests/vgx_indcap_peek_test.cpp, tools/test_vgx_indcap_peek.sh; tests/vgx_indcap_gl.cpp's two-frame queue run).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "vgx_cmd.h"

namespace vgx::indcappeek {

// 0 off (default), 1 apply, 2 count only. Read once.
inline int Mode() {
    static const int m = [] {
        const char* s = std::getenv("GXV_INDCAP_PEEK");
        const int v = s ? std::atoi(s) : 0;
        return v < 0 ? 0 : v > 2 ? 2 : v;
    }();
    return m;
}

struct Counters {
    uint64_t pictures = 0;       // ordinary buffers looked at
    uint64_t nextQueued = 0;     // of those: the next buffer was already queued
    uint64_t nextOrdinary = 0;   // of those: and it is an ordinary buffer the rule may read (no HRW pair opener, ask or partial draw)
    uint64_t captures = 0;       // plain Indirect captures not left out already
    uint64_t readerHere = 0;     // kept: a reader after it in its own picture
    uint64_t deadOwn = 0;        // dead in its own picture (written again / freed before any read): no next buffer needed
    uint64_t noNext = 0;         // no reader here, kept: the next buffer is not queued (or not ordinary)
    uint64_t nextReads = 0;      // no reader here, kept: the next buffer reads the texture before it writes it
    uint64_t nextNoWrite = 0;    // no reader here, kept: the next buffer does not write the whole texture (or a doubtful write)
    uint64_t viaNext = 0;        // dead because the next buffer overwrites it first
    uint64_t refusedShape = 0;   // marked copies of another shape, left alone
    uint64_t shadowChecks = 0;   // the next buffer, when it ran: its first command on the texture was a whole write ...
    uint64_t shadowBad = 0;      // ... or not: a read first (MUST stay 0)
};
struct Shadow {
    const CmdBuf* next = nullptr;
    uint64_t frame = 0;
    uint32_t tex[4] = {};
    uint8_t n = 0;
};
inline Counters g_win, g_tot;
inline Shadow g_shadow;

inline bool Plain(const CopyTexCmd& c) {
    return c.transient && !c.dry && !c.skip && !c.fromPass && !c.passMore && !c.srcTex && !c.dstTexW && !c.depth && !c.fill && c.texId != 0 && !c.bracket && c.fetchMode == 0 &&
           c.nzOn == 0 && c.cornerPass == 0 && c.fuse == 0 && c.stWin == 0 && c.depthMark == 0;
}
// Does a command read, or write the whole of, texture `id`?  R: a read (or a partial / doubtful write: keep). W: a whole write (a plain copy, an upload, a free). N: neither.
enum Ev : uint8_t { EV_NONE, EV_READ, EV_WRITE, EV_DOUBT };
inline Ev Event(const CmdBuf& b, const Cmd& x, uint32_t id) {
    switch (x.type) {
    case CMD_DRAW: {
        const DrawRec& d = b.draws[x.arg];
        for (int m = 0; m < 8; ++m)
            if ((d.texMask & (1u << m)) && d.tex[m].texId == id) return EV_READ;
        return EV_NONE;
    }
    case CMD_COPY_TEX: {
        const CopyTexCmd& y = b.copyTex[x.arg];
        if (y.srcTex == id) return EV_READ;
        if (y.texId != id || y.skip) return EV_NONE;
        if (y.dstTexW || y.nzOn) return EV_DOUBT; // (a cell of an atlas, or a copy of part of the texture: the rest is read back)
        return EV_WRITE;
    }
    case CMD_COPY_DISP: return b.copyDisp[x.arg].keepTex == id ? EV_DOUBT : EV_NONE; // (GXV_PREVCAP's keeper: not expected to be this texture; keep)
    case CMD_TEX_FREE: return x.arg == id ? EV_WRITE : EV_NONE;
    case CMD_TEX_UPLOAD: return b.uploads[x.arg].texId == id ? EV_WRITE : EV_NONE;
    default: return EV_NONE;
    }
}
// What the buffer does first with texture `id` (the whole buffer from `from`).
inline Ev First(const CmdBuf& b, size_t from, uint32_t id) {
    for (size_t j = from; j < b.cmds.size(); ++j) {
        const Ev e = Event(b, b.cmds[j], id);
        if (e != EV_NONE) return e;
    }
    return EV_NONE;
}
// May the next buffer be read as "what runs next"?
inline bool Ordinary(const CmdBuf& b) { return b.hrwId == 0 && b.hrwReplayId == 0 && !b.hrwFresh && !b.cmds.empty(); }

// `cur` is about to be executed (an ordinary buffer: hrwId 0, no replay, no retention), `next` the buffer queued behind it, or null. Count; with `apply` leave the dead
// captures out (CopyTexCmd::skip, exactly what the front end's rule sets for a capture nothing reads). Returns how many.
inline unsigned Decide(CmdBuf& cur, const CmdBuf* next, bool apply, Counters& k, Shadow& sh) {
    ++k.pictures;
    const bool nq = next != nullptr, no = nq && Ordinary(*next);
    k.nextQueued += nq;
    k.nextOrdinary += no;
    sh = Shadow{};
    unsigned left = 0;
    const size_t n = cur.cmds.size();
    for (size_t i = 0; i < n; ++i) {
        if (cur.cmds[i].type != CMD_COPY_TEX) continue;
        CopyTexCmd& c = cur.copyTex[cur.cmds[i].arg];
        if (c.skip || !c.transient) continue;
        if (!Plain(c)) { ++k.refusedShape; continue; }
        ++k.captures;
        Ev tail = EV_NONE;
        for (size_t j = i + 1; j < n && tail == EV_NONE; ++j) tail = Event(cur, cur.cmds[j], c.texId);
        if (tail == EV_READ || tail == EV_DOUBT) { ++k.readerHere; continue; }
        bool dead = false;
        if (tail == EV_WRITE) { // (written again in this picture before any read: dead whatever comes next)
            ++k.deadOwn;
            dead = true;
        } else if (!no) {
            ++k.noNext;
        } else {
            const Ev f = First(*next, 0, c.texId);
            if (f == EV_WRITE) {
                ++k.viaNext;
                dead = true;
                if (sh.n < 4) { sh.tex[sh.n++] = c.texId; sh.next = next; sh.frame = next->frame; }
            } else if (f == EV_READ) {
                ++k.nextReads;
            } else {
                ++k.nextNoWrite; // (nothing, or a doubtful write)
            }
        }
        if (dead) {
            ++left;
            if (apply) {
                c.skip = true; // (what MarkTransientCopies sets for a capture nothing reads, and MarkSwapCopies then resets)
                c.swapOk = false;
                c.swapPredictedNo = false;
            }
        }
    }
    return left;
}
// The buffer that was looked at as "next" is about to run: its first command on each texture a capture was left out for must be a whole write. A read there would be a
// stale read (the capture's picture, or older) - it cannot happen unless the buffer changed after it was queued. true: all fine.
inline bool Verify(const CmdBuf& running, Shadow& sh, Counters& k) {
    if (!sh.n) return true;
    bool ok = true;
    if (sh.next == &running && sh.frame == running.frame) {
        for (unsigned i = 0; i < sh.n; ++i) {
            ++k.shadowChecks;
            if (First(running, 0, sh.tex[i]) != EV_WRITE) { ++k.shadowBad; ok = false; }
        }
    }
    sh = Shadow{};
    return ok;
}
inline void Add(Counters& a, const Counters& b) {
    a.pictures += b.pictures; a.nextQueued += b.nextQueued; a.nextOrdinary += b.nextOrdinary; a.captures += b.captures; a.readerHere += b.readerHere; a.deadOwn += b.deadOwn;
    a.noNext += b.noNext; a.nextReads += b.nextReads; a.nextNoWrite += b.nextNoWrite; a.viaNext += b.viaNext; a.refusedShape += b.refusedShape;
    a.shadowChecks += b.shadowChecks; a.shadowBad += b.shadowBad;
}
// The "indcap-peek:" line, every 600 pictures (render thread, not the display callback). Returns false when it is not due.
inline bool Line(char* out, size_t cap, Counters& win, Counters& tot, unsigned every = 600) {
    if (win.pictures < every) return false;
    const Counters& w = win;
    const double p = static_cast<double>(w.pictures);
    const unsigned long long dead = w.viaNext + w.deadOwn;
    std::snprintf(out, cap,
                  "indcap-peek: GXV_INDCAP_PEEK=%d (%s), last %llu pictures: next buffer queued %.0f%% (usable %.0f%%); %llu plain captures: dead %llu%s (%llu via the next buffer, %llu in own picture), "
                  "kept: reader in own picture %llu, next not queued %llu, next reads first %llu, next has no whole overwrite %llu; marked of another shape %llu; "
                  "shadow check: %llu skips, next buffer's first use was not a whole write %llu (MUST be 0); run totals: %llu dead of %llu captures in %llu pictures",
                  Mode(), Mode() == 1 ? "applied" : "count only, nothing left out", static_cast<unsigned long long>(w.pictures), 100.0 * static_cast<double>(w.nextQueued) / p,
                  100.0 * static_cast<double>(w.nextOrdinary) / p, static_cast<unsigned long long>(w.captures), dead, Mode() == 1 ? "" : " (would be)",
                  static_cast<unsigned long long>(w.viaNext), static_cast<unsigned long long>(w.deadOwn), static_cast<unsigned long long>(w.readerHere),
                  static_cast<unsigned long long>(w.noNext), static_cast<unsigned long long>(w.nextReads), static_cast<unsigned long long>(w.nextNoWrite),
                  static_cast<unsigned long long>(w.refusedShape), static_cast<unsigned long long>(w.shadowChecks), static_cast<unsigned long long>(w.shadowBad),
                  static_cast<unsigned long long>(tot.viaNext + tot.deadOwn + dead), static_cast<unsigned long long>(tot.captures + w.captures),
                  static_cast<unsigned long long>(tot.pictures + w.pictures));
    Add(tot, win);
    win = Counters{};
    return true;
}
} // namespace vgx::indcappeek
