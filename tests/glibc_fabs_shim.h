// Force-included by host tests built against glibc: its <math.h> declares the reserved names __fabs and __fabsf that
// platform/include/port/ppc_intrinsics.h defines as static inlines.  Harmless elsewhere.
#pragma once
#include <math.h>
#define __fabsf port_fabsf_host
#define __fabs port_fabs_host
