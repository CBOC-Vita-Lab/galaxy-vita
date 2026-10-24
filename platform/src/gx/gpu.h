// Emulated Flipper/Hollywood GPU front end: register state + command processor.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

namespace gpu {

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

// Value of a debug environment variable, or nullptr when unset or empty.
inline const char* debugEnv(const char* name) {
    const char* v = getenv(name);
    return v && *v ? v : nullptr;
}
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;

inline u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }
inline u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
inline float befloat(const u8* p) {
    u32 v = be32(p);
    float f;
    __builtin_memcpy(&f, &v, 4);
    return f;
}

// Command-processor (CP) register file relevant to vertex fetch.
struct CPState {
    u32 matIndexA = 0;  // 0x30
    u32 matIndexB = 0;  // 0x40
    u32 vcdLo = 0;      // 0x50
    u32 vcdHi = 0;      // 0x60
    u32 vatA[8] = {};   // 0x70+n
    u32 vatB[8] = {};   // 0x80+n
    u32 vatC[8] = {};   // 0x90+n
    u32 arrayBase[16] = {};    // 0xA0+n (physical or virtual address)
    u32 arrayStride[16] = {};  // 0xB0+n
};

// Transform-unit (XF) memory and registers.
struct XFState {
    float mem[0x1000];  // 0x0000-0x0FFF: matrices, normal matrices, dual tex, lights
    u32 regs[0x100];    // 0x1000-0x10FF
};

struct GpuState {
    CPState cp;
    XFState xf;
    u32 bp[0x100] = {};
    u32 bpMask = 0xFFFFFF;
    // BP 0xE0-0xE7 hold either TEV color registers or konst colors (bit 23).
    u32 tevReg[8] = {};    // PREV/REG0-2: lo (R,A), hi (B,G) pairs
    u32 tevKonst[8] = {};  // K0-K3
};

// Tracks the TEV color/konst register a BP 0xE0-0xE7 write goes to.
inline void trackTevColorWrite(u32* tevReg, u32* tevKonst, u32 reg, u32 value) {
    if (reg >= 0xE0 && reg <= 0xE7) {
        if (value & 0x800000) {
            tevKonst[reg - 0xE0] = value;
        } else {
            tevReg[reg - 0xE0] = value;
        }
    }
}

extern GpuState g;

// Vertex descriptor decoding.
enum AttrType : u8 { ATTR_NONE = 0, ATTR_DIRECT = 1, ATTR_INDEX8 = 2, ATTR_INDEX16 = 3 };

// Size in bytes of one vertex for vertex format `vat` given the current VCD.
u32 vertexSize(int vat);

// Backend interface implemented by the renderer.
struct Backend {
    virtual ~Backend() = default;
    // A primitive batch: `data` points at `count` vertices in the command stream.
    virtual void draw(int primitive, int vat, u32 count, const u8* data, u32 vertexStride) = 0;
    virtual void bpWrite(u32 reg, u32 value, u32 oldValue) = 0;  // after g.bp updated
    virtual void xfWrite(u32 addr, u32 count) = 0;               // after g.xf updated
    virtual void cpWrite(u32 reg, u32 value) = 0;
    virtual void efbCopy(bool toXfb, bool clear) = 0;            // BP 0x52 trigger
    virtual void loadTlut(u32 regValue) = 0;                     // BP 0x65 trigger
    virtual void preloadTexture() = 0;                           // BP 0x63 (TMEM preload)
    virtual u32 peekColor(u16 x, u16 y) = 0;
    virtual u32 peekZ(u16 x, u16 y) = 0;
    // Bracket a display list call (CALL_DL at `addr`, `size` bytes at
    // `data`): the draws in between come from the list.  If Begin returns
    // true, the backend has already replayed the whole list: it is not
    // parsed and End is not called.
    virtual bool displayListBegin(u32 addr, const u8* data, u32 size) { return false; }
    virtual void displayListEnd() {}
};

void setBackend(Backend* b);
Backend* backend();

// Processes a complete command stream (FIFO contents or a display list).
// Each FIFO stream processed bumps gStreamSerial.
// Returns the number of bytes consumed (a trailing incomplete command is left).
size_t process(const u8* data, size_t len, bool isDisplayList);
extern u64 gStreamSerial;

// Host pointer for an address written into GPU registers (physical, cached
// or uncached virtual).
const u8* memPtr(u32 addr);

}  // namespace gpu
