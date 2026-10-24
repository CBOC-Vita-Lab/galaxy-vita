#pragma once
#include "vita_material_cache.h"
#include "gx_ram_texture.h"
#include "port/vi.h"
#include <cstdio>

namespace gpu {
struct VitaXfbPresent {
    PortViDisplay display{};
    uint32_t address=0;
    bool black=true,hasFramebuffer=false;
};
struct VitaXfbDisplay;
int createVitaXfbDisplay(PortVitaGxmDevice*,const VitaShaderLibrary&,VitaXfbDisplay**,std::string&);
// RAM is borrowed only while the guest blocks. Decode/upload actual selected
// YUYV; never present a retained EFB copy packet or the source EFB directly.
int presentVitaXfb(VitaXfbDisplay*,const VitaXfbPresent&,const GxTextureMemory&,std::string&);
bool vitaXfbSceneOpen(const VitaXfbDisplay*);
int discardVitaXfb(VitaXfbDisplay*);
int releaseVitaXfbDisplay(VitaXfbDisplay**);
// Diagnostic PAM output of decoded CPU pixels, not a GXM scanout capture.
bool writeVitaXfbSnapshot(FILE*,const unsigned char*,uint32_t,uint32_t,size_t);
class VitaXfbCapture {
public:
    void configure(FILE* marker);
    // Count successful non-black selected-XFB presentations only. Zero skips,
    // one saves, negative fails; an output failure never retries that slot.
    int capture(const char* directory,const unsigned char*,uint32_t,uint32_t,size_t);
    const char* tag() const { return captureTag; }
    unsigned present() const { return capturePresent; }
private:
    char captureTag[5]{};
    unsigned capturePresent=0;
};
}
