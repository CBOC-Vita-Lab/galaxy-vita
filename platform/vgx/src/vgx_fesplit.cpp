// The front-end split: see vgx_fesplit.h.
//
// The player thread writes operations into a chain of fixed-size chunks; the finishing thread reads them in order.
// An operation is a header word (type | payload words << 8) and its payload. The player publishes how many operations
// it has written (one atomic store per draw); the finishing thread publishes how many it has done. Chunks are recycled
// through a small pool. All waits have time limits (Vita3K has lost condition-variable wake-ups before; see
// vgx_galaxy.cpp RenderMain), so a lost wake-up costs at most a few milliseconds.
//
// Ownership while a frame is being built:
//   player:    the register state (g), the resolved state, keys, shader registries, textures, the display-list cache,
//              the frame's command list (with placeholders), its vertex arena (growth only while the finishing thread is idle),
//              everything else in the CmdBuf;
//   finishing: its copy of what the blocks are built from, its draw records, index and uniform arenas, and the bytes of the
//              vertices it was told to transform (the player does not touch them again in this frame).
// At the frame's end the player waits until the finishing thread has done everything, then takes its arenas.
#include "vgx_fesplit.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>

extern "C" void port_log(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
extern "C" int64_t port_thread_cpu_us(void) __attribute__((weak));
extern "C" int64_t port_host_time_ns(void) __attribute__((weak));

namespace vgx {
namespace split {

bool g_on = false;

namespace {

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (port_log) port_log("%s", line);
    else std::fprintf(stderr, "%s\n", line);
}
int64_t NowNs() {
    if (port_host_time_ns) return port_host_time_ns();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

enum : uint32_t { OP_NEXT = 1, OP_SETFS, OP_SETVS, OP_PROJ, OP_MTX, OP_LIGHT, OP_TEV, OP_STATE, OP_JOB, OP_XFORM };
constexpr uint32_t kChunkWords = 1u << 14; // 64 KiB; the largest operation (a full matrix palette) is 626 words
struct Chunk {
    uint32_t w[kChunkWords];
    Chunk* next = nullptr;
};
static_assert(sizeof(FsKey) % 4 == 0 && sizeof(VsKey) % 4 == 0 && sizeof(DrawJob) % 4 == 0, "keys and jobs travel as words");
static_assert(sizeof(RenderState) == 44 && sizeof(TexBind) == 8, "render state / texture bindings travel as words");
constexpr uint32_t kJobWords = sizeof(DrawJob) / 4;

int g_mode = -1;  // GXV_SPLIT
int g_core = 1;   // GXV_SPLIT_CORE
void (*g_threadInit)(int) = nullptr;
bool g_started = false;

// (the objects the finishing thread waits on are never destroyed: it is detached and outlives static destruction at exit)
std::mutex& g_poolMtx = *new std::mutex;
std::vector<Chunk*>& g_pool = *new std::vector<Chunk*>;
Chunk* TakeChunk() {
    {
        std::lock_guard<std::mutex> lk(g_poolMtx);
        if (!g_pool.empty()) {
            Chunk* c = g_pool.back();
            g_pool.pop_back();
            c->next = nullptr;
            return c;
        }
    }
    return new Chunk();
}
void GiveChunk(Chunk* c) {
    std::lock_guard<std::mutex> lk(g_poolMtx);
    g_pool.push_back(c);
}

// ---- shared between the two threads
std::atomic<uint32_t> g_published{0}; // operations the player has made visible
std::atomic<uint32_t> g_done{0};      // operations the finishing thread has done
std::atomic<bool> g_bSleeping{false}, g_aWaiting{false};
std::mutex& g_mtx = *new std::mutex;
std::condition_variable& g_cvB = *new std::condition_variable;
std::condition_variable& g_cvA = *new std::condition_variable;
std::atomic<int64_t> g_bCpuUs{0};     // the finishing thread's CPU time (now and then)
Chunk* g_firstChunk = nullptr;
// Who runs the operations: the finishing thread, in batches, or the player at a frame's end when the finishing thread is not
// running (it takes over what is left instead of waiting: never slower than the single-thread path for that part). Whoever
// holds g_busy owns the reader and the finishing state (F).
std::atomic<bool> g_busy{false};
struct Reader {
    Chunk* ch = nullptr;
    uint32_t pos = 0;
    uint32_t done = 0;
} g_rd;
std::atomic<uint32_t> g_stolen{0}; // operations the player ran itself (all frames; the perf line)
uint32_t RunUpTo(uint32_t target, uint32_t max); // (g_busy held) runs operations until `target` (at most `max`); returns how many

// ---- player side
Chunk* g_wr = nullptr;
uint32_t g_wrPos = 0;
uint32_t g_produced = 0;
uint32_t g_jobsSinceWake = 0;
uint32_t g_jobs = 0; // this frame
int g_sentFs = -1, g_sentVs = -1;
struct SentState {
    bool valid = false;
    uint16_t vsId = 0, useFs = 0;
    uint8_t texMask = 0;
    uint32_t serial = 0;
} g_sent;
bool g_finished = false;
std::atomic<int64_t> g_waitNs{0};      // the player's waits for the finishing thread (all frames; read by the perf line)
std::atomic<uint32_t> g_waitFrames{0}; // frames finished split
uint64_t g_grows = 0;

// ---- finishing side
struct Finisher {
    State gb; // (only what the block builders read is kept up to date)
    FsKey fs;
    VsKey vs;
    uint16_t vsId = 0, useFs = 0;
    uint8_t texMask = 0;
    uint32_t serial = 0;
    RenderState rs;
    TexBind tex[8];
    UniArena uni;
    uint32_t blockOff[UB_COUNT] = {kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock};
    uint32_t blockLen[UB_COUNT] = {};
    std::vector<DrawRec> draws;
    std::vector<uint16_t, NoInitAlloc<uint16_t>> idx;
    int lastDraw = -1;
    uint32_t lastRecSerial = 0, records = 0;
    std::vector<uint32_t> results; // per job: the record it made, or ~0 (merged)
    uint32_t rowMismatches = 0;    // (GXV_SPLIT=2) the matrix rows the player computed differ from the builder's
    void ResetFrame() {
        uni.clear();
        draws.clear();
        idx.clear();
        results.clear();
        for (int b = 0; b < UB_COUNT; ++b) { blockOff[b] = kNoBlock; blockLen[b] = 0; }
        lastDraw = -1;
        records = 0;
    }
};
Finisher* F = nullptr;

// ---- GXV_SPLIT=2: the single-thread code for the moved work, on the player, into shadow buffers
struct Shadow {
    UniArena uni;
    uint32_t blockOff[UB_COUNT] = {kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock};
    uint32_t blockLen[UB_COUNT] = {};
    std::vector<DrawRec> draws;
    std::vector<uint16_t, NoInitAlloc<uint16_t>> idx;
    std::vector<Cmd> cmds;
    int lastDraw = -1;
    uint32_t lastRecSerial = 0, records = 0;
    std::vector<uint32_t> results;
    size_t scan = 0; // commands of the frame's list copied into `cmds` so far
    struct Xf { size_t off; uint32_t bytes; size_t at; };
    std::vector<Xf> xf;          // transformed vertex ranges, and where the expected bytes are in xfData
    std::vector<uint8_t> xfData;
    uint32_t rowMismatches = 0;
    // totals
    uint64_t frames = 0, badFrames = 0, jobs = 0, badRecords = 0, badCmds = 0, badIdx = 0, badUni = 0, badXf = 0, badRows = 0, badResults = 0;
    void ResetFrame() {
        uni.clear();
        draws.clear();
        idx.clear();
        cmds.clear();
        results.clear();
        xf.clear();
        xfData.clear();
        for (int b = 0; b < UB_COUNT; ++b) { blockOff[b] = kNoBlock; blockLen[b] = 0; }
        lastDraw = -1;
        records = 0;
        scan = 0;
        rowMismatches = 0;
    }
};
Shadow* V = nullptr;
inline bool Verify() { return g_mode == 2; }
BlockCtx ShadowBlocks(bool stale) { return BlockCtx{&V->uni, V->blockOff, V->blockLen, stale, true, CurrentFrame()}; }

// ---- writing operations (player)
inline uint32_t* Begin(uint32_t type, uint32_t words) {
    if (g_wrPos + 2 + words > kChunkWords) { // (room for an OP_NEXT is always kept)
        Chunk* nx = TakeChunk();
        g_wr->next = nx;
        g_wr->w[g_wrPos] = OP_NEXT;
        ++g_produced;
        g_wr = nx;
        g_wrPos = 0;
    }
    g_wr->w[g_wrPos] = type | (words << 8);
    return &g_wr->w[g_wrPos + 1];
}
inline void End(uint32_t words) {
    g_wrPos += 1 + words;
    ++g_produced;
}
inline void Publish() { g_published.store(g_produced, std::memory_order_release); }
void Wake(bool always) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (always || g_bSleeping.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_cvB.notify_one();
    }
}
// Returns when everything written so far has been done: by the finishing thread, or by the player itself when that thread
// is not running (the player takes the reader over as soon as the finishing thread lets go of it).
void Drain() {
    Publish();
    const uint32_t target = g_produced;
    if (g_done.load(std::memory_order_acquire) == target) return;
    const int64_t t0 = NowNs();
    int64_t lastReport = t0;
    for (unsigned spins = 0;; ++spins) {
        if (g_done.load(std::memory_order_acquire) == target) break;
        if (!g_busy.exchange(true, std::memory_order_acquire)) {
            g_stolen.fetch_add(RunUpTo(target, ~0u), std::memory_order_relaxed);
            g_busy.store(false, std::memory_order_release);
            break;
        }
        // (the finishing thread is in the middle of a batch: a few dozen operations)
        if (spins < 4096) continue;
        g_aWaiting.store(true, std::memory_order_seq_cst);
        if (g_busy.load(std::memory_order_seq_cst)) {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cvA.wait_for(lk, std::chrono::milliseconds(1), [] { return !g_busy.load(std::memory_order_acquire); });
        }
        g_aWaiting.store(false, std::memory_order_relaxed);
        const int64_t now = NowNs();
        if (now - lastReport > 2000000000ll) {
            lastReport = now;
            Log("fesplit: the player has waited %.0f ms for the finishing thread to let go of the reader (%u of %u operations done)", (now - t0) / 1e6,
                g_done.load(), target);
        }
    }
    g_waitNs.fetch_add(NowNs() - t0, std::memory_order_relaxed);
}

void SendFs(uint16_t fsId, const FsKey& fs) {
    if (g_sentFs == fsId) return;
    g_sentFs = fsId;
    constexpr uint32_t n = sizeof(FsKey) / 4;
    CopyWords(Begin(OP_SETFS, n), &fs, n);
    End(n);
}
void SendVs(uint16_t vsId, const VsKey& vs) {
    if (g_sentVs == vsId) return;
    g_sentVs = vsId;
    constexpr uint32_t n = sizeof(VsKey) / 4;
    CopyWords(Begin(OP_SETVS, n), &vs, n);
    End(n);
}
void SendState(const DrawStateRef& st) {
    if (g_sent.valid && g_sent.serial == st.serial && g_sent.vsId == st.vsId && g_sent.useFs == st.useFs && g_sent.texMask == st.texMask) return;
    g_sent.valid = true;
    g_sent.serial = st.serial;
    g_sent.vsId = st.vsId;
    g_sent.useFs = st.useFs;
    g_sent.texMask = st.texMask;
    constexpr uint32_t n = 3 + 11 + 16;
    uint32_t* p = Begin(OP_STATE, n);
    p[0] = st.vsId | (static_cast<uint32_t>(st.useFs) << 16);
    p[1] = st.texMask;
    p[2] = st.serial;
    CopyWords(p + 3, st.rs, 11);
    CopyWords(p + 14, st.tex, 16);
    End(n);
}

// ---- the finishing thread
inline BlockCtx FinBlocks(bool stale) { return BlockCtx{&F->uni, F->blockOff, F->blockLen, stale, false, 0}; }
void Run(uint32_t type, const uint32_t* p) {
    State& s = F->gb;
    switch (type) {
    case OP_SETFS: CopyWords(&F->fs, p, sizeof(FsKey) / 4); break;
    case OP_SETVS: CopyWords(&F->vs, p, sizeof(VsKey) / 4); break;
    case OP_PROJ: {
        CopyWords(&s.xfReg[0x20], p + 1, 7);
        BlockCtx c = FinBlocks(p[0] != 0);
        BuildProjC(c, s);
        break;
    }
    case OP_MTX: {
        const bool soft = (p[0] & 1) != 0, stale = (p[0] & 2) != 0;
        s.matIdxA = p[1];
        s.matIdxB = p[2];
        CopyWords(&s.xfReg[0x50], p + 3, 8);
        uint64_t pos = p[11] | (static_cast<uint64_t>(p[12]) << 32);
        uint32_t nrm = p[13];
        uint64_t post = p[14] | (static_cast<uint64_t>(p[15]) << 32);
        const uint64_t used = p[16] | (static_cast<uint64_t>(p[17]) << 32);
        const uint32_t* q = p + 18;
        for (; pos; pos &= pos - 1, q += 4) CopyWords(s.xfPos[__builtin_ctzll(pos)], q, 4);
        for (; nrm; nrm &= nrm - 1, q += 3) CopyWords(s.xfNrm[__builtin_ctz(nrm)], q, 3);
        for (; post; post &= post - 1, q += 4) CopyWords(s.xfPost[__builtin_ctzll(post)], q, 4);
        BlockCtx c = FinBlocks(stale);
        const uint64_t rows = BuildMtxC(c, s, F->vs, soft);
        if (rows != used) ++F->rowMismatches;
        break;
    }
    case OP_LIGHT: {
        CopyWords(&s.xfReg[0x0A], p + 1, 4);
        const uint32_t n = p[5];
        for (uint32_t i = 0; i < n; ++i) CopyWords(s.xfLight[i], p + 6 + i * 16, 16);
        BlockCtx c = FinBlocks(p[0] != 0);
        BuildLightC(c, s, F->vs);
        break;
    }
    case OP_TEV: {
        const bool fold = (p[0] & 1) != 0, stale = (p[0] & 2) != 0;
        uint16_t texW[8], texH[8];
        for (int m = 0; m < 8; ++m) {
            texW[m] = static_cast<uint16_t>(p[1 + m]);
            texH[m] = static_cast<uint16_t>(p[1 + m] >> 16);
        }
        CopyWords(s.tevReg, p + 9, 16);
        CopyWords(s.tevKonst, p + 25, 16);
        CopyWords(&s.bp[0x06], p + 41, 9);
        s.bp[0x25] = p[50];
        s.bp[0x26] = p[51];
        s.bp[0x42] = p[52];
        CopyWords(&s.bp[0xEE], p + 53, 7);
        BlockCtx c = FinBlocks(stale);
        BuildTevC(c, s, F->fs, texW, texH, fold);
        break;
    }
    case OP_STATE:
        F->vsId = static_cast<uint16_t>(p[0]);
        F->useFs = static_cast<uint16_t>(p[0] >> 16);
        F->texMask = static_cast<uint8_t>(p[1]);
        F->serial = p[2];
        CopyWords(&F->rs, p + 3, 11);
        CopyWords(F->tex, p + 14, 16);
        break;
    case OP_JOB: {
        DrawJob j;
        CopyWords(&j, p, kJobWords);
        FinishCtx c{&F->draws, &F->idx, nullptr, F->blockOff, &F->lastDraw, &F->lastRecSerial, &F->records, false};
        const DrawStateRef st{F->vsId, F->useFs, F->texMask, F->serial, &F->rs, F->tex};
        F->results.push_back(j.kind == JOB_STATIC ? FinishStatic(c, j, st) : FinishDraw(c, j, st));
        break;
    }
    case OP_XFORM: {
        uint64_t a = p[0] | (static_cast<uint64_t>(p[1]) << 32);
        uint8_t* v = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(a));
        SoftTransformWith(v, p[2], MakeLayout(p[3]), reinterpret_cast<const float*>(p + 4), reinterpret_cast<const float*>(p + 16));
        break;
    }
    default:
        Log("fesplit: unknown operation %u", type);
        break;
    }
}

uint32_t RunUpTo(uint32_t target, uint32_t max) {
    // (`target` was published when it was read; the reader may be past it if the other thread ran in between)
    uint32_t left = target - g_rd.done;
    if (left > 0x80000000u) left = 0;
    if (left > max) left = max;
    uint32_t n = 0;
    for (; n < left; ++n) {
        const uint32_t h = g_rd.ch->w[g_rd.pos];
        const uint32_t type = h & 0xFFu;
        if (type == OP_NEXT) {
            Chunk* nx = g_rd.ch->next;
            GiveChunk(g_rd.ch);
            g_rd.ch = nx;
            g_rd.pos = 0;
        } else {
            Run(type, &g_rd.ch->w[g_rd.pos + 1]);
            g_rd.pos += 1 + (h >> 8);
        }
        ++g_rd.done;
    }
    g_done.store(g_rd.done, std::memory_order_release);
    return n;
}

// GXV_SPLIT_JITTER=1 (diagnostic, for tests of the hand-over): the finishing thread sleeps a random 0-200 us at its decision
// points, so that the player overtakes it at every possible moment.
void Jitter() {
    static const bool on = diag::Int("GXV_SPLIT_JITTER", 0) != 0;
    if (!on) return;
    static uint32_t r = 12345;
    r = r * 1103515245u + 12345u;
    if ((r >> 16) & 1) std::this_thread::sleep_for(std::chrono::microseconds((r >> 8) % 200));
}

void ThreadMain() {
    if (g_threadInit) g_threadInit(g_core);
    int64_t lastCpu = 0;
    for (;;) {
        uint32_t pub = g_published.load(std::memory_order_acquire);
        uint32_t done = g_done.load(std::memory_order_acquire);
        if (pub == done) {
            for (int i = 0; i < 64 && pub == done; ++i) pub = g_published.load(std::memory_order_acquire);
            if (pub == done) {
                if (port_thread_cpu_us) {
                    const int64_t now = NowNs();
                    if (now - lastCpu > 50000000) { // (every 50 ms at most: it is a system call)
                        lastCpu = now;
                        g_bCpuUs.store(port_thread_cpu_us(), std::memory_order_relaxed);
                    }
                }
                g_bSleeping.store(true, std::memory_order_seq_cst);
                if (g_published.load(std::memory_order_seq_cst) == done) {
                    std::unique_lock<std::mutex> lk(g_mtx);
                    g_cvB.wait_for(lk, std::chrono::milliseconds(2), [done] { return g_published.load(std::memory_order_acquire) != done; });
                }
                g_bSleeping.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        Jitter();
        if (g_busy.exchange(true, std::memory_order_acquire)) {
            // (the player is doing the rest itself: it is quick)
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cvB.wait_for(lk, std::chrono::milliseconds(1), [] { return !g_busy.load(std::memory_order_acquire); });
            continue;
        }
        // (in batches, so that the player can take over at the end of its frame without waiting long; what is published is read
        // again now that the reader is ours: the player may have run some of it meanwhile)
        RunUpTo(g_published.load(std::memory_order_acquire), 32);
        Jitter();
        g_busy.store(false, std::memory_order_release);
        if (g_aWaiting.load(std::memory_order_seq_cst)) {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_cvA.notify_all();
        }
    }
}

bool DiagnosticsInTheWay() {
    static const bool fixed = diag::Int("GXV_TRACE", -1) >= 0 || diag::Int("GXV_TRACE_DRAWS", 0) > 0 || diag::Int("GXV_MERGEFAST", 1) == 2 ||
                              diag::Int("GXV_SOFTNEON", 1) == 2 || diag::Int("GXV_RESOLVEFAST", 1) == 2;
    return fixed || stats::Detailed();
}

void Start() {
    F = new Finisher();
    if (Verify()) V = new Shadow();
    g_firstChunk = g_wr = TakeChunk();
    g_wrPos = 0;
    g_rd.ch = g_firstChunk;
    // (256 KiB of stack: the block builders keep a few KiB of floats on it; the system default is smaller on some hosts)
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u << 10);
    pthread_t th;
    if (pthread_create(&th, &attr, [](void*) -> void* { ThreadMain(); return nullptr; }, nullptr) != 0) {
        pthread_attr_destroy(&attr);
        Log("fesplit: the finishing thread could not be started: the split stays off");
        g_mode = 0;
        return;
    }
    pthread_attr_destroy(&attr);
    pthread_detach(th);
    g_started = true;
    Log("fesplit: front-end split on (GXV_SPLIT=%d): uniform blocks, draw records and CPU transforms are finished on a thread of their own "
        "(core %d)%s", g_mode, g_core, Verify() ? "; every frame is also finished the single-thread way and compared" : "");
}

bool SameRec(const DrawRec& a, const DrawRec& b) {
    return a.vsId == b.vsId && a.fsId == b.fsId && a.prim == b.prim && a.texMask == b.texMask && a.cover == b.cover && a.exX == b.exX &&
           a.exY == b.exY && a.exW == b.exW && a.exH == b.exH && a.layoutMask == b.layoutMask && a.vbuf == b.vbuf && a.vtxOffset == b.vtxOffset &&
           a.vtxCount == b.vtxCount && a.idxOffset == b.idxOffset && a.idxCount == b.idxCount && a.vtxFirst == b.vtxFirst && a.vchunk == b.vchunk &&
           SameWords(a.ublock, b.ublock, UB_COUNT) && SameStruct(a.rs, b.rs) && SameWords(a.tex, b.tex, 16);
}

// GXV_SPLIT=2: the frame's commands since the last call go into the shadow list as the single-thread path had them.
void ShadowSync(const std::vector<Cmd>& cmds) {
    for (size_t i = V->scan; i < cmds.size(); ++i) {
        const Cmd c = cmds[i];
        if (c.type == CMD_DRAW) continue; // (a placeholder: the shadow has its own records)
        V->cmds.push_back(c);
        // (what QueueCopyTex, QueueCopyDisp, QueueSetTarget do to the merge candidate)
        if (c.type == CMD_COPY_TEX || c.type == CMD_COPY_DISP || c.type == CMD_SET_TARGET) V->lastDraw = -1;
    }
    V->scan = cmds.size();
}

void Compare(CmdBuf& buf) {
    Shadow& v = *V;
    ++v.frames;
    v.jobs += F->results.size();
    unsigned bad = 0;
    char what[256] = "";
    if (F->results.size() != v.results.size()) {
        ++v.badResults;
        if (bad++ == 0) std::snprintf(what, sizeof what, "%" VGX_PRIZU " jobs finished, %" VGX_PRIZU " in the shadow", F->results.size(), v.results.size());
    } else {
        for (size_t i = 0; i < v.results.size(); ++i)
            if (F->results[i] != v.results[i]) {
                ++v.badResults;
                if (bad++ == 0) std::snprintf(what, sizeof what, "job %" VGX_PRIZU ": record %d, shadow %d", i, (int)F->results[i], (int)v.results[i]);
                break;
            }
    }
    if (buf.cmds.size() != v.cmds.size() || !std::equal(buf.cmds.begin(), buf.cmds.end(), v.cmds.begin(), [](const Cmd& a, const Cmd& b) { return a.type == b.type && a.arg == b.arg; })) {
        ++v.badCmds;
        if (bad++ == 0) std::snprintf(what, sizeof what, "command lists differ (%" VGX_PRIZU " / %" VGX_PRIZU " commands)", buf.cmds.size(), v.cmds.size());
    }
    if (F->draws.size() != v.draws.size()) {
        ++v.badRecords;
        if (bad++ == 0) std::snprintf(what, sizeof what, "%" VGX_PRIZU " records, shadow %" VGX_PRIZU "", F->draws.size(), v.draws.size());
    } else {
        for (size_t i = 0; i < v.draws.size(); ++i)
            if (!SameRec(F->draws[i], v.draws[i])) {
                ++v.badRecords;
                if (bad++ == 0)
                    std::snprintf(what, sizeof what, "record %" VGX_PRIZU " differs (vs %u/%u fs %u/%u vtx %u/%u idx %u/%u ub0 %u/%u)", i, F->draws[i].vsId, v.draws[i].vsId,
                                  F->draws[i].fsId, v.draws[i].fsId, F->draws[i].vtxCount, v.draws[i].vtxCount, F->draws[i].idxCount, v.draws[i].idxCount,
                                  F->draws[i].ublock[0], v.draws[i].ublock[0]);
                break;
            }
    }
    if (F->idx.size() != v.idx.size() || (!v.idx.empty() && std::memcmp(F->idx.data(), v.idx.data(), v.idx.size() * 2) != 0)) {
        ++v.badIdx;
        if (bad++ == 0) std::snprintf(what, sizeof what, "index arenas differ (%" VGX_PRIZU " / %" VGX_PRIZU ")", F->idx.size(), v.idx.size());
    }
    if (F->uni.size() != v.uni.size() || (!v.uni.empty() && !SameWords(F->uni.data(), v.uni.data(), static_cast<uint32_t>(v.uni.size())))) {
        ++v.badUni;
        size_t at = 0;
        while (at < F->uni.size() && at < v.uni.size() && std::memcmp(&F->uni[at], &v.uni[at], 4) == 0) ++at;
        if (bad++ == 0) std::snprintf(what, sizeof what, "uniform arenas differ (%" VGX_PRIZU " / %" VGX_PRIZU " floats, first at %" VGX_PRIZU ")", F->uni.size(), v.uni.size(), at);
    }
    for (const Shadow::Xf& x : v.xf) {
        if (x.off + x.bytes > buf.vtx.size() || std::memcmp(buf.vtx.data() + x.off, v.xfData.data() + x.at, x.bytes) != 0) {
            ++v.badXf;
            if (bad++ == 0) std::snprintf(what, sizeof what, "transformed vertices at %" VGX_PRIZU " (%u bytes) differ", x.off, x.bytes);
            break;
        }
    }
    if (F->rowMismatches || v.rowMismatches) {
        ++v.badRows;
        if (bad++ == 0) std::snprintf(what, sizeof what, "matrix rows: %u (finishing) / %u (shadow) blocks used other rows than sent", F->rowMismatches, v.rowMismatches);
    }
    F->rowMismatches = 0;
    if (bad) {
        ++v.badFrames;
        if (v.badFrames <= 40) Log("fesplit: MISMATCH frame %llu: %s (%u differences)", static_cast<unsigned long long>(CurrentFrame()), what, bad);
    }
    if (v.frames == 1 || v.frames % 600 == 0)
        Log("fesplit: verify: %llu frames, %llu draws compared; %llu frames with mismatches (job results %llu, commands %llu, records %llu, indices %llu, "
            "uniforms %llu, transforms %llu, matrix rows %llu)",
            static_cast<unsigned long long>(v.frames), static_cast<unsigned long long>(v.jobs), static_cast<unsigned long long>(v.badFrames),
            static_cast<unsigned long long>(v.badResults), static_cast<unsigned long long>(v.badCmds), static_cast<unsigned long long>(v.badRecords),
            static_cast<unsigned long long>(v.badIdx), static_cast<unsigned long long>(v.badUni), static_cast<unsigned long long>(v.badXf),
            static_cast<unsigned long long>(v.badRows));
}

} // namespace

void SetThreadInit(void (*fn)(int core)) { g_threadInit = fn; }

uint32_t StolenOps() { return g_stolen.load(std::memory_order_relaxed); }
void Counters(int64_t& finisherCpuUs, int64_t& playerWaitNs, uint32_t& frames) {
    finisherCpuUs = g_bCpuUs.load(std::memory_order_relaxed);
    playerWaitNs = g_waitNs.load(std::memory_order_relaxed);
    frames = g_waitFrames.load(std::memory_order_relaxed);
}

void FrameBegin() {
    if (g_mode < 0) {
        // GXV_SPLIT: see vgx_fesplit.h. (default: on for the PS Vita; off elsewhere, the ARM harness included: there
        // EXTRA_ENV=GXV_SPLIT=1 shows the two threads' instructions apart)
#if defined(__VGX_VITA__) || defined(__vita__)
        const long dflt = 1;
#else
        const long dflt = 0;
#endif
        g_mode = static_cast<int>(EnvInt("GXV_SPLIT", dflt));
        if (g_mode < 0 || g_mode > 2) g_mode = 0;
        g_core = static_cast<int>(EnvInt("GXV_SPLIT_CORE", 1));
    }
    g_finished = false;
    g_jobs = 0;
    g_sentFs = g_sentVs = -1;
    g_sent.valid = false;
    bool on = g_mode >= 1 && !DiagnosticsInTheWay();
    if (on && !g_started) Start();
    on = on && g_started;
    if (on && V) V->ResetFrame();
    g_on = on;
}

void Proj(bool stale) {
    uint32_t* p = Begin(OP_PROJ, 8);
    p[0] = stale ? 1u : 0u;
    CopyWords(p + 1, &g.xfReg[0x20], 7);
    End(8);
    if (Verify()) {
        BlockCtx c = ShadowBlocks(stale);
        BuildProjC(c, g);
    }
}

uint64_t Mtx(uint16_t vsId, const VsKey& vs, bool soft, bool stale) {
    SendVs(vsId, vs);
    uint64_t pos, post, used;
    uint32_t nrm;
    MtxReads(g, vs, soft, pos, nrm, post, used);
    const uint32_t n = 18 + 4 * static_cast<uint32_t>(__builtin_popcountll(pos)) + 3 * static_cast<uint32_t>(__builtin_popcount(nrm)) +
                       4 * static_cast<uint32_t>(__builtin_popcountll(post));
    uint32_t* p = Begin(OP_MTX, n);
    p[0] = (soft ? 1u : 0u) | (stale ? 2u : 0u);
    p[1] = g.matIdxA;
    p[2] = g.matIdxB;
    CopyWords(p + 3, &g.xfReg[0x50], 8);
    p[11] = static_cast<uint32_t>(pos);
    p[12] = static_cast<uint32_t>(pos >> 32);
    p[13] = nrm;
    p[14] = static_cast<uint32_t>(post);
    p[15] = static_cast<uint32_t>(post >> 32);
    p[16] = static_cast<uint32_t>(used);
    p[17] = static_cast<uint32_t>(used >> 32);
    uint32_t* q = p + 18;
    for (uint64_t m = pos; m; m &= m - 1, q += 4) CopyWords(q, g.xfPos[__builtin_ctzll(m)], 4);
    for (uint32_t m = nrm; m; m &= m - 1, q += 3) CopyWords(q, g.xfNrm[__builtin_ctz(m)], 3);
    for (uint64_t m = post; m; m &= m - 1, q += 4) CopyWords(q, g.xfPost[__builtin_ctzll(m)], 4);
    End(n);
    if (Verify()) {
        BlockCtx c = ShadowBlocks(stale);
        if (BuildMtxC(c, g, vs, soft) != used) ++V->rowMismatches;
    }
    return used;
}

void Light(uint16_t vsId, const VsKey& vs, bool stale) {
    SendVs(vsId, vs);
    const int nvec = UniformPacking() ? VsLightCount(vs) : 44;
    const uint32_t nl = static_cast<uint32_t>((nvec - 4) / 5);
    const uint32_t n = 6 + 16 * nl;
    uint32_t* p = Begin(OP_LIGHT, n);
    p[0] = stale ? 1u : 0u;
    CopyWords(p + 1, &g.xfReg[0x0A], 4);
    p[5] = nl;
    for (uint32_t i = 0; i < nl; ++i) CopyWords(p + 6 + i * 16, g.xfLight[i], 16);
    End(n);
    if (Verify()) {
        BlockCtx c = ShadowBlocks(stale);
        BuildLightC(c, g, vs);
    }
}

void Tev(uint16_t fsId, const FsKey& fs, const uint16_t texW[8], const uint16_t texH[8], bool fold, bool stale) {
    SendFs(fsId, fs);
    constexpr uint32_t n = 60;
    uint32_t* p = Begin(OP_TEV, n);
    p[0] = (fold ? 1u : 0u) | (stale ? 2u : 0u);
    for (int m = 0; m < 8; ++m) p[1 + m] = texW[m] | (static_cast<uint32_t>(texH[m]) << 16);
    CopyWords(p + 9, g.tevReg, 16);
    CopyWords(p + 25, g.tevKonst, 16);
    CopyWords(p + 41, &g.bp[0x06], 9);
    p[50] = g.bp[0x25];
    p[51] = g.bp[0x26];
    p[52] = g.bp[0x42];
    CopyWords(p + 53, &g.bp[0xEE], 7);
    End(n);
    if (Verify()) {
        BlockCtx c = ShadowBlocks(stale);
        BuildTevC(c, g, fs, texW, texH, fold);
    }
}

void Xform(size_t offset, uint32_t count, uint32_t layoutMask) {
    CmdBuf& buf = Pending();
    uint8_t* const v = buf.vtx.data() + offset;
    const uint32_t pi = g.matIdxA & 0x3F;
    const float* m = g.xfPos[pi < 62 ? pi : 61];
    const float* nm = g.xfNrm[(pi & 31) < 30 ? (pi & 31) : 29];
    const bool nrm = (layoutMask & VL_NRM) != 0;
    if (Verify()) {
        // (the expected bytes: the same transform, here, on a copy)
        const VtxLayout L = MakeLayout(layoutMask);
        const uint32_t bytes = count * L.stride;
        const size_t at = V->xfData.size();
        V->xfData.insert(V->xfData.end(), v, v + bytes);
        SoftTransformWith(V->xfData.data() + at, count, L, m, nm);
        V->xf.push_back(Shadow::Xf{offset, bytes, at});
    }
    const uint32_t n = 16 + (nrm ? 9u : 0u);
    uint32_t* p = Begin(OP_XFORM, n);
    const uint64_t a = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(v));
    p[0] = static_cast<uint32_t>(a);
    p[1] = static_cast<uint32_t>(a >> 32);
    p[2] = count;
    p[3] = layoutMask;
    CopyWords(p + 4, m, 12);
    if (nrm) CopyWords(p + 16, nm, 9);
    End(n);
}

uint32_t Job(const DrawJob& j, const DrawStateRef& st) {
    if (Verify()) {
        ShadowSync(Pending().cmds);
        V->scan = Pending().cmds.size() + 1; // (the placeholder the caller pushes next)
        FinishCtx c{&V->draws, &V->idx, &V->cmds, V->blockOff, &V->lastDraw, &V->lastRecSerial, &V->records, false};
        V->results.push_back(j.kind == JOB_STATIC ? FinishStatic(c, j, st) : FinishDraw(c, j, st));
    }
    SendState(st);
    CopyWords(Begin(OP_JOB, kJobWords), &j, kJobWords);
    End(kJobWords);
    Publish();
    if (++g_jobsSinceWake >= 16) {
        g_jobsSinceWake = 0;
        Wake(false);
    }
    return g_jobs++;
}

void BeforeArenaGrow(const std::vector<uint8_t, NoInitAlloc<uint8_t>>&, size_t) {
    ++g_grows;
    Drain();
}

void FinishFrame(CmdBuf& buf) {
    if (!g_on || g_finished) return;
    g_finished = true;
    Drain();
    g_waitFrames.fetch_add(1, std::memory_order_relaxed);
    // (everything is done; the reader is taken so that the finishing thread cannot touch its state while it is handed over)
    for (unsigned spins = 0; g_busy.exchange(true, std::memory_order_acquire); ++spins)
        if (spins > 4096) std::this_thread::yield(); // (it is between two batches of nothing: it lets go at once)
    if (V) ShadowSync(buf.cmds);
    // The placeholders become the records the finishing thread made (or go: merged).
    std::vector<Cmd>& cmds = buf.cmds;
    size_t o = 0;
    for (size_t i = 0; i < cmds.size(); ++i) {
        Cmd c = cmds[i];
        if (c.type == CMD_DRAW) {
            if (c.arg >= F->results.size()) {
                Log("fesplit: placeholder %u of %" VGX_PRIZU " jobs (frame %llu)", c.arg, F->results.size(), static_cast<unsigned long long>(CurrentFrame()));
                continue;
            }
            const uint32_t r = F->results[c.arg];
            if (r == ~0u) continue;
            c.arg = r;
        }
        cmds[o++] = c;
    }
    cmds.resize(o);
    if (V) Compare(buf);
    std::swap(buf.draws, F->draws);
    std::swap(buf.idx, F->idx);
    std::swap(buf.uni, F->uni);
    stats::Cur().drawCalls += F->records;
    F->ResetFrame();
    g_busy.store(false, std::memory_order_release);
}

} // namespace split
} // namespace vgx
