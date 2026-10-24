#include "../../platform/src/gx/vita_game_worker.h"
#include "../runtime/input.h"
#include "port/port.h"
#include "port/game_progress.h"
#include <psp2/io/stat.h>
#include <algorithm>
#include <cstdio>

extern "C" void port_boot(const char*,const char*);

extern "C" int port_vita_game_entry() {
    sceIoMkdir("ux0:data/codex-galaxy",0777);
    sceIoMkdir("ux0:data/codex-galaxy/saves",0777);
    port_log_file("ux0:data/codex-galaxy/native-game.log");
    port_log("kind=native_galaxy_development; native boot/play/pixels/FPS remain unverified");
    if (!port_vita_input_init()) {
        port_log("native game input initialization failed; game boot prevented"); return 1;
    }
    gpu::VitaShaderLibrary library; std::string error;
    std::shared_ptr<gpu::VitaGameWorker> worker;
    if (!gpu::installBakedVitaShaders(library,error) ||
        !gpu::createVitaGameWorker(std::move(library),worker,error) ||
        !gpu::attachRecorderConsumer(worker,error)) {
        port_log("native game GPU initialization failed: %s",error.c_str());
        if (worker && !worker->close(error))
            port_fatal("native GPU initialization cleanup retained ownership: %s",error.c_str());
        port_vita_input_shutdown(); return 2;
    }
    // port_boot starts the guest on its verified 16 MiB kernel pthread stack.
    // This thread stays host-only: it feeds the existing synchronized WPAD path,
    // never guest OS/GX or borrowed RAM. GXM belongs exclusively to the worker.
    port_boot("ux0:data/codex-galaxy/game","ux0:data/codex-galaxy/saves");
    int64_t previous=port_host_time_ns();
    for (;;) {
        const int64_t now=port_host_time_ns();
        PortPadState pad{};
        port_vita_input_poll(std::clamp(float(now-previous)/1000000000.0f,0.0f,0.1f),&pad);
        previous=now; port_input_set(0,&pad);
#if defined(__vita__)
        PortGameProgress progress{};
        if (port_game_progress_poll(now,&progress)) {
            char line[384];
            const int bytes=std::snprintf(line,sizeof(line),
                "native-progress phase=%s generation=%u observed-idle-ms=%lld audio=%s:%u dsp=%s:%u irq=%u:%u gpu=%s:%u cpu=%08x\n",
                port_game_progress_name(progress.phase),progress.generation,
                static_cast<long long>(progress.idle_ns/1000000),
                port_game_progress_name(progress.audio_phase),progress.audio_generation,
                port_game_progress_name(progress.dsp_phase),progress.dsp_generation,
                progress.irq_phase,progress.irq_generation,
                port_game_progress_gpu_name(progress.gpu_phase),progress.gpu_generation,progress.cpu_owner);
            if (bytes>0 && size_t(bytes)<sizeof(line)) std::fwrite(line,1,size_t(bytes),stderr);
        }
#endif
        port_host_sleep_ns(4000000);
    }
}
