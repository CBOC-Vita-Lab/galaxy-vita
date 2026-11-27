// sharp24 on the real GLES back end and Mesa: the texture LOD bias per rung (GXV_RUNG_LODBIAS, GXV_RUNG_LODBIAS_EXTRA, _EXTRA_AF) and the size gate
// of anisotropic filtering (GXV_ANISO_MINSIZE, GXV_ANISO_CENSUS). Calls of glTexParameteri are observed (--wrap), the anisotropic switch is a
// recording hook. The scenario is chosen by argv[1]; tools/test_vgx_sharp24_gl.sh runs them all with the environment each needs.
//   lod <rung> <expected eighths>        draw one mipmapped texture at the rung (GXV_WIDE_RUNGS: 0 608x348, 2 720x404, 6 960x544): the
//                                        GL_TEXTURE_LOD_BIAS it is given (0 = never set)
//   lodaf <expected off> <expected on>   at rung 6: the bias with anisotropic filtering off, then on (EXTRA vs EXTRA_AF)
//   gate <minsize>                       anisotropic filtering on, a 64- and a 256-texel trilinear texture: the hook is turned off and on around
//                                        the small one's filter only when minsize > 64; nothing else is called; the census line is logged
#include "vgx_cmd.h"

#include <GLES2/gl2.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
bool IsCharaTexture(uint32_t) { return false; }
bool VtxPackOn() { return false; }               // (the packed-vertex counters live in vgx_dlcache.cpp, not linked here)
void VtxPackCounters(uint64_t*) {}
void VtxPackDrawCounters(uint64_t*) {}
}
namespace {
std::vector<int> lodSets;
unsigned minFilterSets = 0;
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
    if (pname == GL_TEXTURE_MIN_FILTER) ++minFilterSets;
    __real_glTexParameteri(target, pname, param);
}

namespace {
using namespace vgx;
void Push(CmdBuf& b, CmdType type, uint32_t arg = 0) { b.cmds.push_back(Cmd{type, arg}); }
void Present(CmdBuf& b, bool clear = false) {
    CopyDispCmd d;
    d.src = EfbRect{0, 0, 640, 456};
    d.clear.enable = clear;
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
void Upload(CmdBuf& b, uint32_t id, unsigned side) {
    TexUpload up;
    up.texId = id;
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
// one frame drawing the listed textures (uploaded first when `uploads`)
void Frame(const std::vector<std::pair<uint32_t, unsigned>>& texs, bool uploads) {
    CmdBuf b;
    b.frame = g_frame++;
    b.uni.assign(16, 0);
    for (unsigned i = 0; i < 4; ++i) b.uni[i * 5] = 1;
    if (uploads) for (auto& t : texs) Upload(b, t.first, t.second);
    for (auto& t : texs) AddTextured(b, t.first, FILT_LIN_MIP_LIN);
    Present(b);
    backend::Execute(b);
}
bool Setup(int fw, int fh) {
    if (!host::CreateContext()) { std::printf("no GL context\n"); return false; }
    if (!backend::Init(fw, fh)) { std::printf("back end Init failed\n"); return false; }
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
    Present(setup, true);
    backend::Execute(setup);
    return true;
}
} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "lod" || mode == "lodaf") {
        if (!Setup(0, 0)) return 2;
        const std::vector<std::pair<uint32_t, unsigned>> t = {{30, 64}};
        if (mode == "lod") {
            const int rung = std::atoi(argv[2]), want = std::atoi(argv[3]);
            const int other = rung == 3 ? 4 : 3;
            for (int r : {other, rung}) { // (through another rung first: the first picture is the EFB's own size)
                backend::ForceQualityRung(r);
                Frame(t, r == other);
                Frame(t, false);
            }
            // the last frames' bias: the last LOD-bias call is the bias the texture has now (a rung whose bias equals the previous one makes no call)
            int now = 0;
            for (int v : lodSets) now = v;
            Check(now == want, "rung " + std::to_string(rung) + ": the texture's LOD bias is " + std::to_string(now) + "/8 level (expected " + std::to_string(want) + ")");
        } else {
            const int wantOff = std::atoi(argv[2]), wantOn = std::atoi(argv[3]);
            backend::ForceQualityRung(5);
            Frame(t, true);
            Frame(t, false);
            backend::ForceQualityRung(6);
            Frame(t, false);
            Frame(t, false);
            int now = 0;
            for (int v : lodSets) now = v;
            Check(now == wantOff, "rung 6, anisotropic filtering off: bias " + std::to_string(now) + " (expected " + std::to_string(wantOff) + ")");
            lodSets.clear();
            backend::DebugSetAnisotropic(true, 2);
            Frame(t, false);
            Frame(t, false);
            now = lodSets.empty() ? wantOff : lodSets.back();
            Check(now == wantOn, "rung 6, anisotropic filtering on: bias " + std::to_string(now) + " (expected " + std::to_string(wantOn) + ")");
            lodSets.clear();
            backend::DebugSetAnisotropic(false, 2);
            Frame(t, false);
            Frame(t, false);
            now = lodSets.empty() ? wantOn : lodSets.back();
            Check(now == wantOff, "off again: bias " + std::to_string(now) + " (expected " + std::to_string(wantOff) + ")");
        }
    } else if (mode == "none") {
        // the default environment: no LOD bias is ever set
        if (!Setup(0, 0)) return 2;
        const std::vector<std::pair<uint32_t, unsigned>> t = {{30, 64}};
        for (int r : {3, 2, 0, 6}) { backend::ForceQualityRung(r); Frame(t, r == 3); Frame(t, false); }
        Check(lodSets.empty(), "no GL_TEXTURE_LOD_BIAS call at any rung (" + std::to_string(lodSets.size()) + ")");
    } else if (mode == "gate") {
        const long minSize = std::atol(argv[2]);
        if (!Setup(64, 64)) return 2;
        const std::vector<std::pair<uint32_t, unsigned>> t = {{30, 64}, {31, 256}};
        Frame(t, true);
        Frame(t, false);
        hookCalls.clear();
        minFilterSets = 0;
        backend::DebugSetAnisotropic(true, 2);
        Frame(t, false); // (the switch goes on at the start of this frame: the hook once, then both trilinear textures are set again)
        std::string seq;
        for (unsigned h : hookCalls) seq += std::to_string(h) + " ";
        if (minSize > 64) {
            // frame start: hook(2); the small texture: hook(0), set, hook(2); the big one: set
            Check(hookCalls == std::vector<unsigned>({2u, 0u, 2u}), "small texture gated: hook calls " + seq + "(expected 2 0 2)");
        } else {
            Check(hookCalls == std::vector<unsigned>({2u}), "no gate: hook calls " + seq + "(expected 2)");
        }
        Check(minFilterSets == 2, "both trilinear textures set once (" + std::to_string(minFilterSets) + ")");
        hookCalls.clear();
        Frame(t, false);
        Check(hookCalls.empty() && minFilterSets == 2, "the next frame: nothing set, no hook call");
        backend::DebugSetAnisotropic(false, 2);
        hookCalls.clear();
        Frame(t, false);
        Check(hookCalls == std::vector<unsigned>({0u}), "off: one hook call with 0, no gating calls");
    } else {
        std::printf("usage: vgx_sharp24_gl_test lod <rung> <eighths> | lodaf <off> <on> | none | gate <minsize>\n");
        return 2;
    }
    Check(glGetError() == GL_NO_ERROR || mode.rfind("lod", 0) == 0, "no GL errors (the LOD bias enum is desktop GL: ES2 reports it)");
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "all checks passed", g_fail);
    return g_fail ? 1 : 0;
}
