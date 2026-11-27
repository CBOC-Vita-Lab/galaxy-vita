// GXV_FE_TWOCORE, the second worker ("B"): see vgx_twocore.h and vgx_twocore.cpp. This file is compiled into B's instance of the front
// end (the namespace vgxb), like vgx_twocore_b_state.cpp etc., which compile the front end's files a second time.
#define VGX_CLONE_B 1
#define vgx vgxb
#include "vgx_state.h"
#include "vgx_fesplit.h"
#include "vgx_guestmem.h"
#include "vgx_twocore.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <chrono>
#include <csetjmp>
#include <cstring>
#include <thread>

extern "C" int64_t port_host_time_ns(void) __attribute__((weak));
extern "C" int vgx2_wait_picture(uint32_t seen);
extern "C" void vgx2_b_cpu_note(void);
extern "C" void vgx2_b_nap(uint32_t us, const uint8_t* avail);
// (B's compilation of the front end's files)
extern "C" void vgx2_b_state_cold(void);
extern "C" void vgx2_b_draw_cold(uint32_t serialBase);
extern "C" void vgx2_b_draw_picture(const void* cha, uint32_t chaBytes, uint64_t frame);
extern "C" uint32_t vgx2_b_draw_cmds(std::vector<uint32_t>& jobs); // B's command list: placeholders (job numbers); ~0u: another command
extern "C" void vgx2_b_split_begin(void);
extern "C" void vgx2_b_split_end(Vgx2Seg* s);
extern "C" void vgx2_b_split_drop(void);
extern "C" void vgx2_b_dl_picture(void);
extern "C" int vgx2_b_state_objs(Vgx2Obj* out);
extern "C" int vgx2_b_draw_objs(Vgx2Obj* out);
extern "C" int vgx2_b_split_objs(Vgx2Obj* out);
extern "C" int vgx2_b_draw_can_begin(void);
extern "C" int vgx2_b_draw_resolved(void);
extern "C" void vgx2_a_split_drop(void* first);
extern "C" void vgx2_b_draw_sync_keys(void);
extern "C" int vgx2_b_draw_resolved(void);

namespace vgx {

// ---- what B's compilation takes from the player's (one per process)
uint64_t GuestWriteGeneration(const void* p, uint32_t n) { return vgx2_guest_gen(p, n); }
uint64_t GuestWriteEpoch() { return vgx2_guest_epoch(); }
void GuestWriteGenerationPrefetch(const void* p, uint32_t n) { vgx2_guest_gen_prefetch(p, n); }
bool GuestWritesTouch(uint64_t e0, uint64_t e1, const HostRange* r, int n) { return vgx2_guest_writes_touch(e0, e1, r, n) != 0; }
bool IsGuestRam(const void* p, uint32_t n) { return vgx2_is_guest_ram(p, n) != 0; }
bool LogEnabled() { return vgx2_log_enabled() != 0; }
bool TexFormatIsCI(uint32_t fmt) { return vgx2_tex_is_ci(fmt) != 0; }
uint32_t TexLevelSize(uint32_t w, uint32_t h, uint32_t fmt) { return vgx2_tex_level_size(w, h, fmt); }
bool IsCharaTexture(uint32_t id) {
    two::LockB();
    const int r = vgx2_is_chara_tex(id, 0);
    vgx2_unlock_b();
    return r != 0;
}
bool IsPlayerTexture(uint32_t id) {
    two::LockB();
    const int r = vgx2_is_chara_tex(id, 1);
    vgx2_unlock_b();
    return r != 0;
}
void TexMemoPrefetch(uint8_t, int) {}
void TexCacheEndFrame() {}
void TexCacheReport(FILE*) {}
void SubmitToBackend(CmdBuf&) {}
// (c42: the two below were missing; no Vita build had linked the second instance before, see vita/vgx-game/CMakeLists.txt)
namespace backend {
void Execute(const CmdBuf&) {}
uint64_t Cache24Hints() { return 0; } // (the "cache24:" line is the player's; B issues its own hints and reports none)
} // namespace backend
bool TexChannelRange(uint32_t id, uint8_t lo[4], uint8_t hi[4]) { // (pix23: the player's table, as IsCharaTexture reads the player's)
    // (twocore24, checked: the table is a vector the player grows under its lock only, TexRangeSet from ResolveTextureFull and TexCompApply,
    // both inside two::AGuard; the read below copies 8 bytes and allocates nothing)
    two::LockB();
    const int r = vgx2_tex_channel_range(id, lo, hi);
    vgx2_unlock_b();
    return r != 0;
}
uint32_t AcquireCopyTexture(const void*, uint16_t, uint16_t, uint32_t) { two::Abandon(9); }
uint32_t AllocTextureId() { two::Abandon(9); }

// B's texture binding: the player's full resolution, read only (vgx_texcache.cpp vgx2_tex_peek), from B's registers.
bool ResolveTexture(int texMap, TexBind& out, uint16_t& width, uint16_t& height) {
    const int m = texMap & 7;
    if (g.bpTexStale & (1u << m)) BpTexSlotSync(m);
    const TexSlot& slot = g.tex[m];
    uint64_t tlutHash = 0;
    if (slot.valid && slot.obj.data && TexFormatIsCI(slot.obj.format)) {
        const uint32_t ti = slot.obj.tlut;
        if (ti < static_cast<uint32_t>(kTlutSlots) && g.tlut[ti].valid) {
            const TlutObj& t = g.tlut[ti].obj;
            const uint32_t bytes = t.entries == 0xFFFF ? 32768u : static_cast<uint32_t>(t.entries) * 2u;
            // (B's palette memory holds what the loads it saw put there: a palette loaded before this picture is not known to it)
            if (!t.data || !two::TlutLoaded(ti, (bytes + 511u) >> 9)) two::Abandon(5);
            tlutHash = XXH3_64bits(t.data, bytes) ^ (static_cast<uint64_t>(t.format) << 60) ^ 1; // (vgx_texcache.cpp TlutHash)
        }
    }
    int ret = 0;
    void* entry = nullptr;
    two::LockB();
    const int ok = vgx2_tex_peek(&slot, tlutHash, &out, &width, &height, &ret, &entry);
    vgx2_unlock_b();
    if (!ok) two::Abandon(4);
    if (entry) two::NoteTexUse(entry);
    return ret != 0;
}

namespace two {
bool g_prescan = true;
bool g_decoding = false;
uint32_t g_fmtSeen = 0;
bool TinyOn() { return vgx2_tiny != 0; }
uint8_t g_listWhy = 0; // (StopClean's reason)

namespace {
enum : int { kSegNone = 0, kSegDecoding = 1, kSegClaimed = 2, kSegReady = 3, kSegDropped = 4 };
// why a segment ended (the log line): 1 the player claimed it, 2 a command B does not do, 3 a list it cannot take, 4 a shared table
// would change, 5 a palette not loaded in this picture, 6 the player passed, 7 the picture ended
enum : uint8_t { kWhyClaim = 1, kWhyCommand = 2, kWhyList = 3, kWhyShared = 4, kWhyPalette = 5, kWhyPassed = 6, kWhyEnd = 7 };
std::jmp_buf g_jmp;
const uint8_t* g_cmd = nullptr;   // the current top-level command
const uint8_t* g_segS = nullptr;  // the segment's first command
const uint8_t* g_failAt = nullptr; // a segment ended at its first command here: not begun there again
bool g_picEnd = false;
bool g_stopHere = false;          // Before asked the loop to stop (the state machine moved)
uint32_t g_serialBase = 0x40000000u;
uint64_t g_tlutLoaded[kTlutSlots / 64];
int64_t g_segT0 = 0;
uint32_t g_slot = 0;              // the slot of the segment being decoded (or the next to use)
// twocore25: a segment is published (vgx2_stop_at) only once it has resolved a draw (before that its end state would be the cold start's,
// which the player cannot take). One that ends before that goes back without a trace: its slot is not used up and the player never meets
// it. (Before: every segment was published at its first command; most ended there, at a tiny list, and the ring of 4 slots was full of
// empty segments the player had to reach and release before B could begin another.)
bool g_published = false;
Vgx2Seg* S() { return &vgx2_seg[g_slot]; }
std::atomic<int>& St() { return vgx2_seg_state[g_slot]; }
int64_t NowNs() { return port_host_time_ns ? port_host_time_ns() : 0; }
// twocore24: B's waits for the player's next bytes sleep. (std::this_thread::yield() is empty on the console: a "spin, then yield" loop
// at B's priority kept every thread below it off the fourth core for as long as a picture was open, the par-view worker among them;
// vgx_twocore_impl.h)
inline void Nap(uint32_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
// (GXV_FE_TWOCORE_INJECT, host tests only) 1: B stops in a segment the player has claimed, for GXV_FE_TWOCORE_INJECT_MS (0: for good),
// looking at nothing; 2: B holds the shared lock GXV_FE_TWOCORE_INJECT_MS at every look-up from the 6th picture on; 3: B sleeps
// 0..250 us at random at every hand-over (segment begin, claim seen, end, inside the lock), as a starved core would leave it.
uint32_t g_injRng = 0x2545F491u;
uint32_t InjRand(uint32_t n) {
    g_injRng ^= g_injRng << 13;
    g_injRng ^= g_injRng >> 17;
    g_injRng ^= g_injRng << 5;
    return n ? g_injRng % n : 0;
}
inline void InjJitter(uint32_t maxUs) {
    if (__builtin_expect(vgx2_inject == 3, 0) && InjRand(4) == 0) Nap(InjRand(maxUs) + 1);
}
uint32_t g_injLockPic = 0; // (inject 2: the picture whose lock was held already)
bool g_injStalled = false;
void InjStall() {
    // (inject 1: as stuck as a thread its core never runs: nothing is looked at; the process may end with B here)
    g_injStalled = true;
    const int64_t t0 = NowNs();
    for (;;) {
        Nap(1000);
        if (vgx2_inject_ms && NowNs() - t0 >= static_cast<int64_t>(vgx2_inject_ms) * 1000000) return;
    }
}

bool Supported(const uint8_t* p, const uint8_t* end) {
    const uint8_t c = *p;
    if (c == 0x61) {
        if (end - p < 5) return true; // (cut: the loop waits for the rest)
        const uint8_t r = p[1];
        return !(r == 0x45 || r == 0x47 || r == 0x48 || r == 0x52 || r == 0xFF);
    }
    return c == 0x00 || c == 0x08 || c == 0x10 || c == 0x20 || c == 0x28 || c == 0x30 || c == 0x38 || c == 0x40 || c == 0x44 || c == 0x48 ||
           c == 0x59;
}

void Snapshot(std::vector<uint8_t>& out) {
    Vgx2Obj o[32];
    int n = 0;
    n += vgx2_b_state_objs(o + n);
    n += vgx2_b_draw_objs(o + n);
    n += vgx2_b_split_objs(o + n);
    size_t total = 0;
    for (int i = 0; i < n; ++i) total += o[i].n;
    out.resize(total);
    uint8_t* q = out.data();
    for (int i = 0; i < n; ++i) {
        std::memcpy(q, o[i].p, o[i].n);
        q += o[i].n;
    }
}

void ResetLogs(Vgx2Seg& s) {
    s.cmdJobs.clear();
    s.dlUses.clear();
    s.dlRehash.clear();
    s.dlDrawn.clear();
    s.texUses.clear();
    s.tlutLoads.clear();
    s.chaBoxes.clear();
    s.usedAge = false;
    s.staticRuns = s.lists = s.stateLists = s.bytes = 0;
    s.decodeNs = 0;
    s.why = 0;
    s.listWhy = 0;
}

// The segment begins at cmd (B's state: the stream's up to here), in slot g_slot (free).
void BeginSegment(const uint8_t* cmd) {
    Vgx2Seg& s = *S();
    ResetLogs(s);
    s.s = s.e = cmd;
    for (int i = 0; i < VGX2_EP_COUNT; ++i) s.epochs[i] = vgx2_epoch[i].load(std::memory_order_relaxed);
    if (vgx2_mode == 2) {
        // (twocore25: the palette slots left out, as in the hand-over: nothing compares them)
        constexpr size_t a = offsetof(State, tlut), b = offsetof(State, tlut) + sizeof(State::tlut);
        s.snapS.resize(sizeof(State));
        std::memcpy(s.snapS.data(), &g, a);
        std::memcpy(s.snapS.data() + b, reinterpret_cast<const uint8_t*>(&g) + b, sizeof(State) - b);
    }
    vgx2_b_state_cold();
    g_serialBase += 0x100000u;
    if (g_serialBase < 0x40000000u) g_serialBase = 0x40000000u;
    vgx2_b_draw_cold(g_serialBase);
    vgx2_b_split_begin();
    g_segS = cmd;
    g_prescan = false;
    g_decoding = true;
    g_segT0 = NowNs();
    St().store(kSegDecoding, std::memory_order_release);
    g_published = false; // (vgx2_stop_at: Publish, at the first command after a draw was resolved)
    InjJitter(250);
}
// The segment's start made known to the player (its first draw is resolved). The player may be past it by now: Before drops it then, or the
// player's Missed() does.
void Publish() {
    g_published = true;
    vgx2_stop_at[g_slot].store(g_segS, std::memory_order_release);
}

// The segment ends before cmd. ready: hand it over (claimed or not); else drop it. Either way B moves on to the next slot.
void EndSegment(const uint8_t* cmd, uint8_t why, bool ready) {
    InjJitter(250);
    Vgx2Seg& s = *S();
    std::atomic<int>& st = St();
    g_decoding = false;
    g_prescan = true;
    if (!g_published) {
        // (twocore25: never published: the player cannot have looked at this slot; its operations go back and the slot stays B's next)
        vgx2_b_split_drop();
        s.firstChunk = s.lastChunk = nullptr;
        s.e = s.s;
        s.ops = s.jobs = 0;
        s.snap.clear();
        vgx2_b_unpub[why & 7].fetch_add(1, std::memory_order_relaxed);
        if (why == kWhyList) vgx2_b_unpub_list[g_listWhy < 12 ? g_listWhy : 0].fetch_add(1, std::memory_order_relaxed);
        st.store(kSegNone, std::memory_order_release);
        return;
    }
    const uint32_t slot = g_slot;
    g_slot = (g_slot + 1) % vgx2_nslots;
    if (ready && !vgx2_b_draw_resolved()) ready = false; // (no draw resolved: the player does the segment itself)
    if (ready && vgx2_b_draw_cmds(s.cmdJobs) == ~0u) ready = false;
    if (!ready) {
        vgx2_b_split_drop();
        s.firstChunk = s.lastChunk = nullptr;
        s.e = s.s; // (an empty segment: the player skips it)
        s.ops = s.jobs = 0;
        s.why = why;
        s.snap.clear();
    } else {
        s.e = cmd;
        s.why = why;
        s.bytes = static_cast<uint32_t>(cmd - s.s);
        s.decodeNs = NowNs() - g_segT0;
        vgx2_b_split_end(&s);
        vgx2_b_draw_sync_keys(); // (twocore25: GXV_DIRECT_J3D's deferred key copies made, so that cur.fs / cur.vs are the ids' keys)
        Snapshot(s.snap);
    }
    // (kSegDecoding / kSegClaimed -> kSegReady; kSegDropped: the player has gone past it: the operations go back to the pool)
    int v = st.load(std::memory_order_acquire);
    while ((v == kSegDecoding || v == kSegClaimed) && !st.compare_exchange_weak(v, kSegReady, std::memory_order_acq_rel)) {}
    if (v == kSegDropped) {
        if (s.firstChunk) vgx2_a_split_drop(s.firstChunk);
        s.firstChunk = s.lastChunk = nullptr;
        vgx2_stop_at[slot].store(nullptr, std::memory_order_relaxed);
        st.store(kSegNone, std::memory_order_release);
    }
}
} // namespace

void PictureEndSeen() { g_picEnd = true; }
// The shared lock, or the picture is given up (twocore24: B waits for the player's lock GXV_FE_TWOCORE_BLOCK_US at most, holding nothing)
void LockB() {
    if (!vgx2_lock_b()) Abandon(kAbandonLock);
    if (__builtin_expect(vgx2_inject != 0, 0)) {
        InjJitter(50);
        const uint32_t pic = vgx2_pic.load(std::memory_order_relaxed);
        if (vgx2_inject == 2 && pic >= 6) {
            g_injLockPic = pic;
            Nap(vgx2_inject_ms ? vgx2_inject_ms * 1000u : 30000u);
        }
    }
}
void NoteTlutLoad(uint32_t bp64, uint32_t bp65, uint32_t firstSlot, uint32_t slots) {
    for (uint32_t i = firstSlot; i < firstSlot + slots && i < static_cast<uint32_t>(kTlutSlots); ++i) g_tlutLoaded[i >> 6] |= 1ull << (i & 63);
    if (g_decoding) { S()->tlutLoads.push_back(bp64); S()->tlutLoads.push_back(bp65); }
}
bool TlutLoaded(uint32_t firstSlot, uint32_t slots) {
    for (uint32_t i = firstSlot; i < firstSlot + slots; ++i)
        if (i >= static_cast<uint32_t>(kTlutSlots) || !(g_tlutLoaded[i >> 6] & (1ull << (i & 63)))) return false;
    return true;
}
void NoteChaBox(int16_t x, int16_t y, int16_t w, int16_t h) {
    Vgx2Seg& s = *S();
    s.chaBoxes.push_back(x); s.chaBoxes.push_back(y); s.chaBoxes.push_back(w); s.chaBoxes.push_back(h);
}
void NoteDlUse(void* e) { S()->dlUses.push_back(e); S()->usedAge = true; ++S()->lists; }
void NoteStaticRuns(uint32_t n) { S()->staticRuns += n; }
void NoteTexUse(void* e) { S()->texUses.push_back(e); }
void NoteDlDrawn(void* e) { S()->dlDrawn.push_back(e); }
bool DlDrawnHere(void* e) {
    for (void* x : S()->dlDrawn)
        if (x == e) return true;
    return false;
}
void NoteDlRehash(void* e, uint64_t gl, const uint64_t ga[12]) {
    std::vector<uint64_t>& v = S()->dlRehash;
    v.push_back(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(e)));
    v.push_back(gl);
    for (int i = 0; i < 12; ++i) v.push_back(ga[i]);
}

void StopClean(int why) {
    if (!g_decoding) Abandon(8);
    g_listWhy = static_cast<uint8_t>(why);
    std::longjmp(g_jmp, 1);
}
void Abandon(int why) {
    std::longjmp(g_jmp, 2 + why);
}

// ProcessStream's top level (B's compilation), before each command: true = stop the loop here.
bool Before(const uint8_t* cmdStart, const uint8_t* end) {
    g_cmd = cmdStart;
    if (vgx2_cancel.load(std::memory_order_relaxed) || g_picEnd) return true;
    if (g_decoding) {
        InjJitter(100);
        const int st = St().load(std::memory_order_acquire);
        if (__builtin_expect(vgx2_inject == 1, 0) && st == kSegClaimed && vgx2_pic.load(std::memory_order_relaxed) >= 6) InjStall();
    } else if (__builtin_expect(vgx2_inject == 1, 0) && vgx2_pic.load(std::memory_order_relaxed) >= 12 && !g_injStalled) {
        InjStall(); // (no claim met by then: B stops in the middle of a picture all the same)
    }
    if (g_decoding) {
        if (!g_published && vgx2_b_draw_resolved()) Publish();
        const int st = St().load(std::memory_order_acquire);
        if (st == kSegClaimed) { EndSegment(cmdStart, kWhyClaim, true); g_stopHere = true; return true; }
        if (st == kSegDropped || vgx2_apos.load(std::memory_order_relaxed) > g_segS) { EndSegment(cmdStart, kWhyPassed, false); g_stopHere = true; return true; }
        if (!Supported(cmdStart, end)) { EndSegment(cmdStart, kWhyCommand, true); g_stopHere = true; return true; }
        return false;
    }
    // (scanning: a segment begins at a list call, far enough ahead of the player, when the next slot is free)
    if ((*cmdStart == 0x40 || *cmdStart == 0x59) && St().load(std::memory_order_acquire) == kSegNone && vgx2_b_draw_can_begin()) {
        const uint8_t* a = vgx2_apos.load(std::memory_order_relaxed);
        if (cmdStart > a && cmdStart != g_failAt && static_cast<uint32_t>(cmdStart - a) >= vgx2_lead_bytes) { BeginSegment(cmdStart); g_stopHere = true; return true; }
    }
    return false;
}

namespace {
// One picture, from `start` while the player publishes it, until it ends or is cancelled.
void RunPicture(const uint8_t* start) {
    const uint8_t* volatile pos = start;
    g_picEnd = false;
    g_failAt = nullptr;
    g_slot = 0;
    vgx2_dbg_b.clear();
    g_prescan = true;
    g_decoding = false;
    std::memset(g_tlutLoaded, 0, sizeof g_tlutLoaded);
    vgx2_b_dl_picture();
    const int j = setjmp(g_jmp);
    if (j != 0) {
        // (1: the command at g_cmd cannot be done in a segment; B's state is the stream's before it. 2+: half done: give up)
        if (j == 1 && g_decoding) {
            const uint8_t* at = g_cmd;
            if (at == g_segS) g_failAt = at;
            S()->listWhy = g_listWhy;
            EndSegment(at, kWhyList, true);
            pos = at;
        } else {
            if (j >= 2) vgx2_b_gaveup[(j - 2) & 15].fetch_add(1, std::memory_order_relaxed); // (the player's log line)
            if (g_decoding) EndSegment(g_cmd, kWhyShared, false);
            return;
        }
    }
    for (;;) {
        if (vgx2_cancel.load(std::memory_order_relaxed) || g_picEnd) break;
        const uint8_t* const avail = vgx2_pub_end.load(std::memory_order_acquire);
        if (pos >= avail) {
            if (__builtin_expect(vgx2_inject == 4, 0)) vgx2_b_atend.store(vgx2_pic.load(std::memory_order_relaxed), std::memory_order_release);
            // (nothing more to read yet: a segment ends here when the player is at its start, as it would before the next command)
            if (g_decoding) {
                const int st = St().load(std::memory_order_acquire);
                if (st == kSegClaimed) EndSegment(pos, kWhyClaim, true);
                else if (st == kSegDropped || vgx2_apos.load(std::memory_order_relaxed) > g_segS) EndSegment(pos, kWhyPassed, false);
            }
            for (int i = 0; i < 64 && vgx2_pub_end.load(std::memory_order_acquire) == avail; ++i) {}
            if (vgx2_pub_end.load(std::memory_order_acquire) == avail) {
                vgx2_b_unboost();
                vgx2_b_nap(30, avail); // (twocore24: a real sleep; twocore25: woken at the picture's end)
            }
            continue;
        }
        g_stopHere = false;
        const uint32_t n = ProcessStream(pos, static_cast<uint32_t>(avail - pos), false);
        pos = pos + n;
        if (n == 0 && !g_stopHere) {
            // (a command cut at the end of what is published: wait for more)
            for (int i = 0; i < 64 && vgx2_pub_end.load(std::memory_order_acquire) == avail; ++i) {}
            if (vgx2_pub_end.load(std::memory_order_acquire) == avail) vgx2_b_nap(30, avail);
        }
    }
    if (g_decoding) EndSegment(g_cmd, kWhyEnd, true);
    vgx2_b_unboost();
}
} // namespace
} // namespace two
} // namespace vgx

// ---- the player gives B the state the next picture starts from (B is idle)
extern "C" void* vgx2_resolve_hook(void);
extern "C" void vgx2_b_picture_state(const void* gA, uint32_t gBytes, const void* cha, uint32_t chaBytes, uint64_t frame) {
    {
        // (the player's address map; none of its callbacks: B never raises the game's interrupts)
        vgx::GuestHooks h{};
        h.resolve = reinterpret_cast<const uint8_t* (*)(uint32_t, uint32_t*)>(vgx2_resolve_hook());
        vgx::SetGuestHooks(h);
    }
    if (gBytes == sizeof(vgx::State)) std::memcpy(&vgx::g, gA, gBytes);
    vgx::g.layoutDirty = true; // (B's vertex plans were made for its own descriptor)
    vgx::g.bpTexStale = 0xFF;
    vgx2_b_draw_picture(cha, chaBytes, frame);
}

extern "C" void vgx2_b_tiny_state(void) { vgx2_b_tiny_n.fetch_add(1, std::memory_order_relaxed); }

extern "C" void vgx2_b_thread_main(void) {
    uint32_t seen = vgx2_pic.load(std::memory_order_acquire);
    for (;;) {
        if (vgx2_b_exit.load(std::memory_order_acquire)) return; // (twocore24: the switch went off for the session)
        if (!vgx2_wait_picture(seen)) continue;
        const uint32_t pic = vgx2_pic.load(std::memory_order_acquire);
        const uint8_t* start = vgx2_pic_start;
        seen = pic;
        vgx2_b_busy.store(1, std::memory_order_seq_cst);
        if (vgx2_cancel.load(std::memory_order_seq_cst) || vgx2_pic.load(std::memory_order_seq_cst) != pic) {
            vgx2_b_busy.store(0, std::memory_order_seq_cst);
            continue;
        }
        vgx::two::RunPicture(start);
        vgx2_b_cpu_note();
        vgx2_b_busy.store(0, std::memory_order_seq_cst);
    }
}
