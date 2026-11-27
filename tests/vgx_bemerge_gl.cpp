// GXV_BE_MERGE (ta24): adjacent draw commands that are one draw call (same program, state, textures, uniforms, buffer; index ranges that
// follow each other): the production GX front end (display-list cache included) and the production GLES back end on Mesa.
//
// Objects are drawn from display lists in guest RAM that become static buffers: lists of several GX batches (one record per batch run,
// adjacent index ranges of one buffer), the same list twice in a row under the same matrix and material (an adjacent pair with an
// ASCENDING or a repeated range), lists with a matrix or a material change between the batches (never mergeable), in opaque,
// alpha-blended (draw order matters: overlapping translucent batches), depth-less and textured materials. The picture of every frame is read
// back and the hashes folded together; the runner compares them with GXV_BE_MERGE=0, 1 and 2: bit for bit.
// Usage: vgx_bemerge_gl [frames]
#include "vgx_state.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
}
// (the basic log: the back end's "be-merge:" line comes here)
extern "C" void port_log_summary(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::printf("LOG ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
}
namespace {
using namespace vgx;
constexpr int W = 128, H = 96;
alignas(32) unsigned char destinations[4][32];
alignas(4096) uint8_t ram[1 << 16];
unsigned failures = 0;
void Check(bool ok, const char* label) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
void BP(unsigned r, unsigned v) { WriteBP(r << 24 | v); }
void XF(unsigned r, unsigned v) { WriteXF(r, 1, &v); }

size_t g_records = 0, g_static = 0;
uint64_t g_recordSum = 0;
void Observe(CmdBuf& b) {
    g_records = b.draws.size();
    g_static = 0;
    for (const DrawRec& d : b.draws) g_static += d.vbuf != 0;
}
// BEMERGE_SPLIT=1: every triangle draw record of >= 2 triangles is cut into two (three for every fourth) adjacent records at a triangle
// boundary, same state, same blocks, ranges that follow each other: the picture must stay the one the unsplit records give, whatever
// the back end does with the pair (GXV_BE_MERGE=0 draws two calls, 1 one). =2: the second part of every third record names copies of
// the uniform blocks at other offsets (same values: the merge must see the values).
int g_split = 0, g_vary = 0;
uint64_t g_splitMade = 0, g_varied = 0;
// BEMERGE_VARY=<m> (with BEMERGE_SPLIT=1): the second part of every fifth split record differs from the first in ONE thing the back end must
// compare before it joins two calls: 1 the cull mode, 2 the first texture it samples (none: the back end's white one), 3 the VALUES of the model-view block
// (copied to another offset and the translation changed), 4 a gap in the index range (one triangle left out), 5 the fragment program (a textured record's second part draws with the
// untextured program, its texture binds unchanged). The pictures differ from the unsplit ones then; the runner compares GXV_BE_MERGE=0 with 1 (and the
// mutations of the back end's tests must change them).
std::vector<DrawRec> g_example[2]; // (the last untextured [0] / textured [1] record seen: the untextured fragment program for BEMERGE_VARY=5)
void SplitDraws(CmdBuf& b) {
    std::vector<Cmd> out;
    out.reserve(b.cmds.size() * 2);
    unsigned n = 0;
    for (const Cmd& c : b.cmds) {
        out.push_back(c);
        if (c.type != CMD_DRAW) continue;
        const DrawRec d = b.draws[c.arg];
        if (d.prim != PRIM_TRIANGLES || d.idxCount < 6 || d.idxCount % 3) continue;
        ++n;
        const uint32_t tri = d.idxCount / 3;
        const uint32_t k = 1 + (n * 7919u) % (tri - 1);
        DrawRec second = d;
        b.draws[c.arg].idxCount = k * 3;
        second.idxOffset += k * 3;
        second.idxCount -= k * 3;
        if (g_split == 2 && n % 3 == 0)
            for (int blk = 0; blk < UB_COUNT; ++blk) {
                const uint32_t at = second.ublock[blk];
                if (at == kNoBlock || at + kUbSize[blk] > b.uni.size()) continue;
                const size_t now = b.uni.size();
                b.uni.resize(now + kUbSize[blk]);
                std::memcpy(b.uni.data() + now, b.uni.data() + at, kUbSize[blk] * 4);
                second.ublock[blk] = static_cast<uint32_t>(now);
            }
        if (g_vary && n % 5 == 0) {
            switch (g_vary) {
            case 1: second.rs.cull = second.rs.cull == CULL_NONE ? CULL_BACK : CULL_NONE; break;
            case 2: // the first sampled texture: none (the back end's white one) instead of the copy
                for (int m = 0; m < 8; ++m)
                    if (second.texMask & (1u << m)) { second.tex[m].texId = 0; break; }
                break;
            case 3: // the model-view block's VALUES (a copy at another offset, the translation moved): a different place on the screen
                if (second.ublock[UB_MV] != kNoBlock && second.ublock[UB_MV] + kUbSize[UB_MV] <= b.uni.size()) {
                    const size_t now = b.uni.size();
                    b.uni.resize(now + kUbSize[UB_MV]);
                    std::memcpy(b.uni.data() + now, b.uni.data() + second.ublock[UB_MV], kUbSize[UB_MV] * 4);
                    b.uni[now + 3] += 0.15f;
                    b.uni[now + 7] -= 0.1f;
                    second.ublock[UB_MV] = static_cast<uint32_t>(now);
                }
                break;
            case 4:
                if (second.idxCount >= 6) { second.idxOffset += 3; second.idxCount -= 3; }
                break;
            case 5:
                if (second.texMask) // (a textured record's second part: the untextured fragment program, the texture binds as they were)
                    for (const DrawRec& e : g_example[0])
                        if (e.fsId != second.fsId) { second.fsId = e.fsId; ++g_varied; }
                break;
            }
        }
        {
            auto& e = g_example[d.texMask ? 1 : 0]; // (0: untextured, 1: textured)
            e.assign(1, d);
        }
        b.draws.push_back(second);
        out.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1)});
        ++g_splitMade;
        if (n % 4 == 0 && second.idxCount >= 6) { // a third part
            DrawRec third = second;
            const uint32_t t2 = second.idxCount / 3, k2 = 1 + (n * 104729u) % (t2 - 1);
            b.draws.back().idxCount = k2 * 3;
            third.idxOffset += k2 * 3;
            third.idxCount -= k2 * 3;
            b.draws.push_back(third);
            out.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1)});
            ++g_splitMade;
        }
    }
    b.cmds.swap(out);
}
} // namespace
namespace vgx {
void SubmitToBackend(CmdBuf& b) {
    if (g_split) SplitDraws(b);
    Observe(b);
    backend::Execute(b);
}
} // namespace vgx
namespace {
void Viewport(float w = W, float h = H) {
    const uint32_t v[] = {f2u(w / 2.0f), f2u(-h / 2.0f), f2u(16777215.0f + 1), f2u(w / 2.0f + 342), f2u(h / 2.0f + 342), f2u(16777215.0f + 1)};
    WriteXF(0x101A, 6, v);
}
void Scissor() {
    BP(0x20, 342u | (342u << 12));
    BP(0x21, (H - 1 + 342u) | ((W - 1 + 342u) << 12));
    BP(0x59, 171u | (171u << 10));
}
void PosRows(unsigned row, const float m[12]) {
    uint32_t w[12];
    for (int i = 0; i < 12; ++i) w[i] = f2u(m[i]);
    WriteXF(row * 4, 12, w);
}
void NrmRows(unsigned row, const float m[9]) {
    uint32_t w[9];
    for (int i = 0; i < 9; ++i) w[i] = f2u(m[i]);
    WriteXF(0x400 + row * 3, 9, w);
}
// GX projection registers (XF 0x1020..0x1026)
void Perspective(float fovY, float aspect, float n, float f) {
    const float t = 1.0f / std::tan(fovY * 0.5f);
    const uint32_t p[] = {f2u(t / aspect), 0, f2u(t), 0, f2u(-n / (f - n)), f2u(-(f * n) / (f - n)), 0};
    WriteXF(0x1020, 7, p);
}
void Ortho(float half, float n, float f) {
    const float s = 1.0f / half;
    const uint32_t p[] = {f2u(s), 0, f2u(s), 0, f2u(-1.0f / (f - n)), f2u(-f / (f - n)), 1};
    WriteXF(0x1020, 7, p);
}
// The camera: eye position and yaw / pitch (view = R^T * (p - eye)).
struct Cam { float ex, ey, ez, yaw, pitch; };
struct Placement { float cx, cy, cz, scale; };
// Position matrix slot `slot` (0: PNMTX0, 1: PNMTX1, rows 3..5) = view * model of object `id` at p.
void LoadMatrix(unsigned slot, int id, const Placement& p, const Cam& c) {
    const float a = 0.37f * id + 0.1f, b = 0.23f * id - 0.3f;
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
    const float r[9] = {ca, -sa, 0, sa * cb, ca * cb, -sb, sa * sb, ca * sb, cb}; // model rotation
    // view rotation: yaw about y, then pitch about x
    const float cy = std::cos(c.yaw), sy = std::sin(c.yaw), cp = std::cos(c.pitch), sp = std::sin(c.pitch);
    const float v[9] = {cy, 0, -sy, sy * sp, cp, cy * sp, sy * cp, -sp, cy * cp};
    float vr[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) vr[i * 3 + j] = v[i * 3] * r[j] + v[i * 3 + 1] * r[3 + j] + v[i * 3 + 2] * r[6 + j];
    const float tx = p.cx - c.ex, ty = p.cy - c.ey, tz = p.cz - c.ez;
    const float s = p.scale;
    float m[12];
    for (int i = 0; i < 3; ++i) {
        m[i * 4 + 0] = vr[i * 3 + 0] * s;
        m[i * 4 + 1] = vr[i * 3 + 1] * s;
        m[i * 4 + 2] = vr[i * 3 + 2] * s;
        m[i * 4 + 3] = v[i * 3] * tx + v[i * 3 + 1] * ty + v[i * 3 + 2] * tz;
    }
    PosRows(slot * 3, m);
    NrmRows(slot * 3, vr);
    XF(0x1018, (0x3CF3CF00u & ~0x3Fu) | (slot * 3));
}
void Light() {
    XF(0x100A, 0x303030FFu);
    uint32_t w[16] = {};
    w[3] = 0xFFE0C0FFu;
    w[4] = f2u(1.0f);
    w[7] = f2u(1.0f);
    w[10] = f2u(0.3f); w[11] = f2u(0.4f); w[12] = f2u(3.0f);
    WriteXF(0x600, 16, w);
}
void Material(char m) {
    const bool tex = m == 'C';
    BP(0, (tex ? 1u : 0u) | (1u << 4));
    XF(0x1009, 1);
    XF(0x103F, tex ? 1u : 0u);
    XF(0x1040, 1u << 7);
    XF(0x100C, m == 'B' ? 0xE0A0FF90u : m == 'D' ? 0x40C0FFFFu : 0xFFFFFFFFu);
    XF(0x100E, m == 'D' ? 0u : ((1u << 1) | (1u << 2) | (2u << 7)));
    XF(0x1010, 0);
    BP(0x28, tex ? (1u << 6) : 0u);
    if (tex) BP(0xC0, (15u << 12) | (8u << 8) | (10u << 4) | 15u | (1u << 19));
    else BP(0xC0, (10u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (5u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, m == 'D' ? 0u : (m == 'B' ? (1u | (3u << 1)) : (1u | (3u << 1) | (1u << 4))));
    BP(0x41, m == 'B' ? (1u | (1u << 3) | (1u << 4) | (4u << 8) | (5u << 5)) : ((1u << 3) | (1u << 4)));
    BP(0x42, 0);
}
uint32_t Copy(int index, bool clear) {
    g.texCopySrc = EfbRect{0, 0, W, H};
    g.texCopyDstW = W; g.texCopyDstH = H; g.texCopyFmt = 6;
    QueueCopyTex(destinations[index], clear);
    return Pending().copyTex.back().texId;
}
void BindCopy(int index) {
    BP(0x94, (reinterpret_cast<uintptr_t>(destinations[index]) >> 5) & 0xFFFFFF);
    BP(0x88, (W - 1) | ((H - 1) << 10) | (1u << 20));
    auto& t = g.tex[0]; t = TexSlot{}; t.valid = true;
    t.obj.data = destinations[index]; t.obj.width = W; t.obj.height = H; t.obj.format = 6;
    t.obj.minFilter = t.obj.magFilter = 1;
    g.bpTexStale = 0; g.texDirty = true; g.texSlotDirty = 1;
}
uint64_t Fold(uint64_t h, const std::vector<uint8_t>& p) {
    for (uint8_t b : p) h = (h ^ b) * 1099511628211ull;
    return h;
}
unsigned Distinct(const std::vector<uint8_t>& p) {
    std::map<uint32_t, int> seen;
    for (size_t i = 0; i + 3 < p.size(); i += 4) seen[p[i] | p[i + 1] << 8 | p[i + 2] << 16]++;
    return static_cast<unsigned>(seen.size());
}
void Put32(uint8_t* p, uint32_t v) {
    for (unsigned i = 0; i != 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}

// ---- the lists: a bumpy disc of `tris` triangles (3 * tris vertices), indexed position and normal (the same index), 16-bit indices
struct Mesh {
    int tris = 0;
    uint8_t* list = nullptr;
    uint32_t listBytes = 0;
    uint8_t* pos = nullptr;
    uint8_t* nrm = nullptr;
};
Mesh g_mesh[4];
uint32_t g_ramTop = 0;
uint8_t* Take(uint32_t bytes) {
    uint8_t* p = ram + g_ramTop;
    g_ramTop = (g_ramTop + bytes + 63u) & ~63u;
    return p;
}
void MakeMesh(Mesh& m, int tris) {
    const float pi = 3.14159265f;
    m.tris = tris;
    const int unique = tris + 1;
    m.pos = Take(static_cast<uint32_t>(unique) * 12);
    m.nrm = Take(static_cast<uint32_t>(unique) * 12);
    auto put = [](uint8_t* p, float x, float y, float z) {
        Put32(p, f2u(x)); Put32(p + 4, f2u(y)); Put32(p + 8, f2u(z));
    };
    put(m.pos, 0.0f, 0.0f, 0.3f);
    put(m.nrm, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < tris; ++i) {
        const float a = 2 * pi * i / tris;
        put(m.pos + (1 + i) * 12, std::cos(a), std::sin(a), 0.25f * std::cos(3 * a));
        put(m.nrm + (1 + i) * 12, 0.5f * std::cos(a), 0.5f * std::sin(a), 0.8f);
    }
    m.listBytes = 3 + static_cast<uint32_t>(tris) * 3 * 4;
    m.list = Take(m.listBytes);
    uint8_t* p = m.list;
    *p++ = 0x90; // GX_TRIANGLES, vertex format 0
    *p++ = static_cast<uint8_t>(tris * 3 >> 8);
    *p++ = static_cast<uint8_t>(tris * 3);
    for (int i = 0; i < tris; ++i)
        for (int k = 0; k < 3; ++k) {
            const unsigned ix = k == 0 ? 0u : static_cast<unsigned>(1 + (i + k - 1) % tris);
            *p++ = static_cast<uint8_t>(ix >> 8); *p++ = static_cast<uint8_t>(ix); // position
            *p++ = static_cast<uint8_t>(ix >> 8); *p++ = static_cast<uint8_t>(ix); // normal
        }
}
// (ta23, GXV_STATICCULL_PAL) the same disc with a direct position-matrix index per vertex: the centre and the odd rim vertices name
// rows 0..2 (PNMTX0), the even rim vertices rows 3..5 (PNMTX1): a "skinned" shape of two joints.
void MakeSkinMesh(Mesh& m, const Mesh& from) {
    m = from;
    m.listBytes = 3 + static_cast<uint32_t>(m.tris) * 3 * 5;
    m.list = Take(m.listBytes);
    uint8_t* p = m.list;
    *p++ = 0x90;
    *p++ = static_cast<uint8_t>(m.tris * 3 >> 8);
    *p++ = static_cast<uint8_t>(m.tris * 3);
    for (int i = 0; i < m.tris; ++i)
        for (int k = 0; k < 3; ++k) {
            const unsigned ix = k == 0 ? 0u : static_cast<unsigned>(1 + (i + k - 1) % m.tris);
            *p++ = static_cast<uint8_t>(ix == 0 || (ix & 1) ? 0 : 3); // PNMTXIDX
            *p++ = static_cast<uint8_t>(ix >> 8); *p++ = static_cast<uint8_t>(ix);
            *p++ = static_cast<uint8_t>(ix >> 8); *p++ = static_cast<uint8_t>(ix);
        }
}
Mesh g_skin[3];
// (ta23, GXV_DLVCACHE) a bumpy w x h grid drawn as GX triangle strips of 3..8 vertices along its rows, as J3D's exporter writes a
// shape: with GXV_DLWELD the strips share their vertices and the triangle order decides the vertex-cache hits
Mesh g_grid;
void MakeGrid(Mesh& m, int w, int h) {
    m.pos = Take(static_cast<uint32_t>(w * h) * 12);
    m.nrm = Take(static_cast<uint32_t>(w * h) * 12);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // (the upper half folded back over the lower one, a little above it and lit differently: where the two layers overlap
            // on the screen the depth test decides, or, without one, the order of the triangles)
            const bool fold = y >= h / 2;
            const float fx = -1.0f + 2.0f * x / (w - 1), fy0 = -1.0f + 2.0f * y / (h - 1), fy = fold ? -fy0 + 0.1f : fy0;
            const float z = 0.15f * std::sin(3.0f * fx) * std::cos(2.0f * fy) + (fold ? 0.12f + 0.3f * (fy0 - fy) * 0.2f : 0.0f);
            uint8_t* p = m.pos + (y * w + x) * 12;
            Put32(p, f2u(fx)); Put32(p + 4, f2u(fy)); Put32(p + 8, f2u(z));
            uint8_t* n = m.nrm + (y * w + x) * 12;
            Put32(n, f2u(fold ? 0.0f : -0.3f * fx)); Put32(n + 4, f2u(fold ? 0.0f : 0.2f * fy)); Put32(n + 8, f2u(fold ? -1.0f : 0.9f));
        }
    std::vector<uint8_t> l;
    unsigned r = 99;
    m.tris = 0;
    for (int yy = 0; yy + 1 < h; ++yy) {
        // (rows from the two layers in turn: the list's order and a vertex-cache order put them differently over each other)
        const int y = (yy & 1) ? h - 2 - yy / 2 : yy / 2;
        std::vector<unsigned> row;
        for (int x = 0; x < w; ++x) { row.push_back(static_cast<unsigned>(y * w + x)); row.push_back(static_cast<unsigned>((y + 1) * w + x)); }
        for (size_t st = 0; st + 2 < row.size();) {
            r = r * 1103515245u + 12345u;
            size_t len = 3 + (r >> 8) % 6;
            if (st + len > row.size()) len = row.size() - st;
            l.push_back(0x98);
            l.push_back(0);
            l.push_back(static_cast<uint8_t>(len));
            for (size_t k = 0; k < len; ++k) {
                const unsigned ix = row[st + k];
                l.push_back(static_cast<uint8_t>(ix >> 8)); l.push_back(static_cast<uint8_t>(ix));
                l.push_back(static_cast<uint8_t>(ix >> 8)); l.push_back(static_cast<uint8_t>(ix));
            }
            m.tris += static_cast<int>(len) - 2;
            st += len - 2;
        }
    }
    m.listBytes = static_cast<uint32_t>((l.size() + 31) & ~size_t(31));
    m.list = Take(m.listBytes);
    std::memset(m.list, 0, m.listBytes);
    std::memcpy(m.list, l.data(), l.size());
    m.tris = w * h - 1; // (DrawList: the arrays' size is (tris + 1) * 12 bytes)
}
void DrawList(const Mesh& m) {
    const uint32_t bytes = static_cast<uint32_t>(m.tris + 1) * 12;
    g.arrays[9] = {m.pos, bytes, 12, false};
    g.arrays[10] = {m.nrm, bytes, 12, false};
    g.cpArrayStale = 0;
    CallList(m.list, m.listBytes);
}

// ---- the lists: a bumpy disc of `tris` triangles, drawn as `batches` GX batches (GXBegin ... one vertex-count header each) in ONE list:
// batch b is a fan around hub vertex h(b) (the centre for even b, a rim vertex for odd b), so the batches overlap on the screen with
// different depths and shading: for the translucent material the order of the triangles decides the colours.
Mesh g_mm[4];
void MakeMulti(Mesh& m, int tris, int batches) {
    const float pi = 3.14159265f;
    m.tris = tris;
    const int unique = tris + 1;
    m.pos = Take(static_cast<uint32_t>(unique) * 12);
    m.nrm = Take(static_cast<uint32_t>(unique) * 12);
    auto put = [](uint8_t* p, float x, float y, float z) { Put32(p, f2u(x)); Put32(p + 4, f2u(y)); Put32(p + 8, f2u(z)); };
    put(m.pos, 0.0f, 0.0f, 0.3f);
    put(m.nrm, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < tris; ++i) {
        const float a = 2 * pi * i / tris;
        put(m.pos + (1 + i) * 12, std::cos(a), std::sin(a), 0.25f * std::cos(3 * a));
        put(m.nrm + (1 + i) * 12, 0.5f * std::cos(a), 0.5f * std::sin(a), 0.8f);
    }
    const int per = tris / batches;
    std::vector<uint8_t> l;
    auto vtx = [&](unsigned ix) {
        l.push_back(static_cast<uint8_t>(ix >> 8)); l.push_back(static_cast<uint8_t>(ix));
        l.push_back(static_cast<uint8_t>(ix >> 8)); l.push_back(static_cast<uint8_t>(ix));
    };
    for (int b = 0; b < batches; ++b) {
        const unsigned hub = b % 2 == 0 ? 0u : static_cast<unsigned>(1 + (b * 7) % tris);
        const int n = b == batches - 1 ? tris - per * (batches - 1) : per;
        const int kind = (b + (batches > 2 ? 1 : 0)) % 3; // 0 triangles, 1 strip, 2 fan: another kind is another run of the list's static buffer
        const int count = kind == 0 ? n * 3 : n + 2;
        l.push_back(kind == 0 ? 0x90 : kind == 1 ? 0x98 : 0xA0);
        l.push_back(static_cast<uint8_t>(count >> 8));
        l.push_back(static_cast<uint8_t>(count));
        if (kind == 0) {
            for (int i = 0; i < n; ++i) {
                const int t = b * per + i;
                for (int k = 0; k < 3; ++k) vtx(k == 0 ? hub : static_cast<unsigned>(1 + (t + k - 1) % tris));
            }
        } else if (kind == 1) {
            for (int i = 0; i < count; ++i) vtx(i % 2 == 0 ? static_cast<unsigned>(1 + (b * per + i / 2) % tris) : hub);
        } else {
            vtx(hub);
            for (int i = 0; i < count - 1; ++i) vtx(static_cast<unsigned>(1 + (b * per + i) % tris));
        }
    }
    m.listBytes = static_cast<uint32_t>((l.size() + 31) & ~size_t(31));
    m.list = Take(m.listBytes);
    std::memset(m.list, 0, m.listBytes);
    std::memcpy(m.list, l.data(), l.size());
}
Cam CameraAt(int f) {
    Cam c{0, 0, 0, 0.05f * f, 0};
    c.ez = -0.02f * (f % 40);
    c.ex = 0.03f * (f % 25);
    c.pitch = 0.3f * std::sin(0.4f * f);
    return c;
}
} // namespace

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 40;
    g_split = std::getenv("BEMERGE_SPLIT") ? std::atoi(std::getenv("BEMERGE_SPLIT")) : 0;
    g_vary = std::getenv("BEMERGE_VARY") ? std::atoi(std::getenv("BEMERGE_VARY")) : 0;
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(W, H), "production backend init");
    if (failures) return 1;
    ResetState(); g.efbW = W; g.efbH = H; QueueSetTarget(); BP(0x43, 1);
    const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(-1), f2u(-1), 1};
    WriteXF(0x1020, 7, proj);
    Viewport(); Scissor(); Light();
    const float tm[12] = {0.5f, 0, 0, 0.5f, 0, 0.5f, 0, 0.5f, 0, 0, 0, 1};
    PosRows(60, tm);
    BP(0x4F, 30u | (255u << 8)); BP(0x50, (60u << 8) | 80u); BP(0x51, 0xFFFFFFu);
    SetVcd(9, 3);
    SetVcd(10, 3);
    SetVat(0, 9, 1, 4, 0);
    SetVat(0, 10, 0, 4, 0);
    MakeMulti(g_mm[0], 48, 2);
    MakeMulti(g_mm[1], 72, 3);
    MakeMulti(g_mm[2], 120, 4);
    MakeMulti(g_mm[3], 36, 1);
    hle::RegisterGuestRam(ram, sizeof ram);
    // a first picture: the texture of the textured material
    const Cam still{0, 0, 0, 0, 0};
    Material('A'); BP(0x40, 1u | (7u << 1) | (1u << 4));
    Copy(3, true);
    for (int i = 0; i < 6; ++i) {
        Material(i & 1 ? 'B' : 'A');
        LoadMatrix(0, 100 + i, Placement{-0.6f + 0.25f * i, 0.4f - 0.15f * i, -0.3f, 0.45f}, still);
        DrawList(g_mm[i % 3]);
    }
    Copy(0, true);
    FrontPresent(false, nullptr);
    BindCopy(0);
    uint64_t hash = 14695981039346656037ull;
    std::vector<uint8_t> px;
    int detailed = 0;
    size_t recordSum = 0, staticSum = 0;
    for (int f = 0; f < frames; ++f) {
        Perspective(f % 4 == 3 ? 0.6f : 1.1f, static_cast<float>(W) / H, 0.25f, 40.0f);
        Viewport();
        const int still = std::getenv("BEMERGE_STILL") ? std::atoi(std::getenv("BEMERGE_STILL")) : 0; // (1: the same camera and objects every picture: the lighting census must see no change)
        const Cam cam = CameraAt(still ? 0 : f);
        const char mats[] = "AABCDABAC";
        unsigned r = 4242u + (still ? 0u : static_cast<unsigned>(f));
        auto rnd = [&] { r = r * 1103515245u + 12345u; return ((r >> 8) & 0xFFFF) / 65535.0f; };
        char cur = 0;
        for (int i = 0; i < 60; ++i) {
            const char mat = mats[i % 9];
            if (mat != cur) { Material(mat); cur = mat; }
            const float ang = 6.2831853f * rnd(), dist = 1.5f + 6.0f * rnd();
            const Placement p{dist * std::sin(ang), (rnd() - 0.5f) * 3.0f, -dist * std::cos(ang), 0.3f + 0.9f * rnd()};
            LoadMatrix(0, i + 1, p, cam);
            DrawList(g_mm[i % 4]);
            // the same list again under the same matrix and material: its runs repeat the ranges (never adjacent-ascending)
            if (i % 5 == 1) DrawList(g_mm[i % 4]);
            // a second list under the same matrix right after: another buffer, so no merge with the first one's last run
            if (i % 7 == 2) DrawList(g_mm[(i + 1) % 4]);
        }
        const uint32_t id = Copy(1, true);
        FrontPresent(false, nullptr);

        int pw = 0, ph = 0;
        if (!backend::DebugReadTexture(id, px, pw, ph) || pw != W || ph != H) { Check(false, "read the renderer's picture"); break; }
        hash = Fold(hash, px);
        detailed += Distinct(px) > 40;
        recordSum += g_records;
        staticSum += g_static;
        if (std::getenv("BEMERGE_PPM") && f < 6) {
            char name[64];
            std::snprintf(name, sizeof name, "%s%02d.ppm", std::getenv("BEMERGE_PPM"), f);
            if (FILE* o = std::fopen(name, "wb")) {
                std::fprintf(o, "P6 %d %d 255\n", W, H);
                for (int i = 0; i < W * H; ++i) std::fwrite(&px[static_cast<size_t>(i) * 4], 1, 3, o);
                std::fclose(o);
            }
        }
    }
    Check(detailed * 2 >= frames, "most pictures have detail");
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    std::printf("picture %dx%d hash %016llx\n", W, H, static_cast<unsigned long long>(hash));
    std::printf("records %llu static %llu split %llu varied-program %llu\n", (unsigned long long)recordSum, (unsigned long long)staticSum, (unsigned long long)g_splitMade, (unsigned long long)g_varied);
    return failures ? 1 : 0;
}
