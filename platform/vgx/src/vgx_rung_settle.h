// GXV_RUNG_SKIPRS_SETTLE (bugs26, 12 Nov): after a scene is destroyed (a stage load), a picture-size change must not use the "rewritten every picture"
// evidence (RtSkippable) for a while. The 2.2 crash came 2 s after a stage load with "5 not resampled": the tracking's slots and names were made before the
// load, and the only guard against that (GXV_PACE23's settle window) is off by default. Pure logic, one thread (the back end's): the front end only bumps
// a counter (NoteSceneGone) that the back end reads here.
#pragma once
#include <cstdint>

namespace vgx {
namespace rungsettle {

struct Gate {
    unsigned seen = 0;       // the scene-gone counter as last read
    uint64_t until = 0;      // presents: skipping is refused while presents < until
    unsigned clears = 0;     // how many times the evidence was cleared (census)
    unsigned refusals = 0;   // how many questions were answered "no" because of the window (census)
};

// seq: the counter of destroyed scenes; presents: the back end's present count; settle: window in presents (0 = the guard is off).
// Returns true when the caller must forget its evidence now (a new scene-gone since the last call).
inline bool Note(Gate& g, unsigned seq, uint64_t presents, unsigned settle) {
    if (settle == 0 || seq == g.seen) { g.seen = seq; return false; }
    g.seen = seq;
    g.until = presents + settle;
    ++g.clears;
    return true;
}

// True when evidence may be used.
inline bool Allowed(Gate& g, uint64_t presents) {
    if (presents < g.until) { ++g.refusals; return false; }
    return true;
}

}  // namespace rungsettle
}  // namespace vgx
