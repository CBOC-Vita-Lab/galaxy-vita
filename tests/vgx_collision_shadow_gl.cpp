// Synthetic receiver overlap reproducing CollisionShadow::draw3's two passes.
// Compile against the combined renderer under test, not this test's worktree.
#include "vgx_state.h"
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}
namespace {
using namespace vgx;
alignas(32) unsigned char destination[32];
unsigned failures;
void Check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
void Color(unsigned rgb, unsigned alpha) {
    BP(0xE2, rgb | alpha << 12);
    BP(0xE3, rgb | rgb << 12);
}
void Quad(float right) {
    const float vertices[] = {-1,-1,-0.5f,0, right,-1,-0.5f,0,
                             right,1,-0.5f,0, -1,1,-0.5f,0};
    DrawDecoded(0x80,0,reinterpret_cast<const uint8_t*>(vertices),4);
}
std::vector<uint8_t> Read() {
    g.texCopySrc = EfbRect{0,0,64,64};
    g.texCopyDstW = g.texCopyDstH = 64;
    g.texCopyFmt = 6;
    QueueCopyTex(destination,false);
    const auto id = Pending().copyTex.back().texId;
    FrontPresent(false,nullptr);
    std::vector<uint8_t> pixels;
    int w=0,h=0;
    Check(backend::DebugReadTexture(id,pixels,w,h) && w==64 && h==64,
          "read actual EFB pixels");
    return pixels;
}
void Setup() {
    ResetState();
    g.efbW = g.efbH = 64;
    QueueSetTarget();
    BP(0x43,1);
    BP(0x20,342u | (342u << 12)); BP(0x21,405u | (405u << 12));
    BP(0x59,171u | (171u << 10));
    const uint32_t m[] = {f2u(1),0,0,0, 0,f2u(1),0,0, 0,0,f2u(1),0};
    WriteXF(0,12,m);
    const uint32_t proj[] = {f2u(1),0,f2u(1),0,f2u(1),0,1};
    WriteXF(0x1020,7,proj);
    const uint32_t vp[] = {f2u(32),f2u(-32),f2u(16777216),f2u(374),f2u(374),f2u(16777216)};
    WriteXF(0x101A,6,vp);
    const uint32_t noTex=0; WriteXF(0x103F,1,&noTex);
    BP(0x00,0); BP(0x28,7u << 7);
    BP(0xC0,(2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1,(1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3,(7u << 16) | (7u << 19));
    BP(0x40,0);
}
}
int main(int argc,char** argv) {
    const bool fixed=argc>1 && std::atoi(argv[1])==1;
    Check(vgx::host::CreateContext(),"GLES context");
    Check(vgx::backend::Init(64,64),"production backend init");
    if (failures) return 1;
    Setup();
    for (unsigned receivers : {1u,2u,4u,8u}) {
        // Opaque destination inside the projected shadow volume.
        Color(240,255); BP(0x42,0); BP(0x41,(1u << 3) | (1u << 4)); Quad(1);
        // A fixed 128/255 mask sample substitutes for the captured texture.
        Color(0,128);
        BP(0xF3,(5u << 16) | (5u << 19)); // draw3: alpha != 0, both compares.
        for (unsigned i=0;i<receivers;++i) {
            BP(0x41,1 | (1u << 4) | (6u << 8) | (7u << 5));
            BP(0x42,0); Quad(0); // Color update off; mask alpha first.
            BP(0x41,1 | (1u << 3) | (1u << 4) | (6u << 8) | (7u << 5));
            BP(0x42,1u << 8); Quad(0); // Color on; GXSetDstAlpha(TRUE,0).
        }
        auto pixels=Read();
        if (pixels.empty()) return 1;
        const auto* shadow=&pixels[(32*64+16)*4];
        const auto* floor=&pixels[(32*64+48)*4];
        std::printf("mode=%s receivers=%u shadow=%u,%u,%u,%u outside=%u,%u,%u,%u\n",
          fixed?"zero":"legacy",receivers,shadow[0],shadow[1],shadow[2],shadow[3],
          floor[0],floor[1],floor[2],floor[3]);
        Check(floor[0]==240 && floor[3]==255,"uncovered floor unchanged");
        Check(shadow[3]==(fixed?0:128),"stored alpha follows requested mode");
        if (fixed) Check(shadow[0]>=119 && shadow[0]<=120,"overlap does not repeatedly darken");
        else if (receivers==1) Check(shadow[0]>=119 && shadow[0]<=120,"first shadow matches fixed RGB");
        else if (receivers==8) Check(shadow[0]<=1,"legacy overlap darkens floor to near black");
    }
    Check(glGetError()==GL_NO_ERROR,"no GL error");
    return failures?1:0;
}
