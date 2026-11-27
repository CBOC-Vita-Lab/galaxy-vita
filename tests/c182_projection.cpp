#include <array>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include "vgx_hrw.h"

static unsigned allocations;
void* operator new(size_t n) { ++allocations; if (auto* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
enum { CMD_DRAW = 1, UB_PROJ = 0 };
constexpr uint32_t kNoBlock = UINT32_MAX;
struct Cmd { unsigned type, arg; };
struct DrawRec { bool hud; uint32_t ublock[1]; };
struct CmdBuf { std::vector<Cmd> cmds; std::vector<DrawRec> draws; std::vector<float> uni; };
#include "projection.inc"

static unsigned oldAllocs, newAllocs;
static int compare(CmdBuf& b) {
    std::array<float,16> ref{}, now{};
    ref.fill(-991.f); now = ref;
    unsigned start=allocations;
    bool found=BaselineProjection(b,ref.data());
    oldAllocs+=allocations-start;
    start=allocations;
    assert(FindProjection(b,now.data()) == found);
    newAllocs+=allocations-start;
    assert(!std::memcmp(now.data(),ref.data(),sizeof ref));
    return found ? int(ref[0]) : -1;
}
static unsigned state=0x152a;
static unsigned rnd() { state^=state<<13; state^=state>>17; state^=state<<5; return state; }
static volatile float sink;
static void bench(CmdBuf& b) {
    for (unsigned nproj : {1u,4u,32u}) {
        b.cmds.clear();
        for (unsigned i=0;i<420;++i) b.cmds.push_back({CMD_DRAW,(i/8)%nproj});
        std::array<double,9> before{},after{};
        for (int r=0;r<9;++r) for (int p=0;p<2;++p) {
            bool changed=(r+p)%2; float m[16];
            auto start=std::chrono::steady_clock::now();
            for (unsigned n=0;n<7000;++n) {
                if (changed) FindProjection(b,m); else BaselineProjection(b,m);
                sink=m[0];
            }
            (changed?after:before)[r]=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/7000.;
        }
        std::sort(before.begin(),before.end()); std::sort(after.begin(),after.end());
        printf("host only, 420 synthetic draws, %u projection(s), median us/scan: %.3f -> %.3f\n",nproj,before[4],after[4]);
    }
}
int main(int argc,char**) {
    CmdBuf b; assert(compare(b)==-1);
    for (unsigned i=0;i<80;++i) {
        auto at=b.uni.size(); b.uni.resize(at+16,0); b.uni[at]=float(i); b.uni[at+11]=-1.f;
        b.draws.push_back({false,{uint32_t(at)}});
    }
    // Record 65 onwards cannot enter the original histogram; its earlier IDs still gain votes when full.
    for (unsigned i=0;i<80;++i) b.cmds.push_back({CMD_DRAW,i});
    for (unsigned i=0;i<200;++i) b.cmds.push_back({CMD_DRAW,64});
    assert(compare(b)==0);
    b.cmds.push_back({CMD_DRAW,63}); assert(compare(b)==63);
    b.cmds.push_back({CMD_DRAW,2}); assert(compare(b)==2);
    // HUD, ortho, unknown block, short uniform ranges, invalid/non-draw commands.
    b.draws[0].hud=true; b.uni[1*16+11]=0; b.uni[2*16+15]=1;
    b.draws[3].ublock[0]=kNoBlock; b.draws[4].ublock[0]=unsigned(b.uni.size()-15); b.draws[5].ublock[0]=unsigned(b.uni.size()+8);
    b.cmds={{CMD_DRAW,0},{CMD_DRAW,1},{CMD_DRAW,2},{CMD_DRAW,3},{CMD_DRAW,4},{CMD_DRAW,5},{0,6},{CMD_DRAW,91}};
    assert(compare(b)==-1);
    for (unsigned i=0;i<1200;++i) {
        b.cmds.clear(); unsigned n=rnd()%600;
        for (unsigned j=0;j<n;++j) b.cmds.push_back({rnd()%8?1u:0u,rnd()%84});
        compare(b);
    }
    printf("FindProjection actual-body differential: original %u heap allocations, candidate %u\n",oldAllocs,newAllocs);
    assert(oldAllocs && !newAllocs);
    if (argc>1) {
        for (unsigned i=0;i<80;++i) { b.draws[i]={false,{16*i}}; b.uni[16*i+11]=-1; b.uni[16*i+15]=0; }
        bench(b);
    }
}
