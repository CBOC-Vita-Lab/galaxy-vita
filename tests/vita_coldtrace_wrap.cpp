#include "vita_coldtrace_fixture.h"
#include "../platform/vgx/src/vgx_coldtrace.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace coldtrace_test {
Mode mode=Mode::Hit;
unsigned calls[Count]{};
char path[]="ux0:data/test/cache",source[]="fixture shader",buffer[16]{};
uint32_t binarySize=0;
SceGxmShaderPatcherId programId=nullptr;
SceGxmVertexProgram* vertexProgram=nullptr;
SceGxmFragmentProgram* fragmentProgram=nullptr;
std::atomic<uint64_t> now{0};
std::atomic<unsigned> clockCalls{0};
std::atomic<unsigned> threadCalls{0};
std::atomic<bool> blockRead{false},inRead{false},releaseRead{false};
std::atomic<bool> blockWait{false},inWait{false},releaseWait{false},freezeInRead{false};
thread_local uint32_t tid=1;
void Require(bool good,const char* why) {if(!good){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
void Leaf(Call call) {Require(errno==100+int(call),"the real call receives the original errno");++calls[call];now.fetch_add(10);errno=200+call;}
uint64_t Clock() noexcept {clockCalls.fetch_add(1);errno=EINTR;return now.load();}
uint32_t Thread() noexcept {threadCalls.fetch_add(1);errno=E2BIG;return tid;}
}
using namespace coldtrace_test;
extern "C" {
void port_vgx_coldtrace_runtime_link_guard_v3() {}
void vgxvita_prepareShaderCacheWrite_v1(const char*, int, SceMode) {
    Require(false,"unrelated SDK opens do not enter private shader-cache preparation");
}
int sceIoMkdir(const char* p,SceMode m) {Require(p==path && m==0777,"mkdir args unchanged");Leaf(Mkdir);return -7;}
SceUID sceIoOpen(const char* p,int f,SceMode m) {Require(p==path && f==0x723 && m==0641,"open args unchanged");Leaf(Open);return 17;}
SceOff sceIoLseek(SceUID f,SceOff o,int w) {Require(f==17 && o==0x100000007ll && w==2,"seek preserves 64-bit offset");Leaf(Seek);return -0x100000002ll;}
SceSSize sceIoRead(SceUID f,void* p,SceSize n) {
    Require(f==17 && p==buffer && n==9,"read args unchanged");Leaf(Read);
    if(blockRead.load()){inRead.store(true);while(!releaseRead.load())std::this_thread::yield();}
    if(freezeInRead.load()){vgx::coldtrace::g_recorder.RequestFreeze();Require(!vgx::coldtrace::g_recorder.Frozen().data,"owner wrapper cannot publish from inside a live item");}
    return 7;
}
int sceIoPread(SceUID f,void* p,SceSize n,SceOff o) {Require(f==17 && p==buffer && n==11 && o==0x200000009ll,"pread args and 64-bit offset unchanged");Leaf(Pread);return -19;}
SceSSize sceIoWrite(SceUID f,const void* p,SceSize n) {Require(f==17 && p==buffer && n==9,"write args unchanged");Leaf(Write);return 3;}
int sceIoClose(SceUID f) {Require(f==17,"close args unchanged");Leaf(Close);return -31;}
SceGxmProgram* shark_compile_shader_extended(const char* p,uint32_t* n,shark_type t,shark_opt o,int32_t a,int32_t b,int32_t c) {
    Require(p==source && n==&binarySize && t==SHARK_FRAGMENT_SHADER && o==SHARK_OPT_UNSAFE && a==-7 && b==5 && c==-3,"extended compiler args unchanged");
    Leaf(Shark);WriteMiss();*n=123;errno=200+Shark;return mode==Mode::Failure?nullptr:Ptr<SceGxmProgram>(0x1000);
}
SceGxmProgram* shark_compile_shader(const char* p,uint32_t* n,shark_type t) {Require(p==source && n==&binarySize && t==SHARK_VERTEX_SHADER,"simple compiler args unchanged");Leaf(SharkSimple);return Ptr<SceGxmProgram>(0x2000);}
int sceGxmShaderPatcherRegisterProgram(SceGxmShaderPatcher* p,const SceGxmProgram* g,SceGxmShaderPatcherId* out) {
    Require(p==Ptr<SceGxmShaderPatcher>(0x3000) && g==Ptr<SceGxmProgram>(0x4000) && out==&programId,"register args unchanged");Leaf(Register);*out=Ptr<SceGxmRegisteredProgram>(0x11000);return -41;
}
int sceGxmShaderPatcherCreateVertexProgram(SceGxmShaderPatcher* p,SceGxmShaderPatcherId id,const SceGxmVertexAttribute* a,unsigned na,const SceGxmVertexStream* s,unsigned ns,SceGxmVertexProgram** out) {
    Require(p==Ptr<SceGxmShaderPatcher>(0x3000) && id==programId && a==Ptr<SceGxmVertexAttribute>(0x5000) && na==3 && s==Ptr<SceGxmVertexStream>(0x6000) && ns==5 && out==&vertexProgram,"vertex args unchanged");Leaf(Vertex);*out=Ptr<SceGxmVertexProgram>(0x12000);return -43;
}
int sceGxmShaderPatcherCreateFragmentProgram(SceGxmShaderPatcher* p,SceGxmShaderPatcherId id,SceGxmOutputRegisterFormat fmt,SceGxmMultisampleMode ms,const SceGxmBlendInfo* blend,const SceGxmProgram* v,SceGxmFragmentProgram** out) {
    Require(p==Ptr<SceGxmShaderPatcher>(0x3000) && id==programId && fmt==SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4 && ms==SCE_GXM_MULTISAMPLE_4X && blend==Ptr<SceGxmBlendInfo>(0x7000) && v==Ptr<SceGxmProgram>(0x4000) && out==&fragmentProgram,"fragment args unchanged");Leaf(Fragment);*out=Ptr<SceGxmFragmentProgram>(0x13000);return -47;
}
int sceGxmBeginScene(SceGxmContext* c,unsigned flags,const SceGxmRenderTarget* r,const SceGxmValidRegion* v,SceGxmSyncObject* vs,SceGxmSyncObject* fs,const SceGxmColorSurface* color,const SceGxmDepthStencilSurface* depth) {
    Require(c==Ptr<SceGxmContext>(0x8000) && flags==13 && r==Ptr<SceGxmRenderTarget>(0x9000) && v==Ptr<SceGxmValidRegion>(0xa000) && vs==Ptr<SceGxmSyncObject>(0xb000) && fs==Ptr<SceGxmSyncObject>(0xc000) && color==Ptr<SceGxmColorSurface>(0xd000) && depth==Ptr<SceGxmDepthStencilSurface>(0xe000),"begin args unchanged");Leaf(Begin);return -53;
}
int sceGxmEndScene(SceGxmContext* c,const SceGxmNotification* v,const SceGxmNotification* f) {Require(c==Ptr<SceGxmContext>(0x8000) && v==Ptr<SceGxmNotification>(0xf000) && f==Ptr<SceGxmNotification>(0x10000),"end args unchanged");Leaf(End);return -59;}
void sceGxmFinish(SceGxmContext* c) {Require(c==Ptr<SceGxmContext>(0x8000),"finish args unchanged");Leaf(Finish);}
int sceGxmTransferFinish(void) {Leaf(TransferFinish);return -67;}
int sceGxmNotificationWait(const SceGxmNotification* p) {
    Require(p==Ptr<SceGxmNotification>(0xf000),"notification args unchanged");Leaf(Wait);
    if(blockWait.load()){inWait.store(true);while(!releaseWait.load())std::this_thread::yield();}
    return -61;
}
}

int main() {
    using namespace vgx::coldtrace;
    constexpr size_t seek=static_cast<size_t>(Phase::Seek),read=static_cast<size_t>(Phase::Read),write=static_cast<size_t>(Phase::Write),shark=static_cast<size_t>(Phase::Shark);
    for(const auto m:{Mode::Hit,Mode::Miss,Mode::Failure}) {
        mode=m;now.store(0);std::fill(std::begin(calls),std::end(calls),0);Require(g_recorder.Arm(Clock,Thread,1),"arm actual linker wrappers");
        {Scope root(g_recorder,Key{1,2,3,4,5,6});RunDriver();}
        g_recorder.RequestFreeze();auto view=g_recorder.Frozen();Require(view.data && view.data->worstCount==1,"one finished driver item");
        const auto& item=view.data->worst[0];uint64_t sum=item.selfUs;for(auto n:item.exclusiveUs)sum+=n;
        Require(sum==item.totalUs && item.totalUs==(m==Mode::Hit?140u:170u),"real nested wrappers exactly partition scripted wall time");
        Require(item.calls[read]==2 && item.bytes[read]==7 && item.requested[read]==20 && item.shortResults[read]==1 && item.failures[read]==1,"actual read/pread wrappers report shorts, bytes and failures");
        Require(item.firstError[seek]==-0x100000002ll,"actual wrapper retains signed 64-bit error");
        Require(item.calls[shark]==(m==Mode::Hit?0u:2u) && item.failures[shark]==(m==Mode::Failure?1u:0u),"actual compiler calls distinguish hit, miss and failure");
        Require(item.exclusiveUs[shark]==(m==Mode::Hit?0u:20u) && item.inclusiveUs[shark]==(m==Mode::Hit?0u:30u) && item.bytes[write]==(m==Mode::Hit?0u:3u),"nested cache writes are not counted twice as compiler self");
        for(const auto phase:{Phase::Mkdir,Phase::Open,Phase::Seek,Phase::Close,Phase::Register,Phase::PatchVertex,Phase::PatchFragment,Phase::BeginScene,Phase::EndScene,Phase::Finish,Phase::Notification,Phase::TransferFinish}) {
            const auto p=static_cast<size_t>(phase);
            Require(item.calls[p]==1 && item.inclusiveUs[p]==10 && item.exclusiveUs[p]==10,"every SDK leaf has its own phase with exact wall time");
        }
        Require(vertexProgram==Ptr<SceGxmVertexProgram>(0x12000) && fragmentProgram==Ptr<SceGxmFragmentProgram>(0x13000),"output pointers survive wrapper return unchanged");
        for(unsigned i=0;i<Count;++i)Require(calls[i]==((i==Write || i==Shark || i==SharkSimple)?(m==Mode::Hit?0u:1u):1u),"each real SDK call executes exactly once");
    }
    now.store(0);Require(g_recorder.Arm(Clock,Thread,1),"arm blocked-worker wrapper freeze");
    blockRead.store(true);std::thread worker([]{tid=2;ReadWorker();});
    while(!inRead.load())std::this_thread::yield();g_recorder.RequestFreeze();Require(!g_recorder.Frozen().data,"actual wrapper permit blocks report until real read completes");
    releaseRead.store(true);worker.join();const auto view=g_recorder.Frozen();
    Require(view.data && view.ioCount==1 && view.data->io[0].resultNoted && view.data->io[0].result==7 && view.data->io[0].kind==IoClass::Read,"actual worker read publishes full result after drain");
    blockRead.store(false);mode=Mode::Hit;const auto before=clockCalls.load(),threadsBefore=threadCalls.load();RunDriver();
    Require(clockCalls.load()==before && threadCalls.load()==threadsBefore,"closed wrappers pass through without clock or thread callbacks");
    Require(g_recorder.Arm(Clock,Thread,1),"arm deliberately uncovered owner driver");const auto outsideClock=clockCalls.load();RunDriver();g_recorder.RequestFreeze();
    const auto outside=g_recorder.Frozen();
    Require(outside.data && outside.data->itemsSeen==14 && outside.data->phasesOutsideItems==14 && clockCalls.load()==outsideClock+28,"owner calls outside explicit items retain independent SDK-root timings");
    Require(g_recorder.Arm(Clock,Thread,1),"arm worker I/O class table");mode=Mode::Miss;
    std::thread ioWorker([]{tid=2;RunDriver();});ioWorker.join();g_recorder.RequestFreeze();const auto ioView=g_recorder.Frozen();
    Require(ioView.data && ioView.ioCount==7 && ioView.foreignDropped==10 && ioView.ioDropped==0,"all worker I/O wrappers are concrete classes; non-I/O is explicitly foreign");
    unsigned meta=0,reads=0,writes=0;bool seekError=false;
    for(unsigned i=0;i<ioView.ioCount;++i){const auto& io=ioView.data->io[i];Require(io.resultNoted && io.thread==2,"worker interval has original thread and exact result");
        if(io.kind==IoClass::Meta){++meta;Require(io.requested==0,"metadata has no requested byte count");seekError|=io.result==-0x100000002ll;}
        if(io.kind==IoClass::Read){++reads;Require(io.requested==(io.result==7?9u:11u),"read/pread exact requested count");}
        if(io.kind==IoClass::Write){++writes;Require(io.requested==9 && io.result==3,"short worker write exact result");}}
    Require(meta==4 && reads==2 && writes==1 && seekError,"worker metadata/read/write table covers every I/O wrapper without narrowing");
    Require(g_recorder.Arm(Clock,Thread,1),"arm owner wrapper freeze");freezeInRead.store(true);
    {Scope root(g_recorder,Key{});ReadWorker();}freezeInRead.store(false);const auto ownerView=g_recorder.Frozen();
    Require(ownerView.data && ownerView.data->worstCount==1 && (ownerView.data->worst[0].flags&kFrozenMidItem),"owner-side freeze is flagged and publishes only after full unwind");
    uint64_t ownerSum=ownerView.data->worst[0].selfUs;for(auto n:ownerView.data->worst[0].exclusiveUs)ownerSum+=n;
    Require(ownerSum==ownerView.data->worst[0].totalUs,"owner freeze retains an exact wall partition");
    Require(g_recorder.Arm(Clock,Thread,1),"arm foreign wait freeze");blockWait.store(true);
    std::thread waiter([]{tid=2;WaitWorker();});while(!inWait.load())std::this_thread::yield();g_recorder.RequestFreeze();
    const auto foreignView=g_recorder.Frozen();Require(foreignView.data && foreignView.foreignDropped==1 && foreignView.ioDropped==0 && g_recorder.PendingPermits()==0,"foreign non-I/O wait cannot hold a recorder permit while blocked");
    releaseWait.store(true);waiter.join();
    std::puts("PASS: actual SDK-typed link wrappers preserve args/results/errno, nested hit/miss/error accounting and worker freeze");
}
