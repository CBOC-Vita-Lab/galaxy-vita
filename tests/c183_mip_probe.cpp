// Compile the real Vita probe with a software transfer unit and fault injection.
#include "vgx_swizzle_layout.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int fault, calls, allocs;
static void* alloc(unsigned, size_t n, int) { return (++allocs == 2 && fault == 5) ? nullptr : std::malloc(n); }
static auto vglForceAllocAligned = &alloc;
static auto vglFree = &std::free;
static void XferWait() {}
static void port_log(const char*, ...) {}
enum { GL_TRUE=1, SCE_GXM_TRANSFER_FORMAT_U8U8_GR=2, SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR=4 };
static int sceGxmTransferDownscale(int bpp, const uint8_t* src, int, int, unsigned w, unsigned h, unsigned si,
                                   int, uint8_t* dst, int, int, unsigned so, void*, int, void*) {
    ++calls;
    if (fault == 1) return -1;
    if (fault == 2) return 0; // driver reports success but writes nothing
    for (unsigned y=0; y<h/2; ++y) for (unsigned x=0; x<w/2; ++x) for (int c=0; c<bpp; ++c) {
        const auto* t = src + 2*y*si + 2*x*bpp + c;
        dst[y*so+x*bpp+c] = fault == 3 ? 0 : (t[0]+t[bpp]+t[si]+t[si+bpp])/4;
    }
    if (fault == 4 && w == 4 && bpp == 2) dst[so+3] ^= 16; // tiny mip, padded stride
    return 0;
}
#include "probe.inc"
int main() {
    for (fault=0; fault<=5; ++fault) {
        calls=allocs=0;
        assert(vgxvita_la88MipsExact() == (fault == 0));
        if (fault == 0) assert(calls == 10);
    }
    std::puts("Probe: correct chain accepted through 2x2; refused/no-op/zero/tiny-row fault/OOM rejected");
}
