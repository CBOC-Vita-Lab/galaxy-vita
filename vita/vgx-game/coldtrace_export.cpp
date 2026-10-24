#include "port/vgx_coldtrace_control.h"

#if VGX_COLDTRACE
#include "../../platform/vgx/src/vgx_coldtrace_control.h"
#include "../../platform/vgx/src/vgx_coldtrace_report.h"
#include <psp2/io/fcntl.h>
#include <limits>

static_assert(PORT_VGX_COLDTRACE_CONTROL_VERSION==1 && VGX_COLDTRACE_DETAIL_VERSION==3);
namespace {
using namespace vgx::coldtrace;
constexpr const char* kReportPath="ux0:data/smg/coldtrace-v3.json";
constinit Control control(g_recorder);
int64_t WriteFile(const char* bytes,size_t size,void* handle) noexcept {
    return sceIoWrite(*static_cast<SceUID*>(handle),bytes,static_cast<SceSize>(size));
}
int ExportFile(const FrozenView& view,void*) noexcept {
    if(!view.data)return kReportInvalid;
    SceUID file=sceIoOpen(kReportPath,SCE_O_WRONLY|SCE_O_CREAT|SCE_O_EXCL,0600);
    if(file<0)return file;
    const auto result=WriteReport(view,{WriteFile,&file});
    const int closed=sceIoClose(file);
    if(result.error || closed!=0){
        sceIoRemove(kReportPath); // Cleanup relies on exclusive-create semantics; validate EXCL on target before use.
        if(result.error)return result.error<std::numeric_limits<int>::min() || result.error>std::numeric_limits<int>::max()?kReportSinkError:static_cast<int>(result.error);
        return closed<0?closed:kReportSinkError;
    }
    return 0;
}
}
extern "C" void port_vgx_coldtrace_export_runtime_link_guard_v3() {}
extern "C" void port_vgx_coldtrace_activate(void) {control.Activate();}
extern "C" void port_vgx_coldtrace_pause(int selecting) {control.PauseTurn(selecting!=0,ExportFile,nullptr);}
extern "C" void port_vgx_coldtrace_render_boundary(void) {control.RenderBoundary();}
extern "C" int port_vgx_coldtrace_status(void) {return static_cast<int>(control.Status());}
extern "C" int port_vgx_coldtrace_error(void) {return control.Error();}
extern "C" int port_vgx_coldtrace_export_pending(void) {
    const auto state=control.Status();
    return state!=ControlState::Dormant && state!=ControlState::Complete && state!=ControlState::Failed;
}
#endif
