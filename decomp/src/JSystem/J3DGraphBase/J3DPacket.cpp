#include "JSystem/J3DGraphBase/J3DPacket.hpp"
#include "JSystem/J3DGraphAnimator/J3DModel.hpp"
#include "JSystem/J3DGraphAnimator/J3DModelData.hpp"
#include "JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp"
#include "JSystem/J3DGraphBase/J3DDrawBuffer.hpp"
#include "JSystem/J3DGraphBase/J3DMaterial.hpp"
#include "JSystem/J3DGraphBase/J3DShapeMtx.hpp"
#include "JSystem/JKernel/JKRHeap.hpp"
#include <cstdio>
#include <mem.h>
#include "port/hot_knob.h"
#ifdef TARGET_PC
#include "port/stream_census.h"  // (GXV_STREAM_CENSUS: J3DMatPacket::draw)
#include "port/array_skip.h"  // (GXV_ARRAYSKIP: J3DDisplayListObj::callDL)
#include "port/gx_dedup.h"    // (GXV_GX_DEDUP: J3DDisplayListObj::callDL)
#endif

#ifdef TARGET_PC
// (GXV_J3DNATIVE) the diff flags of the J3DMaterial::diff in progress (the material path runs one at a time: PortDlGameEnter), which the
// list it writes records as the kinds of animation that changed it (J3DDisplayListObj::mPortKinds).
static u32 sPortPendingKinds = 0;
void J3DPortSetDiffKinds(u32 kinds) { sPortPendingKinds = kinds & 0x00FFFFFFu; }
u32 gJ3DPortVer[J3DPortVer_Count];
u32 gJ3DPortCensus[J3DPortCen_Count];
bool J3DDisplayListObj::portKeepIfSame(u32 prevSize) {
    if (J3DPortNativeMode() == 0 || mpDisplayList[0] == mpDisplayList[1] || mpDisplayList[1] == nullptr || mSize != prevSize ||
        memcmp(mpDisplayList[0], mpDisplayList[1], mSize) != 0) {
        return false;
    }
    void* pTmp = mpDisplayList[0];
    mpDisplayList[0] = mpDisplayList[1];
    mpDisplayList[1] = pTmp;
    return true;
}
extern "C" int __PortGXJ3nMode(void);  // gx_fifo.cpp
extern "C" int __PortJ3nEmitOn;           // GXDisplayList.c: the unbracketed-emitter census (GXV_J3DNATIVE >= 1)
extern "C" void port_log_summary(const char* fmt, ...);  // (port_log is a no-op in the player build; the census lines below go to log.txt)
struct PortJ3nEmit { const void* ra; u32 kind, calls, amount; };  // (GXDisplayList.c's)
extern "C" int __PortJ3nEmitTake(PortJ3nEmit* out, int max, u32* lost);
extern "C" void GXCallDisplayList(const void* list, u32 nbytes);
static void J3DPortEmitLog(unsigned pictures) {
    PortJ3nEmit top[12];
    u32 lost = 0;
    const int n = __PortJ3nEmitTake(top, 12, &lost);
    char buf[1100];
    int len = snprintf(buf, sizeof(buf), "j3n game: top unbracketed emitters per picture, call (list calls, bytes) / begin (GXBegin, vertices) @ the caller as an "
                       "offset from GXCallDisplayList (addr2line -f -C -e <elf> at GXCallDisplayList + offset):");
    for (int i = 0; i < n && len < static_cast< int >(sizeof(buf)) - 80; ++i) {
        const long off = static_cast< long >(reinterpret_cast< uintptr_t >(top[i].ra) - reinterpret_cast< uintptr_t >(&GXCallDisplayList));
        len += snprintf(buf + len, sizeof(buf) - len, " %s@%+ld %.1f (%.0f %s)", top[i].kind ? "begin" : "call", off,
                        top[i].calls / static_cast< double >(pictures), top[i].amount / static_cast< double >(top[i].calls),
                        top[i].kind ? "verts" : "bytes");
    }
    if (lost) snprintf(buf + len, sizeof(buf) - len, "; %u not counted (table full)", static_cast< unsigned >(lost));
    port_log_summary("%s", buf);
}
// (GXV_J3DNATIVE, J3DPortNative.hpp; the switch is read once, gx_fifo.cpp)
int J3DPortNativeMode() { return __PortGXJ3nMode(); }
#endif

J3DError J3DDisplayListObj::newDisplayList(u32 maxSize) {
    mMaxSize = ALIGN_NEXT(maxSize, 0x20);
    mpDisplayList[0] = new (0x20) char[mMaxSize];
    mpDisplayList[1] = new (0x20) char[mMaxSize];
    mSize = 0;
#ifdef TARGET_PC
    portBump();
    J3DPortCensusAdd(J3DPortCen_OtherBumps);
#endif

    return kJ3DError_Success;
}

J3DError J3DDisplayListObj::newSingleDisplayList(u32 maxSize) {
    mMaxSize = ALIGN_NEXT(maxSize, 0x20);
    mpDisplayList[0] = new (0x20) char[mMaxSize];
    mpDisplayList[1] = mpDisplayList[0];
    mSize = 0;
#ifdef TARGET_PC
    portBump();
    J3DPortCensusAdd(J3DPortCen_OtherBumps);
#endif

    return kJ3DError_Success;
}

int J3DDisplayListObj::single_To_Double() {
    if (mpDisplayList[0] == mpDisplayList[1]) {
        mpDisplayList[1] = new (0x20) char[mMaxSize];

        memcpy(mpDisplayList[1], mpDisplayList[0], mMaxSize);
        DCStoreRange(mpDisplayList[1], mMaxSize);
#ifdef TARGET_PC
        portBump();
        J3DPortCensusAdd(J3DPortCen_OtherBumps);
#endif
    }

    return kJ3DError_Success;
}

void J3DDisplayListObj::setSingleDisplayList(void* pDLData, u32 size) {
    mMaxSize = ALIGN_NEXT(size, 0x20);
    mpDisplayList[0] = pDLData;
    mpDisplayList[1] = mpDisplayList[0];
    mSize = size;
#ifdef TARGET_PC
    portBump();
    J3DPortCensusAdd(J3DPortCen_OtherBumps);
#endif
}

void J3DDisplayListObj::swapBuffer() {
    void* pTmp = mpDisplayList[0];
    mpDisplayList[0] = mpDisplayList[1];
    mpDisplayList[1] = pTmp;
    // (TARGET_PC, GXV_J3DNATIVE: no bump here: a swap alone changes no bytes; the write that follows it (endDL, endPatch, endDiffDL) bumps
    // when the bytes it leaves differ)
}

void J3DDisplayListObj::callDL() const {
#ifdef TARGET_PC
    // (GXV_ARRAYSKIP, port/array_skip.h: a material or differed list, made by J3D's GD writers, holds no CP command)
    __PortArrKeep = 1;
    PORT_GXDD_KEEP(PORT_GXDD_BUMP_NOCP);  // (GXV_GX_DEDUP, port/gx_dedup_core.h: BP / XF forgotten after it, CP kept)
    GXCallDisplayList(mpDisplayList[0], mSize);
    PORT_GXDD_KEEP(0);
    __PortArrKeep = 0;
#else
    GXCallDisplayList(mpDisplayList[0], mSize);
#endif
}

#ifdef TARGET_PC
// GXV_PAR_DL (gamedraw23; J3DSys.hpp, ModelManager.cpp): the J3DSys of the material path, and whether the overlap is open. Guest-OS calls
// (interrupts) are left out while it is: the game thread holds the interrupts off for the whole overlap, and the worker is no guest thread.
J3DSys* gPortJ3DSysMat = &j3dSys;
J3DSys* gPortJ3DSysDraw = &j3dSys;  // (GXV_DRAWPIPE: the draw path's J3DSys, J3DSys.hpp)
// (GXV_DRAWPIPE=4: the packet prepareDraw last set its shape up for. When a recorded draw is ended in the middle of a model the
// game thread draws by hand, the passes executed there set the same shapes up for their own packets; DrawBufferHolder.cpp
// prepares this one again afterwards.)
const J3DShapePacket* gPortPipePrepared = nullptr;
bool gPortParDlRun = false;
unsigned long gPortParDlStackLo = 0, gPortParDlStackHi = 0;
extern "C" __attribute__((weak)) void port_par_dl_game_enter(void) {}
#define PORT_DL_GUEST_OFF() (gPortParDlRun)
#else
#define PORT_DL_GUEST_OFF() (false)
#endif

void J3DDisplayListObj::beginDL() {
    PortDlGameEnter();  // (GXV_PAR_DL: GD's current list and sGDLObj are the material path's)
    swapBuffer();
    sInterruptFlag = PORT_DL_GUEST_OFF() ? 0 : OSDisableInterrupts();
    GDInitGDLObj(&sGDLObj, (u8*)mpDisplayList[0], mMaxSize);
    GDSetCurrent(&sGDLObj);
}

u32 J3DDisplayListObj::endDL() {
    GDPadCurr32();
    if (!PORT_DL_GUEST_OFF()) {
        OSRestoreInterrupts(sInterruptFlag);
    }
#ifdef TARGET_PC
    const u32 prevSize = mSize;
#endif
    mSize = GDGetGDLObjOffset(&sGDLObj);
    GDFlushCurrToMem();
    GDSetCurrent(NULL);
#ifdef TARGET_PC
    J3DPortCensusAdd(J3DPortCen_EndDL);
    if (portKeepIfSame(prevSize)) {
        J3DPortCensusAdd(J3DPortCen_EndDLSame);
    } else {
        portBump(sPortPendingKinds);
    }
#endif
    return mSize;
}

void J3DDisplayListObj::beginPatch() {
    beginDL();
}

u32 J3DDisplayListObj::endPatch() {
    if (!PORT_DL_GUEST_OFF()) {
        OSRestoreInterrupts(sInterruptFlag);
    }
    GDSetCurrent(NULL);
#ifdef TARGET_PC
    J3DPortCensusAdd(J3DPortCen_EndPatch);
    if (portKeepIfSame(mSize)) {
        J3DPortCensusAdd(J3DPortCen_EndPatchSame);
    } else {
        portBump();
    }
#endif
    return mSize;
}

#ifdef TARGET_PC
// ============================================================== (1.3 port) game-side cuts of the J3D state lists
// The front end (core 2) is the limit of the frame rate, and what it costs is mostly per list call (finding the list's entry,
// its generation, the replay), not per register. Three exact cuts of what the draw pass sends, each behind a switch read once:
//   GXV_J3D_DIFFKEEP=1  (here) J3DMaterial::diff makes a differed list (J3DShapePacket's: the animated TEV colours, texture
//                       matrices, fog, ... of one instance) on every drawn turn, into the other of its two buffers, mostly with the
//                       bytes the current one holds (stopped animations, fog). It is now made aside and stored, in the other buffer
//                       as before, only when the bytes differ; otherwise nothing is written and the current buffer stays. The
//                       front end then finds the list where it was, its memory not written since (no compare of its bytes, nor of
//                       the material lists sharing its 4 KiB), and the game makes no write notification for it.
//   GXV_J3D_MATMERGE=1  (DrawBuffer.cpp) the material list and the first packet's differed list, sent one after the other, become
//                       one list call when the differed list has not changed since the last picture.
//   GXV_J3D_DIFFSKIP=1  (DrawBuffer.cpp) a differed list with the bytes of the one sent just before it (instances of a model in the
//                       same state) is not sent when nothing between could have changed what it writes.
// The front end receives the same register writes in an order with the same effect: see J3DPortDL.hpp and DrawBuffer.cpp.
#include <cstdlib>
#include <cstring>
extern "C" void port_log_summary(const char* fmt, ...);
extern "C" void port_log_summary(const char* fmt, ...);
// (Game/LiveActor/ModelManager.cpp: the actor whose lists are being made, or null; weak: J3D alone, in a host test, has none)
extern "C" __attribute__((weak)) const char* port_dl_owner_name(void);
#include "JSystem/J3DGraphBase/J3DSys.hpp"

namespace {
int j3dPortEnv(const char* pName, int fallback) {
    const char* e = getenv(pName);
    return (e != nullptr && *e != '\0') ? atoi(e) : fallback;
}
u32 sJ3DPortPicture = 1;
// (one list made aside at a time: J3DDisplayListObj::sGDLObj, which it uses, is one too)
u8 sDiffScratch[0x1000] __attribute__((aligned(32)));
bool sDiffScratchOn = false;
bool sDiffScratchKeep = false;  // (GXV_J3D_DIFFKEEP was on when the list was begun)
unsigned sDLOverflows = 0;
J3DPortListStats sJ3DPortLogBase;
}  // namespace

J3DPortListStats gJ3DPortListStats;

// (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h): every material diff, material packet and shape drawer asks.
bool J3DPortDiffKeepOn() {
    return PORT_HOT_KNOB(bool, j3dPortEnv("GXV_J3D_DIFFKEEP", 1) != 0);
}
bool J3DPortMatMergeOn() {
    return PORT_HOT_KNOB(bool, j3dPortEnv("GXV_J3D_MATMERGE", 1) != 0);
}
bool J3DPortDiffSkipOn() {
    return PORT_HOT_KNOB(bool, j3dPortEnv("GXV_J3D_DIFFSKIP", 1) != 0);
}
// GXV_STARBATCH (starbatch25): see J3DPacket.hpp. Asked once a drawer drawn (hot knob).
J3DPortSortStats gJ3DPortSortStats;
int J3DPortSortMode() {
    return PORT_HOT_KNOB(int, j3dPortEnv("GXV_STARBATCH", 0));
}
u32 J3DPortSortMin() {
    return PORT_HOT_KNOB(u32, static_cast< u32 >(j3dPortEnv("GXV_STARBATCH_MIN", 4) < 3 ? 3 : j3dPortEnv("GXV_STARBATCH_MIN", 4)));
}
// GXV_DL_GUARD (dlcrash, 8 Nov; default 1): a differed list (J3DMaterial::diff) is always made aside, in the scratch buffer, also with
// GXV_J3D_DIFFKEEP=0, and one longer than its buffer (J3DShapePacket::newDifferedDisplayList sized it from the diff flags it had then)
// is not stored: one "FATAL: display list overflow" line names it, and the buffer keeps its last list (the bytes are still right for
// the model they were made for). GD writes have no bounds check, on the Wii either (GDOverflowCheck only calls a callback nobody sets):
// a longer list overwrites what follows its buffer, here the next J3DDisplayListObj (the Bee Mushroom crash: see
// Game/LiveActor/ModelManager.cpp, GXV_DL_LAZY_KEY, for its cause). Costs one compare per differed list. 0 = as before.
bool J3DPortDLGuardOn() {
    return PORT_HOT_KNOB(bool, j3dPortEnv("GXV_DL_GUARD", 1) != 0);
}
static void J3DPortDLOverflow(const J3DDisplayListObj* obj, u32 size, u32 maxSize, bool written) {
    ++sDLOverflows;
    if (sDLOverflows > 8 && (sDLOverflows & 1023) != 0) {
        return;
    }
    const char* owner = port_dl_owner_name != nullptr ? port_dl_owner_name() : nullptr;
    port_log_summary("FATAL: display list overflow: a differed list of %u bytes for a %u-byte buffer (list object %p, material packet %p, "
                     "model %p, actor \"%s\", %u so far): %s",
                     static_cast< unsigned >(size), static_cast< unsigned >(maxSize), static_cast< const void* >(obj),
                     static_cast< void* >(J3D_MSYS.getMatPacket()), static_cast< void* >(J3D_MSYS.getModel()), owner != nullptr ? owner : "?",
                     sDLOverflows, written ? "written in place, the memory after it is damaged" : "not stored, the buffer keeps its last list");
}
u32 J3DPortPicture() {
    return sJ3DPortPicture;
}

// GameSystem::frameLoop, before a picture is drawn. The counters, per picture over the last 300 pictures.
extern "C" int port_starbatch_dl_mode(void);  // (ModelManager.cpp)
extern "C" void port_starbatch_dl_counts(unsigned* out);
extern "C" int port_starbatch_far_on(void);  // (ActorTurnLod.cpp)
extern "C" void port_starbatch_far_counts(unsigned* out);
extern "C" void port_vgx_verify_summary(void) __attribute__((weak));  // (weak: the host tests that link this file have no vgx)
// platform/vgx/src/vgx_state.cpp (verifyfix31: GXV_NOOP_FILTER=2 / GXV_RT_INST=2 counts in the game log)
extern "C" void port_j3d_picture_begin(void) {
    ++sJ3DPortPicture;
    if (sJ3DPortPicture % 300 != 0) {
        return;
    }
    if (port_vgx_verify_summary != nullptr) {
        port_vgx_verify_summary();
    }
    const J3DPortListStats& c = gJ3DPortListStats;
    const J3DPortListStats& b = sJ3DPortLogBase;
    const u32 drawers = c.drawers - b.drawers;
    if (drawers != 0 || c.diffMade != b.diffMade) {
        const double n = 300.0;
        port_log_summary("j3d lists: per picture %.1f material lists (%.1f merged with a differed list, GXV_J3D_MATMERGE), differed lists %.1f sent, "
                 "%.1f merged, %.1f not sent (GXV_J3D_DIFFSKIP); merged lists made %.2f; differed lists made %.1f, %.1f of them unchanged "
                 "and not stored (GXV_J3D_DIFFKEEP)",
                 drawers / n, (c.matMerged - b.matMerged) / n, (c.diffSent - b.diffSent) / n, (c.diffMerged - b.diffMerged) / n,
                 (c.diffSkipped - b.diffSkipped) / n, (c.mergeWrites - b.mergeWrites) / n, (c.diffMade - b.diffMade) / n,
                 (c.diffKept - b.diffKept) / n);
    }
    sJ3DPortLogBase = c;
    if (J3DPortSortMode() != 0) {
        // (starbatch25) per 300 pictures (about 5 s): what the drawer planner did. "records" are GL draws the equal-list runs make at 10 a record
        // (GXV_STATIC_INST / GXV_RIGIDMERGE=1), before and after: the draws the order saves once the back end joins runs.
        static J3DPortSortStats base;
        const J3DPortSortStats& t = gJ3DPortSortStats;
        if (t.drawers != base.drawers) {
            const double n = 300.0;
            char why[256];
            int o = 0;
            why[0] = '\0';
            for (int i = J3DPortSortWhy_Few; i < J3DPortSortWhy_Count; ++i) {
                const u32 d = t.why[i] - base.why[i];
                if (d != 0 && o < 200) {
                    o += snprintf(why + o, sizeof why - o, " %s %.1f (%.1f packets)", J3DPortSortWhyName(i), d / n, (t.whyPackets[i] - base.whyPackets[i]) / n);
                }
            }
            port_log_summary("starbatch: mode %d, per picture %.1f drawers asked, %.1f stretches, %.1f packets; ordered %.1f stretches (%.1f packets: runs of equal "
                     "differed lists %.1f -> %.1f, GL records at 10 a record %.1f -> %.1f, saved %.1f), already one list %.1f; left alone:%s; plan checks failed %u",
                     J3DPortSortMode(), (t.drawers - base.drawers) / n, (t.stretches - base.stretches) / n, (t.packets - base.packets) / n,
                     (t.sorted - base.sorted) / n, (t.sortedPackets - base.sortedPackets) / n, (t.runsBefore - base.runsBefore) / n,
                     (t.runsAfter - base.runsAfter) / n, (t.recBefore - base.recBefore) / n, (t.recAfter - base.recAfter) / n,
                     ((t.recBefore - base.recBefore) - (t.recAfter - base.recAfter)) / n, (t.same - base.same) / n, o ? why : " nothing", t.verifyBad);
            // (starbatch26) the stage census: instances batched a picture, lists not sent, the loop's time (GXV_TURN_PROF on) and the mode 2 comparison.
            // "est" is an estimate: lists not sent x 3 us (a list call's game-thread cost, not measured); the loop times are measured but only where
            // GXV_TURN_PROF is on: mode 2 times the drawer's own order ("old") and the planned order ("new") on the same drawers, mode 1 only "new".
            const double listsSaved = ((t.runsBefore - base.runsBefore) - (t.runsAfter - base.runsAfter)) / n;
            const double oldMs = (t.passOldNs - base.passOldNs) / 1.0e6 / n, newMs = (t.passNewNs - base.passNewNs) / 1.0e6 / n, planMs = (t.planNs - base.planNs) / 1.0e6 / n;
            port_log_summary("starbatch: census per picture: %.1f instances batched, %.1f differed lists not sent (est %.2f ms saved at 3 us a list); game-thread loop "
                     "old %.3f ms (%.1f packets) new %.3f ms (%.1f packets) planner %.3f ms (measured only with GXV_TURN_PROF on; mode 2 new = the second pass); "
                     "VERIFY: %u packets compared, %u differed, %u wrong differed lists, %u not comparable",
                     (t.sortedPackets - base.sortedPackets) / n, listsSaved, listsSaved * 0.003, oldMs, (t.passOldN - base.passOldN) / n, newMs, (t.passNewN - base.passNewN) / n,
                     planMs, t.verifyInst, t.verifyInstBad, t.verifyListBad, t.verifyUncomparable);
        }
        base = t;
    }
    if (port_starbatch_dl_mode() != 0 || port_starbatch_far_on() != 0) {
        // (starbatch26) per 300 pictures: GXV_STARBATCH_DL (star bits' material-list refreshes left out) and GXV_STARBATCH_FAR (far floating star bits' poses left out)
        static unsigned dlBase[8], farBase[4];
        unsigned dl[8], fr[4];
        port_starbatch_dl_counts(dl);
        port_starbatch_far_counts(fr);
        const double n = 300.0;
        port_log_summary("starbatch: dl mode %d: per picture %.1f star-bit refreshes asked, %.1f left out, %.1f computed (colour new / first), %.1f words disagreed, verify %.1f compared %.2f differed (must be 0); "
                 "far %s: %.1f asked, %.1f poses left out, %.1f refused (near / not floating)",
                 port_starbatch_dl_mode(), (dl[0] - dlBase[0]) / n, (dl[1] - dlBase[1]) / n, (dl[2] - dlBase[2]) / n, (dl[6] - dlBase[6]) / n, (dl[4] - dlBase[4]) / n,
                 (dl[5] - dlBase[5]) / n, port_starbatch_far_on() ? "on" : "off", (fr[0] - farBase[0]) / n, (fr[1] - farBase[1]) / n, (fr[2] - farBase[2]) / n);
        memcpy(dlBase, dl, sizeof dl);
        memcpy(farBase, fr, sizeof fr);
    }
    if (J3DPortNativeMode() != 0) {
        // (j3n2) the game's half of the GXV_J3DNATIVE census: who bumps the versions, and the top unbracketed emitters (GXDisplayList.c)
        static u32 base[J3DPortCen_Count];
        u32 d[J3DPortCen_Count];
        for (int i = 0; i < J3DPortCen_Count; ++i) {
            const u32 v = __atomic_load_n(&gJ3DPortCensus[i], __ATOMIC_RELAXED);
            d[i] = v - base[i];
            base[i] = v;
        }
        const double n = 300.0;
        port_log_summary("j3n game: per picture: FogCtrl::update %.1f (a real fog change %.2f: only those bump the fog serial), show / hide %.2f; "
                 "lists made again: endDL %.1f (unchanged bytes, swap undone, no bump %.1f), endPatch %.1f (unchanged %.1f), differed lists "
                 "stored changed %.1f, other writers %.2f",
                 d[J3DPortCen_FogCalls] / n, d[J3DPortCen_FogReal] / n, d[J3DPortCen_MemberBumps] / n, d[J3DPortCen_EndDL] / n,
                 d[J3DPortCen_EndDLSame] / n, d[J3DPortCen_EndPatch] / n, d[J3DPortCen_EndPatchSame] / n, d[J3DPortCen_DiffStored] / n,
                 d[J3DPortCen_OtherBumps] / n);
        J3DPortEmitLog(300);
    }
    __PortJ3nEmitOn = J3DPortNativeMode() != 0 ? 1 : 0;
}

void J3DDisplayListObj::beginDiffDL() {
    PortDlGameEnter();  // (GXV_PAR_DL: the scratch buffer, GD's current list and sGDLObj are the material path's)
    const bool keep = J3DPortDiffKeepOn();
    if ((!keep && !J3DPortDLGuardOn()) || mMaxSize > sizeof(sDiffScratch) || mpDisplayList[0] == nullptr) {
        sDiffScratchOn = false;
        beginDL();
        return;
    }
    // (beginDL's, writing into the scratch buffer and without the swap)
    sDiffScratchOn = true;
    sDiffScratchKeep = keep;
    sInterruptFlag = PORT_DL_GUEST_OFF() ? 0 : OSDisableInterrupts();
    GDInitGDLObj(&sGDLObj, sDiffScratch, mMaxSize);
    GDSetCurrent(&sGDLObj);
}

u32 J3DDisplayListObj::endDiffDL() {
    ++gJ3DPortListStats.diffMade;
    if (!sDiffScratchOn) {
        const u32 written = endDL();  // (bumps)
        if (written > mMaxSize && J3DPortDLGuardOn()) {
            J3DPortDLOverflow(this, written, mMaxSize, true);  // (a buffer larger than the scratch one: only reported)
        }
        return written;
    }
    sDiffScratchOn = false;
    GDPadCurr32();
    if (!PORT_DL_GUEST_OFF()) {
        OSRestoreInterrupts(sInterruptFlag);
    }
    const u32 size = GDGetGDLObjOffset(&sGDLObj);
    GDSetCurrent(NULL);
    if (size > mMaxSize && J3DPortDLGuardOn()) {
        J3DPortDLOverflow(this, size, mMaxSize, false);
        return mSize;
    }
    if (sDiffScratchKeep && size == mSize && memcmp(mpDisplayList[0], sDiffScratch, size) == 0) {
        // The bytes the current buffer holds: it stays the one called, unwritten (what endDL would have stored, in the other buffer,
        // is these same bytes).
        ++gJ3DPortListStats.diffKept;
        return mSize;
    }
    // (endDL's: into the other buffer, which becomes the current one, and the write made known as GDFlushCurrToMem does)
    swapBuffer();
    memcpy(mpDisplayList[0], sDiffScratch, size);
    mSize = size;
    DCFlushRange(mpDisplayList[0], mMaxSize);
    portBump(sPortPendingKinds);
    J3DPortCensusAdd(J3DPortCen_DiffStored);
    return mSize;
}
#endif

int J3DPacket::entry(J3DDrawBuffer* pBuffer) {
    return 1;
}

void J3DPacket::addChildPacket(J3DPacket* pPacket) {
    if (mpFirstChild == NULL) {
        mpFirstChild = pPacket;
    } else {
        pPacket->setNextPacket(mpFirstChild);
        mpFirstChild = pPacket;
    }
}

J3DDrawPacket::J3DDrawPacket() {
    mFlags = 0;
    mpDisplayListObj = NULL;
    mpTexMtxObj = NULL;
}

J3DDrawPacket::~J3DDrawPacket() {
}

J3DError J3DDrawPacket::newDisplayList(u32 size) {
    mpDisplayListObj = new J3DDisplayListObj();

    if (mpDisplayListObj == NULL)
        return kJ3DError_Alloc;

    J3DError ret = mpDisplayListObj->newDisplayList(size);
    if (ret != kJ3DError_Success)
        return ret;

    return kJ3DError_Success;
}

J3DError J3DDrawPacket::newSingleDisplayList(u32 size) {
    mpDisplayListObj = new J3DDisplayListObj();

    if (mpDisplayListObj == NULL)
        return kJ3DError_Alloc;

    J3DError ret = mpDisplayListObj->newSingleDisplayList(size);
    if (ret != kJ3DError_Success)
        return ret;

    return kJ3DError_Success;
}

void J3DDrawPacket::draw() {
    callDL();
}

J3DMatPacket::J3DMatPacket() {
    mpInitShapePacket = NULL;
    mpMaterial = NULL;
    mDiffFlag = 0xFFFFFFFF;
    mpTexture = NULL;
    mpMaterialAnm = NULL;
}

J3DMatPacket::~J3DMatPacket() {
}

void J3DMatPacket::addShapePacket(J3DShapePacket* pShape) {
    if (mpShapePacket == NULL) {
        mpShapePacket = pShape;
    } else {
        pShape->setNextPacket(mpShapePacket);
        mpShapePacket = pShape;
    }
}

void J3DMatPacket::beginDiff() {
#ifdef TARGET_PC
    mpInitShapePacket->getDisplayListObj()->beginDiffDL();  // (GXV_J3D_DIFFKEEP)
#else
    mpInitShapePacket->beginDL();
#endif
}

void J3DMatPacket::endDiff() {
#ifdef TARGET_PC
    mpInitShapePacket->getDisplayListObj()->endDiffDL();
#else
    mpInitShapePacket->endDL();
#endif
}

bool J3DMatPacket::isSame(J3DMatPacket* pOther) const {
    return mDiffFlag == pOther->mDiffFlag && (mDiffFlag >> 31) == 0;
}

void J3DMatPacket::draw() {
#ifdef TARGET_PC
    // (GXV_STREAM_CENSUS: a counted picture's packet is its model's, with the size its actor's clipping sphere had; one load otherwise)
    const bool census = __builtin_expect(port_sc_turn != 0, 0);
    if (census) {
        J3DModel* pModel = getShapePacket() != NULL ? getShapePacket()->getModel() : NULL;
        port_stream_census_model_begin2(pModel != NULL ? (const void*)pModel->getModelData() : NULL, pModel != NULL ? pModel->mPortCensusStamp : 0);
    }
#endif
    mpMaterial->load();
    callDL();

    J3DShapePacket* packet = getShapePacket();
    packet->getShape()->loadPreDrawSetting();

    while (packet != NULL) {
        if (packet->getDisplayListObj() != NULL) {
            packet->getDisplayListObj()->callDL();
        }

        packet->drawFast();
        packet = (J3DShapePacket*)packet->getNextPacket();
    }

    J3DShape::resetVcdVatCache();
#ifdef TARGET_PC
    if (census) {
        port_stream_census_model_end();
    }
#endif
}

static u32 sDifferedRegister[7] = {
    J3DDiffFlag_MatColor, J3DDiffFlag_ColorChan,  J3DDiffFlag_TevReg,
    J3DDiffFlag_Fog,      J3DDiffFlag_Blend,    J3DDiffFlag_KonstColor, J3DDiffFlag_TevStageIndirect,
};

static s32 sSizeOfDiffered[7] = {
    13, 21, 120, 55, 15, 19, 45,
};

J3DShapePacket::J3DShapePacket() {
    mpShape = NULL;
    mpMtxBuffer = NULL;
    mpBaseMtxPtr = NULL;
    mDiffFlag = 0;
    mpModel = NULL;
}

J3DShapePacket::~J3DShapePacket() {
}

u32 J3DShapePacket::calcDifferedBufferSize(u32 diffFlags) {
    u32 bufferSize = 0;

    /* SMG1 change -- 7 iterations instead of 8 */
    for (u32 i = 0; i < 7; i++) {
        if ((diffFlags & sDifferedRegister[i]) != 0) {
            bufferSize += sSizeOfDiffered[i];
        }
    }

    u32 lightObjNum = getDiffFlag_LightObjNum(diffFlags);
    bufferSize += lightObjNum * 0x48;

    u32 texGenNum = getDiffFlag_TexGenNum(diffFlags);
    if (texGenNum != 0) {
        u32 mat_texGenNum = mpShape->getMaterial()->getTexGenNum();
        u32 sp30 = texGenNum > mat_texGenNum ? texGenNum : mat_texGenNum;

        if (diffFlags & J3DDiffFlag_TexGen) {
            bufferSize += sp30 * 0x3d + 10;
        } else {
            bufferSize += calcDifferedBufferSize_TexMtxSize(sp30);
        }
    }

    u32 texNoNum = getDiffFlag_TexNoNum(diffFlags);
    if (texNoNum != 0) {
        u8 sp9;
        if (mpShape->getMaterial()->getTevStageNum() > 8) {
            sp9 = 8;
        } else {
            sp9 = mpShape->getMaterial()->getTevStageNum();
        }

        u32 mat_texNoNum = sp9;
        u32 sp24 = texNoNum > mat_texNoNum ? texNoNum : mat_texNoNum;

        if (diffFlags & J3DDiffFlag_TexCoordScale) {
            bufferSize += calcDifferedBufferSize_TexNoAndTexCoordScaleSize(sp24);
        } else {
            bufferSize += calcDifferedBufferSize_TexNoSize(sp24);
        }
    }

    u32 tevStageNum = getDiffFlag_TevStageNum(diffFlags);
    if (tevStageNum != 0) {
        u8 sp8;
        if (mpShape->getMaterial()->getTevStageNum() > 8) {
            sp8 = 8;
        } else {
            sp8 = mpShape->getMaterial()->getTevStageNum();
        }

        u32 mat_tevStageNum = sp8;
        u32 sp18 = tevStageNum > mat_tevStageNum ? tevStageNum : mat_tevStageNum;

        bufferSize += calcDifferedBufferSize_TevStageSize(sp18);
        if (diffFlags & J3DDiffFlag_TevStageIndirect) {
            bufferSize += calcDifferedBufferSize_TevStageDirectSize(sp18);
        }
    }

    return OSRoundUp32B(bufferSize);
}

J3DError J3DShapePacket::newDifferedDisplayList(u32 diffFlags) {
    mDiffFlag = diffFlags;

    u32 bufSize = calcDifferedBufferSize(diffFlags);
    J3DError ret = newDisplayList(bufSize);
    if (ret != kJ3DError_Success) {
        return ret;
    }

    return kJ3DError_Success;
}

void J3DShapePacket::prepareDraw() const {
#ifdef TARGET_PC
    gPortPipePrepared = this;
#endif
    mpModel->getVertexBuffer()->setArray();
    J3D_DSYS.setModel(mpModel);  // (GXV_DRAWPIPE: the draw path's J3DSys, J3DSys.hpp; j3dSys itself outside a frozen pass)
    J3D_DSYS.setShapePacket((J3DShapePacket*)this);

    J3DShapeMtx::setLODFlag(mpModel->checkFlag(J3DMdlFlag_EnableLOD) != 0);

    if (mpModel->checkFlag(J3DMdlFlag_SkinPosCpu)) {
        mpShape->onFlag(J3DShpFlag_SkinPosCpu);
    } else {
        mpShape->offFlag(J3DShpFlag_SkinPosCpu);
    }

    if (mpModel->checkFlag(J3DMdlFlag_SkinNrmCpu) && mpShape->checkFlag(J3DShpFlag_EnableLod) == false) {
        mpShape->onFlag(J3DShpFlag_SkinNrmCpu);
    } else {
        mpShape->offFlag(J3DShpFlag_SkinNrmCpu);
    }

    mpShape->setCurrentViewNoPtr(mpMtxBuffer->getCurrentViewNoPtr());
    mpShape->setScaleFlagArray(mpMtxBuffer->getScaleFlagArray());
    mpShape->setDrawMtx(mpMtxBuffer->getDrawMtxPtrPtr());

    if (!mpShape->getNBTFlag()) {
        mpShape->setNrmMtx(mpMtxBuffer->getNrmMtxPtrPtr());
    } else {
        mpShape->setNrmMtx(mpMtxBuffer->mpBumpMtxArr[1][mpShape->getBumpMtxOffset()]);
    }

#ifdef TARGET_PC
    // (J3DModelData::syncJ3DSysFlags, into the draw path's J3DSys)
    if (mpModel->getModelData()->checkFlag(0x20)) {
        J3D_DSYS.onFlag(J3DSysFlag_PostTexMtx);
    } else {
        J3D_DSYS.offFlag(J3DSysFlag_PostTexMtx);
    }
#else
    mpModel->getModelData()->syncJ3DSysFlags();
#endif
}

void J3DShapePacket::draw() {
    if (!checkFlag(J3DShpFlag_Hidden) && mpShape != NULL) {
        prepareDraw();

        if (mpTexMtxObj != NULL) {
            J3DDifferedTexMtx::sTexGenBlock = mpShape->getMaterial()->getTexGenBlock();
            J3DDifferedTexMtx::sTexMtxObj = getTexMtxObj();
        } else {
            J3DDifferedTexMtx::sTexGenBlock = NULL;
        }

        if (mpDisplayListObj != NULL) {
            mpDisplayListObj->callDL();
        }

        mpShape->draw();
    }
}

void J3DShapePacket::drawFast() {
    if (!checkFlag(J3DShpFlag_Hidden) && mpShape != NULL) {
        prepareDraw();

        if (mpTexMtxObj != NULL) {
            J3DDifferedTexMtx::sTexGenBlock = mpShape->getMaterial()->getTexGenBlock();
            J3DDifferedTexMtx::sTexMtxObj = getTexMtxObj();
        } else {
            J3DDifferedTexMtx::sTexGenBlock = NULL;
        }

        mpShape->drawFast();
    }
}



int J3DMatPacket::entry(J3DDrawBuffer* pBuffer) {
    J3DDrawBuffer::sortFunc func = J3DDrawBuffer::sortFuncTable[pBuffer->getSortMode()];
    return (pBuffer->*func)(this);
}

GDLObj J3DDisplayListObj::sGDLObj;
s32 J3DDisplayListObj::sInterruptFlag;
