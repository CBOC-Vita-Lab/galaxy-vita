#pragma once
// pace33 PATCH9, GXV_SEG_PREWAIT_US (default 4000; 0 off): the game's stream segment is changed just BEFORE it fills, at a turn start where the front end is nearly caught up.
//
// Console 2892a (the Comet Observatory, the defaults): the front end takes 26 ms a picture and ends ~21 ms behind the game's draw, so it is almost never idle at a turn start and
// GXV_SEG_IDLE changes segments only ~2.4 times a second. The 2 MiB segment then fills in the middle of a draw 1.3 times a second and the game thread waits for the front end's
// whole backlog: 15 ms each ("stream: ... 14 after a full segment (the game waited 219 ms in all)"). Those are the pairs over 33 ms (hrw: max 47.8) that keep the game-speed guard
// repaying.
// The rule: at a turn start (port_vgx_publish flag 4), when the segment would fill within about the next two turns (room < 2 * the largest turn of the last 8 + 64 KiB) and the front
// end has at most GXV_SEG_PREWAIT_KIB (64) left to parse, the game thread waits for it, at most GXV_SEG_PREWAIT_US, and changes segments exactly as GXV_SEG_IDLE does. At a drawn
// turn's start the front end is 2-5 ms from done (fe-lane: hand-over 35.9 ms after the turn start, 38.6 at the 95th), so the wait is a few ms instead of 15. One failed wait a
// segment at most. The bytes and their order are unchanged: pictures are identical.
// Pure decision here (host test tests/pace33_segpre_test.cpp); the wait itself is in platform/src/gx/vgx_galaxy.cpp.
#include <cstddef>
#include <cstdint>

namespace port::segpre {
struct Config {
    size_t segSize = 0;       // the usable segment
    size_t idleLimit = 0;     // GXV_SEG_IDLE in bytes (0: the idle changes are off, and so is this)
    int64_t waitNs = 0;       // GXV_SEG_PREWAIT_US
    size_t remLimit = 0;      // GXV_SEG_PREWAIT_KIB
    size_t margin = 64u << 10;
};
struct State {
    uint32_t lastEpoch = 0xFFFFFFFFu, missEpoch = 0xFFFFFFFFu;
    size_t lastOff = 0;
    size_t turn[8] = {};
    unsigned n = 0;
    size_t Largest() const { size_t m = 0; for (size_t t : turn) if (t > m) m = t; return m; }
};
// Called once at every turn start with the write offset. progressKnown: the front end's read position is in this segment; rem: the published bytes it has not parsed.
inline bool Want(State& s, const Config& c, uint32_t epoch, size_t off, bool progressKnown, size_t rem) {
    const size_t d = (epoch == s.lastEpoch && off >= s.lastOff) ? off - s.lastOff : off;
    s.turn[s.n++ & 7] = d;
    s.lastEpoch = epoch;
    s.lastOff = off;
    if (c.waitNs <= 0 || !c.idleLimit || off < c.idleLimit || off >= c.segSize) return false;
    if (c.segSize - off >= 2 * s.Largest() + c.margin) return false;   // (it does not fill within the next two turns)
    if (s.missEpoch == epoch) return false;                            // (one failed wait a segment)
    return progressKnown && rem <= c.remLimit;
}
inline void Missed(State& s, uint32_t epoch) { s.missEpoch = epoch; }
}  // namespace port::segpre
