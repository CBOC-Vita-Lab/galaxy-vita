#pragma once
// dm65 (22 Nov 2026): the stencil layout of GXV_SHADOW_STENCIL together with the player's own-pixel mark of GXV_HRW_INPAINT (dm58). Pure tables, no GL:
// the back end (vgx_backend_gl.cpp, StencilOn) turns them into glStencilFunc / glStencilOp / glStencilMask, tests/shstencil_test.cpp runs them through a
// model of the GL stencil unit (tools/test_shstencil.sh).
//
//   bit 7 (0x80)      the player's own pixels of the pair's first picture (kOwnBit). The shadow volumes never touch it (write mask kUserMask).
//   bits 0..6         ONE counter, 0..127: the volumes' count AND the silhouette. The silhouette is SET to kSilhouette (64): the front end only takes a window
//                     where at most 63 front-face draws follow a mark (2 n <= 127) and at most 63 layers are counted (nBack <= 63), so a marked pixel stays
//                     non zero through every subtraction the window can make, and the count never leaves bits 0..6.
//   INCR / DECR       the volumes' counts. DECR is drawn under "counter != 0" (the counter bits only): a pixel at 0 is left alone (GL's DECR clamps at 0, and
//                     with a bit under the write mask the clamp is lost: dm58's mask 0xDF turned a marked pixel at 0 into 0x1F), and the mark is not in the test.
//   TEST              the fill: counter != 0, the mark ignored.
// Before dm65 the mark was 0x20 in the middle of the counter, the silhouette 0x80: a carry or borrow through the masked bit wrapped the count at 32 layers,
// DECR on a marked pixel at 0 gave 0x1F, and a silhouette reached 0 after 64 subtractions (not 128).
#include <cstdint>

#ifndef SHSTENCIL_MUTANT
#define SHSTENCIL_MUTANT 0  // (tests only: 1 = dm58's layout (mark 0x20, silhouette 0x80, DECR unguarded); 2 = DECR unguarded only)
#endif

namespace port::shstencil {

#if SHSTENCIL_MUTANT == 1
constexpr uint8_t kOwnBit = 0x20, kUserMask = 0xDF, kSilhouette = 0x80, kCountMask = 0xDF;
#else
constexpr uint8_t kOwnBit = 0x80, kUserMask = 0x7F, kSilhouette = 0x40, kCountMask = 0x7F;
#endif
constexpr int kMaxLayers = 63;  // the front end's bound on the layers one window counts when it has a silhouette (64 + 63 < 128)
constexpr int kCountMax = 127;  // ... and without one

enum Func : uint8_t { kAlways, kNotEqual, kEqual };
enum Act : uint8_t { kKeep, kReplace, kIncr, kDecr };

// What the back end gives the GL for a draw with the stencil operation `op` (ST_ZERO 1, ST_INCR 2, ST_DECR 3, ST_SET 4, ST_TEST 5; vgx_cmd.h): the test, then
// what the pixel gets where the draw passes the depth test (fail and depth-fail keep).
struct Setting {
    Func func = kAlways;
    uint8_t ref = 0, funcMask = 0xFF;
    Act depthPass = kKeep;
    uint8_t writeMask = 0xFF;
};

inline Setting ForOp(int op) {
    Setting s;
    s.writeMask = kUserMask;
    switch (op) {
    case 1: s.func = kAlways; s.ref = 0; s.funcMask = 0xFF; s.depthPass = kReplace; break;                    // ST_ZERO
    case 2: s.func = kAlways; s.ref = 0; s.funcMask = 0xFF; s.depthPass = kIncr; break;                       // ST_INCR
#if SHSTENCIL_MUTANT == 0
    case 3: s.func = kNotEqual; s.ref = 0; s.funcMask = kCountMask; s.depthPass = kDecr; break;               // ST_DECR (counter != 0)
#else
    case 3: s.func = kAlways; s.ref = 0; s.funcMask = 0xFF; s.depthPass = kDecr; break;
#endif
    case 4: s.func = kAlways; s.ref = kSilhouette; s.funcMask = 0xFF; s.depthPass = kReplace; break;          // ST_SET
    default: s.func = kNotEqual; s.ref = 0; s.funcMask = kUserMask; s.depthPass = kKeep; break;               // ST_TEST
    }
    return s;
}

// The GL stencil unit for one pixel (what the tests run the tables through): returns the new value, `passed` = the stencil test passed (the fragment is drawn).
inline uint8_t Apply(const Setting& s, uint8_t old, bool depthPassed, bool* passed = nullptr) {
    bool ok = true;
    const uint8_t a = static_cast<uint8_t>(s.ref & s.funcMask), b = static_cast<uint8_t>(old & s.funcMask);
    if (s.func == kNotEqual) ok = a != b;
    else if (s.func == kEqual) ok = a == b;
    if (passed) *passed = ok;
    if (!ok || !depthPassed) return old;
    unsigned v = old;
    switch (s.depthPass) {
    case kKeep: break;
    case kReplace: v = s.ref; break;
    case kIncr: v = old == 255 ? 255 : old + 1u; break;   // (GL_INCR clamps)
    case kDecr: v = old == 0 ? 0 : old - 1u; break;       // (GL_DECR clamps)
    }
    return static_cast<uint8_t>((old & ~s.writeMask) | (v & s.writeMask));
}

}  // namespace port::shstencil
