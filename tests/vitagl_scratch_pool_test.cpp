// Host test of vitaGL's per-picture data pool as the Galaxy back end uses it when the library is built with HAVE_SCRATCH_MEMORY
// (tools/vitagl/build-release.sh) and the back end asks for scratch stream buffers (GXV_VITA_SCRATCH=1: vglSetupScratchMemory(GL_FALSE,
// GL_TRUE)). The actual driver text is used (tools/prepare_vitagl_scratch_fixture.cjs): the pool arrays and set-up, vgl_reserve_data_pool
// (with ALIGNED_POOL), glNamedBufferData, glNamedBufferSubData, glDeleteBuffers, vglSetupScratchMemory, and the pool rotation of
// vglSwapBuffers. Host stubs only: the GPU, the display queue and the garbage collector are models.
//
// The rule under test: a stream buffer lives in the pool of the picture being built (pool index = swap count mod display buffers); that
// pool is written again only after `buffers` swaps, and by then the display queue has released the picture that used it (it holds at
// most buffers-1 pictures, so once swap F has returned, picture F-(buffers-1) is complete). Back-end frames that are not presented
// (vglSubmitWithoutSwap, which ends in sceGxmFinish) do not rotate the pool and are complete when they end.
// What cannot be shown on a PC: that the real display queue keeps that promise (displayQueueMaxPendingCount = buffers-1).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <vector>

typedef int GLboolean;
typedef int GLsizei;
typedef unsigned GLenum;
typedef long GLintptr;
typedef long GLsizeiptr;
typedef uintptr_t GLuint; // (the console's handles are 32-bit pointers; here they are full pointers)
constexpr int GL_TRUE = 1, GL_FALSE = 0;
constexpr GLenum GL_STREAM_DRAW = 0x88E0, GL_STREAM_READ = 0x88E1, GL_STREAM_COPY = 0x88E2, GL_STATIC_DRAW = 0x88E4, GL_DYNAMIC_DRAW = 0x88E8,
                 GL_DYNAMIC_READ = 0x88E9, GL_DYNAMIC_COPY = 0x88EA, GL_INVALID_VALUE = 0x501, GL_INVALID_OPERATION = 0x502, GL_OUT_OF_MEMORY = 0x505;
#define THREAD_SAFE()
#define LOG_ERRORS 1 // (the nhustdx set builds with it)
#define DISPLAY_MAX_BUFFER_COUNT 4
#define VGL_MEM_VRAM 1
static int g_glErrors = 0, g_logs = 0;
#define SET_GL_ERROR(x) { ++g_glErrors; return; }
#define SET_GL_ERROR_WITH_VALUE(x, v) { ++g_glErrors; return; }
#define vgl_log(...) (++g_logs)
#define vgl_fast_memcpy memcpy
#define vgl_memcpy memcpy
#define vgl_mem_get_type_by_addr(p) 0

static uint8_t gxm_display_buffer_count = 3;
static uint32_t vgl_framecount = 0;

// ---- the heap (vitaGL's mempool) and the garbage collector, as models
struct Block { size_t size; bool pool; };
static std::map<uint8_t*, Block> g_heap;      // every block the driver holds: pool memory and heap blocks
static std::vector<void*> g_purge[4];          // the collector's lists (FRAME_PURGE_FREQ buckets)
static int frame_purge_idx = 0, g_cleanIdx = 1;
static unsigned g_heapAllocs = 0, g_heapFrees = 0, g_dirtyMarks = 0;
#define mark_as_dirty(x) (g_purge[frame_purge_idx].push_back((void*)(x)), ++g_dirtyMarks)
static bool g_poolInit = false;

struct Live { uint8_t* p; size_t n; uint32_t id; int picture; bool freed; };
static std::vector<Live> g_live;               // memory the GPU may still read
static unsigned g_violations = 0;
static void Violation(const char* what, bool expected) {
    ++g_violations;
    if (!expected) std::fprintf(stderr, "VIOLATION: %s\n", what);
}
static bool g_expectViolations = false;

static void* gpu_alloc_mapped_for_cpu(size_t size) {
    const size_t rounded = (size + 15u) & ~size_t(15u);
    uint8_t* p = static_cast<uint8_t*>(std::aligned_alloc(16, rounded ? rounded : 16));
    if (!p) std::abort();
    g_heap[p] = {size, !g_poolInit};
    if (g_poolInit) ++g_heapAllocs;
    return p;
}
static void* gpu_alloc_mapped_for_gpu(size_t size) { return gpu_alloc_mapped_for_cpu(size); }
static bool Overlap(const uint8_t* a, size_t an, const uint8_t* b, size_t bn) { return a < b + bn && b < a + an; }
static void vgl_free(void* ptr) {
    if (!ptr) return;
    auto it = g_heap.find(static_cast<uint8_t*>(ptr));
    if (it == g_heap.end() || it->second.pool) { Violation("vgl_free of memory that is not a heap block (pool memory or double free)", false); return; }
    for (Live& l : g_live)
        if (!l.freed && Overlap(l.p, l.n, it->first, it->second.size)) { l.freed = true; Violation("heap block freed while the GPU may still read it", g_expectViolations); }
    ++g_heapFrees;
    g_heap.erase(it);
    std::free(ptr);
}

#include "vitagl_scratch_pool_functions.inc"

// ---- one back-end frame's work, as the Galaxy back end issues it
struct Pool { uint8_t* base; size_t size; };
static Pool PoolOf(int i) { return {circular_data_pool[i], size_t(circular_data_pool_limit[i] - circular_data_pool[i])}; }
static bool InPool(const uint8_t* p, size_t n) {
    for (int i = 0; i < gxm_display_buffer_count; ++i) {
        const Pool q = PoolOf(i);
        if (p >= q.base && p + n <= q.base + q.size) return true;
    }
    return false;
}
static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static void Fill(uint8_t* p, size_t n, uint32_t id) {
    for (size_t i = 0; i < n; ++i) p[i] = uint8_t(id >> ((i & 3) * 8));
}
static bool Intact(const Live& l) {
    for (size_t i = 0; i < l.n; ++i) if (l.p[i] != uint8_t(l.id >> ((i & 3) * 8))) return false;
    return true;
}
static void CompleteUpTo(int picture) {
    for (size_t i = 0; i < g_live.size();) {
        Live& l = g_live[i];
        if (l.picture <= picture) {
            if (!l.freed && !Intact(l)) Violation("data overwritten before the GPU finished with it", g_expectViolations);
            g_live.erase(g_live.begin() + i);
        } else {
            ++i;
        }
    }
}
static vbo* NewBuffer() {
    vbo* b = static_cast<vbo*>(std::malloc(sizeof(vbo)));
    g_heap[reinterpret_cast<uint8_t*>(b)] = {sizeof(vbo), false}; // (the driver frees it with vgl_free in glDeleteBuffers)
    std::memset(b, 0xA5, sizeof(vbo)); // glGenBuffers does not clear the fields it does not set (scratch, size, alloc_func, mapped)
    b->ptr = nullptr;
    b->last_frame = OBJ_NOT_USED;
    return b;
}
static void Upload(vbo* b, int size, GLenum usage, int picture, uint32_t& nextId, bool gpuReads) {
    const GLuint handle = reinterpret_cast<GLuint>(b);
    std::vector<uint8_t> data(size);
    const uint32_t id = ++nextId;
    Fill(data.data(), data.size(), id);
    glNamedBufferData(handle, size, data.data(), usage);
    Require(b->ptr != nullptr, "buffer has memory");
    Require((reinterpret_cast<uintptr_t>(b->ptr) & 15u) == 0, "buffer data starts on 16 bytes");
    if (gpuReads) {
        for (const Live& l : g_live)
            if (!l.freed && Overlap(l.p, l.n, static_cast<uint8_t*>(b->ptr), size)) Violation("new buffer data overlaps memory the GPU may still read", g_expectViolations);
        g_live.push_back({static_cast<uint8_t*>(b->ptr), size_t(size), id, picture, false});
    }
}

struct Result { unsigned heapAllocs, heapFrees, dirtyMarks, pictures, backendFrames, streamInPool, streamOnHeap; };

// Pictures of `hiddenMax` or fewer unpresented frames and one presented frame each. `lag(swap)` gives the newest picture the display
// queue has released after that swap returned (the real promise is picture - (buffers - 1)).
static Result Run(unsigned seed, bool scratchStream, bool scratchDynamic, int pictures, int guaranteeLag, bool checkRotation, uint32_t poolBytes = 3 * 2048) {
    std::mt19937 rng(seed);
    g_violations = 0;
    gxm_display_buffer_count = 3;
    g_poolInit = false;
    for (auto& l : g_purge) l.clear();
    frame_purge_idx = 0;
    g_cleanIdx = 1;
    vgl_framecount = 0;
    vgl_circular_idx = 0;
    circular_data_pool_size = poolBytes; // the default is small pools (2 KiB each): a busy picture overflows into the heap now and then
    fixture_pool_init();
    g_poolInit = true;
    g_heapAllocs = g_heapFrees = g_dirtyMarks = 0;
    g_live.clear();
    vglSetupScratchMemory(scratchDynamic ? GL_TRUE : GL_FALSE, scratchStream ? GL_TRUE : GL_FALSE);

    vbo* stream[3] = {NewBuffer(), NewBuffer(), NewBuffer()}; // the frame's vertex chunks / indices, never deleted
    vbo* statics[4];
    for (vbo*& s : statics) s = NewBuffer();
    uint32_t nextId = 0;
    Result r{};
    int completed = -1;
    for (int picture = 0; picture < pictures; ++picture) {
        Require(static_cast<int>(vgl_framecount) == picture, "the swap count is the picture number");
        if (checkRotation) Require(vgl_circular_idx == picture % gxm_display_buffer_count, "pool index follows the swap count");
        const int hidden = int(rng() % 4 == 0) * int(1 + rng() % 3);
        for (int frame = 0; frame <= hidden; ++frame) {
            const bool visible = frame == hidden;
            ++r.backendFrames;
            // UploadStream: the frame's vertex chunk(s) and its indices, GL_STREAM_DRAW, the whole content again every frame
            const int chunks = 1 + int(rng() % 2 == 0);
            for (int c = 0; c < chunks; ++c) {
                Upload(stream[c], 8 + int(rng() % 1500), GL_STREAM_DRAW, picture, nextId, true);
                stream[c]->last_frame = vgl_framecount; // (the draws record the frame in the buffer they read)
                const bool pooled = InPool(static_cast<uint8_t*>(stream[c]->ptr), stream[c]->size);
                r.streamInPool += pooled;
                r.streamOnHeap += !pooled;
                if (pooled && checkRotation) {
                    const Pool q = PoolOf(vgl_circular_idx);
                    Require(static_cast<uint8_t*>(stream[c]->ptr) >= q.base && static_cast<uint8_t*>(stream[c]->ptr) < q.base + q.size, "a stream buffer comes from the pool of the picture being built");
                }
                if (!scratchStream) Require(!InPool(static_cast<uint8_t*>(stream[c]->ptr), 1), "without the switch the stream buffers are ordinary allocations");
            }
            Upload(stream[2], 2 * (4 + int(rng() % 400)) + int(rng() % 2), GL_STREAM_DRAW, picture, nextId, true);
            stream[2]->last_frame = vgl_framecount;
            r.streamInPool += InPool(static_cast<uint8_t*>(stream[2]->ptr), 1);
            // static geometry, hint GL_DYNAMIC_DRAW (their own heap blocks unless the dynamic switch is on), now and then given again
            if (rng() % 8 == 0) {
                vbo* s = statics[rng() % 4];
                Upload(s, 64 + int(rng() % 600), GL_DYNAMIC_DRAW, picture, nextId, false);
                if (!scratchDynamic) Require(!InPool(static_cast<uint8_t*>(s->ptr), 1), "dynamic buffers are heap blocks when only the stream switch is on");
            }
            // the draws read the data until the end of the frame
            for (const Live& l : g_live)
                if (l.picture == picture && !l.freed && !Intact(l)) Violation("buffer changed during its own frame", g_expectViolations);
            if (!visible) {
                CompleteUpTo(picture); // vglSubmitWithoutSwap ends in sceGxmFinish
            } else {
                fixture_swap_rotation();
                // the collector's job, enqueued at the end of the swap: cleans one list and advances the lists
                for (void* p : g_purge[g_cleanIdx]) vgl_free(p);
                g_purge[g_cleanIdx].clear();
                g_cleanIdx = (g_cleanIdx + 1) % 4;
                frame_purge_idx = (frame_purge_idx + 1) % 4;
                // the display queue: after swap `picture` returned, pictures up to picture-guaranteeLag are released; sometimes more
                int released = picture - guaranteeLag;
                if (rng() % 3 == 0) released = picture - int(rng() % (guaranteeLag + 1));
                if (released > completed) completed = released;
                CompleteUpTo(completed);
            }
        }
        if (checkRotation && hidden) Require(true, "hidden frames");
    }
    CompleteUpTo(pictures);
    // drain: delete everything, let the collector run through its lists
    vgl_framecount += 8;
    GLuint all[7];
    int n = 0;
    for (vbo* b : stream) all[n++] = reinterpret_cast<GLuint>(b);
    for (vbo* b : statics) all[n++] = reinterpret_cast<GLuint>(b);
    glDeleteBuffers(n, all);
    for (int i = 0; i < 8; ++i) {
        for (void* p : g_purge[g_cleanIdx]) vgl_free(p);
        g_purge[g_cleanIdx].clear();
        g_cleanIdx = (g_cleanIdx + 1) % 4;
    }
    unsigned leaked = 0;
    for (auto it = g_heap.begin(); it != g_heap.end();) {
        if (it->second.pool) { ++it; continue; }
        ++leaked;
        std::free(it->first);
        it = g_heap.erase(it);
    }
    if (leaked && !g_expectViolations) std::fprintf(stderr, "leaked %u heap blocks (seed %u)\n", leaked, seed);
    Require(leaked == 0 || g_expectViolations, "no heap block is left behind (the stream buffers' overflow blocks go to the collector)");
    r.heapAllocs = g_heapAllocs;
    r.heapFrees = g_heapFrees;
    r.dirtyMarks = g_dirtyMarks;
    r.pictures = pictures;
    for (auto it = g_heap.begin(); it != g_heap.end();) { std::free(it->first); it = g_heap.erase(it); }
    return r;
}

int main() {
    // 1. the promise holds (once swap F has returned, picture F-2 is complete): no overwrite, no early free, over many random runs
    Result on{}, off{};
    for (unsigned seed = 1; seed <= 40; ++seed) {
        const Result a = Run(seed, true, false, 300, 2, true);
        Require(g_violations == 0, "scratch stream buffers: no overwrite of memory the GPU may still read");
        const Result b = Run(seed, false, false, 300, 2, false);
        Require(g_violations == 0, "ordinary stream buffers: no early free");
        on.heapAllocs += a.heapAllocs; on.heapFrees += a.heapFrees; on.dirtyMarks += a.dirtyMarks; on.pictures += a.pictures; on.backendFrames += a.backendFrames;
        on.streamInPool += a.streamInPool; on.streamOnHeap += a.streamOnHeap;
        off.heapAllocs += b.heapAllocs; off.heapFrees += b.heapFrees; off.dirtyMarks += b.dirtyMarks; off.pictures += b.pictures; off.backendFrames += b.backendFrames;
    }
    Require(on.streamInPool > 0 && on.streamOnHeap > 0, "the runs cover both the pool and the overflow fallback");
    // 2. pools that hold a picture (Galaxy's are 10.7 MiB against about 0.1 MiB of stream data per picture): the same script, counted
    Result bigOn{}, bigOff{};
    for (unsigned seed = 1; seed <= 10; ++seed) {
        const Result a = Run(seed, true, false, 300, 2, true, 3 * 65536);
        Require(g_violations == 0, "scratch stream buffers, pools that hold a picture");
        const Result b = Run(seed, false, false, 300, 2, false, 3 * 65536);
        Require(g_violations == 0, "ordinary stream buffers, pools that hold a picture");
        Require(a.streamOnHeap == 0, "every stream buffer comes from the pool when the picture fits");
        bigOn.heapAllocs += a.heapAllocs; bigOn.dirtyMarks += a.dirtyMarks; bigOn.heapFrees += a.heapFrees; bigOn.backendFrames += a.backendFrames; bigOn.pictures += a.pictures;
        bigOff.heapAllocs += b.heapAllocs; bigOff.dirtyMarks += b.dirtyMarks; bigOff.heapFrees += b.heapFrees; bigOff.backendFrames += b.backendFrames; bigOff.pictures += b.pictures;
    }
    // 3. the test can fail: if the display queue released one picture fewer (a lag of 3 with 3 buffers), the pool would be overwritten
    g_expectViolations = true;
    unsigned caught = 0;
    for (unsigned seed = 1; seed <= 10; ++seed) {
        Run(seed, true, false, 300, 3, false);
        caught += g_violations;
    }
    g_expectViolations = false;
    Require(caught > 0, "a GPU lagging one picture more than the display queue allows is caught");
    // 4. a presented picture rotates the pool, an unpresented frame does not: positions only grow inside a picture
    {
        gxm_display_buffer_count = 3;
        g_poolInit = false;
        vgl_framecount = 0;
        vgl_circular_idx = 0;
        circular_data_pool_size = 3 * 4096;
        fixture_pool_init();
        g_poolInit = true;
        vglSetupScratchMemory(GL_FALSE, GL_TRUE);
        g_heapAllocs = g_dirtyMarks = g_violations = 0;
        vbo* b = NewBuffer();
        const GLuint h = reinterpret_cast<GLuint>(b);
        uint8_t zeros[100] = {};
        uint8_t* seen[7];
        int k = 0;
        for (int i = 0; i < 3; ++i) { glNamedBufferData(h, 100, zeros, GL_STREAM_DRAW); seen[k++] = static_cast<uint8_t*>(b->ptr); }
        Require(seen[1] == seen[0] + 112 && seen[2] == seen[1] + 112, "unpresented frames take the next 16-byte-rounded slice of the same pool");
        fixture_swap_rotation();
        glNamedBufferData(h, 100, zeros, GL_STREAM_DRAW);
        Require(static_cast<uint8_t*>(b->ptr) == circular_data_pool[1], "the next picture starts at the beginning of the next pool");
        fixture_swap_rotation();
        fixture_swap_rotation();
        glNamedBufferData(h, 100, zeros, GL_STREAM_DRAW);
        Require(static_cast<uint8_t*>(b->ptr) == circular_data_pool[0], "three swaps later the first pool is written again from its beginning");
        Require(g_heapAllocs == 0 && g_dirtyMarks == 0, "no heap allocation and no collector entry for stream buffers that fit the pool");
        // glBufferSubData of a buffer drawn this frame takes a fresh slice of the pool and leaves the old one alone
        b->last_frame = vgl_framecount;
        uint8_t* before = static_cast<uint8_t*>(b->ptr);
        glNamedBufferSubData(h, 4, 8, zeros);
        Require(static_cast<uint8_t*>(b->ptr) == before + 112 && g_dirtyMarks == 0, "SubData on a scratch buffer moves it to a new pool slice, nothing goes to the collector");
        // changing a buffer from scratch to a heap block (hint STATIC) frees nothing of the pool; back to scratch frees the heap block
        glNamedBufferData(h, 100, zeros, GL_STATIC_DRAW);
        Require(!InPool(static_cast<uint8_t*>(b->ptr), 1), "a static hint takes a heap block");
        glNamedBufferData(h, 100, zeros, GL_STREAM_DRAW);
        Require(InPool(static_cast<uint8_t*>(b->ptr), 100), "and back to the pool");
        GLuint hh = h;
        glDeleteBuffers(1, &hh);
        unsigned other = 0;
        for (auto it = g_heap.begin(); it != g_heap.end(); ++it) other += !it->second.pool;
        Require(other == 0 && g_violations == 0, "deleting the buffer leaves no heap block behind and frees nothing twice");
        for (auto it = g_heap.begin(); it != g_heap.end();) { std::free(it->first); it = g_heap.erase(it); }
    }
    std::printf("PASS: scratch stream buffers: the pool is never written while the GPU may read it (display queue promise of %d pictures), unpresented frames keep their pool, "
                "overflow falls back to the heap and the collector; 40 runs of 300 pictures\n", 2);
    std::printf("pools that hold a picture (%u pictures, %u back-end frames, 2-3 stream uploads and 0-1 static re-uploads each): stream buffers on: %u heap allocations, %u collector entries, "
                "%u frees; off: %u, %u, %u\n", bigOn.pictures, bigOn.backendFrames, bigOn.heapAllocs, bigOn.dirtyMarks, bigOn.heapFrees, bigOff.heapAllocs, bigOff.dirtyMarks, bigOff.heapFrees);
    std::printf("counts over the 40 runs with 2 KiB pools (%u pictures, %u back-end frames): stream buffers on: %u heap allocations, %u collector entries, %u frees; off: %u, %u, %u; "
                "stream buffers served from the pool %u, from the overflow fallback %u\n",
                on.pictures, on.backendFrames, on.heapAllocs, on.dirtyMarks, on.heapFrees, off.heapAllocs, off.dirtyMarks, off.heapFrees, on.streamInPool, on.streamOnHeap);
    return 0;
}
