#include <cstdio>
#if __has_include("../vita/vgx-game/coldtrace_export.cpp")
#include "port/vgx_coldtrace_control.h"
#include "port/diagnostics.h"
#include "../platform/vgx/src/vgx_coldtrace_control.h"
#include <psp2/io/fcntl.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>

extern "C" void port_zoom_menu(int);
namespace {
using namespace vgx::coldtrace;
const char* mode="ok";bool inPause=false;unsigned opens=0,writes=0,closes=0,removes=0;size_t used=0;
unsigned csvCallbacks=0;
char output[2u*1024u*1024u];std::atomic<uint64_t> now{0};thread_local uint32_t tid=1;
uint64_t Clock() noexcept{return now.load();}uint32_t Thread() noexcept{return tid;}
void Require(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
void FilesystemGate(){Require(inPause && g_recorder.Frozen().data && port_vgx_coldtrace_status()==static_cast<int>(ControlState::Writing),"report filesystem access is restricted to a paused turn after drain");}
void Menu(int selecting){inPause=selecting!=0;port_zoom_menu(selecting);inPause=false;}
}
extern "C" {
void port_vgx_diagnostic_csv_pause(int selecting) {
    if (!selecting) return;
    const auto state=static_cast<ControlState>(port_vgx_coldtrace_status());
    Require(inPause && (state==ControlState::Dormant || state==ControlState::Complete || state==ControlState::Failed),
            "CSV export waits for the coldtrace freeze, drain and report export to complete");
    ++csvCallbacks;
}
void port_log(const char*,...) {}
void vgxvita_prepareShaderCacheWrite_v1(const char*, int, SceMode) {
    Require(false,"paused report export must not enter private shader-cache preparation");
}
SceUID sceIoOpen(const char* path,int flags,SceMode permissions) {
    FilesystemGate();++opens;
    Require(std::strcmp(path,"ux0:data/smg/coldtrace-v3.json")==0,"only the fixed report path may be opened");
    Require(flags==(SCE_O_WRONLY|SCE_O_CREAT|SCE_O_EXCL) && permissions==0600,"report creation never truncates an existing file");
    return std::strcmp(mode,"exists")==0?-17:7;
}
SceSSize sceIoWrite(SceUID fd,const void* bytes,SceSize count) {
    FilesystemGate();Require(fd==7,"write uses only the owned report handle");++writes;
    if(std::strcmp(mode,"write-fail")==0)return -77;if(std::strcmp(mode,"zero-write")==0)return 0;
    const auto n=std::strcmp(mode,"short")==0?std::min<SceSize>(count,3):count;
    Require(n<=sizeof(output)-used,"synthetic report storage is bounded");std::memcpy(output+used,bytes,n);used+=n;return static_cast<SceSSize>(n);
}
int sceIoClose(SceUID fd){FilesystemGate();Require(fd==7,"close uses only the owned report handle");++closes;return std::strstr(mode,"close-fail")?-31:0;}
int sceIoRemove(const char* path){FilesystemGate();Require(std::strcmp(path,"ux0:data/smg/coldtrace-v3.json")==0,"cleanup targets only the new report");++removes;
    if(std::strcmp(mode,"close-fail-remove-fail")==0)return -41;used=0;return 0;}
}
int main(int argc,char** argv) {
    if(argc>1)mode=argv[1];using namespace vgx::coldtrace;
    Menu(1);port_vgx_coldtrace_render_boundary();Require(opens==0 && port_vgx_coldtrace_status()==static_cast<int>(ControlState::Dormant),"unarmed pause has no file side effects");
    Require(g_recorder.Arm(Clock,Thread,1),"arm native bridge fixture");port_vgx_coldtrace_activate();
    {Scope root(g_recorder,Key{7,1,2,3,4,5});now.store(208000);}
    std::atomic<bool> started{false},release{false};
    std::thread worker([&]{tid=2;Scope read(g_recorder,Phase::Read,IoClass::Read);started.store(true);while(!release.load())std::this_thread::yield();read.Result(7,9);});
    while(!started.load())std::this_thread::yield();
    Menu(1);Require(opens==0 && port_vgx_coldtrace_status()==static_cast<int>(ControlState::FreezeRequested),"actual zoom-menu callback requests freeze without file access");
    Require(csvCallbacks==(PORT_RUNTIME_DIAGNOSTICS?1u:0u),"CSV callback cannot run while RAM tracing is still recording or draining");
    Menu(0);Require(opens==0,"unpause before the render boundary does not write");
    port_vgx_coldtrace_render_boundary();Menu(1);Require(opens==0,"paused turn does not export partial worker data");
    release.store(true);worker.join();Menu(0);Require(opens==0,"unpaused control flow never exports");
    Menu(1);
    const bool success=std::strcmp(mode,"ok")==0 || std::strcmp(mode,"short")==0;
    Require(opens==1,"the report is opened once per session");
    Require(port_vgx_coldtrace_status()==static_cast<int>(success?ControlState::Complete:ControlState::Failed),"native status matches checked export result");
    if(success){Require(writes>0 && closes==1 && removes==0 && used>0,"success closes and preserves one complete report");
        Require(std::strstr(output,"\"complete\":true}")!=nullptr,"successful export ends with a complete stream marker");}
    else if(std::strcmp(mode,"exists")==0)Require(writes==0 && closes==0 && removes==0 && port_vgx_coldtrace_error()==-17,"an old report is never modified or removed");
    else if(std::strcmp(mode,"close-fail-remove-fail")==0)Require(closes==1 && removes==1 && used>0 && port_vgx_coldtrace_error()==-31,"failed cleanup preserves the close error and never retries a possibly live handle");
    else Require(closes==1 && removes==1 && used==0 && port_vgx_coldtrace_error()!=0,"a failed newly-owned report is closed, removed and reported as failed");
    const unsigned openCount=opens;for(unsigned i=0;i<1000;++i)Menu(1);Require(opens==openCount,"completed and failed exports are never retried per frame");
    Require(g_recorder.Frozen().ioCount==1,"export I/O is not added back into the frozen capture");
    std::printf("PASS: real pause callback/native export %s, one-shot frozen-only I/O and protected existing file\n",mode);
}
#else
int main(){std::fputs("FAIL: native pause report export is not implemented\n",stderr);return 1;}
#endif
