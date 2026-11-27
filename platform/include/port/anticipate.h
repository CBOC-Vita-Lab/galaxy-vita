// anticip32 (13 Nov 2026): GXV_ANTICIPATE, the scaling engine's look-ahead. Pure logic: no game types, no clock, no threads, no file (host-tested by tests/anticipate_test.cpp).
// The glue is platform/src/port/perf.cpp ("anticip32": the switches, the actuators, the log lines, the card file); the game's hooks call port_anticipate_event (compat.h).
//
// WHY (the owner, 13 Nov 22:50): "maybe we anticipate when things are going to need to scale down, like when he gets launched across a level by a star or boss fights ... scale quick to
// prevent it happening when the action is actually going on". Every controller we have REACTS: the governors after 2 hot 5-second windows, the Quality ladder after 0.75-1.5 s of a threat,
// the rate controller after 8 missed turns of 30, the speed guard after pictures were already dropped. A launch star tells us 0.77 s before the flight that the camera is about to race
// across the level; a place where the engine had to cut on an earlier visit tells us before Mario is there.
//
// WHAT. ONE window at a time: "a heavy moment starts in ~lead ms and lasts ~last ms". While the window is open the glue asks the EXISTING engine for a little less far detail, through its own
// actuators and fades (Performance: the cut level the governors' fader walks, + levels; Quality: the ladder's next least-visible steps as an overlay on its published mask; rate controller:
// no up-switch inside the window, and a down-switch AT the event when the map says this event ended at an even 30 before). When the window closes the request is withdrawn and the same fader
// walks the detail back. This is feed-forward only: nothing here measures a time or decides a level by itself, the governors / ladder / rate controller keep every rule they have.
//   - events (Event): launch star, sling star, boss battle start, power-up change, test event open a window; a demo opens one only where the map has seen an overload in that demo before;
//     a camera cut is only counted. An event inside an open window extends it (never past maxMs from its start).
//   - the spike map (GXV_SPIKEMAP): when the engine had to act (the speed guard's sustained pressure, a rate drop, a governor raise, a ladder step) the place is remembered per stage and
//     scenario as a coarse cell of Mario's position (back-dated by the detector's own delay), or, when an event window was open, as "this event at this place". On a later visit the cell is
//     seen leadMs ahead along Mario's motion and a window is opened; every 4th visit runs WITHOUT the pre-scale (a probe): no overload in the probe forgets the cell one hit at a time.
// Game logic and turn timing are not touched: the module reads a position and a few flags and writes only what the cut systems read.
#pragma once

#include <cstdint>
#include <cstdio>
#include <stdio.h>  // (unqualified snprintf: game units map it to port_snprintf)
#include <cstdlib>
#include <cstring>

namespace port {
namespace anticipate {

enum Kind : uint8_t { kNone = 0, kLaunch, kSling, kDemo, kBoss, kPowerUp, kCamCut, kSpike, kTest, kKindN };
inline const char* KindName(int k) {
    static const char* const t[kKindN] = {"none", "launch star", "sling star", "demo", "boss battle", "power-up", "camera cut", "learned spike cell", "test event"};
    return k >= 0 && k < kKindN ? t[k] : "?";
}
// the kinds that open a window on their own word (mode 1); the others need the map
inline bool OpensUnlearned(int k) { return k == kLaunch || k == kSling || k == kBoss || k == kPowerUp || k == kTest; }
// what the engine reports (Spike): who had to act, and how long after the cause that detector fires (the place is back-dated by it)
enum Src : uint8_t { kSrcGuard = 0, kSrcRate, kSrcGov, kSrcLadder, kSrcN };
inline const char* SrcName(int s) { static const char* const t[kSrcN] = {"speed guard", "rate drop", "governor raise", "ladder step"}; return s >= 0 && s < kSrcN ? t[s] : "?"; }
constexpr uint32_t kSrcDelayMs[kSrcN] = {500, 1000, 10000, 2000};

struct Params {
    int mode = 0;               // GXV_ANTICIPATE: 0 off, 1 events + map, 2 only what the map has seen an overload at
    uint32_t leadMs = 1000;     // GXV_ANTICIPATE_LEAD_MS: how far ahead of a learned cell the window opens (and the longest the pre-scale's own walk may take)
    bool map = false;           // GXV_SPIKEMAP
    float cell = 2000.0f;       // GXV_SPIKEMAP_CELL: the cell's edge in game units (20 m)
    uint32_t tailMs = 1500;     // GXV_ANTICIPATE_TAIL_MS: kept after the stated end (the arrival: the new planet's first pictures)
    uint32_t maxMs = 15000;     // GXV_ANTICIPATE_MAX_MS: a window never lasts longer than this from its start (nothing is pinned by a stuck event)
    uint32_t cellHoldMs = 4000; // how long a learned cell's window lasts
};

struct Cell {
    char stage[40];
    int16_t x, y, z;
    uint8_t kind;    // 0 = a place; else the event that was running (the place is where the event began)
    uint8_t hits;    // overloads seen here (1..250)
    uint8_t flags;   // kFlagRate, kFlagDeep
    uint8_t visits;  // pre-scaled visits since the last overload (every 4th is a probe)
};
enum : uint8_t { kFlagRate = 1 /* it ended in dropped pictures or an even 30 */, kFlagDeep = 2 /* an overload came in spite of the pre-scale: one level more next time */ };

class SpikeMap {
public:
    static constexpr int kCap = 128;
    int Count() const { return n_; }
    const Cell& At(int i) const { return e_[i]; }
    Cell& At(int i) { return e_[i]; }
    int CountStage(const char* stage) const { int c = 0; for (int i = 0; i < n_; ++i) if (!std::strcmp(e_[i].stage, stage)) ++c; return c; }
    // kind < 0: any place cell (kind 0) ; else exactly that kind
    int Find(const char* stage, const int16_t c[3], int kind) const {
        for (int i = 0; i < n_; ++i)
            if (e_[i].x == c[0] && e_[i].y == c[1] && e_[i].z == c[2] && e_[i].kind == static_cast<uint8_t>(kind < 0 ? 0 : kind) && !std::strcmp(e_[i].stage, stage)) return i;
        return -1;
    }
    int Note(const char* stage, const int16_t c[3], int kind, uint8_t flags) {
        int i = Find(stage, c, kind);
        if (i < 0) {
            if (n_ < kCap) i = n_++;
            else { i = 0; for (int k = 1; k < n_; ++k) if (e_[k].hits < e_[i].hits) i = k; }   // (full: the cell with the fewest overloads goes)
            std::memset(&e_[i], 0, sizeof e_[i]);
            std::snprintf(e_[i].stage, sizeof e_[i].stage, "%s", stage);
            e_[i].x = c[0]; e_[i].y = c[1]; e_[i].z = c[2];
            e_[i].kind = static_cast<uint8_t>(kind);
        }
        if (e_[i].hits < 250) ++e_[i].hits;
        e_[i].flags = static_cast<uint8_t>(e_[i].flags | flags);
        e_[i].visits = 0;
        dirty = true;
        return i;
    }
    // a probe that saw no overload: one hit less; none left: the cell is forgotten. True when it was removed.
    bool Forget(int i) {
        if (i < 0 || i >= n_) return false;
        dirty = true;
        if (e_[i].hits > 1) { --e_[i].hits; e_[i].flags = static_cast<uint8_t>(e_[i].flags & ~kFlagDeep); return false; }
        e_[i] = e_[n_ - 1];
        --n_;
        return true;
    }
    // "# spikes v1 ...\n<stage:scenario> <x> <y> <z> <kind> <hits> <flags>\n"; a line that does not parse is skipped. Returns the cells read.
    int Parse(const char* text, size_t len) {
        n_ = 0;
        size_t p = 0;
        while (p < len && n_ < kCap) {
            size_t q = p;
            while (q < len && text[q] != '\n') ++q;
            char line[120];
            const size_t m = q - p < sizeof line - 1 ? q - p : sizeof line - 1;
            std::memcpy(line, text + p, m);
            line[m] = 0;
            p = q + 1;
            if (line[0] == '#' || !line[0]) continue;
            char st[40];
            int x = 0, y = 0, z = 0, kind = 0, hits = 0, flags = 0;
            if (std::sscanf(line, "%39s %d %d %d %d %d %d", st, &x, &y, &z, &kind, &hits, &flags) != 7) continue;
            if (kind < 0 || kind >= kKindN || hits < 1 || x < -32000 || x > 32000 || y < -32000 || y > 32000 || z < -32000 || z > 32000) continue;
            Cell& c = e_[n_++];
            std::memset(&c, 0, sizeof c);
            std::snprintf(c.stage, sizeof c.stage, "%s", st);
            c.x = static_cast<int16_t>(x); c.y = static_cast<int16_t>(y); c.z = static_cast<int16_t>(z);
            c.kind = static_cast<uint8_t>(kind); c.hits = static_cast<uint8_t>(hits > 250 ? 250 : hits); c.flags = static_cast<uint8_t>(flags & 3);
        }
        dirty = false;
        return n_;
    }
    int Format(char* out, size_t cap) const {
        size_t used = static_cast<size_t>(std::snprintf(out, cap, "# spikes v1: stage:scenario, cell x y z (units of the cell edge), event kind (0 = a place), overloads seen, flags (1 = pictures were dropped / even 30, 2 = one level more)\n"));
        for (int i = 0; i < n_ && used + 90 < cap; ++i)
            used += static_cast<size_t>(std::snprintf(out + used, cap - used, "%s %d %d %d %d %d %d\n", e_[i].stage, static_cast<int>(e_[i].x), static_cast<int>(e_[i].y), static_cast<int>(e_[i].z),
                                                      static_cast<int>(e_[i].kind), static_cast<int>(e_[i].hits), static_cast<int>(e_[i].flags)));
        return static_cast<int>(used);
    }
    bool dirty = false;

private:
    Cell e_[kCap];
    int n_ = 0;
};

// What the glue reads after Event / Turn: a window was opened (once), a window was closed (once).
struct Window {
    bool open = false;
    uint8_t kind = 0;
    bool learned = false;   // the map knew it
    bool deep = false;      // ... and asks for one level more
    bool rate = false;      // ... and it ended in dropped pictures / an even 30 before
    uint32_t turn = 0, leadMs = 0;
    uint64_t startUs = 0, endUs = 0;
    int cellIdx = -1;
    float pos[3] = {0, 0, 0};
    bool havePos = false;
    bool overload = false;  // the engine had to act inside it all the same
};

struct Census {
    uint32_t events[kKindN] = {};   // all seen, by kind
    uint32_t opened = 0, extended = 0, ignored = 0, closed = 0;
    uint32_t spikes[kSrcN] = {};
    uint32_t learned = 0, cellHits = 0, probes = 0, forgotten = 0, overloadInside = 0;
    uint64_t openUs = 0;            // time with a window open
};

class Engine {
public:
    void Configure(const Params& p) { p_ = p; }
    const Params& Cfg() const { return p_; }
    SpikeMap& Map() { return map_; }
    const Window& Win() const { return w_; }
    const Census& Stats() const { return cs_; }
    bool On() const { return p_.mode != 0; }

    // A stage (and scenario) begins: the window is dropped (the load is its own cover), the position history starts anew.
    void StageBegin(const char* stage) {
        std::snprintf(stage_, sizeof stage_, "%s", stage ? stage : "?");
        w_ = Window();
        justOpened_ = justClosed_ = false;
        histN_ = histHead_ = 0;
        haveVel_ = false;
        lastCell_ = -1;
        probeIdx_ = -1;
        lastSpikeUs_ = 0;
    }
    const char* Stage() const { return stage_; }

    // A game hook: `kind` begins in about leadMs and lasts about lastMs. Returns true when a window is open after the call because of it.
    bool Event(uint64_t now, uint32_t turn, int kind, uint32_t leadMs, uint32_t lastMs) {
        if (kind <= kNone || kind >= kKindN) return false;
        ++cs_.events[kind];
        if (!On() || kind == kCamCut) return false;
        int16_t c[3];
        int idx = -1;
        if (p_.map && havePos_ && kind != kSpike) { CellOf(pos_, c); idx = map_.Find(stage_, c, kind); }
        const bool learned = idx >= 0 || kind == kSpike;
        if (!learned && ((p_.mode >= 2 && kind != kTest) || !OpensUnlearned(kind))) { ++cs_.ignored; return false; }
        const uint64_t span = static_cast<uint64_t>(leadMs) + lastMs + p_.tailMs;
        if (w_.open) {   // one window: a second event extends it
            const uint64_t cap = w_.startUs + static_cast<uint64_t>(p_.maxMs) * 1000u;
            uint64_t end = now + span * 1000u;
            if (end > cap) end = cap;
            if (end > w_.endUs) { w_.endUs = end; ++cs_.extended; }
            return true;
        }
        w_ = Window();
        w_.open = true;
        w_.kind = static_cast<uint8_t>(kind);
        w_.learned = learned;
        w_.turn = turn;
        w_.leadMs = leadMs;
        w_.startUs = now;
        w_.endUs = now + (span > p_.maxMs ? p_.maxMs : span) * 1000u;
        w_.cellIdx = kind == kSpike ? pendingCell_ : idx;
        if (w_.cellIdx >= 0 && w_.cellIdx < map_.Count()) {
            Cell& ce = map_.At(w_.cellIdx);
            w_.deep = (ce.flags & kFlagDeep) != 0;
            w_.rate = (ce.flags & kFlagRate) != 0;
            if (ce.visits < 250) ++ce.visits;
            ++cs_.cellHits;
        }
        w_.havePos = havePos_;
        if (havePos_) { w_.pos[0] = pos_[0]; w_.pos[1] = pos_[1]; w_.pos[2] = pos_[2]; }
        ++cs_.opened;
        justOpened_ = true;
        return true;
    }

    // Once a game turn. pos: Mario's position (null: none, e.g. a menu). Closes a window whose time is over, samples the position, looks the map up ahead.
    void Turn(uint64_t now, uint32_t turn, const float* pos) {
        if (lastTurnUs_ && w_.open && now > lastTurnUs_) cs_.openUs += now - lastTurnUs_;
        lastTurnUs_ = now;
        turn_ = turn;
        if (w_.open && now >= w_.endUs) {
            closedWin_ = w_;
            closedTurn_ = turn;
            w_.open = false;
            ++cs_.closed;
            justClosed_ = true;
            closedAtUs_ = now;
        }
        havePos_ = pos != nullptr;
        if (!havePos_) { haveVel_ = false; return; }
        if (haveVel_) {   // the motion a turn, smoothed (a warp or a cut of the position is no motion)
            for (int i = 0; i < 3; ++i) {
                const float d = pos[i] - pos_[i];
                vel_[i] = (d > 600.0f || d < -600.0f) ? 0.0f : vel_[i] * 0.875f + d * 0.125f;
            }
        } else vel_[0] = vel_[1] = vel_[2] = 0.0f;
        haveVel_ = true;
        pos_[0] = pos[0]; pos_[1] = pos[1]; pos_[2] = pos[2];
        if (turn % kHistEvery == 0) {
            float* h = hist_[histHead_];
            h[0] = pos[0]; h[1] = pos[1]; h[2] = pos[2];
            histHead_ = (histHead_ + 1) % kHist;
            if (histN_ < kHist) ++histN_;
        }
        if (!On() || !p_.map) return;
        // a probe (a known cell visited without the pre-scale) that saw no overload for 8 s: the cell loses a hit
        if (probeIdx_ >= 0 && now >= probeUntilUs_) {
            if (probeIdx_ < map_.Count() && map_.Forget(probeIdx_)) { ++cs_.forgotten; lastCell_ = -1; w_.cellIdx = closedWin_.cellIdx = -1; }   // (the last cell took its index)
            probeIdx_ = -1;
        }
        if (turn % 8u != 0u) return;
        int16_t c0[3], c1[3];
        CellOf(pos_, c0);
        const float ahead = static_cast<float>(p_.leadMs) * 0.06f;   // turns in leadMs
        const float pp[3] = {pos_[0] + vel_[0] * ahead, pos_[1] + vel_[1] * ahead, pos_[2] + vel_[2] * ahead};
        CellOf(pp, c1);
        int idx = map_.Find(stage_, c1, -1);
        if (idx < 0) idx = map_.Find(stage_, c0, -1);
        if (idx < 0) { lastCell_ = -1; return; }
        if (idx == lastCell_ || w_.open || probeIdx_ >= 0) return;   // (the same cell acts once per visit; it re-arms when Mario has left it)
        lastCell_ = idx;
        Cell& ce = map_.At(idx);
        if (ce.visits % 4u == 3u) {   // every 4th visit is a probe: is this place still heavy?
            ++ce.visits;
            probeIdx_ = idx;
            probeUntilUs_ = now + 8000000u;
            ++cs_.probes;
            return;
        }
        pendingCell_ = idx;
        Event(now, turn, kSpike, p_.leadMs, p_.cellHoldMs);
        pendingCell_ = -1;
    }

    // The engine had to act (a detector fired). Remembers the place. Returns the cell's index or -1 (map off, no position, or within 3 s of the last one).
    int Spike(uint64_t now, int src) {
        if (src < 0 || src >= kSrcN) return -1;
        ++cs_.spikes[src];
        const bool inside = w_.open || (closedAtUs_ != 0 && now - closedAtUs_ < 2000000u);
        if (w_.open && !w_.overload) { w_.overload = true; ++cs_.overloadInside; }
        if (probeIdx_ >= 0) { if (probeIdx_ < map_.Count()) map_.At(probeIdx_).visits = 0; probeIdx_ = -1; }   // (the probe found it still heavy: kept, and pre-scaled again from the next visit)
        if (!On() || !p_.map || !havePos_) return -1;
        if (lastSpikeUs_ != 0 && now - lastSpikeUs_ < 3000000u) return -1;
        lastSpikeUs_ = now;
        const uint8_t fl = static_cast<uint8_t>((src == kSrcGuard || src == kSrcRate) ? kFlagRate : 0);
        const Window& w = w_.open ? w_ : closedWin_;
        int16_t c[3];
        int idx;
        if (inside && w.kind == kSpike && w.cellIdx >= 0 && w.cellIdx < map_.Count()) {   // in spite of the pre-scale: one level more the next time
            Cell& ce = map_.At(w.cellIdx);
            if (ce.hits < 250) ++ce.hits;
            ce.flags = static_cast<uint8_t>(ce.flags | fl | kFlagDeep);
            ce.visits = 0;
            map_.dirty = true;
            idx = w.cellIdx;
        } else if (inside && w.havePos) {   // during an event: "this event, begun at this place"
            CellOf(w.pos, c);
            const bool had = map_.Find(stage_, c, w.kind) >= 0;
            idx = map_.Note(stage_, c, w.kind, static_cast<uint8_t>(fl | (had ? kFlagDeep : 0)));
        } else {
            float at[3];
            PosAgo(kSrcDelayMs[src], at);
            CellOf(at, c);
            idx = map_.Note(stage_, c, 0, fl);
        }
        ++cs_.learned;
        return idx;
    }

    // true once after a window opened / closed (the glue acts and logs)
    bool TakeOpened() { const bool r = justOpened_; justOpened_ = false; return r; }
    bool TakeClosed(Window* w, uint32_t* turn) { if (!justClosed_) return false; justClosed_ = false; if (w) *w = closedWin_; if (turn) *turn = closedTurn_; return true; }

    // "anticipate: <event> at turn N: pre-scaled <what> (lead L ms)" and the same with "; restored at turn M"
    static int FormatOpen(char* out, size_t cap, const Window& w, const char* what) {
        return std::snprintf(out, cap, "anticipate: %s at turn %u: pre-scaled %s (lead %u ms)%s", KindName(w.kind), static_cast<unsigned>(w.turn), what, static_cast<unsigned>(w.leadMs),
                             w.learned ? " [the spike map knows it]" : "");
    }
    static int FormatClose(char* out, size_t cap, const Window& w, const char* what, uint32_t turn) {
        return std::snprintf(out, cap, "anticipate: %s at turn %u: pre-scaled %s (lead %u ms); restored at turn %u%s", KindName(w.kind), static_cast<unsigned>(w.turn), what,
                             static_cast<unsigned>(w.leadMs), static_cast<unsigned>(turn), w.overload ? " [the engine had to cut more inside the window: learned]" : "");
    }
    // the 10 s line
    int FormatSummary(char* out, size_t cap, double secs) const {
        return std::snprintf(out, cap,
                             "anticipate: summary: stage %s, %.0f s: events seen: launch %u, sling %u, demo %u, boss %u, power-up %u, camera cut %u, test %u; windows opened %u (extended %u, "
                             "not opened %u), restored %u, open %.1f s; overloads reported: guard %u, rate %u, governor %u, ladder %u (inside a window %u); spike map: %d cells here (%d in all), "
                             "learned %u, pre-scaled from the map %u, probes %u, forgotten %u",
                             stage_, secs, cs_.events[kLaunch], cs_.events[kSling], cs_.events[kDemo], cs_.events[kBoss], cs_.events[kPowerUp], cs_.events[kCamCut], cs_.events[kTest], cs_.opened,
                             cs_.extended, cs_.ignored, cs_.closed, static_cast<double>(cs_.openUs) / 1e6, cs_.spikes[kSrcGuard], cs_.spikes[kSrcRate], cs_.spikes[kSrcGov], cs_.spikes[kSrcLadder],
                             cs_.overloadInside, map_.CountStage(stage_), map_.Count(), cs_.learned, cs_.cellHits, cs_.probes, cs_.forgotten);
    }

    void CellOf(const float* p, int16_t c[3]) const {
        for (int i = 0; i < 3; ++i) {
            float v = p[i] / p_.cell;
            v = v < 0 ? v - 1.0f : v;   // (floor)
            c[i] = static_cast<int16_t>(v < -32000.0f ? -32000 : (v > 32000.0f ? 32000 : static_cast<int>(v)));
        }
    }

private:
    static constexpr int kHist = 32;         // 16 s of positions
    static constexpr uint32_t kHistEvery = 30;   // one every half second
    // Mario's position about `ms` ago (the oldest sample when the history is shorter; now when there is none)
    void PosAgo(uint32_t ms, float out[3]) const {
        int back = static_cast<int>(ms / 500u);
        if (back > histN_) back = histN_;
        if (back <= 0 || histN_ == 0) { out[0] = pos_[0]; out[1] = pos_[1]; out[2] = pos_[2]; return; }
        const float* h = hist_[(histHead_ - back + 2 * kHist) % kHist];
        out[0] = h[0]; out[1] = h[1]; out[2] = h[2];
    }

    Params p_;
    SpikeMap map_;
    Window w_, closedWin_;
    Census cs_;
    char stage_[40] = "?";
    bool justOpened_ = false, justClosed_ = false, havePos_ = false, haveVel_ = false;
    uint32_t closedTurn_ = 0, turn_ = 0;
    uint64_t closedAtUs_ = 0, lastTurnUs_ = 0, lastSpikeUs_ = 0, probeUntilUs_ = 0;
    float pos_[3] = {0, 0, 0}, vel_[3] = {0, 0, 0};
    float hist_[kHist][3] = {};
    int histN_ = 0, histHead_ = 0, lastCell_ = -1, probeIdx_ = -1, pendingCell_ = -1;
};

}  // namespace anticipate
}  // namespace port
