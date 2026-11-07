// Only SDK transfers and heap boundaries are doubles. Upload/offset code is extracted unchanged.
#include "vita_authored_mips_fixture.h"
#include <stdio.h>

texture fixture_texture;
uint32_t vgl_framecount, unpack_row_len, vgl_aniso_min;
void* color_table;
static void* allocation;
static size_t allocation_size;
static int fallback;
static unsigned transfers, waits, copies, reallocations;
typedef struct { const uint8_t* src; uint8_t* dst; unsigned w,h,ss,ds; } Transfer;
static Transfer pending[16];
static unsigned pending_count;
static void check(int ok, const char* why) { if (!ok) { fprintf(stderr,"FAIL: driver fixture %s\n",why); abort(); } }
void fixture_error(void) { check(0,"unexpected GL error"); }
static void range(const void* p,size_t n) {
    check(allocation && (uintptr_t)p >= (uintptr_t)allocation &&
          (uintptr_t)p - (uintptr_t)allocation <= allocation_size &&
          n <= allocation_size - ((uintptr_t)p - (uintptr_t)allocation), "access inside live allocation");
}
static void idle(void) { check(!pending_count,"CPU writes or allocation cannot race pending downscale"); }
void* gpu_alloc_mapped_for_gpu(size_t n) { idle(); check(!allocation,"one live image"); allocation=malloc(n); allocation_size=n; check(allocation!=NULL,"host allocation"); memset(allocation,0xcd,n); return allocation; }
void* vgl_realloc(void* p,size_t n) {
    idle(); check(p==allocation,"realloc ownership"); ++reallocations;
    if(fallback) return NULL;
    void* q=malloc(n); check(q!=NULL,"host reallocation"); memset(q,0xcd,n); memcpy(q,p,MIN(n,allocation_size)); free(p); allocation=q;allocation_size=n;return q;
}
void gpu_free_texture_data(texture* t) { idle(); check(t->data==allocation,"retire owned image"); free(allocation);allocation=NULL;allocation_size=0;t->data=NULL; }
void* vgl_memcpy(void* d,const void* s,size_t n) { idle(); range(d,n); return memcpy(d,s,n); }
void* vgl_fast_memcpy(void* d,const void* s,size_t n) { idle(); range(d,n); ++copies; return memcpy(d,s,n); }
void* vgl_memset(void* d,int c,size_t n) { idle();range(d,n);return memset(d,c,n); }
void* sceClibMemcpy(void* d,const void* s,SceSize n) { idle();range(d,n);range(s,n);return memcpy(d,s,n); }
void* vglMalloc(uint32_t n) { (void)n; check(0,"unexpected compressed conversion");return NULL; }
void vgl_free(void* p) { (void)p;check(0,"unexpected conversion free"); }
void vgl_log(const char* f,...) { (void)f;check(0,"unexpected driver log"); }
void gpu_alloc_compressed_texture(int32_t l,uint32_t w,uint32_t h,SceGxmTextureFormat f,uint32_t n,const void* p,texture* t,uint8_t b,GLboolean u) { (void)l;(void)w;(void)h;(void)f;(void)n;(void)p;(void)t;(void)b;(void)u;check(0,"compressed route must not handle gradual alpha"); }
void _glTexImage2D_CubeIMPL(texture* t,GLint l,GLint i,GLsizei w,GLsizei h,GLenum f,GLenum y,const void* p,int x) { (void)t;(void)l;(void)i;(void)w;(void)h;(void)f;(void)y;(void)p;(void)x;check(0,"unexpected cube route"); }
int sceGxmTextureSetData(SceGxmTexture* t,const void* p) { (void)t;(void)p;check(0,"fresh upload must not enter in-use subimage copy");return 0; }
int sceGxmTextureSetWidth(SceGxmTexture* t,unsigned w) { (void)t;(void)w;check(0,"unexpected compressed width");return 0; }
int sceGxmTextureSetHeight(SceGxmTexture* t,unsigned h) { (void)t;(void)h;check(0,"unexpected compressed height");return 0; }
int sceGxmTransferDownscale(SceGxmTransferFormat sf,const void* s,unsigned sx,unsigned sy,unsigned w,unsigned h,int ss,SceGxmTransferFormat df,void* d,unsigned dx,unsigned dy,int ds,SceGxmSyncObject* sync,unsigned flags,const SceGxmNotification* notification) {
    check(sf==SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR && df==sf && !sx&&!sy&&!dx&&!dy&&!sync&&!flags&&!notification,"exact RGBA transfer format");
    range(s,(h-1)*ss+w*4);range(d,(h/2-1)*ds+w/2*4);check(pending_count<16,"bounded pending chain");
    pending[pending_count++]=(Transfer){s,d,w,h,ss,ds};++transfers;return 0;
}
int sceGxmTransferFinish(void) {
    ++waits;
    for(unsigned i=0;i<pending_count;++i) {
        Transfer* q=&pending[i];
        for(unsigned y=0;y<q->h/2;++y) for(unsigned x=0;x<q->w/2;++x) for(unsigned c=0;c<4;++c) {
            unsigned sum=0;for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx)sum+=q->src[(2*y+dy)*q->ss+(2*x+dx)*4+c];
            q->dst[y*q->ds+x*4+c]=sum/4;
        }
    }
    pending_count=0;return 0;
}
void fixture_reset(void) { idle();free(allocation);allocation=NULL;allocation_size=0;memset(&fixture_texture,0,sizeof fixture_texture);fixture_texture.use_mips=GL_TRUE;fixture_texture.last_frame=OBJ_NOT_USED;transfers=waits=copies=reallocations=0;fallback=0; }
void fixture_mark_used(void) { fixture_texture.last_frame=vgl_framecount; }
unsigned fixture_mips(void) { return fixture_texture.mip_count; }
unsigned fixture_descriptor_mips(void) { return ((((uint32_t*)&fixture_texture.gxm_tex)[0]>>17)&15)+1; }
unsigned fixture_waits(void) { return waits; }
unsigned fixture_transfers(void) { return transfers; }
unsigned fixture_allocations(void) { return reallocations; }
void fixture_pixels(unsigned level,unsigned w,unsigned h,void* out) {
    idle();const uint32_t word=((uint32_t*)&fixture_texture.gxm_tex)[1];unsigned basew=((word>>12)&4095)+1,baseh=(word&4095)+1;
    check(w==MAX(1u,basew>>level)&&h==MAX(1u,baseh>>level),"oracle dimensions independent of driver offsets");
    size_t offset=0;for(unsigned i=0;i<level;++i) offset+=MAX(basew>>i,8u)*(baseh>>i)*4;
    const unsigned stride=MAX(w,8u)*4;const uint8_t* src=(uint8_t*)fixture_texture.data+offset;
    for(unsigned y=0;y<h;++y) {range(src+y*stride,w*4);memcpy((uint8_t*)out+y*w*4,src+y*stride,w*4);}
}
