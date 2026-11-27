// dive27 (13 Nov, GXV_REHEARSE; 0 off, the default in code, PROPOSED 1 in both modes; read by the renderer: vgx_draw.cpp RehearseMode): the game's half of REHEARSAL, "exercise every
// conditional effect path a stage can enter once, behind the closed loading wipe". The registry and the runner are platform/include/port/rehearse.h; this file is the driver and the
// entries that exist so far (water camera filter, wet ocean forms, the two glow chains). Other first-use paths join with `static port::rehearse::Registrar r("name", fn);` in their own file.
//
// When: PortRehearseSlice is called once a frame from JPAEmitterManager's draw (next to GXV_STAGE_ENUM's slice), only in frames whose screen is closed (port_screen_quiet() == 2: the wipe
// is 90 % closed or more; never in play, never in the pause menu), at most GXV_REHEARSE_FRAME_MS (4) of game time a frame, each entry once a scene. Every entry runs inside the renderer's
// warm-up scope (PORT_GX_MARK_WARM_BEGIN / _END): its draws are resolved (the same keys, textures looked up and uploaded, program variants queued for the back end's warm-up) but never
// recorded, so the picture is not touched; its captures are real copies (the copy target of the exact size, format and destination is made then: no premade guess), with the copy's
// "clear" stripped by the renderer inside a scope. The GX library's state and the draw counter are put back, as the effects' and layouts' warm-ups do.
// What an entry must not do: change game state (it calls the real draw members with nothing set that another part of the game reads), leave an emitter alive, use the RNG.
//
// The hold: port_rehearse_pending() (entries still to do, once the scene's first closed picture has come) joins the back end's closed-screen hold (StageWarmHold, capped by GXV_PREWARM_MS).
// Census: one line a scene when the entries are done: "rehearse: stage ...".
#ifdef TARGET_PC
#include "Game/Map/OceanRing.hpp"
#include "Game/Map/OceanRingDrawer.hpp"
#include "Game/Map/OceanBowl.hpp"
#include "Game/Map/OceanBowlBloomDrawer.hpp"
#include "Game/Map/WaterAreaHolder.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioHolder.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Screen/BloomEffect.hpp"
#include "Game/Screen/BloomEffectSimple.hpp"
#include "Game/Screen/WaterCameraFilter.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "port/compat.h"
#include "port/rehearse.h"
#include <revolution.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" int port_rehearse_mode(void) __attribute__((weak));
extern "C" int port_screen_quiet(void) __attribute__((weak));
extern "C" uint32_t port_scene_serial(void) __attribute__((weak));
extern "C" uint32_t port_scene_key(void) __attribute__((weak));
extern "C" uint64_t port_gx_recording_frame(void) __attribute__((weak));
extern "C" unsigned long long port_par_ptcl_now(void);
extern "C" void port_gx_marker(unsigned int kind);
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
extern "C" int port_res_request(int w, int h, int kind, int depth) __attribute__((weak));  // (stall27: queue a premade target, made in a closed-screen frame)
extern "C" u32 __PortGXDrawSerial;

namespace {
port::rehearse::Runner sRun;
int sMode = -1;
unsigned long long sFrameUs = 4000;
uint64_t sFrame = ~0ull;
GXData sSavedGx;
u32 sSavedSerial = 0;
unsigned sScopes = 0;

unsigned long long Now() {
    return port_par_ptcl_now();
}
void Open() {
    memcpy(&sSavedGx, gx, sizeof sSavedGx);
    sSavedSerial = __PortGXDrawSerial;
    port_gx_marker(PORT_GX_MARK_WARM_BEGIN);
    ++sScopes;
}
void Close() {
    port_gx_marker(PORT_GX_MARK_WARM_END);
    memcpy(gx, &sSavedGx, sizeof sSavedGx);
    __PortGXDrawSerial = sSavedSerial;
}

bool PlayerReady() {
    if (!MR::isExistSceneObj(SceneObj_MarioHolder)) {
        return false;
    }
    MarioHolder* h = MR::getMarioHolder();
    return h != nullptr && h->getMarioActor() != nullptr;
}
WaterAreaHolder* Holder() {
    return MR::isExistSceneObj(SceneObj_WaterAreaHolder) ? MR::getSceneObj< WaterAreaHolder >(SceneObj_WaterAreaHolder) : nullptr;
}
bool HasAreas(const WaterAreaHolder* h) {
    return h->mNumOceanBowls + h->mNumOceanRings + h->mNumOceanSpheres + h->mNumWhirlPools > 0;
}

// The camera filter: capture + material + quad, as draw() does once the camera is wet (its first use: the capture's copy target, the warp material's program, the filter texture's upload).
int RehearseWaterFilter() {
    WaterAreaHolder* h = Holder();
    if (h == nullptr || h->mCameraFilter == nullptr || !HasAreas(h)) {
        return port::rehearse::kSkip;
    }
    if (!PlayerReady()) {
        return port::rehearse::kRetry;
    }
    h->mCameraFilter->rehearse();
    return 1;
}

// The wet forms of the ocean: OceanBowl's 3-stage additive material (a program of its own), the underwater-only bloom materials of the bowl and the ring. (OceanSphere, WhirlPool and the
// ring's own surface have one form dry and wet; the dry forms are drawn from the stage's first picture.)
int RehearseWaterOcean() {
    WaterAreaHolder* h = Holder();
    if (h == nullptr || !HasAreas(h)) {
        return port::rehearse::kSkip;
    }
    if (!PlayerReady()) {
        return port::rehearse::kRetry;
    }
    int n = 0;
    for (s32 i = 0; i < h->mNumOceanBowls; ++i) {
        OceanBowl* b = h->mOceanBowls[i];
        if (b == nullptr || b->mPoints[0][0] == nullptr || b->mPoints[1][24] == nullptr) {
            continue;
        }
        b->rehearse();
        ++n;
        if (b->mBloomDrawer != nullptr) {
            b->mBloomDrawer->rehearse();
            ++n;
        }
    }
    for (s32 i = 0; i < h->mNumOceanRings; ++i) {
        OceanRing* r = h->mOceanRings[i];
        if (r != nullptr && r->mRingDrawer != nullptr && h->mUseBloom) {
            r->mRingDrawer->drawBloom();
            ++n;
        }
    }
    return n;
}

// The glow chains of the stage (any stage that made one: BloomArea / water for the normal bloom, SimpleBloom areas for the simple one): their copy targets and programs.
int RehearseBloomNormal() {
    if (!MR::isExistSceneObj(SceneObj_BloomEffect)) {
        return port::rehearse::kSkip;
    }
    MR::getSceneObj< BloomEffect >(SceneObj_BloomEffect)->portRehearse();
    if (port_res_request != nullptr) {  // (the chain's tile passes: 1/2, 1/4, 1/8 of the picture, no depth; best effort, the queue refuses what does not fit)
        for (int d = 2; d <= 8; d *= 2) {
            port_res_request(MR::getFrameBufferWidth() / d, MR::getFrameBufferHeight() / d, 1, 0);
        }
    }
    return 1;
}
int RehearseBloomSimple() {
    if (!MR::isExistSceneObj(SceneObj_BloomEffectSimple)) {
        return port::rehearse::kSkip;
    }
    MR::getSceneObj< BloomEffectSimple >(SceneObj_BloomEffectSimple)->portRehearse();
    return 1;
}

port::rehearse::Registrar sRegFilter("water.camera-filter", RehearseWaterFilter);
port::rehearse::Registrar sRegOcean("water.wet-ocean", RehearseWaterOcean);
port::rehearse::Registrar sRegBloom("image.bloom-normal", RehearseBloomNormal);
port::rehearse::Registrar sRegBloomSimple("image.bloom-simple", RehearseBloomSimple);

bool On() {
    if (sMode < 0) {
        sMode = port_rehearse_mode ? port_rehearse_mode() : 0;
        const char* f = getenv("GXV_REHEARSE_FRAME_MS");
        const int ms = f != nullptr ? atoi(f) : 4;
        sFrameUs = static_cast< unsigned long long >(ms < 1 ? 1 : ms > 200 ? 200 : ms) * 1000ull;
    }
    return sMode > 0;
}

void Census() {
    if (sRun.logged || !port_log_summary) {
        return;
    }
    sRun.logged = true;
    char list[360];
    int len = 0;
    list[0] = '\0';
    static const char* const stateName[4] = {"todo", "done", "skipped", "gave up"};
    for (unsigned i = 0; i < port::rehearse::g_reg.n && len < static_cast< int >(sizeof list) - 64; ++i) {
        const port::rehearse::Result& r = sRun.r[i];
        len += snprintf(list + len, sizeof list - len, "%s%s %s (%u draws, %.1f ms, %u tries)", i ? "; " : "", port::rehearse::g_reg.e[i].name, stateName[r.state & 3], r.draws,
                        r.us / 1000.0, r.tries);
    }
    port_log_summary("rehearse: stage %08x: %u entries: %u done, %u skipped (nothing for this stage), %u gave up; %.1f ms of game thread over %u closed pictures, %u warm-up scopes: %s",
                     port_scene_key ? port_scene_key() : 0u, port::rehearse::g_reg.n, sRun.Count(port::rehearse::kDone), sRun.Count(port::rehearse::kSkipped),
                     sRun.Count(port::rehearse::kGaveUp), static_cast< double >(sRun.usTotal) / 1000.0, sRun.frames, sScopes, list);
}
}  // namespace

// (JPAEmitterManager::draw -> PortPrewarmGroup) once a frame, behind a closed screen only.
void PortRehearseSlice() {
    if (!On() || !port_screen_quiet || port_screen_quiet() != 2) {
        return;
    }
    const uint32_t serial = port_scene_serial ? port_scene_serial() : 0;
    if (serial == 0) {
        return;
    }
    const uint64_t frame = port_gx_recording_frame ? port_gx_recording_frame() : 0;
    if (frame == sFrame) {
        return;  // (once a frame)
    }
    sFrame = frame;
    sRun.Sync(serial);
    if (sRun.Pending() == 0) {
        sRun.started = true;
        Census();
        return;
    }
    sRun.Step(Now, sFrameUs, Open, Close);
    if (sRun.Pending() == 0) {
        Census();
    }
}

// (the back end's StageWarmHold) entries not done yet, once the scene's first closed picture has come.
extern "C" unsigned port_rehearse_pending(void) {
    if (!On() || !sRun.valid || !sRun.started) {
        return 0;
    }
    return sRun.Pending();
}
#endif
