// logic23 (8 Nov): a uniform grid over conservative world boxes, for exact "which of these N objects can hold this point" lookups:
// GXV_AREA_GRID (AreaObjMgr::find_in, decomp/src/Game/AreaObj/AreaObj.cpp) and GXV_GRAV_GRID (the gravity walk,
// decomp/src/Game/Gravity/PlanetGravityManager.cpp). Host proof: tests/logic23_grids.cpp, tools/test_logic23_grids.sh.
//
// The caller has N objects in an array whose order matters (the original walks it in that order). Each object is either
//   - "static": it has a world box [lo, hi] (floats) that holds EVERY point the object's own test can accept, or
//   - "dynamic": no such box (it moves, or its shape gives none): it is a candidate for every point.
// Build() files every static object's index into the cells its box touches. Candidates() gives, for a point, the indices of the
// static objects filed in the point's cell, merged with the dynamic ones, in the array's order (ascending, or descending for a walk
// from the end). The caller then runs its ORIGINAL per-object test on each candidate, in that order.
//
// Why no object that accepts the point is ever left out (the whole exactness argument):
//   Coord(p) = the cell coordinate of p on one axis = floor(fl(fl(p - o) * inv)), clamped to [-1, dim]. Each step is monotone
//   non-decreasing in p (IEEE subtraction and multiplication by a positive constant under round-to-nearest are monotone; floor and
//   the clamp are monotone), and the SAME inline function computes it at the build and at the lookup, from the same float bits.
//   If an object accepts p, then lo <= p <= hi on every axis (the box's contract), so Coord(lo) <= Coord(p) <= Coord(hi): p's cell
//   is one of the cells the object was filed in. A point whose cell is outside the grid (-1 or dim on an axis) is below every lo or
//   above every hi on that axis (the grid's origin is the smallest lo and its size covers the largest hi, again through Coord), so
//   no static object holds it: only the dynamic ones are candidates.
//   A point with a non-finite coordinate is never looked up (Usable() is false: the caller walks everything, as before).
// What the grid does NOT decide: the order (the array's), and the answer (each candidate's own test, unchanged). Objects that are
// not candidates are ones whose test would have said "no" (and the callers' tests have no side effects).
//
// The grid depends only on the array (its length and order) and on the static boxes: the callers rebuild it when a generation
// counter that every writer of those moves has changed. Memory: malloc'd, kept and reused (realloc) by the next build.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace port {
namespace lgrid {

struct Box {
    float lo[3], hi[3];
};

inline int Coord(float p, float o, float inv, int dim) {
    const float v = (p - o) * inv;
    if (v < 0.0f) return -1;
    if (v >= static_cast< float >(dim)) return dim;
    return static_cast< int >(v);  // (v >= 0: truncation is floor)
}

inline bool Finite3(float x, float y, float z, float limit) {
    return std::fabs(x) <= limit && std::fabs(y) <= limit && std::fabs(z) <= limit;  // (false for NaN)
}

struct Grid {
    bool usable = false;
    int count = 0;            // the array's length at the build
    float o[3] = {0, 0, 0};
    float inv = 1.0f;
    int dim[3] = {0, 0, 0};
    uint32_t* start = nullptr;  // cells + 1 offsets into items
    uint16_t* items = nullptr;  // per cell: static indices, ascending
    uint16_t* dyn = nullptr;    // dynamic indices, ascending
    int nDyn = 0;
    size_t capStart = 0, capItems = 0, capDyn = 0;
    uint32_t builds = 0;

    template < class T > static bool Grow(T*& rBuf, size_t& rCap, size_t need) {
        if (need <= rCap) return true;
        T* p = static_cast< T* >(std::realloc(rBuf, sizeof(T) * need));
        if (p == nullptr) return false;
        rBuf = p;
        rCap = need;
        return true;
    }

    // boxes[i] is read only where isStatic[i]. maxDim: cells along the longest axis (the others in proportion). maxItems: the
    // filing budget (a box that covers many cells costs many entries); over it the grid is coarsened, and at 1 cell it is unusable.
    bool Build(const Box* boxes, const uint8_t* isStatic, int n, int maxDim, size_t maxItems) {
        usable = false;
        count = n;
        nDyn = 0;
        ++builds;
        if (n <= 0 || n > 65535) return false;
        float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        int nStatic = 0;
        if (!Grow(dyn, capDyn, static_cast< size_t >(n))) return false;
        for (int i = 0; i < n; ++i) {
            if (!isStatic[i]) {
                dyn[nDyn++] = static_cast< uint16_t >(i);
                continue;
            }
            ++nStatic;
            for (int a = 0; a < 3; ++a) {
                if (boxes[i].lo[a] < lo[a]) lo[a] = boxes[i].lo[a];
                if (boxes[i].hi[a] > hi[a]) hi[a] = boxes[i].hi[a];
            }
        }
        if (nStatic == 0) return false;
        for (int a = 0; a < 3; ++a) {
            if (!(std::fabs(lo[a]) <= 1e30f) || !(std::fabs(hi[a]) <= 1e30f) || !(lo[a] <= hi[a])) return false;
        }
        const float ext = std::fmax(hi[0] - lo[0], std::fmax(hi[1] - lo[1], hi[2] - lo[2]));
        for (int d = maxDim; d >= 2; d /= 2) {
            o[0] = lo[0];
            o[1] = lo[1];
            o[2] = lo[2];
            inv = ext > 0.0f ? static_cast< float >(d) / ext : 1.0f;
            if (!(inv > 0.0f) || !(inv < 1e30f)) return false;
            size_t cells = 1;
            for (int a = 0; a < 3; ++a) {
                // (Coord of the largest hi with an unclamped dimension: the grid's size covers it, by the same function)
                const float v = (hi[a] - o[a]) * inv;
                if (!(v >= 0.0f) || !(v < 4096.0f)) return false;
                dim[a] = static_cast< int >(v) + 1;
                cells *= static_cast< size_t >(dim[a]);
            }
            if (cells > 8192) continue;
            // count, then fill (each static index lands in its cells in ascending order: i runs ascending)
            if (!Grow(start, capStart, cells + 1)) return false;
            std::memset(start, 0, sizeof(uint32_t) * (cells + 1));
            size_t total = 0;
            bool over = false;
            for (int i = 0; i < n && !over; ++i) {
                if (!isStatic[i]) continue;
                int c0[3], c1[3];
                for (int a = 0; a < 3; ++a) {
                    c0[a] = Coord(boxes[i].lo[a], o[a], inv, dim[a]);
                    c1[a] = Coord(boxes[i].hi[a], o[a], inv, dim[a]);
                    if (c0[a] < 0) c0[a] = 0;  // (cannot happen: lo >= the origin; kept against a degenerate box)
                    if (c1[a] > dim[a] - 1) c1[a] = dim[a] - 1;
                }
                for (int z = c0[2]; z <= c1[2]; ++z) {
                    for (int y = c0[1]; y <= c1[1]; ++y) {
                        for (int x = c0[0]; x <= c1[0]; ++x) {
                            ++start[(static_cast< size_t >(z) * dim[1] + y) * dim[0] + x + 1];
                            ++total;
                        }
                    }
                }
                over = total > maxItems;
            }
            if (over) continue;
            for (size_t c = 0; c < cells; ++c) start[c + 1] += start[c];
            if (!Grow(items, capItems, total ? total : 1)) return false;
            // (fill cursors: start[c] is advanced while filling, then shifted back)
            for (int i = 0; i < n; ++i) {
                if (!isStatic[i]) continue;
                int c0[3], c1[3];
                for (int a = 0; a < 3; ++a) {
                    c0[a] = Coord(boxes[i].lo[a], o[a], inv, dim[a]);
                    c1[a] = Coord(boxes[i].hi[a], o[a], inv, dim[a]);
                    if (c0[a] < 0) c0[a] = 0;
                    if (c1[a] > dim[a] - 1) c1[a] = dim[a] - 1;
                }
                for (int z = c0[2]; z <= c1[2]; ++z) {
                    for (int y = c0[1]; y <= c1[1]; ++y) {
                        for (int x = c0[0]; x <= c1[0]; ++x) {
                            items[start[(static_cast< size_t >(z) * dim[1] + y) * dim[0] + x]++] = static_cast< uint16_t >(i);
                        }
                    }
                }
            }
            for (size_t c = cells; c > 0; --c) start[c] = start[c - 1];
            start[0] = 0;
            usable = true;
            return true;
        }
        return false;
    }

    // The static indices filed in p's cell: [*pBegin, *pEnd) of items (empty for a point outside the grid). p must be finite.
    void Cell(float x, float y, float z, const uint16_t** pBegin, const uint16_t** pEnd) const {
        const int cx = Coord(x, o[0], inv, dim[0]), cy = Coord(y, o[1], inv, dim[1]), cz = Coord(z, o[2], inv, dim[2]);
        if (cx < 0 || cy < 0 || cz < 0 || cx >= dim[0] || cy >= dim[1] || cz >= dim[2]) {
            *pBegin = *pEnd = items;
            return;
        }
        const size_t c = (static_cast< size_t >(cz) * dim[1] + cy) * dim[0] + cx;
        *pBegin = items + start[c];
        *pEnd = items + start[c + 1];
    }
};

// Walks the merge of a cell's static indices and the dynamic ones in ascending order; f(index) returns true to stop.
template < class F > inline bool ForEachAscending(const Grid& g, const uint16_t* sb, const uint16_t* se, F f) {
    const uint16_t* db = g.dyn;
    const uint16_t* de = g.dyn + g.nDyn;
    while (sb != se || db != de) {
        int i;
        if (db == de || (sb != se && *sb < *db)) {
            i = *sb++;
        } else {
            i = *db++;
        }
        if (f(i)) return true;
    }
    return false;
}
// The same, descending (a walk from the array's end).
template < class F > inline bool ForEachDescending(const Grid& g, const uint16_t* sb, const uint16_t* se, F f) {
    const uint16_t* db = g.dyn;
    const uint16_t* de = g.dyn + g.nDyn;
    while (sb != se || db != de) {
        int i;
        if (db == de || (sb != se && se[-1] > de[-1])) {
            i = *--se;
        } else {
            i = *--de;
        }
        if (f(i)) return true;
    }
    return false;
}

}  // namespace lgrid
}  // namespace port
