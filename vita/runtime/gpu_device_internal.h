#pragma once
#include "gpu_device.h"

// SDK-side resource ownership only. Each successful hold is balanced once by
// drop after all native handles/memory are released. Retire also works after a
// callback error, so an unhealthy device never traps its child resources.
int port_vita_gxm_device_hold_resource(PortVitaGxmDevice* device);
int port_vita_gxm_device_retire_resource(PortVitaGxmDevice* device);
// Drain native queue references during cleanup without treating a persistent
// scanout callback error as permission to render or as an unretirable child.
int port_vita_gxm_device_retire_display(PortVitaGxmDevice* device);
void port_vita_gxm_device_drop_resource(PortVitaGxmDevice* device);
bool port_vita_gxm_device_scene_active(const PortVitaGxmDevice* device);
bool port_vita_gxm_device_owns_program(const PortVitaGxmDevice* device, const PortVitaGxmProgram* program);

// Managed material programs reject raw/public-device release. Only the matching
// cache may retire them, including partial native rollback owners.
int port_vita_gxm_device_program_create_owned(PortVitaGxmDevice*, const void* owner,
    const void* vertex_gxp, uint32_t vertex_bytes, const void* fragment_gxp, uint32_t fragment_bytes,
    const PortVitaGxmBlend*, uint32_t pixel_vectors, uint32_t texture_mask,
    PortVitaGxmProgram**, PortVitaGxmProgramInfo*);
int port_vita_gxm_device_program_release_owned(PortVitaGxmDevice*, PortVitaGxmProgram**, const void* owner);

// Borrowed cache targets cannot bypass their CPU leases through public release.
int port_vita_gxm_surface_create_owned(PortVitaGxmDevice*,uint32_t width,uint32_t height,
                                      const void* owner,PortVitaGxmSurface**);
int port_vita_gxm_surface_release_owned(PortVitaGxmSurface**,const void* owner);
