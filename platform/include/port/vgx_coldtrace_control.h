#pragma once
#ifndef VGX_COLDTRACE
#define VGX_COLDTRACE 0
#endif

#if VGX_COLDTRACE
#define PORT_VGX_COLDTRACE_CONTROL_VERSION 1
#ifdef __cplusplus
extern "C" {
#endif
// Probe only. Activate after Arm; pause on the game thread; boundary on the render owner.
void port_vgx_coldtrace_activate(void);
void port_vgx_coldtrace_pause(int selecting);
void port_vgx_coldtrace_render_boundary(void);
int port_vgx_coldtrace_status(void);
int port_vgx_coldtrace_error(void);
int port_vgx_coldtrace_export_pending(void);
#ifdef __cplusplus
}
#endif
#endif
