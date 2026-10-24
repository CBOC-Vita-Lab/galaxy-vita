#pragma once

#include <math.h>

// glibc declares these private names as extern. Under Clang's MS extensions,
// the PPC static-inline replacements would otherwise become global C symbols.
#define __fabsf port_ppc_fabsf
#define __fabs port_ppc_fabs
