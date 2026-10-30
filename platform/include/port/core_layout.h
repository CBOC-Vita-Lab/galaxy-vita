// Which core each kind of port thread may run on (PS Vita: cores 0-2 are the application's, the fourth is the system's).
// Pure policy: no kernel calls, so the selection logic is tested on a PC (tests/core_layout_test.cpp). The Vita runtime
// (vita/runtime/memory.cpp) supplies the kernel; the renderer's host (vgx_host_galaxy_vita.cpp) uses SweepTarget.
//
//   GXV_HELPER_CORES=0   (older switch, default on) the helper threads are not moved off core 0 by their class.
//   GXV_CORE0_ALONE=1    (default 0) the game thread has core 0 to itself: every other thread of the port, the ones with a class
//                        and the ones without (log flusher, watchdog, frame clock, library threads found by name) runs on
//                        cores 1-2 (mask 0x60000), whatever GXV_HELPER_CORES says.
//   GXV_CORE3_IO=1       (default 0) delay-tolerant work may use the system core (mask 0x80000): the disc read-ahead worker and
//                        the third movie decoder. A thread tries the move on itself, keeps it only when the call succeeded AND the
//                        mask reads back as asked, and otherwise goes back where it was, without a message to the player. Never the
//                        render, front-end or present path.
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
    bool core3Io = false;      // GXV_CORE3_IO=1
};

// `get` is std::getenv or a test double: const char* (*)(const char*).
template <class Get> Switches ReadSwitches(Get get) {
    Switches s;
    const char* helper = get("GXV_HELPER_CORES");
    s.helperCores = !(helper && *helper == '0');
    const char* alone = get("GXV_CORE0_ALONE");
    s.core0Alone = alone && std::atoi(alone) == 1;
    const char* io = get("GXV_CORE3_IO");
    s.core3Io = io && std::atoi(io) == 1;
    return s;
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
template <class Kernel> Placement PlaceIoThread(Kernel& kernel, const Switches& s, IoKind kind, int index) {
    Placement p;
    if (!MayUseSystemCore(s, kind, index)) return p;
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
    // Refused, or accepted but not honoured: where the thread was, or on cores 1-2 when that is unknown or not a user core.
    int back = HelperMask(s);
    if (back == 0 && before > 0 && before != kSystem) back = before;
    if (back != 0) {
        p.restoreRc = kernel.Set(back);
        p.mask = back;
    }
    return p;
}

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
