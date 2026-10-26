#include <atomic>
#include <cassert>
#include <cstdio>
#include <initializer_list>
using GLboolean = unsigned char;
constexpr GLboolean GL_FALSE = 0;
static bool activeSplash=false;
namespace splash { bool Active() { return activeSplash; } }
static unsigned submissions=0, finishes=0, transfers=0, queues=0, resumes=0;
static GLboolean previousScene=1;
static int transferResult=0, queueResult=0;
static GLboolean Finish() { ++finishes; return previousScene; }
static void Resume(GLboolean old) { assert(old==previousScene);++resumes; }
static void Submit() { ++submissions; }
static GLboolean (*vglFinishUniformPoolScene)()=Finish;
static void (*vglResumeUniformPoolScene)(GLboolean)=Resume;
static void (*vglSubmitWithoutSwap)()=Submit;
static int sceGxmTransferFinish() {++transfers;return transferResult;}
static int sceGxmDisplayQueueFinish() {++queues;return queueResult;}
static bool g_presentAllowed=true, g_jobSwapped=false, g_wasSplash=true, g_fixedJob=false;
static GLboolean g_settingsSceneResume=GL_FALSE;
static std::atomic<unsigned long long> g_gameSwaps{0};
static std::atomic<unsigned> g_presentSlotEpoch{0};
#include "settings_host_actual.inc"
int main() {
    assert(port_vgx_vita_fixed_cap_supported());
    vglSubmitWithoutSwap=nullptr;
    assert(!port_vgx_vita_fixed_cap_supported());
    vglSubmitWithoutSwap=Submit;
    port_vgx_vita_set_present_allowed(0);
    assert(!g_presentAllowed && !g_jobSwapped);
    port_vgx_vita_finish_unpresented_frame();assert(submissions==1);
    g_jobSwapped=true;
    port_vgx_vita_finish_unpresented_frame();assert(submissions==1);
    port_vgx_vita_set_present_allowed(1);assert(!g_jobSwapped);
    port_vgx_vita_finish_unpresented_frame();assert(submissions==2); // eligible but no actual swap
    activeSplash=true;assert(!port_vgx_vita_settings_finish(0));assert(finishes==0);
    activeSplash=false;vglFinishUniformPoolScene=nullptr;
    assert(!port_vgx_vita_settings_finish(0));
    vglFinishUniformPoolScene=Finish;
    assert(port_vgx_vita_settings_finish(0));
    assert(finishes==1 && transfers==1 && queues==1);
    g_wasSplash=false;
    port_vgx_vita_settings_resume();assert(resumes==1 && g_presentSlotEpoch==1);
    port_vgx_vita_set_present_allowed(0);
    assert(g_wasSplash && !GamePresentAllowed(0,960,544));
    assert(g_wasSplash && !GamePresentAllowed(1,0,544));
    assert(g_wasSplash && GamePresentAllowed(1,960,544));
    assert(port_vgx_vita_game_swaps()==0);
    for (GLboolean old : {GLboolean(0), GLboolean(1)}) {
        previousScene=old;
        const auto epoch=g_presentSlotEpoch.load();
        auto restored=resumes, queued=queues;
        transferResult=-1;
        assert(!port_vgx_vita_settings_finish(0));
        assert(resumes==restored+1 && queues==queued && g_presentSlotEpoch==epoch);
        restored=resumes;transferResult=0;queueResult=-1;
        assert(!port_vgx_vita_settings_finish(0));
        assert(resumes==restored+1 && queues==queued+1 && g_presentSlotEpoch==epoch);
        queueResult=0;
        assert(port_vgx_vita_settings_finish(0));
        assert(resumes==restored+1);
        port_vgx_vita_settings_resume();
        assert(resumes==restored+2 && g_presentSlotEpoch==epoch+1);
    }
    std::puts("PASS: actual Vita host capability, hidden submission and modal driver ownership hooks");
}
