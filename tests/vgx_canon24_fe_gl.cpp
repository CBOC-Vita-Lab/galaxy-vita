// canon24b: the front-end half of GXV_TEVIN_CANON=2, through the production units (vgx_state.cpp TevOorUpdate, vgx_draw.cpp BuildFsKey /
// split::BuildTevC, vgx_fesplit.cpp, the GLES back end on Mesa).
//   tools/test_vgx_canon24_fe_gl.sh [--asan] <output-dir> <vgx_shaderlist.bin | dir of raw .key files>...
//   vgx_canon24_fe_gl <picture-out.bin> <key lists>...      (GXV_TEVIN_CANON / GXV_FIX_TEVIN from the environment)
// 1. The register mask (State::tevOor with GXV_TEVIN_CANON on: bit r = register r has a component outside 0..255): 200,000 random TEV
//    register writes (BP 0xE0..0xE7, both halves, register and konst type, 11-bit values biased to the boundaries -1024 -257 -256 -255
//    -1 0 1 254 255 256 1023, through WriteBP with and without the 0xFE mask, and through StoreBP) against a reference model of the
//    registers (sign-extended 11-bit fields) and its mask; State::keyHash against KeyHashOf.
// 2. BuildTevC's upload against a reference low byte: every s11 value -1024..1023 in every register component, keys with every preWrap
//    mask (inWrap 1 and 0), folded and not: a preWrap part (only with inWrap 1, never a folded register) is ((v mod 256) / 255) correctly
//    rounded, every other register value v / 255, every konst its low byte / 255.
// 3. The arithmetic: the =1 text's vgx_w1 in float (IEEE single, as the SGX's float) on v / 255 gives back the low byte for every s11
//    value, and its last divide equals the CPU's (v & 255) / 255 (correctly rounded), or a multiply by the rounded reciprocal (what GPUs
//    do) differs from it by at most one ulp; counted, with what an 8-bit output conversion (rounding or truncating) makes of it.
// 4. A picture: every stage configuration of the key lists that the front end can mark, set up through BP / XF registers (texture inputs
//    off: white under GXV_FIX_TEXOFF; indirect stages and fog off; the rasterised colours from per-vertex colours, the list's swap;
//    the list's alpha test), drawn as 16 quads a 64x64 picture, twice each with other registers (the same material with its registers
//    moved in and out of range between draws, mid-picture). The pictures go to <picture-out.bin>; the script compares =1 against =2.
// 5. The warm-up list (vgx_draw.cpp EnsureShaderList with CanonListFsKey): a list with marked keys as =0 / =1 record them and as =2 does
//    is read at the first key lookup; afterwards every key in the file is in the form the front end makes under the running mode.
#include "vgx_shadergen.h"
#include "vgx_state.h"
#include "vgx_fesplit.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}
namespace {
using namespace vgx;
unsigned failures;
void Check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x77ull) {}
    uint64_t Next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 0x2545F4914F6CDD1Dull; }
    uint32_t U(uint32_t n) { return static_cast<uint32_t>((Next() >> 33) % n); }
    bool P(unsigned pct) { return U(100) < pct; }
};
inline int Sext11(uint32_t v) { return (v & 0x400u) ? static_cast<int>(v & 0x7FFu) - 2048 : static_cast<int>(v & 0x7FFu); }
inline int LowByte(int v) { return ((v % 256) + 256) % 256; } // (reference: not the & of the code under test)
const int kEdge[11] = {-1024, -257, -256, -255, -1, 0, 1, 254, 255, 256, 1023};
int S11(Rng& r) { return r.P(70) ? kEdge[r.U(11)] : static_cast<int>(r.U(2048)) - 1024; }

// ---- 1. the mask
void MaskTest() {
    ResetState();
    int ref[2][4][4] = {}; // [konst][register][r g b a]
    Rng r(0xCA01);
    unsigned bad = 0, badRegs = 0, badHash = 0, writes = 0;
    const bool canon = TevInCanonOn();
    for (int i = 0; i < 200000; ++i) {
        const unsigned idx = r.U(4), half = r.U(2), konst = r.P(25) ? 1 : 0;
        const int lo = S11(r), hi = S11(r);
        const uint32_t nv = (static_cast<uint32_t>(lo) & 0x7FFu) | ((static_cast<uint32_t>(hi) & 0x7FFu) << 12) | (konst << 23);
        const uint8_t reg = static_cast<uint8_t>(0xE0 + idx * 2 + half);
        const unsigned path = r.U(10);
        uint32_t written = nv;
        if (path < 6) WriteBP(reg, nv);
        else if (path < 8) {
            // (a masked write: the 0xFE mask keeps some bits of the old value)
            const uint32_t mask = r.P(50) ? 0x00FFFFFFu : (r.P(50) ? 0x00FFF000u : 0x000007FFu);
            const uint32_t old = g.bp[reg];
            WriteBP(0xFE, mask);
            WriteBP(reg, nv);
            // (the type bit 23 is in every mask's complement but the full one: the write lands as (old & ~mask) | (nv & mask))
            written = (old & ~mask) | (nv & mask);
        } else StoreBP(reg, nv);
        ++writes;
        const unsigned k = (written >> 23) & 1u;
        const int l = Sext11(written & 0x7FFu), h = Sext11((written >> 12) & 0x7FFu);
        if (half) { ref[k][idx][2] = l; ref[k][idx][1] = h; } else { ref[k][idx][0] = l; ref[k][idx][3] = h; }
        unsigned want = 0;
        for (int q = 0; q < 4; ++q)
            for (int c = 0; c < 4; ++c)
                if (ref[0][q][c] < 0 || ref[0][q][c] > 255) want |= canon ? (1u << q) : 1u;
        if (g.tevOor != want) ++bad;
        if (std::memcmp(g.tevReg, ref[0], sizeof ref[0]) != 0 || std::memcmp(g.tevKonst, ref[1], sizeof ref[1]) != 0) ++badRegs;
        if ((i & 255) == 0 && RsCacheMode() != 0 && KeyHashOf(g) != g.keyHash) ++badHash; // (GXV_RSCACHE=0 keeps no key hash)
    }
    std::printf("mask: %u writes, mask mismatches %u, register mismatches %u, key hash mismatches %u (GXV_TEVIN_CANON %s)\n", writes, bad, badRegs,
                badHash, canon ? "on: a mask" : "off: one bit");
    Check(bad == 0 && badRegs == 0, "State::tevOor is the mask of the registers outside 0..255 after every write");
    Check(badHash == 0, "incremental key hash equals KeyHashOf");
}

// ---- 2. BuildTevC
void UploadTest() {
    ResetState();
    unsigned checked = 0, bad = 0;
    Rng r(0xCA02);
    for (int v = -1024; v <= 1023; ++v)
        for (int rep = 0; rep < 4; ++rep) {
            // registers: component c of register q = v for one (q, c) in turn, the others random; konst random 11-bit
            for (int q = 0; q < 4; ++q)
                for (int c = 0; c < 4; ++c) { g.tevReg[q][c] = S11(r); g.tevKonst[q][c] = S11(r); }
            g.tevReg[rep][(v + 1024) & 3] = v;
            FsKey k;
            k.inWrap = static_cast<uint8_t>(r.P(70) ? 1 : (r.P(50) ? 0 : 2));
            k.setPreWrap(static_cast<uint8_t>(r.U(256)));
            const bool fold = r.P(25);
            UniArena uni;
            uint32_t off[UB_COUNT], len[UB_COUNT];
            std::fill(off, off + UB_COUNT, kNoBlock);
            std::fill(len, len + UB_COUNT, 0u);
            split::BlockCtx bc{&uni, off, len, true, false, 0};
            const uint16_t tw[8] = {}, th[8] = {};
            split::BuildTevC(bc, g, k, tw, th, fold);
            const float* t = uni.data() + off[UB_TEV];
            for (int q = 0; q < 4; ++q)
                for (int c = 0; c < 4; ++c) {
                    const int x = g.tevReg[q][c];
                    const bool pre = (k.inWrap & 1) && ((k.preWrap() >> (c < 3 ? q : q + 4)) & 1u) && !(fold && (q == 1 || q == 2));
                    float want;
                    if (fold && (q == 1 || q == 2)) want = 0.0f;
                    else if (pre) want = static_cast<float>(static_cast<double>(LowByte(x)) / 255.0); // (double divide, then rounded once)
                    else want = static_cast<float>(static_cast<double>(x) / 255.0);
                    ++checked;
                    if (std::memcmp(&t[TEV_REG + q * 4 + c], &want, 4) != 0) ++bad;
                    const float kw = static_cast<float>(static_cast<double>(LowByte(g.tevKonst[q][c])) / 255.0);
                    ++checked;
                    if (std::memcmp(&t[TEV_KONST + q * 4 + c], &kw, 4) != 0) ++bad;
                }
        }
    std::printf("upload: %u values checked against the reference (every s11 value in every register component, random preWrap / inWrap / fold), %u differ\n",
                checked, bad);
    Check(bad == 0, "BuildTevC: a preWrap part is its low byte / 255, everything else as before, bit for bit");
}

// ---- 3. the arithmetic of the wrap in float
void ArithTest() {
    unsigned byteBad = 0, divDiff = 0, recipDiff = 0, recipUlp2 = 0, outRound = 0, outTrunc = 0;
    const volatile float k255 = 255.0f, k256 = 256.0f, kHalf = 0.5f, kOne = 1.0f;
    const float recip = kOne / k255;
    for (int v = -1024; v <= 1023; ++v) {
        const float value = static_cast<float>(v) / 255.0f;             // (BuildTevC, =1)
        const float byteValue = std::floor(value * k255 + kHalf);      // (vgx_w1, each operation rounded to float)
        const float low = byteValue - k256 * std::floor(byteValue / k256);
        if (low != static_cast<float>(LowByte(v))) ++byteBad;
        const float gpuDiv = low / k255;                                // (a correctly rounded divide)
        const float gpuRecip = low * recip;                             // (a multiply by the rounded reciprocal)
        const float cpu = static_cast<float>(v & 255) / 255.0f;         // (BuildTevC, =2)
        if (std::memcmp(&gpuDiv, &cpu, 4) != 0) ++divDiff;
        if (std::memcmp(&gpuRecip, &cpu, 4) != 0) {
            ++recipDiff;
            int32_t a, b;
            std::memcpy(&a, &gpuRecip, 4);
            std::memcpy(&b, &cpu, 4);
            if (std::abs(a - b) > 1) ++recipUlp2;
            // the 8-bit output of the bare value: rounding conversion, truncating conversion
            if (std::lround(gpuRecip * 255.0f) != std::lround(cpu * 255.0f)) ++outRound;
            if (static_cast<int>(gpuRecip * 255.0f) != static_cast<int>(cpu * 255.0f)) ++outTrunc;
        }
    }
    std::printf("arithmetic: s11 values whose float wrap misses the low byte %u; divide differs from the CPU upload %u; reciprocal multiply "
                "differs %u (by more than one ulp %u; 8-bit result differs: rounding conversion %u, truncating conversion %u)\n",
                byteBad, divDiff, recipDiff, recipUlp2, outRound, outTrunc);
    Check(byteBad == 0, "the =1 text's wrap finds the low byte of every s11 register value in float");
    Check(divDiff == 0 && recipUlp2 == 0, "the CPU's (v & 255) / 255 is the GPU's value, or one ulp from a reciprocal multiply");
}

// ---- 4. pictures through the production pipeline
bool ReadList(const char* path, std::vector<FsKey>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    struct Hdr { char magic[8]; uint32_t vs, fs, gen, res; } h;
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, "VGXSHL01", 8) == 0 && h.vs == sizeof(VsKey) && h.fs == sizeof(FsKey);
    uint32_t type;
    while (ok && std::fread(&type, 4, 1, f) == 1) {
        if (type == 1) { VsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; }
        else if (type == 2) { FsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; out.push_back(k); }
        else if (type == 3) { uint32_t v[2]; if (std::fread(v, 8, 1, f) != 1) break; }
        else if (type == 4) { uint32_t v[4]; if (std::fread(v, 16, 1, f) != 1) break; }
        else break;
    }
    std::fclose(f);
    return ok;
}
bool ReadKeyDir(const char* path, std::vector<FsKey>& out) {
    DIR* d = opendir(path);
    if (!d) return false;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".key") == 0 && n[0] == 'f') names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        FILE* f = std::fopen((std::string(path) + "/" + n).c_str(), "rb");
        if (!f) continue;
        FsKey k;
        if (std::fread(&k, sizeof k, 1, f) == 1) out.push_back(k);
        std::fclose(f);
    }
    return true;
}
struct KeyLess { bool operator()(const FsKey& a, const FsKey& b) const { return std::memcmp(&a, &b, sizeof a) < 0; } };
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | (value & 0xFFFFFFu)); }
void SetReg(unsigned idx, const int v[4], bool konst) {
    const unsigned t = konst ? 0x800000u : 0u;
    BP(0xE0 + idx * 2, (static_cast<unsigned>(v[0]) & 0x7FF) | ((static_cast<unsigned>(v[3]) & 0x7FF) << 12) | t);
    BP(0xE1 + idx * 2, (static_cast<unsigned>(v[2]) & 0x7FF) | ((static_cast<unsigned>(v[1]) & 0x7FF) << 12) | t);
}
void Frame() {
    ResetState();
    g.efbW = g.efbH = 64;
    QueueSetTarget();
    BP(0x43, 1);
    BP(0x20, 342u | (342u << 12)); BP(0x21, 405u | (405u << 12));
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1), 0, 0, 0, 0, f2u(1), 0, 0, 0, 0, f2u(1), 0};
    WriteXF(0, 12, m);
    const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(1), 0, 1};
    WriteXF(0x1020, 7, proj);
    const uint32_t vp[] = {f2u(32), f2u(-32), f2u(16777216), f2u(374), f2u(374), f2u(16777216)};
    WriteXF(0x101A, 6, vp);
    const uint32_t one = 1; WriteXF(0x103F, 1, &one);
    const uint32_t tg = 5u << 7; WriteXF(0x1040, 1, &tg);
    // two colour channels, material colour from the vertices, no lighting (ras = the vertex colours)
    const uint32_t two = 2; WriteXF(0x1009, 1, &two);
    const uint32_t chan[4] = {1, 1, 1, 1}; WriteXF(0x100E, 4, chan);
    BP(0x40, 0); BP(0x41, (1u << 3) | (1u << 4));
}
// One quad in cell (cx, cy) of a 4x4 grid with the stage configuration of `c` and the registers / konst / colours of `r`.
void DrawConfig(const FsKey& c, Rng& r, int cx, int cy, unsigned oorMask) {
    const int n = std::clamp<int>(c.numStages, 1, 16);
    BP(0x00, 1u | (2u << 4) | (static_cast<unsigned>(n - 1) << 10)); // one texgen, two colour channels, n stages, no indirect stage
    for (int s = 0; s < n; ++s) {
        const FsKey::Stage& st = c.stage[s];
        BP(0xC0 + 2 * s, st.color);
        BP(0xC1 + 2 * s, (st.alpha & 0x00FFFFF0u) | 1u); // (ras swap table 1, texture swap table 0)
        BP(0x10 + s, 0);
    }
    for (int p = 0; p < 8; ++p) {
        // tref: texture off (white under GXV_FIX_TEXOFF), the stage's colour channel
        unsigned t = 0;
        for (int h = 0; h < 2; ++h) {
            const int s = p * 2 + h;
            const unsigned ch = s < n ? (c.stage[s].rasChan & 7u) : 7u;
            t |= (ch << 7) << (12 * h);
        }
        BP(0x28 + p, t);
    }
    for (int p = 0; p < 8; ++p) {
        // ksel: konst selections of stages 2p, 2p + 1; swap tables: 0 identity, 1 the first stage's ras swap of the key
        unsigned v = 0;
        for (int h = 0; h < 2; ++h) {
            const int s = p * 2 + h;
            if (s < n) v |= (static_cast<unsigned>(c.stage[s].kcsel & 31) << (h ? 14 : 4)) | (static_cast<unsigned>(c.stage[s].kasel & 31) << (h ? 19 : 9));
        }
        const uint8_t sw = p < 2 ? 0xE4 : c.stage[0].rasSwap;
        const unsigned pair = (p & 1) ? ((sw >> 4) & 15u) : (sw & 15u);
        v |= p < 4 ? pair : ((p & 1) ? 14u : 4u);
        BP(0xF6 + p, v);
    }
    if (c.alphaTestOn) BP(0xF3, r.U(256) | (r.U(256) << 8) | (static_cast<unsigned>(c.alphaTest & 7) << 16) | (static_cast<unsigned>((c.alphaTest >> 3) & 7) << 19) |
                                  (static_cast<unsigned>((c.alphaTest >> 6) & 3) << 22));
    else BP(0xF3, (7u << 16) | (7u << 19));
    BP(0xF1, 0);
    for (unsigned q = 0; q < 4; ++q) {
        int v[4];
        for (int k = 0; k < 4; ++k) v[k] = ((oorMask >> q) & 1u) ? S11(r) : static_cast<int>(r.P(50) ? (r.P(50) ? 0 : 255) : r.U(256));
        if (((oorMask >> q) & 1u) && std::all_of(v, v + 4, [](int x) { return x >= 0 && x <= 255; })) v[r.U(4)] = r.P(50) ? -1 - static_cast<int>(r.U(1024)) : 256 + static_cast<int>(r.U(768));
        SetReg(q, v, false);
        int kv[4];
        for (int& x : kv) x = static_cast<int>(r.U(256));
        SetReg(q, kv, true);
    }
    struct V { float pos[4]; uint8_t c0[4], c1[4]; float t[2]; };
    const float x0 = -1.0f + cx * 0.5f, y0 = -1.0f + cy * 0.5f;
    V v[4] = {{{x0, y0, -0.5f, 0}, {}, {}, {0, 1}}, {{x0 + 0.5f, y0, -0.5f, 0}, {}, {}, {1, 1}}, {{x0 + 0.5f, y0 + 0.5f, -0.5f, 0}, {}, {}, {1, 0}},
             {{x0, y0 + 0.5f, -0.5f, 0}, {}, {}, {0, 0}}};
    for (V& x : v)
        for (int k = 0; k < 4; ++k) {
            x.c0[k] = static_cast<uint8_t>(r.P(40) ? (r.P(50) ? 0 : 255) : r.U(256));
            x.c1[k] = static_cast<uint8_t>(r.P(40) ? (r.P(50) ? 0 : 255) : r.U(256));
        }
    DrawDecoded(0x80, VL_COL0 | VL_COL1 | VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4);
}
bool ReadPicture(std::vector<uint8_t>& px) {
    g.texCopySrc = EfbRect{0, 0, 64, 64};
    g.texCopyDstW = g.texCopyDstH = 64;
    g.texCopyFmt = 6;
    alignas(32) static unsigned char dest[32];
    QueueCopyTex(dest, false);
    const auto id = Pending().copyTex.back().texId;
    FrontPresent(false, nullptr);
    int w = 0, h = 0;
    return backend::DebugReadTexture(id, px, w, h) && w == 64 && h == 64;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: vgx_canon24_fe_gl <picture-out.bin> <key lists>...\n"); return 2; }
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(64, 64), "production backend init");
    if (failures) return 1;
    std::printf("GXV_TEVIN_CANON mode %ld, GXV_FIX_TEVIN %d\n", TevInCanonMode(), FixTevIn() ? 1 : 0);
    std::vector<FsKey> all;
    for (int i = 2; i < argc; ++i)
        if (!ReadList(argv[i], all) && !ReadKeyDir(argv[i], all)) { std::fprintf(stderr, "cannot read %s\n", argv[i]); return 2; }
    // 5. the warm-up list (EnsureShaderList, read at the first key lookup): a list with the lists' marked keys as recorded (no preWrap,
    // a list of =0 / =1), the same keys with their preWrap (a list of =2) and unmarked keys; after the run its keys must be the ones the
    // front end makes under this mode (CanonListFsKey).
    const std::string listPath = std::string(argv[1]) + ".shaderlist";
    {
        FILE* lf = std::fopen(listPath.c_str(), "wb");
        if (!lf) { std::perror(listPath.c_str()); return 2; }
        struct Hdr { char magic[8]; uint32_t vs, fs, gen, res; } h{{'V', 'G', 'X', 'S', 'H', 'L', '0', '1'}, sizeof(VsKey), sizeof(FsKey), kShaderGenVersion, 0};
        std::fwrite(&h, sizeof h, 1, lf);
        unsigned n = 0;
        for (const FsKey& k0 : all) {
            if (n >= 300) break;
            FsKey k = k0;
            const uint32_t t = 2;
            std::fwrite(&t, 4, 1, lf);
            if ((k.inWrap & 1) && (n & 1)) k.setPreWrap(FsTevInPreWrapMask(k)); // (as a =2 run would have recorded it)
            std::fwrite(&k, sizeof k, 1, lf);
            ++n;
        }
        std::fclose(lf);
        setenv("GXV_SHADERLIST", listPath.c_str(), 1);
    }
    MaskTest();
    UploadTest();
    ArithTest();
    std::map<FsKey, int, KeyLess> configs;
    for (FsKey k : all) {
        k.inWrap = 0;
        k.setPreWrap(0);
        unsigned need = 0;
        for (unsigned m = 1; m < 16; ++m)
            if (FsTevInNeedsMark(k, m, true)) need |= 1u << m;
        if (need) configs[k] = static_cast<int>(need);
    }
    FILE* out = std::fopen(argv[1], "wb");
    if (!out) { std::perror(argv[1]); return 2; }
    size_t pictures = 0, quads = 0;
    std::vector<const FsKey*> list;
    std::vector<int> needs;
    for (const auto& e : configs) { list.push_back(&e.first); needs.push_back(e.second); }
    // every configuration twice (two masks the front end marks it under), 16 quads a picture
    std::vector<std::pair<size_t, unsigned>> jobs;
    for (size_t i = 0; i < list.size(); ++i) {
        std::vector<unsigned> ms;
        for (unsigned m = 1; m < 16; ++m) if ((needs[i] >> m) & 1u) ms.push_back(m);
        jobs.push_back({i, ms.back()});         // (the widest)
        jobs.push_back({i, ms[ms.size() / 2]}); // (and another)
    }
    Rng r(0xCA04);
    for (size_t j = 0; j < jobs.size(); j += 16) {
        Frame();
        for (size_t q = 0; q < 16 && j + q < jobs.size(); ++q) {
            DrawConfig(*list[jobs[j + q].first], r, static_cast<int>(q % 4), static_cast<int>(q / 4), jobs[j + q].second);
            ++quads;
        }
        std::vector<uint8_t> px;
        if (!ReadPicture(px)) { Check(false, "read the picture"); break; }
        std::fwrite(px.data(), 1, px.size(), out);
        ++pictures;
    }
    std::fclose(out);
    std::printf("pictures: %zu configurations, %zu quads in %zu pictures of 64x64 written\n", list.size(), quads, pictures);
    Check(RsCacheMode() == 0 || KeyHashOf(g) == g.keyHash, "incremental key hash equals KeyHashOf after the pictures");
    {
        std::vector<FsKey> listed;
        Check(ReadList(listPath.c_str(), listed) && !listed.empty(), "the warm-up list was read and is readable");
        unsigned marked = 0, withMask = 0, wrong = 0;
        for (const FsKey& k : listed) {
            marked += (k.inWrap & 1) != 0;
            withMask += k.preWrap() != 0;
            FsKey c = k;
            if (CanonListFsKey(c)) ++wrong;
        }
        std::printf("warm-up list after the run: %zu fragment keys, %u marked, %u with a preWrap mask, %u not in the form this mode makes\n", listed.size(),
                    marked, withMask, wrong);
        Check(wrong == 0, "the warm-up list's keys are the front end's keys of this mode (CanonListFsKey)");
        Check(TevInCanonMode() == 2 ? withMask > 0 : withMask == 0, "=2: marked list keys carry their preWrap mask; otherwise none does");
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
