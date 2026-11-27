// GXV_HRW (hrw26, 12 Nov 2026): "half-rate world, full-rate camera". Pure logic, header only (no GL, no runtime): tests/hrw26_test.cpp compiles it as it is.
//
// The idea: of a pair of game turns, the first draws a picture (A: 21.6 ms on the game thread at Honeyhive), the second runs its logic only (8.7 ms). Today the
// pair gives ONE picture (30 a second). With HRW the pair gives TWO: picture B is the retained command buffer of A drawn AGAIN, geometrically re-projected to the
// camera of the turn after A, then shown on the next refresh.
//
// Why a projection patch is enough (the whole mechanism of this file):
//   GX carries one projection (GXSetProjection, XF registers 0x20..0x26) and a model-view matrix per draw that the game composed on its side (J3D: view x model).
//   The front end turns the projection into the uniform block UB_PROJ (BuildProjC: column major, Y flip and depth remap folded in) and keeps every vertex in the
//   EYE space of the camera the game used for the picture (also the CPU-transformed "soft" draws: "vertices arrive in eye space already"). So with
//       D = V_B * V_A^-1        (3x4, eye space of A -> eye space of B; V = the game's camera view matrix, world -> eye)
//   a point p of the world that picture A put at e_A = V_A p is at e_B = D e_A for the camera of B, and
//       clip_B = P * e_B = (P * D4) * e_A          (D4 = D with the row 0 0 0 1)
//   i.e. replacing every perspective projection block P of the retained buffer by P * D4 re-projects the whole retained scene, real geometry, real depth,
//   sharp, and costs 16 floats of arithmetic per distinct projection block (a picture has 3 to 10), not a pass over the draws. Lighting, fog, texture
//   generation and everything else the vertex shader computes from the eye-space position keep the values of A (they are consistent with each other);
//   only the final projection moves. Orthographic projections (the 2D layers) and draws made between the HUD markers are never touched.
//
// What is not covered here and why (REPORT.txt): a fresh Mario / fresh near actors (stage d, e) need a second, partial picture merged into the retained buffer.
//
// Retention contract (back end, render thread; vgx_galaxy.cpp RenderMain, vgx_backend_gl.cpp ExecuteFrame):
//   * the retained buffer is the buffer A was executed from; executing it again is allowed when Scan() finds no hazard: the replay skips A's resource
//     commands (uploads, frees, shaders, programs), because their effects are in place, and A's frees are deferred until the replay is done;
//   * Plan::Apply patches the blocks, the buffer is executed, Plan::Restore puts the bytes back: the buffer is never left changed (GXV_HRW=2 hashes it
//     before and after every replay and counts differences: that is the "verify-still" proof that the retention is complete and the back end does not mutate it).
#pragma once
#include "vgx_cmd.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vgx {
namespace hrw {

// GXV_HRW: 0 off (default), 1 on, 2 verify-still (B = A again, D = identity, hashes compared), 3 census only (nothing is retained or replayed),
// 4 verify-fresh (hrw27: on a drawn turn the game also makes the partial draw of a pair's second picture, from the same state; the render thread compares the owners' draws of the two paths).
enum Mode : int { kOff = 0, kOn = 1, kVerifyStill = 2, kCensus = 3, kVerifyFresh = 4, kWarp = 5 };  // (hrw31: 5 = the second picture is the first one's finished image re-projected in one full-screen pass)
inline int ParseMode(const char* e) {
    if (e == nullptr || e[0] == 0 || e[1] != 0) return kOff;
    return e[0] >= '1' && e[0] <= '5' ? e[0] - '0' : kOff;
}

// ------------------------------------------------------------------ 3x4 affine matrices, row major (GX Mtx; the missing row is 0 0 0 1)
inline void Ident(float m[12]) {
    for (int i = 0; i < 12; ++i) m[i] = 0.0f;
    m[0] = m[5] = m[10] = 1.0f;
}
inline bool IsIdent(const float m[12]) {
    static const float one[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    return std::memcmp(m, one, sizeof one) == 0;
}
// o = a * b (o may not alias a or b)
inline void Mul(const float a[12], const float b[12], float o[12]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) o[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c] + (c == 3 ? a[r * 4 + 3] : 0.0f);
}
// o = a^-1 for an affine matrix of any (non-singular) 3x3 part, not only a rigid one (a camera with a shake or a zoom is still inverted exactly enough).
// false when singular or not finite.
inline bool Invert(const float a[12], float o[12]) {
    const double m00 = a[0], m01 = a[1], m02 = a[2], m10 = a[4], m11 = a[5], m12 = a[6], m20 = a[8], m21 = a[9], m22 = a[10];
    const double c00 = m11 * m22 - m12 * m21, c01 = m12 * m20 - m10 * m22, c02 = m10 * m21 - m11 * m20;
    const double det = m00 * c00 + m01 * c01 + m02 * c02;
    if (!(std::fabs(det) > 1e-18) || !std::isfinite(det)) return false;
    const double id = 1.0 / det;
    const double i00 = c00 * id, i10 = c01 * id, i20 = c02 * id;
    const double i01 = (m02 * m21 - m01 * m22) * id, i11 = (m00 * m22 - m02 * m20) * id, i21 = (m01 * m20 - m00 * m21) * id;
    const double i02 = (m01 * m12 - m02 * m11) * id, i12 = (m02 * m10 - m00 * m12) * id, i22 = (m00 * m11 - m01 * m10) * id;
    const double tx = a[3], ty = a[7], tz = a[11];
    o[0] = static_cast<float>(i00); o[1] = static_cast<float>(i01); o[2] = static_cast<float>(i02); o[3] = static_cast<float>(-(i00 * tx + i01 * ty + i02 * tz));
    o[4] = static_cast<float>(i10); o[5] = static_cast<float>(i11); o[6] = static_cast<float>(i12); o[7] = static_cast<float>(-(i10 * tx + i11 * ty + i12 * tz));
    o[8] = static_cast<float>(i20); o[9] = static_cast<float>(i21); o[10] = static_cast<float>(i22); o[11] = static_cast<float>(-(i20 * tx + i21 * ty + i22 * tz));
    for (int i = 0; i < 12; ++i) if (!std::isfinite(o[i])) return false;
    return true;
}
// D = V_B * V_A^-1 (computed in double, then rounded once). Identical views give exactly the identity.
inline bool Delta(const float vA[12], const float vB[12], float d[12]) {
    if (std::memcmp(vA, vB, 12 * sizeof(float)) == 0) { Ident(d); return true; }
    float ia[12];
    if (!Invert(vA, ia)) return false;
    double A[12], B[12];
    for (int i = 0; i < 12; ++i) { A[i] = ia[i]; B[i] = vB[i]; }
    // (the inverse was rounded to float once; the product is in double so D carries one rounding of the inverse plus one of the result)
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) d[r * 4 + c] = static_cast<float>(B[r * 4 + 0] * A[0 * 4 + c] + B[r * 4 + 1] * A[1 * 4 + c] + B[r * 4 + 2] * A[2 * 4 + c] + (c == 3 ? B[r * 4 + 3] : 0.0));
    for (int i = 0; i < 12; ++i) if (!std::isfinite(d[i])) return false;
    return true;
}
// The size of a camera change: rotation in degrees (angle of the 3x3), translation length in game units (of the 4th column).
inline void Motion(const float d[12], float* rotDeg, float* trans) {
    double c = (static_cast<double>(d[0]) + d[5] + d[10] - 1.0) * 0.5;
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    if (rotDeg) *rotDeg = static_cast<float>(std::acos(c) * 57.29577951308232);
    if (trans) *trans = static_cast<float>(std::sqrt(static_cast<double>(d[3]) * d[3] + static_cast<double>(d[7]) * d[7] + static_cast<double>(d[11]) * d[11]));
}
// A rigid actor delta for stage (d0) (not wired yet): the world-space change of an actor between A and B, dW = W_B * W_A^-1, seen in A's eye space:
// E = V_A * dW * V_A^-1. The actor's draws of the retained buffer get E on their model-view blocks ON TOP of the projection patch (which already carries D).
inline bool ActorEyeDelta(const float vA[12], const float wA[12], const float wB[12], float e[12]) {
    float iwa[12], iva[12], dw[12], t[12];
    if (!Invert(wA, iwa) || !Invert(vA, iva)) return false;
    Mul(wB, iwa, dw);
    Mul(vA, dw, t);
    Mul(t, iva, e);
    return true;
}

// ------------------------------------------------------------------ projection blocks (UB_PROJ: 16 floats, column major: p[col * 4 + row])
// BuildProjC: perspective has p[11] = -1 and p[15] = 0; orthographic has p[15] = 1 and p[11] = 0.
inline bool IsPerspective(const float p[16]) { return p[11] == -1.0f && p[15] == 0.0f; }
// out = P * D4, D4 = [d; 0 0 0 1]. (clip = P * (D * e): the camera change applied before the projection)
inline void ProjMulD(const float p[16], const float d[12], float out[16]) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            const double d0 = d[0 * 4 + c], d1 = d[1 * 4 + c], d2 = d[2 * 4 + c], d3 = c == 3 ? 1.0 : 0.0;
            out[c * 4 + r] = static_cast<float>(static_cast<double>(p[0 * 4 + r]) * d0 + static_cast<double>(p[1 * 4 + r]) * d1 +
                                                static_cast<double>(p[2 * 4 + r]) * d2 + static_cast<double>(p[3 * 4 + r]) * d3);
        }
}

// ------------------------------------------------------------------ owners (hrw27)
// The game names the owner of a run of draws with PORT_GX_MARK_OWNER: a 16-bit key, id << 8 | phase. The id says whose draws they are, the phase which pass of the picture they are in
// (Mario's body, his decorations' opaque and translucent passes, a group's opaque or translucent pass ...), so that a fresh run goes back where its retained run was in the pass order.
enum OwnerIdValue : uint8_t {
    kOwnNone = 0,
    kOwnMario = 1,       // MarioActor::draw (DrawType_Player) and his decorations (PlayerDecoration groups)
    kOwnPointer = 2,     // the star pointer's cursor (a 2D draw: never patched, drawn fresh at every picture)
    kOwnSky = 0xFE,      // sky / air / sun buffers: camera-parented, re-projected with the rotation only
    kOwnFirstSet = 16,   // the significance set's actors: 16 .. 0xFD, handed out per pair
    kOwnLastSet = 0xFD,
};
enum OwnerPhase : uint8_t {
    kPhPlayerDraw = 1,       // CategoryList::execute(DrawType_Player)
    kPhPlayerDecoOpa = 2,    // PlayerDecoration, opaque
    kPhPlayerDecoXlu = 3,    // PlayerDecoration, translucent
    kPhPointer = 4,
    kPhSky = 5,
    kPhGroupOpa = 0x20,      // + the draw-buffer type (below 0x40): a set member's draws in that group's opaque pass
    kPhGroupXlu = 0x60,      // + the draw-buffer type: ... translucent pass
};
constexpr uint16_t OwnerKey(uint8_t id, uint8_t phase) { return static_cast<uint16_t>((static_cast<uint32_t>(id) << 8) | phase); }
constexpr uint8_t OwnerId(uint16_t key) { return static_cast<uint8_t>(key >> 8); }
constexpr uint8_t OwnerPhaseOf(uint16_t key) { return static_cast<uint8_t>(key & 0xFF); }
// o = d without its translation (the rotation / scale part only). o may not alias d.
inline void RotOnly(const float d[12], float o[12]) {
    for (int i = 0; i < 12; ++i) o[i] = d[i];
    o[3] = o[7] = o[11] = 0.0f;
}

// ------------------------------------------------------------------ the retained buffer's projection plan
struct Plan {
    struct Ref { uint32_t off; float orig[16]; };
    std::vector<Ref> refs;          // the perspective, non-2D blocks (distinct offsets) that Apply patches
    std::vector<Ref> skyRefs;       // (hrw27) ... those only the sky's draws use (owner kOwnSky): they get the rotation-only delta, the sky is parented to the camera
    uint32_t sky = 0;               // draws of the sky (patched with the rotation only)
    uint32_t skyShared = 0;         // sky draws whose projection block another kind of draw uses as well: they get the full delta with the rest (counted)
    uint32_t draws = 0;             // draws in the buffer
    uint32_t persp = 0;             // ... drawn with a patched block
    uint32_t ortho = 0;             // ... with an orthographic block (never patched)
    uint32_t hud = 0;               // ... flagged 2D (DrawRec::hud): never patched, whatever their projection
    uint32_t noProj = 0;            // ... with no projection block
    uint32_t inPass = 0;            // (census) patched draws that sit inside an offscreen pass (CMD_PASS_BEGIN .. its copy)
    uint32_t badRange = 0;          // blocks that do not lie inside the uniform arena
    bool conflict = false;          // a block is used by both kinds of draw, or two blocks overlap: the buffer cannot be replayed
    bool applied = false;

    void Clear() { refs.clear(); skyRefs.clear(); sky = skyShared = 0; draws = persp = ortho = hud = noProj = inPass = badRange = 0; conflict = applied = false; }

    bool Collect(const CmdBuf& b) {
        Clear();
        std::vector<uint32_t> patch, keep, skyBlk;
        uint32_t lastPatch = kNoBlock, lastKeep = kNoBlock;
        bool pass = false;
        const size_t nd = b.draws.size();
        for (const Cmd& c : b.cmds) {
            if (c.type == CMD_PASS_BEGIN) { pass = true; continue; }
            if (c.type == CMD_COPY_TEX) { if (c.arg < b.copyTex.size() && b.copyTex[c.arg].fromPass && !b.copyTex[c.arg].passMore) pass = false; continue; }
            if (c.type == CMD_COPY_DISP || c.type == CMD_SET_TARGET || c.type == CMD_PRESENT) { pass = false; continue; }
            if (c.type != CMD_DRAW || c.arg >= nd) continue;
            const DrawRec& d = b.draws[c.arg];
            ++draws;
            const uint32_t pb = d.ublock[UB_PROJ];
            if (pb == kNoBlock) { ++noProj; continue; }
            if (pb > b.uni.size() || b.uni.size() - pb < 16) { ++badRange; conflict = true; continue; }
            const bool persp3d = !d.hud && IsPerspective(b.uni.data() + pb);
            if (d.hud) ++hud;
            else if (!persp3d) ++ortho;
            if (persp3d && OwnerId(d.owner) == kOwnSky) {
                ++persp;
                ++sky;
                if (pass) ++inPass;
                skyBlk.push_back(pb);
            } else if (persp3d) {
                ++persp;
                if (pass) ++inPass;
                if (pb != lastPatch) { patch.push_back(pb); lastPatch = pb; }
            } else if (pb != lastKeep) {
                keep.push_back(pb);
                lastKeep = pb;
            }
        }
        std::sort(patch.begin(), patch.end());
        patch.erase(std::unique(patch.begin(), patch.end()), patch.end());
        std::sort(keep.begin(), keep.end());
        keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
        // (hrw27) a sky block that another kind of draw uses too keeps the full delta (the sky would otherwise move with the rest wrongly or the rest with the sky's rotation only)
        std::sort(skyBlk.begin(), skyBlk.end());
        skyBlk.erase(std::unique(skyBlk.begin(), skyBlk.end()), skyBlk.end());
        {
            std::vector<uint32_t> only;
            for (uint32_t off : skyBlk) {
                const bool inPatch = std::binary_search(patch.begin(), patch.end(), off), inKeep = std::binary_search(keep.begin(), keep.end(), off);
                if (inKeep) { ++skyShared; conflict = true; }       // (also used by a draw that must not move: the buffer cannot be replayed)
                else if (inPatch) ++skyShared;                      // (also used by 3D draws: the block gets the full delta)
                else only.push_back(off);
            }
            skyBlk.swap(only);
        }
        // a block of both kinds, or blocks that overlap (an in-place patch would change a neighbour)
        for (size_t i = 0; i < patch.size(); ++i) {
            if (i && patch[i] < patch[i - 1] + 16) conflict = true;
            const auto it = std::lower_bound(keep.begin(), keep.end(), patch[i] > 15 ? patch[i] - 15 : 0u);
            if (it != keep.end() && *it < patch[i] + 16) conflict = true;
        }
        for (size_t i = 0; i < skyBlk.size(); ++i) {
            if (i && skyBlk[i] < skyBlk[i - 1] + 16) conflict = true;
            const auto ip = std::lower_bound(patch.begin(), patch.end(), skyBlk[i] > 15 ? skyBlk[i] - 15 : 0u);
            if (ip != patch.end() && *ip < skyBlk[i] + 16) conflict = true;
            const auto ik = std::lower_bound(keep.begin(), keep.end(), skyBlk[i] > 15 ? skyBlk[i] - 15 : 0u);
            if (ik != keep.end() && *ik < skyBlk[i] + 16) conflict = true;
        }
        for (uint32_t off : skyBlk) {
            Ref r;
            r.off = off;
            std::memcpy(r.orig, b.uni.data() + off, sizeof r.orig);
            skyRefs.push_back(r);
        }
        refs.reserve(patch.size());
        for (uint32_t off : patch) {
            Ref r;
            r.off = off;
            std::memcpy(r.orig, b.uni.data() + off, sizeof r.orig);
            refs.push_back(r);
        }
        return !conflict;
    }
    // Patch the blocks in place for the camera change d. Identity: nothing is written (the bytes stay exactly A's).
    // (hrw27) the sky's blocks get the rotation of d only (RotOnly): the sky is parented to the camera, its eye-space positions do not move with the camera's translation.
    void Apply(CmdBuf& b, const float d[12]) {
        if (applied || IsIdent(d)) return;
        for (const Ref& r : refs) {
            float out[16];
            ProjMulD(r.orig, d, out);
            std::memcpy(b.uni.data() + r.off, out, sizeof out);
        }
        if (!skyRefs.empty()) {
            float dr[12];
            RotOnly(d, dr);
            for (const Ref& r : skyRefs) {
                float out[16];
                ProjMulD(r.orig, dr, out);
                std::memcpy(b.uni.data() + r.off, out, sizeof out);
            }
        }
        applied = true;
    }
    void Restore(CmdBuf& b) {
        if (!applied) return;
        for (const Ref& r : refs) std::memcpy(b.uni.data() + r.off, r.orig, sizeof r.orig);
        for (const Ref& r : skyRefs) std::memcpy(b.uni.data() + r.off, r.orig, sizeof r.orig);
        applied = false;
    }
};

// ------------------------------------------------------------------ is the buffer safe to execute twice?
// The replay skips the resource commands, so a texture or buffer id that the buffer re-uploads after a draw used it (the draw would see the new
// contents in the replay), or frees and then uploads again (the deferred free would delete the new object), cannot be replayed.
struct Hazard {
    uint32_t setTarget = 0;        // CMD_SET_TARGET in the buffer (the EFB changes size)
    uint32_t texReuploadUsed = 0;  // a texture re-uploaded after a draw of this buffer sampled it
    uint32_t texReuseAfterFree = 0;
    uint32_t bufReuploadUsed = 0;
    uint32_t bufReuseAfterFree = 0;
    uint32_t idRange = 0;          // an id too large for the tables
    uint32_t dispCopies = 0;       // display copies: exactly one is a picture
    bool Ok() const { return !setTarget && !texReuploadUsed && !texReuseAfterFree && !bufReuploadUsed && !bufReuseAfterFree && !idRange && dispCopies == 1; }
};
struct Scratch { std::vector<uint8_t> tex, buf; };
inline bool Scan(const CmdBuf& b, Hazard& h, Scratch& s) {
    h = Hazard();
    constexpr uint32_t kMaxId = 1u << 20;
    auto at = [&](std::vector<uint8_t>& v, uint32_t id) -> uint8_t* {
        if (id >= kMaxId) { ++h.idRange; return nullptr; }
        if (id >= v.size()) v.resize(id + 256, 0);
        return &v[id];
    };
    s.tex.assign(s.tex.size(), 0);
    s.buf.assign(s.buf.size(), 0);
    const size_t nd = b.draws.size();
    for (const Cmd& c : b.cmds) {
        switch (c.type) {
        case CMD_DRAW:
            if (c.arg < nd) {
                const DrawRec& d = b.draws[c.arg];
                for (int i = 0; i < 8; ++i)
                    if (d.tex[i].texId) { if (uint8_t* p = at(s.tex, d.tex[i].texId)) if (*p == 0) *p = 1; }
                if (d.vbuf) { if (uint8_t* p = at(s.buf, d.vbuf)) if (*p == 0) *p = 1; }
            }
            break;
        case CMD_TEX_UPLOAD:
            if (c.arg < b.uploads.size()) if (uint8_t* p = at(s.tex, b.uploads[c.arg].texId)) { if (*p == 1) ++h.texReuploadUsed; else if (*p == 2) ++h.texReuseAfterFree; }
            break;
        case CMD_TEX_FREE: if (uint8_t* p = at(s.tex, c.arg)) *p = 2; break;
        case CMD_BUF_UPLOAD:
            if (c.arg < b.bufUploads.size()) if (uint8_t* p = at(s.buf, b.bufUploads[c.arg].bufId)) { if (*p == 1) ++h.bufReuploadUsed; else if (*p == 2) ++h.bufReuseAfterFree; }
            break;
        case CMD_BUF_FREE: if (uint8_t* p = at(s.buf, c.arg)) *p = 2; break;
        case CMD_SET_TARGET: ++h.setTarget; break;
        case CMD_COPY_DISP: ++h.dispCopies; break;
        default: break;
        }
    }
    return h.Ok();
}

// What the replay leaves out of the retained buffer's command list: everything that creates or destroys a resource or compiles something. Their effects are
// in place after A; TEX_FREE / BUF_FREE are deferred (not skipped) by the back end until the replay is done.
inline bool SkipInReplay(uint8_t type) {
    return type == CMD_TEX_UPLOAD || type == CMD_TEX_FREE || type == CMD_BUF_UPLOAD || type == CMD_BUF_FREE || type == CMD_SHADER || type == CMD_PROGRAM ||
           type == CMD_VARIANT;
}

// ------------------------------------------------------------------ the partial draw merged into the retained buffer (hrw27, stage d)
// The pair's second picture is the retained buffer A with (1) the projection of every 3D draw re-projected (Plan) and (2) the draws of the owners the game drew again for the pair's own
// camera (a partial draw F: Mario, the star pointer, the significance set) taken out of A and the fresh ones put in their place. "Their place" is the position of the owner's first retained
// draw in the command list: A's other commands (passes, copies, the draws between) keep their order, so blending and depth order stay those of picture A. An owner key (id << 8 | phase) that F
// has and A has not (the owner was not drawn in A) is not inserted; one that A has and F has not stays as retained.
// The fresh draws' data (vertices, indices, uniform blocks, chunk starts) is appended to A's arenas and the fresh DrawRecs rebased; Uncompose truncates it all and puts the command list back,
// so A's buffer is byte for byte what it was (the hash of verify-still checks it). The buffer must have room (capacity) so that no arena moves.
struct Undo {
    std::vector<Cmd> cmds;      // after Compose: the retained buffer's own command list (swapped back by Uncompose)
    size_t draws = 0, vtx = 0, idx = 0, uni = 0, chunks = 0;
    bool active = false;
};
enum Refusal : int { kComposeOk = 0, kNoFresh, kNoAnchor, kNoRoom, kChunkMismatch, kHazardTex, kHazardBuf, kOldScheme, kRefusalCount };
inline const char* RefusalName(int r) {
    static const char* const n[kRefusalCount] = {"ok", "no fresh draws", "no owner in common", "no room in the arenas", "vertex chunk lists differ", "texture re-uploaded", "buffer re-uploaded", "old vertex scheme"};
    return r >= 0 && r < kRefusalCount ? n[r] : "?";
}
struct ComposeStats {
    uint32_t freshKeys = 0, matchedKeys = 0, orphanKeys = 0;  // owner keys F has; of them A has too; A has not
    uint32_t removed = 0, inserted = 0, orphanDraws = 0;       // A's draws taken out, F's put in, F's left out (no anchor)
    uint32_t unownedFresh = 0;                                 // F's draws that no marker named (left out)
    uint32_t vtxBytes = 0, idxElems = 0, uniFloats = 0;        // appended
};
inline bool KeyHas(const std::vector<uint16_t>& v, uint16_t k) { return std::binary_search(v.begin(), v.end(), k); }
inline size_t Align(size_t n, size_t a) { return (n + a - 1) / a * a; }
inline int Compose(CmdBuf& base, const CmdBuf& fresh, Undo& u, ComposeStats& st, Scratch& s) {
    st = ComposeStats();
    u.active = false;
    // F's owners
    std::vector<uint16_t> keys;
    const size_t nfd = fresh.draws.size(), nbd = base.draws.size();
    for (const Cmd& c : fresh.cmds) {
        if (c.type != CMD_DRAW || c.arg >= nfd) continue;
        const uint16_t k = fresh.draws[c.arg].owner;
        if (k == 0) ++st.unownedFresh; else keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    st.freshKeys = static_cast<uint32_t>(keys.size());
    if (keys.empty()) return kNoFresh;
    // A's anchors (the first retained draw of each key) and what would be removed
    std::vector<uint16_t> matched;
    std::vector<size_t> anchorAt(keys.size(), ~static_cast<size_t>(0));
    for (size_t i = 0; i < base.cmds.size(); ++i) {
        const Cmd& c = base.cmds[i];
        if (c.type != CMD_DRAW || c.arg >= nbd) continue;
        const uint16_t k = base.draws[c.arg].owner;
        if (k == 0) continue;
        const auto it = std::lower_bound(keys.begin(), keys.end(), k);
        if (it == keys.end() || *it != k) continue;
        const size_t ki = static_cast<size_t>(it - keys.begin());
        if (anchorAt[ki] == ~static_cast<size_t>(0)) anchorAt[ki] = i;
    }
    for (size_t ki = 0; ki < keys.size(); ++ki)
        if (anchorAt[ki] != ~static_cast<size_t>(0)) matched.push_back(keys[ki]);
    st.matchedKeys = static_cast<uint32_t>(matched.size());
    st.orphanKeys = st.freshKeys - st.matchedKeys;
    if (matched.empty()) return kNoAnchor;
    // the vertex addressing must be the same scheme on both sides
    if (base.vtxChunks.empty() != fresh.vtxChunks.empty()) return kChunkMismatch;
    const bool chunks = !base.vtxChunks.empty();
    if (!chunks)
        for (const Cmd& c : fresh.cmds)
            if (c.type == CMD_DRAW && c.arg < nfd && fresh.draws[c.arg].vbuf == 0 && fresh.draws[c.arg].vtxFirst != 0) return kOldScheme;
    // F's uploads must not replace what A's draws sample (the replay sees the new contents otherwise)
    {
        s.tex.assign(s.tex.size(), 0);
        s.buf.assign(s.buf.size(), 0);
        auto mark = [](std::vector<uint8_t>& v, uint32_t id) { if (id < (1u << 20)) { if (id >= v.size()) v.resize(id + 256, 0); v[id] = 1; } };
        for (const Cmd& c : base.cmds) {
            if (c.type != CMD_DRAW || c.arg >= nbd) continue;
            const DrawRec& d = base.draws[c.arg];
            for (int i = 0; i < 8; ++i) if (d.tex[i].texId) mark(s.tex, d.tex[i].texId);
            if (d.vbuf) mark(s.buf, d.vbuf);
        }
        for (const TexUpload& up : fresh.uploads) if (up.texId < s.tex.size() && s.tex[up.texId]) return kHazardTex;
        for (const BufUpload& bu : fresh.bufUploads) if (bu.bufId < s.buf.size() && s.buf[bu.bufId]) return kHazardBuf;
    }
    // room
    const size_t vtx0 = Align(base.vtx.size(), 16), idx0 = base.idx.size(), uni0 = Align(base.uni.size(), 4);
    if (base.vtx.capacity() < vtx0 + fresh.vtx.size() || base.idx.capacity() < idx0 + fresh.idx.size() || base.uni.capacity() < uni0 + fresh.uni.size()) return kNoRoom;
    // the new command list
    std::vector<Cmd>& out = u.cmds;
    out.clear();
    out.reserve(base.cmds.size() + fresh.cmds.size());
    for (size_t i = 0; i < base.cmds.size(); ++i) {
        const Cmd& c = base.cmds[i];
        if (c.type == CMD_DRAW && c.arg < nbd) {
            const uint16_t k = base.draws[c.arg].owner;
            if (k != 0 && KeyHas(matched, k)) {
                const size_t ki = static_cast<size_t>(std::lower_bound(keys.begin(), keys.end(), k) - keys.begin());
                ++st.removed;
                if (anchorAt[ki] == i) {
                    for (const Cmd& f : fresh.cmds)
                        if (f.type == CMD_DRAW && f.arg < nfd && fresh.draws[f.arg].owner == k) {
                            out.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(nbd + f.arg)});
                            ++st.inserted;
                        }
                }
                continue;
            }
        }
        out.push_back(c);
    }
    for (const Cmd& f : fresh.cmds)
        if (f.type == CMD_DRAW && f.arg < nfd) {
            const uint16_t k = fresh.draws[f.arg].owner;
            if (k != 0 && !KeyHas(matched, k)) ++st.orphanDraws;
        }
    // the arenas and the rebased fresh records (every F record is appended in F's order, so that F's draw f is draw nbd + f; the ones no command names are never drawn)
    u.draws = nbd; u.vtx = base.vtx.size(); u.idx = base.idx.size(); u.uni = base.uni.size(); u.chunks = base.vtxChunks.size();
    base.vtx.resize(vtx0 + fresh.vtx.size());
    if (vtx0 > u.vtx) std::memset(base.vtx.data() + u.vtx, 0, vtx0 - u.vtx);
    if (!fresh.vtx.empty()) std::memcpy(base.vtx.data() + vtx0, fresh.vtx.data(), fresh.vtx.size());
    base.idx.resize(idx0 + fresh.idx.size());
    if (!fresh.idx.empty()) std::memcpy(base.idx.data() + idx0, fresh.idx.data(), fresh.idx.size() * sizeof(uint16_t));
    base.uni.resize(uni0 + fresh.uni.size());
    if (uni0 > u.uni) std::memset(base.uni.data() + u.uni, 0, (uni0 - u.uni) * sizeof(float));
    if (!fresh.uni.empty()) std::memcpy(base.uni.data() + uni0, fresh.uni.data(), fresh.uni.size() * sizeof(float));
    const uint32_t chunkBase = static_cast<uint32_t>(base.vtxChunks.size());
    for (uint32_t cs : fresh.vtxChunks) base.vtxChunks.push_back(cs + static_cast<uint32_t>(vtx0));
    base.draws.reserve(nbd + nfd);
    for (size_t f = 0; f < nfd; ++f) {
        DrawRec d = fresh.draws[f];
        if (d.vbuf == 0) {
            if (chunks) d.vchunk = static_cast<uint16_t>(d.vchunk + chunkBase);
            else d.vtxOffset += static_cast<uint32_t>(vtx0);
            d.idxOffset += static_cast<uint32_t>(idx0);
        }
        for (int b = 0; b < UB_COUNT; ++b) if (d.ublock[b] != kNoBlock) d.ublock[b] += static_cast<uint32_t>(uni0);
        base.draws.push_back(d);
    }
    st.vtxBytes = static_cast<uint32_t>(fresh.vtx.size());
    st.idxElems = static_cast<uint32_t>(fresh.idx.size());
    st.uniFloats = static_cast<uint32_t>(fresh.uni.size());
    std::swap(base.cmds, u.cmds);  // base: the composite; u.cmds: the retained list
    u.active = true;
    return kComposeOk;
}
inline void Uncompose(CmdBuf& base, Undo& u) {
    if (!u.active) return;
    std::swap(base.cmds, u.cmds);
    base.draws.resize(u.draws);
    base.vtx.resize(u.vtx);
    base.idx.resize(u.idx);
    base.uni.resize(u.uni);
    base.vtxChunks.resize(u.chunks);
    u.active = false;
}

// ------------------------------------------------------------------ verify-fresh (hrw27, stage h): where do an owner's draws land on the screen?
// The fresh path's and the full path's draws of the same owner are compared in order, by where a few probe points go: through the draw's model-view block (and the first matrices of its
// palette, for the skinned ones) and its projection block, in EFB pixels. Both paths draw the same state, so the difference is 0 for every draw if the partial draw is right.
struct Probe { uint16_t key; uint16_t n; float px[24][2]; };
inline bool ProjectPx(const float p[16], const float e[3], float w, float h, float out[2]) {
    // p is column major (BuildProjC), Y flip folded in: clip = P * (e, 1)
    const float cx = p[0] * e[0] + p[4] * e[1] + p[8] * e[2] + p[12];
    const float cy = p[1] * e[0] + p[5] * e[1] + p[9] * e[2] + p[13];
    const float cw = p[3] * e[0] + p[7] * e[1] + p[11] * e[2] + p[15];
    if (!(std::fabs(cw) > 1e-6f)) return false;
    out[0] = (cx / cw * 0.5f + 0.5f) * w;
    out[1] = (cy / cw * 0.5f + 0.5f) * h;
    return std::isfinite(out[0]) && std::isfinite(out[1]);
}
// The probes of one draw (up to 24 points). false: nothing to probe.
inline bool DrawProbe(const CmdBuf& b, const DrawRec& d, Probe& pr) {
    pr.key = d.owner;
    pr.n = 0;
    const uint32_t pb = d.ublock[UB_PROJ], mb = d.ublock[UB_MV], palb = d.ublock[UB_PAL];
    if (pb == kNoBlock || pb > b.uni.size() || b.uni.size() - pb < 16) return false;
    const float* P = b.uni.data() + pb;
    const float w = d.rs.vpW > 0 ? d.rs.vpW : 640.0f, h = d.rs.vpH > 0 ? d.rs.vpH : 528.0f;
    static const float cube[8][3] = {{-50, -50, -50}, {50, -50, -50}, {-50, 50, -50}, {50, 50, -50}, {-50, -50, 50}, {50, -50, 50}, {-50, 50, 50}, {50, 50, 50}};
    auto addMtx = [&](const float* m, int npts) {
        for (int i = 0; i < npts && pr.n < 24; ++i) {
            const float* c = cube[i];
            const float e[3] = {m[0] * c[0] + m[1] * c[1] + m[2] * c[2] + m[3], m[4] * c[0] + m[5] * c[1] + m[6] * c[2] + m[7], m[8] * c[0] + m[9] * c[1] + m[10] * c[2] + m[11]};
            float o[2];
            if (ProjectPx(P, e, w, h, o)) { pr.px[pr.n][0] = o[0]; pr.px[pr.n][1] = o[1]; ++pr.n; }
        }
    };
    if (mb != kNoBlock && mb <= b.uni.size() && b.uni.size() - mb >= 24) addMtx(b.uni.data() + mb, 8);
    if (palb != kNoBlock && palb <= b.uni.size() && b.uni.size() - palb >= 384) {
        const float* pal = b.uni.data() + palb;
        for (int m = 0; m < 21 && pr.n < 24; ++m) {  // (posMtx memory: 64 x vec4 = 21 matrices of 3 rows; the origin of each one that is not all zero)
            const float* r = pal + m * 12;
            bool any = false;
            for (int i = 0; i < 12; ++i) if (r[i] != 0.0f) { any = true; break; }
            if (any) addMtx(r, 1);
        }
    }
    if (pr.n == 0 && d.vbuf == 0 && d.vtxCount > 0 && mb == kNoBlock && !(d.layoutMask & VL_QALL)) {
        // a soft-transformed record: its first vertex is an eye-space point
        const VtxLayout lay = MakeLayout(d.layoutMask);
        const size_t chunkStart = (!b.vtxChunks.empty() && d.vchunk < b.vtxChunks.size()) ? b.vtxChunks[d.vchunk] : 0;
        const size_t at = chunkStart + static_cast<size_t>(d.vtxFirst) * lay.stride + d.vtxOffset;
        if (at + 12 <= b.vtx.size()) {
            float e[3];
            std::memcpy(e, b.vtx.data() + at, 12);
            float o[2];
            if (ProjectPx(P, e, w, h, o)) { pr.px[0][0] = o[0]; pr.px[0][1] = o[1]; pr.n = 1; }
        }
    }
    return pr.n > 0;
}
struct OwnerProbes { std::vector<Probe> v; };  // the probes of every owned draw of a buffer, in draw order
inline void CollectProbes(const CmdBuf& b, OwnerProbes& out) {
    out.v.clear();
    const size_t nd = b.draws.size();
    for (const Cmd& c : b.cmds) {
        if (c.type != CMD_DRAW || c.arg >= nd) continue;
        const DrawRec& d = b.draws[c.arg];
        if (d.owner == 0) continue;
        Probe pr;
        DrawProbe(b, d, pr);
        out.v.push_back(pr);
    }
}
struct CompareResult {
    uint32_t keys = 0, keysBadCount = 0;      // owner keys in either set; of them with a different number of draws
    uint32_t drawsCompared = 0, drawsBad = 0; // draws compared (ordinal i of a key against ordinal i); of them more than the tolerance apart
    double sumPx = 0, maxPx = 0;              // over the probe points compared
    uint32_t points = 0;
    uint16_t badKey = 0;                      // (hrw30) the first owner key whose draw counts differ, and the two counts (fresh, full)
    uint32_t badKeyA = 0, badKeyB = 0;
};
// a = the fresh path's, b = the full path's. tolPx 0.25: both paths draw the same state, so anything above float noise is a bug.
inline CompareResult CompareProbes(const OwnerProbes& a, const OwnerProbes& b, double tolPx = 0.25) {
    CompareResult r;
    std::vector<uint16_t> keys;
    for (const Probe& p : a.v) keys.push_back(p.key);
    for (const Probe& p : b.v) keys.push_back(p.key);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    r.keys = static_cast<uint32_t>(keys.size());
    for (uint16_t k : keys) {
        std::vector<const Probe*> pa, pb;
        for (const Probe& p : a.v) if (p.key == k) pa.push_back(&p);
        for (const Probe& p : b.v) if (p.key == k) pb.push_back(&p);
        if (pa.size() != pb.size()) {
            if (r.keysBadCount == 0) { r.badKey = k; r.badKeyA = static_cast<uint32_t>(pa.size()); r.badKeyB = static_cast<uint32_t>(pb.size()); }
            ++r.keysBadCount;
        }
        const size_t n = std::min(pa.size(), pb.size());
        for (size_t i = 0; i < n; ++i) {
            const Probe &x = *pa[i], &y = *pb[i];
            ++r.drawsCompared;
            double worst = 0;
            const uint16_t m = std::min(x.n, y.n);
            if (x.n != y.n) worst = 1e9;
            for (uint16_t j = 0; j < m; ++j) {
                const double dx = x.px[j][0] - y.px[j][0], dy = x.px[j][1] - y.px[j][1];
                const double d = std::sqrt(dx * dx + dy * dy);
                r.sumPx += d;
                ++r.points;
                if (d > worst) worst = d;
                if (d > r.maxPx) r.maxPx = d;
            }
            if (worst > tolPx) ++r.drawsBad;
        }
    }
    return r;
}

// ------------------------------------------------------------------ verify-still: a hash of everything the back end reads from a buffer
inline uint64_t Fnv(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}
template <typename V> inline uint64_t FnvVec(uint64_t h, const V& v) {
    const uint64_t n = v.size();
    h = Fnv(h, &n, sizeof n);
    return v.empty() ? h : Fnv(h, v.data(), v.size() * sizeof(typename V::value_type));
}
// (DrawRec's padding is whatever the front end left in it; the same stored object hashed twice gives the same value, which is all verify-still needs.)
inline uint64_t HashBuf(const CmdBuf& b) {
    uint64_t h = 0xCBF29CE484222325ull;
    h = FnvVec(h, b.cmds);
    h = FnvVec(h, b.draws);
    h = FnvVec(h, b.copyTex);
    h = FnvVec(h, b.copyDisp);
    h = FnvVec(h, b.passes);
    h = FnvVec(h, b.targets);
    h = FnvVec(h, b.vtx);
    h = FnvVec(h, b.idx);
    h = FnvVec(h, b.uni);
    h = FnvVec(h, b.vtxChunks);
    return h;
}

// ------------------------------------------------------------------ does a pair fit?  (the governor and the census use the same arithmetic)
struct Costs {
    double gameDrawnMs = 0;    // game thread, a turn that draws (21.6 at Honeyhive)
    double gameUndrawnMs = 0;  // game thread, a turn without a picture (8.7)
    double partialMs = 0;      // game thread, extra for B's fresh draw of Mario / HUD / near actors (stage d; 0 until built)
    double feMs = 0;           // front end, per picture of the kind that is parsed (A only: 19.9)
    double feBMs = 0;          // front end, B (a fresh partial picture, stage d; 0 for a pure replay)
    double beMs = 0;           // back end CPU, per picture (7 to 8), once for A and once for B
    double gpuMs = 0;          // GPU, per picture (14.5 .. 19.2): B costs the same as A
    double warpMs = 0;         // (gov5) > 0: the pair's B is mode 5's image warp, a cheap full-screen pass: B costs ~warpMs on the GPU and next to nothing on the game thread / front end
    double pairGpuMs = 0;      // (gov77) only while the pairs run: the GPU time of a whole pair as measured (the mean of a picture x pictures per pair); 0 = plain 60 / not measured
    double bShown = -1;        // (gov77) only while the pairs run: the share of the pairs whose second picture was shown (0..1); -1 = not known (too few pairs in the second)
};
struct FitParams {
    double periodMs = 1000.0 / 30.0;  // two refreshes
    double margin = 0.97;             // use at most this much of a period
    double gpuMargin = 0.95;          // the GPU is the one resource that cannot be overlapped away
};
struct Fit {
    double game = 0, fe = 0, be = 0, gpu = 0;  // per pair, ms
    bool gameOk = false, feOk = false, beOk = false, gpuOk = false;
    bool All() const { return gameOk && feOk && beOk && gpuOk; }
    // the first thing that does not fit, for the log ("game", "front end", "back end", "gpu"), or "fits"
    const char* Limit() const { return !gameOk ? "game thread" : !feOk ? "front end" : !beOk ? "back end" : !gpuOk ? "gpu" : "fits"; }
};
inline Fit PairFit(const Costs& c, const FitParams& p = FitParams()) {
    Fit f;
    f.game = c.gameDrawnMs + c.gameUndrawnMs + c.partialMs;
    f.fe = c.feMs + c.feBMs;
    f.be = 2.0 * c.beMs;
    f.gpu = 2.0 * c.gpuMs;
    if (c.warpMs > 0) {   // (gov5, GXV_HRW=5: B is the image warp of A's finished picture: one pass on the GPU, a small job on the back end; the replay's second full picture does not exist)
        f.be = c.beMs + 0.5 * c.warpMs;
        f.gpu = c.gpuMs + c.warpMs;
    }
    f.gameOk = f.game <= p.periodMs * p.margin;
    f.feOk = f.fe <= p.periodMs * p.margin;
    f.beOk = f.be <= p.periodMs * p.margin;
    f.gpuOk = f.gpu <= p.periodMs * p.gpuMargin;
    return f;
}

// ------------------------------------------------------------------ the governor: engage per stage only where 60 cannot be held natively, with hysteresis
// One sample a second (the caller says whether the second was play). Engages when, in `needFit` of the last `win` seconds, the picture rate WITHOUT HRW was under
// engageBelowFps and the pair fits; leaves when `needMiss` of the last `win` seconds had a miss share over missPct (replays dropped, late or not presentable)
// or the pair stopped fitting by more than `leaveSlack`. Never inside the first settleSec seconds of a stage, at most maxChanges a stage, gapSec apart.
// A change of state needs no fade of its own: HRW only adds pictures between two pictures of the 30 fps cadence; what must not pop is the fresh/retained
// membership of actors (stage e), which uses port/lockfade.h's Fader (per-actor staggered thresholds).
struct GovSample {
    bool play = false;
    double nativeFps = 0;   // pictures a second the player saw while HRW was off (while it is on: the pairs' own rate, 2 x pairs; not used for engaging)
    Costs costs;
    double missPct = 0;     // while engaged: replays not presented in time / dropped, percent of pairs
    bool ladderRoom = false;   // (gov5) the GPU ladder (GXV_PERF_GPU896) still has a step to try or one in proof: while the stage is GPU-limited it goes first
    bool engaged = false;      // (gov5) the sample was taken with the pairs running (the costs are then the pair's: see Params::leaveMs)
};
enum GovAction : int { kGovNone = 0, kGovEngage = 1, kGovLeave = 2 };
// hrw28: where the governor stands, for the Performance governors (GXV_FE_GOV / GXV_GAME_GOV, platform/src/port/perf.cpp): while HRW is wanted (pending, engaged, or declined because the pair does not fit)
// they aim at the PAIR's budgets (PairBudgets) instead of the 16.7 ms picture's: raising the cpulod level only as far as the pair needs, and giving it back when the pair still fits without it.
// Declined because the stage holds 60 natively (not needed) hands the governors back their ordinary budgets.
// (gov77) kGovGpuLimit: plain 60 is held back by the GPU, not the game thread: 60 Assist cannot help and is never taken; kGovBlocked: 60 Assist lost its second pictures or the GPU's
// budget in this stage and is not taken again until the stage ends. Neither counts as "wanted" for the Performance governors (ordinary budgets).
enum GovStatus : int { kGovOff = 0, kGovPending = 1, kGovEngaged = 2, kGovNotNeeded = 3, kGovNoFit = 4, kGovGpuLimit = 5, kGovBlocked = 6 };
inline const char* GovStatusName(int s) {
    static const char* const n[7] = {"off", "pending", "engaged", "declined (the stage holds 60 natively)", "declined (the pair does not fit)", "declined (the GPU is the limit: Assist cannot help)", "declined (blocked for the stage)"};
    return s >= 0 && s < 7 ? n[s] : "?";
}
class Governor {
public:
    struct Params {
        double engageBelowFps = 54.0;   // (gov5: 55)
        int win = 6, needFit = 4;
        double settleSec = 12.0, gapSec = 20.0;
        int maxChanges = 4;
        double missPct = 15.0;
        int needMiss = 3;
        double leaveSlack = 1.06;  // the pair must exceed its budget by 6 % to leave on fit alone
        FitParams fit;
        // (gov5, GXV_HRW_GOV5: the governor also drives mode 5. "60 Assist is a step down from full 60": it comes only where plain 60 cannot be held and goes when it can)
        bool warp = false;
        double ladderGpuMs = 15.0;     // the stage counts as GPU-limited (the ladder can help) from this per-picture GPU time
        double ladderWaitSec = 12.0;   // the most the ladder is waited for after the settle: every step is proved in ~2 s, it has up to 5; then the pair is taken whatever it says
        int needLeave = 4;             // seconds of the window that must say "plain 60 would hold" to leave (engaging takes needFit: leaving is the slower decision)
        double leaveMs = 17.5;         // plain 60 is predicted to hold (>= ~57 fps) when the game thread's drawn turn and the front end are under this
        double leaveGpuMs = 17.0;      // ... and the GPU time of a first picture (read off the pair's mean, dearly) under this: the GPU cannot be overlapped away
        double leaveBackoff = 0.94;    // every leave that had to be taken back (engaged again within relapseSec) makes the next one stricter by this factor
        double relapseSec = 20.0;      // (the gap and the window already keep a re-engage 8 + 7 s away at the soonest)
        // (gov77, GXV_HRW_GOV77) 60 Assist helps only where the GAME THREAD keeps plain 60 from holding, and only where the GPU has room for the pair's extra picture.
        // Console night57: Dreadnought plain 47.8 fps with the game thread at 10.3 ms and the GPU at 19.3 ms; the old rule engaged it (native under 55 + "the pair fits") and the
        // pair came out at 30 fps with 0-24 % of the second pictures shown: the GPU, not the game thread, was the limit.
        bool gov77 = false;
        double engageGameMs = 17.0;    // plain 60 cannot be held by the game thread when its drawn turn or the front end is at or over this (the 16.7 ms picture); under it only the GPU can be the limit
        double warpDefaultMs = 2.0;    // the second picture's GPU time until it is measured (learned from the pairs' own seconds)
        double bMinShown = 0.90;       // engaged: under this share of second pictures shown, two seconds in a row, plain 60 at once and no Assist for the rest of the stage
        int bMinSecs = 2;              // ... the seconds in a row (the first second after a changeover is not counted)
        // (gov24, GXV_GOV77_FIX=1) the "limited by the GPU" verdict says what it measured: an unmeasured figure (0 = unknown) is never "GPU-limited", the GPU counts as the limit only when its
        // measured time is at or over gpuLimitMs, and a drop with neither the game thread nor the GPU over budget is called that. The engage rule itself is not touched (those seconds never counted for it).
        bool fix77 = false;
        // (gov24, GXV_GOV77_PAIRPAD=1) the engage test asks for the game thread's time a PAIR really takes: night58 Beach Bowl predicted 31.2 ms (limit 32.3) and measured 33.3 (drawn 22.6 + undrawn 10.7; 57 of 81 pairs over
        // 32.3), so the speed guard gave up 48 of 81 second pictures and the governor left after 3 s. pairPadMs is the measured excess (+2.1), added to the plain stage's drawn + undrawn in the engage test only.
        double pairPadMs = 0;          // 0 = off (as c51)
        double gpuLimitMs = 16.0;      // a measured GPU time a picture from this is "the GPU is the limit" (the 16.7 ms picture less the display's share); Dreadnought 19-24, Honeyhive 15.3 is not
    };
    void Configure(const Params& p) { p_ = p; if (p_.win > kMax) p_.win = kMax; if (p_.win < 1) p_.win = 1; }
    void StageBegin(double nowSec) { stageT0_ = nowSec; lastChange_ = -1e9; changes_ = 0; relapse_ = 0; lastWasLeave_ = false; engaged_ = false; n_ = 0; active_ = true; why_[0] = 0; status_ = kGovPending; lastFit_ = Fit(); blocked_ = false; engRun_ = 0; plainGpu_ = 0; verdict_ = kVNone; verdictWhy_[0] = 0; }
    // (gov77) why the governor stays on plain 60 (or on Assist), one line, and a code that changes when the reason does: the caller logs a line at each change
    enum Verdict : int { kVNone = 0, kVSettling, kVHolds, kVGpuLimit, kVNoGpuFigure, kVNoFit, kVLadder, kVBlocked, kVAssistOn, kVEngageOk, kVNeither };
    int VerdictCode() const { return verdict_; }
    const char* VerdictWhy() const { return verdictWhy_; }
    bool Blocked() const { return blocked_; }
    // (mp30, port/hrw_mp.h) the multiplayer policy drives the governor's state from outside: the last leave was "plain 60 would hold again", lifting the block of a stage, and an engage or a leave
    // the policy makes itself (the state, the gap and the relapse count as for the governor's own; the window starts empty; not counted against maxChanges).
    bool LeftBecausePlainHolds() const { return leaveHolds_; }
    void Unblock() { blocked_ = false; if (status_ == kGovBlocked) status_ = kGovPending; verdict_ = kVNone; }
    void External(double nowSec, bool on, const char* why) {
        if (!active_) return;
        if (on && lastWasLeave_ && nowSec - lastChange_ < p_.relapseSec) ++relapse_;
        lastWasLeave_ = !on && engaged_;   // (a policy move out of a state the governor was not in is no leave: it must not inflate relapse_)
        leaveHolds_ = false;
        engaged_ = on;
        status_ = kGovPending;
        lastChange_ = nowSec;
        n_ = 0;
        engRun_ = 0;
        std::snprintf(why_, sizeof why_, "%s", why ? why : "");
    }
    double LearnedWarpMs() const { return warpLearn_; }
    void StageEnd() { active_ = false; engaged_ = false; n_ = 0; }
    bool Engaged() const { return engaged_; }
    int Status() const { return !active_ ? kGovOff : engaged_ ? kGovEngaged : status_; }
    const Fit& LastFit() const { return lastFit_; }
    int Changes() const { return changes_; }
    const char* Why() const { return why_; }
    GovAction Tick(double nowSec, const GovSample& s) {
        if (!active_) return kGovNone;
        if (!s.play) { n_ = 0; engRun_ = 0; return kGovNone; }
        if (n_ == p_.win) { for (int i = 1; i < n_; ++i) ring_[i - 1] = ring_[i]; n_ = p_.win - 1; }
        ring_[n_++] = s;
        // (gov77) the plain picture's GPU time, smoothed, for the pair's extra cost; the pair's own seconds teach the second picture's GPU time
        if (p_.gov77) {
            if (!engaged_ && !s.engaged && s.costs.gpuMs > 0) plainGpu_ = plainGpu_ > 0 ? 0.7 * plainGpu_ + 0.3 * s.costs.gpuMs : s.costs.gpuMs;
            engRun_ = engaged_ && s.engaged ? engRun_ + 1 : 0;
            if (engaged_ && s.engaged && engRun_ >= 2 && plainGpu_ > 0 && s.costs.pairGpuMs > 0 && s.costs.bShown >= p_.bMinShown) {
                const double extra = std::clamp(s.costs.pairGpuMs - plainGpu_, 0.5, 12.0);
                warpLearn_ = warpLearn_ > 0 ? 0.7 * warpLearn_ + 0.3 * extra : extra;
            }
        }
        Costs cNow = s.costs;
        if (p_.gov77 && p_.warp && warpLearn_ > 0 && !engaged_) cNow.warpMs = warpLearn_;
        else if (p_.gov77 && p_.warp && cNow.warpMs <= 0) cNow.warpMs = p_.warpDefaultMs;
        lastFit_ = PairFit(cNow, p_.fit);
        if (engaged_ && p_.gov77 && p_.warp && engRun_ >= 1 + p_.bMinSecs) {
            // (gov77) Assist is on and not paying: leave at once (no window, no gap), and not again in this stage
            const double gpuLim = p_.fit.periodMs * p_.fit.gpuMargin * p_.leaveSlack;
            int low = 0, over = 0;
            for (int i = n_ - p_.bMinSecs; i < n_; ++i) {
                if (ring_[i].costs.bShown >= 0 && ring_[i].costs.bShown < p_.bMinShown) ++low;
                if (ring_[i].costs.pairGpuMs > gpuLim) ++over;
            }
            if (low >= p_.bMinSecs || over >= p_.bMinSecs) {
                const Costs& c = ring_[n_ - 1].costs;
                if (low >= p_.bMinSecs)
                    Change(nowSec, false, "60 Assist loses its second pictures: shown %.0f %% of the pairs (under %.0f %%) in %d seconds in a row; plain 60 now and no Assist for the rest of the stage", 100.0 * c.bShown, 100.0 * p_.bMinShown, p_.bMinSecs);
                else
                    Change(nowSec, false, "60 Assist is over the GPU's budget: %.1f ms a pair (over %.1f) in %d seconds in a row; plain 60 now and no Assist for the rest of the stage", c.pairGpuMs, gpuLim, p_.bMinSecs);
                blocked_ = true;
                status_ = kGovBlocked;
                lastWasLeave_ = true;
                return kGovLeave;
            }
        }
        if (nowSec - stageT0_ < p_.settleSec || n_ < p_.win || changes_ >= p_.maxChanges || nowSec - lastChange_ < p_.gapSec) {
            if (p_.gov77 && !engaged_ && !blocked_) SetVerdict(kVSettling, "settling (stage start, window or gap not over)");
            return kGovNone;
        }
        if (!engaged_) {
            int ok = 0, notNeeded = 0, noFit = 0, ladder = 0, gpuLimited = 0, noGpu = 0, neither = 0;
            Fit last;
            const bool ladderWaits = p_.warp && nowSec - stageT0_ < p_.settleSec + p_.ladderWaitSec;
            for (int i = 0; i < n_; ++i) {
                Costs ci = ring_[i].costs;
                if (p_.gov77 && p_.warp) ci.warpMs = warpLearn_ > 0 ? warpLearn_ : (ci.warpMs > 0 ? ci.warpMs : p_.warpDefaultMs);
                if (p_.gov77 && p_.warp && p_.pairPadMs > 0 && ci.partialMs <= 0) ci.partialMs = p_.pairPadMs;   // (gov24: plain samples only: the ring is emptied at every changeover)
                last = PairFit(ci, p_.fit);
                const bool need = ring_[i].nativeFps < p_.engageBelowFps, fit = last.All();
                if (p_.gov77 && p_.warp && need) {
                    if (blocked_) continue;
                    // the game thread (its drawn turn, the front end) must be what keeps plain 60 away: a GPU-limited second never counts for the pair
                    if (p_.fix77) {   // (gov24) the same three questions, each answered from a measured figure only
                        if (std::max(ci.gameDrawnMs, ci.feMs) <= 0 || ci.gpuMs <= 0) { ++noGpu; continue; }   // a figure that is 0 is unknown, not "under the limit"
                        if (std::max(ci.gameDrawnMs, ci.feMs) < p_.engageGameMs) { if (ci.gpuMs >= p_.gpuLimitMs) ++gpuLimited; else ++neither; continue; }
                    }
                    if (std::max(ci.gameDrawnMs, ci.feMs) < p_.engageGameMs) { ++gpuLimited; continue; }
                    if (ci.gpuMs <= 0) { ++noGpu; continue; }   // (the GPU's room is unmeasured: never assume it)
                }
                // (gov5) a GPU-limited second with a ladder step still to try belongs to the ladder (cheaper picture, 896, 848, edge off: each proved), not yet to the pair
                if (need && fit && ladderWaits && ring_[i].ladderRoom && ring_[i].costs.gpuMs >= p_.ladderGpuMs) { ++ladder; continue; }
                if (need && fit) ++ok;
                if (!need) ++notNeeded;
                else if (!fit) ++noFit;
            }
            // (hrw28) the verdict the Performance governors read: not needed (60 held), or needed and the pair does not fit, else still pending
            status_ = notNeeded > n_ - p_.needFit ? kGovNotNeeded : (noFit > n_ - p_.needFit ? kGovNoFit : kGovPending);
            if (p_.gov77 && p_.warp) {
                const Costs& c = ring_[n_ - 1].costs;
                if (blocked_) { status_ = kGovBlocked; SetVerdict(kVBlocked, "Assist is blocked for this stage (it lost its second pictures or went over the GPU's budget)"); }
                else if (p_.fix77 && noGpu > n_ / 2) SetVerdict(kVNoGpuFigure, "a figure is not measured yet (the GPU's or the game thread's time unknown in %d of %d seconds): no verdict, no Assist on a guess", noGpu, n_);
                else if (p_.fix77 && neither > n_ / 2) {
                    double fp[kMax], g[kMax], f[kMax], u[kMax];
                    int ng = 0;
                    for (int i = 0; i < n_; ++i) { fp[i] = ring_[i].nativeFps; g[i] = ring_[i].costs.gameDrawnMs; f[i] = ring_[i].costs.feMs; if (ring_[i].costs.gpuMs > 0) u[ng++] = ring_[i].costs.gpuMs; }
                    auto med = [](double* a, int k) { if (k <= 0) return 0.0; std::sort(a, a + k); return k & 1 ? a[k / 2] : 0.5 * (a[k / 2 - 1] + a[k / 2]); };
                    SetVerdict(kVNeither, "native rate under %.0f but neither the game thread nor the GPU is over budget (%d of %d seconds: game thread and front end under %.1f ms, GPU under %.1f ms; window median native %.1f fps, game %.1f, front end %.1f, GPU %.1f ms): the lost pictures come from elsewhere (pacing, hitches, the display), Assist cannot help",
                               p_.engageBelowFps, neither, n_, p_.engageGameMs, p_.gpuLimitMs, med(fp, n_), med(g, n_), med(f, n_), med(u, ng));
                }
                else if (p_.fix77 ? gpuLimited > n_ / 2 : (gpuLimited > n_ - p_.needFit || (gpuLimited + notNeeded > n_ - p_.needFit && gpuLimited > 0 && notNeeded <= n_ - p_.needFit))) {
                    status_ = kGovGpuLimit;
                    {   // (dd24) the figures are the window's MEDIANS (what the classification above counted over), not the last second's: the last second can be a spike that contradicts "both under"
                        double fp[kMax], g[kMax], f[kMax], u[kMax];
                        int ng = 0;
                        for (int i = 0; i < n_; ++i) { fp[i] = ring_[i].nativeFps; g[i] = ring_[i].costs.gameDrawnMs; f[i] = ring_[i].costs.feMs; if (ring_[i].costs.gpuMs > 0) u[ng++] = ring_[i].costs.gpuMs; }
                        auto med = [](double* a, int k) { if (k <= 0) return 0.0; std::sort(a, a + k); return k & 1 ? a[k / 2] : 0.5 * (a[k / 2 - 1] + a[k / 2]); };
                        SetVerdict(kVGpuLimit, "plain 60 is limited by the GPU, not the game thread (window median: native %.1f fps, game thread drawn %.1f ms and front end %.1f ms, %d of %d seconds under %.1f, GPU %.1f ms a picture over %d measured seconds; last second: game %.1f, front end %.1f): Assist cannot help",
                                   med(fp, n_), med(g, n_), med(f, n_), gpuLimited, n_, p_.engageGameMs, med(u, ng), ng, c.gameDrawnMs, c.feMs);
                    }
                } else if (noGpu > n_ - p_.needFit) SetVerdict(kVNoGpuFigure, "the GPU's time is not measured yet: no Assist on a guess");
                else if (status_ == kGovNotNeeded) SetVerdict(kVHolds, "plain 60 holds (native %.1f fps, game thread drawn %.1f ms)", ring_[n_ - 1].nativeFps, c.gameDrawnMs);
                else if (status_ == kGovNoFit) SetVerdict(kVNoFit, "the pair does not fit (limit: %s; game %.1f front end %.1f gpu %.1f ms)", last.Limit(), last.game, last.fe, last.gpu);
                else if (ladder > 0) SetVerdict(kVLadder, "the picture ladder goes first (GPU %.1f ms a picture)", c.gpuMs);
                else SetVerdict(kVSettling, "pending: %d of %d seconds say the game thread cannot hold plain 60 (need %d)", ok, n_, p_.needFit);
            }
            if (ok >= p_.needFit) {
                if (p_.warp && lastWasLeave_ && nowSec - lastChange_ < p_.relapseSec) ++relapse_;   // (gov5) the plain 60 we went back to did not hold: the next leave asks for more room
                if (p_.gov77 && p_.warp)
                    Change(nowSec, true, "native rate under %.0f in %d of %d seconds, the GAME THREAD is what holds plain 60 back (drawn %.1f ms, front end %.1f ms, limit %.1f) and the GPU has room for the pair (plain %.1f + second picture %.1f = %.1f of %.1f ms; game %.1f, front end %.1f, back end %.1f)",
                           p_.engageBelowFps, ok, n_, ring_[n_ - 1].costs.gameDrawnMs, ring_[n_ - 1].costs.feMs, p_.engageGameMs, ring_[n_ - 1].costs.gpuMs, last.gpu - ring_[n_ - 1].costs.gpuMs, last.gpu, p_.fit.periodMs * p_.fit.gpuMargin, last.game, last.fe, last.be);
                else
                Change(nowSec, true, "native rate under %.0f in %d of %d seconds%s and the pair fits (game %.1f, front end %.1f, back end %.1f, gpu %.1f ms)", p_.engageBelowFps, ok, n_,
                       p_.warp ? ", the picture ladder has nothing left to give here (or the stage is not GPU-limited)" : "", last.game, last.fe, last.be, last.gpu);
                lastWasLeave_ = false;
                return kGovEngage;
            }
            if (ladder > 0 && ok + ladder >= p_.needFit) status_ = kGovPending;
            return kGovNone;
        }
        if (p_.gov77 && p_.warp) SetVerdict(kVAssistOn, "Assist on (game thread drawn %.1f ms, B shown %.0f %%)", ring_[n_ - 1].costs.gameDrawnMs, ring_[n_ - 1].costs.bShown >= 0 ? 100.0 * ring_[n_ - 1].costs.bShown : -1.0);
        int miss = 0, nofit = 0, holds = 0;
        const double back = std::pow(p_.leaveBackoff, relapse_ > 4 ? 4 : relapse_);
        FitParams loose = p_.fit;
        loose.margin *= p_.leaveSlack;
        loose.gpuMargin *= p_.leaveSlack;
        for (int i = 0; i < n_; ++i) {
            if (ring_[i].missPct > p_.missPct) ++miss;
            if (!PairFit(ring_[i].costs, loose).All()) ++nofit;
            if (p_.warp && PlainHolds(ring_[i].costs, back)) ++holds;
        }
        if (miss >= p_.needMiss) { Change(nowSec, false, "%d of %d seconds with more than %.0f %% of the pairs missed", miss, n_, p_.missPct); return kGovLeave; }
        if (nofit >= p_.needFit) { Change(nowSec, false, "the pair stopped fitting in %d of %d seconds (%s)", nofit, n_, PairFit(ring_[n_ - 1].costs, loose).Limit()); lastWasLeave_ = true; return kGovLeave; }
        if (p_.warp && n_ >= p_.needLeave && holds >= p_.needLeave) {
            const Costs& c = ring_[n_ - 1].costs;
            Change(nowSec, false, "plain 60 would hold again in %d of %d seconds (game thread %.1f, front end %.1f, gpu of a picture ~%.1f ms; limits %.1f / %.1f ms)", holds, n_, c.gameDrawnMs, c.feMs, NativeGpuMs(c), p_.leaveMs * back, p_.leaveGpuMs * back);
            lastWasLeave_ = true;
            leaveHolds_ = true;
            return kGovLeave;
        }
        return kGovNone;
    }
    // (gov5) the GPU time of one first picture while the pairs run: the stage's figure is the mean of the pair's two pictures, B being cheap, so A's is read off it dearly (2 x mean - warp, never less than the mean)
    double NativeGpuMs(const Costs& c) const { return c.warpMs > 0 ? std::max(c.gpuMs, 2.0 * c.gpuMs - c.warpMs) : c.gpuMs; }
    // (gov5) would the stage hold 60 WITHOUT the pairs? Read from the pair's own costs: a drawn turn is a full turn of the game (logic + draw), the front end's per-picture time is the same work as native,
    // the GPU has to draw every picture in full. Unknown GPU / front end figures never say yes.
    bool PlainHolds(const Costs& c, double back) const {
        if (c.gpuMs <= 0 || c.gameDrawnMs <= 0) return false;
        return c.gameDrawnMs <= p_.leaveMs * back && c.feMs <= p_.leaveMs * back && NativeGpuMs(c) <= p_.leaveGpuMs * back;
    }

private:
    static constexpr int kMax = 16;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
// fmt1131: every caller passes a literal format; a template cannot carry the format attribute
    template <typename... A> void SetVerdict(int code, const char* fmt, A... a) {
        verdict_ = code;
        std::snprintf(verdictWhy_, sizeof verdictWhy_, fmt, a...);
    }
    template <typename... A> void Change(double nowSec, bool on, const char* fmt, A... a) {
        engaged_ = on;
        leaveHolds_ = false;
        status_ = kGovPending;
        lastChange_ = nowSec;
        ++changes_;
        n_ = 0;
        std::snprintf(why_, sizeof why_, fmt, a...);
    }
#pragma GCC diagnostic pop
    Params p_;
    GovSample ring_[kMax];
    int n_ = 0, changes_ = 0;
    double stageT0_ = 0, lastChange_ = -1e9;
    bool engaged_ = false, active_ = false;
    char why_[480] = {};
    int status_ = kGovPending;
    bool lastWasLeave_ = false;
    int relapse_ = 0;
    Fit lastFit_;
    bool blocked_ = false;      // (gov77) no Assist for the rest of the stage
    bool leaveHolds_ = false;   // (mp30) the last change was the leave "plain 60 would hold again"
    int engRun_ = 0;            // (gov77) engaged seconds in a row, counted from the first sample taken with the pairs running
    double plainGpu_ = 0;       // (gov77) the plain picture's GPU time, smoothed
    double warpLearn_ = 0;      // (gov77) the pair's extra GPU time over a plain picture as measured (kept across stages: the same console)
    int verdict_ = 0;
    char verdictWhy_[320] = {};
};

// hrw28: the budgets the Performance governors aim at while HRW is wanted: the pair's period (two refreshes) less what the pair costs besides the figure the governor measures. FE_GOV measures the
// front end's CPU per picture it parses (the first picture of each pair: the second is a replay or a small partial draw): its budget is the pair's. GAME_GOV measures the game thread's DRAWN turn:
// its budget is the pair's less the undrawn turn and the partial draw. Never below `floorMs` (a governor with an impossible target would raise to its top and stay).
struct PairBudgetsOut { double feMs = 0, gameDrawnMs = 0; };
inline PairBudgetsOut PairBudgets(const Costs& c, const FitParams& p = FitParams(), double floorMs = 10.0) {
    PairBudgetsOut o;
    const double period = p.periodMs * p.margin;
    o.feMs = period - c.feBMs;
    o.gameDrawnMs = period - c.gameUndrawnMs - c.partialMs;
    if (o.feMs < floorMs) o.feMs = floorMs;
    if (o.gameDrawnMs < floorMs) o.gameDrawnMs = floorMs;
    return o;
}

// ------------------------------------------------------------------ census
struct Census {
    uint64_t pairs = 0;          // drawn turns that opened a pair
    uint64_t bSent = 0;          // replay jobs asked for
    uint64_t bDropped = 0;       // ... dropped (queue full, no retained buffer, wrong id, hazard, plan conflict) by reason below
    uint64_t bRun = 0;           // replays executed
    uint64_t verifyBad = 0;      // verify-still: the buffer's hash changed across a replay
    uint64_t verifyRuns = 0;
    uint64_t why[8] = {};        // drop reasons: 0 queue, 1 no buffer, 2 id, 3 hazard, 4 conflict, 5 not presentable, 6 mode, 7 other
    uint64_t hazards[6] = {};    // setTarget, texReuploadUsed, texReuseAfterFree, bufReuploadUsed, bufReuseAfterFree, disp != 1
    uint64_t drawsSeen = 0, drawsPatched = 0, drawsOrtho = 0, drawsHud = 0, drawsInPass = 0, blocksPatched = 0;
    uint64_t sinceLog = 0;
};
inline const char* WhyName(int i) {
    static const char* const n[8] = {"queue full", "no retained buffer", "id mismatch", "hazard", "projection conflict", "not presentable", "mode", "other"};
    return i >= 0 && i < 8 ? n[i] : "?";
}

}  // namespace hrw
}  // namespace vgx
