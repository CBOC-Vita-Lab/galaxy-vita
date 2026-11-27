// Galaxy 3.0 multiplayer (MP3 nocap): host tests for the thread layout WITHOUT the Vita's fourth core (no CapUnlocker plug-in: the
// default for most players) and with it. No kernel: the decisions are the port's pure ones (platform/include/port/core_layout.h), the
// parallel workers' placement (platform/include/port/par_exec.h / par_spec.h, their console branch over kernel stand-ins below) and the
// session helper's (platform/src/mp/mp_core_sys.h mp_helper_place).
//   1. the A/B switch: GXV_CORE3=0 in env.txt gives every thread the place it has on a console without the plug-in (the probe says no),
//      over the 1.3 start-up defaults (vita/vgx-game/entry.cpp); the Settings "Core plan: Off" row is NOT that (it also moves the
//      collector and the DSP mixer back to 1.2's places);
//   2. the probe on stock firmware (the system refuses the mask) fails closed at once: one probe, no thread started, none left behind;
//   3. the parallel workers asked on core 3 go to core 1 when the core is not proven, and when the kernel refuses a proven one;
//   4. the session helper never asks for mask 0x80000, whatever it is asked or env.txt says, and keeps core 2 / 0x70 in both layouts.
// Build + run: tests/mp/nocap_build.sh (also run by tests/mp/perf_build.sh).
#include "port/core_layout.h"
#include "port/par_spec.h"   // (and par_exec.h: built with -D__vita__ over tests/par_stubs, so their console branch runs)
#include "../../platform/src/mp/mp_core_sys.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace port::cores;

static int g_fail = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); g_fail++; } } while (0)

// ---- kernel stand-ins for par_exec.h / par_spec.h (console branch): thread creation is recorded, nothing runs
static bool g_plugin = false;          // the kernel lets an application create threads on the system core
static bool g_proven = false;          // what port_vita_core3_available answers (the start-up probe's verdict)
static std::vector<int> g_createMasks;
static int g_createPrio = 0;
extern "C" {
int port_vita_core3_available(void) { return g_proven ? 1 : 0; }
SceUID sceKernelCreateSema(const char*, SceUInt, int, int, const void*) { return 1; }
int sceKernelWaitSema(SceUID, int, SceUInt*) { return 0; }
int sceKernelSignalSema(SceUID, int) { return 0; }
SceUID sceKernelCreateThread(const char*, SceKernelThreadEntry, int prio, SceSize, SceUInt, int mask, const void*) {
    g_createMasks.push_back(mask);
    g_createPrio = prio;
    if (mask == kSystem && !g_plugin) return static_cast<SceUID>(0x80028025u);   // (stock firmware: refused)
    return 0x40010100 + static_cast<SceUID>(g_createMasks.size());
}
int sceKernelStartThread(SceUID, SceSize, void*) { return 0; }   // (the worker never runs here: only its place is tested)
int sceKernelGetThreadCurrentPriority(void) { return 0x9F; }     // (the calling thread: the game thread's class)
SceUInt64 sceKernelGetProcessTimeWide(void) { return 0; }
}

static std::map<std::string, std::string> gEnv;
static const char* Get(const char* name) {
    const auto it = gEnv.find(name);
    return it == gEnv.end() ? nullptr : it->second.c_str();
}
// vita/vgx-game/entry.cpp's 1.3 defaults for the layout (setenv(..., 0): env.txt values, loaded first, win)
static void EntryDefaults() {
    for (const char* n : {"GXV_CORE3", "GXV_CORE3_SPLIT", "GXV_CORE3_AUDIO", "GXV_CORE0_ALONE", "GXV_CORE3_IO"})
        if (!gEnv.count(n)) gEnv[n] = "1";
    if (!gEnv.count("GXV_AUDIO_THREAD_CORE")) gEnv["GXV_AUDIO_THREAD_CORE"] = "1";
}

// What each fourth-core part ends up asking, as vita/runtime/memory.cpp decides it (port_vita_thread_core3 / port_vita_thread_io):
// the switch first, then the probe (which runs only when some part wants the core).
struct Plan {
    bool probes;
    bool part[5];       // [1..4] Core3Part asks for the system core
    bool io[2];         // the disc read-ahead worker, movie decoder 3 ask
    int helperMask, unclassed, splitCore;
};
static Plan Decide(const Switches& s, bool probeSaysYes) {
    Plan p{};
    p.probes = Core3Wanted(s);
    const bool proven = p.probes && probeSaysYes;
    for (int k = 1; k <= 4; k++) p.part[k] = Core3Uses(s, static_cast<Core3Part>(k), proven);
    p.io[0] = MayUseSystemCore(s, IoKind::DiscReader, 0) && proven;
    p.io[1] = MayUseSystemCore(s, IoKind::MovieDecoder, 2) && proven;
    p.helperMask = HelperMask(s);
    p.unclassed = UnclassedMask(s);
    p.splitCore = SplitCore(s, 1);
    return p;
}
static bool Same(const Plan& a, const Plan& b) {
    for (int k = 1; k <= 4; k++) if (a.part[k] != b.part[k]) return false;
    return a.io[0] == b.io[0] && a.io[1] == b.io[1] && a.helperMask == b.helperMask && a.unclassed == b.unclassed && a.splitCore == b.splitCore;
}

static void TestAbSwitch() {
    // the console without the plug-in: 1.3 defaults, the probe runs and says no
    gEnv.clear();
    EntryDefaults();
    const Switches stock = ReadSwitches(Get);
    const Plan noPlugin = Decide(stock, false);
    CHECK(noPlugin.probes);   // (the probe runs: it is what says no)
    for (int k = 1; k <= 4; k++) CHECK(!noPlugin.part[k]);
    CHECK(!noPlugin.io[0] && !noPlugin.io[1]);
    CHECK(noPlugin.helperMask == kHelpers && noPlugin.unclassed == kHelpers && noPlugin.splitCore == 1);
    // with the plug-in: audio output + mixer, the finishing thread, disc read-ahead and movie decoder 3 move (shader worker: only with
    // GXV_ASYNC_SHADERS=1)
    const Plan plugin = Decide(stock, true);
    CHECK(plugin.part[1] && plugin.part[2] && plugin.part[3] && !plugin.part[4]);
    CHECK(!plugin.io[0] && plugin.io[1]);  // (core3-36: the disc worker stays on cores 1-2 unless GXV_CORE3_DISC=1)
    // the A/B switch on a console WITH the plug-in: GXV_CORE3=0 in env.txt (before the defaults) = the console without it
    gEnv.clear();
    gEnv["GXV_CORE3"] = "0";
    EntryDefaults();
    const Switches ab = ReadSwitches(Get);
    CHECK(!Core3Wanted(ab));   // (no probe at all: the basic log says "not probed: GXV_CORE3=0 ...")
    CHECK(Same(Decide(ab, true), noPlugin));
    CHECK(Same(Decide(ab, false), noPlugin));
    // a part switch alone does not undo it
    gEnv["GXV_CORE3_AUDIO"] = "1";
    gEnv["GXV_CORE3_IO"] = "1";
    CHECK(Same(Decide(ReadSwitches(Get), true), noPlugin));
    // "auto" (the Settings default value) behaves as unset: the parts' own defaults
    gEnv.clear();
    gEnv["GXV_CORE3"] = "auto";
    EntryDefaults();
    const Plan autoPlan = Decide(ReadSwitches(Get), true);
    CHECK(autoPlan.part[1] && autoPlan.part[2] && autoPlan.part[3]);   // (GXV_CORE3_AUDIO / _SPLIT=1 from the defaults)
    // Settings "Core plan: Off" writes GXV_CORE3=0 AND GXV_CORE3_IO=0, GXV_GC_CORE=2, GXV_AUDIO_THREAD_CORE=0 (settings_launch.cpp):
    // the fourth-core decisions match, but the collector (core 2) and the DSP mixer (cores 1-2) move too, so it is not the A/B.
    gEnv.clear();
    gEnv["GXV_CORE3"] = "0";
    gEnv["GXV_CORE3_IO"] = "0";
    gEnv["GXV_GC_CORE"] = "2";
    gEnv["GXV_AUDIO_THREAD_CORE"] = "0";
    EntryDefaults();
    CHECK(Same(Decide(ReadSwitches(Get), true), noPlugin));
    CHECK(std::strcmp(Get("GXV_GC_CORE"), "2") == 0 && std::strcmp(Get("GXV_AUDIO_THREAD_CORE"), "0") == 0);
    std::printf("A/B switch GXV_CORE3=0: ok\n");
}

// The probe's view of a console (port::cores::ProveSystemCore's Api).
struct StockApi {
    bool plugin = false;     // CapUnlocker: the system core may be asked for
    bool runsElsewhere = false;
    int creates = 0, changes = 0, starts = 0, deletes = 0, sleeps = 0, waits = 0;
    int Create(int mask) { ++creates; return mask == kSystem && !plugin ? static_cast<int>(0x80028025u) : 0x40010200 + creates; }
    int Change(int, int mask) { ++changes; return mask == kSystem && !plugin ? static_cast<int>(0x80028025u) : 0; }
    int Start(int) { ++starts; return 0; }
    int Wait(int, unsigned) { ++waits; return 0; }
    void Delete(int) { ++deletes; }
    void Sleep(unsigned) { ++sleeps; }
    bool Reported() { return starts > 0; }
    int Cpu() { return runsElsewhere ? 2 : 3; }
    int Mask() { return runsElsewhere ? kUser2 : kSystem; }
    unsigned long long SpinWallUs() { return kProofSpinUs; }
    unsigned long long SpinCpuUs() { return kProofSpinUs * 9 / 10; }
};

static void TestProbeStock() {
    StockApi api;
    int tries = 0;
    Proof r = ProveSystemCoreRetry(api, &tries);
    CHECK(!r.proven && !r.lowShareOnly);
    CHECK(std::strcmp(r.why, "the system refused the mask") == 0);
    CHECK(tries == 1 && api.sleeps == 0);                     // final at once: no retry, no start-up delay
    CHECK(api.creates == 2 && api.starts == 0 && api.deletes == 1 && api.waits == 0);   // (the second route's thread deleted, never run)
    // a kernel that accepts the mask but runs the thread elsewhere: not proven either, still one probe
    StockApi odd;
    odd.plugin = true;
    odd.runsElsewhere = true;
    r = ProveSystemCoreRetry(odd, &tries);
    CHECK(!r.proven && tries == 1 && odd.deletes == 1);
    // the plug-in: proven
    StockApi cap;
    cap.plugin = true;
    r = ProveSystemCoreRetry(cap, &tries);
    CHECK(r.proven && tries == 1 && r.cpu == 3 && r.mask == kSystem);
    std::printf("probe without the plug-in: ok\n");
}

static void* g_keep[64];   // (the workers are never destroyed, as in perf.cpp: kept reachable for LeakSanitizer)
static int g_kept = 0;
template <class T> static T& Keep(T* p) { if (g_kept < 64) g_keep[g_kept++] = p; return *p; }

static void TestParWorkers() {
    struct Case { bool plugin, proven; int wantCore; std::vector<int> wantMasks; } cases[] = {
        {false, false, 1, {kUser1}},            // no plug-in: core 1 at once, nothing asked of the system core
        {true, false, 1, {kUser1}},             // GXV_CORE3=0 with the plug-in (not proven): the same
        {true, true, 3, {kSystem}},             // the plug-in, proven: the fourth core
        {false, true, 1, {kSystem, kUser1}},    // proven but refused at creation (should not happen): core 1 on the second try
    };
    for (const Case& c : cases) {
        g_plugin = c.plugin;
        g_proven = c.proven;
        // par-anim (GXV_PAR_ANIM_CORE=3, priority 0: one step below the game thread's)
        g_createMasks.clear();
        port::par::SpecWorker& anim = Keep(new port::par::SpecWorker);
        CHECK(anim.Start(3, 0, "smg_spec"));
        CHECK(anim.Core() == c.wantCore && g_createMasks == c.wantMasks && anim.Priority() == 0xA0);
        // par-view async (GXV_PAR_CORE=3, priority -1: one step above the game thread's)
        g_createMasks.clear();
        port::par::SpecWorker& view = Keep(new port::par::SpecWorker);
        CHECK(view.Start(3, -1, "smg_view_async"));
        CHECK(view.Core() == c.wantCore && g_createMasks == c.wantMasks && view.Priority() == 0x9E);
        // the joined executor (GXV_PAR_VIEW=1, GXV_PAR_CORE=3)
        g_createMasks.clear();
        port::par::Executor& ex = Keep(new port::par::Executor);
        CHECK(ex.Start(3, 0, 40));
        CHECK(ex.Core() == c.wantCore && g_createMasks == c.wantMasks);
    }
    // a worker asked on a user core never touches the system core
    g_plugin = true;
    g_proven = true;
    for (int core = 0; core <= 2; core++) {
        int eff = -1;
        CHECK(port::par::CoreMask(core, &eff) == (kUser0 << core) && eff == core);
    }
    std::printf("parallel workers without the fourth core: ok\n");
}

static void TestHelperPlace() {
    MpHelperPlace p = mp_helper_place(2, nullptr, nullptr);   // MpCore::ensureThread's call
    CHECK(p.core == 2 && p.mask == kUser2 && p.prio == 0x70 && p.retryMask == kHelpers);
    // the helper sits below every audio thread (output / AI clock 0x40, JAudio 0x4F, DSP mixer 0x50: vita/runtime/audio.cpp) and above
    // the front end's player and the default-priority threads it shares core 2 with
    CHECK(p.prio > 0x50 && p.prio < 0x90);
    // never the system core, whatever the caller or env.txt asks
    const char* envCores[] = {nullptr, "", "3", "4", "-2", "abc", "2x", "0x80000", "1", "0", "-1", "2"};
    const char* envPrios[] = {nullptr, "0x70", "0x30", "0xC0", "0x70x", "0x48", "180"};
    for (int asked = -3; asked <= 5; asked++)
        for (const char* ec : envCores)
            for (const char* ep : envPrios) {
                p = mp_helper_place(asked, ec, ep);
                CHECK((p.mask & kSystem) == 0 && (p.retryMask < 0 || (p.retryMask & (kSystem | kUser0)) == 0));
                CHECK(p.core >= -1 && p.core <= 2 && p.prio >= 0x40 && p.prio <= 0xBF);
            }
    CHECK(mp_helper_place(3, nullptr, nullptr).mask == kUser2);          // (was mask 0x80000: SCE_KERNEL_CPU_MASK_USER_0 << 3)
    CHECK(mp_helper_place(2, "abc", nullptr).mask == kUser2);            // (was core 0, the game's: strtol("abc") = 0)
    CHECK(mp_helper_place(2, "3", nullptr).mask == kUser2);              // (out of range: ignored, as before)
    CHECK(mp_helper_place(2, "1", nullptr).mask == kUser1);              // the A/B override still works
    CHECK(mp_helper_place(2, "-1", nullptr).mask == 0 && mp_helper_place(2, "-1", nullptr).retryMask == -1);
    CHECK(mp_helper_place(2, nullptr, "0x48").prio == 0x48 && mp_helper_place(2, nullptr, "0x30").prio == 0x70);
    CHECK(mp_helper_place(2, nullptr, "0x70x").prio == 0x70);
    // and its place does not depend on the fourth core (nothing in it reads the probe): the same in both layouts by construction
    std::printf("session helper placement: ok\n");
}

int main() {
    TestAbSwitch();
    TestProbeStock();
    TestParWorkers();
    TestHelperPlace();
    if (g_fail) {
        std::printf("FAILED: %d checks\n", g_fail);
        return 1;
    }
    std::printf("all nocap tests passed\n");
    return 0;
}
