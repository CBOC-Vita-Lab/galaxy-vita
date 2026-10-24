#pragma once
#include <stddef.h>
#include <stdint.h>
#include <psp2/gxm.h>

// Actual SDK format enums; the allocation and descriptor sinks are test doubles.
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned char GLboolean;
typedef struct {
    void* data;
    void* palette_data;
    int status, mip_count, last_frame;
    SceGxmTexture gxm_tex;
} texture;
enum { TEX_VALID=1, OBJ_NOT_USED=2 };
extern void* color_table;
void* gpu_alloc_mapped_for_gpu(size_t);
void gpu_free_texture_data(texture*);
void vgl_fast_memcpy(void*,const void*,size_t);
void vgl_memset(void*,int,size_t);
void vglInitLinearTexture(SceGxmTexture*,const void*,SceGxmTextureFormat,unsigned,unsigned,unsigned);
void gpu_alloc_texture(uint32_t,uint32_t,SceGxmTextureFormat,const void*,texture*,uint8_t,
                       uint32_t (*)(void*),void (*)(void*,uint32_t),GLboolean);
uint32_t read_la88(const void*);
void write_ra88(void*,uint32_t);
#ifdef __cplusplus
}
#endif
#define VGL_ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
