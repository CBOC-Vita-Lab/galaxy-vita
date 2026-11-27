// GX capture and replay (port/gx_capture.h; drawpipe24 DESIGN.md 2.3a). The game thread runs the open draw lists with the GX library
// in a capture mode: every GX entry point appends itself and its arguments to a record instead of executing, and a J3D pass becomes a
// token. A draw thread later replays the record through the real library in order and draws the passes at their tokens.
//
// How a call reaches the record. Each entry point of the GX library (decomp/src/RVL_SDK/gx/*.c, gx_fifo.cpp) starts with a hook
// (PORT_GXCAP / PORT_GXCAP_RET / PORT_GXCAP_ABANDON): one load of __PortGXCap and a branch, not taken unless a capture is open or the
// self-test is armed. The bytes the game writes into the FIFO itself (immediate vertices between GXBegin and GXEnd, J3DFifo / J3DGD
// writes, the port's markers) are not calls: while a capture is open, the FIFO's write pointer and limit point into the capture, so
// those unchanged inline writers append to a "raw run" of the record at the cost they always had; the overflow at the run's limit
// moves on to the next chunk (port_gxcap_fifo_pre, called by gx_fifo.cpp's __PortGXFifoOverflow).
//
// Why the replay is exact (the one proof). The stream is a function of (1) the calls made, in order, (2) their arguments, (3) the GX
// library's state when each call runs, (4) what the library reads through pointer arguments at call time, (5) the raw bytes between
// the calls, (6) what the front end reads later through addresses the stream carries. The replay makes
//   (1) the same calls in the same order: every call with a hook is a record, every raw byte is in the run between the records it was
//       written between, every token is where the caller put it, and the token's pass runs there;
//   (2) with the same arguments: by-value arguments are copied into the record;
//   (3) from the same state: the state at the capture's start is the serial state there (the library ran every call before it), and
//       the state at each later call is the result of the same calls from the same start. Nothing but the library changes the
//       library's state (no game code writes __GXData; the port-side globals the library reads are switches, constant while the game
//       runs, plus the three this file handles: __PortGXLightScope is recorded with each light load and with each token and set
//       around them in the replay; __PortGXFx is 0 throughout (a gathering that would open abandons); "the FIFO feeds the stream" holds
//       at the start and at the replay alike, see port_gx_capture_begin);
//   (4) with the same pointees: an argument the library reads at call time (matrices, light / texture / TLUT objects, the fog, filter
//       and vertex-format tables) is copied into the record and the replay passes the copy; what the library writes back into such an
//       object (GXLoadTexObj's register ids, GXLoadTlut's TMEM offset) is written into the game's object at record time by the
//       library's own code (__PortGXTexObjSetIds, __PortGXTlutObjSetOffset);
//   (5) the same raw bytes (copied);
//   (6) the same addresses. The memory behind them (vertex arrays, display lists, textures, TLUTs, copy destinations) is the case the
//       front end already handles: the console's graphics processor read it whenever it got to the command, so the game keeps it
//       unchanged from the call until its frame's draw-done wait, and the port's front end reads it up to a picture later under the
//       same rule (the draw-done wait is deferred to the next turn's start, GXV_DRAWDONE_DEFER; outside writers wait for the front end
//       at port_gx_view_write_from). The replay issues the call later but inside that same window (it ends before the turn's join),
//       so it sees what the call would have seen. A writer the game runs inside the window itself (port_gx_view_write_from, the
//       material-list writers) must end the capture first: port_gx_capture_abandon.
//   What the game reads back from the library while recording is answered so that the game makes the calls it would have made:
//   viewport, projection and scissor from a mirror (the value the library would hold after the recorded calls), the draw serial
//   (PETARI_BLOOM_SKIP) counted the way the library counts it, GXPeekARGB / GXPeekZ with the constant the vgx path answers without
//   reading anything. A getter whose mirror a token may have changed abandons (unless the token's pass is declared not to touch it,
//   port_gx_capture_token_keep). The light dedup's generation (__PortGXLightGen, read by LightFunction's memo) is 0 while recording:
//   every memo misses and the set-up is made in full; its loads are skipped by the library's own light shadow at replay exactly
//   where the memo would have hit (the memo hits only when light memory already holds those words), so the stream is the same.
// Anything else the layer cannot record exactly abandons: the record is replayed at once on the calling thread (the passes at its
// tokens through the registered handler), the FIFO is the real one again and the call runs for real; the caller is serial from there.
// Mode 2 (verify) executes every call for real as it records it, then replays the record into a scratch stream from the state at the
// start (the passes' bytes and the state after them as they really were) and compares bytes and getters.
// A0, the cheap record (dpengine, 12 Nov). The console's first figures: a recorded call cost about what the direct call costs
// (0.4-0.5 us), which leaves the recording thread nothing. Where the time went, and what is changed (the record's format, the
// replay, the verify and every rare path are as they were):
//   - the recorders were declared cold: compiled for size, nothing inlined, each record four more calls deep (enter, beginRecord,
//     closeRaw / reserve, openRaw). They are hot while a capture is open: PORT_GXCAP_IMPL below keeps the attribute off their
//     definitions here (the GX library's translation units still see the declaration cold: their code is unchanged);
//   - "is this the recording thread" was a thread-id call for every record. It is a compare of the stack address with a window
//     around the address the recording began at (ownerNear); only a call from outside the window takes the exact test;
//   - the raw run before a record was closed and the one after it opened by general code: chunk limits recomputed, two
//     functions. One straight piece now (beginRecord's first branch) when the record and the next run fit in the chunk,
//     which is all but one record in some thousand; the run after the record is opened there, endRecord has nothing left to do.
// A recorded "R" call is then the hook's test and call, one compare for the thread, a header and the arguments stored and
// three pointers moved: about 50 instructions against 120-150 and no call below the recorder.
#define PORT_GXCAP_HOOKS 1
#define PORT_GXCAP_IMPL 1
#include "revolution/types.h"
#include "revolution/gx.h"
#include "revolution/gx/GXRegs.h"
#include "port/gx_capture.h"
#include "port/gx_fifo.h"
#include "port/port.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <new>
#include <tuple>
#include <utility>

#include <pthread.h>
#if defined(__vita__)
extern "C" int sceKernelGetThreadId(void);
extern "C" int sceKernelDelayThread(unsigned usec);
#else
#include <unistd.h>
#endif

extern "C" {
int __PortGXCap = 0;
int __PortGXCapTrap = 0;                                        // (dpengine's GX-HOOKS, gx_capture.h)
void port_drawpipe_game_enter(void) __attribute__((weak));     // (decomp/src/Game/Scene/SceneFunction.cpp)
int __PortGXCapPass = 0;
uintptr_t __PortGXCapReplayTop = 0, __PortGXCapReplaySpan = 0;  // (gx_capture.h PORT_GXCAP_FOREIGN; set by port_gx_replay)
uintptr_t __PortGXCapExTop = 0, __PortGXCapExSpan = 0;          // (pipefix31, gx_capture.h: the stack of the other thread that is running its real call)
int port_drawpipe_on_helper(void) __attribute__((weak));        // (SceneFunction.cpp: the caller is the late draw's helper)
int __PortGXCapFifo = 0;
// The library's port-side globals this file reads or sets (GXLight.c, GXGeometry.c / GXDisplayList.c / GXFrameBuf.c, hw_regs.c).
extern int __PortGXLightScope;
extern u32 __PortGXLightGen;
extern u32 __PortGXDrawSerial;
extern volatile u32 __PortGXPERegs[0x400];
extern u8 __PortGXFx;
// gx_fifo.cpp (weak: host tests link their own FIFO)
int __PortGXFifoInStream(void) __attribute__((weak));
int __PortGXPeekConst(void) __attribute__((weak));
u32 __PortGXPeekARGB(u16 x, u16 y);
u32 __PortGXPeekZ(u16 x, u16 y);
int __PortGXRegionDefault(void);  // GXTexture.c
int port_on_frame_thread(void) __attribute__((weak));
// GX library entry points without a declaration in the SDK headers (the port's own, and two the headers leave out)
void port_gx_call_prologue(void);
void __PortGXLightShadowDrop(void);
u32 port_gx_draw_serial(void);
void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field);
void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
}

namespace {

// ---------------------------------------------------------------------------------------------------------------- switches
int sMode = -1;         // GXV_GXCAPTURE
int sSelftest = 0;      // GXV_GXCAPTURE_SELFTEST
int sSync = 0;          // GXV_GXCAPTURE_SYNC
// GXV_GXCAPTURE_NULL=1 (dpengine, 12 Nov; with GXV_GXCAPTURE=1 only): the null recorder, a MEASUREMENT, not a mode to play in.
// While a capture is open a hooked call is counted and dropped: no record is written and nothing of it is ever executed; the raw
// bytes the game writes itself (immediate vertices) go into the capture's run and are thrown away at the next call. Tokens are
// recorded and executed as always (the draw pipeline's J3D passes are still drawn, by the thread that replays). Everything the
// game reads back is still answered (the mirror, the texture objects' ids), so the game's own draw code runs exactly as it does
// while recording. THE PICTURES ARE WRONG ON PURPOSE (whatever the open draw lists draw is missing). What the run gives: the
// game thread's time in the draw with its GX calls costing a hook and a counter each, i.e. the draw lists' own walk: the floor
// under any recorder; and the number of calls a capture ("gxcap:" line), so that control - floor = what the calls cost.
// A dropped call still costs the hook's test, the call into its recorder and the test here: about 25 instructions.
int sNull = 0;
// GXV_GXCAPTURE_SLOW=1 (dpengine, A0): the record path as it was before A0 (the general code for every record, the thread-id
// test for every call), for an A/B of A0 on the console in one build. Default 0.
int sSlow = 0;
alignas(8) uint8_t sNullScratch[4096];  // where the rare record kinds that fill their record in place write, in the null recorder
uint32_t sSelfCap = 0;  // GXV_GXCAPTURE_SELFTEST_CALLS (0: no cut by count)

long envInt(const char* name, long def) {
    const char* e = getenv(name);
    return e && *e ? strtol(e, nullptr, 0) : def;
}
void readSwitches() {
    if (sMode >= 0) return;
    const long m = envInt("GXV_GXCAPTURE", 0);
    sSelftest = envInt("GXV_GXCAPTURE_SELFTEST", 0) == 1 ? 1 : 0;
    sSync = envInt("GXV_GXCAPTURE_SYNC", 0) == 1 ? 1 : 0;
    const long cap = envInt("GXV_GXCAPTURE_SELFTEST_CALLS", 0);
    sSelfCap = cap > 0 ? static_cast<uint32_t>(cap) : 0u;
    sMode = (m == 1 || m == 2) ? static_cast<int>(m) : 0;
    if (!sMode) sSelftest = 0;
    sNull = sMode == 1 && envInt("GXV_GXCAPTURE_NULL", 0) == 1 ? 1 : 0;
    sSlow = envInt("GXV_GXCAPTURE_SLOW", 0) == 1 ? 1 : 0;
}

void logLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logLine(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (port_log_summary) port_log_summary("%s", buf);
    else port_log("%s", buf);
}

uintptr_t selfId() {
#if defined(__vita__)
    return static_cast<uintptr_t>(static_cast<unsigned>(sceKernelGetThreadId()));
#else
    return reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(pthread_self()));
#endif
}

// ---------------------------------------------------------------------------------------------------------------- names
const char* const kNames[PORT_GXCAP_ID_END] = {
#define PORT_GXCAP_NAME_A(n) #n,
#define PORT_GXCAP_NAME_R(k, n, p, a) #n,
    PORT_GXCAP_ABANDON_LIST(PORT_GXCAP_NAME_A) "-",
    PORT_GXCAP_RECORD_LIST(PORT_GXCAP_NAME_R)
#undef PORT_GXCAP_NAME_A
#undef PORT_GXCAP_NAME_R
};

// ---------------------------------------------------------------------------------------------------------------- storage
// The record: chunks of kChunkBytes (malloc, never the game's heaps), a chain of records each 8-aligned:
//   Hdr { size, kind, id, thunk } + payload. RAW: payload = the bytes. CALL: payload = the thunk's arguments. TOKEN: TokenRec.
constexpr uint32_t kChunkBytes = 64u << 10;
constexpr uint32_t kSlack = 64;       // past the raw run's limit: what an element-wise writer may add before it checks
constexpr uint32_t kMinRaw = 256;     // a raw run opens only with at least this much room before the limit
constexpr int kPoolMax = 64;          // chunks kept for reuse (4 MiB)

struct Chunk {
    Chunk* next;
    uint32_t used;
    uint32_t pad;
    alignas(8) uint8_t data[kChunkBytes - 16];
};
constexpr uint32_t kChunkData = sizeof(((Chunk*)nullptr)->data);

enum Kind : uint16_t { K_RAW = 1, K_CALL = 2, K_TOKEN = 3 };
struct Hdr {
    uint32_t size;  // payload bytes
    uint16_t kind;
    uint16_t id;
    void (*thunk)(const uint8_t* payload);
};
constexpr uint32_t kHdr = (sizeof(Hdr) + 7u) & ~7u;
static_assert(offsetof(Hdr, id) == offsetof(Hdr, kind) + 2 && sizeof(uint16_t) * 2 == 4, "kind and id are one 32-bit word of the header");
// (kind and id as the one word they are in memory, for a single store: A0's fastRecord)
inline uint32_t kindAndId(uint16_t kind, uint16_t id) {
    uint16_t w[2] = {kind, id};
    uint32_t v;
    memcpy(&v, w, 4);
    return v;
}
inline uint32_t align8(uint32_t v) { return (v + 7u) & ~7u; }

std::mutex sPoolLock;
Chunk* sPool = nullptr;
int sPoolCount = 0;
Chunk* chunkGet() {
    {
        std::lock_guard<std::mutex> lk(sPoolLock);
        if (sPool) {
            Chunk* c = sPool;
            sPool = c->next;
            --sPoolCount;
            c->next = nullptr;
            c->used = 0;
            return c;
        }
    }
    Chunk* c = static_cast<Chunk*>(malloc(sizeof(Chunk)));
    if (!c) port_fatal("gxcap: out of memory for a capture chunk");
    c->next = nullptr;
    c->used = 0;
    return c;
}
void chunkFree(Chunk* c) {
    while (c) {
        Chunk* n = c->next;
        std::lock_guard<std::mutex> lk(sPoolLock);
        if (sPoolCount < kPoolMax) {
            c->next = sPool;
            sPool = c;
            ++sPoolCount;
        } else {
            free(c);
        }
        c = n;
    }
}

// Mode 2: a growable byte buffer (the real bytes, the scratch replay's bytes, a pass's bytes).
struct Bytes {
    uint8_t* p = nullptr;
    size_t n = 0, cap = 0;
    void reserve(size_t want) {
        if (want <= cap) return;
        size_t c = cap ? cap : 4096;
        while (c < want) c *= 2;
        uint8_t* q = static_cast<uint8_t*>(realloc(p, c));
        if (!q) port_fatal("gxcap: out of memory (verify buffer)");
        p = q;
        cap = c;
    }
    void put(const void* src, size_t k) {
        if (!k) return;
        reserve(n + k);
        memcpy(p + n, src, k);
        n += k;
    }
    void clear() { n = 0; }
    void release() {
        free(p);
        p = nullptr;
        n = cap = 0;
    }
};

// Mode 2: what the scratch replay restores (the library's state and the port-side globals its calls change).
struct Snapshot {
    GXData gxd;
    uint8_t light[PORT_GXCAP_LIGHT_STATE_BYTES];
    u32 serial;
    u32 pe[16];
    void take() {
        memcpy(&gxd, __GXData, sizeof gxd);
        __PortGXLightStateSave(light);
        serial = __PortGXDrawSerial;
        for (int i = 0; i < 16; ++i) pe[i] = __PortGXPERegs[i];
    }
    void put() const {
        memcpy(__GXData, &gxd, sizeof gxd);
        __PortGXLightStateLoad(light);
        __PortGXDrawSerial = serial;
        for (int i = 0; i < 16; ++i) __PortGXPERegs[i] = pe[i];
    }
};

}  // namespace

struct PortGxCapture {
    Chunk* head;
    Chunk* tail;
    uint32_t calls, tokens;
    uint64_t rawBytes, recordBytes;
};

namespace {

struct TokenRec {
    uint32_t id, keep;
    int scope;
    uint32_t passBytes;   // mode 2: the pass's real bytes follow (after a Snapshot pointer)
    Snapshot* after;      // mode 2: the state after the pass (malloc'd; freed with the capture's verify pass)
};

// ---------------------------------------------------------------------------------------------------------------- state
// One capture at a time (the caller owns GX while it records; begin refuses while a replay runs).
PortGxCapture* sCap = nullptr;   // the open capture
uintptr_t sOwner = 0;
bool sAbandoned = false;         // the owner's capture was abandoned: tokens run at once until port_gx_capture_end
bool sSelfCapture = false;       // the open capture is the self-test's
bool sInCall = false;            // mode 2: a recorded call (or a token's pass) is executing for real: hooks let it through
std::atomic<int> sReplaying{0};
void (*sTokenFn)(uint32_t, void*) = nullptr;
void* sTokenCtx = nullptr;
int64_t sOpenAt = 0;

// The raw run: its header's place, the first byte of the run, the real FIFO pointer / limit to put back.
// (A0) What the cheap record path reads and writes for every call, side by side: one address in a register reaches all of it
// (on the console each separate global is two instructions to name), and one cache line holds it.
struct FastState {
    Hdr* rawHdr = nullptr;        // the open raw run's header
    uint8_t* rawStart = nullptr;  // and its first byte
    uint8_t* end = nullptr;       // a record may start at or below this in the tail chunk and leave room for the run after it
    uintptr_t ownerTop = 0, ownerSpan = 0;
    uint32_t recBytes = 0, calls = 0, rawBytes = 0;
    bool on = false;
};
alignas(64) FastState sF;
Hdr*& sRawHdr = sF.rawHdr;
uint8_t*& sRawStart = sF.rawStart;
uint8_t* sRealPtr = nullptr;
uint8_t* sRealLimit = nullptr;

// What the library's port-side tests answered at the start (the real FIFO feeding the stream; constant while the game runs).
bool sRetOk = false, sBrOk = false, sFxOk = false;
u32 sSavedLightGen = 0;
bool sLightGenZeroed = false;

// The mirror: the state the library would hold after the recorded calls (only what the game reads back).
struct Mirror {
    f32 vp[6];
    bool vpOk;
    u32 projType;
    f32 proj[6];
    bool projOk;
    u32 scis0, scis1;
    bool scisOk;
    u32 serial;
    bool serialOk;
} sMirror;

// Mode 2.
bool sVerify = false;
Snapshot* sStart = nullptr;     // the state at the start
Bytes sTruth;                   // every real byte since the start (raw, calls, passes)
Bytes sRawPending;              // the raw bytes since the last record (become a RAW record)
Bytes sPassBytes;               // the running pass's bytes
uint8_t* sTapMark = nullptr;    // real FIFO bytes before this have been taken
enum Phase { PH_RAW, PH_CALL, PH_PASS } sPhase = PH_RAW;
bool sSink = false;             // mode 2's scratch replay: the FIFO writes into sScratch
Bytes sScratch;
uint8_t sSinkBuf[4096 + kSlack];
bool sStreamOverride = false;
char sFirstDiff[256];

// Statistics (capture thread; the replay's are atomics).
struct Totals {
    uint64_t fastCalls = 0;  // (A0) calls recorded by the cheap path
    uint64_t begun, refused, ended, abandoned, verified, calls, rawBytes, recordBytes, tokens, getters, verifyDiffs, foreign,
        contractErrors, captureNs, selfCuts, wouldAbandon;
    uint64_t abandonBy[PORT_GXCAP_ID_END];
    uint64_t refusedBy[8];
} sTot, sLast;
std::atomic<uint64_t> sReplayed{0}, sReplayNs{0};
uint64_t sLastReplayed = 0, sLastReplayNs = 0;
int64_t sLogAt = 0;
enum { R_MODE, R_OPEN, R_REPLAYING, R_DISPLIST, R_FX, R_STREAM, R_NESTED, R_LAST };
const char* const kRefuse[R_LAST] = {"mode", "open", "replaying", "displist", "fx", "stream", "nested"};

bool ownerHere() { return sOwner == selfId(); }
// (A0) A plain mode-1 recording is open: no verify, no null recorder, no self-test call cap. And the recording thread's stack:
// a call whose frame lies in the window [sOwnerTop - sOwnerSpan, sOwnerTop] is the recording thread's. The window is the
// piece of that thread's own stack around the frame the recording began in (at most kOwnerAbove over it and kOwnerBelow under
// it, and never past the stack's two ends: the next allocation may be another thread's stack), so no other thread's frame can
// lie in it and the answer "yes" is exact. Outside the window (a draw deeper than kOwnerBelow, another thread, a host that
// cannot say where the stack is) the thread id decides, as before.
bool& sFast = sF.on;
uintptr_t &sOwnerTop = sF.ownerTop, &sOwnerSpan = sF.ownerSpan;
uint32_t &sFastRecBytes = sF.recBytes, &sFastCalls = sF.calls, &sFastRawBytes = sF.rawBytes;
uint8_t*& sFastEnd = sF.end;
constexpr uintptr_t kOwnerAbove = 64u << 10, kOwnerBelow = 256u << 10;
inline bool ownerNear() { return sOwnerTop - reinterpret_cast<uintptr_t>(__builtin_frame_address(0)) < sOwnerSpan; }
// sF's address through a register the compiler cannot see through: every field is then that register plus an offset (it would
// otherwise name each field's address on its own, and split a file-local structure back into separate globals).
__attribute__((always_inline)) inline FastState* fastState() {
    FastState* f = &sF;
    __asm__("" : "+r"(f));
    return f;
}
__attribute__((always_inline)) inline bool fastOwner(const FastState* f) {
    return f->on && f->ownerTop - reinterpret_cast<uintptr_t>(__builtin_frame_address(0)) < f->ownerSpan;
}
#if defined(__vita__)
extern "C" int port_vita_thread_stack(void** base, uint32_t* size);  // (vita/runtime/memory.cpp)
#endif
// The calling thread's stack [lo, hi), asked once a thread; lo == hi when it cannot be told.
// (pipefix31: stackNow asks every time: the trap uses it on threads that never asked before; stackHere keeps its thread_local cache for the replay modes)
void stackNow(uintptr_t* lo, uintptr_t* hi) {
    *lo = *hi = 0;
#if defined(__vita__)
    void* base = nullptr;
    uint32_t size = 0;
    if (port_vita_thread_stack(&base, &size) >= 0 && base && size) {
        *lo = reinterpret_cast<uintptr_t>(base);
        *hi = *lo + size;
    }
#elif defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* base = nullptr;
        size_t size = 0;
        if (pthread_attr_getstack(&attr, &base, &size) == 0 && base && size) {
            *lo = reinterpret_cast<uintptr_t>(base);
            *hi = *lo + size;
        }
        pthread_attr_destroy(&attr);
    }
#endif
}
void stackHere(uintptr_t* lo, uintptr_t* hi) {
    static thread_local uintptr_t tLo = 0, tHi = 0;
    static thread_local bool tAsked = false;
    if (!tAsked) {
        tAsked = true;
        stackNow(&tLo, &tHi);
    }
    *lo = tLo;
    *hi = tHi;
}
// The window for a recording begun in the frame at `at` (the caller's).
void ownerWindow(uintptr_t at) {
    uintptr_t lo, hi;
    stackHere(&lo, &hi);
    sOwnerTop = sOwnerSpan = 0;
    if (at < lo || at >= hi) return;  // (unknown stack, or a frame not in it: the exact test for every call)
    const uintptr_t top = hi - at > kOwnerAbove ? at + kOwnerAbove : hi - 1;
    const uintptr_t low = at - lo > kOwnerBelow ? at - kOwnerBelow : lo;
    sOwnerTop = top;
    sOwnerSpan = top - low + 1;
}

// ---------------------------------------------------------------------------------------------------------------- record writing
Chunk* tailChunk() { return sCap->tail; }
uint8_t* chunkEnd(Chunk* c) { return c->data + kChunkData; }

void newChunk() {
    Chunk* c = chunkGet();
    sCap->tail->next = c;
    sCap->tail = c;
}

// Room for `bytes` (header included) at the tail's write position.
uint8_t* reserve(uint32_t bytes) {
    Chunk* c = tailChunk();
    if (c->used + bytes > kChunkData) {
        if (bytes > kChunkData) port_fatal("gxcap: a record of %u bytes", (unsigned)bytes);
        newChunk();
        c = tailChunk();
    }
    uint8_t* p = c->data + c->used;
    c->used += bytes;
    return p;
}

// Mode 1: the raw run opens at the tail: the FIFO writes into it from now.
void openRaw() {
    Chunk* c = tailChunk();
    if (c->used + kHdr + kMinRaw + kSlack > kChunkData) {
        newChunk();
        c = tailChunk();
    }
    sRawHdr = reinterpret_cast<Hdr*>(c->data + c->used);
    sRawStart = c->data + c->used + kHdr;
    __PortGXFifoPtr = sRawStart;
    __PortGXFifoLimit = chunkEnd(c) - kSlack;
    sFastEnd = chunkEnd(c) - (kHdr + kMinRaw + kSlack);  // (A0: a record that ends at or below this leaves a run its room)
}
// Mode 1: the raw run closes where the FIFO's write pointer is (an empty one leaves nothing behind).
void closeRaw() {
    Chunk* c = tailChunk();
    const uint32_t n = static_cast<uint32_t>(__PortGXFifoPtr - sRawStart);
    if (!n) {
        c->used = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(sRawHdr) - c->data);
        return;
    }
    sRawHdr->size = n;
    sRawHdr->kind = K_RAW;
    sRawHdr->id = 0;
    sRawHdr->thunk = nullptr;
    c->used = align8(static_cast<uint32_t>(sRawStart + n - c->data));
    sCap->rawBytes += n;
}
// Mode 2: the raw bytes since the last record become a RAW record (they were written to the real FIFO and taken by the tap).
void flushPendingRaw() {
    if (!sRawPending.n) return;
    uint32_t done = 0;
    while (done < sRawPending.n) {
        Chunk* c = tailChunk();
        if (c->used + kHdr + 64 > kChunkData) newChunk(), c = tailChunk();
        const uint32_t room = kChunkData - c->used - kHdr;
        const uint32_t k = static_cast<uint32_t>(sRawPending.n - done) < room ? static_cast<uint32_t>(sRawPending.n - done) : room & ~7u;
        Hdr* h = reinterpret_cast<Hdr*>(c->data + c->used);
        h->size = k;
        h->kind = K_RAW;
        h->id = 0;
        h->thunk = nullptr;
        memcpy(c->data + c->used + kHdr, sRawPending.p + done, k);
        c->used = align8(c->used + kHdr + k);
        done += k;
    }
    sCap->rawBytes += sRawPending.n;
    sRawPending.clear();
}

// Mode 2: the real bytes written since the mark go where the current phase says.
void tap() {
    if (!sVerify || !sTapMark) return;
    uint8_t* const p = __PortGXFifoPtr;
    if (p > sTapMark) {
        const size_t k = static_cast<size_t>(p - sTapMark);
        sTruth.put(sTapMark, k);
        if (sPhase == PH_RAW) sRawPending.put(sTapMark, k);
        else if (sPhase == PH_PASS) sPassBytes.put(sTapMark, k);
    }
    sTapMark = p;
}

// The null recorder: the raw bytes of the open run are counted and thrown away (the run stays open, empty).
inline void nullDrop() {
    sCap->rawBytes += static_cast<uint32_t>(__PortGXFifoPtr - sRawStart);
    __PortGXFifoPtr = sRawStart;
}
// (A0) The record in one straight piece, inline in each recorder: the raw run closed (a header only when bytes were written),
// the record's header written where the run's header stood or behind the run, the next run opened behind the record. nullptr:
// not a plain recording, or the tail chunk has not the room: the general way (beginRecord). The tail chunk's `used` is not kept
// up here: it is where the open run's header stands, and closeRaw, which every general path starts with, says so again.
__attribute__((always_inline)) inline uint8_t* fastRecord(FastState* f, uint32_t kindAndId, uint32_t payload, void (*thunk)(const uint8_t*)) {
    uint8_t* const start = f->rawStart;
    Hdr* const rawHdr = f->rawHdr;
    const uint32_t n = static_cast<uint32_t>(__PortGXFifoPtr - start);
    uint8_t* at = n ? start + ((n + 7u) & ~7u) : reinterpret_cast<uint8_t*>(rawHdr);
    const uint32_t total = kHdr + align8(payload);
    if (!__builtin_expect(at <= f->end - total, 1)) return nullptr;  // (nothing written yet: the general way starts over)
    if (n) {
        rawHdr->size = n;
        rawHdr->kind = K_RAW;
        rawHdr->id = 0;
        rawHdr->thunk = nullptr;
        f->rawBytes += n;
    }
    Hdr* const h = reinterpret_cast<Hdr*>(at);
    h->size = payload;
    memcpy(&h->kind, &kindAndId, 4);
    h->thunk = thunk;
    at += total;
    f->rawHdr = reinterpret_cast<Hdr*>(at);
    f->rawStart = at + kHdr;
    __PortGXFifoPtr = at + kHdr;
    f->recBytes += total;
    ++f->calls;
    return reinterpret_cast<uint8_t*>(h) + kHdr;
}
// The same for a caller that has not looked yet whether the cheap path is open (the recorders of calls with copied pointees).
__attribute__((always_inline)) inline uint8_t* fastRecord(uint32_t kindAndId, uint32_t payload, void (*thunk)(const uint8_t*)) {
    FastState* const f = fastState();
    if (!__builtin_expect(f->on, 1)) return nullptr;
    return fastRecord(f, kindAndId, payload, thunk);
}
// A record of `payload` bytes: the raw run before it is closed (or taken), the header written, the run reopened after it.
uint8_t* beginRecord(uint16_t kind, uint16_t id, uint32_t payload, void (*thunk)(const uint8_t*)) {
    if (__builtin_expect(sNull != 0, 0)) {
        // (the null recorder: the raw bytes written since the last call are dropped; a call's record goes nowhere; a token is
        // recorded for real, below, with an empty run before it)
        nullDrop();
        if (kind != K_TOKEN) {
            if (payload > sizeof sNullScratch) port_fatal("gxcap: a record of %u bytes in the null recorder", (unsigned)payload);
            return sNullScratch;
        }
    }
    if (sVerify) {
        tap();
        flushPendingRaw();
    } else {
        closeRaw();
    }
    uint8_t* p = reserve(kHdr + align8(payload));
    Hdr* h = reinterpret_cast<Hdr*>(p);
    h->size = payload;
    h->kind = kind;
    h->id = id;
    h->thunk = thunk;
    sCap->recordBytes += kHdr + align8(payload);
    // (the run after the record is opened here: its place does not depend on what the caller writes into the record)
    if (!sVerify) openRaw();
    return p + kHdr;
}
// (kept for the callers' shape: since A0 beginRecord leaves the next run open itself)
inline void endRecord() {}

// ---------------------------------------------------------------------------------------------------------------- the FIFO
// Writing a run of bytes into the real stream as element writes would put them (gx_fifo.cpp __PortGXFifoWriteBytes; a host test's
// FIFO has its own overflow): up to the limit, then the overflow, and on.
void writeBytes(const uint8_t* s, uint32_t n) {
    while (n) {
        const size_t room = __PortGXFifoLimit > __PortGXFifoPtr ? static_cast<size_t>(__PortGXFifoLimit - __PortGXFifoPtr) : 0;
        if (!room) {
            __PortGXFifoOverflow();
            continue;
        }
        const size_t k = n < room ? n : room;
        memcpy(__PortGXFifoPtr, s, k);
        __PortGXFifoPtr += k;
        s += k;
        n -= static_cast<uint32_t>(k);
        if (__PortGXFifoPtr >= __PortGXFifoLimit) __PortGXFifoOverflow();
    }
}

void closeCapture(bool replayThreadNext);
void replayRecords(PortGxCapture* c, void (*token)(uint32_t, void*), void* ctx, bool scratch);
void verifyFinish();

// ---------------------------------------------------------------------------------------------------------------- begin / end
bool beginInternal(bool self) {
    readSwitches();
    int why = -1;
    if (!sMode) why = R_MODE;
    else if (sCap) why = R_OPEN;
    else if (sReplaying.load(std::memory_order_acquire)) why = R_REPLAYING;
    else if (__GXData->inDispList) why = R_DISPLIST;
    else if (__PortGXFx) why = R_FX;
    else if (__PortGXFifoInStream && !__PortGXFifoInStream()) why = R_STREAM;
    else if (sInCall) why = R_NESTED;
    if (why >= 0) {
        ++sTot.refused;
        ++sTot.refusedBy[why];
        return false;
    }
    sCap = static_cast<PortGxCapture*>(malloc(sizeof(PortGxCapture)));
    if (!sCap) port_fatal("gxcap: out of memory");
    sCap->head = sCap->tail = chunkGet();
    sCap->calls = sCap->tokens = 0;
    sCap->rawBytes = sCap->recordBytes = 0;
    sOwner = selfId();
    ownerWindow(reinterpret_cast<uintptr_t>(__builtin_frame_address(0)));
    sFastRecBytes = sFastRawBytes = sFastCalls = 0;
    sAbandoned = false;
    sSelfCapture = self;
    sOpenAt = port_host_time_ns();
    ++sTot.begun;
    // The port-side tests the recorded calls will meet at replay, answered now while the FIFO is the real one.
    sRetOk = __PortGXRetainedOk && __PortGXRetainedOk();
    sBrOk = __PortGXBracketOk && __PortGXBracketOk();
    sFxOk = __PortGXFxMergeOk && __PortGXFxMergeOk();
    // The mirror starts as the library's state.
    memcpy(sMirror.vp, &__GXData->vpLeft, sizeof sMirror.vp);
    sMirror.projType = __GXData->projType;
    memcpy(sMirror.proj, __GXData->projMtx, sizeof sMirror.proj);
    sMirror.scis0 = __GXData->suScis0;
    sMirror.scis1 = __GXData->suScis1;
    sMirror.serial = __PortGXDrawSerial;
    sMirror.vpOk = sMirror.projOk = sMirror.scisOk = sMirror.serialOk = true;
    sRealPtr = __PortGXFifoPtr;
    sRealLimit = __PortGXFifoLimit;
    sVerify = sMode == 2;
    if (sVerify) {
        if (!sStart) sStart = static_cast<Snapshot*>(malloc(sizeof(Snapshot)));
        sStart->take();
        sTruth.clear();
        sRawPending.clear();
        sPassBytes.clear();
        sTapMark = __PortGXFifoPtr;
        sPhase = PH_RAW;
        sLightGenZeroed = false;
        __PortGXCapFifo = 1;  // (the tap: every FIFO event takes the bytes written before it)
    } else {
        // LightFunction's memo misses while recording (see the file comment); the real generation is put back at the end.
        sSavedLightGen = __PortGXLightGen;
        __PortGXLightGen = 0;
        sLightGenZeroed = true;
        openRaw();
        __PortGXCapFifo = 1;
    }
    sFast = !sVerify && !sNull && !(self && sSelfCap) && !sSlow;
    __PortGXCap = 1;
    return true;
}

// The capture stops recording: the FIFO is the real one again, the hooks are idle (or armed again for the self-test).
void closeCapture(bool) {
    sFast = false;
    sCap->recordBytes += sFastRecBytes;
    sCap->rawBytes += sFastRawBytes;
    sCap->calls += sFastCalls;
    sTot.fastCalls += sFastCalls;
    sFastRecBytes = sFastRawBytes = sFastCalls = 0;
    if (sVerify) {
        tap();
        flushPendingRaw();
    } else {
        if (sNull) nullDrop();  // (the null recorder: the last raw bytes are dropped too)
        closeRaw();
        __PortGXFifoPtr = sRealPtr;
        __PortGXFifoLimit = sRealLimit;
    }
    if (sLightGenZeroed) {
        __PortGXLightGen = sSavedLightGen;
        sLightGenZeroed = false;
    }
    __PortGXCapFifo = 0;
    __PortGXCap = 0;  // (the self-test is armed again once the record has been replayed: rearm)
    sTot.calls += sCap->calls;
    sTot.rawBytes += sCap->rawBytes;
    sTot.recordBytes += sCap->recordBytes;
    sTot.tokens += sCap->tokens;
    sTot.captureNs += static_cast<uint64_t>(port_host_time_ns() - sOpenAt);
}

// GXV_GXCAPTURE_SELFTEST: after a capture of its has been replayed (not before: a call replayed while armed would open a capture).
void rearm() {
    if (sSelftest && sMode && !sCap && __PortGXCap == 0) __PortGXCap = 2;
}

void tokenFromHandler(uint32_t id) {
    if (sTokenFn) {
        sTokenFn(id, sTokenCtx);
    } else {
        ++sTot.contractErrors;
        logLine("gxcap: CONTRACT: token %u must be drawn now but no token handler is registered (port_gx_capture_set_token_handler): "
                "its pass is missing from this picture", (unsigned)id);
    }
}
void handlerTrampoline(uint32_t id, void*) { tokenFromHandler(id); }

void periodicLog();

// Abandon (mode 1): replay what was recorded here and now; the caller's call then runs for real. Mode 2: the verification of what was
// recorded runs (every call has run already), then the same.
void abandonNow(int id) {
    PortGxCapture* c = sCap;
    const bool verify = sVerify;
    closeCapture(false);
    sCap = nullptr;
    sAbandoned = !sSelfCapture;
    sSelfCapture = false;
    ++sTot.abandoned;
    if (id >= 0 && id < PORT_GXCAP_ID_END) ++sTot.abandonBy[id];
    if (verify) {
        PortGxCapture* keep = sCap;
        sCap = c;
        verifyFinish();
        sCap = keep;
        chunkFree(c->head);
        free(c);
    } else {
        replayRecords(c, handlerTrampoline, nullptr, false);
    }
    sVerify = false;
    if (id != PORT_GXCAP_ID_GXInit && id != PORT_GXCAP_ID_GXAbortFrame) rearm();
}

// GXV_GXCAPTURE_SELFTEST: the self-test's capture ends here and is replayed at once on this thread.
void selfEndHere() {
    PortGxCapture* c = sCap;
    const bool verify = sVerify;
    closeCapture(false);
    sCap = nullptr;
    sSelfCapture = false;
    ++sTot.ended;
    if (verify) {
        sCap = c;
        verifyFinish();
        sCap = nullptr;
        chunkFree(c->head);
        free(c);
    } else {
        replayRecords(c, handlerTrampoline, nullptr, false);
    }
    sVerify = false;
    rearm();
}

// A hooked call: true when it is to be recorded (this thread's capture is open, or the self-test opens one now).
// `lazy`: the self-test may open a capture at this entry point (not at the ones the library calls from inside other entry points
// with more of the outer body still to run: GXFlush, the light shadow's drop).
bool enterSlow(bool lazy);
// ---- (pipefix31) the late draw's owner trap (GXV_PIPE25): armed per overlap by port_gx_replay_arm_trap; three-way.
// WHY the first arm crashed (pipefix30's mystery; read from code, shown by the log of p31-old): the trap was "every thread but the helper", and `through` runs the
// real call with the pass word raised, which the hook lets through only for a caller that is not "foreign". The game thread inside the take-over (Join's
// self-run) and any other thread were foreign for their own `through` too: the hook sent the real call back to the layer, which called `through` again: an endless
// recursion down that thread's stack (fp falling 0x28 a level, 17000 levels), over whatever lies below it, until the join disarmed.
// NOW: (a) the helper passes (its stack window); (b) the game thread joins the helper (port_drawpipe_game_enter) and the trap is down before its real call;
// (c) any other thread is counted, waits until no job is out (WaitOther), and then makes its real call under an EXEMPT window: Pass (below) registers the
// thread's whole stack for the length of the call, so that the call and every hooked call inside it pass the hook. One such thread at a time.
std::atomic<uint32_t> sTrapForeign{0}, sTrapGame{0}, sTrapStuck{0}, sTrapRunaway{0}, sTrapRun{0}, sTrapExempt{0};
std::atomic<int> sExLock{0};
std::atomic<uintptr_t> sExOwner{0};  // (p31e: the thread that holds the window: a nested call of that thread never waits for its own lock)
// (p31e) Why other guest threads cannot race the helper through this window: the guest OS (platform/src/os/os_thread.cpp) runs exactly one
// guest thread at a time and switches only inside its own calls, so while another guest thread is in its real GX call the game thread is not
// running and cannot hand a new job over, and while the game thread runs no other guest thread can start one. The window and the wait cover
// the threads outside that model (none is known to call GX: the trap's "call of ANOTHER thread" log lines name any that does).
void exemptEnter(bool& ex) {
    const uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    if (__PortGXCapReplayTop - fp < __PortGXCapReplaySpan) return;  // the helper
    if (__PortGXCapExTop - fp < __PortGXCapExSpan) return;          // nested in the real call of the thread that holds the window
    if (port_on_frame_thread && port_on_frame_thread()) return;     // the game thread (the trap is down for its real call)
    const uintptr_t me = selfId();
    if (sExOwner.load(std::memory_order_acquire) == me && sExLock.load(std::memory_order_acquire) != 0) return;  // (nested deeper than the window: still its own call)
    for (unsigned tries = 0; sExLock.exchange(1, std::memory_order_acquire) != 0; ++tries) {
        if (tries >= 32) {
#if defined(__vita__)
            sceKernelDelayThread(50);
#else
            usleep(50);
#endif
        }
    }
    sExOwner.store(me, std::memory_order_relaxed);
    uintptr_t lo, hi;
    stackNow(&lo, &hi);
    if (lo == hi || fp < lo || fp >= hi) {  // (unknown: the frames of this call and 256 KiB under it)
        lo = fp - (256u << 10);
        hi = fp + 256;
    }
    // (p31e: Top first, Span last, each a release store: a reader that sees the new Span sees the new Top; a half-set window only traps)
    __atomic_store_n(&__PortGXCapExTop, hi - 1, __ATOMIC_RELEASE);
    __atomic_store_n(&__PortGXCapExSpan, hi - lo, __ATOMIC_RELEASE);
    sTrapExempt.fetch_add(1, std::memory_order_relaxed);
    ex = true;
}
void exemptLeave() {
    __atomic_store_n(&__PortGXCapExSpan, static_cast<uintptr_t>(0), __ATOMIC_RELEASE);  // (Span first: no reader can pair the old window with a new Top)
    __atomic_store_n(&__PortGXCapExTop, static_cast<uintptr_t>(0), __ATOMIC_RELEASE);
    sExOwner.store(0, std::memory_order_relaxed);
    sExLock.store(0, std::memory_order_release);
}
bool trapLate() {
    const uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    if (__PortGXCapReplayTop - fp < __PortGXCapReplaySpan) return false;  // the helper (reaches here only with the pass word down)
    if (!(port_on_frame_thread && port_on_frame_thread())) {
        const uint32_t n = sTrapForeign.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3 || (n & 0xFFFu) == 0)
            logLine("gxcap: trap: call #%u of ANOTHER thread tid=%u fp=%p pass=%d: counted, waits until no job is out, then its real call runs under its own exempt window", n,
                    static_cast<unsigned>(selfId()), reinterpret_cast<void*>(fp), __PortGXCapPass);
        if (port_drawpipe_game_enter) port_drawpipe_game_enter();  // (a thread that is not the game thread: WaitOther)
        return false;
    }
    const uint32_t g = sTrapGame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g <= 2) logLine("gxcap: trap: game thread entry #%u tid=%u fp=%p pass=%d", g, static_cast<unsigned>(selfId()), reinterpret_cast<void*>(fp), __PortGXCapPass);
    if (sTrapRun.fetch_add(1, std::memory_order_relaxed) >= 64) {  // (a call that comes back to the layer again and again: never in a correct run)
        sTrapRunaway.fetch_add(1, std::memory_order_relaxed);
        __PortGXCap = 0;
        logLine("gxcap: trap: RUNAWAY (the game thread re-entered the layer 64 times in one overlap): disarmed");
        return false;
    }
    if (port_drawpipe_game_enter) port_drawpipe_game_enter();  // (joins the helper; its OverlapEnd disarms)
    if (__PortGXCap == 3) {
        // Still armed after the join (the game thread's own take-over, or a stale arm): its real call would be trapped again, for ever. Disarmed here.
        if (sTrapStuck.fetch_add(1, std::memory_order_relaxed) < 3) logLine("gxcap: trap: still armed after the join (take-over or stale): disarmed");
        __PortGXCap = 0;
    }
    return false;
}
// (A0: a plain recording and the recording thread: one flag and one compare, inline in the recorder. sFast is only ever set
// while __PortGXCap is 1.)
__attribute__((always_inline)) inline bool enter(bool lazy) {
    if (__builtin_expect(sFast && ownerNear(), 1)) return true;
    return enterSlow(lazy);
}
__attribute__((noinline)) bool enterSlow(bool lazy) {
    if (__PortGXCap == 1) {
        // (the null recorder asks no "which thread": the measurement is of the game thread's draw in a steady scene, and the
        // owner test's own cost must not be in it)
        if (__builtin_expect(sNull != 0, 0) && !sSelfCapture) return !sInCall;
        if (!ownerHere()) {
            ++sTot.foreign;
            return false;
        }
        if (sInCall) return false;
        if (sSelfCapture && sSelfCap && sCap->calls >= sSelfCap && lazy) {
            ++sTot.selfCuts;
            selfEndHere();
            return beginInternal(true);
        }
        return true;
    }
    if (__PortGXCap == 2 && lazy && !sInCall && (!port_on_frame_thread || port_on_frame_thread())) {
        return beginInternal(true);
    }
    if (__PortGXCap == 3) return trapLate();  // (GX used beside a replay on the draw thread: the game thread joins it first, other threads wait; nothing on the draw thread itself)
    return false;
}

// ---------------------------------------------------------------------------------------------------------------- recording
// Mode 2: run the call for real with the hooks letting it through, taking its bytes as the call's.
// A call the layer does not record runs for real: __PortGXCapPass lets it (and the GX calls it makes) through the hooks.
struct Pass {
    bool ex = false;  // (this call runs under the exempt window of another thread: exemptEnter)
    Pass() {
        __atomic_fetch_add(&__PortGXCapPass, 1, __ATOMIC_RELAXED);
        if (__builtin_expect(__PortGXCap == 3, 0)) exemptEnter(ex);
    }
    ~Pass() {
        if (ex) exemptLeave();
        __atomic_fetch_sub(&__PortGXCapPass, 1, __ATOMIC_RELAXED);
    }
};
template <class... P, class... A> int through(void (*fn)(P...), A... a) {
    Pass pass;
    fn(a...);
    return 0;
}

template <class F> void runReal(F&& f) {
    tap();
    flushPendingRaw();
    sPhase = PH_CALL;
    sInCall = true;
    {
        Pass pass;
        f();
    }
    sInCall = false;
    tap();
    sPhase = PH_RAW;
}

template <class... P> struct CallRec {
    void (*fn)(P...);
    std::tuple<P...> args;
};
template <class... P> void callThunk(const uint8_t* payload) {
    const CallRec<P...>* r = reinterpret_cast<const CallRec<P...>*>(payload);
    std::apply(r->fn, r->args);
}
// A call recorded with its arguments by value.
template <class... P, class... A> __attribute__((always_inline)) inline int recordCall(int id, void (*fn)(P...), A... a) {
    using R = CallRec<P...>;
    static_assert(alignof(R) <= 8, "record alignment");
    if (uint8_t* const f = fastRecord(kindAndId(K_CALL, static_cast<uint16_t>(id)), sizeof(R), &callThunk<P...>)) {
        new (f) R{fn, std::tuple<P...>(a...)};
        return 1;
    }
    if (__builtin_expect(sNull != 0, 0)) {  // (the null recorder: counted and dropped, with the raw bytes before it)
        nullDrop();
        ++sCap->calls;
        return 1;
    }
    uint8_t* p = beginRecord(K_CALL, static_cast<uint16_t>(id), sizeof(R), &callThunk<P...>);
    new (p) R{fn, std::tuple<P...>(a...)};
    ++sCap->calls;
    if (sVerify) runReal([&] { fn(a...); });
    else endRecord();
    return 1;
}
// (A0) fastRecord for a by-value call of fn's signature, and the record's payload written.
template <class... P> __attribute__((always_inline)) inline uint8_t* fastRecordOf(FastState* f, int id, void (*)(P...)) {
    return fastRecord(f, kindAndId(K_CALL, static_cast<uint16_t>(id)), sizeof(CallRec<P...>), &callThunk<P...>);
}
template <class... P, class... A> __attribute__((always_inline)) inline void fastFill(uint8_t* at, void (*fn)(P...), A... a) {
    new (at) CallRec<P...>{fn, std::tuple<P...>(a...)};
}
// Room for a copied pointee inside the record (8-aligned, after the CallRec): the replay passes a pointer to it.
template <class... P> struct CopyRec {
    CallRec<P...> call;
    // followed by the copies
};

// Records fn(args) where some arguments point at copies placed after the record; `copy` fills the copies and gives the arguments.
template <class... P, class Fill> __attribute__((always_inline)) inline int recordWithCopies(int id, void (*fn)(P...), uint32_t copyBytes, Fill fill) {
    using R = CallRec<P...>;
    const uint32_t head = align8(sizeof(R));
    if (uint8_t* const f = fastRecord(kindAndId(K_CALL, static_cast<uint16_t>(id)), head + copyBytes, &callThunk<P...>)) {
        new (f) R{fn, fill(f + head)};
        return 1;
    }
    if (__builtin_expect(sNull != 0, 0)) {
        nullDrop();
        ++sCap->calls;
        return 1;
    }
    uint8_t* p = beginRecord(K_CALL, static_cast<uint16_t>(id), head + copyBytes, &callThunk<P...>);
    new (p) R{fn, fill(p + head)};
    ++sCap->calls;
    if (!sVerify) endRecord();
    return 1;
}

void abandonFor(int id) {
    if (sCap && ownerHere()) abandonNow(id);
}

// ---------------------------------------------------------------------------------------------------------------- mirror updates
void mirrorViewport(f32 l, f32 t, f32 w, f32 h, f32 n, f32 f, u32 field) {
    if (field == 0) t -= 0.5f;
    const f32 v[6] = {l, t, w, h, n, f};
    memcpy(sMirror.vp, v, sizeof v);
    sMirror.vpOk = true;
}
void mirrorScissor(u32 left, u32 top, u32 wd, u32 ht) {
    u32 tp, lf, bm, rt;
    tp = top + (u32)342.0f;
    lf = left + (u32)342.0f;
    bm = tp + ht - 1;
    rt = lf + wd - 1;
    SC_SU_SCIS0_SET_SY0(sMirror.scis0, tp);
    SC_SU_SCIS0_SET_SX0(sMirror.scis0, lf);
    SC_SU_SCIS1_SET_SY1(sMirror.scis1, bm);
    SC_SU_SCIS1_SET_SX1(sMirror.scis1, rt);
    sMirror.scisOk = true;
}
void mirrorProjection(const f32 mtx[4][4], GXProjectionType type) {
    sMirror.projType = type;
    sMirror.proj[0] = mtx[0][0];
    sMirror.proj[2] = mtx[1][1];
    sMirror.proj[4] = mtx[2][2];
    sMirror.proj[5] = mtx[2][3];
    if (type == GX_ORTHOGRAPHIC) {
        sMirror.proj[1] = mtx[0][3];
        sMirror.proj[3] = mtx[1][3];
    } else {
        sMirror.proj[1] = mtx[0][2];
        sMirror.proj[3] = mtx[1][2];
    }
    sMirror.projOk = true;
}
void mirrorProjectionv(const f32* ptr) {
    sMirror.projType = (ptr[0] == 0.0f ? GX_PERSPECTIVE : GX_ORTHOGRAPHIC);
    for (int i = 0; i < 6; ++i) sMirror.proj[i] = ptr[1 + i];
    sMirror.projOk = true;
}
void mirrorSerial(u32 n) { sMirror.serial += n; }

// Mode 2: a mirrored answer against the real one.
void verifyDiff(int id, const char* what) {
    ++sTot.verifyDiffs;
    if (!sFirstDiff[0]) snprintf(sFirstDiff, sizeof sFirstDiff, "%s: %s", kNames[id], what);
}

// ---------------------------------------------------------------------------------------------------------------- replay
void replayRecords(PortGxCapture* c, void (*token)(uint32_t, void*), void* ctx, bool scratch) {
    const int64_t t0 = port_host_time_ns();
    const int was = sReplaying.exchange(1, std::memory_order_acq_rel);  // (no capture may begin meanwhile)
    for (Chunk* k = c->head; k; k = k->next) {
        uint32_t at = 0;
        while (at < k->used) {
            const Hdr* h = reinterpret_cast<const Hdr*>(k->data + at);
            const uint8_t* payload = k->data + at + kHdr;
            switch (h->kind) {
            case K_RAW:
                writeBytes(payload, h->size);
                break;
            case K_CALL:
                h->thunk(payload);
                break;
            case K_TOKEN: {
                const TokenRec* t = reinterpret_cast<const TokenRec*>(payload);
                if (scratch) {
                    // mode 2: the pass's real bytes and the state it left
                    writeBytes(payload + align8(sizeof(TokenRec)), t->passBytes);
                    if (t->after) t->after->put();
                } else {
                    const int scope = __PortGXLightScope;
                    __PortGXLightScope = t->scope;
                    token(t->id, ctx);
                    __PortGXLightScope = scope;
                }
                break;
            }
            default:
                port_fatal("gxcap: a broken record (kind %u)", (unsigned)h->kind);
            }
            at += kHdr + align8(h->size);
        }
    }
    sReplaying.store(was, std::memory_order_release);
    if (!scratch) {
        sReplayed.fetch_add(1, std::memory_order_relaxed);
        sReplayNs.fetch_add(static_cast<uint64_t>(port_host_time_ns() - t0), std::memory_order_relaxed);
        Chunk* head = c->head;
        free(c);
        chunkFree(head);
    }
}

// Mode 2: the record replayed into a scratch stream from the state at the start must give the real bytes.
void verifyFinish() {
    PortGxCapture* c = sCap;
    Snapshot* end = static_cast<Snapshot*>(malloc(sizeof(Snapshot)));
    end->take();
    uint8_t* const realPtr = __PortGXFifoPtr;
    uint8_t* const realLimit = __PortGXFifoLimit;
    sStart->put();
    sScratch.clear();
    sSink = true;
    sStreamOverride = sRetOk || sBrOk;
    __PortGXCapFifo = 1;
    __PortGXFifoPtr = sSinkBuf;
    __PortGXFifoLimit = sSinkBuf + sizeof sSinkBuf - kSlack;
    const int scope = __PortGXLightScope;
    replayRecords(c, nullptr, nullptr, true);
    __PortGXLightScope = scope;
    if (__PortGXFifoPtr > sSinkBuf) sScratch.put(sSinkBuf, static_cast<size_t>(__PortGXFifoPtr - sSinkBuf));
    sSink = false;
    sStreamOverride = false;
    __PortGXCapFifo = 0;
    __PortGXFifoPtr = realPtr;
    __PortGXFifoLimit = realLimit;
    end->put();
    free(end);
    ++sTot.verified;
    if (sScratch.n != sTruth.n || memcmp(sScratch.p, sTruth.p, sTruth.n) != 0) {
        size_t i = 0;
        const size_t m = sScratch.n < sTruth.n ? sScratch.n : sTruth.n;
        while (i < m && sScratch.p[i] == sTruth.p[i]) ++i;
        ++sTot.verifyDiffs;
        if (!sFirstDiff[0])
            snprintf(sFirstDiff, sizeof sFirstDiff, "stream: replay %u bytes, real %u, first difference at byte %u (replay %02x real %02x)",  // (no %zu: the Vita's C library misreads it)
                     static_cast<unsigned>(sScratch.n), static_cast<unsigned>(sTruth.n), static_cast<unsigned>(i), i < sScratch.n ? sScratch.p[i] : 0, i < sTruth.n ? sTruth.p[i] : 0);
    }
    // the passes' snapshots
    for (Chunk* k = c->head; k; k = k->next) {
        for (uint32_t at = 0; at < k->used;) {
            const Hdr* h = reinterpret_cast<const Hdr*>(k->data + at);
            if (h->kind == K_TOKEN) free(reinterpret_cast<const TokenRec*>(k->data + at + kHdr)->after);
            at += kHdr + align8(h->size);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------- the log
void periodicLog() {
    if (!sMode) return;
    const int64_t now = port_host_time_ns();
    if (!sLogAt) {
        sLogAt = now;
        sLast = sTot;
        return;
    }
    if (now - sLogAt < 5000000000ll) return;
    const double s = (now - sLogAt) / 1e9;
    const Totals& a = sTot;
    const Totals& b = sLast;
    const uint64_t caps = (a.ended - b.ended) + (a.abandoned - b.abandoned) + (a.verified - b.verified);
    const uint64_t rep = sReplayed.load(std::memory_order_relaxed), repNs = sReplayNs.load(std::memory_order_relaxed);
    char ab[400];
    int n = 0;
    ab[0] = 0;
    for (int i = 0; i < PORT_GXCAP_ID_END && n < (int)sizeof ab - 48; ++i) {
        const uint64_t d = a.abandonBy[i] - b.abandonBy[i];
        if (d) n += snprintf(ab + n, sizeof ab - n, "%s%s %llu", n ? ", " : "", kNames[i], (unsigned long long)d);
    }
    char rf[160];
    int m = 0;
    rf[0] = 0;
    for (int i = 0; i < R_LAST; ++i) {
        const uint64_t d = a.refusedBy[i] - b.refusedBy[i];
        if (d) m += snprintf(rf + m, sizeof rf - m, "%s%s %llu", m ? ", " : "", kRefuse[i], (unsigned long long)d);
    }
    const double per = caps ? 1.0 / caps : 0;
    logLine("gxcap: %.1f s: mode %d%s%s: %llu captures (%llu begun, %llu refused%s%s%s), per capture %.0f calls (%.0f by the cheap path%s) %.1f KiB raw %.1f KiB records, "
            "%llu tokens, %llu getters mirrored, %llu abandoned [%s], %llu self-test cuts, capture open %.2f ms/s, replay %llu in %.2f ms/s, "
            "verify %llu compared %llu differences%s%s, foreign %llu, contract errors %llu",
            s, sMode, sSelftest ? " self-test" : "",
            sNull ? " NULL RECORDER (calls counted and dropped: the pictures are wrong on purpose; 'calls' are the dropped ones)" : "",
            (unsigned long long)caps, (unsigned long long)(a.begun - b.begun),
            (unsigned long long)(a.refused - b.refused), m ? " [" : "", rf, m ? "]" : "", (a.calls - b.calls) * per,
            (a.fastCalls - b.fastCalls) * per, sSlow ? ": GXV_GXCAPTURE_SLOW=1" : "",
            (a.rawBytes - b.rawBytes) * per / 1024.0, (a.recordBytes - b.recordBytes) * per / 1024.0,
            (unsigned long long)(a.tokens - b.tokens), (unsigned long long)(a.getters - b.getters),
            (unsigned long long)(a.abandoned - b.abandoned), ab, (unsigned long long)(a.selfCuts - b.selfCuts),
            (a.captureNs - b.captureNs) / 1e6 / s, (unsigned long long)(rep - sLastReplayed), (repNs - sLastReplayNs) / 1e6 / s,
            (unsigned long long)(a.verified - b.verified), (unsigned long long)(a.verifyDiffs - b.verifyDiffs),
            sFirstDiff[0] ? ", first: " : "", sFirstDiff, (unsigned long long)(a.foreign - b.foreign),
            (unsigned long long)(a.contractErrors - b.contractErrors));
    sFirstDiff[0] = 0;
    sLogAt = now;
    sLast = sTot;
    sLastReplayed = rep;
    sLastReplayNs = repNs;
}

}  // namespace

// ==================================================================================================================== the hooks
extern "C" {

// Recorded by value (kind R): the generic record. The self-test may open a capture at any of them but GXFlush and the light shadow's
// drop (the library calls those from inside other entry points with more of the outer body still to run).
constexpr bool lazyAt(int id) { return id != PORT_GXCAP_ID_GXFlush && id != PORT_GXCAP_ID___PortGXLightShadowDrop; }
// (A0) The plain recording's case first and alone in the recorder (the thread's compare, then the record in line), everything
// else in a function of its own: the straight path then needs no saved registers and makes no call.
#define PORT_GXCAP_DEFINE_R(k, n, p, a)                                                                                             \
    __attribute__((noinline)) static int port_gxcap_rest_##n p {                                                                    \
        if (!enterSlow(lazyAt(PORT_GXCAP_ID_##n))) return through(&n PORT_GXCAP_ARGS a);                                            \
        return recordCall(PORT_GXCAP_ID_##n, &n PORT_GXCAP_ARGS a);                                                                 \
    }                                                                                                                               \
    int port_gxcap_##n p {                                                                                                          \
        FastState* const fs = fastState();                                                                                          \
        if (__builtin_expect(fastOwner(fs), 1)) {                                                                                   \
            if (uint8_t* const f = fastRecordOf(fs, PORT_GXCAP_ID_##n, &n)) {                                                       \
                fastFill(f, &n PORT_GXCAP_ARGS a);                                                                                  \
                return 1;                                                                                                           \
            }                                                                                                                       \
        }                                                                                                                           \
        return port_gxcap_rest_##n a;                                                                                               \
    }
#define PORT_GXCAP_ARGS(...) , ##__VA_ARGS__  // (no comma for a call without arguments)
#define PORT_GXCAP_DEFINE_SKIP(k, n, p, a)
#define PORT_GXCAP_DEFINE_SEL_R PORT_GXCAP_DEFINE_R
#define PORT_GXCAP_DEFINE_SEL_M PORT_GXCAP_DEFINE_SKIP
#define PORT_GXCAP_DEFINE_SEL_P PORT_GXCAP_DEFINE_SKIP
#define PORT_GXCAP_DEFINE_SEL_G PORT_GXCAP_DEFINE_SKIP
#define PORT_GXCAP_DEFINE_SEL_S PORT_GXCAP_DEFINE_SKIP
#define PORT_GXCAP_DEFINE_SEL_T PORT_GXCAP_DEFINE_SKIP
#define PORT_GXCAP_DEFINE(k, n, p, a) PORT_GXCAP_DEFINE_SEL_##k(k, n, p, a)
PORT_GXCAP_RECORD_LIST(PORT_GXCAP_DEFINE)

// ---- M: by value, and the mirror follows (the draw serial counts as GXGeometry.c / GXDisplayList.c / GXFrameBuf.c count it)
int port_gxcap_GXBegin(GXPrimitive type, GXVtxFmt fmt, u16 n) {
    if (!enter(true)) return through(&GXBegin, type, fmt, n);
    mirrorSerial(1);
    return recordCall(PORT_GXCAP_ID_GXBegin, &GXBegin, type, fmt, n);
}
int port_gxcap_GXCallDisplayList(const void* list, u32 nbytes) {
    if (!enter(true)) return through(&GXCallDisplayList, list, nbytes);
    mirrorSerial(1);
    return recordCall(PORT_GXCAP_ID_GXCallDisplayList, &GXCallDisplayList, list, nbytes);
}
int port_gxcap_GXCopyDisp(void* dest, GXBool clear) {
    if (!enter(true)) return through(&GXCopyDisp, dest, clear);
    mirrorSerial(1);
    return recordCall(PORT_GXCAP_ID_GXCopyDisp, &GXCopyDisp, dest, clear);
}
int port_gxcap_GXCopyTex(void* dest, GXBool clear) {
    if (!enter(true)) return through(&GXCopyTex, dest, clear);
    mirrorSerial(1);
    return recordCall(PORT_GXCAP_ID_GXCopyTex, &GXCopyTex, dest, clear);
}
// (the retained / bracketed call counts once, and once more when it falls back to GXCallDisplayList: decided by the tests answered
// at the start, which hold at replay too)
int port_gxcap_port_gx_call_retained(const void* list, u32 nbytes, u32 handle) {
    if (!enter(true)) return through(&port_gx_call_retained, list, nbytes, handle);
    mirrorSerial((__PortGXFx || handle == 0 || !sRetOk) ? 2 : 1);
    return recordCall(PORT_GXCAP_ID_port_gx_call_retained, &port_gx_call_retained, list, nbytes, handle);
}
int port_gxcap_port_gx_call_bracketed(const void* list, u32 nbytes, u32 id, u32 verA, u32 verB, u32 ext, u32 flags) {
    if (!enter(true)) return through(&port_gx_call_bracketed, list, nbytes, id, verA, verB, ext, flags);
    mirrorSerial((__PortGXFx || !sBrOk) ? 2 : 1);
    return recordCall(PORT_GXCAP_ID_port_gx_call_bracketed, &port_gx_call_bracketed, list, nbytes, id, verA, verB, ext, flags);
}
int port_gxcap_GXSetViewport(f32 l, f32 t, f32 w, f32 h, f32 n, f32 f) {
    if (!enter(true)) return through(&GXSetViewport, l, t, w, h, n, f);
    mirrorViewport(l, t, w, h, n, f, 1);
    return recordCall(PORT_GXCAP_ID_GXSetViewport, &GXSetViewport, l, t, w, h, n, f);
}
int port_gxcap_GXSetViewportJitter(f32 l, f32 t, f32 w, f32 h, f32 n, f32 f, u32 field) {
    if (!enter(true)) return through(&GXSetViewportJitter, l, t, w, h, n, f, field);
    mirrorViewport(l, t, w, h, n, f, field);
    return recordCall(PORT_GXCAP_ID_GXSetViewportJitter, &GXSetViewportJitter, l, t, w, h, n, f, field);
}
int port_gxcap_GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    if (!enter(true)) return through(&GXSetScissor, left, top, wd, ht);
    mirrorScissor(left, top, wd, ht);
    return recordCall(PORT_GXCAP_ID_GXSetScissor, &GXSetScissor, left, top, wd, ht);
}

// ---- P: the pointee copied
int port_gxcap_GXLoadPosMtxImm(const f32 mtx[3][4], u32 id) {
    if (!enter(true)) return through(&GXLoadPosMtxImm, mtx, id);
    recordWithCopies(PORT_GXCAP_ID_GXLoadPosMtxImm, &GXLoadPosMtxImm, 48, [&](uint8_t* copy) {
        memcpy(copy, mtx, 48);
        return std::tuple<const f32(*)[4], u32>(reinterpret_cast<const f32(*)[4]>(copy), id);
    });
    if (sVerify) runReal([&] { GXLoadPosMtxImm(mtx, id); });
    return 1;
}
int port_gxcap_GXLoadNrmMtxImm(const f32 mtx[3][4], u32 id) {
    if (!enter(true)) return through(&GXLoadNrmMtxImm, mtx, id);
    recordWithCopies(PORT_GXCAP_ID_GXLoadNrmMtxImm, &GXLoadNrmMtxImm, 48, [&](uint8_t* copy) {
        memcpy(copy, mtx, 48);
        return std::tuple<const f32(*)[4], u32>(reinterpret_cast<const f32(*)[4]>(copy), id);
    });
    if (sVerify) runReal([&] { GXLoadNrmMtxImm(mtx, id); });
    return 1;
}
int port_gxcap_GXLoadTexMtxImm(const f32 mtx[][4], u32 id, GXTexMtxType type) {
    if (!enter(true)) return through(&GXLoadTexMtxImm, mtx, id, type);
    const uint32_t n = type == GX_MTX2x4 ? 32 : 48;  // (what the library reads: two or three rows)
    recordWithCopies(PORT_GXCAP_ID_GXLoadTexMtxImm, &GXLoadTexMtxImm, 48, [&](uint8_t* copy) {
        memset(copy, 0, 48);
        memcpy(copy, mtx, n);
        return std::tuple<const f32(*)[4], u32, GXTexMtxType>(reinterpret_cast<const f32(*)[4]>(copy), id, type);
    });
    if (sVerify) runReal([&] { GXLoadTexMtxImm(mtx, id, type); });
    return 1;
}
int port_gxcap_GXSetProjection(const f32 mtx[4][4], GXProjectionType type) {
    if (!enter(true)) return through(&GXSetProjection, mtx, type);
    mirrorProjection(mtx, type);
    recordWithCopies(PORT_GXCAP_ID_GXSetProjection, &GXSetProjection, 64, [&](uint8_t* copy) {
        memcpy(copy, mtx, 64);
        return std::tuple<const f32(*)[4], GXProjectionType>(reinterpret_cast<const f32(*)[4]>(copy), type);
    });
    if (sVerify) runReal([&] { GXSetProjection(mtx, type); });
    return 1;
}
int port_gxcap_GXSetProjectionv(const f32* ptr) {
    if (!enter(true)) return through(&GXSetProjectionv, ptr);
    mirrorProjectionv(ptr);
    recordWithCopies(PORT_GXCAP_ID_GXSetProjectionv, &GXSetProjectionv, 28, [&](uint8_t* copy) {
        memcpy(copy, ptr, 28);
        return std::tuple<const f32*>(reinterpret_cast<const f32*>(copy));
    });
    if (sVerify) runReal([&] { GXSetProjectionv(ptr); });
    return 1;
}
int port_gxcap_GXSetIndTexMtx(GXIndTexMtxID mtxId, const f32 offset[2][3], s8 scale) {
    if (!enter(true)) return through(&GXSetIndTexMtx, mtxId, offset, scale);
    recordWithCopies(PORT_GXCAP_ID_GXSetIndTexMtx, &GXSetIndTexMtx, 24, [&](uint8_t* copy) {
        memcpy(copy, offset, 24);
        return std::tuple<GXIndTexMtxID, const f32(*)[3], s8>(mtxId, reinterpret_cast<const f32(*)[3]>(copy), scale);
    });
    if (sVerify) runReal([&] { GXSetIndTexMtx(mtxId, offset, scale); });
    return 1;
}
int port_gxcap_GXSetFogRangeAdj(GXBool enable, u16 center, const GXFogAdjTable* table) {
    if (!enter(true)) return through(&GXSetFogRangeAdj, enable, center, table);
    const bool copyIt = enable && table;  // (the library reads the table only when enabled)
    recordWithCopies(PORT_GXCAP_ID_GXSetFogRangeAdj, &GXSetFogRangeAdj, sizeof(GXFogAdjTable), [&](uint8_t* copy) {
        if (copyIt) memcpy(copy, table, sizeof(GXFogAdjTable));
        return std::tuple<GXBool, u16, const GXFogAdjTable*>(enable, center, copyIt ? reinterpret_cast<const GXFogAdjTable*>(copy) : table);
    });
    if (sVerify) runReal([&] { GXSetFogRangeAdj(enable, center, table); });
    return 1;
}
int port_gxcap_GXSetCopyFilter(GXBool aa, const u8 pattern[12][2], GXBool vf, const u8 vfilter[7]) {
    if (!enter(true)) return through(&GXSetCopyFilter, aa, pattern, vf, vfilter);
    const bool cp = aa && pattern, cv = vf && vfilter;  // (each table is read only when its flag is set)
    recordWithCopies(PORT_GXCAP_ID_GXSetCopyFilter, &GXSetCopyFilter, 32, [&](uint8_t* copy) {
        if (cp) memcpy(copy, pattern, 24);
        if (cv) memcpy(copy + 24, vfilter, 7);
        return std::tuple<GXBool, const u8(*)[2], GXBool, const u8*>(aa, cp ? reinterpret_cast<const u8(*)[2]>(copy) : pattern, vf,
                                                                     cv ? copy + 24 : vfilter);
    });
    if (sVerify) runReal([&] { GXSetCopyFilter(aa, pattern, vf, vfilter); });
    return 1;
}
int port_gxcap_GXSetVtxAttrFmtv(GXVtxFmt fmt, const GXVtxAttrFmtList* list) {
    if (!enter(true)) return through(&GXSetVtxAttrFmtv, fmt, list);
    uint32_t n = 0;
    while (list[n].attr != GX_VA_NULL) ++n;
    ++n;  // (the terminator, read too)
    recordWithCopies(PORT_GXCAP_ID_GXSetVtxAttrFmtv, &GXSetVtxAttrFmtv, align8(n * sizeof(GXVtxAttrFmtList)), [&](uint8_t* copy) {
        memcpy(copy, list, n * sizeof(GXVtxAttrFmtList));
        return std::tuple<GXVtxFmt, const GXVtxAttrFmtList*>(fmt, reinterpret_cast<const GXVtxAttrFmtList*>(copy));
    });
    if (sVerify) runReal([&] { GXSetVtxAttrFmtv(fmt, list); });
    return 1;
}

// The light load: the object copied, and the light dedup's scope as it is now (the replay sets it around the call).
struct LightRec {
    GXLightObj obj;
    GXLightID light;
    int scope;
};
void lightThunk(const uint8_t* p) {
    const LightRec* r = reinterpret_cast<const LightRec*>(p);
    const int scope = __PortGXLightScope;
    __PortGXLightScope = r->scope;
    GXLoadLightObjImm(&r->obj, r->light);
    __PortGXLightScope = scope;
}
int port_gxcap_GXLoadLightObjImm(const GXLightObj* obj, GXLightID light) {
    if (!enter(true)) return through(&GXLoadLightObjImm, obj, light);
    LightRec* r = reinterpret_cast<LightRec*>(beginRecord(K_CALL, PORT_GXCAP_ID_GXLoadLightObjImm, sizeof(LightRec), &lightThunk));
    memcpy(&r->obj, obj, sizeof r->obj);
    r->light = light;
    r->scope = __PortGXLightScope;
    ++sCap->calls;
    if (sVerify) runReal([&] { GXLoadLightObjImm(obj, light); });
    else endRecord();
    return 1;
}

// The texture load: the object copied; the register ids the library writes into it are written into the game's object now.
int port_gxcap_GXLoadTexObj(const GXTexObj* obj, GXTexMapID id) {
    if (!enter(true)) return through(&GXLoadTexObj, obj, id);
    if (!__PortGXRegionDefault()) {  // (a game region callback would run on the replaying thread)
        abandonFor(PORT_GXCAP_ID_texregion_custom);
        return through(&GXLoadTexObj, obj, id);
    }
    recordWithCopies(PORT_GXCAP_ID_GXLoadTexObj, &GXLoadTexObj, sizeof(GXTexObj), [&](uint8_t* copy) {
        memcpy(copy, obj, sizeof(GXTexObj));
        return std::tuple<const GXTexObj*, GXTexMapID>(reinterpret_cast<const GXTexObj*>(copy), id);
    });
    if (sVerify) {
        GXTexObj expect;
        memcpy(&expect, obj, sizeof expect);
        __PortGXTexObjSetIds(&expect, id);
        runReal([&] { GXLoadTexObj(obj, id); });
        if (memcmp(&expect, obj, sizeof expect) != 0) verifyDiff(PORT_GXCAP_ID_GXLoadTexObj, "the object's register ids");
    } else {
        __PortGXTexObjSetIds(const_cast<GXTexObj*>(obj), id);
    }
    return 1;
}
int port_gxcap_GXLoadTlut(const GXTlutObj* obj, u32 name) {
    if (!enter(true)) return through(&GXLoadTlut, obj, name);
    GXTlutObj expect;
    memcpy(&expect, obj, sizeof expect);
    if (!__PortGXTlutObjSetOffset(&expect, name)) {
        abandonFor(PORT_GXCAP_ID_texregion_custom);
        return through(&GXLoadTlut, obj, name);
    }
    recordWithCopies(PORT_GXCAP_ID_GXLoadTlut, &GXLoadTlut, sizeof(GXTlutObj), [&](uint8_t* copy) {
        memcpy(copy, obj, sizeof(GXTlutObj));
        return std::tuple<const GXTlutObj*, u32>(reinterpret_cast<const GXTlutObj*>(copy), name);
    });
    if (sVerify) {
        runReal([&] { GXLoadTlut(obj, name); });
        if (memcmp(&expect, obj, sizeof expect) != 0) verifyDiff(PORT_GXCAP_ID_GXLoadTlut, "the object's TMEM offset");
    } else {
        memcpy(const_cast<GXTlutObj*>(obj), &expect, sizeof expect);
    }
    return 1;
}

// ---- S
// GXSetDrawSync: recorded with GXV_GXCAPTURE_SYNC=1 (the token reaches the front end later: the game's callback runs later than
// today, the stream is the same), else it abandons.
int port_gxcap_GXSetDrawSync(u16 token) {
    if (!enter(true)) return through(&GXSetDrawSync, token);
    if (!sSync) {
        abandonFor(PORT_GXCAP_ID_GXSetDrawSync_abandon);
        return through(&GXSetDrawSync, token);
    }
    return recordCall(PORT_GXCAP_ID_GXSetDrawSync, &GXSetDrawSync, token);
}
// The particle gathering (GXV_FXMERGE): port_gx_fx_begin answers 0 without any effect when the gathering cannot start (the switch is
// off); that answer is given here. A gathering that would start abandons. port_gx_fx_end with no gathering does nothing.
int port_gxcap_port_gx_fx_begin(void) {
    if (enter(false)) {
        if (__PortGXFx || !sFxOk) return 0;
        abandonFor(PORT_GXCAP_ID_fx_begin);
    }
    Pass pass;
    return port_gx_fx_begin();
}
int port_gxcap_port_gx_fx_end(void) {
    if (enter(false)) {
        if (!__PortGXFx) return 1;
        abandonFor(PORT_GXCAP_ID_fx_begin);
    }
    return through(&port_gx_fx_end);
}

// ---- G: answered from the mirror (mode 2: also from the library, and compared)
static bool getterOk(bool ok, int id) {
    ++sTot.getters;
    if (ok) return true;
    if (sVerify) {
        ++sTot.wouldAbandon;  // (mode 1 abandons here; mode 2 answers from the library and has no mirror value to compare)
        return true;
    }
    abandonFor(PORT_GXCAP_ID_getter_after_token);
    return false;
}
int port_gxcap_GXGetViewportv(f32* vp) {
    if (!enter(false)) return through(&GXGetViewportv, vp);
    if (!getterOk(sMirror.vpOk, PORT_GXCAP_ID_GXGetViewportv)) return through(&GXGetViewportv, vp);
    if (sVerify) {
        runReal([&] { GXGetViewportv(vp); });
        if (sMirror.vpOk && memcmp(vp, sMirror.vp, sizeof sMirror.vp) != 0) verifyDiff(PORT_GXCAP_ID_GXGetViewportv, "mirror differs");
        return 1;
    }
    memcpy(vp, sMirror.vp, sizeof sMirror.vp);
    return 1;
}
int port_gxcap_GXGetProjectionv(f32* ptr) {
    if (!enter(false)) return through(&GXGetProjectionv, ptr);
    if (!getterOk(sMirror.projOk, PORT_GXCAP_ID_GXGetProjectionv)) return through(&GXGetProjectionv, ptr);
    f32 m[7];
    m[0] = sMirror.projType ? 1.0f : 0.0f;
    memcpy(&m[1], sMirror.proj, sizeof sMirror.proj);
    if (sVerify) {
        runReal([&] { GXGetProjectionv(ptr); });
        if (sMirror.projOk && memcmp(ptr, m, sizeof m) != 0) verifyDiff(PORT_GXCAP_ID_GXGetProjectionv, "mirror differs");
        return 1;
    }
    memcpy(ptr, m, sizeof m);
    return 1;
}
int port_gxcap_GXGetScissor(u32* left, u32* top, u32* wd, u32* ht) {
    if (!enter(false)) return through(&GXGetScissor, left, top, wd, ht);
    if (!getterOk(sMirror.scisOk, PORT_GXCAP_ID_GXGetScissor)) return through(&GXGetScissor, left, top, wd, ht);
    u32 tp, lf, bm, rt, v[4];
    tp = SU_SCIS0_GET_SY0(sMirror.scis0);
    lf = SU_SCIS0_GET_SX0(sMirror.scis0);
    bm = SU_SCIS1_GET_SY1(sMirror.scis1);
    rt = SU_SCIS1_GET_SX1(sMirror.scis1);
    v[0] = lf - (u32)342.0f;
    v[1] = tp - (u32)342.0f;
    v[2] = rt - lf + 1;
    v[3] = bm - tp + 1;
    if (sVerify) {
        runReal([&] { GXGetScissor(left, top, wd, ht); });
        if (sMirror.scisOk && (*left != v[0] || *top != v[1] || *wd != v[2] || *ht != v[3])) verifyDiff(PORT_GXCAP_ID_GXGetScissor, "mirror differs");
        return 1;
    }
    *left = v[0];
    *top = v[1];
    *wd = v[2];
    *ht = v[3];
    return 1;
}
// GXPeekARGB / GXPeekZ: with the vgx renderer the library answers a constant without reading anything (gx_fifo.cpp
// __PortGXPeekARGB); that answer is given here. With any other renderer it reads the frame buffer: abandon.
int port_gxcap_GXPeekARGB(u16 x, u16 y, u32* color) {
    if (!enter(false)) return through(&GXPeekARGB, x, y, color);
    if (!__PortGXPeekConst || !__PortGXPeekConst()) {
        abandonFor(PORT_GXCAP_ID_GXPeek_abandon);
        return through(&GXPeekARGB, x, y, color);
    }
    ++sTot.getters;
    *color = __PortGXPeekARGB(x, y);
    return 1;
}
int port_gxcap_GXPeekZ(u16 x, u16 y, u32* z) {
    if (!enter(false)) return through(&GXPeekZ, x, y, z);
    if (!__PortGXPeekConst || !__PortGXPeekConst()) {
        abandonFor(PORT_GXCAP_ID_GXPeek_abandon);
        return through(&GXPeekZ, x, y, z);
    }
    ++sTot.getters;
    *z = __PortGXPeekZ(x, y);
    return 1;
}
// The draw serial (PETARI_BLOOM_SKIP): the start's value plus what the recorded calls add; unknown after a token (its pass draws).
u32 port_gxcap_port_gx_draw_serial(void) {
    if (enter(false)) {
        ++sTot.getters;
        if (!sMirror.serialOk) {
            if (sVerify) {
                ++sTot.wouldAbandon;
            } else {
                abandonFor(PORT_GXCAP_ID_draw_serial_after_token);
                Pass pass;
                return port_gx_draw_serial();
            }
        }
        if (sVerify) {
            u32 real = 0;
            runReal([&] { real = port_gx_draw_serial(); });
            if (sMirror.serialOk && real != sMirror.serial) verifyDiff(PORT_GXCAP_ID_port_gx_draw_serial, "mirror differs");
            return real;
        }
        return sMirror.serial;
    }
    Pass pass;
    return port_gx_draw_serial();
}

// ---- A
void port_gxcap_abandon_entry(int id) {
    if (__PortGXCap == 1) {
        if (sCap && ownerHere() && !sInCall) abandonNow(id);
        return;
    }
    if (__PortGXCap == 3 && port_drawpipe_game_enter) {
        port_drawpipe_game_enter();
        return;
    }
    // (armed: GXInit and GXAbortFrame run many recordable calls inside their bodies; the self-test opens nothing until the next turn)
    if (__PortGXCap == 2 && (id == PORT_GXCAP_ID_GXInit || id == PORT_GXCAP_ID_GXAbortFrame)) __PortGXCap = 0;
}

// ---- the FIFO's events (gx_fifo.cpp, and a host test's FIFO)
int port_gxcap_fifo_pre(int event) {
    if (sSink) {  // mode 2's scratch replay: the bytes go to the scratch buffer, nothing is published
        if (__PortGXFifoPtr > sSinkBuf) sScratch.put(sSinkBuf, static_cast<size_t>(__PortGXFifoPtr - sSinkBuf));
        __PortGXFifoPtr = sSinkBuf;
        return 1;
    }
    if (!sCap || !ownerHere()) return 0;
    if (sNull && event == PORT_GXCAP_EV_OVERFLOW && __PortGXFifoPtr >= sRawStart && __PortGXFifoPtr <= chunkEnd(tailChunk())) {
        nullDrop();  // (the null recorder: a full run of raw bytes is dropped, the game writes on into the same room)
        return 1;
    }
    if (sVerify) {  // the real event runs; what was written before it is taken first
        tap();
        return 0;
    }
    if (event == PORT_GXCAP_EV_OVERFLOW && __PortGXFifoPtr >= sRawStart && __PortGXFifoPtr <= chunkEnd(tailChunk())) {
        closeRaw();
        newChunk();
        openRaw();
        return 1;
    }
    // A flush or the idle point while recording: not from a recorded call (those are records); the real FIFO is needed now.
    abandonNow(event == PORT_GXCAP_EV_FLUSH ? PORT_GXCAP_ID_fifo_flush
               : event == PORT_GXCAP_EV_IDLE ? PORT_GXCAP_ID_fifo_idle : PORT_GXCAP_ID_fifo_overflow);
    return 0;
}
void port_gxcap_fifo_post(int) {
    if (sVerify && sCap && ownerHere()) sTapMark = __PortGXFifoPtr;
}
int port_gxcap_stream_override(void) { return sStreamOverride ? 1 : 0; }

// ==================================================================================================================== interface
int port_gx_capture_mode(void) {
    readSwitches();
    return sMode;
}

void port_gx_capture_init(void) {
    readSwitches();
    logLine("gxcap: GXV_GXCAPTURE=%d (%s), GXV_GXCAPTURE_SELFTEST=%d, GXV_GXCAPTURE_SYNC=%d, GXV_GXCAPTURE_SELFTEST_CALLS=%u%s%s", sMode,
            sMode == 1 ? "on" : sMode == 2 ? "verify" : "off", sSelftest, sSync, (unsigned)sSelfCap,
            sSlow ? ", GXV_GXCAPTURE_SLOW=1: the record path of before the cheap one" : "",
            sNull ? ", GXV_GXCAPTURE_NULL=1: THE NULL RECORDER, a measurement: recorded calls are counted and dropped, the pictures are wrong on purpose" : "");
}

// The turn's start (vgx_galaxy.cpp port_vgx_present_turn, game thread): the self-test's open capture ends and is replayed, the
// self-test is armed again, the log line.
void port_gxcap_turn(void) {
    if (sMode <= 0) return;
    if (sSelftest) {
        if (sCap && sSelfCapture && ownerHere()) selfEndHere();
        if (!sCap && __PortGXCap == 0) __PortGXCap = 2;
    }
    if (!sCap) periodicLog();
}

bool port_gx_capture_begin(void) {
    readSwitches();
    if (sCap && sSelfCapture && ownerHere()) {
        ++sTot.refused;
        ++sTot.refusedBy[R_OPEN];
        return false;
    }
    return beginInternal(false);
}

bool port_gx_capture_open(void) { return sCap != nullptr && !sSelfCapture && ownerHere(); }

void port_gx_capture_set_token_handler(void (*token)(uint32_t id, void* ctx), void* ctx) {
    sTokenFn = token;
    sTokenCtx = ctx;
}

void port_gx_capture_token_keep(uint32_t id, uint32_t keep) {
    if (!sCap || !ownerHere()) {
        if (sAbandoned && ownerHere()) {
            tokenFromHandler(id);  // (abandoned earlier: the pass runs here, where it runs serially)
        } else {
            ++sTot.contractErrors;
            logLine("gxcap: CONTRACT: port_gx_capture_token(%u) with no capture begun on this thread: the pass runs now", (unsigned)id);
            tokenFromHandler(id);
        }
        return;
    }
    if (!sVerify) {
        TokenRec* t = reinterpret_cast<TokenRec*>(beginRecord(K_TOKEN, 0, sizeof(TokenRec), nullptr));
        t->id = id;
        t->keep = keep;
        t->scope = __PortGXLightScope;
        t->passBytes = 0;
        t->after = nullptr;
        ++sCap->tokens;
        endRecord();
    } else {
        // Mode 2: the pass runs now, for real; its bytes and the state it leaves go into the record for the scratch replay.
        tap();
        flushPendingRaw();
        const Mirror before = sMirror;
        u32 serialBefore = 0;
        {
            Pass pass;
            serialBefore = port_gx_draw_serial();
        }
        sPassBytes.clear();
        sPhase = PH_PASS;
        sInCall = true;
        {
            Pass pass;
            tokenFromHandler(id);
        }
        sInCall = false;
        tap();
        sPhase = PH_RAW;
        TokenRec* t = reinterpret_cast<TokenRec*>(beginRecord(K_TOKEN, 0, align8(sizeof(TokenRec)) + static_cast<uint32_t>(sPassBytes.n), nullptr));
        t->id = id;
        t->keep = keep;
        t->scope = __PortGXLightScope;
        t->passBytes = static_cast<uint32_t>(sPassBytes.n);
        t->after = static_cast<Snapshot*>(malloc(sizeof(Snapshot)));
        t->after->take();
        memcpy(reinterpret_cast<uint8_t*>(t) + align8(sizeof(TokenRec)), sPassBytes.p, sPassBytes.n);
        ++sCap->tokens;
        // the claims of `keep`, against what the pass really left
        if ((keep & PORT_GXCAP_KEEP_VIEWPORT) && memcmp(&__GXData->vpLeft, before.vp, sizeof before.vp) != 0)
            verifyDiff(PORT_GXCAP_ID_GXGetViewportv, "a token declared to keep the viewport changed it");
        if ((keep & PORT_GXCAP_KEEP_PROJECTION) &&
            (__GXData->projType != before.projType || memcmp(__GXData->projMtx, before.proj, sizeof before.proj) != 0))
            verifyDiff(PORT_GXCAP_ID_GXGetProjectionv, "a token declared to keep the projection changed it");
        if ((keep & PORT_GXCAP_KEEP_SCISSOR) && (__GXData->suScis0 != before.scis0 || __GXData->suScis1 != before.scis1))
            verifyDiff(PORT_GXCAP_ID_GXGetScissor, "a token declared to keep the scissor changed it");
        if (keep & PORT_GXCAP_KEEP_SERIAL) {
            Pass pass;
            if (port_gx_draw_serial() != serialBefore)
                verifyDiff(PORT_GXCAP_ID_port_gx_draw_serial, "a token declared to leave the draw serial alone moved it");
        }
    }
    if (!(keep & PORT_GXCAP_KEEP_VIEWPORT)) sMirror.vpOk = false;
    if (!(keep & PORT_GXCAP_KEEP_PROJECTION)) sMirror.projOk = false;
    if (!(keep & PORT_GXCAP_KEEP_SCISSOR)) sMirror.scisOk = false;
    if (!(keep & PORT_GXCAP_KEEP_SERIAL)) sMirror.serialOk = false;
}

void port_gx_capture_token(uint32_t id) { port_gx_capture_token_keep(id, 0); }

PortGxCapture* port_gx_capture_end(void) {
    if (!sCap || !ownerHere() || sSelfCapture) {
        if (sAbandoned && ownerHere()) sAbandoned = false;
        return nullptr;
    }
    PortGxCapture* c = sCap;
    const bool verify = sVerify;
    closeCapture(false);
    sCap = nullptr;
    sAbandoned = false;
    if (verify) {
        sCap = c;
        verifyFinish();
        sCap = nullptr;
        chunkFree(c->head);
        free(c);
        sVerify = false;
        periodicLog();
        return nullptr;
    }
    ++sTot.ended;
    periodicLog();
    return c;
}

// The game thread, when it hands a capture to another thread for a replay beside it (the draw pipeline's hand-over, with
// __PortGXCapTrap set): the hooks are armed from here on, not from the moment the other thread gets to port_gx_replay, so a GX
// call of the game thread in between joins too. [lo, lo + span): the replaying thread's stack. span 0: disarmed (after the join).
void port_gx_replay_arm(uintptr_t lo, uintptr_t span) {
    if (span == 0) {
        if (__PortGXCap == 3) __PortGXCap = 0;
        return;
    }
    if (__PortGXCapTrap == 0 || __PortGXCap != 0) return;
    __PortGXCapReplayTop = lo + span - 1;
    __PortGXCapReplaySpan = span;
    __PortGXCap = 3;
}
// (pipefix31) The late draw's trap (three-way, see trapLate): the window is the helper's stack. 0: not armed (no trap flag, the layer is not idle, no stack range).
// The disarm is port_gx_replay_arm(0, 0).
int port_gx_replay_arm_trap(uintptr_t lo, uintptr_t span) {
    if (__PortGXCapTrap == 0 || __PortGXCap != 0 || span == 0) return 0;
    __PortGXCapReplayTop = lo + span - 1;
    __PortGXCapReplaySpan = span;
    sTrapRun.store(0, std::memory_order_relaxed);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __PortGXCap = 3;
    return 1;
}
void port_gx_trap_stats(uint32_t* foreign, uint32_t* gameEntries, uint32_t* stuck, uint32_t* runaway) {
    *foreign = sTrapForeign.load(std::memory_order_relaxed);
    *gameEntries = sTrapGame.load(std::memory_order_relaxed);
    *stuck = sTrapStuck.load(std::memory_order_relaxed);
    *runaway = sTrapRunaway.load(std::memory_order_relaxed);
}

// (pipefix31) The start-up checks of the owner trap (the late draw runs them once, before use, with no draw in flight). A hooked getter and a hooked setter
// (the same scissor back) are the probe: a thread makes both under the armed trap. port_gx_trap_selftest: the basic cases (a thread with no job out, with the
// pass word up and down; the calling thread's own call under an armed trap). The late draw adds the case that matters, a probe thread that calls WHILE a
// job is out on the helper: it must return only after the job ended (port_gx_trap_probe_start / _wait). nullptr: it behaves; else the reason (the late
// draw then reports "pipe25: DISABLED: <reason>" and draws serially).
namespace {
struct TrapProbe {
    std::atomic<int> done{0};
    int64_t returnedAt = 0;
    pthread_t th;
};
void* trapProbeMain(void* arg) {
    TrapProbe* p = static_cast<TrapProbe*>(arg);
    u32 l, t, w, h;
    GXGetScissor(&l, &t, &w, &h);
    GXSetScissor(l, t, w, h);
    p->returnedAt = port_host_time_ns();
    p->done.store(1, std::memory_order_release);
    return nullptr;
}
}  // namespace
void* port_gx_trap_probe_start(void) {
    TrapProbe* p = new (std::nothrow) TrapProbe;
    if (!p) return nullptr;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 10);
    const bool made = pthread_create(&p->th, &attr, &trapProbeMain, p) == 0;
    pthread_attr_destroy(&attr);
    if (!made) {
        delete p;
        return nullptr;
    }
    return p;
}
int port_gx_trap_probe_wait(void* probe, uint32_t ms, int64_t* returnedAtNs) {
    TrapProbe* p = static_cast<TrapProbe*>(probe);
    if (!p) return 0;
    for (uint32_t i = 0; i < ms && p->done.load(std::memory_order_acquire) == 0; ++i) {
#if defined(__vita__)
        sceKernelDelayThread(1000);
#else
        usleep(1000);
#endif
    }
    const int ok = p->done.load(std::memory_order_acquire);
    if (!ok) __PortGXCap = 0;  // (a thread that is stuck in the trap: let it out, then it is joined)
    pthread_join(p->th, nullptr);
    if (returnedAtNs) *returnedAtNs = p->returnedAt;
    delete p;
    return ok;
}
namespace {
bool trapProbeOnce(uintptr_t hLo, uintptr_t hSpan, int passRaised) {
    if (!port_gx_replay_arm_trap(hLo, hSpan)) return false;
    if (passRaised) __atomic_fetch_add(&__PortGXCapPass, 1, __ATOMIC_RELAXED);
    void* pr = port_gx_trap_probe_start();
    const bool ok = pr != nullptr && port_gx_trap_probe_wait(pr, 1000, nullptr) != 0;
    if (passRaised) __atomic_fetch_sub(&__PortGXCapPass, 1, __ATOMIC_RELAXED);
    __PortGXCap = 0;
    return ok;
}
}  // namespace
static const char* trapSelftestBody(uintptr_t hLo, uintptr_t hSpan);
const char* port_gx_trap_selftest(uintptr_t hLo, uintptr_t hSpan) {
    if (__PortGXCap != 0) return "the capture layer is not idle";
    const int oldTrap = __PortGXCapTrap;
    __PortGXCapTrap = 1;  // (the draw pipeline sets it for each overlap; the check does the same)
    const char* r = trapSelftestBody(hLo, hSpan);
    __PortGXCapTrap = oldTrap;
    return r;
}
static const char* trapSelftestBody(uintptr_t hLo, uintptr_t hSpan) {
    uint32_t f0, g0, s0, r0, f1, g1, s1, r1;
    port_gx_trap_stats(&f0, &g0, &s0, &r0);
    if (!trapProbeOnce(hLo, hSpan, 1)) return "another thread's GX calls did not return under the armed trap (pass word up)";
    if (!trapProbeOnce(hLo, hSpan, 0)) return "another thread's GX calls did not return under the armed trap (pass word down)";
    port_gx_trap_stats(&f1, &g1, &s1, &r1);
    if (f1 < f0 + 4) return "the layer did not see another thread's hooked calls (the hook does not send them to the trap)";
    if (__PortGXCapExSpan != 0 || sExLock.load() != 0) return "another thread's exempt window was left registered";
    // the calling thread under an armed trap with no draw in flight: the join has nothing to do, the trap must come down by itself
    if (!port_gx_replay_arm_trap(hLo, hSpan)) return "the trap cannot arm";
    u32 a, b, c, d;
    GXGetScissor(&a, &b, &c, &d);
    const bool down = __PortGXCap == 0;
    __PortGXCap = 0;
    port_gx_trap_stats(&f1, &g1, &s1, &r1);
    if (!down || s1 != s0 + 1 || r1 != r0) return "the calling thread's call left the trap armed (the stuck-trap guard did not act)";
    return nullptr;
}

void port_gx_replay(PortGxCapture* c, void (*token)(uint32_t id, void* ctx), void* ctx) {
    if (!c) return;
    if (__PortGXCap == 1) {
        ++sTot.contractErrors;
        logLine("gxcap: CONTRACT: port_gx_replay while a capture is open: the FIFO is the capture's, this picture is wrong");
    }
    const bool trap = __PortGXCapTrap != 0 && (__PortGXCap == 0 || __PortGXCap == 3);  // (3: armed by the game thread, below)
    if (trap) {
        // (every hooked call of another thread asks the draw pipeline while this replay runs: see gx_capture.h. This thread's
        // own calls, the replayed ones and the passes' at the tokens, go straight into the library: the pass word is held for
        // the whole replay, and the hooks tell this thread from the others by its stack. A host that cannot say where the
        // stack is: every caller counts as this thread, which is the pass word's meaning of before.)
        uintptr_t lo, hi;
        stackHere(&lo, &hi);
        __PortGXCapReplayTop = lo != hi ? hi - 1 : UINTPTR_MAX;
        __PortGXCapReplaySpan = lo != hi ? hi - lo : UINTPTR_MAX;
        __atomic_fetch_add(&__PortGXCapPass, 1, __ATOMIC_RELAXED);
        __PortGXCap = 3;
    }
    replayRecords(c, token ? token : handlerTrampoline, ctx, false);
    if (trap) {
        __PortGXCap = 0;
        __atomic_fetch_sub(&__PortGXCapPass, 1, __ATOMIC_RELAXED);
    }
}

void port_gx_capture_abandon(const char* why) {
    (void)why;
    if (sCap && ownerHere() && !sInCall) {
        if (sSelfCapture) {
            selfEndHere();
            return;
        }
        abandonNow(PORT_GXCAP_ID_outside);
    }
}

void port_gx_capture_stats(PortGxCaptureStats* out) {
    out->begun = sTot.begun;
    out->refused = sTot.refused;
    out->ended = sTot.ended;
    out->abandoned = sTot.abandoned;
    out->verified = sTot.verified;
    out->replayed = sReplayed.load(std::memory_order_relaxed);
    out->calls = sTot.calls;
    out->rawBytes = sTot.rawBytes;
    out->recordBytes = sTot.recordBytes;
    out->tokens = sTot.tokens;
    out->getters = sTot.getters;
    out->verifyDiffs = sTot.verifyDiffs;
    out->foreign = sTot.foreign;
    out->contractErrors = sTot.contractErrors;
    out->captureNs = sTot.captureNs;
    out->replayNs = sReplayNs.load(std::memory_order_relaxed);
}

// (tests) the first difference mode 2 found, and the abandons by entry point
const char* port_gxcap_first_diff(void) { return sFirstDiff; }
// (tests/gx_capture.cpp: the cheap path's thread test from where the caller stands: 1 the frame is in the recording thread's
// window, 0 it is not and the thread id decides, -1 no recording on the cheap path is open)
unsigned long long port_gxcap_test_fast_calls(void) { return sTot.fastCalls + sFastCalls; }  // (calls the cheap path recorded)
int port_gxcap_test_owner(void) { return sFast ? (ownerNear() ? 1 : 0) : -1; }
void port_gxcap_test_null(int on) { sNull = on && sMode == 1 ? 1 : 0; }  // (tests/gx_capture.cpp: the null recorder on and off in one run)
uint64_t port_gxcap_abandons_by(int id) { return id >= 0 && id < PORT_GXCAP_ID_END ? sTot.abandonBy[id] : 0; }
const char* port_gxcap_name(int id) { return id >= 0 && id < PORT_GXCAP_ID_END ? kNames[id] : "?"; }

}  // extern "C"
