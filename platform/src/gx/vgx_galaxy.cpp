// See vgx_galaxy.h.
#if defined(PORT_GX_VGX) && PORT_GX_VGX
#include "vgx_galaxy.h"

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "port/gx_fifo.h"
#include "port/heap_routing.h"
#include "port/port.h"
#include "port/diagnostics.h"
#include "port/qemu_hooks.h"
#include "port/vclock.h"
#include "vgx_fesplit.h"
#include "vgx_guestmem.h"
#include "vgx_hdtex.h"
#include "vgx_hle.h"
#include "vgx_state.h"

extern "C" volatile uint32_t __PortGXPERegs[0x400];
extern "C" void port_headless_write_png(const char* path, const unsigned char* rgba, int w, int h);
#if !defined(PORT_GX_VGX_ONLY)
extern "C" void port_gx_use_recorder_backend(void);
#endif
// A native pointer's region: its start and size (os_mem.cpp).
extern "C" int port_native_window(const void* p, void** base, size_t* bytes);
extern "C" int port_memory_region(unsigned index, void** base, void** alias, uint32_t* physical, uint32_t* size, int* kind);
extern "C" int64_t port_thread_cpu_us(void); // CPU time of the calling thread
extern "C" void port_perf_game_totals(int64_t* workNs, uint32_t* frames, int64_t* cpuUs, int64_t* maxWorkNs); // port/perf.cpp
extern "C" uint32_t port_frameskip_count(void);
extern "C" void port_handheld_texture(uint64_t hash, uint32_t width, uint32_t height, uint8_t* rgba); // port/handheld_ui.cpp
extern "C" bool port_handheld_texture_needs_rgba(uint64_t hash, uint32_t width, uint32_t height);
#if defined(__vita__)
extern "C" int port_vita_pin_core(int core); // vita/runtime/memory.cpp
extern "C" int port_vgx_vita_start(void); // vita/runtime: vitaGL, on the calling (render) thread
extern "C" void port_vgx_vita_frame_done(void);
extern "C" void port_vgx_vita_ready(void);          // the back end exists: the loading screen moves over to it
extern "C" int port_vgx_vita_splash_active(void);   // 1 while the loading screen is up
extern "C" void port_vgx_vita_idle(void);           // keep the loading screen moving when no frame arrives
extern "C" void port_vgx_vita_idle_gap(unsigned gapUs);  // (the same, not sooner than gapUs after the last picture)
extern "C" void port_vgx_vita_prepare(void);       // the host's file work and settings for the front end, before the render thread starts
#endif

namespace vgx {
bool LogEnabled() {
    static const bool on = EnvInt("GXV_LOG", 0) != 0;
    return on;
}
// (FrontPresent's fall-back when no frame sink is set; the sink below always is)
void SubmitToBackend(CmdBuf&) {}
} // namespace vgx

extern "C" void port_input_latency_frame_done(unsigned long long shown); // input/wpad.cpp

namespace {

std::atomic<bool> sActive{false};
std::atomic<unsigned long long> sFramesQueued{0}, sFramesShown{0};

// ------------------------------------------------------------------ "perf:" line
// Every GXV_PERF seconds (default 5 on handhelds, 0 = off elsewhere), on the render thread: how
// fast the game runs and what each of the three threads spent per game frame. This is the
// line to read after a run on the console: the slowest of the three sets the frame rate.
std::atomic<int64_t> sPlCpuUs{0};      // the player thread's CPU time at its last display copy
std::atomic<int64_t> sGmWaitNs{0};     // game thread: time spent waiting for the player (full segment)
std::atomic<uint64_t> sPlRecords{0}, sPlPrims{0}; // draw records / GX primitives of all frames so far (front end)
struct PerfWindow {
    int64_t t0 = 0, work = 0, gameCpu = 0, plCpu = 0, beCpu = 0, beWall = 0, wait = 0;
    uint32_t frames = 0;
    unsigned long long queued = 0, shown = 0;
    uint64_t draws = 0, records = 0, prims = 0;
    uint32_t skipped = 0;
} sPerf;
int sPerfSeconds = 0;
// GXV_CHARAA (vgx_draw.cpp, character edge smoothing): what the last window smoothed, per display copy (front end: frames with
// character rectangles, character draws, rectangles and their EFB pixels; back end: rectangles drawn and the screen pixels
// shaded a second time, presents skipped because the EFB is multisampled). Every perf window, when the switch is on.
void CharaPerfLine() {
    if (vgx::CharaAAMode() == 0) return;
    static uint64_t lf[11] = {}, lb[4] = {};
    uint64_t f[11], b[4];
    vgx::CharaCounters(f);
    vgx::backend::CharaAACounters(b[0], b[1], b[2], b[3]);
    const double copies = f[0] - lf[0] ? static_cast<double>(f[0] - lf[0]) : 1.0;
    port_log("perf: charaa: %llu display copies, %llu with characters; per copy: %.1f character draws (%.1f without bounds), %.1f HUD draws measured, "
             "%.2f boxes, %.0f EFB px; back end: %.2f boxes drawn and %.0f screen px shaded per copy, %llu over the area limit, %llu dropped by "
             "clearing copies, %llu HUD-cut pieces lost, %llu too small, %llu presents skipped (multisampled EFB)",
             (unsigned long long)(f[0] - lf[0]), (unsigned long long)(f[1] - lf[1]), (f[2] - lf[2]) / copies, (f[3] - lf[3]) / copies, (f[4] - lf[4]) / copies,
             (f[5] - lf[5]) / copies, (f[6] - lf[6]) / copies, (b[1] - lb[1]) / copies, (b[2] - lb[2]) / copies, (unsigned long long)(f[7] - lf[7]),
             (unsigned long long)(f[8] - lf[8]), (unsigned long long)(f[9] - lf[9]), (unsigned long long)(f[10] - lf[10]),
             (unsigned long long)(b[3] - lb[3]));
    std::memcpy(lf, f, sizeof f);
    std::memcpy(lb, b, sizeof b);
}

// Freeze watch (the Vita, the log on; GXV_FREEZE_WATCH=0 off): a thread looks once a second whether a new picture was shown.
// When none came for 3 s it writes a line at once (flushed to the card, so it is there even if the game never recovers), saying
// whether the game thread is still running turns (then the renderer is what stopped) or not (a load, or the game is stuck), and
// when pictures come back it writes how long the screen stood still. Counts seconds of its own loop, not the clock: a console
// that sleeps or suspends the game stops this thread too, and that is not reported as a freeze. Neither is a cutscene skip: its
// fast-forward runs the game's turns without pictures on purpose (a few seconds for the prologue).
extern "C" void port_log_flush(void);
extern "C" int port_skip_fast_forwarding(void) __attribute__((weak));
void StartFreezeWatch() {
    if (!PORT_LOGGING || vgx::EnvInt("GXV_FREEZE_WATCH", 1) == 0) return;
    std::thread([] {
        unsigned long long lastShown = sFramesShown.load(std::memory_order_relaxed);
        uint32_t lastTurns = 0;
        int still = 0;
        bool reported = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const unsigned long long shown = sFramesShown.load(std::memory_order_relaxed);
            int64_t work = 0, cpu = 0, maxWork = 0;
            uint32_t turns = 0;
            port_perf_game_totals(&work, &turns, &cpu, &maxWork);
            if (port_skip_fast_forwarding && port_skip_fast_forwarding()) {
                still = 0;  // (a skip, not a freeze)
                reported = false;
                lastShown = shown;
            } else if (shown != lastShown) {
                if (reported) port_log_summary("freeze watch: pictures again after about %d s without one", still);
                still = 0;
                reported = false;
                lastShown = shown;
            } else if (shown > 0) {  // (not before the first picture: the boot has its own loading screen)
                ++still;
                if (still == 3 || (reported && still % 30 == 0)) {
                    port_log_summary("freeze watch: no new picture for %d s; the game thread %s (%u turns in the last second)", still,
                                     turns != lastTurns ? "is running, the renderer is not presenting" : "is not running turns (a load, or stuck)",
                                     turns - lastTurns);
                    port_log_flush();
                    reported = true;
                }
            }
            lastTurns = turns;
        }
    }).detach();
}

// (render thread) `execNs`: wall time of the frame just executed.
void PerfFrame(int64_t execNs, uint32_t drawCalls) {
    if (!PORT_LOGGING) return;
    if (sPerfSeconds <= 0) return;
    if (!PORT_RUNTIME_DIAGNOSTICS) {
        // The player build's basic log (ux0:data/smg/log.txt): every GXV_PERF seconds (default 5) one line with the frame rate,
        // the hitches (presents more than 100 ms apart: what a player feels as a stutter) and the picture size.
        const int64_t now = port_host_time_ns();
        static int64_t lastPresent = 0, longest = 0;
        static unsigned hitches = 0;
        if (lastPresent) {
            const int64_t gap = now - lastPresent;
            if (gap > 100000000ll) ++hitches;
            if (gap > longest) longest = gap;
        }
        lastPresent = now;
        if (sPerf.t0 && now - sPerf.t0 < static_cast<int64_t>(sPerfSeconds) * 1000000000ll) return;
        PerfWindow w;
        int64_t unusedMax;
        port_perf_game_totals(&w.work, &w.frames, &w.gameCpu, &unusedMax);
        w.t0 = now;
        w.shown = sFramesShown.load(std::memory_order_relaxed);
        w.skipped = port_frameskip_count();
        if (sPerf.t0) {
            const double seconds = (now - sPerf.t0) / 1e9;
            int rw = 0, rh = 0, rs = 1;
            vgx::backend::QualityRungSize(rw, rh, rs);
            // Shaders prepared since the last line (loaded from the ready-made cache or compiled; the ms tell which: a real compile of a new effect is a hitch, the package ships 1,131 ready ones).
            static uint32_t lastCompiles = 0;
            static double lastShaderMs = 0;
            const vgx::backend::Stats bs = vgx::backend::GetStats();
            const unsigned compiles = bs.shaderCompiles - lastCompiles;
            const double compileMs = bs.shaderWorkMs - lastShaderMs;
            lastCompiles = bs.shaderCompiles;
            lastShaderMs = bs.shaderWorkMs;
            char compileNote[64] = "";
            if (compiles) std::snprintf(compileNote, sizeof compileNote, "; %u shaders prepared (%.0f ms)", compiles, compileMs);
            port_log_summary("fps: %.1f s: shown %.1f fps; game %.1f turns/s; %u skipped; %u hitches over 100 ms (longest %.0f ms); picture %dx%d%s%s",
                             seconds, (w.shown - sPerf.shown) / seconds, (w.frames - sPerf.frames) / seconds, w.skipped - sPerf.skipped, hitches,
                             longest / 1e6, rw, rh, rs > 1 ? (rs > 2 ? " 4x" : " 2x") : "", compileNote);
            hitches = 0;
            longest = 0;
            CharaPerfLine();
        }
        sPerf = w;
        return;
    }
    static int64_t beWall = 0;
    static uint64_t draws = 0;
    beWall += execNs;
    draws += drawCalls;
    const int64_t now = port_host_time_ns();
    if (!sPerf.t0) {
        sPerf.t0 = now;
        return;
    }
    if (now - sPerf.t0 < static_cast<int64_t>(sPerfSeconds) * 1000000000ll) return;
    PerfWindow w;
    int64_t maxWork = 0;
    port_perf_game_totals(&w.work, &w.frames, &w.gameCpu, &maxWork);
    w.t0 = now;
    w.plCpu = sPlCpuUs.load(std::memory_order_relaxed);
    w.beCpu = port_thread_cpu_us();
    w.beWall = beWall;
    w.wait = sGmWaitNs.load(std::memory_order_relaxed);
    w.queued = sFramesQueued.load();
    w.shown = sFramesShown.load();
    w.draws = draws;
    w.records = sPlRecords.load(std::memory_order_relaxed);
    w.prims = sPlPrims.load(std::memory_order_relaxed);
    w.skipped = port_frameskip_count();
    const double secs = (now - sPerf.t0) / 1e9;
    const unsigned gf = static_cast<unsigned>(w.queued - sPerf.queued), sf = static_cast<unsigned>(w.shown - sPerf.shown);
    const unsigned lf = w.frames - sPerf.frames; // frame-loop turns (a turn without a display copy is a loading frame)
    const double g = gf ? gf : 1.0, s = sf ? sf : 1.0, l = lf ? lf : 1.0;
    port_log("perf: %.1f s: game %.1f fps (%.1f loop turns/s, %u without a picture), shown %.1f fps; game thread %.1f ms/turn avg %.1f max, cpu %.1f, waited for player %.1f; "
             "player cpu %.1f ms/frame, %u primitives in %u draw records/frame; back end %.1f ms/frame wall, cpu %.1f, %u draws/frame",
             secs, gf / secs, lf / secs, w.skipped - sPerf.skipped, sf / secs, (w.work - sPerf.work) / 1e6 / l, maxWork / 1e6, (w.gameCpu - sPerf.gameCpu) / 1e3 / l,
             (w.wait - sPerf.wait) / 1e6 / l, (w.plCpu - sPerf.plCpu) / 1e3 / g, static_cast<unsigned>((w.prims - sPerf.prims) / (gf ? gf : 1)),
             static_cast<unsigned>((w.records - sPerf.records) / (gf ? gf : 1)), (w.beWall - sPerf.beWall) / 1e6 / s, (w.beCpu - sPerf.beCpu) / 1e3 / s,
             static_cast<unsigned>((w.draws - sPerf.draws) / (sf ? sf : 1)));
    {
        static uint64_t lastSl[5] = {};
        uint64_t sl[5];
        vgx::StateListCounters(sl);
        if (sl[0] != lastSl[0] || sl[1] != lastSl[1] || sl[4] != lastSl[4])
            port_log("perf: state lists: %llu replays, %llu parses, %llu not cacheable%s%llu verified, %llu mismatching",
                     (unsigned long long)(sl[0] - lastSl[0]), (unsigned long long)(sl[1] - lastSl[1]), (unsigned long long)(sl[2] - lastSl[2]),
                     "; ", (unsigned long long)(sl[3] - lastSl[3]), (unsigned long long)(sl[4] - lastSl[4]));
        std::memcpy(lastSl, sl, sizeof sl);
    }
    {
        // HD texture replacement (vgx_hdtex.cpp): logged when something happened since the last line.
        static uint64_t lastHashed = 0, lastHits = 0;
        vgx::hdtex::Counters hc;
        vgx::hdtex::GetCounters(hc);
        if (hc.entries && (hc.hashed != lastHashed || hc.hits != lastHits))
            port_log("perf: hdtex: %llu replaced (%llu KiB read in %.1f ms, longest %.2f ms), %llu refused over budget, %llu uploads hashed; "
                     "lookups %.2f ms in all; HD textures alive at most %llu KiB",
                     (unsigned long long)hc.hits, (unsigned long long)(hc.bytesRead >> 10), hc.readNs / 1e6, hc.readMaxNs / 1e6,
                     (unsigned long long)hc.refused, (unsigned long long)hc.hashed, hc.lookupNs / 1e6, (unsigned long long)(hc.peakLive >> 10));
        lastHashed = hc.hashed;
        lastHits = hc.hits;
    }
    CharaPerfLine();
    {
        // Front-end split (GXV_SPLIT, vgx_fesplit.h): the finishing thread's CPU per frame, and how long the player waited for it.
        // ("caught up": the player's time at the frame ends, waiting for the finishing thread or doing what it had not done yet)
        static int64_t lastCpu = 0, lastWait = 0;
        static uint32_t lastFrames = 0, lastStolen = 0;
        int64_t cpu = 0, wait = 0;
        uint32_t frames = 0;
        vgx::split::Counters(cpu, wait, frames);
        const uint32_t stolen = vgx::split::StolenOps();
        if (frames != lastFrames) {
            const double n = frames - lastFrames;
            port_log("perf: front-end split: %u frames, finishing thread cpu %.1f ms/frame; the player caught up with it for %.2f ms/frame, "
                     "doing %.0f of its operations/frame itself",
                     frames - lastFrames, (cpu - lastCpu) / 1e3 / n, (wait - lastWait) / 1e6 / n, (stolen - lastStolen) / n);
        }
        lastCpu = cpu;
        lastWait = wait;
        lastFrames = frames;
        lastStolen = stolen;
    }
    {
        // GXV_RSCACHE / GXV_REGSHADOW / GXV_TEXMEMO (vgx): memo use, and the verify modes' checks (=2) since the last line
        static uint64_t last[9] = {};
        uint64_t c[9];
        vgx::ResolveCacheCounters(c);
        vgx::RegShadowCounters(c + 4);
        vgx::TexMemoCounters(c + 6);
        port_log("perf: fecache: key memo %llu hits %llu misses, texture memo %llu hits; verified: resolutions %llu (%llu mismatching), "
                 "register writes %llu (%llu mismatching), bindings %llu (%llu mismatching)",
                 (unsigned long long)(c[2] - last[2]), (unsigned long long)(c[3] - last[3]), (unsigned long long)(c[6] - last[6]),
                 (unsigned long long)(c[0] - last[0]), (unsigned long long)(c[1] - last[1]), (unsigned long long)(c[4] - last[4]),
                 (unsigned long long)(c[5] - last[5]), (unsigned long long)(c[7] - last[7]), (unsigned long long)(c[8] - last[8]));
        std::memcpy(last, c, sizeof c);
    }
    sPerf = w;
}

// ------------------------------------------------------------------ address hook
// Console address (as the game's GX library writes it into a register) -> host memory.
// Physical addresses below 0x40000000 go through the port's region table; anything above
// is a native pointer (Vita, PETARI_NATIVE_ADDRESSES) or a cached / uncached console alias.
struct Window { uint32_t lo = 1, hi = 0; const uint8_t* data = nullptr; };
Window sPhysWin[4], sNatWin[4];
unsigned sPhysNext, sNatNext;

const uint8_t* Resolve(uint32_t addr, uint32_t* avail) {
    *avail = 0;
    if (addr < 0x40000000u) {
        for (const Window& w : sPhysWin)
            if (addr >= w.lo && addr < w.hi) { *avail = w.hi - addr; return w.data + (addr - w.lo); }
        void* data = nullptr;
        uint32_t base = 0;
        size_t bytes = 0;
        if (!port_physical_window(addr, &data, &base, &bytes) || !data) return nullptr;
        Window& w = sPhysWin[sPhysNext++ & 3];
        w.lo = base; w.hi = base + static_cast<uint32_t>(bytes); w.data = static_cast<const uint8_t*>(data);
        *avail = w.hi - addr;
        return w.data + (addr - w.lo);
    }
    for (const Window& w : sNatWin)
        if (addr >= w.lo && addr < w.hi) { *avail = w.hi - addr; return w.data + (addr - w.lo); }
    void* base = nullptr;
    size_t bytes = 0;
    if (!port_native_window(reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), &base, &bytes) || !base) return nullptr;
    Window& w = sNatWin[sNatNext++ & 3];
    w.lo = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base));
    w.hi = w.lo + static_cast<uint32_t>(bytes);
    w.data = static_cast<const uint8_t*>(base);
    *avail = w.hi - addr;
    return w.data + (addr - w.lo);
}

// ------------------------------------------------------------------ pixel-engine sync
void PeFinish() { port_irq_raise(PORT_IRQ_PE_FINISH); }
void PeToken(uint16_t token, bool interrupt) {
    reinterpret_cast<volatile uint16_t*>(__PortGXPERegs)[7] = token;
    if (interrupt) port_irq_raise(PORT_IRQ_PE_TOKEN);
}

// ------------------------------------------------------------------ back-end thread
// Owns the GL context. Frames (command buffers) are handed over through a short queue: the
// front end blocks only when `sMaxInFlight` frames are waiting, which is what paces a game
// that produces frames faster than they can be drawn.
std::mutex sMtx;
std::condition_variable sCv;
std::vector<vgx::CmdBuf*> sFree, sJobs;
unsigned sMaxInFlight = 1;
// How many frames the back end may hold at once (the one it draws and the ones waiting for it; sMaxInFlight + 1 buffers exist).
// Every frame that waits is a frame of delay between a press and the picture it causes.  Where the back end is the bottleneck (a
// busy scene at 30 fps) a waiting frame buys nothing, so the limit drops to one and the press shows up 1-2 frames sooner; where
// it is not (60 fps scenes) the queue evens out the frames' unequal times, and the full limit stays.  Guarded by sMtx.
unsigned sQueueLimit = 1;
bool sAdaptDepth = false;
// Where the back end is, for the stall report: 0 waiting for a frame, 1 executing it, 2 after it (the counters, the picture
// copy), and when it got there (host ns).
std::atomic<int> sBackStage{0};
std::atomic<int64_t> sBackSince{0};
bool sSkipWhenBehind = false;
bool sStarted = false, sStartOk = false, sThreadMade = false;
pthread_t sThread;
// capture request (desktop)
std::string sCapPath;
bool sCapWanted = false, sCapDone = false;
unsigned long long sCapFrame = 0;

void RenderMain() {
    PORT_QEMU_THREAD("vgx-render");
    port_thread_class(3);
    bool ok;
#if defined(__vita__)
    ok = port_vgx_vita_start() != 0;
#else
    ok = vgx::host::CreateContext();
#endif
    if (ok) {
        int w = 0, h = 0;
        if (const char* rs = getenv("GXV_RENDER_SIZE")) sscanf(rs, "%dx%d", &w, &h);
        ok = vgx::backend::Init(w, h);
    }
    {
        std::lock_guard<std::mutex> lk(sMtx);
        sStarted = true;
        sStartOk = ok;
    }
    sCv.notify_all();
    if (!ok) return;
#if defined(__vita__)
    port_vgx_vita_ready();
#endif
    for (;;) {
        std::unique_lock<std::mutex> lk(sMtx);
#if defined(__vita__)
        // While the loading screen is up the wait has a timeout: the picture moves although no frame comes.
        // GXV_WARM_IDLE=1: the deferred shader warm-up is worked off here too, whenever no frame is waiting. While the game loads a frame
        // comes about every 70 ms and this thread is idle in between, but the warm-up reads the shader cache from the card and the game
        // loads from the same card: on the console the game's own loading took 22 s instead of 12 s and the start was no shorter
        // (26 s against 24-27 s), so by default only the slice at the start of each frame is done.
        static const bool warmIdleOn = getenv("GXV_WARM_IDLE") && getenv("GXV_WARM_IDLE")[0] == '1';
        while (sJobs.empty()) {
            const bool warm = warmIdleOn && vgx::backend::WarmPending() > 0;
            if (!port_vgx_vita_splash_active()) {
                // (a time limit, not an endless wait: Vita3K's emulation of the kernel's condition variables sometimes loses a
                // wake-up, and with the queue cut to one frame the back end waits for every frame; the render stood still for good
                // in three emulator runs, stall report: "waiting for a frame" while a frame was queued. The console has not shown it; this costs it nothing.)
                if (sCv.wait_for(lk, std::chrono::milliseconds(warm ? 1 : 20), [] { return !sJobs.empty(); })) break;
                if (warm) {
                    lk.unlock();
                    vgx::backend::WarmIdle();
                    lk.lock();
                }
                continue;
            }
            if (sCv.wait_for(lk, std::chrono::milliseconds(warm ? 1 : 20), [] { return !sJobs.empty(); })) break;
            lk.unlock();
            if (warm) {
                port_vgx_vita_idle_gap(80000u);
                vgx::backend::WarmIdle();
            } else {
                port_vgx_vita_idle();
            }
            lk.lock();
        }
#else
        sCv.wait(lk, [] { return !sJobs.empty(); });
#endif
        vgx::CmdBuf* job = sJobs.front();
        const bool cap = sCapWanted;
        // Desktop harness (software GL): when frames pile up, the older ones only have their
        // uploads, shaders, copies and clears executed, so that the game keeps its pace and
        // scripted input (which is timed by the clock) arrives at the same moments as in a
        // reference run. A frame that is captured is always drawn in full.
        const bool skip = sSkipWhenBehind && !cap && sJobs.size() > 1;
        lk.unlock();
        vgx::backend::SetSkipDraws(skip);
        vgx::backend::RequestCapture(cap);
        const int64_t tExec = PORT_RUNTIME_DIAGNOSTICS && sPerfSeconds > 0 ? port_host_time_ns() : 0;
        const int64_t tAdapt = sAdaptDepth ? port_host_time_ns() : 0;
        if (PORT_RUNTIME_DIAGNOSTICS) {
            sBackSince.store(port_host_time_ns(), std::memory_order_relaxed);
            sBackStage.store(1, std::memory_order_relaxed);
        }
        vgx::backend::Execute(*job);
        if (PORT_RUNTIME_DIAGNOSTICS) {
            sBackSince.store(port_host_time_ns(), std::memory_order_relaxed);
            sBackStage.store(2, std::memory_order_relaxed);
        }
        unsigned newLimit = 0;  // (0: unchanged)
        if (sAdaptDepth) {
            // The mean time of 24 frames' execution (a frame's time is capped, so a shader-compile stall does not decide).
            static int64_t sumNs = 0;
            static unsigned n = 0, changes = 0;
            static bool tight = false;
            const int64_t t = port_host_time_ns() - tAdapt;
            sumNs += t < 60000000 ? t : 60000000;
            if (++n >= 24) {
                const int64_t avg = sumNs / n;
                sumNs = 0;
                n = 0;
                if (!tight && avg >= 26000000) {
                    tight = true;
                    newLimit = 1;
                } else if (tight && avg <= 20000000) {
                    tight = false;
                    newLimit = sMaxInFlight + 1;
                }
                if (newLimit && changes < 40) {
                    ++changes;
                    port_log("gx: the back end takes %.1f ms a frame: %s", avg / 1e6,
                             tight ? "it is the bottleneck, so the queue is cut to one frame" : "it keeps up, so the queue is open again");
                }
            }
        }
        const unsigned long long shown = sFramesShown.fetch_add(1) + 1;
        port_input_latency_frame_done(shown);
        if (sPerfSeconds > 0) {
            const int64_t execNs = PORT_RUNTIME_DIAGNOSTICS ? port_host_time_ns() - tExec : 0;
            const vgx::backend::Stats st = vgx::backend::GetStats();
            PerfFrame(execNs, st.drawCallsThisFrame);
            // A back-end frame of 45 ms or more (the game thread then waits for it, "gx-finish-wait"):
            // what it did that costs time. Counters are the changes since the previous frame.
            static vgx::backend::Stats prev;
            static unsigned slowLogged = 0;
            if (execNs >= 45000000 && slowLogged < 300) {
                ++slowLogged;
                port_log("back end: slow frame %.0f ms: %u shader compiles, %u program links (%.0f ms of shader work), %u KiB of textures, %u draws, %u textures live",
                         execNs / 1e6, st.shaderCompiles - prev.shaderCompiles, st.programLinks - prev.programLinks, st.shaderWorkMs - prev.shaderWorkMs,
                         static_cast<unsigned>((st.textureBytes - prev.textureBytes) / 1024), st.drawCallsThisFrame, st.textures);
            }
            prev = st;
        }
#if defined(__vita__)
        port_vgx_vita_frame_done();
#endif
        if (cap) {
            int w = 0, h = 0;
            const std::vector<uint8_t>& px = vgx::backend::LastDisplayCopy(w, h);
            if (w > 0 && h > 0) {
                // The copy is top row first; the PNG writer takes GL order (bottom row first).
                std::vector<uint8_t> flipped(px.size());
                const size_t row = static_cast<size_t>(w) * 4;
                for (int y = 0; y < h; ++y) {
                    memcpy(flipped.data() + static_cast<size_t>(y) * row, px.data() + static_cast<size_t>(h - 1 - y) * row, row);
                    for (int x = 0; x < w; ++x) flipped[static_cast<size_t>(y) * row + x * 4 + 3] = 255; // EFB alpha is not part of the picture
                }
                port_headless_write_png(sCapPath.c_str(), flipped.data(), w, h);
            }
        }
        job->clear();
        if (PORT_RUNTIME_DIAGNOSTICS) {
            sBackStage.store(0, std::memory_order_relaxed);
            sBackSince.store(port_host_time_ns(), std::memory_order_relaxed);
        }
        lk.lock();
        if (newLimit) sQueueLimit = newLimit;
        sJobs.erase(sJobs.begin());
        sFree.push_back(job);
        if (cap) { sCapWanted = false; sCapDone = true; sCapFrame = shown; }
        lk.unlock();
        sCv.notify_all();
    }
}

void* RenderEntry(void*) {
    RenderMain();
    return nullptr;
}

// wait: return when the render thread has its graphics up (otherwise at once: the front end can fill its frames meanwhile, they wait in the
// queue, and FrameSink watches for a thread that failed to start).
bool EnsureThread(bool wait = true) {
    std::unique_lock<std::mutex> lk(sMtx);
    if (!sThreadMade) {
        sThreadMade = true;
        for (unsigned i = 0; i < sMaxInFlight + 1; ++i) sFree.push_back(new vgx::CmdBuf());
        // 4 MiB of stack: on the console the system's shader compiler runs on this thread.
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 4u << 20);
        if (pthread_create(&sThread, &attr, &RenderEntry, nullptr) != 0) {
            pthread_attr_destroy(&attr);
            return false;
        }
        pthread_attr_destroy(&attr);
    }
    if (!wait) return true;
    sCv.wait(lk, [] { return sStarted; });
    return sStartOk;
}

// The front end finished a frame: swap its buffer for an empty one and queue it.
void FrameSink(vgx::CmdBuf& buf) {
    std::unique_lock<std::mutex> lk(sMtx);
    while (!sCv.wait_for(lk, std::chrono::milliseconds(20), [] { return !sFree.empty() && sJobs.size() < sQueueLimit; })) {
        // (time limit: see RenderMain)
        if (sStarted && !sStartOk) port_fatal("vgx: no graphics context for the renderer's back end");
    }
    vgx::CmdBuf* b = sFree.back();
    sFree.pop_back();
    std::swap(*b, buf);
    sJobs.push_back(b);
    sFramesQueued.fetch_add(1);
    lk.unlock();
    sCv.notify_all();
}

// A display copy is the end of the game's frame.
void DisplayCopy() {
    vgx::split::FinishFrame(vgx::Pending()); // (front-end split: the frame's records join it, and count in the statistics read here)
    const vgx::stats::Frame fs = vgx::stats::Cur();  // (FrontPresent resets it)
    vgx::FrontPresent(false, nullptr);
    if (PORT_RUNTIME_DIAGNOSTICS && sPerfSeconds > 0) {
        sPlCpuUs.store(port_thread_cpu_us(), std::memory_order_relaxed);
        sPlRecords.fetch_add(fs.drawCalls, std::memory_order_relaxed);
        sPlPrims.fetch_add(fs.prims, std::memory_order_relaxed);
    }
    PORT_QEMU_FRAME_STATS(static_cast<uint32_t>(sFramesQueued.load()), fs.prims, fs.verts, fs.tris, fs.drawCalls, fs.streams);
    PORT_QEMU_MARK(static_cast<uint32_t>(sFramesQueued.load()));  // armhf/qemu harness: end of a game frame
    // GXV_STATS=<file>: the renderer's feature-usage report (what the game asked for that is
    // approximated or missing, texture and copy formats, cache counters), rewritten now and then.
    static const char* const statsPath = vgx::diag::Env("GXV_STATS");
    static unsigned n = 0;
    if (statsPath && *statsPath && (++n % 300) == 0) vgx::stats::Dump(statsPath);
}

#if !defined(__vita__)
// GXV_TEXDUMP=<folder> (desktop): every texture the renderer uploads as RGBA8, as
// <hash>_<w>x<h>_f<format>.png. The hash is the one the handheld's picture replacement keys on
// (run with GXV_DXT1=0 to see the compressed ones too).
void TexDump(const vgx::TexUpload& up, uint32_t gxFormat, uint64_t) {
    static const char* const dir = getenv("GXV_TEXDUMP");
    if (!dir || !*dir || up.format != vgx::TF_RGBA8 || up.levels.empty()) return;
    const uint32_t w = up.width, h = up.height;
    if (up.levels[0].size() < static_cast<size_t>(w) * h * 4) return;
    std::vector<uint8_t> flipped(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) memcpy(flipped.data() + static_cast<size_t>(y) * w * 4, up.levels[0].data() + static_cast<size_t>(h - 1 - y) * w * 4, static_cast<size_t>(w) * 4);
    char path[512];
    snprintf(path, sizeof path, "%s/%016llx_%ux%u_f%u.png", dir, static_cast<unsigned long long>(vgx::LastUploadHash()), w, h, gxFormat);
    port_headless_write_png(path, flipped.data(), static_cast<int>(w), static_cast<int>(h));
}
#endif

// The front-end split's finishing thread (vgx_fesplit.h): on the back end's core by default, which has CPU to spare
// in every scene measured, one priority step above the renderer's threads (Vita).
//   GXV_SPLIT_PRIO=<n>  (Vita) its scheduling priority: 0 (default) one step above the renderer's threads (a lower number; they
//                       run at the library's default unless GXV_PRIO says otherwise), 1 the same as theirs, 0x40-0xBF that one.
//                       Threads of one priority do not preempt each other, so at the back end's priority the finishing thread
//                       only runs while the back end waits (for a frame, for the GPU), and the player waits for it at the end
//                       of its frame whenever the back end was busy (in Vita3K, where the back end takes 200-500 ms a frame, the
//                       player waited 10-70 ms a frame). One step above, it runs as soon as it has work, in short bursts (a few
//                       dozen draws at a time) that delay the back end's submission by as much.
#if defined(__vita__)
extern "C" int sceKernelChangeThreadPriority(int thid, int priority);
extern "C" int sceKernelGetThreadCurrentPriority(void);
#endif
void SplitThreadInit(int core) {
    PORT_QEMU_THREAD("vgx-finish");
#if defined(__vita__)
    const int pin = core >= 0 && core <= 2 ? port_vita_pin_core(core) : 0;
    port_thread_class(3);
    long prio = vgx::EnvInt("GXV_SPLIT_PRIO", 0);
    if (prio == 0) prio = sceKernelGetThreadCurrentPriority() - 1; // (the renderer's threads have the class this thread has now)
    const int rc = prio >= 0x40 && prio <= 0xBF ? sceKernelChangeThreadPriority(0, static_cast<int>(prio)) : 0;
    port_log("gx: front-end split: finishing thread on core %d%s, priority 0x%X%s", core, pin < 0 ? " (pinning refused)" : "",
             sceKernelGetThreadCurrentPriority(), rc < 0 ? " (GXV_SPLIT_PRIO refused)" : "");
#else
    (void)core;
    port_thread_class(3);
#endif
}

void RegisterMemory() {
    // What the display-list and texture caches may keep decodes of: the console's two RAM
    // banks and the executable's own data (the game keeps some display lists and tables there).
    for (unsigned i = 0;; ++i) {
        void *base = nullptr, *alias = nullptr;
        uint32_t physical = 0, size = 0;
        int kind = 0;
        if (!port_memory_region(i, &base, &alias, &physical, &size, &kind)) break;
        if (kind <= 2) vgx::hle::RegisterGuestRam(base, size); // Mem1, Mem2, Image
    }
}

} // namespace

// The game's GX markers (PORT_GX_MARK_*, platform/include/port/compat.h) for the vgx front end, in the GX stream at the point
// the game calls them: a write of BP register 0xFF, which the console's GPU does not have (vgx_state.cpp WriteBP). Only the
// HUD's markers, and only with GXV_CHARAA on (vgx_draw.cpp, character edge smoothing: the HUD is never smoothed); otherwise
// nothing is written and the stream is the game's own, byte for byte. Game thread (the GX library's FIFO writer).
extern "C" void port_vgx_marker(uint32_t kind) {
    if ((kind != 1u && kind != 2u) || vgx::CharaAAMode() == 0) return; // (PORT_GX_MARK_HUD_BEGIN, _END)
    __PortGXFifo_u8(0x61);
    __PortGXFifo_u32((0xFFu << 24) | kind);
}

// The front end's CPU time at its last display copy (perf.cpp's slow-turn line compares it across a turn).
extern "C" int64_t port_vgx_player_cpu_us(void) { return sPlCpuUs.load(std::memory_order_relaxed); }
extern "C" int port_vgx_active(void) { return sActive.load(std::memory_order_relaxed) ? 1 : 0; }
// Pictures the front end has handed to the back end that it has not finished drawing (the one being
// drawn included): the frame-skip rule in port/perf.cpp reads it.
extern "C" unsigned port_vgx_backend_pending(void) {
    const unsigned long long queued = sFramesQueued.load(std::memory_order_relaxed), shown = sFramesShown.load(std::memory_order_relaxed);
    return queued > shown ? static_cast<unsigned>(queued - shown) : 0u;
}
// Frames the game has finished (display copies), whichever renderer is selected.
#if defined(PORT_GX_VGX_ONLY)
// The build's only renderer (PS Vita): what the legacy recorder's files otherwise define.
extern "C" uint64_t port_gx_recording_frame(void) { return sFramesQueued.load(); }
extern "C" void port_gx_marker(uint32_t kind) { port_vgx_marker(kind); }
// The game's per-frame presentation hints (GameScene.cpp). One is of use on a console: bit 1
// of the flags word (PORT_GX_CAMERA_POINTER_UI) says the Wii pointer is driving a 2D menu
// (file select, pause, yes/no, the storybook, the galaxy domes); the pad code then lets the
// left stick move the pointer (vita/runtime/input.cpp).
static std::atomic<int64_t> sPointerMenuAt{0};
extern "C" void port_gx_camera(const float* camera) {
    uint32_t flags = 0;
    memcpy(&flags, &camera[25], sizeof flags);
    sPointerMenuAt.store((flags & 2u) ? port_host_time_ns() : 0, std::memory_order_relaxed);
}
extern "C" int port_pointer_on_menus(void) {
    const int64_t at = sPointerMenuAt.load(std::memory_order_relaxed);
    return at != 0 && port_host_time_ns() - at < 300000000; // (hints come with drawn frames: a few may be skipped)
}
extern "C" void port_gx_present_xfb(void*, int) {}
#else
extern "C" uint64_t port_gx_recording_frame(void);
#endif
extern "C" unsigned long long port_game_frames(void) {
    return sActive.load(std::memory_order_relaxed) ? sFramesQueued.load() : port_gx_recording_frame();
}
extern "C" unsigned long long port_vgx_frames(void) { return sFramesShown.load(); }

// ------------------------------------------------------------------ player thread
// The front end (stream parser, vertex and texture decoding, draw merging) costs more than the
// game itself does per frame, so it gets a processor core of its own: the game thread writes
// GX commands into a staging segment and publishes its write position at the GX library's
// flush points (and every few dozen KiB in between); the player consumes behind it. The
// game's frame loop draws first and then runs its logic, and only waits for the "draw done"
// interrupt at the end of the frame, which the player raises when it reaches that command:
// the two overlap for most of a frame, as the console's CPU and GPU did. Everything the
// player reads from the game's memory is read when it parses the command that refers to it,
// so "draw done" still means the game may change that memory again.
//
// A publish is two atomic operations (the game flushes some 600 times a frame: every
// material's display list is recorded with the FIFO pointed elsewhere, and re-pointing it
// flushes). The player is only woken through the kernel when a synchronisation command or a
// good amount of bytes waits for it. There are two segments: the game moves to the other one
// when the player has caught up, so the positions never have to be changed under its feet.
namespace {
constexpr size_t kWakeBytes = 16u << 10;
std::atomic<bool> sPlayerOn{false};
uint8_t* sSegBase[2] = {nullptr, nullptr};
size_t sSegSize = 0;
// epoch << 32 | bytes published in segment (epoch & 1). Written by the game thread.
std::atomic<uint64_t> sPubState{0};
// Written by the player: the bytes it has consumed, then epoch << 32 | the published position
// it has worked up to. When the second equals sPubState the player has nothing to do, and the
// first is stable.
std::atomic<uint32_t> sPlRd{0};
std::atomic<uint64_t> sPlDone{0};
std::atomic<bool> sPlSleeping{false};
std::mutex sPlMtx;
std::condition_variable sPlCv;
pthread_t sPlThread;
// ARM harness, PETARI_VCLOCK_FE=1: the player's unfinished work holds the virtual clock (port_vclock_external_*), so the game
// sees the front end as infinitely fast and plays the same frames whatever the front end costs (instruction-count A/B runs
// otherwise land in different scenes: a slower front end made the game skip retraces). sFeBusy: the clock is held.
std::atomic<bool> sFeBusy{false};
inline bool VclockFe() {
#if defined(PORT_ARMHF_HARNESS)
    static const bool on = port_vclock_enabled() && getenv("PETARI_VCLOCK_FE") && atoi(getenv("PETARI_VCLOCK_FE")) != 0;
    return on;
#else
    return false;
#endif
}
inline void FeHold() { if (VclockFe() && !sFeBusy.exchange(true)) port_vclock_external_begin(); }
inline void FeRelease() { if (VclockFe() && sFeBusy.exchange(false)) port_vclock_external_end(); }
// game thread only
uint32_t sGmEpoch = 0;
size_t sGmPubOff = 0, sGmWakeOff = 0;

void* PlayerMain(void*) {
#if defined(__vita__)
    port_vita_pin_core(2);
#endif
    port_thread_class(3);
    uint32_t epoch = 0;
    size_t rd = 0, seen = 0;
    for (;;) {
        const uint64_t st = sPubState.load(std::memory_order_acquire);
        const uint32_t e = static_cast<uint32_t>(st >> 32);
        const size_t pub = static_cast<uint32_t>(st);
        if (e != epoch) { // the game moved on to the other segment (what was left of this one went with it)
            epoch = e;
            rd = seen = 0;
        }
        if (pub == seen) {
            sPlRd.store(static_cast<uint32_t>(rd), std::memory_order_release);
            sPlDone.store((static_cast<uint64_t>(epoch) << 32) | seen, std::memory_order_release);
            if (VclockFe()) {
                FeRelease();
                if (sPubState.load(std::memory_order_acquire) != st) { FeHold(); continue; } // (published meanwhile)
            }
            // More usually follows at once while the game is drawing: look again a few
            // times before going to sleep (a wake-up costs the game thread a kernel call).
            bool more = false;
            for (int i = 0; i < 400 && !more; ++i) more = sPubState.load(std::memory_order_acquire) != st;
            if (more) continue;
            sPlSleeping.store(true);
            if (sPubState.load() != st) { sPlSleeping.store(false); continue; }
            {
                std::unique_lock<std::mutex> lk(sPlMtx);
                // (the time limit bounds how long bytes that were published without a wake-up can wait)
                sPlCv.wait_for(lk, std::chrono::milliseconds(2), [&] { return sPubState.load() != st; });
            }
            sPlSleeping.store(false);
            continue;
        }
        seen = pub;
        rd += vgx::ProcessStream(sSegBase[epoch & 1] + rd, static_cast<uint32_t>(pub - rd), false);
    }
    return nullptr;
}

inline void WakePlayer() {
    // Pair with the player's sleeping-store/publication-load: release publication
    // alone does not order this store-to-load handshake on ARM.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (sPlSleeping.load()) {
        std::lock_guard<std::mutex> lk(sPlMtx);
        sPlCv.notify_one();
    }
}
} // namespace

extern "C" int port_vgx_player(void) { return sPlayerOn.load(std::memory_order_relaxed) ? 1 : 0; }

extern "C" void port_vgx_stream_setup(uint8_t* seg0, uint8_t* seg1, size_t size) {
    sSegBase[0] = seg0;
    sSegBase[1] = seg1;
    sSegSize = size;
}

// flags: 1 the segment is full (the call waits for the player and moves to the other segment),
// 2 the bytes end with a synchronisation command (draw done, draw-sync token).
// Returns where writing continues: wr, or a position in the other segment.
extern "C" uint8_t* port_vgx_publish(uint8_t* wr, int flags) {
    uint8_t* const base = sSegBase[sGmEpoch & 1];
    const size_t off = static_cast<size_t>(wr - base);
    const uint64_t state = (static_cast<uint64_t>(sGmEpoch) << 32) | off;
    if (off != sGmPubOff) {
        sGmPubOff = off;
        FeHold(); // (before the bytes are visible: the player releases only after it has seen them)
        sPubState.store(state, std::memory_order_release);
    }
    const bool full = (flags & 1) != 0;
    if (full) {
        // Wait until the player has done everything published (what it leaves is a command
        // that is not complete yet).
        const int64_t t0 = PORT_RUNTIME_DIAGNOSTICS && sPerfSeconds > 0 ? port_host_time_ns() : 0;
        while (sPlDone.load(std::memory_order_acquire) != state) {
            WakePlayer();
            // (real time, not port_host_sleep_ns: under the ARM harness's virtual clock that one
            // only returns when every game thread is idle, and this is a game thread waiting)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        if (PORT_RUNTIME_DIAGNOSTICS && sPerfSeconds > 0) sGmWaitNs.fetch_add(port_host_time_ns() - t0, std::memory_order_relaxed);
    }
    // Move to the other segment when the player has caught up and this one is half used.
    if ((full || off > sSegSize / 2) && sPlDone.load(std::memory_order_acquire) == state) {
        const size_t rd = sPlRd.load(std::memory_order_acquire);
        const size_t left = off - rd;
        uint8_t* const next = sSegBase[(sGmEpoch + 1) & 1];
        if (left) memcpy(next, base + rd, left);
        ++sGmEpoch;
        sGmPubOff = sGmWakeOff = left;
        if (left) FeHold();
        sPubState.store((static_cast<uint64_t>(sGmEpoch) << 32) | left, std::memory_order_release);
        return next + left;
    }
    if ((flags & 2) || off - sGmWakeOff >= kWakeBytes) {
        sGmWakeOff = off;
        WakePlayer();
    }
    return wr;
}

// One line: where the renderer's host memory is (the frame buffers keep what their largest frame
// needed; the display-list cache keeps decoded vertices; the game's `new` is everything of it).
// Meant for the status log and the ends of a fast-forward; the figures are read without stopping
// the player, so they are approximate.
extern "C" unsigned port_host_new_live_kib(void);
extern "C" void port_vgx_diagnostic_csv_pause(int selecting) {
    if (!PORT_RUNTIME_DIAGNOSTICS || !selecting) return;
    vgx::stats::ExportFrameLog();
    vgx::backend::ExportCountLog();
}

extern "C" void port_vgx_census_log(const char* when) {
    if (!sActive.load(std::memory_order_relaxed)) return;
    size_t ring = 0, vtx = 0, other = 0;
    unsigned bufs = 0;
    {
        std::lock_guard<std::mutex> lk(sMtx);
        for (const vgx::CmdBuf* b : sFree) { ring += b->HostBytes(); vtx += b->vtx.capacity(); ++bufs; }
        for (const vgx::CmdBuf* b : sJobs) { ring += b->HostBytes(); vtx += b->vtx.capacity(); ++bufs; }
    }
    const size_t pending = vgx::Pending().HostBytes();
    other = ring - vtx;
    port_log("vgx host memory %s: frame buffers %u KiB in %u (vertex arenas %u KiB, the rest %u KiB), the front end's own frame %u KiB, display-list cache %u KiB "
             "(+ %u KiB static in the back end), the game's `new` %u KiB",
             when, static_cast<unsigned>(ring / 1024), bufs, static_cast<unsigned>(vtx / 1024), static_cast<unsigned>(other / 1024),
             static_cast<unsigned>(pending / 1024), static_cast<unsigned>(vgx::DlCacheLiveBytes() / 1024),
             static_cast<unsigned>(vgx::DlCacheStaticBytes() / 1024), port_host_new_live_kib());
}

// A stall report: the hand-over state between the game thread, the player and the back end.
extern "C" void port_vgx_debug_dump(void) {
    if (!PORT_RUNTIME_DIAGNOSTICS) return;
    const uint64_t pub = sPubState.load(), done = sPlDone.load();
    size_t jobs = 0, freeBufs = 0;
    {
        std::lock_guard<std::mutex> lk(sMtx);
        jobs = sJobs.size();
        freeBufs = sFree.size();
    }
    static const char* const kStage[] = {"waiting for a frame", "executing a frame", "finishing a frame", "?"};
    const int stage = sBackStage.load();
    const double stageMs = (port_host_time_ns() - sBackSince.load()) / 1e6;
    port_log("vgx: published epoch %u offset %u; player done epoch %u offset %u, read %u, %s; frames queued %llu, shown %llu; back end: %u job(s) waiting, %u free buffer(s), queue limit %u, %s for %.0f ms",
             static_cast<unsigned>(pub >> 32), static_cast<unsigned>(pub), static_cast<unsigned>(done >> 32), static_cast<unsigned>(done), sPlRd.load(),
             sPlSleeping.load() ? "asleep" : "awake", sFramesQueued.load(), sFramesShown.load(), static_cast<unsigned>(jobs), static_cast<unsigned>(freeBufs),
             sQueueLimit, kStage[stage >= 0 && stage < 3 ? stage : 3], stageMs);
}

extern "C" size_t port_vgx_consume(const uint8_t* data, size_t len) {
    // (the caller holds a PortHostAllocScope: the front end allocates from the host heap)
    return vgx::ProcessStream(data, static_cast<uint32_t>(len), false);
}

extern "C" void port_vgx_notify_write(const void* p, size_t n) {
    if (!sActive.load(std::memory_order_relaxed) || !n) return;
    vgx::hle::NotifyGuestWrite(p, static_cast<uint32_t>(n > 0xFFFFFFFFu ? 0xFFFFFFFFu : n));
}

extern "C" unsigned long long port_vgx_capture(const char* path, int) {
    std::unique_lock<std::mutex> lk(sMtx);
    sCapPath = path;
    sCapDone = false;
    sCapWanted = true;
    if (!sCv.wait_for(lk, std::chrono::seconds(5), [] { return sCapDone; })) {
        sCapWanted = false;
        return 0;
    }
    return sCapFrame;
}

extern "C" void port_gx_select_backend(void) {
#if !defined(PORT_GX_VGX_ONLY)
    const char* want = getenv("PETARI_BACKEND");
    if (!(want && !strcmp(want, "vgx"))) {
        port_gx_use_recorder_backend();
        return;
    }
#endif
    PortHostAllocScope hostAlloc;
#if defined(__vita__) || defined(PORT_ARMHF_HARNESS)
    const long inflightDefault = 2, skipDefault = 0;
#else
    const long inflightDefault = 8, skipDefault = 1;
#endif
    sMaxInFlight = static_cast<unsigned>(vgx::EnvInt("GXV_INFLIGHT", inflightDefault));
    if (sMaxInFlight < 1) sMaxInFlight = 1;
    if (sMaxInFlight > 16) sMaxInFlight = 16;
    sQueueLimit = sMaxInFlight + 1;
#if defined(__vita__)
    // (GXV_INFLIGHT set by hand: that number is kept; GXV_ADAPT_DEPTH=0 turns the adaptation off)
    sAdaptDepth = !getenv("GXV_INFLIGHT") && vgx::EnvInt("GXV_ADAPT_DEPTH", 1) != 0;
#endif
    sSkipWhenBehind = vgx::EnvInt("GXV_SKIP_BEHIND", skipDefault) != 0;
#if defined(__vita__)
    sPerfSeconds = PORT_LOGGING ? static_cast<int>(vgx::EnvInt("GXV_PERF", 5)) : 0;
    StartFreezeWatch();
#else
    sPerfSeconds = PORT_LOGGING ? static_cast<int>(vgx::EnvInt("GXV_PERF", 0)) : 0;
#endif
    vgx::SetTexturePatchHook(port_handheld_texture, port_handheld_texture_needs_rgba);
    // HD textures for Mario, the main characters and key items (GXV_HDTEX, vgx_hdtex.h).
    vgx::hdtex::Install(&port_log);
#if !defined(__vita__)
    if (const char* d = getenv("GXV_TEXDUMP"); d && *d) vgx::SetTextureDumpHook(TexDump);
#endif
#if defined(__vita__)
    // vitaGL's start-up takes 2.3 s of the render thread's time and nothing in the game's boot needs it: the thread is started and the
    // game goes on (GXV_RENDER_ASYNC=0: wait for it here). What the front end takes from the host is set first, on this thread.
    port_vgx_vita_prepare();
    const bool asyncStart = vgx::EnvInt("GXV_RENDER_ASYNC", 1) != 0;
    if (!EnsureThread(!asyncStart)) port_fatal("vgx: no graphics context for the renderer's back end");
    if (asyncStart) port_log("gx: the render thread starts the graphics while the game boots");
#else
    if (!EnsureThread()) port_fatal("vgx: no graphics context for the renderer's back end");
#endif
    vgx::ResetState();
    vgx::GuestHooks hooks;
    hooks.resolve = &Resolve;
    hooks.peFinish = &PeFinish;
    hooks.peToken = &PeToken;
    hooks.displayCopy = &DisplayCopy;
    vgx::SetGuestHooks(hooks);
    vgx::SetFrameSink(&FrameSink);
    vgx::split::SetThreadInit(&SplitThreadInit);
    if (const char* sp = vgx::diag::Env("GXV_STATS"); sp && *sp) vgx::stats::SetDetailed(true);
    // GXV_TRACK=0: no memory tracking (every display list decoded and every bound texture
    // hashed on each use): to tell a caching fault from a rendering one.
    if (vgx::EnvInt("GXV_TRACK", 1)) RegisterMemory();
    // The game's EFB: 640 x 456 (every render mode the game sets up uses that height).
    vgx::g.efbW = 640;
    vgx::g.efbH = static_cast<uint16_t>(vgx::EnvInt("GXV_EFB_HEIGHT", 456));
    vgx::QueueSetTarget();
    // GXV_PLAYER=0: the front end runs on the game thread, inside the GX flush (to tell a fault
    // of the hand-over from one of the renderer).
    if (vgx::EnvInt("GXV_PLAYER", 1)) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1u << 20);
        if (pthread_create(&sPlThread, &attr, &PlayerMain, nullptr) == 0) sPlayerOn.store(true);
        pthread_attr_destroy(&attr);
    }
    sActive.store(true);
    port_log("gx: vgx renderer selected (EFB %ux%u, %u frame(s) in flight%s, front end on %s)", vgx::g.efbW, vgx::g.efbH, sMaxInFlight,
             sAdaptDepth ? ", cut to one while the back end is the bottleneck" : "", sPlayerOn.load() ? "its own thread" : "the game thread");
}

#else // !PORT_GX_VGX
#include "vgx_galaxy.h"
extern "C" void port_gx_use_recorder_backend(void);
extern "C" void port_gx_select_backend(void) { port_gx_use_recorder_backend(); }
extern "C" int port_vgx_active(void) { return 0; }
extern "C" size_t port_vgx_consume(const uint8_t*, size_t len) { return len; }
extern "C" int port_vgx_player(void) { return 0; }
extern "C" void port_vgx_stream_setup(uint8_t*, uint8_t*, size_t) {}
extern "C" uint8_t* port_vgx_publish(uint8_t* wr, int) { return wr; }
extern "C" void port_vgx_notify_write(const void*, size_t) {}
extern "C" unsigned long long port_vgx_capture(const char*, int) { return 0; }
extern "C" unsigned long long port_vgx_frames(void) { return 0; }
extern "C" void port_vgx_debug_dump(void) {}
extern "C" void port_vgx_marker(uint32_t) {}
extern "C" uint64_t port_gx_recording_frame(void);
extern "C" unsigned long long port_game_frames(void) { return port_gx_recording_frame(); }
#endif

// See port/compat.h.
extern "C" int port_handheld(void) {
#if defined(__vita__) || defined(PORT_ARMHF_HARNESS)
    return 1;
#else
    static const int on = [] { const char* e = getenv("PETARI_HANDHELD"); return e && atoi(e) != 0 ? 1 : 0; }();
    return on;
#endif
}
