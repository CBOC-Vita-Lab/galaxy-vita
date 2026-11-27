// The Bee Mushroom crash (2.0 .. 2.3), on the host with AddressSanitizer: decomp/src/JSystem/J3DGraphBase/J3DPacket.cpp (the differed
// lists: J3DShapePacket::newDifferedDisplayList, J3DDisplayListObj::beginDiffDL / endDiffDL, GXV_DL_GUARD) and RVL_SDK GDBase.c as they
// are, and a model of GXV_DL_LAZY's request / flush protocol for Mario's one model manager (decomp/src/Game/LiveActor/ModelManager.cpp
// port_dl_defer, port_dl_flush_stale, GXV_DL_LAZY_KEY), in the order of the c3 dumps:
//   turn 1  Mario touches the mushroom, still the normal body: MR::updateModelDiffDL(Mario) is listed (deferred);
//   turn 2  MarioActor::changeDisplayMode(2): the maker's model is BeeMario's (its differed lists sized for fog only, 0x40 bytes);
//   flush   Mario's maker diffs with Mario's flags (fog + 2 texture numbers, 0x10020000: 70 bytes, 0x60 with the padding).
// tools/test_dl_lazy_retarget.sh builds it and runs the three cases:
//   dl_lazy_retarget key     GXV_DL_LAZY_KEY=1: the entry is left out ("dl-lazy: stale entry"), nothing is written
//   dl_lazy_retarget guard   GXV_DL_LAZY_KEY=0, GXV_DL_GUARD=1: the list is made, found longer than its buffer, not stored
//                            ("FATAL: display list overflow"), the neighbouring list object intact
//   dl_lazy_retarget crash   both 0 (the 2.2 / c3 code): AddressSanitizer reports the heap-buffer-overflow (the script expects it)
// beefix25 (11 Nov), the cases the community fix (PR #34: mStaleDL cleared at the switch) and ours (the key) must agree on:
//   dl_lazy_retarget same       the refresh of a model that is still the one pointed at is made (BeeMario's own manager, its own fog-only flags)
//   dl_lazy_retarget roundtrip  normal -> bee -> normal before the flush: the refresh asked for the normal body is made, nothing is left out
//   dl_lazy_retarget relist     normal -> bee, then a request for the bee body (as calcAnim makes after a switch): made for the bee, once
// prog32a (20 Nov): the other form changes the same mechanism can hit. MarioActor::changeDisplayMode picks one of six bodies (0 normal, 1 bone =
// electric hit / paralyze flicker, 2 bee, 3 ice, 4 rainbow, 5 spring; MarioActor.cpp ~l.1995), all behind Mario's ONE model manager:
//   dl_lazy_retarget matrix      key on: every from -> x -> to of the six bodies (x = none or any: damage / respawn / the paralyze flicker switch
//                                twice in a few turns): a refresh asked on "from", the body switched, the flush: nothing is written into "to"
//                                and the entry is left out (made only when to == from); then the bone <-> normal flicker x 8
//   dl_lazy_retarget matrixguard key off, GXV_DL_GUARD on: every switch the old code would overflow (a 0xC0 list into a 0x40 buffer) is
//                                refused with the FATAL line and the buffer stays empty; the ones that fit are made
// (the form bodies other than the normal one are modelled as BeeMario's: fog-only maker, 0x40-byte buffers. Their real sizes are not read
// here: [guess] for bodies 1, 3, 4, 5; the normal body and the bee are the c3 dump's.)
#ifdef NDEBUG
#error "tests/dl_lazy_retarget.cpp needs assert()"
#endif
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "JSystem/J3DGraphBase/J3DPacket.hpp"
#include "JSystem/J3DGraphBase/J3DDrawBuffer.hpp"
#include "JSystem/J3DGraphBase/J3DShape.hpp"
#include "JSystem/J3DGraphBase/J3DShapeMtx.hpp"
#include "JSystem/J3DGraphAnimator/J3DModelData.hpp"
#include "JSystem/J3DGraphBase/J3DVertex.hpp"
#include "revolution/gd/GDBase.h"

// ---- what J3DPacket.cpp links against, outside the differed lists
bool J3DShapeMtx::sLODFlag;
void* J3DShape::sOldVcdVatCmd;
J3DDrawBuffer::sortFunc J3DDrawBuffer::sortFuncTable[6];
J3DTexGenBlock* J3DDifferedTexMtx::sTexGenBlock;
J3DTexMtxObj* J3DDifferedTexMtx::sTexMtxObj;
void J3DPacket::draw() {}
void J3DModelData::syncJ3DSysFlags() const {}
void J3DVertexBuffer::setArray() const {}
void J3DShape::loadPreDrawSetting() const {}
J3DSys::J3DSys() {}  // (zero-initialised: the log line reads its material packet and model)
J3DSys j3dSys;
extern "C" {
void DCFlushRange(void*, u32) {}
void DCStoreRange(void*, u32) {}
BOOL OSDisableInterrupts() { return 0; }
BOOL OSRestoreInterrupts(BOOL) { return 0; }
void GXCallDisplayList(const void*, u32) {}
}
// (JKRHeap's aligned new: exact-size blocks, so that AddressSanitizer sees the end of each buffer)
void* operator new[](size_t size, int align) {
    void* p = nullptr;
    if (posix_memalign(&p, static_cast< size_t >(align < 0 ? -align : align), size) != 0) abort();
    return p;
}
// (beefix25: J3DPacket.cpp gained these since the test was written; the plain host build needs them to link)
struct PortJ3nEmit { const void* ra; u32 kind, calls, amount; };
extern "C" {
int __PortArrKeep;
int __PortGXJ3nMode(void) { return 0; }
int __PortJ3nEmitOn;
int __PortJ3nEmitTake(PortJ3nEmit*, int, u32* lost) { *lost = 0; return 0; }
}
extern "C" {
int port_sc_turn;
void port_stream_census_model_begin2(const void*, unsigned) {}
void port_stream_census_model_end(void) {}
}
static int sFatal = 0, sStale = 0;
extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char line[512];
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("log: %s\n", line);
    if (strncmp(line, "FATAL: display list overflow", 28) == 0) ++sFatal;
}
extern "C" const char* port_dl_owner_name(void) { return "MarioActor"; }
// (tests31: the star batch hooks J3DPacket.cpp gained: off here)
extern "C" int port_starbatch_dl_mode(void) { return 0; }
extern "C" int port_starbatch_far_on(void) { return 0; }
extern "C" void port_starbatch_dl_counts(unsigned* out) { (void)out; }
extern "C" void port_starbatch_far_counts(unsigned* out) { (void)out; }

// ---- the model: a body's material packets' differed lists, as DisplayListMaker::newDifferedDisplayList makes them
struct Body {
    J3DShapePacket packet[2];
};
static void NewDiffered(Body& b, u32 flag) {
    for (J3DShapePacket& p : b.packet) {
        assert(p.newDifferedDisplayList(flag) == kJ3DError_Success);  // (fog only / no texture: calcDifferedBufferSize reads no shape)
    }
}
// J3DMaterial::diff's writes for Mario's flags 0x10020000, as in the c3 dump's bytes: two textures (BP 0x94 0x88 0x80 0x84, 0x95 0x89
// 0x81 0x85) and the fog (0xEE..0xF2, 0xE8): 70 bytes, 0x60 with GDPadCurr32.
static void BP(u32 v) {
    GDWrite_u8(0x61);
    GDWrite_u32(v);
}
static void DiffMaterial(J3DDisplayListObj* obj, u32 flag) {
    obj->beginDiffDL();
    if (flag & 0xF0000) {
        const u8 regs[8] = {0x94, 0x88, 0x80, 0x84, 0x95, 0x89, 0x81, 0x85};
        for (u8 r : regs) BP(static_cast< u32 >(r) << 24 | 0x000195);
    }
    if (flag & 0x10000000) {
        const u8 regs[6] = {0xEE, 0xEF, 0xF0, 0xF1, 0xF2, 0xE8};
        for (u8 r : regs) BP(static_cast< u32 >(r) << 24 | 0x00FFFF);
    }
    obj->endDiffDL();
}

// ---- GXV_DL_LAZY's protocol for one model manager (ModelManager.cpp), the maker reduced to its model and flags
struct Maker {
    Body* model;
    u32 flag[2];
};
struct Manager {
    Maker* maker;
    bool staleDL = false;
    Body* listedFor = nullptr;  // (GXV_DL_LAZY_KEY: mStaleDLDiffModel)
};
static bool sKey = true;
static void Request(Manager& mm) {  // port_dl_defer
    mm.staleDL = true;
    mm.listedFor = mm.maker->model;
}
static void Flush(Manager& mm) {  // port_dl_flush_stale: updateDL(true) -> DisplayListMaker::diff
    if (!mm.staleDL) return;
    mm.staleDL = false;
    if (sKey && mm.maker->model != mm.listedFor) {
        printf("log: dl-lazy: stale entry \"MarioActor\" left out\n");
        ++sStale;
        return;
    }
    for (int i = 0; i < 2; i++) {
        DiffMaterial(mm.maker->model->packet[i].getDisplayListObj(), mm.maker->flag[i]);
    }
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "key";
    if (strncmp(mode, "matrix", 6) == 0) {
        const bool guardOnly = strcmp(mode, "matrixguard") == 0;
        sKey = !guardOnly;
        setenv("GXV_DL_GUARD", "1", 1);
        setenv("GXV_J3D_DIFFKEEP", "1", 1);
        static Body bodies[6];
        for (int b = 0; b < 6; b++) {
            if (b == 0) {
                for (J3DShapePacket& p : bodies[b].packet) assert(p.newDisplayList(0xC0) == kJ3DError_Success);
            } else {
                NewDiffered(bodies[b], 0x10000000);
            }
        }
        const auto flagsOf = [](int b) { return b == 0 ? 0x10020000u : 0x10000000u; };
        int checked = 0, refused = 0, made = 0;
        for (int from = 0; from < 6; from++) {
            for (int x = -1; x < 6; x++) {
                for (int to = 0; to < 6; to++) {
                    if (from == to && x < 0) continue;
                    for (Body& b : bodies) {  // (every body's list empty before the case)
                        for (J3DShapePacket& p : b.packet) p.getDisplayListObj()->mSize = 0;
                    }
                    Maker mk{&bodies[from], {flagsOf(from), flagsOf(from)}};
                    Manager mm{&mk};
                    Request(mm);
                    if (x >= 0) mk.model = &bodies[x];
                    mk.model = &bodies[to];
                    const int staleBefore = sStale, fatalBefore = sFatal;
                    Flush(mm);
                    const u32 t0 = bodies[to].packet[0].getDisplayListObj()->mSize;
                    if (!guardOnly) {
                        if (to == from) {
                            assert(sStale == staleBefore && t0 > 0 && sFatal == fatalBefore);
                            ++made;
                        } else {
                            assert(sStale == staleBefore + 1 && t0 == 0 && sFatal == fatalBefore);
                            ++refused;
                        }
                    } else {
                        // (key off, GXV_DL_LAZY_KEY=0: the request's flags run over whatever is pointed at: only the guard stops it)
                        const bool fits = flagsOf(from) == flagsOf(to) || to == 0;
                        assert(sStale == staleBefore);
                        if (fits) {
                            assert(sFatal == fatalBefore && t0 > 0);
                            ++made;
                        } else {
                            assert(t0 == 0 && (sFatal > fatalBefore || sFatal >= 8));  // (the guard's log line is rate limited after 8)
                            ++refused;
                        }
                    }
                    for (Body& b : bodies) {
                        assert(b.packet[0].getDisplayListObj()->mSize <= b.packet[0].getDisplayListObj()->mMaxSize);
                    }
                    ++checked;
                }
            }
        }
        if (!guardOnly) {  // the electric / paralyze flicker: bone <-> normal, a refresh asked each normal turn
            for (Body& b : bodies) {
                for (J3DShapePacket& p : b.packet) p.getDisplayListObj()->mSize = 0;
            }
            Maker mk{&bodies[0], {flagsOf(0), flagsOf(0)}};
            Manager mm{&mk};
            const int fatalBefore = sFatal;
            for (int turn = 0; turn < 8; turn++) {
                mk.model = &bodies[0];
                Request(mm);
                mk.model = &bodies[1];  // the next turn is the bone body
                Flush(mm);              // left out
                mk.model = &bodies[0];
                Request(mm);
                Flush(mm);              // the normal body again: made
            }
            assert(sFatal == fatalBefore && bodies[1].packet[0].getDisplayListObj()->mSize == 0 && bodies[0].packet[0].getDisplayListObj()->mSize == 0x60);
        }
        printf("dl_lazy_retarget %s: ok (%d switches: %d left out or refused, %d made)\n", mode, checked, refused, made);
        return 0;
    }
    sKey = strcmp(mode, "key") == 0;
    setenv("GXV_DL_GUARD", strcmp(mode, "crash") == 0 ? "0" : "1", 1);
    setenv("GXV_J3D_DIFFKEEP", "1", 1);

    static Body normal, bee;
    const bool extra = strcmp(mode, "same") == 0 || strcmp(mode, "roundtrip") == 0 || strcmp(mode, "relist") == 0;
    if (extra) sKey = true;
    // Mario's flags: calcDifferedBufferSize(0x10020000) = fog 55 + 2 texture numbers x 0x37 (a material of at most 2 TEV stages) = 165,
    // 0xC0 (made directly here: the texture count reads the shape's material, which the fixture has not)
    for (J3DShapePacket& p : normal.packet) {
        assert(p.newDisplayList(0xC0) == kJ3DError_Success);
    }
    NewDiffered(bee, 0x10000000);     // BeeMario's (its own maker: fog only): 0x40
    assert(normal.packet[0].getDisplayListObj()->mMaxSize == 0xC0);
    assert(bee.packet[0].getDisplayListObj()->mMaxSize == 0x40);
    Maker marioMaker{&normal, {0x10020000, 0x10020000}};
    Manager mario{&marioMaker};

    if (extra) {
        for (J3DShapePacket& p : normal.packet) assert(p.newDisplayList(0xC0) == kJ3DError_Success);
        NewDiffered(bee, 0x10000000);
        Maker beeMaker{&bee, {0x10000000, 0x10000000}};
        Manager beeOwn{&beeMaker};
        Maker marioMaker2{&normal, {0x10020000, 0x10020000}};
        Manager mario2{&marioMaker2};
        if (strcmp(mode, "same") == 0) {
            assert(bee.packet[0].getDisplayListObj()->mSize == 0);
            Request(beeOwn);
            Flush(beeOwn);
            assert(bee.packet[0].getDisplayListObj()->mSize > 0 && bee.packet[0].getDisplayListObj()->mSize <= 0x40);
            assert(sStale == 0 && sFatal == 0);
        } else if (strcmp(mode, "roundtrip") == 0) {
            assert(normal.packet[0].getDisplayListObj()->mSize == 0);
            Request(mario2);
            marioMaker2.model = &bee;
            marioMaker2.model = &normal;
            Flush(mario2);
            assert(normal.packet[0].getDisplayListObj()->mSize == 0x60 && sStale == 0 && sFatal == 0);
            assert(bee.packet[0].getDisplayListObj()->mSize == 0);
        } else {  // relist: the manager now points at the bee body and a new request notes it (the latest wins)
            Request(mario2);
            marioMaker2.model = &bee;
            marioMaker2.flag[0] = marioMaker2.flag[1] = 0x10000000;  // (the bee body is diffed with fog only)
            Request(mario2);
            Flush(mario2);
            assert(bee.packet[0].getDisplayListObj()->mSize > 0 && bee.packet[0].getDisplayListObj()->mSize <= 0x40);
            assert(normal.packet[0].getDisplayListObj()->mSize == 0 && sStale == 0 && sFatal == 0);
        }
        printf("dl_lazy_retarget %s: ok\n", mode);
        return 0;
    }

    // a turn with the normal body, flushed before its picture: as the game made it
    Request(mario);
    Flush(mario);
    assert(normal.packet[0].getDisplayListObj()->mSize == 0x60);

    // turn 1: listed; turn 2: MarioActor::changeDisplayMode(2); the drawn turn's flush
    Request(mario);
    marioMaker.model = &bee;
    J3DDisplayListObj before = *bee.packet[1].getDisplayListObj();
    Flush(mario);

    const J3DDisplayListObj& after = *bee.packet[1].getDisplayListObj();
    assert(after.mpDisplayList[0] == before.mpDisplayList[0] && after.mpDisplayList[1] == before.mpDisplayList[1]);
    assert(bee.packet[0].getDisplayListObj()->mSize <= bee.packet[0].getDisplayListObj()->mMaxSize);
    if (strcmp(mode, "key") == 0) {
        assert(sStale == 1 && sFatal == 0);
    } else {
        assert(sStale == 0 && sFatal == 2);  // (both of BeeMario's materials refused)
    }
    // back to the normal body: listed and made as before
    marioMaker.model = &normal;
    Request(mario);
    Flush(mario);
    assert(normal.packet[1].getDisplayListObj()->mSize == 0x60);
    printf("dl_lazy_retarget %s: ok\n", mode);
    return 0;
}
