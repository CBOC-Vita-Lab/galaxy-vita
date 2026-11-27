// GXV_J3DNATIVE=4 / 5 (j3n2: stage 2 of the J3D-native path). Shared by vgx_state.cpp (vgx_j3native2.inc: chains of lazy lists,
// checkpoints, materialisation) and vgx_draw.cpp (vgx_j3native2_draw.inc: templated resolutions of static runs). Player thread only.
//
// What it does (the full description is at the head of vgx_j3native2.inc):
//   A chain is a stretch of the top-level stream made only of J3D material / differed list calls (bracketed), other state-list calls,
//   retained shape calls and "live" writes (CP registers, XF matrix and light memory, indexed matrix loads). Inside a chain whose
//   lists are the ones recorded at the same place in an earlier picture (same lists, same bytes, same versions) and whose history (the
//   registers the chain does not write before its first draw) is the recorded one, the lists' register parts are not applied (only their
//   CP / XF-memory parts are), and each static run is drawn from a template: the resolved state and the register values its uniform
//   blocks read, captured when the same lazy register state was real. Anything else ends the chain: the register file is made real
//   (a recorded checkpoint copied, the lists after it replayed) and the resolver starts cold.
#pragma once
#include "vgx_state.h"

#include <cstdint>
#include <vector>

namespace vgx {
namespace j3n2 {

// The register words the uniform blocks of a static run read that lists write (split::Tev / Light / Mtx payloads): TEV registers and
// konst colours, the indirect matrices and scales (BP 0x06..0x0E, 0x25, 0x26), BP 0x42, fog and alpha (BP 0xEE..0xF4), the ambient and
// material colours (XF 0x100A..0x100D), the post-matrix indices (XF 0x1050..0x1057), the texture units' mode0 / mode1 (GXV_FIX_LODBIAS).
constexpr uint32_t kImgWords = 16 + 16 + 9 + 3 + 7 + 4 + 8 + 16;
struct Img {
    uint32_t w[kImgWords];
};
// What a template is good for: the chain's lazy-state version (the register file is a function of it), the vertex-matrix mask, the
// vertex descriptor words (CP 0x50 / 0x60: the vertex key reads the descriptor) and the palette class of the run.
struct TplKey {
    uint32_t v = 0, mask = 0, vcd0 = 0, vcd1 = 0;
    uint8_t palCls = 0, pad[3] = {};
    bool operator==(const TplKey& o) const { return v == o.v && mask == o.mask && vcd0 == o.vcd0 && vcd1 == o.vcd1 && palCls == o.palCls; }
};
struct Tpl {
    TplKey k;
    uint16_t fsId = 0, vsId = 0, useFs = 0;
    uint8_t texMask = 0, foldOk = 0, cullAll = 0, ok = 0;
    uint32_t img = 0;
    RenderState rs{};
    TexBind tex[8]{};
    uint16_t texW[8] = {}, texH[8] = {};
    uint32_t texRegs[8][5] = {};
};
struct TplSet {
    std::vector<Tpl> t;
    std::vector<Img> img;
    std::vector<int32_t> slots; // open addressing over t by key (-1 empty)
    void Clear() { t.clear(); img.clear(); slots.clear(); }
    static uint32_t HashOf(const TplKey& k) {
        uint32_t h = k.v * 0x9E3779B1u ^ k.mask * 0x85EBCA6Bu ^ k.vcd0 * 0xC2B2AE35u ^ k.vcd1 * 0x27D4EB2Fu ^ k.palCls;
        return h ^ (h >> 15);
    }
    Tpl* Find(const TplKey& k) {
        if (slots.empty()) return nullptr;
        const uint32_t m = static_cast<uint32_t>(slots.size()) - 1u;
        for (uint32_t i = HashOf(k) & m;; i = (i + 1) & m) {
            const int32_t s = slots[i];
            if (s < 0) return nullptr;
            if (t[static_cast<size_t>(s)].k == k) return &t[static_cast<size_t>(s)];
        }
    }
    Tpl* Add(const TplKey& k) {
        if ((t.size() + 1) * 2 > slots.size()) {
            const size_t n = slots.empty() ? 64 : slots.size() * 2;
            slots.assign(n, -1);
            for (size_t j = 0; j < t.size(); ++j) Place(t[j].k, static_cast<int32_t>(j));
        }
        t.emplace_back();
        t.back().k = k;
        Place(k, static_cast<int32_t>(t.size() - 1));
        return &t.back();
    }
    void Place(const TplKey& k, int32_t idx) {
        const uint32_t m = static_cast<uint32_t>(slots.size()) - 1u;
        uint32_t i = HashOf(k) & m;
        while (slots[i] >= 0) i = (i + 1) & m;
        slots[i] = idx;
    }
};
enum : int { kOff = 0, kRec = 1, kTpl = 2, kShadow = 3 };
// Set by the state side around a retained shape call of an active chain: EmitStaticRun resolves through it.
struct DrawCtx {
    int mode = kOff;
    TplSet* set = nullptr;
    uint32_t v = 0;
};
extern DrawCtx* g_draw;
// (vgx_state.cpp) a template that cannot be used (template mode): the register file is made real, the chain re-anchored at the current
// token; the caller then resolves the run the normal way. reason: 0 no template, 1 a texture, 2 a template not usable.
void Bail(int reason);
// (vgx_draw.cpp) the resolver's state after a materialisation: nothing it resolved before is trusted (as a picture's start).
void ResolverCold();
// (vgx_draw.cpp) whether the switches that read the register file per static run (outside the resolution) are off; why not if not.
bool DrawUsable(const char** why);
// (vgx_draw.cpp) the counters of the draw side, read by the log line.
struct DrawCtr {
    uint64_t templated = 0, bails[3] = {}, captured = 0, refused = 0, checked = 0, bad = 0, projSent = 0, lightSent = 0, tevSent = 0;
};
extern DrawCtr g_dctr;
// (vgx_texcache.cpp) the texture memo's answer for a template's registers: ResolveTexture's hit, with its side effects, and whether
// it is the binding expected; and (no side effects) whether a binding just resolved is one the memo would give again.
bool TexTouch(int m, const uint32_t r[5], const TexBind& expect, uint16_t w, uint16_t h);
bool TexKnown(int m, const uint32_t r[5], const TexBind& b, uint16_t w, uint16_t h);
void TexRegsOf(int m, uint32_t r[5]);

} // namespace j3n2
} // namespace vgx
