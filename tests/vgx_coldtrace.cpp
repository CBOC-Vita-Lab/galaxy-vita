// Data-only tracing tests. Synthetic times cannot identify a physical Vita stall.
#include <cstdio>
#if __has_include("../platform/vgx/src/vgx_coldtrace.h")
#include "../platform/vgx/src/vgx_coldtrace.h"
#include <atomic>
#include <array>
#include <cstdlib>
#include <new>
#include <optional>
#include <thread>

namespace {
std::atomic<uint64_t> testTime{0};
std::atomic<uint32_t> clockCalls{0};
thread_local uint32_t testThread=1;
std::atomic<bool> countNew{false};
std::atomic<unsigned> newCalls{0};
uint64_t Clock() noexcept { clockCalls.fetch_add(1,std::memory_order_relaxed);return testTime.load(std::memory_order_relaxed); }
uint32_t Thread() noexcept { return testThread; }
void Time(uint64_t value) { testTime.store(value,std::memory_order_relaxed); }
void Require(bool ok,const char* why) { if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);} }
using namespace vgx::coldtrace;
constexpr size_t Id(Phase p) { return static_cast<size_t>(p); }
void Partition(const Item& item) {
    uint64_t sum=item.selfUs;
    for(uint64_t n:item.exclusiveUs)sum+=n;
    Require(sum==item.totalUs,"exclusive phases plus item self exactly partition wall time");
}
const Item& Only(const FrozenView& view) {
    Require(view.data && view.data->worstCount==1,"one immutable item is available after drain");
    Partition(view.data->worst[0]); return view.data->worst[0];
}
void Nested() {
    Recorder r; Time(0); Require(r.Arm(Clock,Thread,1),"arm from fully frozen state");
    {
        Scope item(r,Key{7,1,12,34,0,0});
        Time(5000);
        { Scope compile(r,Phase::Compile);
          Time(15000); { Scope read(r,Phase::Read,IoClass::Read); read.Result(64,64); Time(65000); }
          { Scope shark(r,Phase::Shark); Time(165000); } Time(175000); }
        Time(180000); { Scope wait(r,Phase::Finish); Time(200000); } Time(208000);
    }
    r.RequestFreeze(); const auto view=r.Frozen(); const auto& item=Only(view);
    Require(item.totalUs==208000 && item.selfUs==18000,"fake208ms item retains exact root wall/self time");
    Require(item.inclusiveUs[Id(Phase::Compile)]==170000 && item.exclusiveUs[Id(Phase::Compile)]==20000,"compile includes children only in inclusive time");
    Require(item.exclusiveUs[Id(Phase::Read)]==50000 && item.exclusiveUs[Id(Phase::Shark)]==100000,"I/O and actual compiler time remain distinct");
    Require(item.bytes[Id(Phase::Read)]==64 && item.calls[Id(Phase::Read)]==1,"leaf results retain byte and call counts");
    Require(!Overlap(view,item,IoClass::Any).complete,"an unsynchronized arm cannot exclude worker I/O that was already running");
    std::puts("PASS: coldtrace nested208ms partition, leaf results and immutable freeze");
}
void RecursionAndErrors() {
    Recorder r; Time(0); Require(r.Arm(Clock,Thread,1),"arm recursion");
    { Scope item(r,Key{}); Scope outer(r,Phase::Compile); Time(10);
      { Scope inner(r,Phase::Compile); Time(20); } Time(30); }
    r.RequestFreeze(); const auto& recursive=Only(r.Frozen());
    Require(recursive.inclusiveUs[Id(Phase::Compile)]==30 && recursive.exclusiveUs[Id(Phase::Compile)]==30,"same-phase recursion is not double-counted");
    Require(r.Arm(Clock,Thread,1),"rearm only after old report consumer is finished");
    { Scope item(r,Key{}); std::array<std::optional<Scope>,kMaxDepth+2> scopes;
      for(auto& s:scopes)s.emplace(r,Phase::Read);
      Time(40); for(size_t i=scopes.size();i>0;--i)scopes[i-1].reset(); }
    r.RequestFreeze(); auto view=r.Frozen(); Partition(Only(view));
    Require(view.data->depthOverflows==2,"depth overflow never corrupts the real stack");
    Require(r.Arm(Clock,Thread,1),"rearm for backwards clock"); Time(100);
    { Scope item(r,Key{}); Time(110); { Scope phase(r,Phase::Link); Time(90); } Time(120); }
    r.RequestFreeze(); view=r.Frozen(); Require(view.clockErrors==1,"backwards timestamps are counted and clamped"); Partition(Only(view));
    std::puts("PASS: coldtrace recursive phases, bounded depth and backwards clock");
}
void LifetimeAndFreeze() {
    Recorder r; Time(0); Require(r.Arm(Clock,Thread,1),"arm lifetime");
    std::optional<Scope> root,child; root.emplace(r,Key{}); child.emplace(r,Phase::Compile);
    Time(10); root.reset(); r.RequestFreeze();
    Require(!r.Frozen().data,"an outstanding child permit prevents publication even after root closes");
    child.reset(); const auto view=r.Frozen(); Partition(Only(view));
    Require(view.data->unbalancedScopes>=1,"misordered lifetimes are reported, not silently trusted");
    Require(r.Arm(Clock,Thread,1),"arm active worker freeze");
    std::atomic<bool> entered{false},release{false};
    std::thread worker([&]{testThread=2;Scope io(r,Phase::Read,IoClass::Read);entered.store(true,std::memory_order_release);
        while(!release.load(std::memory_order_acquire))std::this_thread::yield();Time(200);});
    while(!entered.load(std::memory_order_acquire))std::this_thread::yield();
    root.emplace(r,Key{}); r.RequestFreeze(); const auto calls=clockCalls.load();
    { Scope rejected(r,Phase::Shark); rejected.Result(0); }
    Require(clockCalls.load()==calls,"closed admissions do not call clocks or inspect mutable trace state");
    Require(!r.Arm(Clock,Thread,1),"rearm cannot reset a still-draining capture");
    root.reset(); Require(!r.Frozen().data,"freezing the render side alone does not publish active worker data");
    release.store(true,std::memory_order_release);worker.join();
    const auto frozen=r.Frozen();Require(frozen.data && frozen.ioCount==1,"publication waits for admitted worker I/O to finish");
    Require(Only(frozen).flags&kFrozenMidItem,"mid-item freeze explicitly marks incomplete leaf admission");
    Require(!Overlap(frozen,Only(frozen),IoClass::Any).complete,"mid-item freeze cannot claim complete overlap after rejecting new worker I/O");
    std::puts("PASS: coldtrace freeze closes admission and drains render plus worker permits");
}
void RetentionAndOverlap() {
    Recorder r; Time(0); Require(r.Arm(Clock,Thread,1),"arm retention");
    for(unsigned i=0;i<kWorstItems+150;++i){Time(uint64_t(i)*300000);Scope item(r,Key{0,0,i,0,0,0});Time(uint64_t(i)*300000+(i==0?208000:1000+i));}
    r.RequestFreeze(); auto view=r.Frozen();bool kept=false;
    for(size_t i=0;i<view.data->worstCount;++i){Partition(view.data->worst[i]);kept|=view.data->worst[i].key.a==0;}
    Require(kept && view.data->itemsSeen==kWorstItems+150 && view.data->worstCount==kWorstItems,"worst retention keeps an old208ms item through many later items");
    Time(0);Require(r.Arm(Clock,Thread,1,true),"arm I/O overlap from a known quiescent test boundary");Time(140);std::optional<Scope> item;item.emplace(r,Key{});
    testThread=2;Time(100);std::optional<Scope> first,second,write;
    first.emplace(r,Phase::Read,IoClass::Read);Time(150);second.emplace(r,Phase::Read,IoClass::Read);
    Time(200);first.reset();Time(220);write.emplace(r,Phase::Write,IoClass::Write);Time(250);second.reset();Time(260);write.reset();
    testThread=1;Time(240);item.reset();r.RequestFreeze();view=r.Frozen();const auto& root=Only(view);
    Require(Overlap(view,root,IoClass::Read).us==100 && Overlap(view,root,IoClass::Write).us==20,"partial and nested I/O intervals use a clipped union, not summed durations");
    Require(Overlap(view,root,IoClass::Any).us==100,"read/write overlaps are not added twice");
    Require(Overlap(view,root,IoClass::Any).complete,"known-start capture with no drops or mid-item freeze has complete overlap");
    Time(0);Require(r.Arm(Clock,Thread,1),"arm bounded I/O");testThread=2;
    for(unsigned i=0;i<kIoIntervals+7;++i){Time(i*2);Scope io(r,Phase::Read,IoClass::Read);Time(i*2+1);}
    testThread=1;Time(0);{Scope rootScope(r,Key{});Time(10000);}r.RequestFreeze();view=r.Frozen();
    Require(view.ioCount==kIoIntervals && view.ioDropped==7,"full I/O storage drops explicitly without overwriting earlier intervals");
    Require(!Overlap(view,Only(view),IoClass::Read).complete,"overflow prevents claiming that observed overlap is complete");
    std::puts("PASS: coldtrace worst-item retention, interval union and explicit overflow");
}
void Property() {
    Recorder r;uint32_t random=1234567;
    for(unsigned batch=0;batch<20;++batch){Time(0);Require(r.Arm(Clock,Thread,1),"arm partition property batch");
        std::array<std::array<uint64_t,kPhaseCount>,kWorstItems> exclusive{},inclusive{};
        std::array<uint64_t,kWorstItems> self{};
        for(unsigned n=0;n<kWorstItems;++n){Scope root(r,Key{0,0,n,0,0,0});std::array<std::optional<Scope>,kMaxDepth> stack;std::array<Phase,kMaxDepth> phases{};unsigned depth=0;
            for(unsigned step=0;step<80;++step){random=random*1664525u+1013904223u;
                if((random&3)==0 && depth){stack[--depth].reset();}
                else if((random&3)==1 && depth<kMaxDepth){phases[depth]=static_cast<Phase>((random>>8)%kPhaseCount);stack[depth].emplace(r,phases[depth]);++depth;}
                const uint64_t dt=(random>>16)%100;Time(Clock()+dt);
                if(depth)exclusive[n][Id(phases[depth-1])]+=dt;else self[n]+=dt;
                for(unsigned p=0;p<kPhaseCount;++p){bool present=false;for(unsigned d=0;d<depth;++d)present|=Id(phases[d])==p;if(present)inclusive[n][p]+=dt;}
            }
            while(depth)stack[--depth].reset();
        }
        r.RequestFreeze();const auto view=r.Frozen();Require(view.data->worstCount==kWorstItems,"every property item is retained in its bounded batch");
        for(const auto& item:view.data->worst){Partition(item);Require(item.exclusiveUs==exclusive[item.key.a] && item.inclusiveUs==inclusive[item.key.a] && item.selfUs==self[item.key.a],"random nested phases match an independent per-tick oracle");}
    }
    std::puts("PASS: coldtrace1280random item partitions match an independent interval oracle");
}
void MisclassifiedWorkerIo() {
    Recorder r;
    for(const auto kind:{IoClass::None,IoClass::Any}) {
        Time(0);Require(r.Arm(Clock,Thread,1,true),"arm known-start worker I/O classification test");
        {Scope item(r,Key{});Time(10);testThread=2;
         {Scope io(r,Phase::Read,kind);io.Result(4,4);Time(20);}
         testThread=1;Time(30);}
        r.RequestFreeze();const auto view=r.Frozen();
        Require(!Overlap(view,Only(view),IoClass::Any).complete,"misclassified worker I/O cannot claim complete overlap");
        Require(view.ioDropped==1 && view.foreignDropped==1,"misclassified worker I/O is an explicit coverage drop");
    }
    std::puts("PASS: misclassified worker I/O invalidates overlap coverage");
}
void NoAllocAndConcurrency() {
    Recorder r;Time(0);Require(r.Arm(Clock,Thread,1),"arm no-allocation capture");
    newCalls.store(0);countNew.store(true);
    for(unsigned i=0;i<1000;++i){SpanScope span(r,SpanKind::Execute,i);Scope item(r,Key{});Scope io(r,Phase::Write,IoClass::Write);io.Result(i&1?-5:2,4);Time(i+1);}
    r.RequestFreeze();const auto view=r.Frozen();
    for(size_t i=0;i<view.data->worstCount;++i){const auto& item=view.data->worst[i];Partition(item);Require(item.calls[Id(Phase::Write)]==1 && item.requested[Id(Phase::Write)]==4 && item.failures[Id(Phase::Write)]+item.shortResults[Id(Phase::Write)]==1,"failed and short I/O results remain explicit");}
    countNew.store(false);Require(newCalls.load()==0,"recording and freeze allocate no heap memory");
    Require(r.Arm(Clock,Thread,1),"arm concurrent admissions");std::atomic<bool> go{false},published{false};std::atomic<unsigned> ready{0};std::array<std::thread,3> workers;
    for(unsigned n=0;n<workers.size();++n)workers[n]=std::thread([&,n]{testThread=n+2;
        {Scope io(r,Phase::Read,IoClass::Read);ready.fetch_add(1,std::memory_order_release);while(!go.load(std::memory_order_acquire))std::this_thread::yield();}
        for(unsigned i=1;i<12000;++i){Scope io(r,Phase::Read,IoClass::Read);}});
    while(ready.load(std::memory_order_acquire)!=3)std::this_thread::yield();
    std::thread consumer([&]{FrozenView snapshot;while(!(snapshot=r.Frozen()).data)std::this_thread::yield();
        Require(snapshot.ioCount+snapshot.ioDropped<=36000 && snapshot.ioCount>=3,"published I/O counts are bounded by admitted worker calls");
        for(uint32_t i=0;i<snapshot.ioCount;++i){const auto& io=snapshot.data->io[i];Require(io.kind==IoClass::Read && io.thread>=2 && io.thread<=4 && io.endUs>=io.startUs && !io.resultNoted,"consumer sees complete uniquely owned slots before joining writers, not default EOF results");}
        for(size_t i=0;i<snapshot.data->worstCount;++i)Partition(snapshot.data->worst[i]);published.store(true,std::memory_order_release);});
    go.store(true,std::memory_order_release);
    for(unsigned i=0;i<12000;++i){Scope root(r,Key{});Scope phase(r,Phase::Link);if(i==6000)r.RequestFreeze();}
    for(auto& worker:workers)worker.join();consumer.join();r.RequestFreeze();Require(r.Frozen().data && published.load(std::memory_order_acquire),"concurrent admitted writers all drain before report publication");
    std::puts("PASS: coldtrace zero allocations and concurrent admission/freeze stress (not a race proof alone)");
}
Recorder* freezeDuringArm=nullptr;
uint64_t ArmClock() noexcept { if(freezeDuringArm){freezeDuringArm->RequestFreeze();freezeDuringArm=nullptr;}return Clock(); }
void DetailedMetadata() {
#if defined(VGX_COLDTRACE_DETAIL_VERSION) && VGX_COLDTRACE_DETAIL_VERSION>=2
    Recorder r;Time(0);freezeDuringArm=&r;
    Require(!r.Arm(ArmClock,Thread,1) && !r.Frozen().data,"freeze during initialization aborts rather than publishing a fake empty capture");
    Time(0);Require(r.Arm(Clock,Thread,1),"arm detailed metadata");
    {Scope omitted(r,Phase::Draw);}
    {Scope item(r,Key{4,12,7,8,0,0});
     r.MarkShader(100,1,0xabcdef0011223344ull);r.MarkShader(101,2,0x123456789abcdef0ull);r.MarkShader(100,1,0xabcdef0011223344ull);
     {Scope inherited(r,Key{4,1,9,0,0,0});Scope io(r,Phase::Open);io.Result(-0x100000002ll);}
     testThread=2;{Scope foreign(r,Phase::Shark);}testThread=1;Time(100);}
    r.RequestFreeze();const auto view=r.Frozen();
    Require(view.data && view.data->worstCount==2,"outside SDK phase and explicit item both retained");
    const auto& item=view.data->worst[1];Partition(item);
    Require(item.firstError[Id(Phase::Open)]==-0x100000002ll,"negative64bit results are not narrowed or lost");
    Require(item.shaderCount==2 && item.shaders[0].sourceHash==0xabcdef0011223344ull && item.shaders[1].kind==2,"exact cache-source hashes are retained without text or duplicate annotations");
    Require(view.data->inheritedItems==1 && view.data->phasesOutsideItems==1 && view.foreignDropped==1,"nested item requests and untracked phases are explicit, not silent absence");
    Require(view.data->worst[0].origin==Origin::Sdk && view.ioCount==0,"standalone SDK phase has its own origin and owner I/O does not become a worker interval");
    Recorder bad;Time(0);Require(bad.Arm(Clock,Thread,1),"arm misuse detector");std::optional<Scope> misplaced;misplaced.emplace(bad,Key{});
    std::thread wrong([&]{testThread=2;misplaced.reset();});wrong.join();bad.RequestFreeze();
    Require(!bad.Frozen().data && bad.PendingPermits()==1 && bad.ScopeMisuses()==1,"cross-thread scope destruction fails closed instead of racing or publishing partial data");
    std::puts("PASS: coldtrace initialization freeze, exact errors/source identity, coverage counters and misuse guard");
#else
    Require(false,"coldtrace lacks explicit source/error/coverage metadata and initialization-abort handling");
#endif
}
}
void* operator new(std::size_t n){if(countNew.load())newCalls.fetch_add(1);if(void* p=std::malloc(n))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
int main(){Nested();RecursionAndErrors();LifetimeAndFreeze();RetentionAndOverlap();Property();NoAllocAndConcurrency();DetailedMetadata();MisclassifiedWorkerIo();}
#else
int main(){std::fputs("FAIL: real RAM-only coldtrace collector is not implemented\n",stderr);return 1;}
#endif
