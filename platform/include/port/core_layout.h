// Which core each kind of port thread may run on (PS Vita: cores 0-2 are the application's, the fourth is the system's).
// Pure policy: no kernel calls, so the selection logic is tested on a PC (tests/core_layout_test.cpp). The Vita runtime
// (vita/runtime/memory.cpp) supplies the kernel; the renderer's host (vgx_host_galaxy_vita.cpp) uses SweepTarget.
//
//   GXV_HELPER_CORES=0   (older switch, default on) the helper threads are not moved off core 0 by their class.
//   GXV_CORE0_ALONE=1    (default 0) the game thread has core 0 to itself: every other thread of the port, the ones with a class
//                        and the ones without (log flusher, watchdog, frame clock, library threads found by name) runs on
//                        cores 1-2 (mask 0x60000), whatever GXV_HELPER_CORES says.
//   GXV_CORE3_IO=1       (default 0) delay-tolerant work may use the system core (mask 0x80000): the disc read-ahead worker and
//                        the third movie decoder. A thread tries the move on itself (1.3: only once the probe below proved the core),
//                        keeps it only when the call succeeded AND the mask reads back as asked, and otherwise goes back where it was,
//                        without a message to the player. Never the render, front-end or present path.
//
// The fourth core (1.3). An application may use the system core only when the console's owner has installed a kernel plug-in that
// opens it to game-mode applications (GrapheneCt's CapUnlocker); the game neither needs nor looks for one. Every use below is gated
// by one probe per process (ProveSystemCore, run by vita/runtime/memory.cpp port_vita_core3_available): a thread made for the
// system core must run, end within 200 ms, report CPU 3 and the mask 0x80000 alone, and get at least half of a 10 ms spin.
// Anything else is "not available" and nothing moves (the basic log says which, in a "clocks/cores: fourth core" line).
//   GXV_CORE3=0          nothing uses the fourth core, whatever the part switches say, and there is no probe.
//   GXV_CORE3 unset/auto (default) the parts below as their own switches say: GXV_CORE3_IO (entry.cpp sets 1), the audio output
//                        thread (GXV_CORE3_AUDIO defaults to 2 here), and nothing else.
//   GXV_CORE3=1          every part: IO, audio output and the DSP mixer, and the front-end finishing thread. A part switch set to 0
//                        still keeps its part off.
//   GXV_CORE3_AUDIO=1    the audio output thread and the DSP mixer thread; =2 the output thread only; =0 neither.
//   GXV_CORE3_SPLIT=1    the front-end split's finishing thread (default off unless GXV_CORE3=1); a SplitGuard moves it back.
// A thread that asks moves itself, keeps the core only when the call succeeded AND the mask reads back as asked, and otherwise goes
// back to the place it had planned (PlaceOnSystemCore).
#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace port::cores {

constexpr int kUser0 = 0x10000, kUser1 = 0x20000, kUser2 = 0x40000, kSystem = 0x80000;
constexpr int kHelpers = kUser1 | kUser2;  // 0x60000: the cores the helper threads share
constexpr unsigned kUserAll = static_cast<unsigned>(kUser0 | kUser1 | kUser2);

struct Switches {
    bool helperCores = true;   // GXV_HELPER_CORES (anything but "0")
    bool core0Alone = false;   // GXV_CORE0_ALONE=1
    bool core3Io = false;      // GXV_CORE3_IO=1 (or GXV_CORE3=1), and not GXV_CORE3=0
    int core3Audio = 0;        // GXV_CORE3_AUDIO resolved: 0 none, 1 output + DSP mixer, 2 output only
    bool core3Split = false;   // GXV_CORE3_SPLIT=1 (or GXV_CORE3=1)
};

// A part switch: 1 on, 0 off, -1 not set (or not understood: the umbrella's default applies). `top` is the highest "on" value.
inline int PartSwitch(const char* text, int top) {
    if (!text || !*text) return -1;
    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);
    return (end && !*end && v >= 0 && v <= top) ? static_cast<int>(v) : -1;
}

// `get` is std::getenv or a test double: const char* (*)(const char*).
template <class Get> Switches ReadSwitches(Get get) {
    Switches s;
    const char* helper = get("GXV_HELPER_CORES");
    s.helperCores = !(helper && *helper == '0');
    const char* alone = get("GXV_CORE0_ALONE");
    s.core0Alone = alone && std::atoi(alone) == 1;
    // GXV_CORE3: 0 off, 1 every part, anything else (unset, "auto") each part's own default.
    const int all = PartSwitch(get("GXV_CORE3"), 1);
    if (all == 0) return s;
    const int io = PartSwitch(get("GXV_CORE3_IO"), 1), audio = PartSwitch(get("GXV_CORE3_AUDIO"), 2), split = PartSwitch(get("GXV_CORE3_SPLIT"), 1);
    s.core3Io = io == 1 || (io < 0 && all == 1);                    // (unset: off in code; vita/vgx-game/entry.cpp sets 1)
    s.core3Audio = audio >= 0 ? audio : all == 1 ? 1 : 2;           // (unset: the output thread only, the mixer only with GXV_CORE3=1)
    s.core3Split = split == 1 || (split < 0 && all == 1);           // (unset: off; GXV_CORE3=1 or GXV_CORE3_SPLIT=1)
    return s;
}

// Whether any part could use the fourth core: if not, there is no probe at all.
constexpr bool Core3Wanted(const Switches& s) { return s.core3Io || s.core3Audio != 0 || s.core3Split; }

// The parts of the fourth core plan, in the order port_vita_thread_core3 numbers them.
enum class Core3Part { AudioOutput = 1, AudioMixer = 2, SplitFinisher = 3 };

// Whether a thread of this part asks for the system core: its switch is on AND the probe proved the core.
constexpr bool Core3Uses(const Switches& s, Core3Part part, bool proven) {
    if (!proven) return false;
    switch (part) {
    case Core3Part::AudioOutput: return s.core3Audio == 1 || s.core3Audio == 2;
    case Core3Part::AudioMixer: return s.core3Audio == 1;
    case Core3Part::SplitFinisher: return s.core3Split;
    }
    return false;
}

// The mask for a helper thread with a class (clocks, disc reader, host jobs, movie decoders, audio): cores 1-2, or 0 = leave the
// thread where it is (the older GXV_HELPER_CORES=0 behaviour, which GXV_CORE0_ALONE overrides).
constexpr int HelperMask(const Switches& s) { return (s.helperCores || s.core0Alone) ? kHelpers : 0; }

// The mask for a helper thread that never had a class (log writer, freeze watch, the display's frame clock when it is not raised):
// only GXV_CORE0_ALONE moves them.
constexpr int UnclassedMask(const Switches& s) { return s.core0Alone ? kHelpers : 0; }

// The front-end finishing thread asks for a core by number (GXV_SPLIT_CORE, 0-2); core 0 is the game's.
constexpr int SplitCore(const Switches& s, int requested) { return (s.core0Alone && requested == 0) ? 1 : requested; }

enum class IoKind { DiscReader = 0, MovieDecoder = 1 };

// Only the disc reader and the third movie decoder (index 2: the one that used to be planned for the game's core) may try the
// system core. The other decoders stay on cores 1-2, so a system core that gives an application only part of its time cannot
// slow a movie below what two decoders do.
constexpr bool MayUseSystemCore(const Switches& s, IoKind kind, int index) {
    return s.core3Io && (kind == IoKind::DiscReader || (kind == IoKind::MovieDecoder && index == 2));
}

struct Placement {
    bool tried = false;      // the system core was asked for
    bool onSystem = false;   // and the thread is there now
    int askRc = 0;           // the change call's result when tried
    int readBack = 0;        // the mask read back when the call succeeded
    int mask = 0;            // the mask the thread has afterwards (0: it was never changed)
    int restoreRc = 0;       // result of putting the thread back after a refusal (0 when nothing was needed)
};

// Kernel: int Set(int mask) (the change call: negative = refused), int Get() (the mask now; negative = unknown).
// The calling thread asks for the system core and keeps it only when the call succeeded and the mask reads back as asked. Otherwise
// (refused, or accepted but not honoured) it goes to `fallback`, its planned place; with no plan (0) back where it was, if that was a
// user-core mask, and else it is left alone.
template <class Kernel> Placement PlaceOnSystemCore(Kernel& kernel, int fallback) {
    Placement p;
    p.tried = true;
    const int before = kernel.Get();
    p.askRc = kernel.Set(kSystem);
    if (p.askRc >= 0) {
        p.readBack = kernel.Get();
        if (p.readBack == kSystem) {
            p.onSystem = true;
            p.mask = kSystem;
            return p;
        }
    }
    int back = fallback & static_cast<int>(kUserAll);
    if (back == 0 && before > 0 && before != kSystem) back = before;
    if (back != 0) {
        p.restoreRc = kernel.Set(back);
        p.mask = back;
    }
    return p;
}

// (the disc worker and a movie decoder: back on cores 1-2 when refused, or where the thread was when GXV_HELPER_CORES=0)
template <class Kernel> Placement PlaceIoThread(Kernel& kernel, const Switches& s, IoKind kind, int index) {
    if (!MayUseSystemCore(s, kind, index)) return Placement{};
    return PlaceOnSystemCore(kernel, HelperMask(s));
}

// ---- the probe that proves the fourth core (once per process; vita/runtime/memory.cpp port_vita_core3_available)
constexpr int kProofStartMask = kUser2;            // the second route: a thread made on core 2, then moved
constexpr unsigned kProofTimeoutUs = 200 * 1000;   // a probe thread that has not ended in 200 ms has no usable core
constexpr unsigned kProofSpinUs = 10 * 1000;       // the probe spins this long by the clock, measuring its own CPU time
constexpr unsigned kProofMinSharePct = 50;         // and must get at least this share of it

struct Proof {
    bool proven = false;
    const char* why = "not tried";
    int createRc = 0, changeRc = 0, startRc = 0, waitRc = 0, cpu = -1, mask = 0;
    unsigned sharePct = 0;
    bool lowShareOnly = false;  // every check passed but the share (ProveSystemCoreRetry tries again)
};

// Api: int Create(int mask) (a thread id, negative when refused; it does not run yet), int Change(int id, int mask), int Start(int id),
// int Wait(int id, unsigned timeoutUs) (0 when the thread ended), void Delete(int id), and what the thread reported itself:
// bool Reported(), int Cpu() (the core it ran on, -1 when that changed), int Mask(), unsigned long long SpinWallUs(), SpinCpuUs().
// Fails closed: every way out but the last says not proven.
template <class Api> Proof ProveSystemCore(Api& api) {
    Proof r;
    int id = r.createRc = api.Create(kSystem);
    if (id < 0) {
        id = api.Create(kProofStartMask);
        if (id < 0) { r.why = "no probe thread could be made"; return r; }
        r.changeRc = api.Change(id, kSystem);
        if (r.changeRc < 0) { api.Delete(id); r.why = "the system refused the mask"; return r; }
    }
    r.startRc = api.Start(id);
    if (r.startRc < 0) { api.Delete(id); r.why = "the probe thread did not start"; return r; }
    r.waitRc = api.Wait(id, kProofTimeoutUs);
    if (r.waitRc < 0) { api.Delete(id); r.why = "the probe thread did not end in 200 ms"; return r; }  // (it ends by itself; static data only)
    api.Delete(id);
    if (!api.Reported()) { r.why = "the probe thread did not report"; return r; }
    r.cpu = api.Cpu();
    r.mask = api.Mask();
    const unsigned long long wall = api.SpinWallUs(), cpu = api.SpinCpuUs();
    r.sharePct = wall ? static_cast<unsigned>((cpu > wall ? wall : cpu) * 100ull / wall) : 0;
    if (r.cpu != 3) { r.why = "the probe thread ran on another core"; return r; }
    if (r.mask != kSystem) { r.why = "the probe thread's mask is not the system core alone"; return r; }
    if (wall < kProofSpinUs) { r.why = "the probe thread's spin was not measured"; return r; }
    if (r.sharePct < kProofMinSharePct) { r.why = "the probe thread got too little of the core"; r.lowShareOnly = true; return r; }
    r.proven = true;
    r.why = "ok";
    return r;
}

// The share is a 10 ms sample taken at start-up, while the system core is busy with the launch itself. On the console (6 Nov,
// CapUnlocker) one launch measured 3 % and ran its whole session without the fourth core (the t82 tour; other launches 53-91 %).
// A low share alone is therefore retried, up to kProofTries probes kProofRetryGapUs apart; every other failure (a refused mask, the
// wrong core, no report, a thread that did not end ...) is final at once. Api also needs void Sleep(unsigned us). *tries: probes run.
constexpr int kProofTries = 6;
constexpr unsigned kProofRetryGapUs = 40 * 1000;
template <class Api> Proof ProveSystemCoreRetry(Api& api, int* tries = nullptr) {
    Proof r;
    int n = 0;
    for (;;) {
        r = ProveSystemCore(api);
        ++n;
        if (r.proven || !r.lowShareOnly || n >= kProofTries) break;
        api.Sleep(kProofRetryGapUs);
    }
    if (tries) *tries = n;
    return r;
}

// ---- the finishing thread on the fourth core: back to its planned core when the player waits for it
// GXV_CORE3_SPLIT_GUARD_MS=<ms> (default 3; 0: never moves it back). Fed every ~5 s with the player's catch-up at the picture ends
// (waiting for the finishing thread, or doing what it had not done yet) per picture. Two windows over the limit in a row say the
// system core does not give the thread enough of its time. Windows with fewer than kGuardMinPictures pictures (a load) do not count.
constexpr unsigned kGuardMinPictures = 30;
struct SplitGuard {
    double limitMs = 3.0;
    int strikes = 0;
    bool Feed(double caughtUpMsPerPicture, unsigned pictures) {
        if (limitMs <= 0 || pictures < kGuardMinPictures) return false;
        strikes = caughtUpMsPerPicture > limitMs ? strikes + 1 : 0;
        return strikes >= 2;
    }
};

// A thread found by name in the census (library threads that the port did not make itself). Returns the mask it should move to,
// or 0 to leave it. Under GXV_CORE0_ALONE: a thread that may run on core 0 together with other cores (or has no restriction at all) is
// moved to cores 1-2; a thread pinned to core 0 alone is left (those are the game's own, and the ones the port pinned there on
// purpose), and so is everything that cannot run on a user core.
constexpr int SweepTarget(const Switches& s, unsigned currentMask) {
    if (!s.core0Alone) return 0;
    if (currentMask == 0) return kHelpers;
    if ((currentMask & kUser0) == 0) return 0;
    if (currentMask == static_cast<unsigned>(kUser0)) return 0;
    return kHelpers;
}

}  // namespace port::cores
