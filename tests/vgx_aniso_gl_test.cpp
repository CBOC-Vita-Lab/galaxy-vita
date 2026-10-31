// Anisotropic filtering as a run-time step of the quality ladder (GXV_AUTOQ_ANISO) on the real GLES back end and Mesa: what the back end
// does between frames when the switch changes (backend::DebugSetAnisotropic, the state the controller sets), with a recording hook in
// place of vitaGL's vglSetAnisotropicFilter. vitaGL applies the filter when a texture's minification filter is SET, not at a draw, so
// the back end must call the switch between frames and set the minification filter again, once, on the first bind after a change, of
// every texture that is trilinear (and of no other). Checked frame by frame:
//   - the hook is called once per change, with the filter value (2 or 3) or 0, at the start of the frame, never otherwise;
//   - glTexParameteri(GL_TEXTURE_MIN_FILTER) is called as before for new textures and filter changes, once more for each trilinear
//     texture after a change of the switch (a plain linear texture: never), and not at all in a frame without a change;
//   - the cheaper background (DebugSetQualitySteps) and the switch together: the demoted texture is not trilinear and is not set again
//     for the switch; when it is trilinear again it is set once, with the switch's current state;
//   - the pictures are the ones of the trilinear filter (the hook changes nothing in Mesa) in every frame.
// Wrappers observe calls and always forward them. Usage: vgx_aniso_gl_test
#include "vgx_cmd.h"

#include <GLES2/gl2.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
bool IsCharaTexture(uint32_t id) { return id == 31; } // (the character's texture is never made cheaper by the background step)
}
namespace {
unsigned minFilterSets = 0;
std::vector<unsigned> hookCalls;
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
void Hook(unsigned filter) { hookCalls.push_back(filter); }
}
extern "C" void __real_glTexParameteri(GLenum, GLenum, GLint);
extern "C" void __wrap_glTexParameteri(GLenum target, GLenum pname, GLint param) {
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
constexpr unsigned kLevelValue[7] = {20, 60, 100, 140, 180, 220, 250};
void AddTextured(CmdBuf& b, bool right, uint32_t tex, uint8_t minFilter) {
    const float x0 = right ? 0.0f : -1.0f, x1 = x0 + 1.0f;
    const float v[] = {x0, -1, 0, 0, 0, 0,  x1, -1, 0, 0, 0.75f, 0,  x0, 0, 0, 0, 0, 0.75f,  x1, 0, 0, 0, 0.75f, 0.75f};
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
unsigned Probe(const std::vector<uint8_t>& px, int w, int x) {
    unsigned best = 0;
    for (int y : {16, 48}) best = std::max<unsigned>(best, px[(static_cast<size_t>(y) * w + x) * 4]);
    return best;
}
bool Near(double got, double want, double tol) { return got >= want - tol && got <= want + tol; }

struct Step {
    int aniso;       // -1: leave as it is; 0 off; 2 / 3 on with that filter
    bool bg;         // the cheaper background
    int hooks;       // expected calls of the hook this frame: -1 none, else the filter value of the one call
    unsigned sets;   // expected glTexParameteri(MIN_FILTER) calls
    const char* what;
};
} // namespace

int main() {
    if (!host::CreateContext()) { std::printf("no GL context\n"); return 2; }
    if (!backend::Init(64, 64)) { std::printf("back end Init failed\n"); return 2; }
    backend::SetAnisotropicHook(&Hook);
    ShaderSource vs, fs;
    vs.id = fs.id = 2;
    vs.attrMask = (1u << AT_POS) | (1u << AT_TEX0);
    vs.text = "attribute vec4 a_pos; attribute vec2 a_tex0; uniform mat4 u_proj; varying vec2 v_t;"
              " void main() { gl_Position = u_proj * vec4(a_pos.xyz, 1.0); v_t = a_tex0; }";
    fs.fragment = true;
    fs.texMask = 1;
    fs.text = "precision mediump float; uniform sampler2D u_tex0; varying vec2 v_t; void main() { gl_FragColor = texture2D(u_tex0, v_t); }";
    {
        CmdBuf setup;
        setup.frame = 1;
        setup.targets.push_back(SetTargetCmd{640, 456});
        Push(setup, CMD_SET_TARGET);
        setup.shaders = {&vs, &fs};
        Push(setup, CMD_SHADER, 0);
        Push(setup, CMD_SHADER, 1);
        Present(setup, true);
        backend::Execute(setup);
    }
    // texture 30: trilinear (LIN_MIP_LIN), ordinary; texture 31: a character's trilinear one; texture 32: plain linear
    const Step steps[] = {
        {0, false, -1, 3, "frame 2: the textures are new: their filters are set"},
        {-1, false, -1, 0, "no change of the switch: nothing is set again"},
        {2, false, 2, 2, "on (filter 2): the hook is called once; the two trilinear textures are set again, the linear one is not"},
        {-1, false, -1, 0, "still on: nothing"},
        {0, false, 0, 2, "off: the hook gets 0; the trilinear textures are set again"},
        {3, false, 3, 2, "on with filter 3"},
        {-1, true, -1, 1, "the cheaper background: the ordinary trilinear texture is demoted (set once); the character's is not"},
        {-1, true, -1, 0, "... and stays so without being set again"},
        {-1, false, -1, 1, "the background step off: the texture is trilinear again, set once, with the switch on"},
        {0, true, 0, 2, "off with the background step on again: the hook gets 0; the ordinary texture is demoted (set once), the character's trilinear one is set for the switch"},
        {-1, false, -1, 1, "the demoted texture is trilinear again: set once (the switch is off)"},
        {2, false, 2, 2, "on again"},
        {-1, false, -1, 0, "still on: nothing"},
    };
    unsigned frame = 2;
    for (const Step& s : steps) {
        hookCalls.clear();
        minFilterSets = 0;
        if (s.aniso >= 0) backend::DebugSetAnisotropic(s.aniso > 0, static_cast<unsigned>(s.aniso));
        backend::DebugSetQualitySteps(false, s.bg);
        CmdBuf b;
        b.frame = frame;
        b.uni.assign(16, 0);
        for (unsigned i = 0; i < 4; ++i) b.uni[i * 5] = 1;
        if (frame == 2)
            for (uint32_t id : {30u, 31u, 32u}) {
                TexUpload up;
                up.texId = id;
                up.width = up.height = 64;
                up.authored = 7;
                for (unsigned l = 0, size = 64; l < 7; ++l, size /= 2) {
                    std::vector<uint8_t> level(static_cast<size_t>(size) * size * 4, static_cast<uint8_t>(kLevelValue[l]));
                    for (size_t a = 3; a < level.size(); a += 4) level[a] = 255;
                    up.levels.push_back(std::move(level));
                }
                b.uploads.push_back(up);
                Push(b, CMD_TEX_UPLOAD, static_cast<uint32_t>(b.uploads.size() - 1));
            }
        AddTextured(b, false, 30, FILT_LIN_MIP_LIN);
        AddTextured(b, true, 31, FILT_LIN_MIP_LIN);
        AddTextured(b, true, 32, FILT_LINEAR);
        Present(b);
        backend::RequestCapture(true);
        backend::Execute(b);
        backend::RequestCapture(false);
        int w = 0, h = 0;
        const auto& px = backend::LastDisplayCopy(w, h);
        char line[300];
        std::snprintf(line, sizeof line, "%s: hook calls %zu%s, MIN_FILTER sets %u (expected %d, %u)", s.what, hookCalls.size(),
                      hookCalls.size() == 1 ? (std::string(" (") + std::to_string(hookCalls[0]) + ")").c_str() : "", minFilterSets, s.hooks < 0 ? 0 : 1, s.sets);
        const bool hookOk = s.hooks < 0 ? hookCalls.empty() : (hookCalls.size() == 1 && hookCalls[0] == static_cast<unsigned>(s.hooks));
        // frame 2 sets the three textures' filters (3); texture 32 is plain linear and is never set again
        Check(hookOk && minFilterSets == s.sets, line);
        // the picture: the ordinary texture (left) is trilinear, or the nearest level with the background step; the character's (right) always trilinear
        const double trilinear = kLevelValue[0] + (kLevelValue[1] - kLevelValue[0]) * 0.585;
        const unsigned left = Probe(px, w, 16);
        if (s.bg) Check(Near(left, kLevelValue[1], 3), "  picture: the ordinary texture takes the nearest mip level");
        else Check(Near(left, trilinear, 6), "  picture: the ordinary texture is trilinear");
        Check(glGetError() == GL_NO_ERROR, "  no GL errors");
        ++frame;
    }
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "all checks passed", g_fail);
    return g_fail ? 1 : 0;
}
