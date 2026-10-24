#include "port/game_progress.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <atomic>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

extern "C" void port_game_progress_checkpoint(uint32_t) __attribute__((weak));
extern "C" int port_game_progress_poll(int64_t,PortGameProgress*) __attribute__((weak));
extern "C" const char* port_game_progress_name(uint32_t) __attribute__((weak));
extern "C" void port_game_progress_irq_checkpoint(uint32_t,uint32_t) __attribute__((weak));
extern "C" void port_game_progress_gpu_checkpoint(uint32_t) __attribute__((weak));
extern "C" void port_game_progress_cpu_checkpoint(uint32_t) __attribute__((weak));
extern "C" const char* port_game_progress_gpu_name(uint32_t) __attribute__((weak));
extern "C" int64_t port_host_time_ns() { return 0; }
extern "C" int port_handheld() { return 0; }
extern "C" void port_log(const char*,...) {}

static unsigned checks;
static void require(bool ok,const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr,"Game progress FAILED: %s\n",message); std::exit(1); }
}
static constexpr int64_t second=1000000000ll;
static void noEvents() {
    PortGameProgress out{}; out.phase=77; out.generation=88; out.idle_ns=99;
    require(!port_game_progress_poll(second,&out) && out.phase==77 && out.generation==88 && out.idle_ns==99,
        "no game checkpoint cannot invent progress or overwrite an absent sample");
    port_game_progress_checkpoint(0); port_game_progress_checkpoint(UINT32_MAX);
    require(!port_game_progress_poll(9*second,&out),"invalid phases cannot invent an initial checkpoint");
    require(std::strcmp(port_game_progress_name(UINT32_MAX),"unknown")==0,
        "invalid phase labels are bounded independently of guest data");
}
static void unchanged() {
    PortGameProgress out{};
    port_game_progress_checkpoint(PORT_GAME_PHASE_BEGIN_RENDER);
    require(!port_game_progress_poll(second,&out),"first checkpoint initializes observation without an early report");
    require(!port_game_progress_poll(6*second-1,&out),"diagnostic report respects its five-second observation interval");
    require(port_game_progress_poll(6*second,&out) && out.phase==PORT_GAME_PHASE_BEGIN_RENDER &&
        out.generation==1 && out.idle_ns==5*second &&
        std::strcmp(port_game_progress_name(out.phase),"begin-render")==0,
        "a stalled atomic checkpoint is reported with its exact phase, generation and observed idle duration");
    require(!out.irq_phase && !out.irq_generation && !out.gpu_phase && !out.gpu_generation && !out.cpu_owner,
        "unpublished system lanes have explicit absent defaults");
    port_game_progress_checkpoint(0); port_game_progress_checkpoint(PORT_GAME_PHASE_COUNT);
    require(!port_game_progress_poll(7*second,&out),"invalid phases do not defeat report pacing");
    require(port_game_progress_poll(11*second,&out) && out.generation==1 && out.idle_ns==10*second,
        "invalid phases neither overwrite the last valid phase nor fake a new generation");
}
static void moving() {
    PortGameProgress out{};
    port_game_progress_checkpoint(PORT_GAME_PHASE_AUDIO_UPDATE_SCENE);
    require(!port_game_progress_poll(second,&out),"moving observer starts with a real checkpoint");
    port_game_progress_checkpoint(PORT_GAME_PHASE_AUDIO_UPDATE_SCENE);
    require(!port_game_progress_poll(4*second,&out),"repeated phase advances generation without an early report");
    require(port_game_progress_poll(6*second,&out) && out.phase==PORT_GAME_PHASE_AUDIO_UPDATE_SCENE &&
        out.generation==2 && out.idle_ns==2*second,
        "repeated equal phases count actual checkpoint progress rather than appearing continuously stalled");
    port_game_progress_checkpoint(PORT_GAME_PHASE_AUDIO_MIX);
    require(!port_game_progress_poll(7*second,&out),"a changed phase is sampled before the next reporting slot");
    require(port_game_progress_poll(11*second,&out) && out.phase==PORT_GAME_PHASE_AUDIO_MIX &&
        out.generation==3 && out.idle_ns==4*second,
        "a new phase and its generation are returned coherently after current progress");
}
static void bounded() {
    PortGameProgress out{}; out.phase=77; out.generation=88; out.idle_ns=99;
    port_game_progress_checkpoint(PORT_GAME_PHASE_END_FRAME);
    require(!port_game_progress_poll(second,nullptr),"null output cannot consume an observation slot");
    require(!port_game_progress_poll(-1,&out),"negative time cannot invent an observation epoch");
    require(!port_game_progress_poll(second,&out),"valid observer still initializes after rejected input");
    require(!port_game_progress_poll(0,&out),"regressing monotonic time cannot corrupt observation duration");
    for (unsigned slot=1;slot<=4;++slot)
        require(port_game_progress_poll((1+5*slot)*second,&out) && out.generation==1 && out.idle_ns==int64_t(5*slot)*second,
            "four paced diagnostic slots retain actual unchanged checkpoint evidence");
    require(!port_game_progress_poll(26*second,&out) && !port_game_progress_poll(INT64_MAX,&out),
        "host reporting remains bounded after four samples even over long runtimes");
}
static void concurrent() {
    PortGameProgress out{};
    std::thread a([] { for (unsigned i=0;i<1000;++i) port_game_progress_checkpoint(PORT_GAME_PHASE_DRAW); });
    std::thread b([] { for (unsigned i=0;i<1000;++i) port_game_progress_checkpoint(PORT_GAME_PHASE_AUDIO_GATE); });
    a.join(); b.join();
    require(!port_game_progress_poll(second,&out),"concurrent checkpoints can initialize one host observer");
    require(port_game_progress_poll(6*second,&out) && out.generation==2000 &&
        (out.phase==PORT_GAME_PHASE_DRAW || out.phase==PORT_GAME_PHASE_AUDIO_GATE),
        "atomic updates retain every concurrent generation with a valid coherent final phase");
}
static void livePublisher() {
    PortGameProgress out{};
    std::atomic<bool> stop{false},published{false};
    std::thread producer([&] {
        uint32_t generation=0;
        while (!stop.load(std::memory_order_relaxed)) {
            ++generation;
            port_game_progress_checkpoint((generation&1u) ? PORT_GAME_PHASE_DRAW : PORT_GAME_PHASE_AUDIO_GATE);
            published.store(true,std::memory_order_release);
        }
    });
    while (!published.load(std::memory_order_acquire)) std::this_thread::yield();
    require(!port_game_progress_poll(second,&out),"live producer initializes the single observer");
    for (unsigned slot=1;slot<=4;++slot) {
        require(port_game_progress_poll((1+5*slot)*second,&out),"polling succeeds while a producer remains active");
        require(out.phase==((out.generation&1u) ? PORT_GAME_PHASE_DRAW : PORT_GAME_PHASE_AUDIO_GATE),
            "phase and generation remain coherent during actual concurrent publication");
    }
    stop.store(true,std::memory_order_relaxed); producer.join();
}
static void timeBoundary() {
    PortGameProgress out{};
    port_game_progress_checkpoint(PORT_GAME_PHASE_END_FRAME);
    const int64_t start=INT64_MAX-5*second;
    require(!port_game_progress_poll(start,&out),"active observer initializes near the signed time limit");
    require(!port_game_progress_poll(INT64_MAX-1,&out),"time boundary retains exact pacing");
    require(port_game_progress_poll(INT64_MAX,&out) && out.phase==PORT_GAME_PHASE_END_FRAME && out.idle_ns==5*second,
        "active time subtraction remains defined at the signed limit");
    for (uint32_t phase=1;phase<PORT_GAME_PHASE_COUNT;++phase)
        require(std::strcmp(port_game_progress_name(phase),"unknown")!=0,"every current native checkpoint has a bounded label");
}
static void separateWorkers() {
    PortGameProgress out{};
    port_game_progress_checkpoint(PORT_GAME_PHASE_GX_FINISH_WAIT);
    require(!port_game_progress_poll(second,&out),"main progress starts before background activity");
    port_game_progress_checkpoint(PORT_GAME_PHASE_AUDIO_RESOURCES);
    port_game_progress_checkpoint(PORT_GAME_PHASE_AUDIO_READY);
    port_game_progress_checkpoint(PORT_GAME_PHASE_DSP_BOOT);
    port_game_progress_checkpoint(PORT_GAME_PHASE_AUDIO_MESSAGE_WAIT);
    require(!port_game_progress_poll(4*second,&out),"background checkpoints retain main observation pacing");
    require(port_game_progress_poll(6*second,&out) && out.phase==PORT_GAME_PHASE_GX_FINISH_WAIT &&
        out.generation==1 && out.idle_ns==5*second,
        "background audio must not hide the last main phase or fake main progress");
    require(out.audio_phase==PORT_GAME_PHASE_AUDIO_READY && out.audio_generation==2 &&
        out.dsp_phase==PORT_GAME_PHASE_AUDIO_MESSAGE_WAIT && out.dsp_generation==2,
        "each background lane preserves its own coherent phase and generation");
}
static void systemMetadata() {
    require(port_game_progress_irq_checkpoint && port_game_progress_gpu_checkpoint && port_game_progress_cpu_checkpoint &&
        port_game_progress_gpu_name,"real production IRQ/GPU/CPU publication exists");
    PortGameProgress out{};
    port_game_progress_checkpoint(PORT_GAME_PHASE_GX_FINISH_RESTORE);
    require(!port_game_progress_poll(second,&out),"system observer begins with a real frame checkpoint");
    port_game_progress_irq_checkpoint(1,1);
    port_game_progress_gpu_checkpoint(PORT_GAME_GPU_QUEUE);
    port_game_progress_cpu_checkpoint(0x12345678);
    require(port_game_progress_poll(6*second,&out) && out.phase==PORT_GAME_PHASE_GX_FINISH_RESTORE &&
        out.generation==1 && out.idle_ns==5*second,"system metadata does not invent frame progress");
    require(out.irq_phase==2 && out.irq_generation==1 && out.gpu_phase==PORT_GAME_GPU_QUEUE &&
        out.gpu_generation==1 && out.cpu_owner==0x12345678,"published VI entry/GPU stage/CPU identifier are sampled without foreign reads");
    port_game_progress_irq_checkpoint(UINT32_MAX,1); port_game_progress_irq_checkpoint(1,2);
    port_game_progress_gpu_checkpoint(UINT32_MAX);
    port_game_progress_irq_checkpoint(1,0); port_game_progress_gpu_checkpoint(PORT_GAME_GPU_QUEUE_RETURN);
    port_game_progress_cpu_checkpoint(0);
    require(port_game_progress_poll(11*second,&out) && out.irq_phase==0 && out.irq_generation==2 &&
        out.gpu_phase==PORT_GAME_GPU_QUEUE_RETURN && out.gpu_generation==2 && out.cpu_owner==0,
        "invalid metadata cannot displace valid handler return, SDK return or idle owner");
    require(std::strcmp(port_game_progress_gpu_name(UINT32_MAX),"unknown")==0,
        "GPU labels remain bounded for rejected metadata");
    for (uint32_t phase=1;phase<PORT_GAME_GPU_COUNT;++phase)
        require(std::strcmp(port_game_progress_gpu_name(phase),"unknown")!=0,"every valid GPU checkpoint has a bounded label");
}
static void liveSystemMetadata() {
    std::atomic<bool> stop{false},irqReady{false},gpuReady{false};
    std::thread irq([&] {
        uint32_t generation=0;
        while (!stop.load(std::memory_order_relaxed)) {
            port_game_progress_irq_checkpoint(1,(++generation)&1u);
            irqReady.store(true,std::memory_order_release);
        }
    });
    std::thread gpu([&] {
        uint32_t generation=0;
        while (!stop.load(std::memory_order_relaxed)) {
            port_game_progress_gpu_checkpoint(((++generation)&1u) ? PORT_GAME_GPU_PRESENT : PORT_GAME_GPU_QUEUE);
            gpuReady.store(true,std::memory_order_release);
        }
    });
    while (!irqReady.load(std::memory_order_acquire) || !gpuReady.load(std::memory_order_acquire)) std::this_thread::yield();
    port_game_progress_checkpoint(PORT_GAME_PHASE_GX_FINISH_RESTORE);
    PortGameProgress out{};
    require(!port_game_progress_poll(second,&out),"independent live system lanes initialize one host observer");
    for (unsigned slot=1;slot<=4;++slot) {
        require(port_game_progress_poll((1+5*slot)*second,&out) && out.generation==1,
            "concurrent metadata remains independent of original frame progress");
        require(out.irq_phase==((out.irq_generation&1u) ? 2u : 0u) &&
            out.gpu_phase==((out.gpu_generation&1u) ? PORT_GAME_GPU_PRESENT : PORT_GAME_GPU_QUEUE),
            "each IRQ/GPU phase remains coherent with its own generation during live publication");
    }
    stop.store(true,std::memory_order_relaxed); irq.join(); gpu.join();
}
int main() {
    require(port_game_progress_checkpoint && port_game_progress_poll && port_game_progress_name,
        "real production atomic checkpoint and host observer exist");
    for (auto test:{noEvents,unchanged,moving,bounded,concurrent,livePublisher,timeBoundary,separateWorkers,systemMetadata,liveSystemMetadata}) {
        const pid_t child=fork(); require(child>=0,"isolated diagnostic test child starts");
        if (!child) { test(); std::fprintf(stderr,"Game progress child: %u checks\n",checks); _exit(0); }
        int status=0;
        require(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),
            "actual production checkpoint/poll behavior passes in a reaped isolated child");
    }
    std::fprintf(stderr,"Game progress: ten production-state scenarios passed; not guest progress or native performance proof\n");
}
