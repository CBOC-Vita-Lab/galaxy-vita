#include "renderer.h"
#include "../runtime/input.h"
#include "../../platform/src/gx/vita_material_cache.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <cstdio>

int main() {
    sceIoMkdir("ux0:data/codex-galaxy",0777);
    FILE* log=std::fopen("ux0:data/codex-galaxy/bringup.log","w");
    if (!log) { sceKernelExitProcess(2); return 2; }
    std::fprintf(log,"kind=native_gxm_input_diagnostic; not Galaxy gameplay or FPS evidence\n"); std::fflush(log);
    VitaBringupRenderer* renderer=nullptr; std::string error;
    gpu::VitaShaderLibrary library;
    int result=gpu::installBakedVitaShaders(library,error) ? createVitaBringupRenderer(library,&renderer,error) : -1;
    bool input=false;
    if (result>=0) {
        input=port_vita_input_init()!=0;
        if (!input) { result=-1; error="native controller initialization failed"; }
    }
    uint64_t previous=sceKernelGetProcessTimeWide(),frames=0;
    unsigned previousButtons=0;
    while (result>=0) {
        const uint64_t now=sceKernelGetProcessTimeWide(); PortPadState pad{};
        port_vita_input_poll(float(now-previous)/1000000,&pad); previous=now;
        if (pad.buttons&0x10) break;
        if ((pad.buttons&0x1000) && !(previousButtons&0x1000)) port_vita_input_recenter();
        previousButtons=pad.buttons;
        result=drawVitaBringup(renderer,pad,error);
        if (result>=0) ++frames;
        if (frames==1 || result<0) {
            std::fprintf(log,"submitted_diagnostic_frames=%llu result=%d detail=%s\n",
                static_cast<unsigned long long>(frames),result,error.c_str()); std::fflush(log);
        }
    }
    if (input) port_vita_input_shutdown();
    std::fprintf(log,"terminal frames=%llu result=%d detail=%s\n",static_cast<unsigned long long>(frames),result,error.c_str());
    const int closed=renderer ? releaseVitaBringupRenderer(&renderer) : 0;
    std::fprintf(log,"renderer_close=%d\n",closed); std::fclose(log);
    const int status=result<0 || closed<0 ? 1 : 0;
    sceKernelExitProcess(status); return status;
}
