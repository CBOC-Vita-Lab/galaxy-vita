// GXV_ALPHATEST_PREPASS (dread25): an alpha-tested (`discard`) draw is drawn as two passes, so that the part of the work the GPU cannot cull
// (the fragment shader's alpha test) is a cheap one and the expensive colour shader runs once per visible pixel, without a `discard`.
//
//   pass P (prepass)  the draw's own vertex shader and state, the colour and alpha masked, depth written as the draw writes it; its fragment
//                     shader is the key's text up to and including the alpha test (FsKey::preAlpha), so it kills exactly the fragments the
//                     tested shader kills (the alpha expression is the same text);
//   pass C (colour)   the same vertex shader and uniforms (so the same clip positions, bit for bit), the key's text WITHOUT the test
//                     (FsKey::atPass: the same variant GXV_PIX_ATPASS makes), depth function EQUAL, depth write off, the draw's colour mask and
//                     blend as before (it is not blended).
//
// Exactness. A fragment of C has depth z and passes iff z == the depth buffer after P (and after whatever was drawn since). P wrote the nearest
// surviving fragment's depth (func LESS / LEQUAL: the depth only falls), so at a pixel C passes for exactly the fragment that P let through there,
// unless something nearer was drawn in between. The picture is that of the original single draw when:
//   * the two passes' depths are bit-identical (same vertex program text, same uniforms, same state: the vertex path never differs; a GPU
//     whose depth interpolation differed between two draws with identical inputs would lose pixels: the census / the test's negative control
//     show what that looks like);
//   * mode 1 (the pair in place): nothing is drawn between P and C. Then C writes colour exactly where the original draw's surviving
//     fragments passed the original depth test (z <= buf, LEQUAL: a tie passes P and passes EQUAL), because P's own test was the same test;
//     (a LESS draw is not split: at a tie P fails and EQUAL passes);
//   * mode 2 (P hoisted to the start of its segment, C in place), for a draw whose function is LEQUAL and a segment made only of plain draws
//     (depth test and write, LESS or LEQUAL, no blend, colour and alpha written, no stencil): see Hoist below.
//
// Hoist. A segment is a maximal run of plain draws (the same notion as the draw-order planner's, vgx_hsrorder.h). Every tested LEQUAL draw of a
// segment that has an opaque draw before it gets its P drawn at the segment's start and its C at its place. Claim: the colour and depth buffers
// after the segment are those of the original order. The final depth at a pixel is the minimum over the surviving fragments either way (hoisting
// changes what a draw is tested against, never which fragments survive, and a killed fragment writes nothing). The colour at a pixel is that of
// the last fragment in the original order among those that win at the minimal depth z*:
//   * an opaque draw BEFORE the tested draw: with P first it fails where it lay behind the tested fragment (the tested draw overwrote it anyway),
//     and at an exact tie z == z* it passes (LEQUAL) or fails (LESS); either way C, which comes later and passes at the tie (EQUAL), overwrites it,
//     exactly as the original tested draw (LEQUAL, later) overwrote the opaque one;
//   * an opaque draw AFTER the tested draw sees the depth P left, as it saw the depth the original draw left: nothing changes for it;
//   * C draws in the original order among themselves, each passing where its depth equals the buffer's then: the last of the tied tested fragments
//     writes, as in the original.
// (a tested draw whose own function is LESS is not split at all, see StateOk)
// The tests (tests/vgx_prepass_test.cpp: the planner; tests/vgx_hsrorder_gl.cpp on Mesa: pictures) include those ties.
#pragma once
#include "vgx_cmd.h"
#include <cstdint>
#include <string>
#include <vector>

namespace vgx {
// GXV_ALPHATEST_PREPASS (dread25, vgx_prepass.h): whether fragment shader `fs` is the depth-only first pass of an alpha-tested key (FsKey::preAlpha), and the
// ids of the tested key it was made from and of its colour variant (the same text without the test). Written by the front end when the pair is made, read by the back end.
bool PrepassPair(uint16_t fs, uint16_t& orig, uint16_t& colour);
// 1 once any pair exists (a cheap test in the back end's draw path)
bool PrepassAny();
void PrepassCounters(uint64_t out[9]); // [0] draws given a prepass key, [1] keys made, [2] refused, [3..8] tested draws left as they are: state not plain, folded, key flags, id too large, refused key (see PreCtr::why; [3] first)
namespace atpp {

// The state a draw must have for the split (front end, at the time its fragment shader is chosen; the back end asks again with the finished record).
// LEQUAL only: with LESS, a fragment that ties with the depth already there fails the prepass but passes the EQUAL colour pass (the model test shows it).
inline bool StateOk(const RenderState& rs) {
    return !rs.blend && rs.depthTest && rs.depthWrite && rs.depthFunc == CMP_LEQUAL && rs.colorWrite && rs.alphaWrite;
}
// Whether the pre text is the tested text up to and including its alpha-test line, then the constant output, and nothing else.
inline bool TextOk(const std::string& orig, const std::string& pre) {
    static const char kTail[] = "  gl_FragColor = vec4(0.0);\n}\n";
    size_t pos = 0, end = std::string::npos;
    while (pos < orig.size()) {
        size_t e = orig.find('\n', pos);
        if (e == std::string::npos) e = orig.size() - 1;
        if (orig.compare(pos, 8, "  if (!(") == 0 && orig.find("discard;", pos) != std::string::npos && orig.find("discard;", pos) <= e) end = e + 1;
        pos = e + 1;
    }
    if (end == std::string::npos) return false;
    return pre.size() == end + sizeof kTail - 1 && pre.compare(0, end, orig, 0, end) == 0 && pre.compare(end, std::string::npos, kTail) == 0;
}

// What the back end knows of each command, for the planner. 0: a barrier (not a draw, or not a plain draw); 1: a plain opaque draw (no alpha test, or one that is not
// split); 2: a plain tested draw that is not split (another depth function, or no pair); 3: a plain tested draw that is split and may be hoisted (LEQUAL).
enum Kind : uint8_t { kBarrier = 0, kOpaque = 1, kSplit = 2, kHoistable = 3 };
struct Entry { uint32_t at; uint32_t cmd; }; // draw `cmd`'s prepass is drawn before command `at` (the first command of its segment)
struct Stat { uint32_t segments = 0, segmentsWithHoist = 0, hoisted = 0, inPlace = 0; uint64_t positions = 0; };

// kinds[i] for each command. Appends the hoists to out, in the order of the draws; Entry.at ascending.
inline void PlanHoist(const uint8_t* kinds, size_t n, std::vector<Entry>& out, Stat& st) {
    size_t i = 0;
    while (i < n) {
        if (kinds[i] == kBarrier) { ++i; continue; }
        const size_t a = i;
        while (i < n && kinds[i] != kBarrier) ++i;
        ++st.segments;
        bool seenOpaque = false, any = false;
        for (size_t k = a; k < i; ++k) {
            if (kinds[k] == kOpaque) seenOpaque = true;
            else if (kinds[k] == kHoistable) {
                if (seenOpaque) {
                    out.push_back(Entry{static_cast<uint32_t>(a), static_cast<uint32_t>(k)});
                    ++st.hoisted;
                    st.positions += k - a;
                    any = true;
                } else {
                    ++st.inPlace;
                }
            } else if (kinds[k] == kSplit) {
                ++st.inPlace;
            }
        }
        if (any) ++st.segmentsWithHoist;
    }
}

} // namespace atpp
} // namespace vgx
