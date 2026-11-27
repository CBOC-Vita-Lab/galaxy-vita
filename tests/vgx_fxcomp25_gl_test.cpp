// compositor25: pictures of the effects compositor with the automatic choice (GXV_FXLOWRES_AUTO) and its quality levels, real back end on Mesa, 960x544.
// One process renders every variant: the stage's switches are set between them through the stage-knob table (as a stage preset does), a stage load is
// simulated (DebugFxAutoReset) and a few warm-up pictures let the model decide; the last picture of each variant is read back to <prefix>-<name>.rgba.
// tools/fxcomp25_compare.py compares them with the unrouted reference and checks the numbers. The scenes:
//   ring    a far wall, a near pillar and a mid-distance box in front of four far additive glow sheets (4.2 screens of fill): depth edges everywhere
//   thin    the same sheets and 80 thin bright additive lines (lightning) of a key whose fill would pass a low threshold
//   energy  glow sheets only (a smooth glow on a dark wall): the sum of the light, routed and not
//   static  the ring scene with the glow sheets drawn from a static buffer (the fill is measured from the kept copy of the buffer)
//   alpha   sheets that write alpha as a constant (as the game's glow sheets do), with nothing after them reading it; alpharead: a copy that reads alpha follows
#include "vgx_cmd.h"
#include "port/stage_knob.h"

#include <GLES2/gl2.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace vgx { bool LogEnabled() { return false; } }
std::vector<std::string> g_logLines;
extern "C" void port_log_summary(const char* fmt, ...) {
    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (!std::strncmp(line, "fxauto:", 7)) { std::printf("%s\n", line); g_logLines.push_back(line); }
}
namespace vgx {
bool VtxPackOn() { return false; }
void VtxPackCounters(uint64_t out[7]) { for (int i = 0; i < 7; ++i) out[i] = 0; }
void VtxPackDrawCounters(uint64_t out[4]) { for (int i = 0; i < 4; ++i) out[i] = 0; }
bool PrepassPair(uint16_t, uint16_t&, uint16_t&) { return false; }
bool PrepassAny() { return false; }
void PrepassCounters(uint64_t out[9]) { for (int i = 0; i < 9; ++i) out[i] = 0; }
} // namespace vgx

namespace {
using namespace vgx;
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void Push(CmdBuf& b, CmdType type, uint32_t arg = 0) { b.cmds.push_back(Cmd{type, arg}); }
void Present(CmdBuf& b) {
    CopyDispCmd d;
    d.src = EfbRect{0, 0, 640, 456};
    d.clear.enable = true;
    b.copyDisp.push_back(d);
    Push(b, CMD_COPY_DISP, static_cast<uint32_t>(b.copyDisp.size() - 1));
    Push(b, CMD_PRESENT);
}
void Geometry(CmdBuf& b) {
    const float quad[16] = {-1, -1, -1, 0, 1, -1, -1, 0, -1, 1, -1, 0, 1, 1, -1, 0};
    b.vtx.resize(sizeof quad);
    std::memcpy(b.vtx.data(), quad, sizeof quad);
    b.idx = {0, 1, 2, 2, 1, 3};
    const float n = 1.0f, f = 20000.0f;
    const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -(f + n) / (f - n), -1, 0, 0, -2.0f * f * n / (f - n), 0};
    b.uni.assign(proj, proj + 16);
}
enum Fs { FS_FLAT = 1, FS_SOFT = 2, FS_LINE = 3, FS_SMOKE = 4, FS_TEXG = 5 };
// A camera-facing card: centre (cx, cy), half-size hx x hy (NDC), `dist` units from the camera plane. kind 0 opaque (writes depth), 2 additive.
void Card(CmdBuf& b, int kind, int fs, float cx, float cy, float hx, float hy, float dist, float r, float g, float bl, float a, bool isStatic = false, bool alphaWrite = false, uint32_t tex = 0) {
    DrawRec d;
    d.vsId = 1;
    d.fsId = static_cast<uint16_t>(fs);
    d.vtxCount = 4;
    d.idxCount = 6;
    if (tex) { d.texMask = 1; d.tex[0].texId = tex; }
    d.vbuf = isStatic ? 1u : 0u;
    d.ublock[UB_PROJ] = 0;
    d.ublock[UB_MV] = static_cast<uint32_t>(b.uni.size());
    b.uni.insert(b.uni.end(), {hx * dist, 0, 0, cx * dist, 0, hy * dist, 0, cy * dist, 0, 0, dist, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
    d.ublock[UB_TEV] = static_cast<uint32_t>(b.uni.size());
    b.uni.insert(b.uni.end(), {r, g, bl, a});
    d.rs.blend = kind != 0;
    d.rs.blendSrc = kind == 0 ? BF_ONE : BF_SRC_ALPHA;
    d.rs.blendDst = kind == 0 ? BF_ZERO : (kind == 1 ? BF_INV_SRC_ALPHA : BF_ONE);
    d.rs.depthTest = 1;
    d.rs.depthFunc = CMP_LEQUAL;
    d.rs.depthWrite = kind == 0;
    d.rs.colorWrite = 1;
    d.rs.alphaWrite = alphaWrite ? 1 : 0;
    d.rs.dstAlphaZero = alphaWrite ? 1 : 0;
    d.rs.scX = d.rs.scY = 0;
    d.rs.scW = 640;
    d.rs.scH = 456;
    d.rs.vpX = d.rs.vpY = 0;
    d.rs.vpW = 640;
    d.rs.vpH = 456;
    b.draws.push_back(d);
    Push(b, CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1));
}
void CopyRead(CmdBuf& b, uint32_t id) { // (a copy of the EFB that reads its alpha)
    CopyTexCmd c;
    c.texId = id;
    c.src = EfbRect{0, 0, 640, 456};
    c.dstW = 640;
    c.dstH = 456;
    for (unsigned i = 0; i < 4; ++i) c.mat[i][i] = 1;
    b.copyTex.push_back(c);
    Push(b, CMD_COPY_TEX, static_cast<uint32_t>(b.copyTex.size() - 1));
}
uint32_t g_seed = 4242;
float Rand() { g_seed = g_seed * 1664525u + 1013904223u; return static_cast<float>((g_seed >> 8) & 0xFFFF) / 65535.0f; }
// the sheets: a smooth additive glow far behind the objects
void Sheets(CmdBuf& b, bool isStatic, bool alphaWrite, float jitter) {
    // five sheets, about 5 screens of fill in all (one key): a smooth glow, soft at the edges
    const float s = 1.0f + jitter * (Rand() - 0.5f);
    Card(b, 2, FS_SOFT, -0.2f, 0.0f, 1.1f * s, 1.1f, 8000, 1.0f, 0.55f, 0.15f, 0.30f, isStatic, alphaWrite);
    Card(b, 2, FS_SOFT, 0.2f, 0.1f, 1.1f, 1.1f * s, 8000, 1.0f, 0.55f, 0.15f, 0.30f, isStatic, alphaWrite);
    Card(b, 2, FS_SOFT, 0.0f, -0.2f, 1.2f * s, 1.0f, 8000, 0.9f, 0.5f, 0.2f, 0.30f, isStatic, alphaWrite);
    Card(b, 2, FS_SOFT, 0.0f, 0.3f, 1.2f, 1.0f * s, 8000, 0.9f, 0.5f, 0.2f, 0.30f, isStatic, alphaWrite);
    Card(b, 2, FS_SOFT, 0.1f, 0.0f, 1.3f * s, 1.3f, 8000, 0.8f, 0.5f, 0.3f, 0.25f, isStatic, alphaWrite);
}
void Scene(CmdBuf& b, const std::string& kind, float jitter) {
    Geometry(b);
    g_seed = 4242;
    Card(b, 0, FS_FLAT, 0.0f, 0.0f, 1.0f, 1.0f, 9000, 0.10f, 0.14f, 0.30f, 1);                  // the far wall
    if (kind == "ring" || kind == "static" || kind == "alpha" || kind == "alpharead") {
        Card(b, 0, FS_FLAT, -0.25f, -0.05f, 0.07f, 0.85f, 1500, 0.45f, 0.30f, 0.18f, 1);        // the near pillar (x -0.32 .. -0.18)
        Card(b, 0, FS_FLAT, 0.45f, 0.10f, 0.15f, 0.30f, 3500, 0.30f, 0.34f, 0.28f, 1);          // the box (x 0.30 .. 0.60)
    }
    if (kind == "ring" || kind == "energy" || kind == "thin") Sheets(b, false, false, jitter);
    if (kind == "static") Sheets(b, true, false, jitter);
    if (kind == "alpha" || kind == "alpharead") Sheets(b, false, true, jitter);
    if (kind == "thin") {
        for (int i = 0; i < 80; ++i) // lightning: 1.5 px wide, far
            Card(b, 2, FS_LINE, -0.9f + 1.8f * Rand(), -0.5f + 1.0f * Rand(), 0.0016f, 0.5f, 7000, 1.0f, 1.0f, 0.9f, 1.0f);
    }
    if (kind == "texsoft" || kind == "texnoise") {  // five sheets of a textured glow: the texture is smooth (soft: quarter size) or noise (half)
        const uint32_t t = kind == "texsoft" ? 40u : 41u;
        for (int i = 0; i < 5; ++i) Card(b, 2, FS_TEXG, -0.2f + 0.1f * i, 0.0f, 1.1f, 1.1f, 8000, 1.0f, 0.6f, 0.2f, 0.3f, false, false, t);
    }
    if (kind == "defer" || kind == "deferover" || kind == "deferb" || kind == "defercap") {
        // four glow layers of one shader (about a screen each) separated by opaque depth writers: in place four runs that do not pay, deferred one that does
        const float s1 = 1.0f + jitter * (Rand() - 0.5f);
        auto sheet = [&](float cx, float cy, float a) { Card(b, 2, FS_SOFT, cx, cy, 1.2f * s1, 1.2f, 8000, 1.0f, 0.55f, 0.15f, a); };
        Card(b, 0, FS_FLAT, -0.25f, -0.05f, 0.07f, 0.85f, 1500, 0.45f, 0.30f, 0.18f, 1);         // the near pillar
        sheet(-0.1f, 0.0f, 0.22f);
        if (kind == "deferover") Card(b, 1, FS_SMOKE, 0.0f, 0.0f, 0.35f, 0.35f, 6000, 0.1f, 0.1f, 0.12f, 0.6f); // an "over" blend after the first layer, overlapping it
        Card(b, 0, FS_FLAT, 0.45f, 0.10f, 0.15f, 0.30f, 3500, 0.30f, 0.34f, 0.28f, 1);           // the box
        if (kind == "deferb") Card(b, 0, FS_FLAT, 0.0f, -0.1f, 0.25f, 0.25f, 8500, 0.50f, 0.10f, 0.10f, 1); // an opaque draw BEHIND the layers (9000 > 8500 > 8000), after the first
        sheet(0.1f, 0.1f, 0.22f);
        if (kind == "defercap") CopyRead(b, 21);  // (a capture between the layers: a stretch ends here unless GXV_FXLOWRES_DEFER=2)
        Card(b, 0, FS_FLAT, 0.0f, 0.35f, 0.2f, 0.1f, 2500, 0.30f, 0.30f, 0.40f, 1);              // a third depth writer
        sheet(0.0f, -0.1f, 0.22f);
        Card(b, 0, FS_FLAT, 0.7f, -0.4f, 0.1f, 0.2f, 3000, 0.30f, 0.20f, 0.20f, 1);              // a fourth
        sheet(0.05f, 0.05f, 0.22f);
    }
    if (kind == "alpharead") CopyRead(b, 20);
    Present(b);
}
void Save(const std::string& path, const std::vector<uint8_t>& pixels, int w, int h) {
    FILE* f = std::fopen(path.c_str(), "wb");
    Require(f != nullptr, "open the picture file");
    const uint32_t dims[] = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    Require(std::fwrite(dims, sizeof dims, 1, f) == 1 && std::fwrite(pixels.data(), pixels.size(), 1, f) == 1, "write picture");
    std::fclose(f);
}
void Knobs(std::initializer_list<std::pair<const char*, float>> kv) {
    port::stageknob::ClearAll();
    for (const auto& p : kv) port::stageknob::Set(p.first, p.second);
    port::stageknob::Commit();
}
unsigned g_frame = 2;
} // namespace

int main(int argc, char** argv) {
    Require(argc >= 2, "output prefix argument");
    // "console" (the second argument): the environment is vita/vgx-game/entry.cpp's defaults for every GXV_FXLOWRES* name plus GXV_FXLOWRES_AUTO=1 alone, no stage knobs, the
    // decision window and the 1-in-4 observation at their defaults: what the console ran (round 3: it logged nothing and routed nothing)
    const bool console = argc >= 3 && std::strcmp(argv[2], "console") == 0;
    if (console) {
        setenv("GXV_FXLOWRES", "0", 0);
        setenv("GXV_FXLOWRES_CEILING", "0", 0);
        for (const char* name : {"GXV_FXLOWRES_STATIC", "GXV_FXLOWRES_K1", "GXV_FXLOWRES_K2", "GXV_FXLOWRES_K3", "GXV_FXLOWRES_K4"}) setenv(name, "0", 0);
        for (const char* name : {"GXV_FXLOWRES_UPS", "GXV_FXLOWRES_DIV", "GXV_FXLOWRES_DEFER"}) setenv(name, "0", 0);
        setenv("GXV_FXLOWRES_AUTO", "1", 1);
        unsetenv("GXV_FXLOWRES_AUTO_EVERY");
        unsetenv("GXV_FXLOWRES_EVERY");
    } else {
        setenv("GXV_FXLOWRES_AUTO", "1", 1); // (the environment arms the kept copies of static buffers; the stage knobs below choose per variant)
    }
    Require(host::CreateContext(), "Mesa EGL context");
    Require(backend::Init(960, 544), "real backend initialization");
    const std::string prefix = argv[1];

    ShaderSource vs, fsFlat, fsSoft, fsLine, fsSmoke;
    vs.id = 1;
    vs.attrMask = 1u << AT_POS;
    vs.nMv = 3;
    vs.text = "attribute vec4 a_pos; uniform mat4 u_proj; uniform vec4 u_mv[3]; varying vec2 v_uv; void main() { vec4 p = vec4(a_pos.xyz, 1.0); v_uv = a_pos.xy; "
              "gl_Position = u_proj * vec4(dot(u_mv[0], p), dot(u_mv[1], p), dot(u_mv[2], p), 1.0); }";
    fsFlat.id = FS_FLAT; fsFlat.fragment = true; fsFlat.nTev = 1; fsFlat.keyHash = 0x1234ABCDEF01ull;
    fsFlat.text = "precision mediump float; uniform vec4 u_tev[1]; varying vec2 v_uv; void main() { gl_FragColor = vec4(u_tev[0].rgb * (0.85 + 0.15 * v_uv.y), 1.0); }";
    fsSoft.id = FS_SOFT; fsSoft.fragment = true; fsSoft.nTev = 1; fsSoft.keyHash = 0x9A8B7C6D5E4Full;
    fsSoft.text = "precision mediump float; uniform vec4 u_tev[1]; varying vec2 v_uv; void main() { float f = clamp(1.0 - dot(v_uv, v_uv), 0.0, 1.0); "
                  "gl_FragColor = vec4(u_tev[0].rgb, u_tev[0].a * f * f); }";
    fsLine.id = FS_LINE; fsLine.fragment = true; fsLine.nTev = 1; fsLine.keyHash = 0x5566778899AAull;
    fsLine.text = "precision mediump float; uniform vec4 u_tev[1]; varying vec2 v_uv; void main() { gl_FragColor = vec4(u_tev[0].rgb * 0.9, u_tev[0].a); }";
    fsSmoke.id = FS_SMOKE; fsSmoke.fragment = true; fsSmoke.nTev = 1; fsSmoke.keyHash = 0x0102030405A5ull;
    fsSmoke.text = "precision mediump float; uniform vec4 u_tev[1]; varying vec2 v_uv; void main() { float f = clamp(1.0 - dot(v_uv, v_uv), 0.0, 1.0); "
                   "gl_FragColor = vec4(u_tev[0].rgb, u_tev[0].a * f); }";
    ShaderSource fsTexg;
    fsTexg.id = FS_TEXG; fsTexg.fragment = true; fsTexg.nTev = 1; fsTexg.texMask = 1; fsTexg.keyHash = 0x1122334455BBull;
    fsTexg.text = "precision mediump float; uniform sampler2D u_tex0; uniform vec4 u_tev[1]; varying vec2 v_uv; void main() { "
                  "vec4 t = texture2D(u_tex0, v_uv * 0.5 + 0.5); gl_FragColor = vec4(u_tev[0].rgb * t.r, u_tev[0].a * t.r); }";
    CmdBuf setup;
    setup.frame = 1;
    setup.targets.push_back(SetTargetCmd{640, 456});
    Push(setup, CMD_SET_TARGET);
    setup.shaders = {&vs, &fsFlat, &fsSoft, &fsLine, &fsSmoke, &fsTexg};
    Push(setup, CMD_SHADER, 0);
    Push(setup, CMD_SHADER, 1);
    Push(setup, CMD_SHADER, 2);
    Push(setup, CMD_SHADER, 3);
    Push(setup, CMD_SHADER, 4);
    Push(setup, CMD_SHADER, 5);
    for (const uint32_t id : {40u, 41u}) {  // textures: 40 a smooth blob, 41 noise
        TexUpload up;
        up.texId = id;
        up.width = up.height = 64;
        up.authored = 1;
        std::vector<uint8_t> level(64 * 64 * 4, 255);
        uint32_t seed = 77;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                const float dx = (x - 31.5f) / 32.0f, dy = (y - 31.5f) / 32.0f;
                float v = std::exp(-3.0f * (dx * dx + dy * dy));
                if (id == 41) { seed = seed * 1664525u + 1013904223u; v = static_cast<float>((seed >> 16) & 0xFF) / 255.0f; }
                uint8_t* q = &level[(static_cast<size_t>(y) * 64 + x) * 4];
                q[0] = q[1] = q[2] = static_cast<uint8_t>(v * 255.0f);
            }
        up.levels.push_back(std::move(level));
        setup.uploads.push_back(std::move(up));
        Push(setup, CMD_TEX_UPLOAD, static_cast<uint32_t>(setup.uploads.size() - 1));
    }
    {   // the static buffer of the "static" scene: the unit quad
        BufUpload u;
        u.bufId = 1;
        const float quad[16] = {-1, -1, -1, 0, 1, -1, -1, 0, -1, 1, -1, 0, 1, 1, -1, 0};
        u.vtx.resize(sizeof quad);
        std::memcpy(u.vtx.data(), quad, sizeof quad);
        u.idx = {0, 1, 2, 2, 1, 3};
        setup.bufUploads.push_back(u);
        Push(setup, CMD_BUF_UPLOAD, 0);
    }
    Present(setup);
    backend::Execute(setup);

    struct Variant { const char* name; const char* scene; std::initializer_list<std::pair<const char*, float>> knobs; unsigned warm; };
    const std::initializer_list<std::pair<const char*, float>> off = {{"GXV_FXLOWRES", 0}, {"GXV_FXLOWRES_AUTO", 0}};
    const Variant consoleVariants[] = {
        {"console-ring", "ring", {}, 300},
        {"console-texsoft", "texsoft", {}, 300},
    };
    const Variant variants[] = {
        // the unrouted reference of each scene, then the automatic choice at composite level 0 and 1
        {"ring-ref", "ring", off, 1},
        {"ring-l0", "ring", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}, {"GXV_FXLOWRES_UPS", 0}, {"GXV_FXLOWRES_DIV", 2}}, 14},
        {"ring-l1", "ring", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}, {"GXV_FXLOWRES_UPS", 1}, {"GXV_FXLOWRES_DIV", 2}}, 14},
        {"ring-auto", "ring", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}, {"GXV_FXLOWRES_UPS", 1}}, 14}, // (the size from the softness: soft glow of 5 screens: quarter)
        {"ring-q", "ring", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}, {"GXV_FXLOWRES_UPS", 1}, {"GXV_FXLOWRES_DIV", 4}}, 14},
        {"energy-ref", "energy", off, 1},
        {"energy-l0", "energy", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}}, 14},
        {"thin-ref", "thin", off, 1},
        {"thin-on", "thin", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}, {"GXV_FXLOWRES_AUTO_MILLI", 50}}, 14},
        {"thin-off", "thin", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}, {"GXV_FXLOWRES_AUTO_MILLI", 50}, {"GXV_FXLOWRES_THIN", 0}}, 14},
        {"static-ref", "static", off, 1},
        {"static-l0", "static", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}}, 14},
        {"alpha-ref", "alpha", off, 1},
        {"alpha-on", "alpha", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}}, 14},
        {"alpha-off", "alpha", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}, {"GXV_FXLOWRES_ALPHA", 0}}, 14},
        {"alpharead-on", "alpharead", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}}, 14},
        // round 2: the deferral. In place (DEFER=0) the layers form three runs; deferred (1) one. The same scene with an "over" blend after the first layer: that layer stays in place.
        {"defer-ref", "defer", off, 1},
        {"defer-inplace", "defer", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}, {"GXV_FXLOWRES_DIV", 2}}, 14},
        {"defer-on", "defer", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}, {"GXV_FXLOWRES_DIV", 2}, {"GXV_FXLOWRES_DEFER", 1}}, 14},
        {"deferover-ref", "deferover", off, 1},
        {"deferover-inplace", "deferover", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}, {"GXV_FXLOWRES_DIV", 2}}, 14},
        {"deferover-on", "deferover", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}, {"GXV_FXLOWRES_DIV", 2}, {"GXV_FXLOWRES_DEFER", 1}}, 14},
        {"deferover-forced", "deferover", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}, {"GXV_FXLOWRES_DIV", 2}, {"GXV_FXLOWRES_DEFER", 1}, {"GXV_FXLOWRES_DEFER_TOL", 1000}}, 14}, // (negative control: the overlap accepted)
        {"deferb-ref", "deferb", off, 1},
        {"deferb-on", "deferb", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}, {"GXV_FXLOWRES_DIV", 2}, {"GXV_FXLOWRES_DEFER", 1}}, 14},
        {"defercap-on", "defercap", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}, {"GXV_FXLOWRES_DIV", 2}, {"GXV_FXLOWRES_DEFER", 1}}, 14},
        {"defercap-cross", "defercap", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}, {"GXV_FXLOWRES_DIV", 2}, {"GXV_FXLOWRES_DEFER", 2}}, 14},
        // round 2: the size from the softness of the texture (>= 90 % of the fill soft and >= 4 screens: quarter)
        {"texsoft", "texsoft", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}}, 14},
        {"texnoise", "texnoise", {{"GXV_FXLOWRES_AUTO", 1}, {"GXV_FXLOWRES_AUTO_WIN", 2}}, 14},
        // the census: the same decisions, nothing routed: the picture is the reference's
        {"ring-census", "ring", {{"GXV_FXLOWRES_AUTO", 2}, {"GXV_FXLOWRES_AUTO_WIN", 2}}, 14},
    };
    const Variant* vbeg = console ? consoleVariants : variants;
    const Variant* vend = console ? consoleVariants + 2 : variants + sizeof variants / sizeof variants[0];
    for (const Variant* vp = vbeg; vp != vend; ++vp) {
        const Variant& v = *vp;
        Knobs(v.knobs);
        g_logLines.clear();
        backend::DebugFxAutoReset();
        for (unsigned k = 0; k < v.warm; ++k) {
            CmdBuf b;
            b.frame = g_frame++;
            Scene(b, v.scene, std::strcmp(v.scene, "ring") == 0 ? 0.04f : 0.0f);
            backend::Execute(b);
        }
        CmdBuf b;
        b.frame = g_frame++;
        Scene(b, v.scene, 0.0f);
        backend::RequestCapture(true);
        backend::Execute(b);
        backend::RequestCapture(false);
        int w = 0, h = 0;
        const auto& pixels = backend::LastDisplayCopy(w, h);
        Require(w == 960 && h == 544, "requested render dimensions");
        Save(prefix + "-" + v.name + ".rgba", pixels, w, h);
        uint64_t a[8] = {}, r[5] = {}, df[8] = {};
        backend::DebugFxAuto(a);
        backend::DebugFxDefer(df);
        backend::DebugFxLowresRender(r);
        std::printf("RESULT %s: observed %llu, keys routed %llu, divisor %llu, decisions %llu, runs/picture x100 %llu, shared depth copies %llu, level-1 composites %llu, reduced-size runs drawn (all variants so far) %llu\n", v.name,
                    (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[3], (unsigned long long)a[2], (unsigned long long)a[4], (unsigned long long)a[5], (unsigned long long)a[6], (unsigned long long)r[1]);
        std::printf("DEFER %s: additive layer draws %llu, deferrable %llu, deferred runs %llu; kept in place: over %llu, depth-writing blend %llu, overwrite %llu, unknown %llu, texture %llu\n", v.name,
                    (unsigned long long)df[0], (unsigned long long)df[1], (unsigned long long)df[2], (unsigned long long)df[3], (unsigned long long)df[4], (unsigned long long)df[5], (unsigned long long)df[6], (unsigned long long)df[7]);
        Require(glGetError() == GL_NO_ERROR, "no GL errors");
        const std::string nm = v.name;
        {   // round 3: every stage says what it did: at least the first-picture line and the decision line, each naming the keys measured and routed
            uint64_t sd[8] = {};
            backend::DebugFxStage(sd);
            unsigned stageLines = 0;
            for (const std::string& l : g_logLines) stageLines += l.find("fxauto: stage ") == 0 && l.find("keys measured") != std::string::npos ? 1u : 0u;
            std::printf("STAGE %s: pictures %llu, unable %llu, without candidate %llu, with a run %llu, windows %llu, stage lines %u\n", v.name, (unsigned long long)sd[1], (unsigned long long)sd[2], (unsigned long long)sd[3], (unsigned long long)sd[4], (unsigned long long)sd[5], stageLines);
            if (console || nm == "ring-auto") {
                Require(stageLines >= 2 && sd[0] >= 2, "AUTO=1 logs the stage's first-picture line and its first-decision line");
                if (!g_logLines.empty()) std::printf("LOG %s\n", g_logLines.back().c_str());
            }
        }
        if (nm.compare(0, 8, "console-") == 0) Require(a[1] >= 1, "AUTO=1 alone (the console's environment) routes the key of a five-screen glow");
        if (nm == "texsoft") Require(a[1] >= 1 && a[3] == 4, "smooth texture: soft fill of 5 screens: quarter size");
        if (nm == "texnoise") Require(a[1] >= 1 && a[3] == 2, "noise texture: not soft: half size");
        if (nm == "defer-inplace") Require(a[1] == 0, "in place, four runs of a screen do not pay for their scenes: nothing routed");
        if (nm == "defer-on") Require(a[1] >= 1 && df[1] > 0 && df[2] > 0, "deferred: the layers form one run that pays");
        if (nm == "deferover-on") Require(df[3] > 0, "a later over-blend overlapping the first layer keeps it in place");
        if (nm == "deferover-on") Require(df[1] > 0 && a[1] >= 1, "the other layers are still deferred");
        if (nm == "deferover-forced") Require(df[3] == 0, "the tolerance accepted the over-blend (negative control)");
        if (nm == "defercap-on") Require(a[1] == 0, "a capture between the layers ends the stretch: two runs of two screens do not pay");
        if (nm == "defercap-cross") Require(a[1] >= 1, "DEFER=2 crosses the capture: one run of four screens pays");
    }
    glFinish();
    std::printf("PASS: pictures written\n");
    return 0;
}
