#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vitaGL.h>
#include <psp2/kernel/clib.h>
#include "authored-driver-types.h"
#include "texture_callbacks.h"

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define VGL_ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define OBJ_NOT_USED 0xFFFFFFFFu
#define FRAME_PURGE_FREQ 4
enum { TEX_UNUSED, TEX_UNINITIALIZED, TEX_VALID };
extern texture fixture_texture;
extern uint32_t vgl_framecount, unpack_row_len, vgl_aniso_min;
extern void* color_table;
void fixture_error(void);
#define SET_GL_ERROR(x) { fixture_error(); return; }
#define SET_GL_ERROR_WITH_VALUE(x,y) { fixture_error(); return; }
void *gpu_alloc_mapped_for_gpu(size_t n);
void *vgl_realloc(void*, size_t);
void gpu_free_texture_data(texture*);
void *vgl_memcpy(void*, const void*, size_t);
void *vgl_fast_memcpy(void*, const void*, size_t);
void *vgl_memset(void*, int, size_t);
void vgl_free(void*);
void vgl_log(const char*, ...);
void gpu_alloc_compressed_texture(int32_t,uint32_t,uint32_t,SceGxmTextureFormat,uint32_t,const void*,texture*,uint8_t,GLboolean);
void _glTexImage2D_CubeIMPL(texture*,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void*,int);
