// Actual GXTransform C entry paths and FIFO overflow implementation, with a
// pinned pre-change translation unit as the independent command-stream oracle.
#include <array>
#include <cerrno>
#include <cfenv>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../platform/vgx/src/vgx_common.h"
#include "../platform/src/gx/gpu.h"
#include "../platform/src/gx/vgx_galaxy.h"
#include "port/gx_fifo.h"
#include "port/heap_routing.h"
#include "port/port.h"
#include "revolution/gx.h"
#include "revolution/gx/GXRegs.h"
#include "revolution/gx/GXPerf.h"
#include "revolution/os/OSInterrupt.h"
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

static unsigned parses, mismatches, coverage;
static bool injectFault;
static uintptr_t sourceBegin, sourceEnd;
namespace vgx {
static long BurstEnvInt(const char* name, long fallback) {
    if (strcmp(name, "GXV_XFBURST") == 0) ++parses;
    return EnvInt(name, fallback);
}
}
static void* BurstMemcpy(void* dest, const void* source, size_t size) {
    void* result = memcpy(dest, source, size);
    const uintptr_t address = reinterpret_cast<uintptr_t>(source);
    // Corrupt only candidate source loads, not the original FIFO primitive or
    // reference translation unit. This must produce an oracle mismatch.
    if (injectFault && size == 4 && address >= sourceBegin && address < sourceEnd)
        static_cast<unsigned char*>(dest)[0] ^= 1;
    return result;
}
#if defined(__ARM_NEON)
static uint8x16_t BurstLoad(const uint8_t* source) {
    uint8x16_t result = vld1q_u8(source);
    const uintptr_t address = reinterpret_cast<uintptr_t>(source);
    if (injectFault && address >= sourceBegin && address < sourceEnd) {
        static const uint8_t fault[16] = {1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0};
        result = veorq_u8(result, vld1q_u8(fault));
    }
    return result;
}
#pragma push_macro("vld1q_u8")
#undef vld1q_u8
#define vld1q_u8 BurstLoad
#endif
#define EnvInt BurstEnvInt
#define memcpy BurstMemcpy
#define __PortGXFifoOverflow ActualFifoOverflow
#include "../platform/src/gx/gx_fifo.cpp"
#undef __PortGXFifoOverflow
#undef memcpy
#undef EnvInt
#if defined(__ARM_NEON)
#pragma pop_macro("vld1q_u8")
static constexpr bool burstEnabled = true;
#else
static constexpr bool burstEnabled = false;
#endif

namespace reference {
#include XF_BURST_BASELINE
}

static GXData gxState;
extern "C" GXData* const __GXData = &gxState;
extern "C" void TestProjectionPayload(const float*);

alignas(16) static std::array<unsigned char, 1024> fifo;
static std::vector<std::string> events;
static std::vector<unsigned char> staging0, staging1;
static bool player, rebind;
static unsigned publication;
static int roundingMode = FE_DOWNWARD;
static constexpr size_t captured = 131072;

static void require(bool yes, const char* message) {
    if (!yes) { fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static std::string position(const void* ptr) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    struct Region { const char* name; const unsigned char* base; size_t size; };
    const Region regions[] = {{"fifo",fifo.data(),fifo.size()}, {"sink",sOverflowSink,sizeof sOverflowSink},
        {"a",staging0.data(),staging0.size()}, {"b",staging1.data(),staging1.size()}};
    for (const auto& r : regions) {
        const uintptr_t b = reinterpret_cast<uintptr_t>(r.base);
        if (r.base && p >= b && p-b <= r.size) return std::string(r.name)+":"+std::to_string(p-b);
    }
    return "outside";
}
extern "C" void __PortGXFifoOverflow() {
    events.push_back("overflow:"+position(__PortGXFifoPtr));
    ActualFifoOverflow();
    events.push_back("resume:"+position(__PortGXFifoPtr)+":"+position(__PortGXFifoLimit));
}
extern "C" int port_vgx_player() { return player; }
extern "C" int port_vgx_active() { return 1; }
extern "C" uint8_t* port_vgx_publish(uint8_t* p, int flags) {
    events.push_back("publish:"+position(p)+":"+std::to_string(flags));
    ++publication;
    errno = EDOM;
    if (rebind) return p >= staging0.data() && p < staging0.data()+staging0.size() ? staging1.data()+16 : staging0.data()+16;
    return p;
}
extern "C" size_t port_vgx_consume(const uint8_t*, size_t n) { events.push_back("consume:"+std::to_string(n)); return n; }
extern "C" void port_vgx_stream_setup(uint8_t*,uint8_t*,size_t) {}
extern "C" void port_perf_record(int64_t) {}
extern "C" int64_t port_host_time_ns() { return 1; }
extern "C" void* port_host_alloc(size_t n,int alignment) { void* p=nullptr; require(posix_memalign(&p,alignment,n)==0,"host allocation"); return p; }
extern "C" void port_fatal(const char*,...) { std::abort(); }
extern "C" void port_log(const char* fmt,...) {
    if (strstr(fmt,"xfburst mismatch")) ++mismatches;
    if (strstr(fmt,"xfburst oracle")) ++coverage;
    errno = ENOSPC;
    fesetround(FE_UPWARD); feraiseexcept(FE_INVALID);
}
extern "C" void port_log_summary(const char* fmt,...) { port_log(fmt); }
extern "C" void port_host_alloc_scope_enter() {}
extern "C" void port_host_alloc_scope_leave() {}
namespace gpu { size_t process(const uint8_t*,size_t n,bool) { return n; } }

static constexpr uint32_t bits[12] = {0x00000000,0x80000000,0x00000001,0x007fffff,
    0x3f800000,0xbf123456,0x7f800000,0xff800000,0x7fc12345,0x7fa54321,0xffc22222,0x00800000};
static constexpr unsigned words[] = {6,12,8,9,6,12,8};
struct Result {
    std::array<unsigned char,1024> image;
    std::array<unsigned char,256> sink;
    std::vector<unsigned char> first, second;
    std::vector<std::string> trace;
    std::string cursor, limit;
    unsigned wrap, publications;
    int error, flags, rounding;
};
static void invoke(bool old, unsigned shape, const float* src) {
    const auto m = reinterpret_cast<const float (*)[4]>(src);
    if (old) {
        switch(shape) {
        case 0: reference::WriteProjPS(src,nullptr); break;
        case 1: reference::GXLoadPosMtxImm(m,7); break;
        case 2: reference::GXLoadTexMtxImm(m,39,GX_MTX2x4); break;
        case 3: reference::GXLoadNrmMtxImm(m,7); break;
        case 4: reference::__GXSetProjection(); break;
        case 5: reference::GXLoadTexMtxImm(m,125,GX_MTX3x4); break;
        case 6: reference::GXLoadTexMtxImm(m,125,static_cast<GXTexMtxType>(17)); break;
        }
    } else {
        switch(shape) {
        case 0: TestProjectionPayload(src); break;
        case 1: GXLoadPosMtxImm(m,7); break;
        case 2: GXLoadTexMtxImm(m,39,GX_MTX2x4); break;
        case 3: GXLoadNrmMtxImm(m,7); break;
        case 4: __GXSetProjection(); break;
        case 5: GXLoadTexMtxImm(m,125,GX_MTX3x4); break;
        case 6: GXLoadTexMtxImm(m,125,static_cast<GXTexMtxType>(17)); break;
        }
    }
}
static Result run(bool old,unsigned shape,int distance,unsigned offset,int alias,bool linked=false,unsigned repeats=1,
                  bool hard=false,bool synchronous=false) {
    fifo.fill(0xA5); memset(sOverflowSink,0xA5,sizeof sOverflowSink);
    memset(&gxState,0,sizeof gxState); memcpy(gxState.projMtx,bits,24); gxState.projType=GX_ORTHOGRAPHIC;
    sCPUFifo={}; sGPFifo={}; sCPUReady=true; sGPReady=false; sLinked=linked; sInFlush=false;
    player=linked && !synchronous; publication=0; events.clear();
    unsigned char* base=fifo.data();
    if (linked) {
        if(staging0.empty()) { staging0.resize(kStagingSize); staging1.resize(kStagingSize); }
        std::fill(staging0.begin(),staging0.begin()+captured,0xA5);
        std::fill(staging1.begin(),staging1.begin()+captured,0xA5);
        std::fill(staging0.end()-128,staging0.end(),0xA5);
        std::fill(staging1.end()-128,staging1.end(),0xA5);
        base=staging0.data(); sStaging=base; sStagingOther=staging1.data();
    }
    __PortGXFifoPtr=base+64+offset;
    const unsigned header=shape==0 ? 0 : 5;
    if(hard) __PortGXFifoPtr=base+kStagingSize-kSlack-header-distance;
    __PortGXFifoLimit=__PortGXFifoPtr+header+distance;
    sCPUFifo.base=base; sCPUFifo.top=__PortGXFifoLimit; sCPUFifo.wrPtr=__PortGXFifoPtr;
    alignas(16) float input[12]; memcpy(input,bits,sizeof bits);
    float* src=input;
    if(alias!=999) { src=reinterpret_cast<float*>(__PortGXFifoPtr+header+alias); memcpy(src,bits,sizeof bits); }
    sourceBegin=reinterpret_cast<uintptr_t>(shape==4 ? gxState.projMtx : src);
    sourceEnd=sourceBegin+(shape==0 || shape==4 ? 24 : shape==2 || shape==6 ? 32 : shape==3 ? 44 : 48);
    errno=E2BIG; fesetround(roundingMode); feclearexcept(FE_ALL_EXCEPT); feraiseexcept(FE_DIVBYZERO);
    for(unsigned i=0;i<repeats;++i) invoke(old,shape,src);
    Result r{fifo,{}, {},{},events,position(__PortGXFifoPtr),position(__PortGXFifoLimit),sCPUFifo.wrap,publication,errno,fetestexcept(FE_ALL_EXCEPT),fegetround()};
    memcpy(r.sink.data(),sOverflowSink,sizeof sOverflowSink);
    if(linked) {
        r.first.assign(staging0.begin(),staging0.begin()+captured); r.second.assign(staging1.begin(),staging1.begin()+captured);
        r.first.insert(r.first.end(),staging0.end()-128,staging0.end()); r.second.insert(r.second.end(),staging1.end()-128,staging1.end());
    }
    feclearexcept(FE_ALL_EXCEPT); fesetround(FE_TONEAREST);
    return r;
}
static void compare(const Result& a,const Result& b) {
    require(a.image==b.image && a.sink==b.sink && a.first==b.first && a.second==b.second,"exact command bytes including untouched storage");
    require(a.trace==b.trace && a.cursor==b.cursor && a.limit==b.limit && a.wrap==b.wrap && a.publications==b.publications,"overflow/publication trace, cursor, limit and wrap match old path");
    if(a.error!=b.error || a.flags!=b.flags || a.rounding!=b.rounding)
        fprintf(stderr,"state old/new errno=%d/%d flags=%d/%d round=%d/%d\n",a.error,b.error,a.flags,b.flags,a.rounding,b.rounding);
    require(a.error==b.error && a.flags==b.flags && a.rounding==b.rounding,"errno, FP exceptions and rounding match old path");
}
int main(int argc,char** argv) {
    require(argc==3,"setting and scenario required");
    if(strcmp(argv[1],"unset")==0) unsetenv("GXV_XFBURST"); else setenv("GXV_XFBURST",argv[1],1);
    const std::string scenario=argv[2];
    compare(run(true,1,80,0,999),run(false,1,80,0,999));
    require(parses==unsigned(burstEnabled),"real GXTransform dispatch reads once on NEON; non-NEON has no dispatch");
    if(scenario=="fast-fault") {
        injectFault=true;
        for(unsigned shape=0;shape<7;++shape) {
            Result expected=run(true,shape,80,0,999);
            const unsigned header=shape==0 ? 0 : 5;
            if(burstEnabled && (shape==1 || shape==5))
                for(unsigned i=0;i<words[shape];++i) expected.image[64+header+i*4+3]^=1;
            compare(expected,run(false,shape,80,0,999));
        }
        require(mismatches==0 && coverage==0,"fast path has no oracle accounting");
    } else if(scenario=="fault") {
        injectFault=true;
        for(unsigned i=0;i<20;++i) compare(run(true,1,80,0,999),run(false,1,80,0,999));
        require(burstEnabled ? mismatches>0 && mismatches<=4 : mismatches==0,
                "injected candidate mismatch is bounded and retains old output; excluded path has no candidate");
    } else {
        setenv("GXV_XFBURST","invalid-after-first-use",1);
        for(unsigned shape=0;shape<7;++shape) {
            for(unsigned offset=0;offset<4;++offset)
                for(int distance=-5;distance<=int(words[shape]*4+5);++distance)
                    compare(run(true,shape,distance,offset,999),run(false,shape,distance,offset,999));
            if(shape!=4) for(int alias : {-16,-4,0,4,16,32})
                compare(run(true,shape,80,shape==0 ? 0 : 3,alias),run(false,shape,80,shape==0 ? 0 : 3,alias));
            rebind=true;
            for(int distance : {-5,-4,-1,0,1,4,8,23,24,31,32,35,36,47,48,49})
                compare(run(true,shape,distance,0,999,true),run(false,shape,distance,0,999,true));
        }
        rebind=false;
        compare(run(true,1,25,0,999,true,1300),run(false,1,25,0,999,true,1300));
        require(publication>=2,"real soft-limit path produces repeated publications");
        rebind=true;
        for(unsigned shape=0;shape<7;++shape) for(int distance : {-1,0,1,23,24,47,48})
            compare(run(true,shape,distance,0,999,true,1,true),run(false,shape,distance,0,999,true,1,true));
        compare(run(true,1,20,0,999,true,1,false,true),run(false,1,20,0,999,true,1,false,true));
        compare(run(true,1,20,0,999,false,30),run(false,1,20,0,999,false,30));
        for(int mode : {FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
            roundingMode=mode;
            for(unsigned shape=0;shape<7;++shape)
                compare(run(true,shape,80,0,999),run(false,shape,80,0,999));
        }
        require(mismatches==0,"unmodified oracle has no mismatches");
    }
    const bool oracle = burstEnabled && strcmp(argv[1],"2")==0 && PORT_RUNTIME_DIAGNOSTICS && PORT_LOGGING;
    require(oracle ? coverage>0 : coverage==0,"oracle success coverage only in diagnostic mode 2; quiet mode 2 is fallback");
    require(parses==unsigned(burstEnabled),"control remains read-once or excluded after environment changes");
    printf("PASS xfburst setting=%s scenario=%s mismatches=%u\n",argv[1],argv[2],mismatches);
}
