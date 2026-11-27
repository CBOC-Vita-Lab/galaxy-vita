// texwarm30 (GXV_TEXWARM_KEY, read by the renderer: vgx_texcache.cpp; the walk runs only while it says so): the game's half of "tell the texture warm-up WHERE each texture of this stage's
// models lies in memory now". The warm-up's book knows a texture by what it is (size, format, hash of its bytes), not by where it lay last visit: the addresses of a stage's archives
// change at every load. To replay a book item behind the cover the renderer needs the stage's textures by address; they are in the J3DModelData of every resource archive the stage
// has loaded (J3DTexture: an array of ResTIMG; the image is at resTIMG + mImageDataOffset, which is exactly the pointer J3DSys loadTexNo hands to the GX state: J3DTevs.cpp).
//
// The walk. The same one PortStageEnum.cpp makes (every ResourceHolder the ResourceHolderManager holds, in the order they were added; every model of the holder), here every texture of the
// model's texture table: port_texwarm_cand(key, serial, image, width, height, format, mip count, min filter, max lod, order) once each. Reading only: it writes nothing into the GX
// state, draws nothing, and needs no warm-up scope. At the end port_texwarm_cand_done(key, serial, count). A holder added later (an archive that loads after the scene was built)
// is walked when the manager grows. Texture tables of one model data shared by two holders are reported twice (the renderer keeps one by address).
//
// When: once a frame, at the particle manager's first draw group (the call PortStageEnumSlice makes), at most 512 textures and 2 ms (0.4 ms in play) of the game thread a frame.
// What it does NOT see (the renderer counts them as "first used in play, no candidate"): textures made by code (fur, Mii faces, the screen captures, DisplayListMaker),
// particle textures (JPA), layout textures (J2D BTI / the HUD), textures of models created from archives after the walk, palette textures (not keyed).
#ifdef TARGET_PC
#include "Game/System/ResourceHolder.hpp"
#include "Game/System/ResourceHolderManager.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <revolution.h>
#include <stdint.h>

#include "port/offset32.h"

extern "C" int port_texwarm_cand_mode(void) __attribute__((weak));
extern "C" void port_texwarm_cand(uint32_t key, uint32_t serial, const void* image, unsigned w, unsigned h, unsigned fmt, unsigned mips, unsigned minFilter, unsigned maxLod8, unsigned order)
    __attribute__((weak));
extern "C" void port_texwarm_cand_done(uint32_t key, uint32_t serial, unsigned count) __attribute__((weak));
extern "C" int port_screen_quiet(void) __attribute__((weak));
extern "C" uint32_t port_scene_key(void) __attribute__((weak));
extern "C" uint32_t port_scene_loading_key(void) __attribute__((weak));
extern "C" uint32_t port_scene_serial(void) __attribute__((weak));
extern "C" unsigned long long port_par_ptcl_now(void);

namespace {
struct PortTexWalk {
    int mode = -1;
    uint32_t key = 0, serial = 0;
    u32 holderCount = 0;   // the manager's size when the walk last began (it grows while a stage loads)
    u32 hi = 0, mi = 0, ti = 0;
    bool done = false;
    unsigned count = 0;    // textures reported so far for this key
    unsigned order = 0;
};
PortTexWalk sTw;

void Restart(uint32_t key, uint32_t serial) {
    const int mode = sTw.mode;
    sTw = PortTexWalk();
    sTw.mode = mode;
    sTw.key = key;
    sTw.serial = serial;
}
}  // namespace

// (JPAEmitterManager::draw -> PortPrewarmGroup) a slice of the walk.
void PortTexEnumSlice() {
    if (sTw.mode < 0)
        sTw.mode = port_texwarm_cand_mode ? port_texwarm_cand_mode() : 0;
    if (sTw.mode == 0 || !port_texwarm_cand)
        return;
    const uint32_t loading = port_scene_loading_key ? port_scene_loading_key() : 0;
    const uint32_t key = loading ? loading : (port_scene_key ? port_scene_key() : 0);
    if (key == 0)
        return;
    const uint32_t serial = port_scene_serial ? port_scene_serial() : 0;
    ResourceHolderManager* mgr = SingletonHolder< ResourceHolderManager >::get();
    if (mgr == NULL)
        return;
    if (key != sTw.key || serial != sTw.serial || (sTw.holderCount != 0 && mgr->portHolderCount() < sTw.holderCount))
        Restart(key, serial);
    if (sTw.done) {
        if (mgr->portHolderCount() <= sTw.holderCount)
            return;      // (nothing new)
        sTw.done = false;  // (an archive was added since: walk again from the first holder; the renderer keeps one candidate an address)
        sTw.hi = sTw.mi = sTw.ti = 0;
    }
    sTw.holderCount = mgr->portHolderCount();
    const bool closed = port_screen_quiet ? port_screen_quiet() != 0 : true;
    const unsigned long long budgetUs = closed ? 2000ull : 400ull;
    const unsigned long long t0 = port_par_ptcl_now();
    unsigned n = 0;
    while (n < 512 && !sTw.done) {
        if (sTw.hi >= mgr->portHolderCount()) {
            sTw.done = true;
            break;
        }
        ResourceHolder* rh = mgr->portHolderAt(sTw.hi);
        ResTable* t = rh != NULL ? rh->mModelResTable : NULL;
        if (t == NULL || sTw.mi >= t->mCount) {
            ++sTw.hi;
            sTw.mi = 0;
            sTw.ti = 0;
            continue;
        }
        J3DModelData* md = static_cast< J3DModelData* >(t->getRes(sTw.mi));
        J3DTexture* tex = md != NULL ? md->getTexture() : NULL;
        if (tex == NULL || sTw.ti >= tex->getNum()) {
            ++sTw.mi;
            sTw.ti = 0;
            continue;
        }
        const ResTIMG* img = tex->getResTIMG(static_cast< u16 >(sTw.ti++));
        if (img == NULL || img->mWidth == 0 || img->mHeight == 0)
            continue;
        const void* data = port_offset32(img, img->mImageDataOffset);
        port_texwarm_cand(key, serial, data, img->mWidth, img->mHeight, static_cast< unsigned >(img->mFormat & 0x0f), img->mImageNum, img->mMinType, img->mMaxLod, sTw.order++);
        ++sTw.count;
        ++n;
        if ((n & 63) == 0 && port_par_ptcl_now() - t0 >= budgetUs)
            break;
    }
    if (sTw.done && port_texwarm_cand_done)
        port_texwarm_cand_done(key, serial, sTw.count);
}
#endif
