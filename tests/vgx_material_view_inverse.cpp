#include <array>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using f32 = float;
using u32 = std::uint32_t;
using s32 = std::int32_t;
using Mtx = float[3][4];
using Mtx44 = float[4][4];
struct Vec { float x, y, z; };
struct J3DTextureSRTInfo { float mScaleX, mScaleY; std::int16_t mRotation; float mTranslationX, mTranslationY; };
struct Info { unsigned mInfo; J3DTextureSRTInfo mSRT; Vec mCenter; Mtx44 mEffectMtx; };
enum { J3DTexMtxMode_None, J3DTexMtxMode_EnvmapBasic, J3DTexMtxMode_ProjmapBasic,
    J3DTexMtxMode_ViewProjmapBasic, J3DTexMtxMode_Unknown4, J3DTexMtxMode_Unknown5,
    J3DTexMtxMode_EnvmapOld, J3DTexMtxMode_Envmap, J3DTexMtxMode_Projmap,
    J3DTexMtxMode_ViewProjmap, J3DTexMtxMode_EnvmapOldEffectMtx, J3DTexMtxMode_EnvmapEffectMtx };
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#ifndef TEST_MTX_PROFILE
#define TEST_MTX_PROFILE 1
#endif
#define PORT_MTX_PROFILE_COMPILED TEST_MTX_PROFILE

struct Event {
    unsigned kind, slot, value;
    std::array<unsigned char, sizeof(Mtx)> input{};
    bool operator==(const Event& b) const { return kind == b.kind && slot == b.slot && value == b.value && input == b.input; }
};
static std::vector<Event> trace;
static unsigned inverseCalls, inverseFailures, callbackCalls, checks;
static unsigned profileBegins, profileEnds, profileFailures;
static bool profilePairsValid, profileRequested = true;
[[maybe_unused]] static unsigned port_mtx_profile_begin(const void*) {
    ++profileBegins;
    return profileRequested ? profileBegins : 0;
}
[[maybe_unused]] static void port_mtx_profile_end(unsigned token, unsigned result) {
    profilePairsValid &= token == profileEnds + 1 && token == profileBegins;
    ++profileEnds; profileFailures += !result;
}
static void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); }
}
struct Sys {
    Mtx view;
    unsigned sentinel = 0x12345678;
    float (*getViewMtx())[4] { return view; }
} j3dSys;
static Mtx j3dDefaultMtx = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
static void PSMTXCopy(const Mtx, Mtx);
static u32 PSMTXInverse(const Mtx, Mtx);
struct TexCoord {
    unsigned slot = 0, reg = 0;
    void resetTexMtxReg() { trace.push_back({0, slot, reg}); reg = 99; }
    void setTexMtxReg(unsigned v) { trace.push_back({1, slot, v}); reg = v; }
};
struct J3DTexMtx {
    Info mTexMtxInfo{};
    Mtx mMtx{};
    unsigned slot = 0;
    Info& getTexMtxInfo() { return mTexMtxInfo; }
    void realCalcPost(const Mtx);
    void calcPostTexMtx(const Mtx m) {
        Event event{2, slot, j3dSys.sentinel};
        std::memcpy(event.input.data(), m, sizeof(Mtx));
        trace.push_back(event);
        ++callbackCalls;
        realCalcPost(m);
    }
};
struct BlockBase {
    J3DTexMtx* mTexMtx[8]{};
    TexCoord mTexCoord[8]{};
};
struct BaselineBlock : BlockBase { void calcPostTexMtx(const Mtx); };
struct OptimizedBlock : BlockBase { void calcPostTexMtx(const Mtx); };

// The real texture-matrix math uses a read-only trig table; both paths use this deterministic lookup stand-in.
namespace JMath {
struct SinCosTable { float cosShort(std::int16_t r) const { return std::cos(float(r) * 0.00009587379924f); } };
static const SinCosTable sSinCosTable;
}
static float JMASSin(std::int16_t r) { return std::sin(float(r) * 0.00009587379924f); }
#include "vgx_material_view_inverse_fixture.inc"
static u32 PSMTXInverse(const Mtx src, Mtx dst) {
    ++inverseCalls;
    const u32 ok = C_MTXInverse(src, dst);
    inverseFailures += !ok;
    return ok;
}

struct Result {
    std::array<J3DTexMtx, 8> tex;
    std::array<unsigned, 8> regs;
    std::vector<Event> events;
    unsigned inverses, failures, callbacks;
    unsigned begins, ends, profileFailed;
    bool paired;
    int exceptions;
};
template<class Block> static Result Run(const std::array<J3DTexMtx, 8>& input, unsigned mask, const Mtx view, bool shared) {
    auto tex = input;
    Block block;
    for (unsigned i = 0; i < 8; ++i) { block.mTexMtx[i] = (mask & (1u << i)) ? &tex[shared ? i / 2 : i] : nullptr; block.mTexCoord[i] = {i, 80u + i}; }
    std::memcpy(j3dSys.view, view, sizeof(Mtx));
    j3dSys.sentinel = 0x12345678;
    trace.clear(); inverseCalls = inverseFailures = callbackCalls = 0;
    profileBegins = profileEnds = profileFailures = 0; profilePairsValid = true;
    std::feclearexcept(FE_ALL_EXCEPT);
    block.calcPostTexMtx(j3dDefaultMtx);
    Result out{tex, {}, trace, inverseCalls, inverseFailures, callbackCalls, profileBegins, profileEnds,
        profileFailures, profilePairsValid, std::fetestexcept(FE_ALL_EXCEPT)};
    for (unsigned i = 0; i < 8; ++i) out.regs[i] = block.mTexCoord[i].reg;
    Check(std::memcmp(j3dSys.view, view, sizeof(Mtx)) == 0 && j3dSys.sentinel == 0x12345678, "view and unrelated global state unchanged");
    return out;
}
static void Compare(const std::array<J3DTexMtx, 8>& tex, unsigned mask, const Mtx view, bool shared = false) {
    const auto a = Run<BaselineBlock>(tex, mask, view, shared), b = Run<OptimizedBlock>(tex, mask, view, shared);
    Check(a.events == b.events && a.regs == b.regs && a.callbacks == b.callbacks, "slot reset/register/calc call order and input bits retained");
    for (unsigned i = 0; i < 8; ++i) {
        Check(std::memcmp(a.tex[i].mMtx, b.tex[i].mMtx, sizeof(Mtx)) == 0, "real texture-matrix outputs bit-identical");
        Check(std::memcmp(&a.tex[i].mTexMtxInfo, &b.tex[i].mTexMtxInfo, sizeof(Info)) == 0, "prepared SRT/effect inputs unchanged");
    }
    Check(a.exceptions == b.exceptions, "floating-point sticky exceptions unchanged");
    unsigned expected = a.inverses;
#if TARGET_PC
    if (a.inverses && !a.failures) expected = 1;
#endif
    Check(b.inverses == expected, "successful inverse is computed only once per invocation");
    Check(b.failures == a.failures, "each failed inverse retains its original per-slot attempt");
    for (const Result* r : {&a, &b}) {
        Check(r->begins == (TEST_MTX_PROFILE ? r->inverses : 0u), "compiled profiler sees every real inverse computation");
        Check(r->ends == (TEST_MTX_PROFILE && profileRequested ? r->inverses : 0u), "enabled profiler ends every real inverse computation");
        Check(r->paired && r->profileFailed == (TEST_MTX_PROFILE && profileRequested ? r->failures : 0u), "profile tokens and singular results remain paired");
    }
}
static std::array<J3DTexMtx, 8> Textures(unsigned mode) {
    std::array<J3DTexMtx, 8> a{};
    for (unsigned i = 0; i < 8; ++i) {
        a[i].slot = i; a[i].mTexMtxInfo.mInfo = mode;
        a[i].mTexMtxInfo.mSRT = {1.25f, 0.75f, 1234, 0.25f, -0.125f};
        a[i].mTexMtxInfo.mCenter = {0.5f, 0.5f, 0};
        for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c) a[i].mTexMtxInfo.mEffectMtx[r][c] = r == c ? 1.0f : 0.0f;
    }
    return a;
}
static std::uint32_t rng = 0x125dead;
static unsigned Next() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float Random() { return (int(Next() % 20001) - 10000) * 0.001f; }
static float Bits(unsigned bits) { float f; std::memcpy(&f, &bits, sizeof f); return f; }
int main() {
    Mtx view = {{1.2f, 0.1f, 0.2f, 4}, {0.3f, 1.4f, 0.1f, -5}, {0.2f, 0.4f, 1.8f, 6}};
    Compare(Textures(2), 255, view); // TDD: the original path computes eight equal inverses.
    const auto before = Run<BaselineBlock>(Textures(2), 255, view, false);
    const auto after = Run<OptimizedBlock>(Textures(2), 255, view, false);
    Check(before.inverses == 8 && after.inverses == (TEST_TARGET_PC ? 1u : 8u), "eight-slot real inverse computation census");
    std::printf("inverse computations, eight projection slots: %u -> %u\n", before.inverses, after.inverses);
    profileRequested = false;
    Compare(Textures(2), 255, view);
    profileRequested = true;
    for (unsigned mode = 0; mode < 64; ++mode) for (unsigned maya : {0u, 128u}) for (unsigned mask : {0u, 1u, 3u, 85u, 128u, 255u}) Compare(Textures(mode | maya), mask, view);
    Mtx singular{};
    for (unsigned mode : {2u, 5u, 8u, 10u, 11u}) Compare(Textures(mode), 255, singular);
    auto mixed = Textures(2);
    const unsigned modes[8] = {2, 5, 8, 10, 11, 0, 3, 7};
    for (unsigned i = 0; i < 8; ++i) mixed[i].mTexMtxInfo.mInfo = modes[i];
    Compare(mixed, 255, singular);
    Mtx dependent = {{1, 2, 3, 4}, {1, 2, 3, 4}, {5, 6, 7, 8}};
    Compare(mixed, 255, dependent);
    Compare(mixed, 255, view, true);
    for (unsigned i = 0; i < 20000; ++i) {
        auto tex = Textures(0);
        for (auto& t : tex) {
            t.mTexMtxInfo.mInfo = (Next() % 64) | (Next() & 128u);
            t.mTexMtxInfo.mSRT = {Random(), Random(), std::int16_t(Next()), Random(), Random()};
            for (auto& row : t.mTexMtxInfo.mEffectMtx) for (auto& v : row) v = Random();
        }
        for (auto& row : view) for (auto& v : row) v = Random();
        if (i % 7 == 0) view[2][2] = 1e-30f;
        if (i % 11 == 0) view[0][3] = -0.0f;
        Compare(tex, Next() & 255u, view, i % 10 == 0);
    }
    for (unsigned bits : {0u, 0x80000000u, 0x7fc12345u, 0xffc12345u, 0x7f800000u, 0xff800000u, 1u, 0x00800000u}) {
        std::memcpy(view, j3dDefaultMtx, sizeof view);
        view[0][0] = Bits(bits);
        Compare(Textures(2), 255, view);
        Compare(Textures(11), 255, view);
    }
    // Outer callbacks/state changes before each call must be observed: there is no cross-call cache.
    for (int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        Check(std::fesetround(rounding) == 0, "set test rounding mode");
        for (unsigned i = 0; i < 16; ++i) {
            std::memcpy(view, j3dDefaultMtx, sizeof view); view[0][3] = float(i); view[1][1] = 1.0f + i * 0.125f;
            Compare(Textures((i & 1) ? 2 : 11), 255, view);
        }
    }
    std::fesetround(FE_TONEAREST);
    std::printf("PASS: material view inverse actual-body differential (%u checks, TARGET_PC=%d, diagnostics=%d, profile=%d)\n", checks, TEST_TARGET_PC, PORT_RUNTIME_DIAGNOSTICS, TEST_MTX_PROFILE);
}
