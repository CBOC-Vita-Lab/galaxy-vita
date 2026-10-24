#ifndef VGX_FROZEN_TEXTURE_TEST_H
#define VGX_FROZEN_TEXTURE_TEST_H
#include <stdint.h>
#include <stddef.h>

/* SDK/allocator doubles, not a native GPU implementation. */
#ifdef __cplusplus
extern "C" {
#endif
typedef int SceGxmTextureFormat;
typedef unsigned char GLboolean;
enum {
    SCE_GXM_TEXTURE_FORMAT_PVRT2BPP_1BGR = 1, SCE_GXM_TEXTURE_FORMAT_PVRT2BPP_ABGR,
    SCE_GXM_TEXTURE_FORMAT_PVRT4BPP_1BGR, SCE_GXM_TEXTURE_FORMAT_PVRT4BPP_ABGR,
    SCE_GXM_TEXTURE_FORMAT_PVRTII2BPP_ABGR, SCE_GXM_TEXTURE_FORMAT_PVRTII4BPP_ABGR,
    SCE_GXM_TEXTURE_FORMAT_UBC1_1BGR, SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR,
    SCE_GXM_TEXTURE_FORMAT_ETC1_1BGR, SCE_GXM_TEXTURE_FORMAT_UBC4_R,
    SCE_GXM_TEXTURE_FORMAT_UBC2_ABGR, SCE_GXM_TEXTURE_FORMAT_UBC3_ABGR, SCE_GXM_TEXTURE_FORMAT_UBC5_GR,
    SCE_GXM_TRANSFER_COLORKEY_NONE, SCE_GXM_TRANSFER_FORMAT_RAW128, SCE_GXM_TRANSFER_FORMAT_RAW64,
    SCE_GXM_TRANSFER_LINEAR, SCE_GXM_TRANSFER_SWIZZLED,
    TEX_VALID = 100, OBJ_NOT_USED = 200
};
typedef struct { uint32_t width, height, mips; SceGxmTextureFormat format; const void *data; } SceGxmTexture;
typedef struct {
    void *data, *palette_data;
    int status, mip_count, last_frame;
    GLboolean use_mips;
    SceGxmTexture gxm_tex;
} texture;

void *gpu_alloc_mapped_for_gpu(size_t);
void *gpu_alloc_mapped_temp(size_t);
void *vgl_realloc(void *, size_t);
void gpu_free_texture_data(texture *);
void vgl_memcpy(void *, const void *, size_t);
void vgl_fast_memcpy(void *, const void *, size_t);
void vgl_memset(void *, int, size_t);
void vglGetTexSizes(SceGxmTexture *, uint32_t *, uint32_t *);
void vglInitSwizzledTexture(SceGxmTexture *, const void *, SceGxmTextureFormat, unsigned, unsigned, unsigned);
uint8_t tex_format_to_alignment(SceGxmTextureFormat);
void dxt_compress(void *, uint8_t *, unsigned, unsigned, int);
void SwizzleTexData64Bpp(uint8_t *, uint8_t *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
void SwizzleTexData128Bpp(uint8_t *, uint8_t *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
void SwizzleTexDataETC1(uint8_t *, uint8_t *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
int sceGxmTransferCopy(unsigned, unsigned, unsigned, unsigned, int, int, int, void *, unsigned, unsigned, unsigned,
                       int, int, void *, unsigned, unsigned, unsigned, void *, unsigned, void *);
int sceGxmTransferFinish(void);
void gpu_alloc_compressed_texture(int32_t, uint32_t, uint32_t, SceGxmTextureFormat, uint32_t,
                                  const void *, texture *, uint8_t, GLboolean);
int test_gpu_mip_size(int, int, int);
int test_gpu_mip_offset(int, int, int);
int test_gpu_chain_size(int, int, int);
#ifdef __cplusplus
}
#endif
#ifndef __cplusplus
#include <math.h>
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define ALIGNBLOCK(x,a) (((x) + (a) - 1) & ~((a) - 1))
#endif
#endif
