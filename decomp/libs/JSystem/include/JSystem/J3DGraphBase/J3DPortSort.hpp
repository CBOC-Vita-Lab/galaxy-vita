#pragma once

// (starbatch25, 12 Nov 2026) GXV_STARBATCH: the plan of a drawer's draw order. Header-only and free of the game's types (as J3DPortDL.hpp,
// whose analysis of a state list it uses): tests/starbatch_logic_test.cpp drives these functions, DrawBuffer.cpp calls them.
//
// What is planned. DrawBufferShapeDrawer::draw sends, per drawer (one material of one model, for every actor of the model in the buffer):
// the material's list, then per packet (one actor's shape) its differed list (the instance's colours, texture matrices: MR::initDLMakerMatColor0
// is what gives every star bit its colour) and the shape (matrices, geometry). Packets in the drawer's order, which is the order the actors
// were added in: star bits of six colours come out as an arbitrary sequence of colours, and every change of colour is a differed list sent,
// state the front end rebuilds (light / colour block, key hashing) and a draw record the back end cannot join to the one before it
// (GXV_RIGIDMERGE / GXV_STATIC_INST join only draws that differ in their matrix).
// The plan reorders the packets of one drawer (inside the stretch between two light loads) so that packets with the same differed list bytes
// are adjacent: one list per colour, then the front end's own cuts (GXV_J3D_DIFFSKIP, the render-state cache, the record joins) apply to
// the runs.
//
// Why the picture is the same. A packet is a unit: its differed list, then its shape. The units commute when
//   (1) the draws are opaque with depth test and depth write on (compare LESS / LEQUAL / GREATER / GEQUAL), no blending, no logic op: of
//       two units the one nearer wins at every pixel whatever the order (colour and alpha planes alike); the only exception is two
//       fragments of different units at exactly the same depth and pixel (an ASSUMPTION, stated here and in the report: two separate
//       collectibles are not coincident surfaces);
//   (2) every unit writes every register any unit writes (the "skeleton": the same sequence of register addresses and masks, data aside),
//       so that the state a unit draws with is a function of its own bytes (and of what precedes the first unit), never of the unit before;
//   (3) the first packet of a stretch stays first (its light load, and the material list merged with the first packet's differed list,
//       belong to it) and the last stays last, so that the registers the drawer leaves behind for the next drawer are the original's.
// Not planned (the drawer is drawn as it always was; the reason is counted): fewer packets than the limit, a differed list that is not
// "skippable" (J3DPortDiffSkippable: pure, no CP register, no vertex-format / matrix-index register, no position / normal matrix memory), one
// that writes a pixel-engine register (z mode, blend, destination alpha, pixel control), differed lists whose skeletons differ, packets with
// different shapes, a multi-matrix or enveloped shape, a differed texture matrix object or user area, a material list that is not an opaque
// depth-writing one, more groups or packets than the limits.

#include <stdint.h>
#include <string.h>

#include "J3DPortDL.hpp"

enum {
    J3DPortSort_MaxPackets = 256,  // a stretch of more active packets than this is not planned (the plan's arrays are on the stack)
    J3DPortSort_MaxGroups = 32     // more distinct differed lists than this: not planned
};

// Why a stretch was not (re)ordered. Ok: it was. Same: all its differed lists are equal already (nothing to do).
enum J3DPortSortWhy {
    J3DPortSortWhy_Ok = 0,
    J3DPortSortWhy_Same,
    J3DPortSortWhy_Few,
    J3DPortSortWhy_Many,
    J3DPortSortWhy_Groups,
    J3DPortSortWhy_Impure,
    J3DPortSortWhy_PixelRegs,
    J3DPortSortWhy_Skeleton,
    J3DPortSortWhy_Shape,
    J3DPortSortWhy_TexMtx,
    J3DPortSortWhy_MatList,
    J3DPortSortWhy_User,
    J3DPortSortWhy_Count
};

inline const char* J3DPortSortWhyName(int w) {
    static const char* const n[J3DPortSortWhy_Count] = {"ok", "same", "few", "many", "groups", "impure", "pixelregs", "skeleton", "shape", "texmtx", "matlist", "user"};
    return (w >= 0 && w < J3DPortSortWhy_Count) ? n[w] : "?";
}

namespace j3dportsort {

inline uint32_t be24(const uint8_t* p) { return (static_cast< uint32_t >(p[0]) << 16) | (static_cast< uint32_t >(p[1]) << 8) | p[2]; }

inline uint32_t Mix(uint32_t h, uint32_t v) {
    h ^= v;
    h *= 16777619u;
    return h;
}

// The bytes of a list as one 32-bit hash (words, then the tail bytes).
inline uint32_t HashBytes(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u ^ n;
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t w;
        memcpy(&w, p + i, 4);
        h = (h ^ w) * 16777619u;
        h ^= h >> 15;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

// The skeleton of a pure state list: the sequence of what it writes, not the values (BP: register, and for the texture-environment colour
// registers the half-select bit, and the value of a mask write; CP: register; XF: address and count). false when the list is not parseable
// as NOPs, BP / CP / XF writes only. *pe is set when it writes a pixel-engine register: 0x40 z mode, 0x41 blend / logic, 0x42 destination
// alpha, 0x43 pixel control (and the zero-time mask 0xFE does not make it one).
inline bool Skeleton(const uint8_t* p, uint32_t size, uint32_t* hash, uint32_t* pe) {
    uint32_t h = 0x811C9DC5u;
    uint32_t pix = 0;
    const uint8_t* const end = p + size;
    while (p < end) {
        const uint8_t cmd = *p;
        if (cmd == 0x00) {
            ++p;
            continue;
        }
        if (cmd == 0x61) {
            if (end - p < 5) return false;
            const uint8_t reg = p[1];
            h = Mix(h, 0x6100u | reg);
            if (reg == 0xFE) h = Mix(h, be24(p + 2));  // (the mask: which bits the next write sets)
            else if (reg >= 0xE0 && reg <= 0xE7) h = Mix(h, p[2] >> 7);
            if (reg >= 0x40 && reg <= 0x43) pix = 1;
            p += 5;
            continue;
        }
        if (cmd == 0x08) {
            if (end - p < 6) return false;
            h = Mix(h, 0x0800u | p[1]);
            p += 6;
            continue;
        }
        if (cmd == 0x10) {
            if (end - p < 5) return false;
            const uint32_t count = (static_cast< uint32_t >(p[1]) << 8) | p[2];
            if (count > 0xF || static_cast< uint32_t >(end - p) < 5u + 4u * (count + 1u)) return false;
            h = Mix(h, 0x1000u | count);
            h = Mix(h, (static_cast< uint32_t >(p[3]) << 8) | p[4]);
            p += 5u + 4u * (count + 1u);
            continue;
        }
        return false;
    }
    *hash = h;
    *pe = pix;
    return true;
}

// Whether the material list leaves the draws that follow opaque with depth test and depth write: it writes the z mode (BP 0x40) with the
// compare enabled (bit 0), the function one of LESS / LEQUAL / GREATER / GEQUAL (bits 1..3) and the update enabled (bit 4), and the blend
// mode (BP 0x41) with blending (bit 0) and the logic op (bit 1) off, all bits known: set by whole writes or by masked ones that cover them.
// A list that is not NOPs and register writes, or leaves any of them to an earlier list, is not one.
inline bool MatListOpaqueDepth(const uint8_t* p, uint32_t size) {
    uint32_t val[2] = {0, 0}, known[2] = {0, 0};
    uint32_t mask = 0x00FFFFFFu;
    const uint8_t* const end = p + size;
    while (p < end) {
        const uint8_t cmd = *p;
        if (cmd == 0x00) {
            ++p;
            continue;
        }
        if (cmd == 0x61) {
            if (end - p < 5) return false;
            const uint8_t reg = p[1];
            const uint32_t data = be24(p + 2);
            p += 5;
            if (reg == 0xFE) {
                mask = data;
                continue;
            }
            if (reg == 0x40 || reg == 0x41) {
                const int k = reg - 0x40;
                val[k] = (val[k] & ~mask) | (data & mask);
                known[k] |= mask;
            }
            mask = 0x00FFFFFFu;
            continue;
        }
        if (cmd == 0x08) {
            if (end - p < 6) return false;
            p += 6;
            continue;
        }
        if (cmd == 0x10) {
            if (end - p < 5) return false;
            const uint32_t count = (static_cast< uint32_t >(p[1]) << 8) | p[2];
            if (count > 0xF || static_cast< uint32_t >(end - p) < 5u + 4u * (count + 1u)) return false;
            p += 5u + 4u * (count + 1u);
            continue;
        }
        return false;
    }
    if ((known[0] & 0x1Fu) != 0x1Fu || (known[1] & 3u) != 3u) return false;
    const uint32_t z = val[0];
    const uint32_t func = (z >> 1) & 7u;
    if (!(z & 1u) || !(z & 0x10u) || !(func == 1 || func == 3 || func == 4 || func == 6)) return false;
    return (val[1] & 3u) == 0;
}

// One packet of a stretch: its differed list's bytes (null, 0: none).
struct In {
    const uint8_t* dl;
    uint32_t size;
};

struct Info {
    uint32_t groups;      // distinct differed lists in the stretch
    uint32_t runsBefore;  // maximal runs of equal lists in the order given
    uint32_t runsAfter;   // ... in the order planned
    uint32_t recBefore;   // GL draws the runs make at `slots` draws a record (the palette's size), before and after
    uint32_t recAfter;
};

inline uint32_t Records(const uint32_t* runLen, uint32_t n, uint32_t slots) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < n; ++i) r += (runLen[i] + slots - 1) / slots;
    return r;
}

// The plan of a stretch of m packets (their differed lists in `in`, in the drawer's order): the order to send them in, as indices into `in`
// (order[0..m)). pinFirst: in[0] must stay first (it is the stretch's first packet: its light load, and the merged material list's differed
// list, are its). The last stays last always. Returns Ok (order written), Same (one group: nothing to do, order = identity) or the reason
// it is left alone (order = identity). `slots`: the record size the estimate in info uses (10: GXV_RIGIDMERGE=1).
inline int Plan(const In* in, uint32_t m, bool pinFirst, uint16_t* order, Info* info, uint32_t slots, uint32_t minPackets) {
    for (uint32_t i = 0; i < m; ++i) order[i] = static_cast< uint16_t >(i);
    if (m < minPackets || m < 3) return J3DPortSortWhy_Few;
    if (m > J3DPortSort_MaxPackets) return J3DPortSortWhy_Many;
    uint32_t hash[J3DPortSort_MaxPackets];
    uint8_t group[J3DPortSort_MaxPackets];
    uint32_t head[J3DPortSort_MaxGroups];
    uint32_t nGroups = 0;
    for (uint32_t i = 0; i < m; ++i) {
        hash[i] = HashBytes(in[i].dl, in[i].size);
        uint32_t g = 0;
        for (; g < nGroups; ++g) {
            const In& h = in[head[g]];
            if (hash[head[g]] == hash[i] && h.size == in[i].size && (h.dl == in[i].dl || in[i].size == 0 || memcmp(h.dl, in[i].dl, in[i].size) == 0)) break;
        }
        if (g == nGroups) {
            if (nGroups == J3DPortSort_MaxGroups) return J3DPortSortWhy_Groups;
            head[nGroups++] = i;
        }
        group[i] = static_cast< uint8_t >(g);
    }
    // the runs as given
    uint32_t runLen[J3DPortSort_MaxPackets];
    uint32_t nRuns = 0;
    for (uint32_t i = 0; i < m; ++i) {
        if (i > 0 && group[i] == group[i - 1]) ++runLen[nRuns - 1];
        else runLen[nRuns++] = 1;
    }
    if (info != nullptr) {
        info->groups = nGroups;
        info->runsBefore = nRuns;
        info->runsAfter = nGroups;
        info->recBefore = Records(runLen, nRuns, slots);
        uint32_t cnt[J3DPortSort_MaxGroups] = {};
        for (uint32_t i = 0; i < m; ++i) ++cnt[group[i]];
        info->recAfter = Records(cnt, nGroups, slots);
    }
    if (nGroups == 1) return J3DPortSortWhy_Same;
    // the lists: skippable, no pixel-engine register, one skeleton
    uint32_t sk0 = 0;
    for (uint32_t g = 0; g < nGroups; ++g) {
        const In& h = in[head[g]];
        if (h.size == 0 || h.dl == nullptr) {
            // (no list at all, and others with one: their skeletons differ)
            return J3DPortSortWhy_Skeleton;
        }
        if (!J3DPortDiffSkippable(J3DPortScanDL(h.dl, h.size))) return J3DPortSortWhy_Impure;
        uint32_t sk, pe;
        if (!Skeleton(h.dl, h.size, &sk, &pe)) return J3DPortSortWhy_Impure;
        if (pe) return J3DPortSortWhy_PixelRegs;
        if (g == 0) sk0 = sk;
        else if (sk != sk0) return J3DPortSortWhy_Skeleton;
    }
    // the order: groups by first appearance, the group of the last packet at the end; a group that holds both the pinned first packet and
    // the last is split (its other members first, the last packet at the very end)
    const uint32_t gl = group[m - 1];
    const uint32_t g0 = group[0];
    uint32_t o = 0;
    const bool split = (gl == g0 && pinFirst);
    uint32_t gOrder[J3DPortSort_MaxGroups];
    uint32_t ng = 0;
    uint8_t seen[J3DPortSort_MaxGroups] = {};
    for (uint32_t i = 0; i < m; ++i) {
        const uint32_t g = group[i];
        if (seen[g]) continue;
        seen[g] = 1;
        if (g == gl && !split) continue;  // (moved to the end)
        gOrder[ng++] = g;
    }
    for (uint32_t k = 0; k < ng; ++k) {
        const uint32_t g = gOrder[k];
        for (uint32_t i = 0; i < m; ++i)
            if (group[i] == g && !(split && i == m - 1)) order[o++] = static_cast< uint16_t >(i);
    }
    if (split) {
        order[o++] = static_cast< uint16_t >(m - 1);
    } else {
        for (uint32_t i = 0; i < m; ++i)
            if (group[i] == gl) order[o++] = static_cast< uint16_t >(i);
    }
    if (info != nullptr) info->runsAfter = nGroups + (split ? 1u : 0u);
    if (info != nullptr && split) {
        uint32_t cnt[J3DPortSort_MaxGroups + 1] = {};
        for (uint32_t i = 0; i < m; ++i) ++cnt[group[i]];
        --cnt[gl];
        cnt[nGroups] = 1;
        info->recAfter = Records(cnt, nGroups + 1, slots);
    }
    return J3DPortSortWhy_Ok;
}

// The checks that make the plan trustworthy, for GXV_STARBATCH=2 and the tests: order is a permutation of 0..m-1, the first stays first when
// pinned, the last stays last, and the packets of one list are one run (or two for the split group).
inline bool CheckPlan(const In* in, uint32_t m, bool pinFirst, const uint16_t* order) {
    if (m == 0 || m > J3DPortSort_MaxPackets) return false;
    uint8_t seen[J3DPortSort_MaxPackets] = {};
    for (uint32_t k = 0; k < m; ++k) {
        if (order[k] >= m || seen[order[k]]) return false;
        seen[order[k]] = 1;
    }
    if (pinFirst && order[0] != 0) return false;
    if (order[m - 1] != m - 1) return false;
    return true;
}

}  // namespace j3dportsort

// The sorting state counters (DrawBuffer.cpp portDrawPort; logged with the "j3d lists:" line, J3DPacket.cpp).
struct J3DPortSortStats {
    uint32_t drawers;            // drawers considered (enough packets, GXV_STARBATCH on)
    uint32_t stretches;          // stretches between two light loads considered
    uint32_t packets;            // active packets in the stretches considered
    uint32_t sorted;             // stretches reordered (GXV_STARBATCH=1) / planned (=2)
    uint32_t sortedPackets;      // packets in them
    uint32_t same;               // stretches with one list already
    uint32_t runsBefore, runsAfter;  // equal-list runs, summed over the sorted stretches
    uint32_t recBefore, recAfter;    // GL records those runs make (palette of 10), summed
    uint32_t why[J3DPortSortWhy_Count];  // stretches (and packets in whyPackets) left alone, by reason
    uint32_t whyPackets[J3DPortSortWhy_Count];
    uint32_t verifyBad;          // GXV_STARBATCH=2: plans that failed the checks (and packets drawn in one pass only)
    uint32_t verifyInst;         // GXV_STARBATCH=2 (starbatch26): packets compared between the own-order pass and the planned pass
    uint32_t verifyInstBad;      // ... of them, with a different stream or differed list
    uint32_t verifyListBad;      // ... packets whose differed list in force was not their own (a wrong skip)
    uint32_t verifyUncomparable; // ... packets whose stream changed segment under the shape draw (not compared)
    uint32_t verifyLogged;       // (log lines written)
    uint64_t passOldNs, passNewNs;  // GXV_TURN_PROF on: ns the drawer's packet loop took in the drawer's own order (mode 2) / in the planned order (mode 1; mode 2's second pass)
    uint32_t passOldN, passNewN;    // ... the packets they sent
    uint64_t planNs;             // ns the planner took (GXV_TURN_PROF on)
};
