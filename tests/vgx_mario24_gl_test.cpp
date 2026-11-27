// mario24 on the real GLES back end and Mesa (tools/test_vgx_mario24_gl.sh runs every scenario with the environment it needs): the switches that
// change only the player's pixels (agents/mario24/REPORT.txt).
//   lod <rung> <player eighths> <other eighths>   GXV_PLAYER_LODBIAS: two mipmapped textures, one uploaded with TexUpload::player, drawn one after
//                                                 the other at the rung; the GL_TEXTURE_LOD_BIAS each is given (0: never set)
//   aniso <expected hook calls, "-" separated>    GXV_PLAYER_ANISO: the anisotropic switch's calls while the player's trilinear texture is bound the first
//                                                 time, with the rest's anisotropy off; the other texture makes none
//   present <mode>                                GXV_EDGE_AA_PLAYER / GXV_PLAYER_SHARPEN at the present (960x544, 1:1, the whole-picture edge smoothing on):
//                                                 a 45-degree white staircase over grey; the picture with a player rectangle against the same frame
//                                                 without one. off: identical bit for bit and no pass counted; on: identical outside the rectangle (the
//                                                 screen's pixels, 2 px of slack), different inside, no scene added, the pass counted once
#include "vgx_autoq.h"
#include "vgx_cmd.h"

#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
bool IsCharaTexture(uint32_t) { return false; }
bool VtxPackOn() { return false; }
void VtxPackCounters(uint64_t*) {}
void VtxPackDrawCounters(uint64_t*) {}
}
namespace {
std::vector<int> lodSets;
std::vector<unsigned> hookCalls;
int g_fail = 0;
void Check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++g_fail;
}
void Hook(unsigned filter) { hookCalls.push_back(filter); }
}
extern "C" void __real_glTexParameteri(GLenum, GLenum, GLint);
extern "C" void __wrap_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    if (pname == 0x8501) lodSets.push_back(param);
    __real_glTexParameteri(target, pname, param);
}

namespace {
using namespace vgx;
void Push(CmdBuf& b, CmdType type, uint32_t arg = 0) { b.cmds.push_back(Cmd{type, arg}); }
void Present(CmdBuf& b, bool clear, const EfbRect* player, bool edge) {
    CopyDispCmd d;
    d.src = EfbRect{0, 0, 640, 456};
    d.clear.enable = clear;
    d.clear.rgba[0] = d.clear.rgba[1] = d.clear.rgba[2] = 0.5f;
    d.edge = edge;
    if (player) { d.player[0] = *player; d.playerN = 1; }
    b.copyDisp.push_back(d);
    Push(b, CMD_COPY_DISP, static_cast<uint32_t>(b.copyDisp.size() - 1));
    Push(b, CMD_PRESENT);
}
void AddTextured(CmdBuf& b, uint32_t tex, uint8_t minFilter) {
    const float v[] = {-1, -1, 0, 0, 0, 0, 0, -1, 0, 0, 0.75f, 0, -1, 0, 0, 0, 0, 0.75f, 0, 0, 0, 0, 0.75f, 0.75f};
    const uint32_t first = static_cast<uint32_t>(b.vtx.size() / 24);
    const size_t at = b.vtx.size();
    b.vtx.resize(at + sizeof v);
    std::memcpy(b.vtx.data() + at, v, sizeof v);
    DrawRec d;
    d.vsId = 2;
    d.fsId = 2;
    d.layoutMask = VL_TEX0;
    d.texMask = 1;
    d.tex[0].texId = tex;
    d.tex[0].minFilter = minFilter;
    d.tex[0].magFilter = FILT_LINEAR;
    d.vtxCount = 4;
    d.idxOffset = static_cast<uint32_t>(b.idx.size());
    d.idxCount = 6;
    for (uint16_t i : {0, 1, 2, 2, 1, 3}) b.idx.push_back(static_cast<uint16_t>(first + i));
    d.ublock[UB_PROJ] = 0;
    d.rs.scX = 0;
    d.rs.scW = 640;
    d.rs.scH = 456;
    d.rs.vpH = 456;
    b.draws.push_back(d);
    Push(b, CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1));
}
void Upload(CmdBuf& b, uint32_t id, unsigned side, bool player) {
    TexUpload up;
    up.texId = id;
    up.player = player;
    up.width = up.height = static_cast<uint16_t>(side);
    unsigned levels = 0;
    for (unsigned s = side; s >= 1; s /= 2) ++levels;
    up.authored = static_cast<uint8_t>(levels);
    for (unsigned l = 0, size = side; l < levels; ++l, size /= 2) {
        std::vector<uint8_t> level(static_cast<size_t>(size) * size * 4, static_cast<uint8_t>(40 + 30 * (l % 7)));
        for (size_t a = 3; a < level.size(); a += 4) level[a] = 255;
        up.levels.push_back(std::move(level));
    }
    b.uploads.push_back(up);
    Push(b, CMD_TEX_UPLOAD, static_cast<uint32_t>(b.uploads.size() - 1));
}
uint64_t g_frame = 2;
void Frame(const std::vector<uint32_t>& draw, const std::vector<std::pair<uint32_t, bool>>& uploads) {
    CmdBuf b;
    b.frame = g_frame++;
    b.uni.assign(16, 0);
    for (unsigned i = 0; i < 4; ++i) b.uni[i * 5] = 1;
    for (auto& u : uploads) Upload(b, u.first, 64, u.second);
    for (uint32_t t : draw) AddTextured(b, t, FILT_LIN_MIP_LIN);
    Present(b, false, nullptr, false);
    backend::Execute(b);
}
bool Setup() {
    if (!host::CreateContext()) { std::printf("no GL context\n"); return false; }
    if (!backend::Init(0, 0)) { std::printf("back end Init failed\n"); return false; }
    backend::SetAnisotropicHook(&Hook);
    static ShaderSource vs, fs;
    vs.id = fs.id = 2;
    vs.attrMask = (1u << AT_POS) | (1u << AT_TEX0);
    vs.text = "attribute vec4 a_pos; attribute vec2 a_tex0; uniform mat4 u_proj; varying vec2 v_t;"
              " void main() { gl_Position = u_proj * vec4(a_pos.xyz, 1.0); v_t = a_tex0; }";
    fs.fragment = true;
    fs.texMask = 1;
    fs.text = "precision mediump float; uniform sampler2D u_tex0; varying vec2 v_t; void main() { gl_FragColor = texture2D(u_tex0, v_t); }";
    CmdBuf setup;
    setup.frame = 1;
    setup.targets.push_back(SetTargetCmd{640, 456});
    Push(setup, CMD_SET_TARGET);
    setup.shaders = {&vs, &fs};
    Push(setup, CMD_SHADER, 0);
    Push(setup, CMD_SHADER, 1);
    Present(setup, true, nullptr, false);
    backend::Execute(setup);
    return true;
}

struct Shot {
    std::vector<uint8_t> px; // top row first, RGBA
    int w = 0, h = 0;
    unsigned scenes = 0;
};
Shot PresentFrame(uint64_t frame, const EfbRect* box) {
    {
        CmdBuf a;
        a.frame = frame;
        Present(a, true, nullptr, false); // (the EFB grey)
        backend::Execute(a);
    }
    const float white[4] = {1, 1, 1, 1};
    backend::DebugDrawEfbTriangle(white);
    CmdBuf b;
    b.frame = frame + 1;
    Present(b, false, box, true);
    backend::RequestCapture(true);
    backend::Execute(b);
    backend::RequestCapture(false);
    Shot s;
    s.px = backend::LastDisplayCopy(s.w, s.h);
    s.scenes = backend::DebugLastFrameScenes();
    return s;
}
inline int Lum(const uint8_t* p) { return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000; }
} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "lod") {
        if (!Setup()) return 2;
        const int rung = std::atoi(argv[2]), wantPlayer = std::atoi(argv[3]), wantOther = std::atoi(argv[4]);
        const int other = rung == 3 ? 4 : 3;
        lodSets.clear(); // (kept across both rungs: a bias already set by the first one is not set again)
        for (int r : {other, rung}) { // (through another rung first: the first picture is the EFB's own size)
            backend::ForceQualityRung(r);
            Frame({40}, r == other ? std::vector<std::pair<uint32_t, bool>>{{40, true}, {41, false}} : std::vector<std::pair<uint32_t, bool>>{});
            Frame({40}, {});
        }
        int now = 0;
        for (int v : lodSets) now = v;
        Check(now == wantPlayer, "rung " + std::to_string(rung) + ": the player's texture has LOD bias " + std::to_string(now) + "/8 (expected " + std::to_string(wantPlayer) + ")");
        lodSets.clear();
        Frame({41}, {});
        Frame({41}, {});
        now = 0;
        for (int v : lodSets) now = v;
        Check(now == wantOther, "the other texture has " + std::to_string(now) + "/8 (expected " + std::to_string(wantOther) + ")");
    } else if (mode == "aniso") {
        if (!Setup()) return 2;
        std::string want = argv[2];
        backend::ForceQualityRung(3);
        hookCalls.clear();
        Frame({41}, {{40, true}, {41, false}}); // (the other texture alone is drawn: it must make no call)
        Check(hookCalls.empty(), "the rest's texture: no call of the anisotropic switch (" + std::to_string(hookCalls.size()) + ")");
        hookCalls.clear();
        Frame({40}, {});
        std::string seq;
        for (unsigned h : hookCalls) seq += (seq.empty() ? "" : "-") + std::to_string(h);
        if (seq.empty()) seq = "-";
        Check(seq == want, "the player's texture: switch calls " + seq + " (expected " + want + ")");
        hookCalls.clear();
        Frame({40}, {});
        Check(hookCalls.empty(), "and none on later binds (" + std::to_string(hookCalls.size()) + ")");
    } else if (mode == "present") {
        if (!host::CreateContext()) { std::printf("no GL context\n"); return 2; }
        if (!backend::Init(0, 0)) { std::printf("back end Init failed\n"); return 2; }
        {
            CmdBuf b;
            b.frame = 1;
            b.targets.push_back(SetTargetCmd{640, 456});
            Push(b, CMD_SET_TARGET);
            Present(b, true, nullptr, false);
            backend::Execute(b);
        }
        backend::ForceQualityRung(6);
        const char* what = argc > 2 ? argv[2] : "off";
        const EfbRect box{200, 100, 200, 200};
        const int bx0 = 200 * 960 / 640 - 2, bx1 = 400 * 960 / 640 + 2, by0 = 100 * 544 / 456 - 2, by1 = (300 * 544 + 455) / 456 + 2;
        const Shot n = PresentFrame(100, nullptr);
        uint64_t p0, r0, x0;
        backend::PlayerPassCounters(p0, r0, x0);
        const Shot a = PresentFrame(200, &box);
        uint64_t p1, r1, x1;
        backend::PlayerPassCounters(p1, r1, x1);
        if (n.w != 960 || n.h != 544 || a.w != 960 || a.h != 544) { std::printf("  [FAIL] captures are %dx%d / %dx%d, expected 960x544 (GXV_CAP_PRESENT=960x544)\n", n.w, n.h, a.w, a.h); return 1; }
        int rw = 0, rh = 0, rs = 0;
        backend::QualityRungSize(rw, rh, rs);
        Check(rw == 960 && rh == 544 && rs == 1, "the picture is rendered at 960x544 and shown 1:1 (" + std::to_string(rw) + "x" + std::to_string(rh) + ")");
        const int W = n.w, H = n.h;
        uint64_t outside = 0, inside = 0;
        double stepN = 0, stepA = 0;
        int rows = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const bool diff = std::memcmp(&n.px[(static_cast<size_t>(y) * W + x) * 4], &a.px[(static_cast<size_t>(y) * W + x) * 4], 3) != 0;
                if (x >= bx0 && x < bx1 && y >= by0 && y < by1) inside += diff; else outside += diff;
            }
        for (int y = by0 + 5; y < by1 - 5; ++y) {
            int mn = 0, ma = 0;
            for (int x = bx0 + 5; x < bx1 - 6; ++x) {
                mn = std::max(mn, std::abs(Lum(&n.px[(static_cast<size_t>(y) * W + x + 1) * 4]) - Lum(&n.px[(static_cast<size_t>(y) * W + x) * 4])));
                ma = std::max(ma, std::abs(Lum(&a.px[(static_cast<size_t>(y) * W + x + 1) * 4]) - Lum(&a.px[(static_cast<size_t>(y) * W + x) * 4])));
            }
            if (mn < 20) continue;
            ++rows;
            stepN += mn;
            stepA += ma;
        }
        char line[400];
        if (std::strcmp(what, "off") == 0) {
            std::snprintf(line, sizeof line, "switches off: a player rectangle changes %llu pixels (expected 0) and the pass is not counted (%llu presents)", static_cast<unsigned long long>(inside + outside),
                          static_cast<unsigned long long>(p1 - p0));
            Check(inside + outside == 0 && p1 == p0, line);
        } else {
            std::snprintf(line, sizeof line, "outside the rectangle: %llu pixels differ (expected 0)", static_cast<unsigned long long>(outside));
            Check(outside == 0, line);
            std::snprintf(line, sizeof line, "inside the rectangle: %llu pixels differ (expected > 200); steepest step per row %.1f -> %.1f over %d rows", static_cast<unsigned long long>(inside),
                          rows ? stepN / rows : 0.0, rows ? stepA / rows : 0.0, rows);
            Check(inside > 200 && rows > 100, line);
            if (std::strcmp(what, "leaveout") == 0 || std::strcmp(what, "sharpen") == 0)
                Check(stepA > stepN * 1.1, "the edge is no longer blended there: the steepest step is higher (" + std::to_string(stepA / std::max(rows, 1)) + " against " + std::to_string(stepN / std::max(rows, 1)) + ")");
            std::snprintf(line, sizeof line, "counters: %llu present with the pass, %llu rectangle, %llu screen pixels (expected 1, 1, up to %d)", static_cast<unsigned long long>(p1 - p0),
                          static_cast<unsigned long long>(r1 - r0), static_cast<unsigned long long>(x1 - x0), (bx1 - bx0) * (by1 - by0));
            Check(p1 - p0 == 1 && r1 - r0 == 1 && x1 - x0 > 1000 && x1 - x0 <= static_cast<uint64_t>((bx1 - bx0) * (by1 - by0)), line);
            std::snprintf(line, sizeof line, "scenes of the frame: %u with the rectangle, %u without (no scene added)", a.scenes, n.scenes);
            Check(a.scenes == n.scenes && a.scenes > 0, line);
        }
    } else if (mode == "none") {
        // the default environment: the player's textures get no bias and no switch call, whatever the rung
        if (!Setup()) return 2;
        for (int r : {3, 2, 0, 6}) {
            backend::ForceQualityRung(r);
            Frame({40}, r == 3 ? std::vector<std::pair<uint32_t, bool>>{{40, true}} : std::vector<std::pair<uint32_t, bool>>{});
            Frame({40}, {});
        }
        Check(lodSets.empty() && hookCalls.empty(), "no LOD bias and no anisotropic switch call for a player texture at any rung (" + std::to_string(lodSets.size()) + ", " + std::to_string(hookCalls.size()) + ")");
    } else {
        std::printf("usage: vgx_mario24_gl_test lod|aniso|present|none ...\n");
        return 2;
    }
    std::printf("%s (%d failed)\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed", g_fail);
    return g_fail ? 1 : 0;
}
