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

    std::puts("PASS: core layout policy: switches, helper masks, sweep, system-core placement with read-back and fallback");
    return 0;
}
