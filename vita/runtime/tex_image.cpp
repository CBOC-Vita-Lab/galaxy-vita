#include "../../platform/src/gx/gx_record.h"

namespace gpu {
// The native cache owns its GXM image independently. Recorder images own only
// decoded CPU pixels; desktop GL deletion stays in gl_renderer.cpp.
TexImage::~TexImage() = default;
}
