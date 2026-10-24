#pragma once
#include "gpu_texture.h"

// Cache-owned views cannot be released by the public borrowed-handle API.
// Owner identity is scalar across game/SDK enum ABIs. One submission thread.
int port_vita_gxm_texture_view_create_owned(PortVitaGxmTexture*, uint32_t mode0, uint32_t mode1,
                                           const void* owner, PortVitaGxmTextureView**,bool explicitLinearMip=false);
int port_vita_gxm_texture_view_release_owned(PortVitaGxmTextureView**, const void* owner);
