// GXV_BLOOM_BRACKET (bloom37, 21 Nov 2026): the dead bloom bracket. No GL, no SDK: the decision, the census and the log line, so
// tests/vgx_bloombracket_test.cpp can drive them without the game.
//
// BloomEffect::preDraw captures the whole scene into a texture (_24, RGBA8) and fills the EFB black; the glow models are drawn on the black;
// postDraw runs the glow chain and puts the scene back with one covering quad and adds the glow. When no glow source drew
// (ImageEffectLocalUtil::portBloomNothingDrawn: nothing at all was sent between the fill and postDraw), the glow is black and the
// bracket restores what was there: the capture splits the EFB's scene (store, copy scene, load), the fill and the put-back are two covering
// quads. The game marks the capture (PORT_GX_MARK_BLOOM_BRACKET, vgx_draw.cpp BloomBracketMarker), tags the fill's and the put-back's draws
// and says "dead" (the glow chain is made dry then); at the end of the picture the front end checks that dropping all three is an identity
// and, when it is, leaves the three commands out BEFORE any other pass looks at the picture (so the EFB scene runs through).
//
// Dropping the three is an identity when:
//   - the capture is one whole-rectangle 1:1 RGBA8 colour copy with the identity conversion, no clear, no depth, not made from a pass or an atlas;
//   - the fill and the put-back are each ONE draw, an unblended rectangle that writes every pixel of its extent (DrawRec::cover) and whose extent is
//     exactly the capture's rectangle (so the fill's black is covered by the put-back, pixel for pixel), neither tests nor writes depth, no stencil
//     work, no destination-alpha constant; they write the same planes: the put-back writes alpha wherever the fill did;
//   - between the capture and the put-back there is no other draw, copy, pass or target change (the game's own check, "nothing sent", is repeated here);
//   - nothing else of the picture samples the capture's texture (it is shared with the screen blur and the depth-of-field blur, which write it
//     before they read it, or read what the last writer left: a reader here means the texture's contents matter) and the texture is not read by
//     a later picture before it is captured again (the next bloom picture captures first; the half-rate reuse reads _3C only).
// The put-back is a texel-for-pixel copy of the capture (the same size, at the render scale), so it is the identity at every rung and with or
// without the upscaler; a multisampled EFB is not (the capture resolves it): refused. Not under GXV_HRW (the pairs replay a picture), and not in a
// diagnostic or capture frame (the back end publishes that in HostBlock).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace vgx::bb {

// The marker's sub-kinds (PORT_GX_MARK_BLOOM_BRACKET, bits 8-15; platform/include/port/compat.h PORT_BB_*)
enum Sub : uint32_t { kSubCapture = 1, kSubFillBegin = 2, kSubFillEnd = 3, kSubPutBegin = 4, kSubPutEnd = 5, kSubDead = 6 };

enum Reason : int {
    kDropped = 0,   // the bracket was dead and its capture, fill and put-back were left out (=2: would be)
    kLive,          // a glow source drew: the bracket is not dead (counted as a picture's bracket, not a refusal)
    kMarkers,       // the game's markers did not give a capture, a fill and a put-back that the picture's commands show
    kForeign,       // another draw, copy, pass or target change between the capture and the put-back
    kCopyShape,     // the capture is not a whole 1:1 RGBA8 colour copy with the identity conversion and no clear
    kDrawShape,     // the fill or the put-back is not one covering rectangle of the capture's extent (blend, depth, stencil, dst alpha, extent)
    kPlanes,        // they write different planes (the fill writes alpha, the put-back does not)
    kSharedRead,    // another draw of the picture samples the capture's texture
    kHrw,           // GXV_HRW is not 0
    kMsaa,          // multisampled EFB (the capture resolves it)
    kHost,          // a diagnostic or capture frame, or the picture's host facts say no
    kCount
};
inline const char* ReasonName(int r) {
    static const char* const n[kCount] = {"dropped", "live", "markers", "foreign command", "copy shape", "draw shape", "planes", "shared reader", "hrw", "msaa", "host frame"};
    return r >= 0 && r < kCount ? n[r] : "?";
}

// What the back end publishes for the front end (read with a relaxed load; a change reaches the front end a picture or two late, which only matters
// in the pictures around the change itself: a multisampled picture kept without the bracket is the sharper one, never a wrong one).
enum HostBits : uint32_t { kHostMsaa = 1, kHostFrame = 2 };
inline std::atomic<uint32_t>& HostBlock() {
    static std::atomic<uint32_t> b{0};
    return b;
}

struct CopyFacts {
    bool present = false;
    uint32_t texId = 0;
    bool depth = false, fromPass = false, srcTex = false, dstTex = false, fill = false, skip = false;
    bool clearEnable = false;
    uint8_t gxFmt = 0xFF;
    int x = 0, y = 0, w = 0, h = 0;   // the source rectangle
    int dstW = 0, dstH = 0;
    bool identity = false;            // mat = identity, add = 0 (four channels), or three channels when the EFB has no alpha
};
struct DrawFacts {
    bool present = false;
    bool cover = false;               // DrawRec::cover: one rectangle that writes every pixel of its extent (unblended, colour written, no alpha test)
    bool blend = false, depthTest = false, depthWrite = false, colorWrite = false, alphaWrite = false;
    uint8_t dstAlphaZero = 0, stOp = 0;
    int x = 0, y = 0, w = 0, h = 0;   // its extent
};
struct Inputs {
    int mode = 0;                     // GXV_BLOOM_BRACKET: 0 off, 1 on, 2 count only
    bool dead = false;                // the game said "dead" for this bracket
    bool efbAlpha = true;
    bool markersOk = false;           // the capture, the fill's draw and the put-back's draw were all found, in this order
    bool foreignBetween = false;      // a draw, copy, pass or target change between the capture and the put-back (not counting the two)
    bool sharedReader = false;        // another draw of the picture samples the capture's texture
    uint32_t host = 0;                // HostBlock()
    int hrw = 0;
    CopyFacts copy;
    DrawFacts fill, put;
};
inline Reason Decide(const Inputs& i) {
    if (!i.dead) return kLive;
    if (i.hrw != 0) return kHrw;
    if ((i.host & kHostMsaa) != 0) return kMsaa;
    if ((i.host & kHostFrame) != 0) return kHost;
    if (!i.markersOk || !i.copy.present || !i.fill.present || !i.put.present) return kMarkers;
    if (i.foreignBetween) return kForeign;
    const CopyFacts& c = i.copy;
    if (c.depth || c.fromPass || c.srcTex || c.dstTex || c.fill || c.skip || c.clearEnable || c.gxFmt != 0x06 || !c.identity) return kCopyShape;
    if (c.w <= 0 || c.h <= 0 || c.x != 0 || c.y != 0 || c.dstW != c.w || c.dstH != c.h) return kCopyShape;
    const auto okDraw = [&](const DrawFacts& d) {
        return d.cover && !d.blend && !d.depthTest && !d.depthWrite && d.colorWrite && d.dstAlphaZero == 0 && d.stOp == 0 && d.x == c.x && d.y == c.y && d.w == c.w && d.h == c.h;
    };
    if (!okDraw(i.fill) || !okDraw(i.put)) return kDrawShape;
    if (i.efbAlpha && i.fill.alphaWrite && !i.put.alphaWrite) return kPlanes;
    if (i.sharedReader) return kSharedRead;
    return kDropped;
}

// The state behind a refusal, for the log line (the last bracket that took it).
inline int Describe(char* out, size_t cap, Reason r, const Inputs& i) {
    const auto d = [](const DrawFacts& f, char* o, size_t n) {
        std::snprintf(o, n, "cover %d blend %d depth %d/%d colour %d alpha %d dstA %d stencil %d ex %d,%d %dx%d", f.cover ? 1 : 0, f.blend ? 1 : 0, f.depthTest ? 1 : 0, f.depthWrite ? 1 : 0, f.colorWrite ? 1 : 0,
                      f.alphaWrite ? 1 : 0, f.dstAlphaZero, f.stOp, f.x, f.y, f.w, f.h);
    };
    switch (r) {
    case kCopyShape:
        return std::snprintf(out, cap, "copy fmt 0x%02X src %d,%d %dx%d dst %dx%d identity %d clear %d depth %d pass %d atlas %d fill %d skip %d", i.copy.gxFmt, i.copy.x, i.copy.y, i.copy.w, i.copy.h, i.copy.dstW,
                             i.copy.dstH, i.copy.identity ? 1 : 0, i.copy.clearEnable ? 1 : 0, i.copy.depth ? 1 : 0, i.copy.fromPass ? 1 : 0, (i.copy.srcTex || i.copy.dstTex) ? 1 : 0, i.copy.fill ? 1 : 0, i.copy.skip ? 1 : 0);
    case kDrawShape: {
        char a[160], b[160];
        d(i.fill, a, sizeof a);
        d(i.put, b, sizeof b);
        return std::snprintf(out, cap, "copy src %dx%d; fill: %s; put-back: %s", i.copy.w, i.copy.h, a, b);
    }
    case kPlanes:
        return std::snprintf(out, cap, "fill alpha %d, put-back alpha %d, EFB alpha %d", i.fill.alphaWrite ? 1 : 0, i.put.alphaWrite ? 1 : 0, i.efbAlpha ? 1 : 0);
    case kMarkers:
        return std::snprintf(out, cap, "markers ok %d, copy %d fill %d put-back %d", i.markersOk ? 1 : 0, i.copy.present ? 1 : 0, i.fill.present ? 1 : 0, i.put.present ? 1 : 0);
    default:
        return std::snprintf(out, cap, "-");
    }
}

struct Census {
    uint64_t pictures = 0;            // pictures with at least one bracket
    uint64_t brackets = 0, dead = 0, dropped = 0, reason[kCount] = {};
    uint64_t markerRefusedHrw = 0;    // markers ignored because of GXV_HRW
    uint64_t lastLog = 0;
    Inputs last[kCount];              // the inputs of the last bracket that took each refusal
    void Picture(bool any) { if (any) ++pictures; }
    void Note(Reason r, const Inputs* in = nullptr) {
        ++brackets;
        if (r == kLive) return;
        ++dead;
        if (r == kDropped) ++dropped;
        else {
            ++reason[r];
            if (in) last[r] = *in;
        }
    }
    // The basic-log line, every `every` brackets (0: never); returns the text length, 0 when not due.
    int Line(char* out, size_t cap, uint64_t every, int mode) {
        if (every == 0 || brackets < lastLog + every) return 0;
        lastLog = brackets;
        const double pct = brackets ? 100.0 * static_cast<double>(dead) / static_cast<double>(brackets) : 0.0;
        int n = std::snprintf(out, cap, "bloom-bracket: GXV_BLOOM_BRACKET=%d: %llu pictures, %llu brackets, dead %llu (%.0f%%), %s %llu (capture, fill and put-back; =2: would be); refused:", mode,
                              static_cast<unsigned long long>(pictures), static_cast<unsigned long long>(brackets), static_cast<unsigned long long>(dead), pct,
                              mode == 1 ? "dropped" : "would drop", static_cast<unsigned long long>(dropped));
        bool any = false;
        int top = -1;
        for (int r = kMarkers; r < kCount && n > 0 && n < static_cast<int>(cap); ++r)
            if (reason[r]) {
                n += std::snprintf(out + n, cap - static_cast<size_t>(n), " %s %llu,", ReasonName(r), static_cast<unsigned long long>(reason[r]));
                any = true;
                if (top < 0 || reason[r] > reason[top]) top = r;
            }
        if (top >= 0 && n > 0 && n < static_cast<int>(cap)) {
            n += std::snprintf(out + n, cap - static_cast<size_t>(n), " top: %s [", ReasonName(top));
            if (n > 0 && n < static_cast<int>(cap)) n += Describe(out + n, cap - static_cast<size_t>(n), static_cast<Reason>(top), last[top]);
            if (n > 0 && n < static_cast<int>(cap)) n += std::snprintf(out + n, cap - static_cast<size_t>(n), "]");
        }
        if (!any && n > 0 && n < static_cast<int>(cap)) n += std::snprintf(out + n, cap - static_cast<size_t>(n), " none");
        if (markerRefusedHrw && n > 0 && n < static_cast<int>(cap)) n += std::snprintf(out + n, cap - static_cast<size_t>(n), " (hrw: %llu markers ignored)", static_cast<unsigned long long>(markerRefusedHrw));
        return n;
    }
};

// The owner tag a bracket's draws carry (DrawRec::owner, no merge with another owner): 0xB000 | id << 2 | role, id 1..63, role 1 fill, 2 put-back.
// GXV_HRW's owner keys are (id << 8 | phase) with small ids and 0xFE: they never collide with 0xB000..0xBFFF, and the markers are not honoured under HRW at all.
constexpr uint16_t kOwnerBase = 0xB000;
inline uint16_t OwnerTag(unsigned id, unsigned role) { return static_cast<uint16_t>(kOwnerBase | ((id & 0x3Fu) << 2) | (role & 3u)); }
inline bool IsOwnerTag(uint16_t o) { return (o & 0xF000u) == kOwnerBase; }
inline unsigned OwnerId(uint16_t o) { return (o >> 2) & 0x3Fu; }
inline unsigned OwnerRole(uint16_t o) { return o & 3u; }

} // namespace vgx::bb
