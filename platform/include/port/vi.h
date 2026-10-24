#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Value copied from the VI configuration committed at retrace, never a
// borrowed GXRenderModeObj. The guest snapshots this before GPU handoff.
typedef struct PortViDisplay {
    uint32_t configured,tv_mode,fb_width,fb_height,xfb_mode,field_rendering;
    uint32_t pan_x,pan_y,pan_width,pan_height;
    uint32_t origin_x,origin_y,display_width,display_height,aspect;
} PortViDisplay;
void port_vi_display_config(PortViDisplay* out);

#ifdef __cplusplus
}
#endif
