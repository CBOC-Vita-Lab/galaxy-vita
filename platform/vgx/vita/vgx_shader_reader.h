#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
// Immutable borrowed file image; the driver copies it before deserialization.
int vgxvita_shaderCacheRead_v1(const char* path, const void** bytes, size_t* size);
void port_vgx_shader_cache_reader_driver_v1(void);
#ifdef __cplusplus
}
#endif
