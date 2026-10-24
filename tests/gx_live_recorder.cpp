// Actual producer/parser/presentation routing; the renderer endpoint is a probe,
// not GXM execution. Process exits match the existing desktop session lifetime.
#include "gpu.h"
#include "gx_live.h"
#include "tex_decode.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sys/wait.h>
#include <unistd.h>

using namespace gpu;
static unsigned char ram[65536];
static unsigned checks,irqs,scopeDepth;
static size_t liveBytes;
static void (*fatalCheck)();
std::atomic<uint64_t> gPresentedFrames{0};
extern "C" void port_gx_present_xfb(void*,int);
extern "C" void port_gx_recorder_end_frame();
extern "C" uint64_t port_gx_recording_frame();
extern "C" void port_gx_marker(uint32_t);
extern "C" void port_gx_camera(const float*);

static void require(bool ok,const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr,"producer FAILED: %s\n",message); _exit(1); }
}
struct alignas(std::max_align_t) Allocation { size_t bytes; };
void* operator new(size_t bytes) {
    auto* allocation=static_cast<Allocation*>(std::malloc(sizeof(Allocation)+bytes));
    if (!allocation) throw std::bad_alloc();
    allocation->bytes=bytes; liveBytes+=bytes; return allocation+1;
}
void operator delete(void* p) noexcept {
    if (!p) return;
    auto* allocation=static_cast<Allocation*>(p)-1;
    liveBytes-=allocation->bytes; std::free(allocation);
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p,size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,size_t) noexcept { ::operator delete(p); }

extern "C" {
volatile uint32_t __PortGXPERegs[0x400];
void port_log(const char*,...) {}
int port_vgx_active() { return 0; }
__attribute__((noreturn)) void port_fatal(const char* format,...) {
    std::fputs("expected producer stop: ",stderr);
    va_list args; va_start(args,format); std::vfprintf(stderr,format,args); va_end(args);
    std::fputc('\n',stderr);
    if (fatalCheck) fatalCheck();
    _exit(94);
}
void* port_resolve_gpu_address(uint32_t address) { return address<sizeof(ram) ? ram+address : nullptr; }
int port_is_native_range(const void* p,size_t bytes) {
    const uintptr_t start=reinterpret_cast<uintptr_t>(ram),value=reinterpret_cast<uintptr_t>(p);
    return value>=start && value-start<=sizeof(ram) && bytes<=sizeof(ram)-(value-start);
}
void port_irq_raise(int) { ++irqs; }
void port_host_alloc_scope_enter() { ++scopeDepth; }
void port_host_alloc_scope_leave() { require(scopeDepth,"balanced host allocation scope"); --scopeDepth; }
void __PortGXFifoFlush() {}
}

struct Probe final : RecorderConsumer {
    std::shared_ptr<const Frame> last;
    std::array<float,2> positions{};
    unsigned draws=0,copies=0,presents=0;
    bool rejectCopy=false,rejectPeek=false,rejectPresent=false;
    bool consume(std::shared_ptr<const Frame> f,const RecorderMemory&,std::string& error) override {
        require(scopeDepth && f,"producer retains its allocation scope and immutable segment owner");
        last=std::move(f);
        for (size_t i=0;i<last->cmds.size();) {
            switch (last->cmds[i]) {
            case CMD_BP: i+=2; break;
            case CMD_XF: i+=3+last->cmds[i+2]; break;
            case CMD_MARKER: i+=2; break;
            case CMD_CAMERA: i+=1+kCameraWords; break;
            case CMD_TEX: i+=3; break;
            case CMD_TEX_EFB: i+=6; break;
            case CMD_DRAW: {
                float x; std::memcpy(&x,&last->verts[last->cmds[i+3]],sizeof(x));
                positions[0]=positions[1]; positions[1]=x; ++draws; i+=5; break;
            }
            case CMD_EFB_COPY:
                ++copies;
                if (rejectCopy) { error="copy rejected by endpoint probe"; return false; }
                // Successful-copy/RAM-visible contract only, not copy-format pixels.
                std::memset(ram+32,9,9); i+=11; break;
            default: require(false,"known original recorder command");
            }
        }
        return true;
    }
    bool peek(const RecorderPeek&,uint32_t& result,std::string& error) override {
        if (rejectPeek) { error="peek rejected by endpoint probe"; return false; }
        result=0x123456; return true;
    }
    bool present(const void* xfb,bool black,std::string& error) override {
        ++presents;
        require(xfb==ram+1024 && black,"original native XFB/black request reaches endpoint");
        if (rejectPresent) { error="presentation rejected by endpoint probe"; return false; }
        return true;
    }
};
static std::shared_ptr<Probe> probe;
static void word(std::vector<unsigned char>& bytes,uint32_t value) {
    for (int shift:{24,16,8,0}) bytes.push_back(static_cast<unsigned char>(value>>shift));
}
static void processBytes(const std::vector<unsigned char>& bytes) {
    require(process(bytes.data(),bytes.size(),false)==bytes.size(),"actual parser consumes complete original FIFO stream");
}
static std::vector<unsigned char> bpBytes(uint32_t reg,uint32_t value) {
    std::vector<unsigned char> bytes{0x61}; word(bytes,reg<<24|value); return bytes;
}
static void bp(uint32_t reg,uint32_t value) { processBytes(bpBytes(reg,value)); }
static const std::vector<unsigned char> indexed{0x90,0,3,0,1,2};
static std::vector<unsigned char> call(uint32_t address,const std::vector<unsigned char>& list) {
    std::memcpy(ram+address,list.data(),list.size());
    std::vector<unsigned char> bytes{0x40}; word(bytes,address); word(bytes,uint32_t(list.size())); return bytes;
}
static void initialize(bool live=true) {
    g={}; g.cp.vcdLo=2u<<9; g.cp.vatA[0]=1; g.cp.arrayBase[0]=32; g.cp.arrayStride[0]=3;
    std::memset(ram+32,3,9);
    if (live) {
        probe=std::make_shared<Probe>(); std::string error;
        require(!attachRecorderConsumer(nullptr,error),"null consumer cannot install a placeholder renderer");
        require(attachRecorderConsumer(probe,error),"one real producer installs an owned endpoint");
        useRecorderBackend();
    } else useRecorderBackend();
}
static void copyInList() {
    initialize(); auto list=indexed; const auto copy=bpBytes(0x52,0);
    list.insert(list.end(),copy.begin(),copy.end()); list.insert(list.end(),indexed.begin(),indexed.end());
    const auto command=call(4096,list); processBytes(command);
    require(probe->positions==std::array<float,2>{3,9},"first list draw sees original RAM and post-copy draw sees committed RAM");
    processBytes(command);
    require(probe->positions==std::array<float,2>{9,9},"copy-containing list cannot reuse pre-copy words under end-of-list RAM hash");
}
static void xfbArrayCopy() {
    initialize(); const auto command=call(4096,indexed); processBytes(command);
    auto stream=command; const auto copy=bpBytes(0x52,1u<<14);
    stream.insert(stream.end(),copy.begin(),copy.end()); stream.insert(stream.end(),command.begin(),command.end());
    processBytes(stream);
    require(probe->positions==std::array<float,2>{3,9},"successful XFB copy invalidates same-stream cached array dependency");
}
static void textureTurnover() {
    initialize(); bp(0x28,64); bp(0x94,16); std::memset(ram+512,0x66,2048);
    bp(0x88,TF_I8<<20); processBytes(indexed); const auto pinned=probe->last;
    bp(0x88,1|(TF_I8<<20)); processBytes(indexed);
    std::weak_ptr<TexImage> retired=probe->last->textures[0];
    for (unsigned width=3;width<=260;++width) {
        bp(0x88,(width-1)|(TF_I8<<20)); processBytes(indexed);
    }
    require(retired.expired(),"native producer retires unused texture keys within bounded same-frame cache turnover");
    require(pinned->textures[0]->rgba[0]==0x66666666u,"eviction does not invalidate an externally retained immutable image");
}
static void listTurnover() {
    initialize(); processBytes(call(4096,indexed));
    const size_t baseline=liveBytes;
    for (unsigned i=1;i<=64;++i) processBytes(call(4096+i*32,indexed));
    const size_t small=liveBytes-baseline;
    for (unsigned i=65;i<=400;++i) processBytes(call(4096+i*32,indexed));
    require(liveBytes-baseline<small*3,"native display-list cache has a bounded footprint under same-frame scene churn");
}
static void textureBytes() {
    initialize(); bp(0x28,64); bp(0x94,16); std::memset(ram+512,0x66,40000);
    bp(0x88,447|(63u<<10)|(TF_I8<<20)); processBytes(indexed);
    std::weak_ptr<TexImage> retired=probe->last->textures[0];
    for (unsigned width=449;width<=512;++width) {
        bp(0x88,(width-1)|(63u<<10)|(TF_I8<<20)); processBytes(indexed);
    }
    require(retired.expired(),"native texture byte budget retires large CPU snapshots before the entry-count limit");
}
static void nativePresentation() {
    initialize(); port_gx_present_xfb(ram+1024,1);
    require(port_gx_recording_frame()==1 && probe->presents==1,"VI without a completed XFB cannot fabricate a new frame");
    for (uint32_t marker:{MARK_HUD_BEGIN,MARK_SKY_BEGIN,MARK_PLAYER_BEGIN,MARK_POINTER_BEGIN}) port_gx_marker(marker);
    float camera[kCameraWords]{}; camera[12]=5; port_gx_camera(camera); processBytes(indexed);
    camera[12]=9; port_gx_camera(camera); processBytes(indexed);
    uint32_t firstCamera; const float five=5; std::memcpy(&firstCamera,&five,sizeof(firstCamera));
    require(probe->last->startMarkers==15 && probe->last->startCameraValid && probe->last->startCamera[12]==firstCamera,
        "all active markers and only first camera continue between actual segments");
    for (uint32_t marker:{MARK_HUD_END,MARK_SKY_END,MARK_PLAYER_END,MARK_POINTER_END}) port_gx_marker(marker);
    processBytes(indexed); processBytes(indexed);
    require(!probe->last->startMarkers && probe->last->startCameraValid,"all marker ends preserve the first camera but clear only marker state");
    for (uint32_t marker:{MARK_HUD_BEGIN,MARK_SKY_BEGIN,MARK_PLAYER_BEGIN,MARK_POINTER_BEGIN}) port_gx_marker(marker);
    processBytes(indexed);
    bp(0x52,1u<<14); port_gx_recorder_end_frame();
    require(port_gx_recording_frame()==1,"end-frame notification cannot bypass true XFB presentation");
    port_gx_present_xfb(ram+1024,1); processBytes(indexed);
    require(port_gx_recording_frame()==2 && !probe->last->startMarkers && !probe->last->startCameraValid &&
        frameQueue().latestNumber()==0 && gPresentedFrames.load()==0,
        "actual native wrapper advances true XFB frame and resets metadata without desktop queue or fake counter");
}
static void copyFailure() {
    initialize(); probe->rejectCopy=true;
    fatalCheck=[] { require(probe->copies==1 && !probe->draws && !irqs,"rejected copy cannot reach trailing draw or PE_DONE"); };
    auto stream=bpBytes(0x52,0); stream.insert(stream.end(),indexed.begin(),indexed.end());
    const auto done=bpBytes(0x45,2); stream.insert(stream.end(),done.begin(),done.end()); processBytes(stream);
    require(false,"failed copy must stop original FIFO immediately");
}
static void peekFailure() {
    initialize(); probe->rejectPeek=true;
    fatalCheck=[] { require(!probe->draws && !irqs,"rejected peek cannot fabricate a result or continue guest work"); };
    backend()->peekColor(1,2); processBytes(indexed); require(false,"failed peek must stop caller immediately");
}
static void presentationFailure() {
    initialize(); bp(0x52,1u<<14); probe->rejectPresent=true;
    fatalCheck=[] { require(port_gx_recording_frame()==1 && probe->presents==1 && !probe->draws && !irqs,
        "rejected actual native presentation cannot advance frame or continue guest work"); };
    port_gx_present_xfb(ram+1024,1); processBytes(indexed); require(false,"failed presentation must stop caller immediately");
}
static void desktopQueue() {
    initialize(false); processBytes(indexed);
    require(frameQueue().latestNumber()==0,"desktop recorder retains unfinished frame until XFB");
    bp(0x52,1u<<14); port_gx_present_xfb(ram+1024,1);
    const auto published=frameQueue().latest();
    require(published && published->number==1 && published->draws==1 && published->xfbCopies==1 &&
        frameQueue().latestNumber()==1 && gPresentedFrames.load()==1 && port_gx_recording_frame()==2,
        "actual desktop wrapper still publishes latest frame and legacy diagnostic counter");
    std::string error;
    require(!attachRecorderConsumer(std::make_shared<Probe>(),error),"native endpoint cannot attach after completed desktop recordings");
}
static unsigned failures,scenarios;
static void isolated(const char* name,void (*scenario)(),int expected=0) {
    const pid_t child=fork(); require(child>=0,"isolated production-session scenario");
    if (!child) { scenario(); std::printf("producer %s: %u checks\n",name,checks); std::fflush(stdout); _exit(0); }
    int status=0; require(waitpid(child,&status,0)==child,"reap owned producer fixture"); ++scenarios;
    if (!WIFEXITED(status) || WEXITSTATUS(status)!=expected) {
        ++failures; std::fprintf(stderr,"producer scenario %s failed, status %d expected %d\n",name,status,expected);
    }
}
int main() {
#if defined(__vita__)
    isolated("unattached native boot",[] { useRecorderBackend(); require(false,"native boot without consumer must fail"); },94);
    isolated("copy inside display list",copyInList);
    isolated("XFB array copy",xfbArrayCopy);
    isolated("texture turnover",textureTurnover);
    isolated("texture bytes",textureBytes);
    isolated("display-list turnover",listTurnover);
    isolated("native presentation",nativePresentation);
    isolated("copy failure",copyFailure,94);
    isolated("peek failure",peekFailure,94);
    isolated("presentation failure",presentationFailure,94);
#else
    isolated("desktop queue and attachment",desktopQueue);
#endif
    std::printf("Recorder routing: %u session scenarios, %u failures; endpoint doubles, not native pixels/FPS\n",scenarios,failures);
    return failures ? 1 : 0;
}
