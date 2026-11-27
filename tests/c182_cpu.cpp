// Compile the actual old and new game methods against the same instrumented GX sink.
#include <algorithm>
#include <array>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using f32 = float;
using u32 = uint32_t;
using u16 = uint16_t;
using Mtx = f32[3][4];
using Mtx44 = f32[4][4];
using MtxPtr = f32 (*)[4];
using GXTexGenType = unsigned;
using GXTexMtxType = unsigned;
enum { GX_MTX3x4, J3DSysFlag_PostTexMtx };
struct Vec {
    float x, y, z;
    void cross(const Vec& a, const Vec& b) {
        x = a.y*b.z - a.z*b.y; y = a.z*b.x - a.x*b.z; z = a.x*b.y - a.y*b.x;
    }
};
using TVec3f = Vec;
struct TPos3f {
    Mtx m{};
    void identity() { memset(m, 0, sizeof m); m[0][0] = m[1][1] = m[2][2] = 1.f; }
    void setXYZDir(const Vec& a, const Vec& b, const Vec& c) {
        m[0][0]=a.x; m[1][0]=a.y; m[2][0]=a.z; m[0][1]=b.x; m[1][1]=b.y; m[2][1]=b.z; m[0][2]=c.x; m[1][2]=c.y; m[2][2]=c.z;
    }
    void setTrans(const Vec& v) { m[0][3]=v.x; m[1][3]=v.y; m[2][3]=v.z; }
};
struct FishGroup { Vec mUp{}; };
struct BaseFish {
    Vec mFront{}, mPosition{};
    FishGroup* mGroup{};
    TPos3f out{};
    void calcAndSetBaseMtx();
};
struct Fish : BaseFish {
    bool mHaveBasis = false;
    Vec mBasisFront{}, mBasisUp{};
    TPos3f mBasisMtx{};
    void calcAndSetBaseMtx();
};
static unsigned norms, inverses;
#include "normalize.inc"
namespace MR {
static void normalize(const Vec& a, Vec* d) {
    ++norms;
    C_VECNormalize(&a,d);
}
static void normalize(Vec* v) { normalize(*v, v); }
static __attribute__((noinline)) void setBaseTRMtx(BaseFish* f, const TPos3f& m) { f->out=m; }
#include "orientation.inc"
}
struct J3DTextureSRTInfo { float mScaleX, mScaleY; int16_t mRotation; float mTranslationX, mTranslationY; };
struct J3DTexMtxInfo { unsigned mInfo{}, mProjection{}; J3DTextureSRTInfo mSRT{}; Vec mCenter{}; };
struct J3DTexMtx { J3DTexMtxInfo info; auto& getTexMtxInfo() { return info; } };
struct Coord { unsigned type{}; unsigned getTexGenType() { return type; } };
struct J3DTexGenBlock {
    Coord coords[8]{}; J3DTexMtx tex[8]{};
    auto getTexCoord(int i) { return &coords[i]; }
    auto getTexMtx(int i) { return &tex[i]; }
};
struct J3DTexMtxObj {
    unsigned num=8; Mtx mats[8]{}; Mtx44 effects[8]{};
    unsigned getNumTexMtx() { return num; }
    auto& getMtx(int i) { return mats[i]; }
    auto& getEffectMtx(int i) { return effects[i]; }
};
struct Sys {
    Mtx view{}; bool post=false;
    bool checkFlag(unsigned) { return post; } MtxPtr getViewMtx() { return view; }
} J3D_DSYS;
static J3DTexGenBlock block;
static J3DTexMtxObj obj;
static auto* sTexGenBlock=&block;
static auto* sTexMtxObj=&obj;
struct BaseShape { static void loadExecute(const Mtx); };
struct J3DDifferedTexMtx { static void loadExecute(const Mtx); };
struct Command { unsigned id{}, type{}; Mtx m{}; };
static std::array<Command,8> cmds;
static unsigned nextCmd;
static __attribute__((noinline)) void GXLoadTexMtxImm(const Mtx m, unsigned id, unsigned type) {
    auto& c=cmds[nextCmd++]; c.id=id; c.type=type; memcpy(c.m,m,sizeof(Mtx));
}
namespace JMath { static struct { float cosShort(int16_t v) const { return std::cos(v*0.00009587379924f); } } sSinCosTable; }
static float JMASSin(int16_t v) { return std::sin(v*0.00009587379924f); }
static void PSMTXCopy(const Mtx a, Mtx b) { std::memmove(b,a,sizeof(Mtx)); }
#include "math.inc"
static __attribute__((noinline)) u32 PSMTXInverse(const Mtx a, Mtx b) { ++inverses; return C_MTXInverse(a,b); }
#include "base.inc"
#include "current.inc"
static void check(bool ok, const char* msg) { if (!ok) { fprintf(stderr,"FAIL: %s\n",msg); exit(1); } }
static Mtx model={{1,.12f,.1f,4},{.3f,1,.1f,-1},{.15f,.1f,1,-2}};
static unsigned state=1234;
static unsigned randomWord() { state ^= state<<13; state ^= state>>17; state ^= state<<5; return state; }
static void setup() {
    memcpy(J3D_DSYS.view,model,sizeof model);
    for (unsigned i=0;i<8;++i) {
        block.tex[i].info={2,i%2,{0.75f,1.25f,124,0.125f,-0.5f},{.5f,.5f,0}};
        memcpy(obj.mats[i],model,sizeof model); memcpy(obj.effects[i],model,sizeof model); obj.effects[i][3][3]=1;
    }
}
static void compare(bool singular) {
    inverses=nextCmd=0; cmds={}; std::feclearexcept(FE_ALL_EXCEPT);
    BaseShape::loadExecute(model);
    const auto ref=cmds; const auto inv=inverses, count=nextCmd; const int flags=std::fetestexcept(FE_ALL_EXCEPT);
    inverses=nextCmd=0; cmds={}; std::feclearexcept(FE_ALL_EXCEPT);
    J3DDifferedTexMtx::loadExecute(model);
    check(flags==std::fetestexcept(FE_ALL_EXCEPT),"sticky FP flags");
    check(count==nextCmd && !memcmp(ref.data(),cmds.data(),sizeof cmds),"GX command order, IDs, types and all 12 matrix elements");
    check(inverses==(!J3D_DSYS.post||singular?inv:std::min(1u,inv)),"only post slots reuse a successful inverse; retry singular views per slot");
}
static void shapes() {
    setup();
    for (bool post : {false,true}) for (unsigned mode : {2u,5u,8u,10u,11u}) {
        J3D_DSYS.post=post;
        for (auto& t:block.tex) t.info.mInfo=mode;
        for (unsigned i=0;i<4;++i) {
            memcpy(J3D_DSYS.view,model,sizeof model);
            if (i==1) J3D_DSYS.view[0][3]=-0.f;
            if (i==2) J3D_DSYS.view[1][1]=-2.f;
            if (i==3) memset(J3D_DSYS.view,0,sizeof model);
            compare(i==3);
        }
    }
    for (bool post : {false,true}) {
        J3D_DSYS.post=post;
        for (unsigned i=0;i<5000;++i) {
            obj.num=randomWord()%9;
            for (unsigned j=0;j<8;++j) {
                block.tex[j].info.mInfo=(randomWord()%15)|(randomWord()&128); block.coords[j].type=randomWord()%4;
            }
            memcpy(J3D_DSYS.view,model,sizeof model); J3D_DSYS.view[1][1]+=float(i%17)*.025f;
            const bool singular=(i%7)==0;
            if (singular) memset(J3D_DSYS.view,0,sizeof model);
            compare(singular);
        }
    }
}
static void fishTest() {
    Fish f; BaseFish base; FishGroup g{{0,1,0}}; f.mGroup=base.mGroup=&g;
    unsigned oldNorm=0,newNorm=0; f.mFront={1,0,0.3f};
    for (unsigned i=0;i<10000;++i) {
        if (i%19==0) f.mFront.z=float(randomWord()%113)*.025f;
        if (i%31==0) g.mUp.x=float(randomWord()%17)*.01f;
        if (i%59==0) f.mFront.y=-0.f; else if (i%59==1) f.mFront.y=+0.f;
        f.mPosition={float(i),-float(i%17),float(i%31)};
        base.mPosition=f.mPosition; base.mFront=f.mFront;
        // The live model can be touched after publication; a cache hit must still republish all 12 elements.
        memset(f.out.m,0xcd,sizeof f.out.m);
        norms=0; base.calcAndSetBaseMtx(); oldNorm+=norms;
        norms=0; f.calcAndSetBaseMtx(); newNorm+=norms;
        check(!memcmp(base.out.m,f.out.m,sizeof(Mtx)),"fish full orientation and this turn's position");
    }
    check(newNorm < oldNorm/2,"unchanged fish basis avoids repeated normalization");
    printf("fish synthetic turns: 10000; normalization calls %u -> %u\n",oldNorm,newNorm);
    for (Vec front : {Vec{0,0,0}, Vec{INFINITY,0,1}, Vec{2,-0.f,NAN}, Vec{0,1,0}}) {
        g.mUp={0,1,0};
        f.mFront=base.mFront=front;
        for (unsigned i=0;i<2;++i) {
            base.calcAndSetBaseMtx(); f.calcAndSetBaseMtx();
            check(!memcmp(base.out.m,f.out.m,sizeof(Mtx)),"parallel axes and exceptional front vectors");
        }
    }
}
static volatile float sink;
static void bench() {
    setup(); J3D_DSYS.post=true;
    for (unsigned slots : {0u,1u,2u,4u,8u}) {
        obj.num=slots;
        for (auto& c:block.coords) c.type=0;
        std::array<double,9> before{},after{};
        for (int r=0;r<9;++r) for (int p=0;p<2;++p) {
            bool changed=(r+p)%2;
            const auto start=std::chrono::steady_clock::now();
            for (unsigned n=0;n<120000;++n) {
                J3D_DSYS.view[0][3]=float(n%23); nextCmd=0;
                if (changed) J3DDifferedTexMtx::loadExecute(model); else BaseShape::loadExecute(model);
                sink=cmds[slots?slots-1:0].m[0][3];
            }
            const auto elapsed=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/120000.;
            (changed?after:before)[r]=elapsed;
        }
        std::sort(before.begin(),before.end()); std::sort(after.begin(),after.end());
        printf("host only, %u projected slot(s), median ns/invocation: %.2f -> %.2f\n",slots,before[4],after[4]);
    }
    for (unsigned period : {1u,19u,1000000u}) {
        std::array<double,9> before{},after{};
        for (int r=0;r<9;++r) for (int p=0;p<2;++p) {
            bool changed=(r+p)%2;
            Fish f; BaseFish orig; FishGroup group{{0,1,0}}; BaseFish* a=changed?&f:&orig;
            a->mGroup=&group; a->mFront={1,0,.3f};
            const auto start=std::chrono::steady_clock::now();
            for (unsigned n=0;n<240000;++n) {
                a->mPosition={float(n),-float(n%13),float(n%31)};
                if (n%period==0) a->mFront.z=.3f+float(n%17)*.025f;
                if (changed) f.calcAndSetBaseMtx(); else orig.calcAndSetBaseMtx();
                sink=a->out.m[0][0]+a->out.m[2][3];
            }
            (changed?after:before)[r]=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/240000.;
        }
        std::sort(before.begin(),before.end()); std::sort(after.begin(),after.end());
        printf("host only, fish heading updated every %u turn(s), median ns/invocation: %.2f -> %.2f\n",period,before[4],after[4]);
    }
}
int main(int argc,char**) { shapes(); fishTest(); puts("PASS: C182 CPU production-body differentials"); if (argc>1) bench(); }
