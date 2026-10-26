#pragma once
#include "port/diagnostics.h"

#if PORT_RUNTIME_DIAGNOSTICS && PORT_LOGGING && !defined(__vita__)
#define PORT_MTX_PROFILE_COMPILED 1
#else
#define PORT_MTX_PROFILE_COMPILED 0
#endif

#if PORT_MTX_PROFILE_COMPILED
#ifdef __cplusplus
extern "C" {
#endif
unsigned port_mtx_profile_begin(const void* matrix);
void port_mtx_profile_end(unsigned token, unsigned result);
void port_mtx_profile_work_begin(void);
void port_mtx_profile_work_done(void);
#ifdef __cplusplus
}
#endif
#endif
