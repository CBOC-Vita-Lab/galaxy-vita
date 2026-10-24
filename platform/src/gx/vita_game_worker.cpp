#include "vita_game_worker.h"
#include "vita_frame.h"
#include "vita_efb.h"
#include "vita_xfb.h"
#include "port/port.h"
#include "port/heap_routing.h"
#include <condition_variable>
#include "port/game_progress.h"
#include <mutex>
#include <thread>
#include <system_error>
#include <cstdio>

namespace gpu {
namespace {
constexpr int invalid=-1;
thread_local const void* activeWorker=nullptr;
bool ramWindow(uint32_t address,GxTextureMemory& memory,std::string& error) {
    void* data=nullptr; uint32_t base=0; size_t bytes=0;
    if (!port_physical_window(address,&data,&base,&bytes)) {
        error="original GX physical address has no registered native RAM region"; return false;
    }
    memory.ram=static_cast<const unsigned char*>(data); memory.ramBase=base; memory.ramBytes=bytes;
    return true;
}

class GameRenderer final : public VitaFrameBackend {
public:
    explicit GameRenderer(VitaShaderLibrary value) : library(std::move(value)) {}
    bool open(std::string& error) {
        int result=port_vita_gxm_device_create(&device);
        if (result>=0) result=createVitaFrameReplay(device,&replay);
        if (result>=0) result=createVitaMaterialCache(device,&materials);
        if (result>=0) result=createVitaTextureCache(device,&textures);
        if (result>=0) result=createVitaEfbCache(device,library,&efbs,error);
        if (result>=0) result=createVitaXfbDisplay(device,library,&display,error);
        if (result>=0) result=createGxCopyHistory(&history);
#if defined(__vita__)
        if (result>=0) {
            FILE* marker=std::fopen("ux0:data/codex-galaxy/diagnostics/shader-inventory","rb");
            configureVitaShaderCapture(marker,shaderCapture);
            if (marker) std::fclose(marker);
        }
#endif
        if (result<0 && error.empty()) error="native game renderer initialization failed: "+std::to_string(result);
        return result>=0;
    }
    bool consume(std::shared_ptr<const Frame> frame,const RecorderMemory& incoming,std::string& error) {
        std::shared_ptr<const DrawSpans> spans; DrawSpanError spanError;
        if (!buildDrawSpans(std::move(frame),spans,spanError,dimensions)) {
            error="GX command "+std::to_string(spanError.commandWord)+": "+spanError.message; return false;
        }
        std::shared_ptr<const IndexChunks> chunks; IndexChunkError chunkError;
        if (!buildIndexChunks(spans,chunks,chunkError)) { error=chunkError.message; return false; }
        // The only borrowed live window is valid while the guest retains its CPU.
        memory=incoming;
        VitaFrameError failure;
        bool ok=false;
#if defined(__cpp_exceptions)
        try { ok=submitVitaFrame(replay,std::move(chunks),*this,failure); }
        catch (...) { memory={}; throw; }
#else
        ok=submitVitaFrame(replay,std::move(chunks),*this,failure);
#endif
        memory={};
        if (!ok) {
#if defined(__vita__)
            diagnoseShaders(spans,failure);
#endif
            error="GX frame "+std::to_string(spans->frame().number)+", command "+
                std::to_string(failure.commandWord)+": "+failure.message;
            return false;
        }
        dimensions={spans->efbWidth(),spans->efbHeight()};
        const uint64_t frameNumber=spans->frame().number;
        spans.reset();
        return collect(frameNumber,error);
    }
    bool peek(const RecorderPeek& request,uint32_t& value,std::string& error) {
        if (!target || !targetLease) {
            error="original native GX EFB peek requires a rendered source target"; return false;
        }
        return peekVitaEfb(efbs,target,request,value,error)>=0;
    }
    bool present(const VitaXfbPresent& request,std::string& error) {
        GxTextureMemory ram{};
        if (!request.black && request.hasFramebuffer && !ramWindow(request.address,ram,error)) return false;
        return presentVitaXfb(display,request,ram,error)>=0;
    }
    bool close(std::string& error) {
        auto check=[&](int result,const char* phase) {
            if (result<0) error=std::string(phase)+": "+std::to_string(result);
            return result>=0;
        };
        if (!discarded) {
            // End the actual scene owner before unrelated staging resets. Once
            // release starts, a partial closing owner cannot be discarded again.
            if (display && vitaXfbSceneOpen(display) && !check(discardVitaXfb(display),"game VI scene discard")) return false;
            if (efbs && vitaEfbSceneOpen(efbs) && !check(discardVitaEfbClear(efbs),"game EFB scene discard")) return false;
            if (replay && !check(discardVitaFrame(replay),"game replay discard")) return false;
            if (efbs && !check(discardVitaEfbClear(efbs),"game EFB discard")) return false;
            discarded=true;
        }
        targetLease.reset(); target=nullptr; packet.reset(); memory={};
        if (display && !check(releaseVitaXfbDisplay(&display),"game VI retirement")) return false;
        if (replay && !check(releaseVitaFrameReplay(&replay),"game replay retirement")) return false;
        if (textures && !check(releaseVitaTextureCache(&textures),"game texture retirement")) return false;
        if (materials && !check(releaseVitaMaterialCache(&materials),"game material retirement")) return false;
        if (history && !check(releaseGxCopyHistory(&history),"game copy history retirement")) return false;
        if (efbs && !check(releaseVitaEfbCache(&efbs),"game EFB retirement")) return false;
        if (device && !check(port_vita_gxm_device_release(&device),"game GXM retirement")) return false;
        error.clear(); return true;
    }
    int resolve(const IndexChunks& source,const IndexChunk& chunk,const VitaPreparedDraw& prepared,
                VitaDrawBinding& output,std::string& error) override {
        VitaRamTextures ram{history,{nullptr,0,0,memory.tlut,0,memory.tlutBytes,
            source.frame().number,copySequence},ramWindow};
        return resolveVitaDrawWithRam(device,materials,textures,library,source,chunk,prepared,ram,output,error);
    }
    int surface(uint32_t width,uint32_t height,PortVitaGxmSurface*& output,
                std::shared_ptr<const void>& lease,std::string& error) override {
        const int result=resolveVitaEfb(efbs,width,height,output,lease,error);
        if (result>=0) { target=output; targetLease=lease; }
        return result;
    }
    int copy(const Frame& frame,const SpanCopy& copy,std::string& error) override {
        // A copy can precede the first draw in this segment (or the session).
        PortVitaGxmSurface* source=nullptr; std::shared_ptr<const void> sourceLease;
        int result=resolveVitaEfb(efbs,copy.efbWidth,copy.efbHeight,source,sourceLease,error);
        if (result<0) return result;
        target=source; targetLease=std::move(sourceLease);
        void* data=nullptr; uint32_t base=0; size_t bytes=0;
        const uint64_t address=uint64_t(copy.words[3])<<5;
        if (address>UINT32_MAX || !port_physical_window(uint32_t(address),&data,&base,&bytes)) {
            error="original EFB copy destination has no registered native RAM region"; return invalid;
        }
        packet.reset();
        result=copyVitaEfbToMemory(efbs,history,target,frame.number,copy,
            {static_cast<unsigned char*>(data),base,bytes},packet,error);
        // Even a failed post-copy clear may have a durable RAM/history commit.
        if (packet) copySequence=packet->sequence();
        return result;
    }
    int snapshot(PortVitaGxmSurface* source,uint32_t,uint32_t,
                 std::shared_ptr<const VitaEfbBlendSnapshot>& out,std::string& error) override {
        return resolveVitaEfbBlendSnapshot(efbs,source,out,error);
    }
    int marker(const Frame&,const SpanMarker&,std::string&) override { return 0; }
    int camera(const Frame&,const SpanCamera&,std::string&) override { return 0; }
private:
#if defined(__vita__)
    void diagnoseShaders(const std::shared_ptr<const DrawSpans>& spans,const VitaFrameError& failure) {
        if (!shaderCapture.tag[0] || shaderCapture.attempted ||
            (failure.message.find("missing exact full-precision CPU-XF/raster-aware TEV compiler artifact (uid-bytes=")!=0 &&
             failure.message.find("missing exact full-precision CPU-XF/snapshot-aware TEV compiler artifact (uid-bytes=")!=0)) return;
        bool saved=false; VitaShaderInventory inventory;
#if defined(__cpp_exceptions)
        try {
#endif
            // Rebuild metadata only after terminal shader failure; keep the original error and owner.
            std::shared_ptr<const IndexChunks> diagnostic; IndexChunkError error;
            if (buildIndexChunks(spans,diagnostic,error)) {
                inventoryVitaFrameShaders(*diagnostic,library,inventory);
                saved=captureVitaShaderInventory("ux0:data/codex-galaxy/diagnostics",spans->frame().number,
                    inventory,shaderCapture)>0;
            }
#if defined(__cpp_exceptions)
        } catch (...) { saved=false; }
#endif
        shaderCapture.attempted=true;
        std::fprintf(stderr,"native-shader-inventory tag=%s saved=%u complete=%u requests=%llu chunks=%llu rejected=%llu\n",
            shaderCapture.tag,unsigned(saved),unsigned(saved && inventory.complete),
            static_cast<unsigned long long>(inventory.requests.size()),static_cast<unsigned long long>(inventory.inspectedChunks),
            static_cast<unsigned long long>(inventory.rejectedChunks));
        std::fflush(stderr);
    }
    VitaShaderCapture shaderCapture;
#endif
    bool collect(uint64_t frame,std::string& error) {
        packet.reset();
        int result=collectVitaTextures(textures);
        if (result>=0) result=collectVitaMaterials(materials);
        if (result>=0) result=collectGxCopyBytes(history,frame);
        if (result>=0) result=collectVitaEfbs(efbs);
        if (result<0) error="native game resource collection failed: "+std::to_string(result);
        return result>=0;
    }
    PortVitaGxmDevice* device=nullptr;
    VitaFrameReplay* replay=nullptr;
    VitaMaterialCache* materials=nullptr;
    VitaTextureCache* textures=nullptr;
    VitaEfbCache* efbs=nullptr;
    VitaXfbDisplay* display=nullptr;
    GxCopyHistory* history=nullptr;
    PortVitaGxmSurface* target=nullptr;
    std::shared_ptr<const void> targetLease;
    std::shared_ptr<const GxCopyBytes> packet;
    uint64_t copySequence=0;
    VitaShaderLibrary library;
    RecorderMemory memory;
    DrawSpanBuildOptions dimensions;
    bool discarded=false;
};
}

struct VitaGameWorker::State {
    enum class Request { Initialize, Consume, Peek, Present, Close };
    explicit State(VitaShaderLibrary library) : renderer(std::move(library)) {}
    GameRenderer renderer;
    std::thread thread;
    std::mutex shutdown;
    std::mutex mutex;
    std::condition_variable jobsChanged,callersChanged;
    Request request=Request::Initialize;
    bool pending=false,completed=false,ok=false,failed=false,closing=false,closed=false;
    std::string failure,error;
    std::shared_ptr<const Frame> frame;
    RecorderMemory memory;
    RecorderPeek probe;
    VitaXfbPresent presentation;
    uint32_t peekValue=0;
#if defined(__vita__)
    unsigned traceStarted[6]{};
#endif

    void run() {
        activeWorker=this;
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_WORKER_WAIT);
            jobsChanged.wait(lock,[&] { return pending && !completed; });
            const Request work=request;
            lock.unlock();
            bool result=false; std::string detail;
#if defined(__vita__)
            constexpr PortGameGpuPhase phases[]={PORT_GAME_GPU_INITIALIZE,PORT_GAME_GPU_CONSUME,
                PORT_GAME_GPU_PEEK,PORT_GAME_GPU_PRESENT,PORT_GAME_GPU_CLOSE};
            PORT_GAME_GPU_PROGRESS(phases[unsigned(work)]);
            // Trace black and selected-XFB presentation separately, twice each.
            const unsigned traceKey=work==Request::Present && !presentation.black ? 5 : unsigned(work);
            const bool traced=traceStarted[traceKey]<2;
            if (traced) ++traceStarted[traceKey];
            const auto trace=[&](const char* phase) {
                if (!traced) return;
                const char* names[]={"initialize","consume","peek","present","close"};
                std::fprintf(stderr,"native-worker op=%s phase=%s ok=%u frame=%llu black=%u framebuffer=%u\n",
                    names[unsigned(work)],phase,unsigned(result),
                    static_cast<unsigned long long>(frame ? frame->number : 0),
                    unsigned(presentation.black),unsigned(presentation.hasFramebuffer));
                std::fflush(stderr);
            };
            trace("begin");
#endif
#if defined(__cpp_exceptions)
            try {
#endif
                switch (work) {
                case Request::Initialize: result=renderer.open(detail); break;
                case Request::Consume: result=renderer.consume(frame,memory,detail); break;
                case Request::Peek: result=renderer.peek(probe,peekValue,detail); break;
                case Request::Present: result=renderer.present(presentation,detail); break;
                case Request::Close: result=renderer.close(detail); break;
                }
#if defined(__cpp_exceptions)
            } catch (const std::exception& exception) {
                detail=std::string("native game GPU worker failed: ")+exception.what();
            } catch (...) { detail="native game GPU worker failed with an unknown exception"; }
#endif
#if defined(__vita__)
            trace("complete");
#endif
            lock.lock();
            frame.reset(); memory={}; presentation={};
            ok=result; error=std::move(detail);
            if (!result && work!=Request::Close) { failed=true; failure=error; }
            if (work==Request::Close && result) closed=true;
            completed=true; callersChanged.notify_all();
            if (closed) { activeWorker=nullptr; return; }
            // A new job changes completed under the same lock. Do not require
            // observing an intermediate empty slot: back-to-back callers can
            // publish the next job before this thread is scheduled again.
        }
    }
    bool call(Request work,std::shared_ptr<const Frame> source,const RecorderMemory& window,std::string& detail,
              const RecorderPeek& access={},uint32_t* value=nullptr,const void* xfb=nullptr,bool black=false) {
        detail.clear();
        if (activeWorker==this) {
            detail="native GPU worker cannot reenter its own request queue"; return false;
        }
        if (work!=Request::Initialize && work!=Request::Close && !port_owns_game_cpu()) {
            detail="live GX request requires the calling guest to own the virtual CPU"; return false;
        }
        std::unique_lock<std::mutex> lock(mutex);
        callersChanged.wait(lock,[&] { return !pending; });
        if (closed) {
            if (work==Request::Close) return true;
            detail="native game GPU worker is closed"; return false;
        }
        if (closing && work!=Request::Close) { detail="native game GPU worker is closing"; return false; }
        if (failed && work!=Request::Close) { detail=failure; return false; }
        if (work==Request::Close) closing=true;
        if (work==Request::Present) {
            port_vi_display_config(&presentation.display);
            presentation.black=black; presentation.hasFramebuffer=xfb!=nullptr;
            if (!black && xfb) presentation.address=port_host_to_phys(xfb);
        }
        if (work==Request::Consume && (!source || (!window.tlut && window.tlutBytes))) {
            detail="live GX request has no immutable frame or an invalid TLUT window"; return false;
        }
        frame=std::move(source); memory=window; probe=access; request=work; completed=false; pending=true;
        jobsChanged.notify_one();
        callersChanged.wait(lock,[&] { return completed; });
        const bool result=ok; detail.swap(error);
        if (result && work==Request::Peek && value) *value=peekValue;
        pending=false; callersChanged.notify_all(); return result;
    }
};

VitaGameWorker::VitaGameWorker(VitaShaderLibrary library) : state(std::make_unique<State>(std::move(library))) {}
VitaGameWorker::~VitaGameWorker() {
    if (!state || !state->thread.joinable()) return;
    std::string error;
    if (!close(error)) port_fatal("native game worker destroyed with live GPU ownership: %s",error.c_str());
}
bool createVitaGameWorker(VitaShaderLibrary library,std::shared_ptr<VitaGameWorker>& output,std::string& error) {
    PortHostAllocScope scope; error.clear();
    if (output) { error="native game worker creation requires an empty owner"; return false; }
#if defined(__cpp_exceptions)
    try {
#endif
        output=std::shared_ptr<VitaGameWorker>(new VitaGameWorker(std::move(library)));
        auto* state=output->state.get();
        state->thread=std::thread([state] { state->run(); });
        return state->call(VitaGameWorker::State::Request::Initialize,{}, {},error);
#if defined(__cpp_exceptions)
    } catch (const std::exception& exception) { error=exception.what(); return false; }
#endif
}
bool VitaGameWorker::consume(std::shared_ptr<const Frame> frame,const RecorderMemory& memory,std::string& error) {
    PortHostAllocScope scope;
    return state->call(State::Request::Consume,std::move(frame),memory,error);
}
bool VitaGameWorker::peek(const RecorderPeek& request,uint32_t& value,std::string& error) {
    PortHostAllocScope scope;
    return state->call(State::Request::Peek,{}, {},error,request,&value);
}
bool VitaGameWorker::present(const void* xfb,bool black,std::string& error) {
    PortHostAllocScope scope;
    return state->call(State::Request::Present,{}, {},error,{},nullptr,xfb,black);
}
bool VitaGameWorker::close(std::string& error) {
    PortHostAllocScope scope;
    if (activeWorker==state.get()) {
        error="native GPU worker cannot shut down or join itself"; return false;
    }
    std::lock_guard<std::mutex> shutdown(state->shutdown);
    if (!state->thread.joinable()) { error.clear(); return true; }
    if (!state->call(State::Request::Close,{}, {},error)) return false;
    state->thread.join(); return true;
}
}
