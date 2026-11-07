// Synthetic ClipArea/FallOutField-style passes through the real GX front end
// and GLES backend. No Nintendo geometry or textures are used.
#include "vgx_state.h"
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}
namespace {
using namespace vgx;
alignas(32) unsigned char maskDestination[32];
unsigned failures = 0;
void Check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
void XF(unsigned reg, unsigned value) { WriteXF(reg, 1, &value); }
void Viewport(float scale = 1, float offset = 0) {
    const uint32_t w[] = {f2u(32), f2u(-32), f2u(scale * 16777215.0f + 1),
                         f2u(374), f2u(374), f2u(scale * 16777215.0f + 1 + offset * 16777215.0f)};
    WriteXF(0x101A, 6, w);
}
void Color(unsigned r, unsigned green, unsigned b, unsigned alpha) {
    BP(0xE2, r | alpha << 12);
    BP(0xE3, b | green << 12);
}
void Pass() {
    BP(0x00, 0);
    XF(0x103F, 0);
    BP(0x28, 7u << 7);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
}
void Quad(float left, float right, float depth, bool tex = false) {
    const float z = depth - 1;
    const float v[] = {left,-1,z,0, 0,1, right,-1,z,0, 1,1,
                      right,1,z,0, 1,0, left,1,z,0, 0,0};
    if (tex) DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4);
    else {
        float plain[16];
        for (int i = 0; i < 4; ++i) std::memcpy(plain + i * 4, v + i * 6, 16);
        DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(plain), 4);
    }
}
void Flush() { FrontPresent(false, nullptr); }
void Copy(unsigned format = 0x27) {
    g.texCopySrc = EfbRect{0,0,64,64};
    g.texCopyDstW = g.texCopyDstH = 64;
    g.texCopyFmt = format;
    QueueCopyTex(maskDestination, false);
}
std::vector<uint8_t> Read(unsigned format) {
    Copy(format);
    const auto id = Pending().copyTex.back().texId;
    Flush();
    std::vector<uint8_t> pixels;
    int w=0,h=0;
    Check(backend::DebugReadTexture(id,pixels,w,h) && w==64 && h==64, "read actual 64x64 copied EFB");
    return pixels;
}
const uint8_t* Pixel(const std::vector<uint8_t>& p, int x) { return &p[(32*64+x)*4]; }
void BindMask() {
    auto& t = g.tex[0];
    t = TexSlot{};
    t.valid = true;
    t.obj.data = maskDestination;
    t.obj.width = t.obj.height = 64;
    t.obj.format = 1;
    g.bpTexStale = 0;
    g.texDirty = true;
    g.texSlotDirty = 1;
    XF(0x103F, 1);
    XF(0x1040, 5u << 7);
    BP(0x00, 1);
    BP(0x28, (1u << 6) | (7u << 7));
}
}
int main(int argc, char** argv) {
    const bool zero = argc < 2 || std::strcmp(argv[1], "zero") == 0;
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(64,64), "production backend init");
    if (failures) return 1;
    ResetState();
    g.efbW = g.efbH = 64;
    QueueSetTarget();
    BP(0x43, 1); // GX_PF_RGBA6_Z24: the alpha-volume path needs EFB alpha.
    BP(0x20, 342u | (342u << 12));
    BP(0x21, 405u | (405u << 12));
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1),0,0,0, 0,f2u(1),0,0, 0,0,f2u(1),0};
    WriteXF(0,12,m);
    const uint32_t proj[] = {f2u(1),0,f2u(1),0,f2u(1),0,1};
    WriteXF(0x1020,7,proj);
    Viewport();
    Pass();

    // Green floor, with alpha zero and depth 0.5, is submitted as full geometry.
    Color(0,255,0,255);
    BP(0x40, 1 | (7u << 1) | (1u << 4));
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 1u << 8);
    Quad(-1,1,0.5f);

    // Left: back face behind floor, front face before floor, leaving alpha 4.
    // Right: both faces behind floor, so add/subtract must cancel to alpha 0.
    Color(0,0,0,4);
    BP(0x40, 1 | (6u << 1)); // GEQUAL, no Z writes.
    BP(0x42, 0);
    BP(0x41, 1 | (1u << 4) | (1u << 8) | (1u << 5));
    Quad(-1,1,0.75f);
    BP(0x41, (1u << 4) | (1u << 11));
    Quad(-1,0,0.25f);
    Quad(0,1,0.6f);
    auto mask = Read(0x27);
    if (mask.empty()) return 1;
    std::printf("volume mask L=%u R=%u\n", Pixel(mask,16)[3], Pixel(mask,48)[3]);
    Check(Pixel(mask,16)[3]==4 && Pixel(mask,48)[3]==0, "volume add/subtract then A8 capture");

    // FallOutFieldDraw masks with (konst alpha 1 > captured alpha), blackens
    // only outside the volume and resets that region's depth to far.
    BindMask();
    Color(0,0,0,255);
    BP(0xE0, (1u << 23) | (1u << 12)); // K0 alpha = 1.
    BP(0xE1, 1u << 23);
    BP(0xF6, 28u << 9); // K0_A selector.
    BP(0xC1, (6u << 13) | (4u << 10) | (1u << 7) | (7u << 4) |
              (3u << 16) | (1u << 19) | (3u << 20)); // A8_GT.
    BP(0xF3, (4u << 16) | (7u << 19)); // alpha > 0.
    BP(0x40, 1 | (7u << 1) | (1u << 4));
    BP(0x41, 1 | (1u << 3) | (4u << 8) | (5u << 5));
    Viewport(0,1);
    Quad(-1,1,0.5f,true);
    auto filled=Read(6);
    std::printf("filled L=(%u,%u,%u) R=(%u,%u,%u)\n",Pixel(filled,16)[0],Pixel(filled,16)[1],Pixel(filled,16)[2],Pixel(filled,48)[0],Pixel(filled,48)[1],Pixel(filled,48)[2]);
    Check(Pixel(filled,16)[1]==255 && Pixel(filled,48)[1]==0, "captured mask preserves inside floor and erases outside");
    Pass();
    Viewport();
    Color(0,0,255,255);
    BP(0x40, 1 | (3u << 1) | (1u << 4));
    BP(0x41, 1u << 3);
    Quad(-1,1,0.9f);
    auto background=Read(6);
    Check(Pixel(background,16)[1]==255 && Pixel(background,48)[2]==255,
          "far background appears only where GXSetZScaleOffset cleared depth");

    // Real edge pass requests blended color, but constant stored alpha 0.
    Color(255,0,0,128);
    BP(0x40, 1 | (7u << 1));
    BP(0x41, 1 | (1u << 3) | (1u << 4) | (4u << 8) | (5u << 5));
    BP(0x42, 1u << 8);
    Quad(-1,1,0.5f);
    auto edge=Read(6);
    std::printf("edge alpha L=%u R=%u\n", Pixel(edge,16)[3], Pixel(edge,48)[3]);
    Check(Pixel(edge,16)[3]==(zero ? 0 : 66) && Pixel(edge,48)[3]==(zero ? 0 : 64),
          zero ? "blended edge clears stored alpha independently of source alpha" : "legacy edge retains the known uncleared alpha");
    Check(Pixel(edge,16)[0]==128 && Pixel(edge,16)[1]==127 && Pixel(edge,48)[0]==128 && Pixel(edge,48)[2]==127,
          "RGB keeps original source-alpha factors");
    setenv("GXV_DSTALPHA_ZERO", zero ? "0" : "1", 1);
    const auto reset = [] {
        Pass();
        BP(0x43,1);
        Color(200,200,200,200);
        BP(0x42,0);
        BP(0x41,(1u << 3) | (1u << 4));
        BP(0x40,1 | (7u << 1) | (1u << 4));
        Quad(-1,1,0.5f);
    };
    const auto blend = [] {
        Color(128,0,0,128);
        BP(0x42,1u << 8);
        BP(0x41,1 | (1u << 3) | (1u << 4) | (4u << 8) | (5u << 5));
        BP(0x40,1 | (7u << 1));
    };
    reset(); blend(); Quad(-1,1,0.5f);
    BP(0x42,0); // Identical RGB factors must still restore the alpha factors.
    Quad(-1,1,0.5f);
    auto restored=Read(6);
    Check(Pixel(restored,16)[3]==(zero ? 64 : 146), "ordinary blending restores alpha factors after override");
    reset(); blend(); BP(0xF3,200u | (4u << 16) | (7u << 19)); Quad(-1,1,0.5f);
    auto alphaRejected=Read(6);
    Check(Pixel(alphaRejected,16)[0]==200 && Pixel(alphaRejected,16)[3]==200,
          "alpha-test-rejected fragments leave RGB and alpha untouched");
    reset(); blend(); BP(0x40,1); Quad(-1,1,0.5f);
    auto depthRejected=Read(6);
    Check(Pixel(depthRejected,16)[0]==200 && Pixel(depthRejected,16)[3]==200,
          "depth-test-rejected fragments leave RGB and alpha untouched");
    reset(); blend(); BP(0x41,1 | (1u << 3) | (4u << 8) | (5u << 5)); Quad(-1,1,0.5f);
    auto alphaDisabled=Read(6);
    Check(Pixel(alphaDisabled,16)[3]==200, "disabled alpha update does not clear destination alpha");
    reset(); blend(); BP(0x42,(1u << 8) | 17u); Quad(-1,1,0.5f);
    auto nonzero=Read(6);
    Check(Pixel(nonzero,16)[3]==164, "nonzero destination-alpha requests retain legacy behavior");
    reset(); blend(); BP(0x43,0); Quad(-1,1,0.5f);
    auto noAlpha=Read(0x27);
    Check(Pixel(noAlpha,16)[3]==255, "RGB8 EFB alpha copies still read opaque");
    reset(); blend(); BP(0x41,(1u << 3) | (1u << 4) | (1u << 11)); Quad(-1,1,0.5f);
    auto subtract=Read(6);
    Check(Pixel(subtract,16)[0]==72 && Pixel(subtract,16)[3]==(zero ? 0 : 72),
          "subtract blending clears only alpha and preserves subtract RGB");
    reset(); blend(); BP(0x41,1 | (1u << 3) | (1u << 4) | (1u << 8)); Quad(-1,1,0.5f);
    auto replaceBlend=Read(6);
    Check(Pixel(replaceBlend,16)[0]==128 && Pixel(replaceBlend,16)[3]==(zero ? 0 : 128),
          "ONE/ZERO blend still honors constant zero alpha");
    Check(glGetError()==GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
