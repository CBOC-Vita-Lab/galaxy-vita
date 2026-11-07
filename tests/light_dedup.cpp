// GXV_LIGHT_DEDUP host proof (tools/test_light_dedup.sh): the production GXLight.c / GXDisplayList.c and port/light_memo.h.
// The FIFO is decoded by a small XF interpreter (light memory, list calls, BP writes). Random scenarios of light loads, light
// set-ups (a model of LightFunction::loadActorLightInfo with the memo as LightFunction.cpp uses it), recorded display lists with and
// without light loads, list calls, markers and draw-buffer scopes are run twice: without any scope (the switch off: the original
// stream) and with the scopes. At every marker, light memory and every non-light command before it must be the same in both.
// A negative control (a list holding light loads that GX did not see being recorded) must make the two differ.
#define PORT_GX_FIFO_BATCH_OWNER 1
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include "revolution/types.h"
#include "revolution/gx.h"
#include "revolution/gx/GXRegs.h"
#include "port/light_memo.h"
#include <stdarg.h>

extern "C" {
#undef vsnprintf
int port_snprintf(char* b, size_t n, const char* f, ...) {  // (compat.h routes snprintf here; the C library's on the host)
    va_list a;
    va_start(a, f);
    const int r = vsnprintf(b, n, f, a);
    va_end(a);
    return r;
}
uint8_t* __PortGXFifoPtr;
uint8_t* __PortGXFifoLimit;
int __PortGXFifoBatch;
static GXData gxData;
GXData* const __GXData = &gxData;
void __GXSetDirtyState(void) {}
void __GXSendFlushPrim(void) {}
int __PortGXFifoXfBurst12(const float*) { return 0; }
void GXResetWriteGatherPipe(void) {}
u32 port_host_to_phys(const void* p) { return (u32)(uintptr_t)p; }
void __PortGXFifoOverflow(void) {
    fprintf(stderr, "FIFO overflow in the test\n");
    __builtin_trap();
}
extern int __PortGXLightScope;
extern u32 __PortGXLightGen;
extern u32 __PortGXLightSkipped;
void __PortGXLightShadowDrop(void);
}

namespace {
std::vector<uint8_t> gStream(1 << 22);
struct List {
    std::vector<uint8_t> bytes;
    uint32_t size;
};
std::vector<List> gLists;
std::map<uint32_t, size_t> gListByAddr;

uint32_t rng = 1;
uint32_t Rand() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

// ---------------------------------------------------------------- the interpreter
struct Machine {
    uint32_t light[0x80];
    std::vector<std::string> log;  // markers (with light memory) and every non-light command, in order
};
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
void Run(Machine& m, const uint8_t* p, const uint8_t* end, int depth) {
    char line[160];
    while (p < end) {
        const uint8_t op = *p;
        if (op == 0x00) { ++p; continue; }
        if (op == 0x61) {
            const uint32_t v = be32(p + 1);
            p += 5;
            if ((v >> 24) == 0xAA) {
                std::string s = "marker " + std::to_string(v & 0xFFFFFF) + ":";
                for (int i = 0; i < 0x80; ++i) { snprintf(line, sizeof line, " %08x", m.light[i]); s += line; }
                m.log.push_back(s);
            } else {
                snprintf(line, sizeof line, "bp %08x", v);
                m.log.push_back(line);
            }
            continue;
        }
        if (op == 0x10) {
            const uint32_t h = be32(p + 1);
            const uint32_t n = (h >> 16) + 1, addr = h & 0xFFFF;
            p += 5;
            for (uint32_t i = 0; i < n; ++i, p += 4) {
                const uint32_t a = addr + i;
                if (a >= 0x600 && a < 0x680) {
                    m.light[a - 0x600] = be32(p);
                } else {
                    snprintf(line, sizeof line, "xf %04x %08x", a, be32(p));
                    m.log.push_back(line);
                }
            }
            continue;
        }
        if (op == 0x40) {
            const uint32_t addr = be32(p + 1), size = be32(p + 5);
            p += 9;
            auto it = gListByAddr.find(addr);
            if (it == gListByAddr.end() || depth > 0) { fprintf(stderr, "bad list call\n"); __builtin_trap(); }
            const List& l = gLists[it->second];
            snprintf(line, sizeof line, "call %zu", it->second);
            m.log.push_back(line);
            Run(m, l.bytes.data(), l.bytes.data() + (size < l.size ? size : l.size), depth + 1);
            continue;
        }
        fprintf(stderr, "unknown opcode %02x\n", op);
        __builtin_trap();
    }
}

// ---------------------------------------------------------------- the game side
GXLightObj Palette(uint32_t k) {
    GXLightObj o;
    memset(&o, 0, sizeof o);
    GXInitLightPos(&o, (f32)(k % 3), (f32)(k % 5) * 0.5f, -1.0f);
    GXInitLightAttn(&o, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    GXColor c = {(u8)(k * 40), (u8)(k * 7), 3, 255};
    GXInitLightColor(&o, c);
    return o;
}
// A model of LightFunction::loadActorLightInfo: lights 0, 1 and 2 as a function of (info, view), then the ambient colour (a write
// outside light memory, always made). With the memo inside a scope, as LightFunction.cpp does it.
port::LightMemo gMemo = {0, 0, {}};
uint32_t gMemoHits = 0;
void Setup(uint32_t info, uint32_t view, bool memoOn) {
    uint8_t infoBytes[48];
    float viewMtx[12];
    for (int i = 0; i < 48; ++i) infoBytes[i] = (uint8_t)(info * 31 + i);
    for (int i = 0; i < 12; ++i) viewMtx[i] = (float)(view + i);
    bool memo = false;
    if (memoOn && __PortGXLightScope > 0) {
        memo = true;
        if (port::LightMemoSame(gMemo, __PortGXLightGen, infoBytes, sizeof infoBytes, viewMtx, sizeof viewMtx)) {
            ++gMemoHits;
            GX_WRITE_RA_REG((0xABu << 24) | info);  // (the ambient colour)
            return;
        }
    }
    for (uint32_t s = 0; s < 3; ++s) {
        GXLightObj o = Palette(info * 3 + view * 7 + s);
        GXLoadLightObjImm(&o, (GXLightID)(1u << s));
    }
    GX_WRITE_RA_REG((0xABu << 24) | info);
    if (memo) port::LightMemoDone(gMemo, __PortGXLightGen);
}
bool gInScope = false;
void ScopeEnter() {  // (port_light_scope_enter with the switch on)
    __PortGXLightShadowDrop();
    ++__PortGXLightScope;
    gInScope = true;
}
void ScopeExit() {
    --__PortGXLightScope;
    __PortGXLightShadowDrop();
    port::LightMemoDrop(gMemo);
    gInScope = false;
}

// One scenario; scopes == false: the same calls without any scope (the switch off). Returns the interpreter's log.
std::vector<std::string> Scenario(uint32_t seed, bool scopes, bool negative) {
    rng = seed * 2654435761u + 7;
    gLists.clear();
    gListByAddr.clear();
    gLists.reserve(256);
    __PortGXFifoPtr = gStream.data();
    __PortGXFifoLimit = gStream.data() + gStream.size() - 256;
    __PortGXFifoBatch = (seed & 1);
    gMemo = {0, 0, {}};
    gInScope = false;
    __PortGXLightScope = 0;
    __PortGXLightShadowDrop();
    uint32_t marker = 0;
    for (int step = 0; step < 4000; ++step) {
        const uint32_t r = Rand() % 100;
        if (r < 6) {
            if (!gInScope && scopes) ScopeEnter();
        } else if (r < 10) {
            if (gInScope) ScopeExit();
        } else if (r < 50) {
            Setup(Rand() % 3, Rand() % 2, scopes);
        } else if (r < 60) {
            GXLightObj o = Palette(Rand() % 4);
            GXLoadLightObjImm(&o, (GXLightID)(1u << (Rand() % 8)));
        } else if (r < 75) {
            GX_WRITE_RA_REG((0xAAu << 24) | marker++);
        } else if (r < 80 && gLists.size() < 250) {
            // a list recorded now: its own light loads (or none), a BP write
            List l;
            l.bytes.assign(1024, 0);
            uint8_t* savePtr = __PortGXFifoPtr;
            uint8_t* saveLimit = __PortGXFifoLimit;
            __PortGXFifoPtr = l.bytes.data();
            __PortGXFifoLimit = l.bytes.data() + 900;
            const bool lights = (Rand() % 3) == 0;
            const bool seen = !(negative && lights);  // (negative control: light words written into a list behind GX's back)
            gxData.inDispList = GX_TRUE;
            if (lights) {
                for (int i = 0, n = 1 + Rand() % 3; i < n; ++i) {
                    GXLightObj o = Palette(Rand() % 4);
                    const u32 slot = Rand() % 3;
                    if (seen) {
                        GXLoadLightObjImm(&o, (GXLightID)(1u << slot));
                    } else {
                        const u32* w = reinterpret_cast<const u32*>(&o);
                        GX_WRITE_U8(0x10);
                        GX_WRITE_U32(CP_XF_LOADREGS(0x600 + slot * 0x10, 0xF));
                        for (int k = 0; k < 3; ++k) GX_WRITE_U32(0);
                        for (int k = 3; k < 16; ++k) GX_WRITE_U32(w[k]);
                    }
                }
            }
            GX_WRITE_RA_REG((0x55u << 24) | (uint32_t)gLists.size());
            gxData.inDispList = GX_FALSE;
            l.size = (uint32_t)(__PortGXFifoPtr - l.bytes.data());
            __PortGXFifoPtr = savePtr;
            __PortGXFifoLimit = saveLimit;
            gLists.push_back(std::move(l));
            gListByAddr[(uint32_t)(uintptr_t)gLists.back().bytes.data()] = gLists.size() - 1;
        } else if (r < 90 && !gLists.empty()) {
            const List& l = gLists[Rand() % gLists.size()];
            GXCallDisplayList(l.bytes.data(), l.size);
        } else {
            GX_WRITE_XF_REG(0x1008 + (Rand() & 7), Rand(), 0);
        }
    }
    if (gInScope) ScopeExit();
    GX_WRITE_RA_REG((0xAAu << 24) | marker++);
    Machine m;
    memset(m.light, 0, sizeof m.light);
    Run(m, gStream.data(), __PortGXFifoPtr, 0);
    return m.log;
}
}  // namespace

int main() {
    *(u32*)&gxData.vNumNot = 1;
    gxData.dirtyState = 0;
    // (first: GXLight.c's "a list was recorded with lights" state is for the whole run, and the positive runs set it)
    int negBad = 0;
    for (uint32_t seed = 1; seed <= 50; ++seed) {
        if (Scenario(seed, false, true) != Scenario(seed, true, true)) ++negBad;
    }
    printf("light-dedup: negative control (lights in a list GX did not record): %d of 50 scenarios differ (%s)\n", negBad,
           negBad ? "detected, as it must be" : "NOT DETECTED");
    int bad = 0;
    uint64_t skipped = 0, hits = 0;
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        const std::vector<std::string> off = Scenario(seed, false, false);
        const uint32_t s0 = __PortGXLightSkipped;
        gMemoHits = 0;
        const std::vector<std::string> on = Scenario(seed, true, false);
        skipped += __PortGXLightSkipped - s0;
        hits += gMemoHits;
        if (off != on) {
            ++bad;
            size_t i = 0;
            while (i < off.size() && i < on.size() && off[i] == on[i]) ++i;
            if (bad <= 5) fprintf(stderr, "seed %u: differs at entry %zu of %zu/%zu\n", seed, i, off.size(), on.size());
        }
    }
    printf("light-dedup: 200 scenarios, %s; %llu light objects not sent, %llu set-ups left out\n", bad ? "FAIL" : "identical",
           (unsigned long long)skipped, (unsigned long long)hits);
    if (bad || !negBad || skipped == 0 || hits == 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
