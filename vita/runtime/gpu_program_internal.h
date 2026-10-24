#pragma once
#include "gpu_program.h"

// Scene ownership bookkeeping, including a bind followed by a failed draw.
bool port_vita_gxm_program_bound(PortVitaGxmProgram* program, void* context);

struct PortVitaGxmDevice;
// Only the device wrapper adopts/removes a managed program. Public raw release
// cannot bypass the lease/list that makes its executable lifetime safe.
void port_vita_gxm_program_adopt(PortVitaGxmProgram* program, PortVitaGxmDevice* owner);
int port_vita_gxm_program_release_owned(PortVitaGxmProgram** program, PortVitaGxmDevice* owner);
