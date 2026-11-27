// GXV_FE_TWOCORE: a two-core front end (fesplit23, 8 Nov 2026). The player's side: vgx_twocore.cpp; the second worker's side:
// vgx_twocore_b.cpp; report: agents/fesplit23/REPORT.txt.
//
// A second instance of the front end ("B": the same sources compiled a second time into the namespace vgxb, vgx_twocore_b_*.cpp,
// with VGX_CLONE_B defined) runs on the fourth core ahead of the player ("A", core 2). B follows the picture's stream with a
// state-only scan (register writes, material lists, palette loads; no draw, no cache) and, where it is far enough ahead of the
// player, decodes a contiguous run of the stream (a "segment": state commands, material lists, display-list cache hits of promoted
// lists, i.e. static J3D geometry) into operations for the split's finishing thread. When the player reaches the segment's first
// command it splices B's operations into its own operation stream (the finishing thread then makes the records in stream order),
// takes B's state at the segment's end and goes on from there. A command B cannot do exactly (a dynamic draw, an EFB copy, a token,
// a marker, a cache miss, a new shader, ...) ends B's segment before it: the player does it itself. Exactness: vgx_twocore.cpp.
//
// Included by both compilations. What is declared extern "C" exists once (the player's compilation defines it, unless its name
// says vgx2_b_) and is how the two meet; everything else is per compilation.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// What B hands over for one segment (written by B before it marks the segment ready; read by the player after).
struct Vgx2Seg {
    const uint8_t* s = nullptr;       // the segment's first command
    const uint8_t* e = nullptr;       // where B stopped (the first command not done)
    void* firstChunk = nullptr;       // B's operations: a chain of the split's chunks (vgx_fesplit.cpp), ops operations in all
    void* lastChunk = nullptr;
    uint32_t lastPos = 0, ops = 0, jobs = 0;
    uint32_t epochs[4] = {};          // the shared tables' epochs when B began (vgx2_epoch)
    std::vector<uint32_t> cmdJobs;    // B's command list: one placeholder per job, in order (B makes no other command)
    std::vector<void*> dlUses;        // display-list cache entries B used (their lastUse is the picture's age)
    std::vector<uint64_t> dlRehash;   // entries B found unchanged by their contents (CallListBody's "rehash"): entry, list's and 12 arrays'
                                      // write generations, 14 words each, for the player to store as that branch does
    std::vector<void*> texUses;       // texture cache entries B bound (their lastUsed is the picture)
    std::vector<void*> dlDrawn;       // (GXV_STATICCULL) entries whose first use since their build was B's (the "drawn" flag)
    std::vector<uint32_t> tlutLoads;  // palette loads B made: BP 0x64, 0x65 per load
    std::vector<int16_t> chaBoxes;    // GXV_CHARAA: the rectangles B's draws added (x, y, w, h per box)
    bool usedAge = false;             // B looked the display-list cache up (the picture's age is then in use)
    std::vector<uint8_t> snap;        // B's per-stream state at e (the objects of the vgx2_*_objs lists, in order)
    std::vector<uint8_t> snapS;       // (GXV_FE_TWOCORE=2) B's register state at s, for the player's check
    // B's own counters for the log line
    uint32_t staticRuns = 0, lists = 0, stateLists = 0, bytes = 0;
    int64_t decodeNs = 0;
    uint8_t why = 0;                  // why it ended (vgx_twocore_b.cpp kWhy*)
    uint8_t listWhy = 0;              // (kWhyList) StopClean's reason
};

extern "C" {
// GXV_FE_TWOCORE (vgx_twocore.cpp): 0 off (default; nothing of this runs), 1 on, 2 verify (B's segments are made, checked
// against the player's own decode and not used). Set once.
extern int vgx2_mode;
// The tables B reads while the player may change them: the player bumps an epoch whenever it changes one in a way that can
// change what B read, and a segment whose epochs moved while it was made is not used.
enum { VGX2_EP_DL = 0, VGX2_EP_TEX = 1, VGX2_EP_COUNT = 4 };
extern std::atomic<uint32_t> vgx2_epoch[VGX2_EP_COUNT];
// One lock over the shared tables: the player holds it while it changes their structure (insertions, erasures, rebuilds), B while
// it reads them. The player's is recursive (a cache build emits draws that look shaders up).
// twocore24: neither side can wait on the other without a bound. The player's wait for B's short critical sections first raises
// B's priority (port_vgx2_boost) and counts a hard timeout past GXV_FE_TWOCORE_LOCK_US; B never waits for the player's lock longer
// than GXV_FE_TWOCORE_BLOCK_US: vgx2_lock_b then returns 0 and B gives up its picture (two::Abandon). B holds the lock across nothing
// that can block (no allocation, no log, no other lock), and the player never waits for B while it holds the lock.
void vgx2_lock_a(void);
void vgx2_unlock_a(void);
int vgx2_lock_b(void);
void vgx2_unlock_b(void);
// (B's own thread: gives back a priority the player raised while B held the lock; cheap when nothing was raised)
void vgx2_b_unboost(void);
// Counters B keeps for the player's log line (written by B with relaxed stores): pictures B gave up by two::Abandon's reason
// (index = the reason, 0..15), and the test injections it made (GXV_FE_TWOCORE_INJECT)
extern std::atomic<uint32_t> vgx2_b_gaveup[16];
extern std::atomic<int> vgx2_b_exit;              // (the switch went off for the session: B's thread ends when idle)
extern int vgx2_inject;                           // GXV_FE_TWOCORE_INJECT (host tests only; 0 on the console unless asked)
extern uint32_t vgx2_inject_ms;                   // GXV_FE_TWOCORE_INJECT_MS
extern uint32_t vgx2_block_us;                    // GXV_FE_TWOCORE_BLOCK_US: B's longest wait for the player's lock
// twocore25: GXV_FE_TWOCORE_TINY (default 1): B parses the tiny lists that only write registers (J3D's small differed lists) as the player
// does, instead of ending its segment there; B's counters for the log: segments it dropped before publishing them (no draw resolved yet),
// by kWhy* and, for lists, by StopClean's reason; tiny register lists parsed in segments
extern int vgx2_tiny;
extern std::atomic<uint32_t> vgx2_b_unpub[8];
extern std::atomic<uint32_t> vgx2_b_unpub_list[12];
extern std::atomic<uint32_t> vgx2_b_tiny_n;
extern std::atomic<uint32_t> vgx2_b_atend;        // (GXV_FE_TWOCORE_INJECT=4, host measurement) the picture whose published bytes B went through
// ---- what the player's compilation gives B's (B's compilation forwards its own names to these: vgx_twocore_b.cpp)
uint64_t vgx2_guest_gen(const void* p, uint32_t n);
uint64_t vgx2_guest_epoch(void);
void vgx2_guest_gen_prefetch(const void* p, uint32_t n);
int vgx2_is_guest_ram(const void* p, uint32_t n);
int vgx2_guest_writes_touch(uint64_t e0, uint64_t e1, const void* ranges, int n);
int vgx2_log_enabled(void);
int vgx2_tex_is_ci(uint32_t fmt);
uint32_t vgx2_tex_level_size(uint32_t w, uint32_t h, uint32_t fmt);
int vgx2_is_chara_tex(uint32_t id, int playerOnly);    // (vgx_texcache.cpp; under B's lock)
int vgx2_tex_channel_range(uint32_t id, uint8_t* lo, uint8_t* hi); // (vgx_texcache.cpp, pix23's TexChannelRange; under B's lock)
// The binding of one texmap as the player's ResolveTextureFull gives it, without changing anything (vgx_texcache.cpp; under B's
// lock): slot = B's TexSlot of the texmap, tlutHash = its palette's hash (TlutHash; 0 none). 1 with *out etc. set, and *entry
// the cache entry (its lastUsed is to be set), or 0 when the cache would have to change (B's segment then ends).
int vgx2_tex_peek(const void* slot, uint64_t tlutHash, void* outBind, uint16_t* w, uint16_t* h, int* ret, void** entry);
int vgx2_reg_find(int fragment, const void* key, uint32_t n, uint64_t h); // (vgx_draw.cpp; under B's lock) a shader key's id, -1 none
int vgx2_variant_known(uint32_t progKey, uint64_t variantKey);           // (vgx_draw.cpp; under B's lock) on the warm-up list already
void* vgx2_dl_shared(int which);  // (vgx_dlcache.cpp) 0 the map, 1 the bypass map, 2 the age of the last sweep
void* vgx2_take_chunk(void);      // (vgx_fesplit.cpp) the split's chunk pool
void vgx2_give_chunk(void* c);
// ---- the protocol (vgx_twocore.cpp), read by B
extern std::atomic<const uint8_t*> vgx2_pub_end;  // bytes of the picture's stream B may read: up to here
extern std::atomic<const uint8_t*> vgx2_apos;     // the player's position in the top-level stream (twocore25: a cache line of its own)
extern std::atomic<uint32_t> vgx2_pic;            // bumped when a picture starts (vgx2_pic_start is its first byte)
extern const uint8_t* vgx2_pic_start;
extern std::atomic<int> vgx2_cancel;              // the player has ended the picture: B lets go of it
extern std::atomic<int> vgx2_b_busy;              // B is in a picture
// The segments: a ring of a few, which B fills and the player takes in order.
enum { VGX2_SLOTS = 32 };                         // (the arrays' size; twocore25: the ring's is vgx2_nslots, GXV_FE_TWOCORE_SLOTS, default 8)
extern uint32_t vgx2_nslots;
extern std::atomic<int> vgx2_seg_state[VGX2_SLOTS];          // see vgx_twocore_impl.h kSeg*
extern std::atomic<const uint8_t*> vgx2_stop_at[VGX2_SLOTS]; // the segment's first command, for the player's test
extern Vgx2Seg vgx2_seg[VGX2_SLOTS];
extern uint32_t vgx2_lead_bytes;                  // how far ahead of the player B must be to begin a segment (vgx_twocore.cpp)
extern uint64_t vgx2_pic_age;                     // the display-list cache's age of the picture (B's lookups use it)
// ---- (debug, GXV_FE_TWOCORE_DEBUG=1) per top-level command: offset in the picture and a hash of the registers before it
extern std::vector<uint64_t> vgx2_dbg_a, vgx2_dbg_b;
extern int vgx2_dbg;
uint64_t vgx2_a_regs_hash(void);
uint64_t vgx2_b_regs_hash(void);
// ---- the per-stream state of each file, for the hand-overs (the same objects in the same order in both compilations)
struct Vgx2Obj { void* p; uint32_t n; };
int vgx2_a_state_objs(Vgx2Obj* out);
int vgx2_a_draw_objs(Vgx2Obj* out);
int vgx2_a_split_objs(Vgx2Obj* out);
// (and vgx2_b_state_objs etc., B's compilation's; declared where they are used)
}

// The per-file lists are defined under names that differ between the compilations.
#if defined(VGX_CLONE_B)
#define VGX2_SIDE(name) vgx2_b_##name
#else
#define VGX2_SIDE(name) vgx2_a_##name
#endif

namespace vgx {
namespace two {
#if !defined(VGX_CLONE_B)
// ---- the player's side (vgx_twocore.cpp)
// ProcessStream's top level: the bytes [p, end) were handed over (any call), and the test before each command.
void Published(const uint8_t* p, const uint8_t* end);
const uint8_t* Arrive(const uint8_t* cmdStart);   // a segment starts here: where to go on (cmdStart: nothing taken)
void StartPicture(const uint8_t* first);
void PublishedUpTo(const uint8_t* end);           // (hosts that hand the stream over in pieces: what is there already)
void PictureEnd();                                // FrontPresent, once the picture is out
void ReportNow();                                 // the "fetwo:" summary of the pictures since the last one (hosts: at exit)
void SetThreadInit(int (*fn)(int core));          // (the host: the second worker's core and priority, on its own thread; 0 = not placed)
void Init();                                      // (split::FrameBegin: reads the switch once, starts the worker)
extern bool g_needStart;                          // the next top-level command is a picture's first
extern const uint8_t* g_verifyAt;                 // (GXV_FE_TWOCORE=2) where a segment B made ends: the player compares there
void VerifyAt(const uint8_t* cmdStart);
inline bool On() { return vgx2_mode != 0; }
extern uint32_t g_aSlot;                          // the slot of the next segment the player meets
extern bool g_aActive;                            // this picture was given to B (twocore24: no slot is looked at otherwise)
inline const uint8_t* StopAt() { return g_aActive ? vgx2_stop_at[g_aSlot].load(std::memory_order_relaxed) : nullptr; }
void Missed();                                    // the player is past the next segment's start without having met it
// The player's own changes of the shared tables: the lock (nothing while the mode is off) and the epochs.
struct AGuard {
    bool held;
    AGuard() : held(vgx2_mode != 0) { if (held) vgx2_lock_a(); }
    ~AGuard() { if (held) vgx2_unlock_a(); }
    AGuard(const AGuard&) = delete;
    AGuard& operator=(const AGuard&) = delete;
};
inline void Bump(int t) { if (vgx2_mode) vgx2_epoch[t].fetch_add(1, std::memory_order_relaxed); }
#else
struct AGuard { AGuard() {} }; // (B's compilation: the player's guards are nothing)
inline void Bump(int) {}
// ---- B's side (vgx_twocore_b.cpp), called from B's compilation of the front end
extern bool g_prescan;                            // B is scanning, not decoding
extern bool g_decoding;                           // B is in a segment
bool TinyOn();                                    // (twocore25) GXV_FE_TWOCORE_TINY
extern uint32_t g_fmtSeen;                        // (the scan) the vertex formats of the draws parsed since it was cleared
bool Before(const uint8_t* cmdStart, const uint8_t* end); // ProcessStream's top level, before each command: true = stop here
// The current top-level command cannot be done: the segment ends before it. why (the log's "list stops"): 1 a cache mode B does not
// run, 2 a tiny list that draws, 3 not in tracked guest memory, 4 a bypassed list that draws, 5 a sweep due, 6 a list the cache does not
// know, 7 no entry for the vertex state, 8 the contents changed, 9 not promoted (or about to be)
[[noreturn]] void StopClean(int why);
// Something was half done: B gives the picture up. why: 1 a dynamic draw, 2 a shader key the player has not made, 3 a warm-up variant
// not noted yet, 4 a texture the cache would have to change, 5 a palette not loaded in this picture, 7 inside the HUD markers, 8 a list
// stop outside a segment, 9 an EFB copy texture, 10 the player's lock held longer than GXV_FE_TWOCORE_BLOCK_US (twocore24)
[[noreturn]] void Abandon(int why);
enum : int { kAbandonLock = 10 };
void LockB();                                     // vgx2_lock_b, or Abandon(kAbandonLock) (and the test injections inside the lock)
void PictureEndSeen();                            // the display copy (B stops at the picture's end)
void NoteTlutLoad(uint32_t bp64, uint32_t bp65, uint32_t firstSlot, uint32_t slots);
bool TlutLoaded(uint32_t firstSlot, uint32_t slots);
void NoteChaBox(int16_t x, int16_t y, int16_t w, int16_t h);
void NoteDlUse(void* e);
void NoteTexUse(void* e);
void NoteDlRehash(void* e, uint64_t gl, const uint64_t ga[12]);
void NoteDlDrawn(void* e);                        // (GXV_STATICCULL) the entry's first use since its build was B's
bool DlDrawnHere(void* e);                        // ... in this segment
void NoteStaticRuns(uint32_t n);                   // (the log) static runs emitted from a cache hit
#endif
} // namespace two
} // namespace vgx
