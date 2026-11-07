// Procedural ClipArea sphere volumes and FallOutField's tiled capture chain.
// This test uses production GX command decoding, TEV generation and GLES.
#include "vgx_state.h"
#include <GLES2/gl2.h>
#include <cmath>
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
constexpr int W=128, H=96;
alignas(32) unsigned char destinations[8][32];
unsigned failures=0;
struct Picture { std::vector<uint8_t> p; int w=0,h=0; };
void Check(bool ok,const char* label) {
    std::printf("%s: %s\n",ok?"PASS":"FAIL",label);
    if(!ok) ++failures;
}
void BP(unsigned r,unsigned v) { WriteBP(r<<24|v); }
void XF(unsigned r,unsigned v) { WriteXF(r,1,&v); }
void Viewport(float scale=1,float offset=0,bool inset=false) {
    const float x=inset?4:0,y=inset?2:0,w=inset?120:W,h=inset?92:H;
    const uint32_t v[]={f2u(w/2),f2u(-h/2),f2u(scale*16777215+1),
        f2u(x+w/2+342),f2u(y+h/2+342),f2u(scale*16777215+1+offset*16777215)};
    WriteXF(0x101A,6,v);
}
void Scissor(bool inset=false) {
    const unsigned x=inset?8:0,y=inset?6:0,w=inset?112:W,h=inset?84:H;
    BP(0x20,(y+342)|((x+342)<<12));
    BP(0x21,(y+h-1+342)|((x+w-1+342)<<12));
    BP(0x59,171u|(171u<<10));
}
void Color(unsigned r,unsigned green,unsigned b,unsigned a) {
    BP(0xE2,r|(a<<12)); BP(0xE3,b|(green<<12));
}
void Pass(unsigned cull=0) {
    BP(0, cull<<14); XF(0x103F,0); BP(0x28,7u<<7);
    BP(0xC0,(2u<<12)|(15u<<8)|(15u<<4)|15u|(1u<<19));
    BP(0xC1,(1u<<13)|(7u<<10)|(7u<<7)|(7u<<4)|(1u<<19));
    BP(0xF3,(7u<<16)|(7u<<19));
}
void Quad(float left,float top,float right,float bottom,float depth,
          bool tex=false,float du=0,float dv=0) {
    const float z=-depth;
    const float v[]={left,bottom,z,0,du,1+dv, right,bottom,z,0,1+du,1+dv,
        right,top,z,0,1+du,dv, left,top,z,0,du,dv};
    if(tex) DrawDecoded(0x80,VL_TEX0,reinterpret_cast<const uint8_t*>(v),4);
    else {
        float plain[16];
        for(int i=0;i<4;++i) std::memcpy(plain+i*4,v+i*6,16);
        DrawDecoded(0x80,0,reinterpret_cast<const uint8_t*>(plain),4);
    }
}
void Tile(int divisions,int tile,float du=0,float dv=0) {
    const int x=tile%divisions,y=tile/divisions;
    Quad(-1+2.0f*x/divisions,1-2.0f*y/divisions,
         -1+2.0f*(x+1)/divisions,1-2.0f*(y+1)/divisions,.5f,true,du,dv);
}
struct V { float x,y,z,pad=0; };
V Point(float a,float b) { return {std::sin(a)*std::cos(b),std::cos(a),std::sin(a)*std::sin(b)}; }
void Sphere(float cx,float cy,float depth,float radius) {
    std::vector<V> mesh;
    constexpr int lat=24,lon=48;
    constexpr float pi=3.14159265358979323846f;
    auto triangle=[&](V a,V b,V c) {
        const V u{b.x-a.x,b.y-a.y,b.z-a.z},v{c.x-a.x,c.y-a.y,c.z-a.z};
        const V n{u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
        // GX exterior faces are clockwise before the EFB Y flip.
        if(n.x*a.x+n.y*a.y+n.z*a.z>0) std::swap(b,c);
        for(V p:{a,b,c}) mesh.push_back({cx+p.x*radius,cy+p.y*radius,-depth+p.z*radius});
    };
    for(int y=0;y<lat;++y) for(int x=0;x<lon;++x) {
        V a=Point(pi*y/lat,2*pi*x/lon),b=Point(pi*(y+1)/lat,2*pi*x/lon);
        V c=Point(pi*(y+1)/lat,2*pi*(x+1)/lon),d=Point(pi*y/lat,2*pi*(x+1)/lon);
        triangle(a,b,c); triangle(a,c,d);
    }
    DrawDecoded(0x90,0,reinterpret_cast<const uint8_t*>(mesh.data()),mesh.size());
}
uint32_t Copy(int index,int x,int y,int sw,int sh,int dw,int dh,unsigned format=0x27) {
    g.texCopySrc=EfbRect{int16_t(x),int16_t(y),int16_t(sw),int16_t(sh)};
    g.texCopyDstW=dw;g.texCopyDstH=dh;g.texCopyFmt=format;
    QueueCopyTex(destinations[index],false);
    return Pending().copyTex.back().texId;
}
Picture Read(uint32_t id) {
    FrontPresent(false,nullptr);
    Picture out;
    Check(backend::DebugReadTexture(id,out.p,out.w,out.h),"read actual renderer texture");
    return out;
}
unsigned Pixel(const Picture& p,float u,float v,int channel=3) {
    int x=std::min(p.w-1,std::max(0,int(u*p.w))),y=std::min(p.h-1,std::max(0,int(v*p.h)));
    return p.p[(y*p.w+x)*4+channel];
}
void Bind(int index,int w,int h) {
    // The production binding memo keys BP texture registers, not host TexObj
    // pointers. Keep those identities in step with this host fixture binding.
    BP(0x94,(reinterpret_cast<uintptr_t>(destinations[index])>>5)&0xFFFFFF);
    BP(0x88,(w-1)|((h-1)<<10)|(1u<<20));
    auto& t=g.tex[0];t=TexSlot{};t.valid=true;
    t.obj.data=destinations[index];t.obj.width=w;t.obj.height=h;t.obj.format=1;
    t.obj.minFilter=t.obj.magFilter=1;
    g.bpTexStale=0;g.texDirty=true;g.texSlotDirty=1;
    XF(0x103F,1);XF(0x1040,5u<<7);BP(0,1);BP(0x28,(1u<<6)|(7u<<7));
}
void Compare() {
    BP(0xE0,(1u<<23)|(1u<<12));BP(0xE1,1u<<23);BP(0xF6,28u<<9);
    BP(0xC1,(6u<<13)|(4u<<10)|(1u<<7)|(7u<<4)|(3u<<16)|(1u<<19)|(3u<<20));
}
void Volumes() {
    for(unsigned cull:{2u,1u}) {
        Pass(cull);Color(0,0,0,4);BP(0x40,1|(6u<<1));BP(0x42,0);
        BP(0x41,cull==2?(1|(1u<<4)|(1u<<8)|(1u<<5)):((1u<<4)|(1u<<11)));
        Sphere(-.4f,0,.5f,.4f);
        Sphere(-.2f,0,.5f,.3f);
        Sphere(.5f,0,.85f,.12f);
    }
}
void Run(bool inset) {
    Pass();Viewport();Scissor();Color(0,255,0,255);
    BP(0x40,1|(7u<<1)|(1u<<4));BP(0x41,(1u<<3)|(1u<<4));BP(0x42,1u<<8);
    Quad(-1,1,1,-1,.5f);
    Viewport(1,0,inset);Scissor(inset);Volumes();
    uint32_t original=Copy(0,0,0,W,H,W/2,H/2);

    // Original half-size A8 -> binary alpha into top-left half EFB -> quarter A8.
    Pass();Viewport(0,0);Scissor();Bind(0,W/2,H/2);Color(255,255,255,255);Compare();
    BP(0x40,0);BP(0x41,1u<<4);BP(0x42,0);Tile(2,0);
    uint32_t binary=Copy(1,0,0,W/2,H/2,W/4,H/4);

    // Match the five-tap translated-texture sequence, including the actor's
    // alpha TEV (texture alpha directly; material intensity only affects RGB).
    Bind(1,W/4,H/4);
    BP(0xC1,(4u<<13)|(7u<<10)|(7u<<7)|(7u<<4)|(1u<<19));
    BP(0xC0,(9u<<12)|(15u<<8)|(15u<<4)|15u|(1u<<19));
    for(int i=0;i<5;++i) {
        const float a=6.28318530718f*i/5;
        BP(0x41,i?1|(1u<<4)|(1u<<8)|(1u<<5):(1u<<4));
        Tile(4,4,.004f*std::cos(a),.004f*1.333f*std::sin(a));
    }
    uint32_t blurred=Copy(2,0,H/4,W/4,H/4,W/4,H/4);

    // Edge consumes only blurred alpha, then fill reloads original mask.
    Bind(2,W/4,H/4);Color(0,46,200,128);
    BP(0xC0,(2u<<12)|(15u<<8)|(15u<<4)|15u|(1u<<19));
    BP(0xC1,(7u<<13)|(1u<<10)|(4u<<7)|(7u<<4)|(1u<<19));
    BP(0x41,1|(1u<<3)|(1u<<4)|(4u<<8)|(5u<<5));BP(0x42,1u<<8);Tile(1,0);
    Bind(0,W/2,H/2);Color(0,0,0,255);Compare();
    BP(0xF3,(4u<<16)|(7u<<19));BP(0x40,1|(7u<<1)|(1u<<4));
    BP(0x41,1|(1u<<3)|(4u<<8)|(5u<<5));Viewport(0,1);Tile(1,0);
    // A later background exposes only the pixels whose depth was erased.
    Pass();Viewport();Color(0,0,255,255);BP(0x40,1|(3u<<1)|(1u<<4));BP(0x41,1u<<3);
    Quad(-1,1,1,-1,.9f);
    Picture final=Read(Copy(3,0,0,W,H,W,H,6));
    Picture mask=Read(original),threshold=Read(binary),blur=Read(blurred);
    std::printf("mask %dx%d inset=%d samples=%u,%u,%u\n",mask.w,mask.h,inset,
        Pixel(mask,.175f,.5f),Pixel(mask,.35f,.5f),Pixel(mask,.75f,.5f));
    Check(Pixel(mask,.175f,.5f)==4,"outward sphere front/back culling leaves one depth intersection");
    Check(Pixel(mask,.35f,.5f)==8,"overlapping spheres accumulate intersection count");
    Check(Pixel(mask,.75f,.5f)==0,"sphere completely behind floor cancels after reverse subtract");
    std::printf("binary %dx%d samples=%u,%u\n",threshold.w,threshold.h,Pixel(threshold,.35f,.5f),Pixel(threshold,.8f,.5f));
    Check(Pixel(threshold,.35f,.5f)==0 && Pixel(threshold,.8f,.5f)==255,"half-screen compare and quarter-size capture preserve interior/exterior");
    Check(Pixel(blur,.35f,.5f)==0 && Pixel(blur,.8f,.5f)==255,"tiled five-tap blur retains interior/exterior");
    std::printf("final %dx%d inside G=%u outside B=%u\n",final.w,final.h,Pixel(final,.35f,.5f,1),Pixel(final,.8f,.5f,2));
    Check(Pixel(final,.35f,.5f,1)==255 && Pixel(final,.35f,.5f,2)==0,"full tiled chain preserves the revealed floor");
    Check(Pixel(final,.8f,.5f,2)==255 && Pixel(final,.8f,.5f,1)==0,"full tiled chain reveals background only outside the sphere");
    Check(glGetError()==GL_NO_ERROR,"no GL error");
}
}
int main(int argc,char** argv) {
    const int rw=argc>1?std::atoi(argv[1]):W,rh=argc>2?std::atoi(argv[2]):H;
    Check(host::CreateContext(),"GLES context");Check(backend::Init(rw,rh),"production backend init");
    if(failures)return 1;
    ResetState();g.efbW=W;g.efbH=H;QueueSetTarget();BP(0x43,1);
    const uint32_t m[]={f2u(1),0,0,0,0,f2u(1),0,0,0,0,f2u(1),0};WriteXF(0,12,m);
    const uint32_t p[]={f2u(1),0,f2u(1),0,f2u(-1),f2u(-1),1};WriteXF(0x1020,7,p);
    Run(false);Run(true);
    return failures?1:0;
}
