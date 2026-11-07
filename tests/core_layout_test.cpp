// Host test of the thread-layout policy (platform/include/port/core_layout.h): which mask each kind of thread gets with the
// switches off (the behaviour before the switches existed) and on, and how a thread that asks for the system core is kept or put back.
// No kernel is involved: a fake one answers. This does not say whether a console grants the system core to an application.
#include "port/core_layout.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

using namespace port::cores;

static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

static std::map<std::string, std::string> gEnv;
static const char* Get(const char* name) {
    const auto it = gEnv.find(name);
    return it == gEnv.end() ? nullptr : it->second.c_str();
}

// A kernel with the system core's possible answers.
struct FakeKernel {
    enum Mode { Grant, Refuse, IgnoreSilently, GrantOtherCore } mode = Grant;
    int mask = 0;           // the thread's mask (0: made without a restriction)
    int sets = 0, gets = 0;
    int history[8] = {};
    int Set(int m) {
        history[sets < 8 ? sets : 7] = m;
        ++sets;
        if (m == kSystem) {
            if (mode == Refuse) return static_cast<int>(0x80020000u);        // a negative error code, the mask unchanged
            if (mode == IgnoreSilently) return 0;                             // "ok", but the thread stays where it was
            if (mode == GrantOtherCore) { mask = kUser1; return 0; }          // moved, but not to the system core
        }
        mask = m;
        return 0;
    }
    int Get() { ++gets; return mask; }
};

int main() {
    // ---- the switches as read from the environment
    {
        gEnv.clear();
        Switches s = ReadSwitches(Get);
        Require(s.helperCores && !s.core0Alone && !s.core3Io, "nothing set: helpers off core 0 as before, the two new switches off");
        gEnv["GXV_HELPER_CORES"] = "0";
        s = ReadSwitches(Get);
        Require(!s.helperCores, "GXV_HELPER_CORES=0 is read");
        gEnv["GXV_HELPER_CORES"] = "1";
        gEnv["GXV_CORE0_ALONE"] = "1";
        gEnv["GXV_CORE3_IO"] = "1";
        s = ReadSwitches(Get);
        Require(s.helperCores && s.core0Alone && s.core3Io, "both new switches read as 1");
        gEnv["GXV_CORE0_ALONE"] = "0";
        gEnv["GXV_CORE3_IO"] = "2";
        s = ReadSwitches(Get);
        Require(!s.core0Alone && !s.core3Io, "only the value 1 turns them on (0 and 2 do not)");
        gEnv["GXV_CORE0_ALONE"] = "";
        s = ReadSwitches(Get);
        Require(!s.core0Alone, "an empty value is off");
    }

    // ---- masks with a class: the old behaviour with the switches off
    {
        Switches off;                                   // defaults
        Require(HelperMask(off) == 0x60000, "helper threads with a class: cores 1-2 by default (as before)");
        Require(UnclassedMask(off) == 0, "threads without a class are not touched by default (as before)");
        Require(SplitCore(off, 0) == 0 && SplitCore(off, 1) == 1 && SplitCore(off, 2) == 2, "the finishing thread's core is as asked by default");
        Switches helpersOff;
        helpersOff.helperCores = false;
        Require(HelperMask(helpersOff) == 0 && UnclassedMask(helpersOff) == 0, "GXV_HELPER_CORES=0 alone leaves everything where it is (as before)");
    }

    // ---- GXV_CORE0_ALONE
    {
        Switches alone;
        alone.core0Alone = true;
        Require(HelperMask(alone) == 0x60000 && UnclassedMask(alone) == 0x60000, "core 0 alone: classed and unclassed helpers on cores 1-2");
        Require(SplitCore(alone, 0) == 1 && SplitCore(alone, 1) == 1 && SplitCore(alone, 2) == 2, "core 0 alone: the finishing thread never asks for core 0");
        alone.helperCores = false;
        Require(HelperMask(alone) == 0x60000, "core 0 alone overrides GXV_HELPER_CORES=0");
        Require((HelperMask(alone) & kUser0) == 0 && (UnclassedMask(alone) & kUser0) == 0, "no helper mask contains core 0");
    }

    // ---- the library-thread sweep
    {
        Switches off, alone;
        alone.core0Alone = true;
        const unsigned masks[] = {0, 0x10000, 0x20000, 0x40000, 0x80000, 0x30000, 0x50000, 0x70000, 0xF0000, 0x90000};
        for (unsigned m : masks) Require(SweepTarget(off, m) == 0, "no sweep without the switch, whatever the mask");
        Require(SweepTarget(alone, 0) == 0x60000, "a thread with no restriction is moved");
        Require(SweepTarget(alone, 0x10000) == 0, "a thread pinned to core 0 alone is the game's (or pinned on purpose): left");
        Require(SweepTarget(alone, 0x20000) == 0 && SweepTarget(alone, 0x40000) == 0 && SweepTarget(alone, 0x60000) == 0, "threads already off core 0 are left");
        Require(SweepTarget(alone, 0x80000) == 0, "a thread on the system core is left");
        Require(SweepTarget(alone, 0x30000) == 0x60000 && SweepTarget(alone, 0x50000) == 0x60000, "core 0 plus one other core: moved");
        Require(SweepTarget(alone, 0x70000) == 0x60000 && SweepTarget(alone, 0xF0000) == 0x60000, "all user cores (with or without the system core): moved");
        Require(SweepTarget(alone, 0x90000) == 0x60000, "core 0 and the system core: moved to cores 1-2");
        for (unsigned m : masks) {
            const int t = SweepTarget(alone, m);
            Require(t == 0 || (t & kUser0) == 0, "a sweep target never contains core 0");
        }
    }

    // ---- the system core: who may ask
    {
        Switches off, io;
        io.core3Io = true;
        Require(!MayUseSystemCore(off, IoKind::DiscReader, 0), "switch off: the disc worker does not ask");
        Require(!MayUseSystemCore(off, IoKind::MovieDecoder, 2), "switch off: no decoder asks");
        Require(MayUseSystemCore(io, IoKind::DiscReader, 0), "switch on: the disc read-ahead worker asks");
        Require(!MayUseSystemCore(io, IoKind::MovieDecoder, 0) && !MayUseSystemCore(io, IoKind::MovieDecoder, 1), "switch on: decoders 0 and 1 stay on cores 1-2");
        Require(MayUseSystemCore(io, IoKind::MovieDecoder, 2), "switch on: the third decoder asks");
        Require(!MayUseSystemCore(io, IoKind::MovieDecoder, 3), "no fourth lane");
    }

    // ---- placing a thread
    {
        Switches off, io;
        io.core3Io = true;
        FakeKernel k;
        k.mask = 0x40000;
        Placement p = PlaceIoThread(k, off, IoKind::DiscReader, 0);
        Require(!p.tried && k.sets == 0 && k.gets == 0 && k.mask == 0x40000, "switch off: the kernel is not called at all");

        k = FakeKernel{};
        k.mask = 0x60000;
        p = PlaceIoThread(k, io, IoKind::MovieDecoder, 1);
        Require(!p.tried && k.sets == 0 && k.gets == 0, "a decoder that may not ask never calls the kernel");

        k = FakeKernel{};
        k.mask = 0x60000;
        p = PlaceIoThread(k, io, IoKind::DiscReader, 0);
        Require(p.tried && p.onSystem && p.mask == kSystem && k.mask == kSystem && k.sets == 1, "granted: the thread stays on the system core after one call");
        Require(p.askRc >= 0 && p.readBack == kSystem && p.restoreRc == 0, "granted: result and read-back recorded");

        k = FakeKernel{};
        k.mode = FakeKernel::Refuse;
        k.mask = 0x60000;
        p = PlaceIoThread(k, io, IoKind::DiscReader, 0);
        Require(p.tried && !p.onSystem && p.askRc < 0, "refused: not on the system core, and the refusal is kept");
        Require(k.mask == 0x60000 && p.mask == 0x60000, "refused: the thread ends on cores 1-2");
        Require(k.sets == 2 && k.history[0] == kSystem && k.history[1] == 0x60000, "refused: one ask, then the helper cores again (nothing else)");

        k = FakeKernel{};
        k.mode = FakeKernel::IgnoreSilently;
        k.mask = 0x40000;
        p = PlaceIoThread(k, io, IoKind::MovieDecoder, 2);
        Require(p.tried && !p.onSystem && p.askRc == 0 && p.readBack == 0x40000, "accepted but not honoured: the read-back catches it");
        Require(k.mask == 0x60000, "accepted but not honoured: the thread is put on cores 1-2");

        k = FakeKernel{};
        k.mode = FakeKernel::GrantOtherCore;
        k.mask = 0x60000;
        p = PlaceIoThread(k, io, IoKind::DiscReader, 0);
        Require(!p.onSystem && k.mask == 0x60000, "moved to some other core: not counted as the system core, put on cores 1-2");

        // With the helper cores off and the switch on, a refused thread goes back to where it was.
        Switches odd;
        odd.core3Io = true;
        odd.helperCores = false;
        k = FakeKernel{};
        k.mode = FakeKernel::Refuse;
        k.mask = 0x30000;
        p = PlaceIoThread(k, odd, IoKind::DiscReader, 0);
        Require(k.mask == 0x30000 && p.mask == 0x30000, "refused with GXV_HELPER_CORES=0: back to the mask it had");
        k = FakeKernel{};
        k.mode = FakeKernel::IgnoreSilently;
        k.mask = 0;
        p = PlaceIoThread(k, odd, IoKind::DiscReader, 0);
        Require(k.sets == 1 && p.mask == 0, "refused from an unrestricted thread: nothing to put back, no second call");

        // The two ways to be on a user core never include the system core, and a core-0-alone layout never puts a thread back on core 0.
        Switches both;
        both.core3Io = both.core0Alone = true;
        both.helperCores = false;
        k = FakeKernel{};
        k.mode = FakeKernel::Refuse;
        k.mask = 0x70000;
        p = PlaceIoThread(k, both, IoKind::DiscReader, 0);
        Require((k.mask & kUser0) == 0 && k.mask == 0x60000, "core 0 alone: a refused thread goes to cores 1-2, never back to a mask with core 0");
    }

    // ---- the fourth core: the umbrella and the part switches
    {
        gEnv.clear();
        Switches s = ReadSwitches(Get);
        Require(!s.core3Io && s.core3Audio == 2 && !s.core3Split, "nothing set: IO off in code (entry.cpp sets it), audio output only, no split");
        Require(Core3Wanted(s), "nothing set: the probe runs (the audio output thread may use a proven core)");
        gEnv["GXV_CORE3_IO"] = "1";
        s = ReadSwitches(Get);
        Require(s.core3Io && s.core3Audio == 2 && !s.core3Split, "the Vita's start-up default (GXV_CORE3_IO=1): IO and audio output");
        Require(!Core3Uses(s, Core3Part::AudioMixer, true) && !Core3Uses(s, Core3Part::SplitFinisher, true), "default: mixer and split stay");
        Require(Core3Uses(s, Core3Part::AudioOutput, true) && !Core3Uses(s, Core3Part::AudioOutput, false), "default: audio output only when proven");
        gEnv["GXV_CORE3"] = "1";
        s = ReadSwitches(Get);
        Require(s.core3Io && s.core3Audio == 1 && s.core3Split, "GXV_CORE3=1: every part");
        Require(Core3Uses(s, Core3Part::AudioOutput, true) && Core3Uses(s, Core3Part::AudioMixer, true) && Core3Uses(s, Core3Part::SplitFinisher, true),
                "GXV_CORE3=1, proven: every part asks");
        for (Core3Part part : {Core3Part::AudioOutput, Core3Part::AudioMixer, Core3Part::SplitFinisher})
            Require(!Core3Uses(s, part, false), "GXV_CORE3=1 without a proven core: no part asks");
        gEnv["GXV_CORE3_SPLIT"] = "0";
        gEnv["GXV_CORE3_AUDIO"] = "2";
        s = ReadSwitches(Get);
        Require(s.core3Io && s.core3Audio == 2 && !s.core3Split, "GXV_CORE3=1: a part switch set to 0 (or 2) still wins");
        gEnv["GXV_CORE3_AUDIO"] = "0";
        gEnv["GXV_CORE3_IO"] = "0";
        s = ReadSwitches(Get);
        Require(!Core3Wanted(s), "GXV_CORE3=1 with every part at 0: no probe");
        gEnv.clear();
        gEnv["GXV_CORE3"] = "0";
        gEnv["GXV_CORE3_IO"] = "1";
        gEnv["GXV_CORE3_AUDIO"] = "1";
        gEnv["GXV_CORE3_SPLIT"] = "1";
        s = ReadSwitches(Get);
        Require(!s.core3Io && s.core3Audio == 0 && !s.core3Split && !Core3Wanted(s), "GXV_CORE3=0 (Core plan Off): nothing, whatever the parts say, no probe");
        Require(!MayUseSystemCore(s, IoKind::DiscReader, 0), "GXV_CORE3=0: the disc worker does not ask even with GXV_CORE3_IO=1");
        gEnv["GXV_CORE3"] = "auto";
        s = ReadSwitches(Get);
        Require(s.core3Io && s.core3Audio == 1 && s.core3Split, "GXV_CORE3=auto: each part as its own switch says");
        gEnv.clear();
        gEnv["GXV_CORE3_AUDIO"] = "3";
        gEnv["GXV_CORE3_SPLIT"] = "yes";
        s = ReadSwitches(Get);
        Require(s.core3Audio == 2 && !s.core3Split, "values not understood are 'not set'");
        gEnv["GXV_CORE3_AUDIO"] = "1";
        gEnv["GXV_CORE3_SPLIT"] = "1";
        s = ReadSwitches(Get);
        Require(s.core3Audio == 1 && s.core3Split && !s.core3Io, "the parts one by one without the umbrella");
    }

    // ---- the fourth core: a thread of a part, refused or granted
    {
        FakeKernel k;
        k.mask = 0x20000;   // the mixer on core 1 (its plan), or the finishing thread
        Placement p = PlaceOnSystemCore(k, 0x20000);
        Require(p.onSystem && k.mask == kSystem && k.sets == 1, "granted: on the system core after one call");
        k = FakeKernel{};
        k.mode = FakeKernel::Refuse;
        k.mask = 0x20000;
        p = PlaceOnSystemCore(k, 0x20000);
        Require(!p.onSystem && k.mask == 0x20000 && p.mask == 0x20000 && k.history[1] == 0x20000, "refused: back to the planned core");
        k = FakeKernel{};
        k.mode = FakeKernel::IgnoreSilently;
        k.mask = 0x40000;
        p = PlaceOnSystemCore(k, 0x20000);
        Require(!p.onSystem && k.mask == 0x20000, "accepted but not honoured: to the planned core");
        k = FakeKernel{};
        k.mode = FakeKernel::GrantOtherCore;
        k.mask = 0x40000;
        p = PlaceOnSystemCore(k, 0);
        Require(!p.onSystem && k.mask == 0x40000, "moved elsewhere, no plan: back where it was");
        k = FakeKernel{};
        k.mode = FakeKernel::Refuse;
        k.mask = 0x60000;
        p = PlaceOnSystemCore(k, kSystem | 0x20000);
        Require(k.mask == 0x20000, "a plan is never the system core itself");
    }

    // ---- the fourth core: the probe that proves it
    {
        struct FakeProof {
            int createAt = 0;          // 0: creation with the system mask works; 1: only on core 2; 2: no creation at all
            bool changeOk = true, startOk = true, ends = true, reports = true;
            int cpu = 3, mask = kSystem;
            unsigned long long wall = 10000, used = 9800;
            int created = 0, deleted = 0, started = 0, lastMask = 0, slept = 0, goodFrom = 0;  // goodFrom: probe number from which the share is full
            void Sleep(unsigned us) { Require(us == kProofRetryGapUs, "retries 40 ms apart"); ++slept; if (goodFrom && created + 1 >= goodFrom) used = 9800; }
            int Create(int m) {
                lastMask = m;
                if (createAt == 2 || (createAt == 1 && m == kSystem)) return -0x7FFD7FDB;
                ++created;
                return 7;
            }
            int Change(int, int) { return changeOk ? 0 : static_cast<int>(0x80028025u); }
            int Start(int) { ++started; return startOk ? 0 : -1; }
            int Wait(int, unsigned timeout) { Require(timeout == kProofTimeoutUs, "the probe waits 200 ms"); return ends ? 0 : static_cast<int>(0x80028005u); }
            void Delete(int) { ++deleted; }
            bool Reported() { return reports; }
            int Cpu() { return cpu; }
            int Mask() { return mask; }
            unsigned long long SpinWallUs() { return wall; }
            unsigned long long SpinCpuUs() { return used; }
        };
        FakeProof f;
        Proof r = ProveSystemCore(f);
        Require(r.proven && r.cpu == 3 && r.mask == kSystem && r.sharePct == 98 && f.deleted == 1, "a working core is proven, the probe thread deleted");
        f = FakeProof{};
        f.createAt = 1;
        r = ProveSystemCore(f);
        Require(r.proven && r.createRc < 0 && f.lastMask == kProofStartMask, "made on core 2 and moved: proven too");
        f = FakeProof{};
        f.createAt = 1;
        f.changeOk = false;
        r = ProveSystemCore(f);
        Require(!r.proven && f.started == 0 && f.deleted == 1 && std::strcmp(r.why, "the system refused the mask") == 0,
                "no plug-in (the console without CapUnlocker: 0x80028025): not proven, nothing started, the thread deleted");
        f = FakeProof{};
        f.createAt = 2;
        r = ProveSystemCore(f);
        Require(!r.proven && f.started == 0, "no thread at all: not proven");
        f = FakeProof{};
        f.startOk = false;
        r = ProveSystemCore(f);
        Require(!r.proven && f.deleted == 1, "a thread that does not start: not proven");
        f = FakeProof{};
        f.ends = false;
        r = ProveSystemCore(f);
        Require(!r.proven && std::strstr(r.why, "200 ms"), "a thread that does not end in 200 ms: not proven");
        f = FakeProof{};
        f.reports = false;
        r = ProveSystemCore(f);
        Require(!r.proven, "a thread that ended without its report: not proven");
        f = FakeProof{};
        f.cpu = 2;
        Require(!ProveSystemCore(f).proven, "ran on core 2: not proven");
        f = FakeProof{};
        f.cpu = -1;
        Require(!ProveSystemCore(f).proven, "changed cores while it ran: not proven");
        f = FakeProof{};
        f.mask = 0xF0000;
        Require(!ProveSystemCore(f).proven, "a mask with user cores as well: not proven (core 3 may have been luck)");
        f = FakeProof{};
        f.used = 3000;
        r = ProveSystemCore(f);
        Require(!r.proven && r.sharePct == 30, "30 % of the spin: the system lends too little of the core, not proven");
        f = FakeProof{};
        f.wall = 0;
        Require(!ProveSystemCore(f).proven, "no measured spin: not proven");
        {   // retries: only a low share is tried again
            int tries = 0;
            f = FakeProof{};
            f.used = 300; f.goodFrom = 3;
            r = ProveSystemCoreRetry(f, &tries);
            Require(r.proven && tries == 3 && f.slept == 2 && f.deleted == 3, "3 % at the launch, full on the third probe: proven after 3 probes");
            f = FakeProof{};
            f.used = 300;
            r = ProveSystemCoreRetry(f, &tries);
            Require(!r.proven && tries == kProofTries && f.slept == kProofTries - 1, "a share that stays low: not proven after every probe");
            f = FakeProof{};
            f.createAt = 1; f.changeOk = false;
            r = ProveSystemCoreRetry(f, &tries);
            Require(!r.proven && tries == 1 && f.slept == 0, "no plug-in: one probe, no retry");
            f = FakeProof{};
            f.cpu = 2; f.used = 300;
            r = ProveSystemCoreRetry(f, &tries);
            Require(!r.proven && tries == 1, "the wrong core: final at once");
            f = FakeProof{};
            r = ProveSystemCoreRetry(f, &tries);
            Require(r.proven && tries == 1 && f.slept == 0, "a working core: one probe");
        }
        f = FakeProof{};
        f.used = 20000;
        r = ProveSystemCore(f);
        Require(r.proven && r.sharePct == 100, "CPU time over the wall time (clock granularity) counts as 100 %");
    }

    // ---- the fourth core: the finishing thread's guard
    {
        SplitGuard g;
        Require(!g.Feed(5.0, 300) && g.Feed(5.0, 300), "two windows over 3 ms in a row: back to the planned core");
        SplitGuard h;
        Require(!h.Feed(5.0, 300) && !h.Feed(1.0, 300) && !h.Feed(5.0, 300), "a good window in between starts the count again");
        SplitGuard l;
        Require(!l.Feed(9.0, 10) && !l.Feed(9.0, 10) && !l.Feed(9.0, 29), "load windows (under 30 pictures) do not count");
        Require(!l.Feed(9.0, 30) && l.Feed(9.0, 30), "...and do not reset the count either way");
        SplitGuard off{0.0, 0};
        Require(!off.Feed(50.0, 300) && !off.Feed(50.0, 300) && !off.Feed(50.0, 300), "GXV_CORE3_SPLIT_GUARD_MS=0: never");
        SplitGuard at{3.0, 0};
        Require(!at.Feed(3.0, 300) && !at.Feed(3.0, 300), "exactly at the limit is not over it");
    }

    std::puts("PASS: core layout policy: switches, helper masks, sweep, system-core placement with read-back and fallback, fourth-core "
              "switches, proof and split guard");
    return 0;
}
