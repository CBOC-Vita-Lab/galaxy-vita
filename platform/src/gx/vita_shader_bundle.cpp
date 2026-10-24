#include "vita_material_cache.h"
#if __has_include("../../../build-gxm-material/vita_shader_bundle.h")
#include "../../../build-gxm-material/vita_shader_bundle.h"
#endif

namespace gpu {
bool installBakedVitaShaders(VitaShaderLibrary& library,std::string& error) {
#if __has_include("../../../build-gxm-material/vita_shader_bundle.h")
    return installBundledVitaShaders(library,error);
#else
    (void)library;
    error="verified native shader bundle is missing from this build";
    return false;
#endif
}
}
