// GXV_DRAWPIPE=3 / 4 (dpengine, 11 Nov 2026): the draw thread of the pipelined game thread, and the turn's hand-over and join.
// Header only (the game's glue is decomp/src/Game/Scene/SceneFunction.cpp, the host test tests/draw_pipe.cpp); DESIGN.md of
// /home/claude/g23/agents/dpengine has the plan, port/draw_pipe.h the modes.
//
// Two threads: G, the game thread (core 0), and D, the draw thread (GXV_DRAWPIPE_CORE, default 1). One job at a time:
//   mode 3  the whole serial draw. G hands it over and WAITS; D draws as the game thread would have (the game's glue lends it the
//           game thread's identity for the job: the "frame thread" mark and the guest OS's current thread).
//   mode 4  G runs the draw with the GX library recording (the capture interface below), hands the recording to D and goes on
//           with update(); D replays it, drawing each J3D pass from the frozen frame at its token; G joins before calcAnim().
// The draw has three parts (PipeHost): its begin (beginRender), its middle (draw()) and its end (endRender: the copy to the
// display buffer). Only the middle is recorded: the begin and the end make GX calls no recording can hold (the capture layer ends
// a capture at them), so in mode 4 G makes the begin itself before the recording starts and the end itself after the join, when
// the stream holds the whole picture. The stream's order is today's: begin, middle, end, then whatever the logic sends.
//
// How a freeze between the two threads is ruled out (a two-thread deadlock froze the console in another feature):
//   1. Waiting goes one way only. G waits for D (the join). D never waits for G: everything on D's path that could wait for the
//      game thread asks Pipe::OnDrawThread() first and returns (GameEnter below; the guest OS's entry points, os_thread.cpp).
//   2. D takes no lock that G can hold across a wait, and holds none itself while it waits: the engine has no mutex at all (two
//      counting semaphores and one atomic state word); D's only waits are the semaphore it sleeps on between jobs and whatever
//      the GX library waits for (the front end, when a staging segment is full: the wait the game thread makes today).
//   3. G never waits blind in mode 4: the join is a loop of timed waits (GXV_DRAWPIPE_SLICE_US, default 1000) with the guest's
//      safe point between them (PipeHost::Poll: interrupts are taken, other guest threads run), so nothing D's progress could
//      depend on through the guest stands still while G waits. No spin: a waiting G is asleep in the kernel.
//   4. A job D has not STARTED when G reaches the join is taken back (an atomic claim: exactly one of the two runs it) and run by
//      G in today's order. So a D that does not get its core costs the turn its gain, never its picture.
//      A join made from inside the join (the safe point took an interrupt whose handler uses GX) is complete when it returns;
//      the outer one sees that and returns too.
//   5. The watchdog: a join that has waited GXV_DRAWPIPE_WATCHDOG_MS (default 200) logs once and turns the pipeline off for the
//      session; this turn is finished by waiting for D as before (a started job cannot change hands: D is inside GX calls), with
//      a line every 5 s that says where D is. From the next turn on every draw is serial.
// The states of the job: 0 idle -> 1 armed -> 2 running (D) -> 3 done;  1 armed -> 4 taken (G).
#pragma once

#include "port/draw_pipe.h"  // (the reasons a turn is serial)
#include <cstdlib>
#include "port/pipe25.h"  // (GXV_PIPE25: Census, VerifyStats; class Late below)
#include "port/par_spec.h"  // (NowUs, CoreMask, FpModeGet / FpModeSet)
#include "port/core3_budget.h"  // (core3-38: the core-3 meter)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#if !defined(__vita__) || defined(PORT_ARMHF_HARNESS)  // (armhf harness: Linux threads)
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

extern "C++" {
namespace port {
namespace drawpipe {

// ------------------------------------------------------------------------------------------------- the capture interface
// platform/include/port/gx_capture.h (the GX capture-and-replay layer, another engineer's: platform/src/gx/gx_capture.cpp), held
// as pointers so that the game binds the layer's functions when they are linked (weak: absent = "capture unavailable", every draw
// is serial) and the host test its own recorder. The four of the agreed interface, and three of the layer's additions:
//   setHandler  the function that executes a token when the layer has to do it itself: at an abandon (what was recorded is executed
//               at once on the recording thread, the passes at their tokens), and for every token after an abandon (the pass then
//               runs where it stands). So after an abandon the engine goes on leaving tokens and the picture is the serial one.
//   abandon     the engine's own abandon: something the recording cannot hold is about to happen on the recording thread (a pass
//               that must be drawn from the live lists, a fur draw, a list written outside the flushes).
//   tokenKeep   a token whose pass leaves the viewport, the projection and the scissor as they are (a J3D pass never sets them),
//               so that the layer's mirror goes on answering their getters after it instead of abandoning. Optional.
// Without setHandler or abandon a recorded pass could be lost, so mode 4 does not engage (the "drawpipe:" start line says so).
struct CaptureApi {
    bool (*begin)(void) = nullptr;
    void (*token)(uint32_t id) = nullptr;
    void (*tokenKeep)(uint32_t id, uint32_t keep) = nullptr;
    void* (*end)(void) = nullptr;
    void (*replay)(void* capture, void (*token)(uint32_t id, void* ctx), void* ctx) = nullptr;
    void (*setHandler)(void (*token)(uint32_t id, void* ctx), void* ctx) = nullptr;
    void (*abandon)(const char* why) = nullptr;
    bool (*open)(void) = nullptr;  // (optional) a capture is open on the calling thread: false after an abandon
    bool Complete() const { return begin && token && end && replay && setHandler && abandon; }
};
// (PORT_GXCAP_KEEP_*; kKeepSerial is the engine's proposal, GX-HOOKS.diff: "the token's pass leaves the draw serial alone: no
// primitive, no list call, no frame buffer copy", so that the layer's mirror of the serial stays good across a pass that draws
// nothing; a layer that does not know the bit ignores it)
enum : uint32_t { kKeepViewport = 1, kKeepProjection = 2, kKeepScissor = 4, kKeepSerial = 8 };

// ------------------------------------------------------------------------------------------------- a semaphore with a timed wait
class TimedSema {
public:
#if defined(__vita__) && !defined(PORT_ARMHF_HARNESS)  // (armhf harness: Linux threads, no psp2 kernel headers)
    bool Init(const char* name) {
        id = sceKernelCreateSema(name, 0, 0, 0x7FFF, nullptr);
        return id >= 0;
    }
    void Signal() { sceKernelSignalSema(id, 1); }
    // true: taken. false: `us` microseconds passed (0: no limit).
    bool Wait(uint32_t us) {
        SceUInt t = us;
        return sceKernelWaitSema(id, 1, us ? &t : nullptr) >= 0;
    }

private:
    SceUID id = -1;
#else
    bool Init(const char*) { return true; }
    void Signal() {
        std::lock_guard< std::mutex > lock(m);
        ++count;
        cv.notify_one();
    }
    bool Wait(uint32_t us) {
        std::unique_lock< std::mutex > lock(m);
        if (us == 0) {
            cv.wait(lock, [this] { return count > 0; });
        } else if (!cv.wait_for(lock, std::chrono::microseconds(us), [this] { return count > 0; })) {
            return false;
        }
        --count;
        return true;
    }

private:
    std::mutex m;
    std::condition_variable cv;
    unsigned count = 0;
#endif
};

// ------------------------------------------------------------------------------------------------- the thread
class Worker {
public:
    typedef void (*Fn)(void*);
    enum : uint32_t { kIdle = 0, kArmed = 1, kRunning = 2, kDone = 3, kTaken = 4 };

    // core: 0-3 (3 falls back to 1 without the fourth core). priority: the console's (0x40 highest .. 0xBF lowest); 0: one step
    // BELOW the calling thread's (the game thread's 0x9F gives 0xA0: under the back end's render thread and the game's audio
    // thread, which are made at the game thread's priority and share core 1, above the 0xBF background work). A pthread, as the
    // guest's threads are, so that thread-local state behaves on it as it does on them. false: no thread could be created.
    // The thread reports in BEFORE it moves itself to its core and priority: a thread placed below busy threads on a busy core
    // may not run for seconds (the console's first run: the boot's shader work on core 1 kept it off for more than the two
    // seconds the starter then waited, and the starter wrote it off for the session). A thread that exists is "ready": a job
    // it does not start is taken back by the game thread (Take), so a late or starved thread costs the gain, never the picture.
    bool Start(int core, int priority, uint32_t stackBytes) {
        if (started) {
            return ok;
        }
        started = true;
        if (!work.Init("smg_draw_work") || !done.Init("smg_draw_done") || !up.Init("smg_draw_up")) {
            return ok = false;
        }
#if defined(__vita__) && !defined(PORT_ARMHF_HARNESS)  // (armhf harness: Linux threads, no psp2 kernel headers)
        if (priority <= 0) {
            // (0: one step below the caller's; negative, pipe27: the caller's own)
            const int step = priority < 0 ? 0 : 1;
            const int mine = sceKernelGetThreadCurrentPriority();
            priority = mine >= 0x40 && mine < 0xBF ? mine + step : 0xA0;
        }
        wantMask = par::CoreMask(core, &effectiveCore);
#else
        (void)par::CoreMask(core, &effectiveCore);
#endif
        wantPriority = priority;
        stackSize = stackBytes < (256u << 10) ? (256u << 10) : stackBytes;
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0) {
            return ok = false;
        }
        pthread_attr_setstacksize(&attr, stackSize);
        pthread_t pt;
        const int rc = pthread_create(&pt, &attr, &Worker::Entry, this);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            return ok = false;
        }
        pthread_detach(pt);
        // (a short wait for its report, so that the start line can say "running"; nothing depends on it)
        (void)up.Wait(upWaitUs);
        if (SelfResult() == 2) {
            return ok = false;  // (the thread cannot run a pass: its identity or thread-local state is broken)
        }
        return ok = true;
    }
    bool Ready() const { return ok; }
    int Core() const { return effectiveCore; }
    int Mask() const { return wantMask; }  // (the affinity mask it asked for; the priority it got is read back: Priority())
    // The priority the thread has given itself (0: it has not got that far yet), and the one it was asked to take.
    int Priority() const { return gotPriority.load(std::memory_order_relaxed); }
    int AskedPriority() const { return wantPriority; }
    // 0: the thread has not run yet; 1: it runs (its stack is known); 2: it has moved to its core and priority.
    uint32_t Reported() const { return reported.load(std::memory_order_relaxed); }
    uint32_t upWaitUs = 200000;
    // (the host test: the thread reports in this many microseconds late)
    std::atomic< uint32_t > testUpDelayUs{0};

    // Is the caller the draw thread? By the stack: a frame inside the thread's stack is the thread's (a compare, no kernel call:
    // the guest OS asks on every interrupt-state change while a job is out).
    bool OnThread() const {
        const uintptr_t sp = reinterpret_cast< uintptr_t >(__builtin_frame_address(0));
        return sp - stackLo.load(std::memory_order_relaxed) < stackSpan.load(std::memory_order_relaxed);
    }
    uintptr_t StackLo() const { return stackLo.load(std::memory_order_relaxed); }
    uintptr_t StackSpan() const { return stackSpan.load(std::memory_order_relaxed); }

    // G: publishes fn(ctx). The job before it must have been joined (WaitDone true, or Take + Reset).
    void Dispatch(Fn fn, void* ctx) {
        jFn = fn;
        jCtx = ctx;
        jFpMode = par::FpModeGet();
        tDispatch = par::NowUs();
        state.store(kArmed, std::memory_order_release);  // (publishes the job and everything G wrote before it)
        work.Signal();
    }
    // G: true when D had not started the job: it is the caller's now (run it, then Reset()). false: D has it.
    bool Take() {
        uint32_t s = kArmed;
        return state.compare_exchange_strong(s, kTaken, std::memory_order_acq_rel);
    }
    void Reset() { state.store(kIdle, std::memory_order_relaxed); }
    // G: sleeps until D's job is done, at most `us` microseconds (0: no limit). true: done, everything D wrote is visible, D is idle.
    bool WaitDone(uint32_t us) {
        if (!done.Wait(us)) {
            return false;
        }
        (void)state.load(std::memory_order_acquire);
        state.store(kIdle, std::memory_order_relaxed);
        return true;
    }
    uint32_t State() const { return state.load(std::memory_order_acquire); }

    // D's time on its jobs, their number, and the time from each dispatch to its start, since the last call.
    void TakeStats(uint32_t* busyUs, uint32_t* jobs, uint32_t* startUs) {
        *busyUs = busy.exchange(0, std::memory_order_relaxed);
        *jobs = count.exchange(0, std::memory_order_relaxed);
        *startUs = startDelay.exchange(0, std::memory_order_relaxed);
    }
    // (the host test: D starts each job this many microseconds late, so that G reaches the join first)
    std::atomic< uint32_t > testStartDelayUs{0};

private:
    static void* Entry(void* self) {
        static_cast< Worker* >(self)->Loop();
        return nullptr;
    }
    void Loop() {
        // (the stack: this frame is near its top; a job's frames lie below it, inside the size the thread was made with less a margin)
        const uintptr_t top = reinterpret_cast< uintptr_t >(__builtin_frame_address(0)) + 512;
        const uintptr_t span = stackSize - (16u << 10);
        if (const uint32_t late = testUpDelayUs.load(std::memory_order_relaxed)) {
            const uint64_t until = par::NowUs() + late;
            while (par::NowUs() < until) {
                par::CpuRelax();
            }
        }
        stackSpan.store(span, std::memory_order_relaxed);
        stackLo.store(top - span, std::memory_order_relaxed);
        SelfTest();  // (pipefix26: thread-local and pthread state of THIS thread, before anything is handed to it)
        reported.store(1, std::memory_order_release);
        up.Signal();  // (before the move below: see Start)
#if defined(__vita__) && !defined(PORT_ARMHF_HARNESS)  // (armhf harness: Linux threads, no psp2 kernel headers)
        const int moveRc = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), wantMask);
        core3::MarkOnCore3(c3Slot = core3::RegisterThread("vgx-draw"), wantMask == 0x80000 && moveRc >= 0);
        if (wantPriority >= 0x40 && wantPriority <= 0xBF) {
            sceKernelChangeThreadPriority(0, wantPriority);
        }
        gotPriority.store(sceKernelGetThreadCurrentPriority(), std::memory_order_relaxed);
#else
        gotPriority.store(wantPriority, std::memory_order_relaxed);
#endif
        reported.store(2, std::memory_order_release);
        core3::Worker c3(c3Slot);  // (one pass is one call: on core 3 it is only metered)
        for (;;) {
            work.Wait(0);
            c3.Wake();
            if (const uint32_t late = testStartDelayUs.load(std::memory_order_relaxed)) {
                const uint64_t until = par::NowUs() + late;
                while (par::NowUs() < until && state.load(std::memory_order_acquire) == kArmed) {
                    par::CpuRelax();
                }
            }
            uint32_t s = kArmed;
            if (!state.compare_exchange_strong(s, kRunning, std::memory_order_acq_rel)) {
                continue;  // (G took the job back before this wake)
            }
            const uint64_t t0 = par::NowUs();
            par::FpModeSet(jFpMode);  // (the game thread's floating-point mode: a matrix is the same bytes on either thread)
            jFn(jCtx);
            c3.Sleep();
            const uint64_t t1 = par::NowUs();
            busy.fetch_add(static_cast< uint32_t >(t1 - t0), std::memory_order_relaxed);
            startDelay.fetch_add(static_cast< uint32_t >(t0 > tDispatch ? t0 - tDispatch : 0), std::memory_order_relaxed);
            count.fetch_add(1, std::memory_order_relaxed);
            state.store(kDone, std::memory_order_release);
            done.Signal();
        }
    }

    // pipefix26: what a pass uses of the thread's own state, proven on the helper before its first job. The C library's thread keys
    // (pthread-embedded; the compiler's emulated thread_local is built on them: __emutls_get_address) and the thread's identity. A failure
    // keeps the thread from being "ready": every draw is then serial and the start line says why ("pipe25: DISABLED: ...").
    // 0 not run yet, 1 ok, 2 failed (SelfWhy names the step).
public:   // (read by the start line in SceneFunction.cpp)
    uint32_t SelfResult() const { return selfResult.load(std::memory_order_acquire); }
    const char* SelfWhy() const { return selfWhy; }
private:
    void SelfTest() {
        static thread_local int tlProbe = 0;
        const char* why = nullptr;
        const pthread_t me = pthread_self();
        pthread_key_t key;
        tlProbe = 0x5A5A;
        if (!pthread_equal(me, pthread_self())) {
            why = "pthread_self is not stable on the helper";
        } else if (pthread_key_create(&key, nullptr) != 0) {
            why = "no pthread key can be made on the helper";
        } else {
            int probe = 0;
            if (pthread_setspecific(key, &probe) != 0 || pthread_getspecific(key) != &probe) {
                why = "a pthread key does not hold a value on the helper";
            }
            pthread_key_delete(key);
        }
        if (why == nullptr && (tlProbe != 0x5A5A || &tlProbe == nullptr)) {
            why = "a thread_local does not hold a value on the helper";
        }
        selfWhy = why != nullptr ? why : "tls ok";
        selfResult.store(why != nullptr ? 2u : 1u, std::memory_order_release);
    }

    TimedSema work, done, up;
    std::atomic< uint32_t > selfResult{0};
    const char* selfWhy = "not run";
    bool started = false, ok = false;
    int effectiveCore = 1, wantPriority = 0, wantMask = 0;
    std::atomic< int > gotPriority{0};
    std::atomic< uint32_t > reported{0};
    uint32_t stackSize = 0;
    int c3Slot = -1;
    Fn jFn = nullptr;
    void* jCtx = nullptr;
    uint32_t jFpMode = 0;
    uint64_t tDispatch = 0;
    alignas(64) std::atomic< uint32_t > state{kIdle};
    std::atomic< uint32_t > busy{0}, count{0}, startDelay{0};
    std::atomic< uintptr_t > stackLo{0}, stackSpan{0};
};

// ------------------------------------------------------------------------------------------------- the turn
// What the game (or the host test) does for the engine. All on G unless said otherwise.
struct PipeHost {
    // The draw's three parts, on the calling thread (G; D in mode 3). Serial: one after the other. Mode 4: the begin on G, the
    // middle on G with the GX library recording, the end on G after the join (at once when the recording was abandoned).
    virtual void DrawBegin() = 0;         // beginRender
    virtual void DrawMiddle() = 0;        // draw(), and what belongs to it
    virtual void DrawEnd() = 0;           // endRender
    virtual void OwnerEnter() = 0;        // mode 3, on D before the draw: D becomes the game thread for the guest OS and the frame mark
    virtual void OwnerLeave() = 0;        // mode 3, on D after it: given back
    virtual bool CaptureOpen() = 0;       // mode 4: the passes leave tokens from now on. false: not possible this turn
    virtual void CaptureClose(bool recorded) = 0;  // the recording is over: `recorded`: handed to D; else the rest of the draw is serial
    // A token, on the thread that executes the recording (D; G at a take). `displaced`: G, at an abandon: a token recorded
    // earlier is executed now, in the middle of whatever the game thread's draw was doing when the capture ended.
    virtual void RunToken(uint32_t id, bool displaced) = 0;
    virtual void OverlapBegin() = 0;      // mode 4, before the dispatch: what D reads is published by it
    virtual void OverlapEnd() = 0;        // after the join: the frame is out of use, the passes' J3DSys words go back
    virtual void AfterReplay() = 0;       // mode 4, on D when the recording has been executed: the stream's tail is published
    virtual void Poll() = 0;              // G's safe point while it waits at the join (guest interrupts, other guest threads)
    virtual int64_t NowNs() = 0;
    virtual void Log(const char* line) = 0;

protected:
    ~PipeHost() {}
};

// ---- GXV_PIPE25 (pipe27): what the game (or the host test) does for the late draw. G unless said otherwise.
struct LateHost {
    virtual void DrawBegin() = 0;   // beginRender
    virtual void DrawMiddle() = 0;  // draw(): the passes call the host's token function
    virtual void DrawEnd() = 0;     // endRender
    // Before the draw's begin: the FIFO is armed for tokens (and, in verify, the buffers made). false: not possible this turn.
    virtual bool Open(bool verify) = 0;
    // After the middle: the passes handed over (their number), 0 when none (the draw is then whole as serial, also after an abandon:
    // `abandoned`). Real mode: the chain is closed and the library's data after the open lists is kept for the merge.
    virtual uint32_t Close(bool verify, bool abandoned) = 0;
    // Runner (helper, or G at a take-over): Begin once, Step for each pass in order, End once. Step runs pass i and puts it into the
    // stream (real) or re-runs it into the scratch buffer and compares (verify).
    virtual uint32_t Begin(bool verify) = 0;
    virtual void Step(uint32_t i, bool verify) = 0;
    virtual void End(bool verify) = 0;
    // G, mid-draw (Late::Abandon): run the passes made so far, in order, now (real); verify: drop this picture's compare.
    virtual void AbandonNow(bool verify) = 0;
    // before the dispatch: the owner trap, the stack range. false: the trap could not be armed (this turn's job is then never handed over: G runs it itself)
    virtual bool OverlapBegin() = 0;
    virtual void OverlapEnd() = 0;    // after the join: the frame is out of use
    virtual void LogExtra() {}        // (pipefix31) after each window line: the host's own census
    virtual void SelfRunBegin() {}    // (pipefix31) G is about to run the job itself (take-over): its own GX calls must not meet the owner trap
    virtual void Poll() = 0;          // G's safe point while it waits at the join
    virtual int64_t NowNs() = 0;
    virtual void Log(const char* line) = 0;

protected:
    ~LateHost() {}
};

class Pipe {
public:
    Worker worker;
    CaptureApi api;
    uint32_t watchdogUs = 200000;  // GXV_DRAWPIPE_WATCHDOG_MS
    uint32_t sliceUs = 1000;       // GXV_DRAWPIPE_SLICE_US
    uint32_t maxTaken = 30;        // joins in a row that found the job not started: the pipeline goes off (D does not get its core)
    uint32_t startGraceUs = 2000;  // mode 3: a job not started this long after its dispatch is taken back

    bool Off() const { return mOff; }
    void TurnOff(PipeHost& host, const char* why) {
        if (!mOff) {
            mOff = true;
            char line[240];
            snprintf(line, sizeof line, "drawpipe: OFF for this session: %s. Every draw is serial from the next turn on", why);
            host.Log(line);
        }
    }
    bool Overlapped() const { return mOverlap; }
    // Is the caller D with a job? (For the hooks that must not wait for the game thread, and the guest OS's owner check.)
    bool OnDrawThread() const { return mJobOut.load(std::memory_order_relaxed) != 0 && worker.OnThread(); }
    bool JobOut() const { return mJobOut.load(std::memory_order_relaxed) != 0; }

    // The turn's draw. `mode`: 3 or 4; `why`: kGo, or the reason this turn is serial. Returns how it was drawn: 0 serial on the
    // calling thread, 3 by D while G waited, 4 recorded and handed to D (Join() must follow, before calcAnim()).
    int Draw(PipeHost& host, int mode, int why) {
        ++mWin.turns;
        if (why == kGo && mOff) {
            why = kWhyOff;
        }
        if (why == kGo && !worker.Ready()) {
            why = kWhyConfig;
        }
        if (why == kGo && mode >= 4 && !api.Complete()) {
            why = kWhyConfig;
        }
        if (why != kGo) {
            ++mWin.serial[why];
            Serial(host);
            return 0;
        }
        mHost = &host;
        if (mode == 3) {
            const int64_t t0 = host.NowNs();
            mJobKind = 3;
            mJobOut.store(1, std::memory_order_relaxed);
            worker.Dispatch(&Pipe::JobEntry, this);
            // G waits blind: for the length of this job D is the game thread (the guest's current thread, the frame mark), so G
            // must not take a guest safe point of its own. D cannot wait for G: G holds nothing here.
            // A job D has not STARTED after startGraceUs (it does not get its core: the back end and the boot's shader work
            // outrank it) is taken back and drawn here, as in mode 4; many in a row turn the pipeline off. A job that has
            // started is waited for, the watchdog logging once.
            bool tripped = false;
            while (!worker.WaitDone(sliceUs)) {
                const int64_t waited = host.NowNs() - t0;
                if (worker.State() == Worker::kArmed && waited > static_cast< int64_t >(startGraceUs) * 1000 && worker.Take()) {
                    // (D never started: nothing of the draw was made; G makes it, as always)
                    mJobOut.store(0, std::memory_order_relaxed);
                    worker.Reset();
                    ++mWin.taken;
                    if (++mTakenRun >= maxTaken) {
                        TurnOff(host, "the draw thread did not start its job in time, many turns in a row (mode 3: it does not get its core)");
                    }
                    Serial(host);
                    return 0;
                }
                if (!tripped && waited > static_cast< int64_t >(watchdogUs) * 1000) {
                    tripped = true;
                    TurnOff(host, "the game thread waited longer than the watchdog's time for the whole draw on the draw thread (mode 3)");
                    StillWaiting(host, t0);
                    break;
                }
            }
            mTakenRun = 0;
            mJobOut.store(0, std::memory_order_relaxed);
            const uint64_t ns = static_cast< uint64_t >(host.NowNs() - t0);
            mWin.waitNs += ns;
            if (ns > mWin.waitMaxNs) mWin.waitMaxNs = ns;
            ++mWin.whole;
            return 3;
        }
        // ---- mode 4: the begin for real, the middle recorded, on G
        if (!host.CaptureOpen()) {
            ++mWin.serial[kWhyNoMemory];
            Serial(host);
            return 0;
        }
        host.DrawBegin();
        api.setHandler(&Pipe::HandlerToken, this);
        if (!api.begin()) {
            host.CaptureClose(false);
            ++mWin.serial[kWhyBusy];
            host.DrawMiddle();
            host.DrawEnd();
            return 0;
        }
        const int64_t t0 = host.NowNs();
        mCapturing = true;
        mAbandonWhy = kWhyAbandoned;
        mTokens = 0;
        host.DrawMiddle();  // (every GX call recorded; every J3D pass a token: Token() below)
        mCapturing = false;
        // nullptr: the capture was abandoned on the way (by the layer, or by Abandon() below). What had been recorded was executed
        // on this thread then, and every token since was executed where it stood: the middle is drawn, in today's order.
        void* capture = api.end();
        host.CaptureClose(capture != nullptr);
        if (capture == nullptr) {
            ++mWin.serial[mAbandonWhy];
            host.DrawEnd();
            return 0;
        }
        mWin.captureNs += static_cast< uint64_t >(host.NowNs() - t0);
        mWin.tokens += mTokens;
        host.OverlapBegin();
        mCapture = capture;
        mJobKind = 4;
        mOverlap = true;
        mJobOut.store(1, std::memory_order_relaxed);
        worker.Dispatch(&Pipe::JobEntry, this);
        ++mWin.overlapped;
        return 4;
    }

    // A token of the draw being recorded (G): a J3D pass. false: nothing is being recorded (the caller draws the pass at once, as
    // always), also after an abandon.
    // `noDraw`: the pass draws nothing (its buffer type had no drawer of this half at the hand-over): it loads lights at most.
    bool Token(uint32_t id, bool noDraw) {
        if (!mCapturing || mExecuting != 0) {
            return false;  // (also while a recorded token is being executed on this thread: what a pass does is done, not recorded)
        }
        if (api.open != nullptr && !api.open()) {
            return false;  // (abandoned on the way: the layer's contract is "a token only into an open capture"; the pass is drawn where it stands)
        }
        ++mTokens;
        mInToken = true;  // (a handler call from inside this is this very token, executed where it stands)
        if (api.tokenKeep != nullptr) {
            api.tokenKeep(id, kKeepViewport | kKeepProjection | kKeepScissor | (noDraw ? kKeepSerial : 0u));
        } else {
            api.token(id);
        }
        mInToken = false;
        return true;
    }
    bool Capturing() const { return mCapturing; }
    // The engine's own abandon (G, while recording): something the recording cannot hold is about to happen. The layer executes
    // what was recorded here, in order (the passes through the handler); the rest of the draw is serial.
    void Abandon(int why) {
        if (!mCapturing || mExecuting != 0) {
            return;
        }
        if (mAbandonWhy == kWhyAbandoned) {
            mAbandonWhy = why;
        }
        api.abandon(ReasonName(why));
    }

    // Before calcAnim() (and wherever the logic needs GX or the lists' buffers during the overlap: GameEnter). Nothing to do when
    // no draw is out. When the recording has been executed (by D, or by G itself after a take), G makes the draw's end: the
    // stream then holds begin, middle, end in today's order, before anything the caller sends next.
    void Join(PipeHost& host) {
        if (!mOverlap) {
            return;
        }
        const int64_t t0 = host.NowNs();
        if (worker.Take()) {
            // D has not started: G executes the recording itself. The order of the stream is today's.
            mJobOut.store(0, std::memory_order_relaxed);
            mSelfRun = true;  // (a GX call of this replay that passes the logic's hook, GameEnter, is the draw's own)
            RunJob();
            mSelfRun = false;
            worker.Reset();
            ++mWin.taken;
            if (++mTakenRun >= maxTaken) {
                TurnOff(host, "the draw thread had not started when the game thread reached the join, many turns in a row (it does not get its core)");
            }
        } else {
            mTakenRun = 0;
            bool tripped = false;
            int64_t lastLine = t0;
            while (!worker.WaitDone(sliceUs)) {
                host.Poll();
                if (!mOverlap) {
                    // The safe point ran something that needed GX (an interrupt handler, say) and that joined the draw from in
                    // here (GameEnter): the job is done and taken, the draw's end is made. Waiting on would wait for a signal
                    // that has been consumed: for ever.
                    return;
                }
                const int64_t now = host.NowNs();
                if (!tripped && now - t0 > static_cast< int64_t >(watchdogUs) * 1000) {
                    tripped = true;
                    TurnOff(host, "the join waited longer than the watchdog's time for the draw thread");
                    lastLine = now;
                } else if (tripped && now - lastLine > 5000000000ll) {
                    lastLine = now;
                    WhereLine(host, now - t0);
                }
            }
            mJobOut.store(0, std::memory_order_relaxed);
        }
        const uint64_t ns = static_cast< uint64_t >(host.NowNs() - t0);
        mWin.waitNs += ns;
        if (ns > mWin.waitMaxNs) mWin.waitMaxNs = ns;
        if (ns > 100000) ++mWin.waited;  // (joins that waited more than 0.1 ms: D was not done when update() was)
        mOverlap = false;
        mCapture = nullptr;
        host.OverlapEnd();
        const int64_t t1 = host.NowNs();
        host.DrawEnd();
        mWin.endNs += static_cast< uint64_t >(host.NowNs() - t1);
    }
    // The logic is about to use GX, or to write something the draw side reads, while a draw is out: the draw is joined first.
    // On D itself (its own GX calls pass the same hooks) nothing happens. Returns true when it joined.
    bool GameEnter(PipeHost& host) {
        if (!mOverlap || mSelfRun || worker.OnThread()) {
            return false;
        }
        ++mWin.earlyJoins;
        Join(host);
        return true;
    }
    // A guest thread other than the game thread (a loading thread) is about to write what the draw thread may read. The join is
    // the game thread's alone (its state is not shared), so this thread only waits until the draw thread's job is over: timed
    // sleeps, at most the watchdog's time (a job that has not even started by then is left alone and the wait is counted as
    // given up). Nothing can wait for this thread meanwhile: the draw thread waits for nobody.
    void WaitOther(PipeHost& host) {
        if (mJobOut.load(std::memory_order_acquire) == 0 || worker.OnThread()) {
            return;
        }
        const int64_t t0 = host.NowNs();
        mOtherWaits.fetch_add(1, std::memory_order_relaxed);
        for (;;) {
            const uint32_t st = worker.State();
            if (mJobOut.load(std::memory_order_acquire) == 0 || (st != Worker::kArmed && st != Worker::kRunning)) {
                return;
            }
            if (host.NowNs() - t0 > static_cast< int64_t >(watchdogUs) * 1000) {
                mOtherGaveUp.fetch_add(1, std::memory_order_relaxed);
                return;
            }
#if defined(__vita__) && !defined(PORT_ARMHF_HARNESS)  // (armhf harness: Linux threads, no psp2 kernel headers)
            sceKernelDelayThread(sliceUs);
#else
            std::this_thread::sleep_for(std::chrono::microseconds(sliceUs));
#endif
        }
    }
    uint32_t OtherWaits() const { return mOtherWaits.load(std::memory_order_relaxed); }
    // Every reason that held at this turn's hand-over (bit r = reason r), whichever of them the turn is counted under: a turn
    // has one "serial" reason in the log's first list, the first that applied; this second count shows what stands behind it.
    void NoteHeld(uint32_t mask) {
        for (int r = 1; r < kReasons; ++r) {
            if (mask & (1u << r)) ++mWin.held[r];
        }
    }
    // (D, from the guest OS's owner check: a guest call was made on the draw thread during an overlap. Counted; the game's glue
    // turns the pipeline off after the join.)
    void NoteGuestCall(void* from) {
        if (mGuestCalls.fetch_add(1, std::memory_order_relaxed) == 0) {
            mGuestFrom.store(from, std::memory_order_relaxed);
        }
    }
    uint32_t GuestCalls(void** from) {
        *from = mGuestFrom.load(std::memory_order_relaxed);
        return mGuestCalls.exchange(0, std::memory_order_relaxed);
    }
    // Where D is in its job, for the log: the tokens it has run, and the one it is in (0: between two).
    void NoteToken(uint32_t id, bool in) {
        mWhereToken.store(in ? id + 1 : 0, std::memory_order_relaxed);
        if (in) mWhereCount.fetch_add(1, std::memory_order_relaxed);
    }

    // The window's line (every `every` turns; the host test asks for it at its end with every = 1). true: a line was written.
    // The two times a console run is read for: "the game thread inside the recording" (the draw's middle with the GX library
    // recording: the open lists' own code, the recording of their calls, the tokens, the view jobs' retire) and the draw
    // thread's job (the recorded calls executed and the passes drawn; the passes' part is given apart).
    bool MaybeLog(PipeHost& host, int mode, uint32_t every) {
        if (mWin.turns < every) {
            return false;
        }
        uint32_t busyUs, jobs, startUs;
        worker.TakeStats(&busyUs, &jobs, &startUs);
        const double perJob = jobs ? 1.0 / jobs : 0.0;
        const uint32_t piped = mWin.overlapped + mWin.whole;
        const double perPiped = piped ? 1.0 / piped : 0.0;
        const uint64_t passNs = mPassNs.exchange(0, std::memory_order_relaxed);
        char line[1700];
        int o = snprintf(line, sizeof line,
                         "drawpipe: GXV_DRAWPIPE=%d, %u drawn turns: %u whole draws on the draw thread, %u recorded and handed over "
                         "(%.1f tokens a turn, the game thread inside the recording %.3f ms); draw thread (core %d, priority 0x%X%s): %u jobs, "
                         "%.3f ms a job (%.3f of it inside the passes), started %.3f ms after the dispatch; the game thread at the join: %.3f ms a turn, longest %.3f, %u joins waited, %u jobs "
                         "taken back unstarted, %u joined early by the logic; the draw's end after the join %.3f ms; serial turns:",
                         mode, mWin.turns, mWin.whole, mWin.overlapped, mWin.tokens * (mWin.overlapped ? 1.0 / mWin.overlapped : 0.0),
                         mWin.captureNs * (mWin.overlapped ? 1.0 / mWin.overlapped : 0.0) / 1e6, worker.Core(),
                         static_cast< unsigned >(worker.Reported() >= 2 ? worker.Priority() : worker.AskedPriority()),
                         !worker.Ready() ? ", NOT CREATED" : worker.Reported() == 0 ? " asked, it has not run yet" : worker.Reported() == 1 ? " asked, not set yet" : "",
                         jobs, busyUs * perJob / 1e3, passNs * perJob / 1e6, startUs * perJob / 1e3, mWin.waitNs * perPiped / 1e6, mWin.waitMaxNs / 1e6, mWin.waited,
                         mWin.taken, mWin.earlyJoins, mWin.endNs * (mWin.overlapped ? 1.0 / mWin.overlapped : 0.0) / 1e6);
        bool any = false;
        for (int r = 1; r < kReasons && o < static_cast< int >(sizeof line) - 48; ++r) {
            if (mWin.serial[r]) {
                any = true;
                o += snprintf(line + o, sizeof line - static_cast< size_t >(o), " %s %u;", ReasonName(r), mWin.serial[r]);
            }
        }
        if (!any && o < static_cast< int >(sizeof line) - 8) {
            o += snprintf(line + o, sizeof line - static_cast< size_t >(o), " none");
        }
        bool anyHeld = false;
        for (int r = 1; r < kReasons && o < static_cast< int >(sizeof line) - 96; ++r) {
            if (mWin.held[r]) {
                o += snprintf(line + o, sizeof line - static_cast< size_t >(o), "%s %s %u;", anyHeld ? "" : " every reason that held at a hand-over (a turn can have several):",
                              ReasonName(r), mWin.held[r]);
                anyHeld = true;
            }
        }
        if (const uint32_t other = mOtherWaits.load(std::memory_order_relaxed)) {
            if (o < static_cast< int >(sizeof line) - 120) {
                snprintf(line + o, sizeof line - static_cast< size_t >(o), " since the start: %u waits for the draw thread by another guest thread (%u given up)",
                         other, mOtherGaveUp.load(std::memory_order_relaxed));
            }
        }
        host.Log(line);
        for (int r = 0; r < kReasons; ++r) mTotalSerial[r] += mWin.serial[r];
        mTotalOverlapped += mWin.overlapped, mTotalWhole += mWin.whole, mTotalTaken += mWin.taken, mTotalEarly += mWin.earlyJoins;
        mWin = Window();
        return true;
    }
    // (since the start: what the host test checks)
    uint32_t TotalSerial(int r) const { return mTotalSerial[r] + mWin.serial[r]; }
    uint32_t TotalOverlapped() const { return mTotalOverlapped + mWin.overlapped; }
    uint32_t TotalWhole() const { return mTotalWhole + mWin.whole; }
    uint32_t TotalTaken() const { return mTotalTaken + mWin.taken; }
    uint32_t TotalEarly() const { return mTotalEarly + mWin.earlyJoins; }

private:
    struct Window {
        uint32_t turns = 0, whole = 0, overlapped = 0, taken = 0, waited = 0, earlyJoins = 0;
        uint32_t serial[kReasons] = {};
        uint32_t held[kReasons] = {};
        uint64_t tokens = 0, captureNs = 0, waitNs = 0, waitMaxNs = 0, endNs = 0;
    };
    void Serial(PipeHost& host) {
        host.DrawBegin();
        host.DrawMiddle();
        host.DrawEnd();
    }
    static void JobEntry(void* self) { static_cast< Pipe* >(self)->RunJob(); }
    // The job, on D (or on G after a take): the whole draw (3), or the recording executed with its tokens (4).
    void RunJob() {
        if (mJobKind == 3) {
            mHost->OwnerEnter();
            Serial(*mHost);
            mHost->OwnerLeave();
        } else {
            mWhereCount.store(0, std::memory_order_relaxed);
            api.replay(mCapture, &Pipe::ReplayToken, this);
            if (!mSelfRun) {
                mHost->AfterReplay();  // (on D: G is still in update(); after a take G makes the draw's end at once instead)
            }
        }
    }
    static void ReplayToken(uint32_t id, void* self) {
        Pipe* p = static_cast< Pipe* >(self);
        p->NoteToken(id, true);
        ++p->mExecuting;
        if (p->mSelfRun) {
            p->mHost->RunToken(id, false);  // (the game thread executes the recording it took back: not the draw thread's time)
        } else {
            const int64_t t0 = p->mHost->NowNs();
            p->mHost->RunToken(id, false);
            p->mPassNs.fetch_add(static_cast< uint64_t >(p->mHost->NowNs() - t0), std::memory_order_relaxed);
        }
        --p->mExecuting;
        p->NoteToken(id, false);
    }
    // The layer executes a token itself (at an abandon, and for each token after one): on the recording thread.
    static void HandlerToken(uint32_t id, void* self) {
        Pipe* p = static_cast< Pipe* >(self);
        const bool displaced = !p->mInToken;
        const bool inToken = p->mInToken;
        p->mInToken = false;
        ++p->mExecuting;
        p->mHost->RunToken(id, displaced);
        --p->mExecuting;
        p->mInToken = inToken;
    }
    void WhereLine(PipeHost& host, int64_t waitedNs) {
        char line[240];
        const uint32_t in = mWhereToken.load(std::memory_order_relaxed);
        snprintf(line, sizeof line,
                 "drawpipe: still waiting for the draw thread after %.1f s: job state %u, %u tokens run, %s %u (between tokens it is inside the "
                 "GX library: the front end's back-pressure)",
                 waitedNs / 1e9, worker.State(), mWhereCount.load(std::memory_order_relaxed), in ? "in token" : "after token", in ? in - 1 : 0);
        host.Log(line);
    }
    void StillWaiting(PipeHost& host, int64_t t0) {
        while (!worker.WaitDone(5000000)) {
            WhereLine(host, host.NowNs() - t0);
        }
    }

    PipeHost* mHost = nullptr;
    void* mCapture = nullptr;
    int mJobKind = 0, mAbandonWhy = kWhyAbandoned;
    bool mOff = false, mOverlap = false, mCapturing = false, mInToken = false, mSelfRun = false;
    uint32_t mTokens = 0, mTakenRun = 0;
    int mExecuting = 0;  // (the thread that executes the recording is inside a token)
    std::atomic< uint32_t > mJobOut{0}, mGuestCalls{0}, mWhereToken{0}, mWhereCount{0}, mOtherWaits{0}, mOtherGaveUp{0};
    std::atomic< void* > mGuestFrom{nullptr};
    std::atomic< uint64_t > mPassNs{0};  // (the time inside the passes of the recordings executed, since the last line)
    Window mWin;
    uint32_t mTotalSerial[kReasons] = {}, mTotalOverlapped = 0, mTotalWhole = 0, mTotalTaken = 0, mTotalEarly = 0;
};

// ======================================================================================================== GXV_PIPE25 (pipe27, 12 Nov)
// The late draw: the engine of platform/include/port/pipe25.h's design (modes 6 / 7 of pipe26's report; here Late::Draw(host, 1 or 2)).
// One helper thread (Worker, its own: GXV_PIPE25_CORE / _PRIO) and the game thread G. A drawn turn:
//   hand-over   G: Open (the raw FIFO is armed), DrawBegin, DrawMiddle (every J3D pass is a token: the GX library's lazy state is
//               flushed, GXData snapshotted, the FIFO redirected into the side chain; the open lists' bytes go into the chain),
//               Close (the chain is closed, the library's data after the open lists is kept), then the job is dispatched. G goes on
//               with update() at once. The draw's end (endRender) is G's, at the join, once the stream holds the whole middle.
//   the job     helper: Begin (the splice starts at the stream's tail), then for each pass i: Step(i) = the pass i into the stream from
//               its snapshot, then the chain bytes Bi behind it (so the stream is published in order, piece by piece), End (the
//               library's data is merged as the serial draw would have left it).
//   the join    G, before calcAnim (and wherever the logic needs GX or the lists: GameEnter). The normal case waits for nothing: the
//               helper has been at work for the length of update(). Never a deadlock, never a blind wait:
//                 - a job the helper has not STARTED is taken back by atomic claim and run by G (counted: taken);
//                 - a join that has waited takeoverUs asks the helper to stop at its next pass boundary (it never stops inside a pass)
//                   and G runs the remaining passes itself (counted: takenOver); the cursor is the one thing they share, handed over
//                   by the done semaphore;
//                 - a join that waits longer than the watchdog logs once and turns the late draw off for the session.
//   VERIFY (mode 2): the passes ran in place on G as today (the picture is the game thread's); the helper re-runs each from its
//               snapshot into a scratch buffer and the host compares per pass (ComparePass).
// Waiting goes one way only (G waits for the helper; the helper waits for nobody but the front end's back-pressure, as G would).
class Late {
public:
    Worker worker;
    uint32_t watchdogUs = 200000, sliceUs = 1000, takeoverUs = 1500;
    pipe25::Census c;        // the window's totals (the host adds its own: token time, chain bytes, merged words)
    pipe25::VerifyStats vs;  // (the host's compare writes it)

    bool Off() const { return mOff; }
    bool Overlapped() const { return mOverlap; }
    bool Capturing() const { return mCapturing; }
    bool JobOut() const { return mJobOut.load(std::memory_order_relaxed) != 0; }
    bool OnHelper() const { return mJobOut.load(std::memory_order_relaxed) != 0 && worker.OnThread(); }
    int Mode() const { return mMode; }
    void TurnOff(LateHost& host, const char* why) {
        if (!mOff) {
            mOff = true;
            char line[240];
            snprintf(line, sizeof line, "pipe25: OFF for this session: %s. Every draw is serial from the next turn on", why);
            host.Log(line);
        }
    }
    void NoteHeld(uint32_t mask) {
        for (int r = 1; r < kReasons; ++r) {
            if (mask & (1u << r)) ++mHeld[r];
        }
    }

    // The turn's draw. `mode`: 1 on, 2 verify; `why`: kGo, or the reason this turn is serial. Returns 0 serial (also when the draw was
    // abandoned or nothing could be deferred), 6 handed to the helper (Join must follow), 7 the same, verify.
    int Draw(LateHost& host, int mode, int why) {
        if (mOverlap) {
            Join(host);  // (not expected: the join comes before calcAnim)
        }
        mHost = &host;
        mMode = mode;
        ++c.pictures;
        const int64_t tD0 = host.NowNs();
        if (mWinStart == 0) {
            mWinStart = tD0;
            mCtlLeft = kCtlTurns;
        } else if (tD0 - mWinStart >= kWindowNs) {
            EndWindow(host, tD0);
        }
        if (why == kGo && mOff) why = kWhyOff;
        if (why == kGo && !worker.Ready()) why = kWhyConfig;
        if (why != kGo) {
            ++c.serial;
            ++mSerial[why];
            Serial(host);
            return 0;
        }
        const bool verify = mode == 2;
        if (mode == 1 && mCtlLeft > 0 && netLoss && (++mCtlTick % 3u) == 0) {  // (p31e: every third turn until the window has its control turns: spread over the window)
            // (pipefix31) a control turn: this turn would have been handed over; it is drawn serially and timed, to be compared with the late turns of the window
            --mCtlLeft;
            ++c.serial;
            ++mCtlN;
            Serial(host);
            mCtlNs += static_cast< uint64_t >(host.NowNs() - tD0);
            return 0;
        }
        if (!host.Open(verify)) {
            ++c.serial;
            ++mSerial[kWhyBusy];
            Serial(host);
            return 0;
        }
        host.DrawBegin();
        mCapturing = true;
        mTokens = 0;
        mAbandonWhy = 0;
        host.DrawMiddle();  // (every pass a token: Token() / CountToken(); the host's glue does the GX side)
        mCapturing = false;
        const uint32_t n = host.Close(verify, mAbandonWhy != 0);
        if (n == 0) {
            // (nothing is out: no pass at all, or the draw was abandoned and the passes ran where they were; the stream is whole)
            ++c.serial;
            ++mSerial[mAbandonWhy != 0 ? mAbandonWhy : kWhyNoPass];
            if (mAbandonWhy != 0) ++c.abandoned;
            host.DrawEnd();
            return 0;
        }
        ++c.deferred;
        c.tokens += n;
        mDrawEndAtJoin = !verify;
        if (verify) {
            host.DrawEnd();  // (the passes ran in place: the stream is whole)
        }
        const bool armed = host.OverlapBegin();
        mN = n;
        mNext = 0;
        mBegan = mFinished = false;
        mStop.store(false, std::memory_order_relaxed);
        mOverlap = true;
        if (!armed) {
            // (pipefix31) no trap: the job is not handed over. G runs the passes itself, now, in order, and the feature goes off.
            --c.deferred;  // (p31e: counted once, as a serial turn)
            c.tokens -= n;
            ++c.serial;
            TurnOff(host, "the owner trap could not be armed: this turn's passes ran on the game thread");
            host.SelfRunBegin();
            mSelfRun = true;
            RunJob(false);
            mSelfRun = false;
            mOverlap = false;
            host.OverlapEnd();
            if (mDrawEndAtJoin) host.DrawEnd();
            return 0;
        }
        mJobOut.store(1, std::memory_order_relaxed);
        worker.Dispatch(&Late::JobEntry, this);
        if (mode == 1) {
            mLateNs += static_cast< uint64_t >(host.NowNs() - tD0);
            ++mLateN;
        }
        return mode == 2 ? 7 : 6;
    }

    // A pass is about to be deferred (G, in the draw): true while the draw is open for tokens. The host does the GX side; this counts.
    void CountToken() { ++mTokens; }
    // The engine's own abandon (G, while the draw is open): something the deferral cannot hold is about to happen (a pass from the
    // live lists, a fur write, a list written in the draw). The host runs the passes made so far, in order, here; the rest of the
    // draw is serial. Safe to call when nothing is open.
    void Abandon(LateHost& host, int why) {
        if (!mCapturing || mAbandonWhy != 0) {
            return;
        }
        mAbandonWhy = why;
        mCapturing = false;  // (before the host runs the passes: a pass must not be deferred again from inside)
        host.AbandonNow(mMode == 2);
    }

    // Before calcAnim() and anywhere the logic needs GX or the lists: the helper's job is over and the stream whole. Nothing to do when
    // none is out.
    void Join(LateHost& host) {
        if (!mOverlap) {
            return;
        }
        const bool verify = mMode == 2;
        const int64_t t0 = host.NowNs();
        if (worker.Take()) {
            // The helper has not started: G does the whole job. The stream's order is the same.
            mJobOut.store(0, std::memory_order_relaxed);
            host.SelfRunBegin();
            mSelfRun = true;
            RunJob(false);
            mSelfRun = false;
            worker.Reset();
            ++c.taken;
        } else {
            bool stopSent = false, tripped = false;
            int64_t lastLine = t0;
            while (!worker.WaitDone(sliceUs)) {
                host.Poll();
                if (!mOverlap) {
                    return;  // (an interrupt handler used GX at the safe point and joined from in here: it is done and the end made)
                }
                const int64_t now = host.NowNs();
                if (!verify && !stopSent && now - t0 > static_cast< int64_t >(takeoverUs) * 1000) {
                    stopSent = true;
                    mStop.store(true, std::memory_order_release);  // (the helper stops before its next pass; a pass in progress is finished)
                }
                if (!tripped && now - t0 > static_cast< int64_t >(watchdogUs) * 1000) {
                    tripped = true;
                    TurnOff(host, "the join waited longer than the watchdog's time for the helper");
                    lastLine = now;
                } else if (tripped && now - lastLine > 5000000000ll) {
                    lastLine = now;
                    char line[200];
                    snprintf(line, sizeof line, "pipe25: still waiting for the helper after %.1f s: pass %u of %u", (now - t0) / 1e9, mNext, mN);
                    host.Log(line);
                }
            }
            mJobOut.store(0, std::memory_order_relaxed);
            if (!mFinished) {
                // The helper stopped at a pass boundary (or before it began): G runs what is left, from the cursor.
                host.SelfRunBegin();
                mSelfRun = true;
                RunJob(false);
                mSelfRun = false;
                ++c.takenOver;
            }
        }
        const uint64_t ns = static_cast< uint64_t >(host.NowNs() - t0);
        c.waitNs += ns;
        if (mMode == 1) mLateNs += ns;
        if (ns > mWaitMaxNs) mWaitMaxNs = ns;
        if (ns > 100000) ++c.waited;
        mOverlap = false;
        host.OverlapEnd();
        if (mDrawEndAtJoin) {
            const int64_t t1 = host.NowNs();
            host.DrawEnd();
            const uint64_t endNs = static_cast< uint64_t >(host.NowNs() - t1);
            mEndNs += endNs;
            if (mMode == 1) mLateNs += endNs;
        }
    }
    // ---- pipefix31: the honest gain. Per drawn turn the game thread's time WITH the helper (the hand-over: open, tokens, close; the join's wait and any take-over
    // work; the draw's end after the join) against a control: every 10 s window opens with kCtlTurns turns that are drawn serially and timed (they would have been
    // handed over). A window in which the helper path costs the game thread more than the control turns the feature off (netLoss, GXV_PIPE25_NETLOSS).
    bool netLoss = true;
    double kMinJudgedPassMs = 0.8;  // (pipe60; GXV_PIPE25_JUDGE_PASS_US)
    void EndWindow(LateHost& host, int64_t now) {
        mLastLateMs = mLateN ? mLateNs / 1e6 / mLateN : 0.0;
        mLastCtlMs = mCtlN ? mCtlNs / 1e6 / mCtlN : 0.0;
        mLastLateN = mLateN;
        mLastCtlN = mCtlN;
        ++mWindows;
        // (pipe60) Only a window with something to gain is judged: the helper's passes must average kMinJudgedPassMs per late turn (about 1.2 ms at
        // Honeyhive, 0.25 ms in the title and the menus). Console runs c118 (4-6 passes a turn, 0.25-0.45 ms) ended the feature in the menus before the stage:
        // the control turns' mean moves by more than the whole possible gain there (+0.07, -0.05, -1.3, -0.4 ms with the same scene), and the session was OFF.
        const double passMs = mLateN ? mWinPassNs.exchange(0, std::memory_order_relaxed) / 1e6 / mLateN : (mWinPassNs.store(0, std::memory_order_relaxed), 0.0);
        const bool judged = passMs >= kMinJudgedPassMs;
        const bool losing = mMode == 1 && judged && mLateN >= 20 && mCtlN >= 8 && mLastLateMs > mLastCtlMs * 1.05 + 0.05;
        mLossStreak = losing ? mLossStreak + 1 : (judged ? 0 : mLossStreak);  // (a window not judged neither adds nor clears)
        if (netLoss && !mOff && losing && mLossStreak >= 2) {  // (p31e: two windows in a row, 5 % and 0.05 ms over: one window with a scene change or a cold cache does not decide)
            char why[200];
            snprintf(why, sizeof why, "net loss over two 10 s windows in a row: in the last, the game thread spends %.3f ms a turn with the helper (%u turns) against %.3f ms drawing serially (%u control turns)",
                     mLastLateMs, mLateN, mLastCtlMs, mCtlN);
            mNetLossOff = true;
            TurnOff(host, why);
        }
        mLateNs = mCtlNs = 0;
        mLateN = mCtlN = 0;
        mCtlLeft = kCtlTurns;
        mWinStart = now;
    }
    // Start-up check (the host's self-test): a job that holds the helper for holdUs and stamps the time it ended into *endNs. Not an overlap: no pass, no join.
    // TestJobEnd waits for it (false: the helper did not finish within a second).
    // (p31e) ready: polled by the job before its hold (up to a second): the job then lasts until the other side has really reached its wait. TestJobReady(): it did.
    bool TestJobBegin(LateHost& host, uint32_t holdUs, int64_t* endNs, bool (*ready)(void) = nullptr) {
        if (mOverlap || mJobOut.load() != 0 || !worker.Ready()) return false;
        mHost = &host;
        mTestHoldUs = holdUs;
        mTestEnd = endNs;
        mTestReadyFn = ready;
        mTestReady = false;
        mJobOut.store(1, std::memory_order_relaxed);
        worker.Dispatch(&Late::TestJobEntry, this);
        return true;
    }
    // (pipe33) `us`: how long the job is given. The test job's own length is bounded (the ready poll, then the hold), but the helper's share of its core is
    // not: at start-up it took 2.4 s on the console (the graphics start-up beside it), not 5 ms.
    bool TestJobEnd(uint32_t us = 1000000) {
        const bool ok = worker.WaitDone(us);
        mJobOut.store(0, std::memory_order_relaxed);
        if (!ok) mOff = true;
        return ok;
    }
    // The logic is about to use GX or write what the draw reads while a job is out: joined first. true when it joined.
    bool GameEnter(LateHost& host) {
        if (!mOverlap || mSelfRun || worker.OnThread()) {
            return false;
        }
        ++c.early;
        Join(host);
        return true;
    }
    // Another guest thread is about to write what the helper reads: it waits until the job is over (the join is G's alone).
    void WaitOther(LateHost& host) {
        if (mJobOut.load(std::memory_order_acquire) == 0 || worker.OnThread()) {
            return;
        }
        const int64_t t0 = host.NowNs();
        bool warned = false;
        for (;;) {
            const uint32_t st = worker.State();
            if (mJobOut.load(std::memory_order_acquire) == 0 || (st != Worker::kArmed && st != Worker::kRunning)) {
                return;
            }
            if (!warned && host.NowNs() - t0 > static_cast< int64_t >(watchdogUs) * 1000) {
                // (p31e: no early way out: a thread let in while the job is live would run beside the helper in the same GX state. The helper's
                // job always ends; this only says that it took long.)
                warned = true;
                mOtherLong.fetch_add(1, std::memory_order_relaxed);
            }
#if defined(__vita__) && !defined(PORT_ARMHF_HARNESS)  // (armhf harness: Linux threads, no psp2 kernel headers)
            sceKernelDelayThread(sliceUs);
#else
            std::this_thread::sleep_for(std::chrono::microseconds(sliceUs));
#endif
        }
    }
    uint32_t OtherWaitsOverWatchdog() const { return mOtherLong.load(std::memory_order_relaxed); }
    void NoteGuestCall(void* from) {
        if (mGuestCalls.fetch_add(1, std::memory_order_relaxed) == 0) {
            mGuestFrom.store(from, std::memory_order_relaxed);
        }
    }
    uint32_t GuestCalls(void** from) {
        *from = mGuestFrom.load(std::memory_order_relaxed);
        return mGuestCalls.exchange(0, std::memory_order_relaxed);
    }

    // The window's line (every `every` drawn turns): the census (pipe25.h LogCensus) and what the console run is read for.
    bool MaybeLog(LateHost& host, int mode, uint32_t every) {
        if (c.pictures < every) {
            return false;
        }
        uint32_t busyUs, jobs, startUs;
        worker.TakeStats(&busyUs, &jobs, &startUs);
        c.jobNs = static_cast< uint64_t >(busyUs) * 1000u;
        c.passNs = mPassNs.exchange(0, std::memory_order_relaxed);
        c.spliceNs = c.jobNs > c.passNs ? c.jobNs - c.passNs : 0;
        char line[1800];
        pipe25::LogCensus(c, mode, vs, line, sizeof line);
        int o = static_cast< int >(strlen(line));
        const double per = c.deferred ? 1.0 / c.deferred : 0.0;
        const double perJob = jobs ? 1.0 / jobs : 0.0;
        // The estimate of the game thread's gain per late turn: the passes' time on the helper (what G no longer spends) less what G
        // now spends on tokens and on the join's wait. The helper may be slower than G was: an ESTIMATE, to be read with the turn-prof draw bucket.
        const double saved = (c.passNs * per - c.tokenNs * per - c.waitNs * per) / 1e6;
        o += snprintf(line + o, sizeof line - static_cast< size_t >(o),
                      "; helper (core %d, affinity mask 0x%X asked, priority 0x%X read back%s): %u jobs, started %.3f ms after the dispatch; joins: %u waited over 0.1 ms (longest %.3f ms), "
                      "%u jobs taken back unstarted, %u stopped for takeover (the game thread finished them); the draw's end after the join %.3f ms a turn; "
                      "ESTIMATED game-thread saving %.3f ms a late turn (helper passes %.3f - tokens %.3f - join wait %.3f)",
                      worker.Core(), static_cast< unsigned >(worker.Mask()), static_cast< unsigned >(worker.Reported() >= 2 ? worker.Priority() : worker.AskedPriority()),
                      !worker.Ready() ? ", NOT CREATED" : "", jobs, startUs * perJob / 1e3, c.waited, mWaitMaxNs / 1e6, c.taken, c.takenOver,
                      mEndNs * per / 1e6, saved, c.passNs * per / 1e6, c.tokenNs * per / 1e6, c.waitNs * per / 1e6);
        if (mode == 1 && o < static_cast< int >(sizeof line) - 220) {
            o += snprintf(line + o, sizeof line - static_cast< size_t >(o),
                          "; MEASURED (last 10 s window, game thread, per drawn turn): helper path %.3f ms (%u turns: hand-over + join wait + take-over + draw end) vs serial control %.3f ms (%u turns), net %+.3f ms%s",
                          mLastLateMs, mLastLateN, mLastCtlMs, mLastCtlN, mLastCtlN ? mLastCtlMs - mLastLateMs : 0.0, mNetLossOff ? " [SWITCHED OFF: net loss]" : "");
        }
        if (mode == 2 && o < static_cast< int >(sizeof line) - 120) {
            o += snprintf(line + o, sizeof line - static_cast< size_t >(o), "; the game thread's own time in the in-place passes %.3f ms a turn (what the walk costs it today)",
                          c.gameInPassNs * per / 1e6);
        }
        bool any = false;
        for (int r = 1; r < kReasons && o < static_cast< int >(sizeof line) - 64; ++r) {
            if (mSerial[r]) {
                o += snprintf(line + o, sizeof line - static_cast< size_t >(o), "%s %s %u;", any ? "" : "; serial turns by reason:", ReasonName(r), mSerial[r]);
                any = true;
            }
        }
        bool anyHeld = false;
        for (int r = 1; r < kReasons && o < static_cast< int >(sizeof line) - 64; ++r) {
            if (mHeld[r]) {
                o += snprintf(line + o, sizeof line - static_cast< size_t >(o), "%s %s %u;", anyHeld ? "" : " every reason that held (a turn can have several):", ReasonName(r), mHeld[r]);
                anyHeld = true;
            }
        }
        host.Log(line);
        host.LogExtra();
        c = pipe25::Census();
        for (int r = 0; r < kReasons; ++r) mSerial[r] = mHeld[r] = 0;
        mWaitMaxNs = mEndNs = 0;
        return true;
    }

private:
    void Serial(LateHost& host) {
        host.DrawBegin();
        host.DrawMiddle();
        host.DrawEnd();
    }
    static void JobEntry(void* self) { static_cast< Late* >(self)->RunJob(true); }
    static void TestJobEntry(void* self) {
        Late* l = static_cast< Late* >(self);
        if (l->mTestReadyFn != nullptr) {
            // (pipe33: bounded by time, 10 s, not by a number of polls: a 200 us delay is about 0.5 ms on the console, and a probe thread that is slow to
            // start must not read as "not trapped")
            const int64_t t0 = l->mHost->NowNs();
            while (!l->mTestReady && l->mHost->NowNs() - t0 < 10000000000ll) {
                if (l->mTestReadyFn()) l->mTestReady = true;
#if defined(__vita__) && !defined(PORT_ARMHF_HARNESS)  // (armhf harness: Linux threads, no psp2 kernel headers)
                else sceKernelDelayThread(200);
#else
                else std::this_thread::sleep_for(std::chrono::microseconds(200));
#endif
            }
        }
#if defined(__vita__) && !defined(PORT_ARMHF_HARNESS)  // (armhf harness: Linux threads, no psp2 kernel headers)
        sceKernelDelayThread(l->mTestHoldUs);
#else
        std::this_thread::sleep_for(std::chrono::microseconds(l->mTestHoldUs));
#endif
        if (l->mTestEnd != nullptr) *l->mTestEnd = l->mHost->NowNs();
    }
    // The job, on the helper (or on G: after a take, or to finish what the helper stopped before). The cursor is plain memory: whoever
    // runs it owns it, and ownership changes hands only through the dispatch (G to helper) and the done semaphore (helper to G).
    void RunJob(bool helper) {
        LateHost& host = *mHost;
        const bool verify = mMode == 2;
        if (!mBegan) {
            if (helper && mStop.load(std::memory_order_acquire)) {
                return;
            }
            if (helper) {
                // fx66 test switch GXV_PIPE25_TESTDELAY_US (default 0, never set in play): the helper starts this long late, so that with GXV_PIPE25=2 (verify) its re-run
                // overlaps the logic's calcAnim() (and what that writes outside the hooks shows as a mismatch), not just update(); read once.
                static const long testDelayUs = [] { const char* e = std::getenv("GXV_PIPE25_TESTDELAY_US"); return e && *e ? std::atol(e) : 0L; }();
                if (testDelayUs > 0) {
#if defined(__vita__) && !defined(PORT_ARMHF_HARNESS)
                    sceKernelDelayThread(static_cast< SceUInt32 >(testDelayUs));
#else
                    std::this_thread::sleep_for(std::chrono::microseconds(testDelayUs));
#endif
                }
            }
            (void)host.Begin(verify);
            mBegan = true;
        }
        for (;;) {
            if (helper && mStop.load(std::memory_order_acquire)) {
                return;  // (before a pass, never inside one)
            }
            const uint32_t i = mNext;
            if (i >= mN) {
                break;
            }
            const int64_t t0 = host.NowNs();
            host.Step(i, verify);
            mPassNs.fetch_add(static_cast< uint64_t >(host.NowNs() - t0), std::memory_order_relaxed);
            mWinPassNs.fetch_add(static_cast< uint64_t >(host.NowNs() - t0), std::memory_order_relaxed);  // (pipe60: for the net-loss rule's window)
            mNext = i + 1;
        }
        host.End(verify);
        mFinished = true;
    }

    LateHost* mHost = nullptr;
    int mMode = 1, mAbandonWhy = 0;
    bool mOff = false, mOverlap = false, mCapturing = false, mSelfRun = false, mDrawEndAtJoin = false;
    bool mBegan = false, mFinished = false, mNetLossOff = false;
    bool (*mTestReadyFn)(void) = nullptr;
    volatile bool mTestReady = false;
public:
    bool TestJobReady() const { return mTestReady; }
private:
    static constexpr uint32_t kCtlTurns = 20;
    static constexpr int64_t kWindowNs = 10000000000ll;
    int64_t mWinStart = 0, *mTestEnd = nullptr;
    uint64_t mLateNs = 0, mCtlNs = 0;
    uint32_t mCtlTick = 0, mLossStreak = 0;
    uint32_t mLateN = 0, mCtlN = 0, mCtlLeft = 0, mLastLateN = 0, mLastCtlN = 0, mWindows = 0, mTestHoldUs = 0;
    double mLastLateMs = 0.0, mLastCtlMs = 0.0;
    uint32_t mN = 0, mNext = 0, mTokens = 0;
    std::atomic< bool > mStop{false};
    std::atomic< uint32_t > mJobOut{0}, mGuestCalls{0}, mOtherLong{0};
    std::atomic< void* > mGuestFrom{nullptr};
    std::atomic< uint64_t > mPassNs{0};
    std::atomic< uint64_t > mWinPassNs{0};  // (pipe60: passes' time in the current net-loss window)
    uint64_t mWaitMaxNs = 0, mEndNs = 0;
    uint32_t mSerial[kReasons] = {}, mHeld[kReasons] = {};
};

}  // namespace drawpipe
}  // namespace port
}  // extern "C++"
