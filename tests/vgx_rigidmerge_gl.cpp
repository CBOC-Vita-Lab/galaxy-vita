// GXV_RIGIDMERGE: rigid draws that differ only in their model-view matrix become one draw with the matrix palette; the production GX front
// end and the production GLES back end on Mesa.
//
// Many rigid objects (no per-vertex matrix: each has its own matrix, loaded into position-matrix row 0 before it is drawn) are drawn in
// runs of four materials (opaque lit and depth tested; blended lit; textured with a texgen from the normal, its texture the renderer's own
// earlier picture; unlit). Objects overlap, so that the order of the depth and blend operations shows in the picture. Some objects are
// small (CPU-transformed with the default GXV_SOFTXFORM), two in a row have the same matrix, and a skinned draw whose vertex key is the
// palette twin of the rigid one sits between runs.
//   * the picture is read back and hashed; the runner compares the hash across GXV_RIGIDMERGE 0 / 1 / 2 (and invalid values) and the other
//     front-end switches: bit for bit;
//   * the records the back end is given are counted and compared with a model of the merging (cap 10 slots with 1, 4 with 2, none with 0),
//     and so are the records that use a palette shader.
// Usage: vgx_rigidmerge_gl     (the switches are read from the environment)
#include "vgx_state.h"
#include <GLES2/gl2.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
}
namespace {
using namespace vgx;
constexpr int W = 128, H = 96;
alignas(32) unsigned char destinations[4][32];
unsigned failures = 0;
void Check(bool ok, const char* label) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
void BP(unsigned r, unsigned v) { WriteBP(r << 24 | v); }
void XF(unsigned r, unsigned v) { WriteXF(r, 1, &v); }

// ---- what the back end is given
std::map<uint16_t, const ShaderSource*> g_vs;
size_t g_records = 0, g_palRecords = 0;
bool g_count = false;
void Observe(CmdBuf& b) {
    for (const Cmd& c : b.cmds)
        if (c.type == CMD_SHADER && c.arg < b.shaders.size() && !b.shaders[c.arg]->fragment) g_vs[b.shaders[c.arg]->id] = b.shaders[c.arg];
    if (!g_count) return;
    for (const DrawRec& d : b.draws) {
        ++g_records;
        if (std::getenv("RM_DUMP")) std::printf("rec %zu: vs %u fs %u verts %u idx %u mv %u pal %u\n", g_records, d.vsId, d.fsId, d.vtxCount, d.idxCount, d.ublock[UB_MV], d.ublock[UB_PAL]);
        auto it = g_vs.find(d.vsId);
        if (it != g_vs.end() && it->second->usesPalette) ++g_palRecords;
    }
}
} // namespace
namespace vgx {
void SubmitToBackend(CmdBuf& b) {
    Observe(b);
    backend::Execute(b);
}
} // namespace vgx
namespace {
void Viewport() {
    const uint32_t v[] = {f2u(W / 2.0f), f2u(-H / 2.0f), f2u(16777215.0f + 1), f2u(W / 2.0f + 342), f2u(H / 2.0f + 342), f2u(16777215.0f + 1)};
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
// Matrix `id`: a rotation, a uniform scale and a translation, all different per id; loaded into rows `row`.
struct Placement { float cx, cy, cz, scale; };
void LoadMatrix(unsigned row, int id, const Placement& p) {
    const float a = 0.37f * id + 0.1f, b = 0.23f * id - 0.3f;
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
    const float r[9] = {ca, -sa, 0, sa * cb, ca * cb, -sb, sa * sb, ca * sb, cb};
    const float s = p.scale;
    const float m[12] = {r[0] * s, r[1] * s, r[2] * s, p.cx, r[3] * s, r[4] * s, r[5] * s, p.cy, r[6] * s, r[7] * s, r[8] * s, p.cz};
    PosRows(row, m);
    NrmRows(row, r);
}
void Light() {
    XF(0x100A, 0x303030FFu); // ambient
    uint32_t w[16] = {};
    w[3] = 0xFFE0C0FFu;      // colour
    w[4] = f2u(1.0f);
    w[7] = f2u(1.0f);
    w[10] = f2u(0.3f); w[11] = f2u(0.4f); w[12] = f2u(3.0f); // position
    WriteXF(0x600, 16, w);
}
// A bumpy disc of `tris` triangles around its centre, with normals: model space, radius 1.
struct Vtx { float x, y, z, idx, nx, ny, nz; };
std::vector<Vtx> Disc(int tris, float idx) {
    std::vector<Vtx> m;
    const float pi = 3.14159265f;
    auto P = [&](int i) {
        const float a = 2 * pi * (i % tris) / tris;
        const float z = 0.25f * std::cos(3 * a);
        return Vtx{std::cos(a), std::sin(a), z, idx, 0.5f * std::cos(a), 0.5f * std::sin(a), 0.8f};
    };
    for (int i = 0; i < tris; ++i) {
        m.push_back(Vtx{0.0f, 0.0f, 0.3f, idx, 0.0f, 0.0f, 1.0f});
        m.push_back(P(i));
        m.push_back(P(i + 1));
    }
    return m;
}
void DrawMesh(const std::vector<Vtx>& mesh, uint32_t posMtxMax = 0) {
    SetPendingPosMtxMax(posMtxMax);
    DrawDecoded(0x90, VL_NRM, reinterpret_cast<const uint8_t*>(mesh.data()), static_cast<uint32_t>(mesh.size()));
}
// Materials: A opaque lit, B blended lit, C textured (texgen from the normal) x raster colour, D unlit.
void Material(char m) {
    const bool tex = m == 'C';
    BP(0, (tex ? 1u : 0u) | (1u << 4));
    XF(0x1009, 1);
    XF(0x103F, tex ? 1u : 0u);
    XF(0x1040, 1u << 7); // texgen 0: regular, 2x4 matrix, source row 1 (the normal)
    XF(0x100C, m == 'B' ? 0xE0A0FF90u : m == 'D' ? 0x40C0FFFFu : 0xFFFFFFFFu);
    XF(0x100E, m == 'D' ? 0u : ((1u << 1) | (1u << 2) | (2u << 7)));
    XF(0x1010, 0);
    BP(0x28, tex ? (1u << 6) : 0u);
    if (tex) {
        BP(0xC0, (15u << 12) | (8u << 8) | (10u << 4) | 15u | (1u << 19)); // texture colour x raster colour
    } else {
        BP(0xC0, (10u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19)); // a = raster colour, b = c = d = zero
    }
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
uint64_t Hash(const std::vector<uint8_t>& p) {
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : p) h = (h ^ b) * 1099511628211ull;
    return h;
}
unsigned Distinct(const std::vector<uint8_t>& p) {
    std::map<uint32_t, int> seen;
    for (size_t i = 0; i + 3 < p.size(); i += 4) seen[p[i] | p[i + 1] << 8 | p[i + 2] << 16]++;
    return static_cast<unsigned>(seen.size());
}

// ---- the scene: a list of draws
struct Obj {
    char mat;        // A B C D
    int id;          // matrix id (equal ids in a row: the same matrix)
    int tris;        // 44 -> 132 vertices (not CPU-transformed), 8 -> 24 (CPU-transformed, but for material C)
    bool skinned;    // a skinned draw (per-vertex matrix: palette key)
    Placement pl;
};
std::vector<Obj> Scene() {
    std::vector<Obj> s;
    unsigned r = 12345;
    auto rnd = [&] { r = r * 1103515245u + 12345u; return ((r >> 8) & 0xFFFF) / 65535.0f; };
    int id = 1;
    auto add = [&](char mat, int n, int tris, bool sameFirstTwo = false) {
        for (int i = 0; i < n; ++i) {
            Placement p{-0.8f + 1.6f * rnd(), -0.8f + 1.6f * rnd(), -0.25f - 0.5f * rnd(), 0.13f + 0.1f * rnd()};
            int use = id++;
            if (sameFirstTwo && i == 1) { use = s.back().id; p = s.back().pl; }
            s.push_back(Obj{mat, use, (i % 5 == 3 && tris == 44) ? 8 : tris, false, p});
        }
    };
    add('A', 12, 44);                 // longer than a palette: cap 10 / cap 4
    add('B', 5, 44);
    add('A', 1, 44);                  // alone between two other materials
    add('C', 7, 44);                  // textured: never CPU-transformed
    add('A', 3, 44);
    add('D', 2, 44);
    {
        Obj k{'A', 0, 0, true, Placement{0.0f, 0.0f, -0.4f, 0.2f}};
        s.push_back(k);               // skinned: its key is the palette twin of A's
        s.push_back(k);
    }
    add('A', 6, 44, true);            // two in a row with the same matrix
    add('A', 4, 8);                   // small only
    add('B', 3, 44);
    return s;
}
// Records the model expects: `cap` slots per palette (0: none).
void Model(const std::vector<Obj>& scene, uint32_t cap, uint32_t softLimit, size_t& records, size_t& palRecords) {
    records = palRecords = 0;
    bool open = false, skinned = false;
    char mat = 0;
    long newestKey = 0;      // matrix of the newest draw in the record (soft draws: all one key, the identity)
    uint32_t slots = 0;
    auto close = [&] { if (open && !skinned && slots >= 2) ++palRecords; open = false; };
    for (const Obj& o : scene) {
        if (o.skinned) {
            if (open && skinned) continue; // (the same state and the same palette: an ordinary merge)
            close();
            ++records; ++palRecords;
            open = true; skinned = true; slots = 0;
            continue;
        }
        const bool soft = o.mat != 'C' && static_cast<uint32_t>(o.tris * 3) <= softLimit;
        const long key = soft ? -1 : o.id;
        bool join = open && !skinned && mat == o.mat;
        if (join) {
            if (key == newestKey) { /* the same matrix block: an ordinary merge */ }
            else if (cap && slots < cap) ++slots;
            else join = false;
        }
        if (join) { newestKey = key; continue; }
        close();
        ++records;
        open = true; skinned = false; mat = o.mat; slots = 1; newestKey = key;
    }
    close();
}
} // namespace

int main(int, char**) {
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(W, H), "production backend init");
    if (failures) return 1;
    ResetState(); g.efbW = W; g.efbH = H; QueueSetTarget(); BP(0x43, 1);
    const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(-1), f2u(-1), 1};
    WriteXF(0x1020, 7, proj);
    Viewport(); Scissor(); Light();
    // texgen matrix (row 60..62, the identity matrix of GX): u = 0.5 n.x + 0.5, v = 0.5 n.y + 0.5
    const float tm[12] = {0.5f, 0, 0, 0.5f, 0, 0.5f, 0, 0.5f, 0, 0, 0, 1};
    PosRows(60, tm);
    // clear (colour and depth), then a first picture with big overlapping shapes: the texture of the textured material
    BP(0x4F, 30u | (255u << 8)); BP(0x50, (60u << 8) | 80u); BP(0x51, 0xFFFFFFu);
    Material('A'); BP(0x40, 1u | (7u << 1) | (1u << 4));
    Copy(3, true);
    for (int i = 0; i < 6; ++i) {
        Material(i & 1 ? 'B' : 'A');
        LoadMatrix(0, 100 + i, Placement{-0.6f + 0.25f * i, 0.4f - 0.15f * i, -0.3f, 0.45f});
        DrawMesh(Disc(44, 0.0f));
    }
    const uint32_t texSource = Copy(0, true);
    FrontPresent(false, nullptr);
    (void)texSource;
    BindCopy(0);
    // the scene
    const std::vector<Obj> scene = Scene();
    g_count = true;
    char cur = 0;
    for (const Obj& o : scene) {
        if (o.skinned) {
            Material('A');
            cur = 'A';
            SetVcd(0, 1);
            LoadMatrix(0, 7, o.pl);
            LoadMatrix(3, 8, Placement{o.pl.cx + 0.3f, o.pl.cy, o.pl.cz, o.pl.scale});
            std::vector<Vtx> a = Disc(44, 0.0f), b = Disc(44, 3.0f);
            a.insert(a.end(), b.begin(), b.end());
            DrawMesh(a, 3);
            SetVcd(0, 0);
            continue;
        }
        if (o.mat != cur) { Material(o.mat); cur = o.mat; }
        LoadMatrix(0, o.id, o.pl);
        DrawMesh(Disc(o.tris, 0.0f));
    }
    const uint32_t id = Copy(1, false);
    FrontPresent(false, nullptr);
    g_count = false;
    std::vector<uint8_t> px;
    int pw = 0, ph = 0;
    Check(backend::DebugReadTexture(id, px, pw, ph), "read the renderer's picture");
    Check(pw == W && ph == H, "picture size");
    // ---- the count of records
    const char* sw = std::getenv("GXV_RIGIDMERGE");
    const long mode = sw ? std::strtol(sw, nullptr, 10) : 0;
    const char* pk = std::getenv("GXV_UNIPACK");
    const bool packed = !(pk && std::atol(pk) == 0);
    const char* sl = std::getenv("GXV_SOFTXFORM");
    const uint32_t softLimit = sl ? static_cast<uint32_t>(std::strtoul(sl, nullptr, 10)) : 128u;
    const uint32_t cap = !packed ? 0u : mode == 1 ? 10u : mode == 2 ? 4u : 0u;
    size_t modelRecords = 0, modelPal = 0, offRecords = 0, offPal = 0;
    Model(scene, cap, softLimit, modelRecords, modelPal);
    Model(scene, 0, softLimit, offRecords, offPal);
    std::printf("draws: %zu objects; records %zu (%zu with a palette shader); model %zu (%zu); without the switch %zu\n", scene.size(), g_records, g_palRecords,
                modelRecords, modelPal, offRecords);
    Check(g_records == modelRecords, "records equal the model's");
    Check(g_palRecords == modelPal, "palette-shader records equal the model's (runs of two or more matrices, and the skinned record)");
    if (cap) Check(g_records < offRecords, "fewer records than without the switch");
    else Check(g_records == offRecords, "switch off: no merging beyond identical matrices");
    uint64_t counters[2];
    RigidMergeCounters(counters);
    std::printf("rigid runs %llu, draws joined %llu\n", static_cast<unsigned long long>(counters[0]), static_cast<unsigned long long>(counters[1]));
    if (!cap) Check(counters[0] == 0 && counters[1] == 0, "no palette run made while the switch is off");
    Check(Distinct(px) > 60, "the picture has detail");
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    std::printf("picture %dx%d distinct colours %u hash %016llx\n", pw, ph, Distinct(px), static_cast<unsigned long long>(Hash(px)));
    std::printf("records %zu\n", g_records);
    return failures ? 1 : 0;
}
